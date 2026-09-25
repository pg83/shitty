/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#if defined(__APPLE__)
    #define _DARWIN_C_SOURCE
#endif

#define _XOPEN_SOURCE 700

#include "pty.h"

#include "startup.h"

#include <lib/vterm/fatal.h>

#include <plt/fiber.h>
#include <plt/loop_wake.h>
#include <plt/platform.h>
#include <plt/poller.h>

#include <std/dbg/insist.h>
#include <std/ios/input.h>
#include <std/ios/out_buf.h>
#include <std/ios/output.h>
#include <std/ios/sys.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>
#include <std/str/view.h>
#include <std/sys/atomic.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

using namespace stl;

namespace {
    // The forked terminal child rewires its standard descriptors onto the
    // PTY slave; the originals remain close-on-exec solely so a failure
    // between fork and exec can still report on the real stderr.
    int originalFds[3] = {-1, -1, -1};
    const int targetFds[3] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};

    void restoreFds() {
        for (int index = 0; index < 3; ++index) {
            if (originalFds[index] >= 0) {
                dup2(originalFds[index], targetFds[index]);
                close(originalFds[index]);
                originalFds[index] = -1;
            }
        }
    }

    [[noreturn]] void childError(const char* message) {
        restoreFds();
        (void)!write(STDERR_FILENO, message, __builtin_strlen(message));
        (void)!write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }

    [[noreturn]] void sysError(const char* message) {
        const int error = errno;
        restoreFds();
        OutBuf output(stderrStream());
        output << StringView(u8"Error: ") << StringView(message) << StringView(u8": ") << StringView(strerror(error)) << StringView(u8" (errno=") << (i64)(error) << StringView(u8")") << endL << finI;
        exit(1);
    }

    // A failed spawn must not take the terminal down with it: opening one
    // more tab can hit a descriptor, pty or process limit while every
    // existing tab is perfectly healthy. Unlike sysError this throws, and
    // releases the descriptor the failed step still holds.
    [[noreturn]] void spawnError(const char* message, int error, int heldFd) {
        if (heldFd >= 0) {
            close(heldFd);
        }
        raiseError(StringView(message), StringView(u8": "), StringView(strerror(error)), StringView(u8" (errno="), (i64)(error), StringView(u8")"));
    }

    void sysWarn(const char* message) {
        const int error = errno;
        sysE << StringView(u8"Warning: ") << StringView(message) << StringView(u8": ") << StringView(strerror(error)) << StringView(u8" (errno=") << (i64)(error) << StringView(u8")") << endL;
    }

    void saveFds() {
        for (int index = 0; index < 3; ++index) {
            originalFds[index] = fcntl(targetFds[index], F_DUPFD_CLOEXEC, 3);
            if (originalFds[index] < 0) {
                childError("Error: F_DUPFD_CLOEXEC");
            }
        }
    }

    void redirectFds(int fd) {
        saveFds();
        for (int index = 0; index < 3; ++index) {
            if (dup2(fd, targetFds[index]) != targetFds[index]) {
                childError("Error: dup2");
            }
        }
        if (fd != targetFds[0] && fd != targetFds[1] && fd != targetFds[2]) {
            close(fd);
        }
    }

    int openPtyMaster(char* slaveName, size_t capacity) {
        const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master < 0) {
            spawnError("can't open master pty: posix_openpt()", errno, -1);
        }
        if (fcntl(master, F_SETFD, FD_CLOEXEC) < 0) {
            spawnError("can't open master pty: fcntl(FD_CLOEXEC)", errno, master);
        }
        if (grantpt(master) < 0) {
            spawnError("can't open master pty: grantpt()", errno, master);
        }
        if (unlockpt(master) < 0) {
            spawnError("can't open master pty: unlockpt()", errno, master);
        }
        const char* const name = ptsname(master);
        if (name == nullptr) {
            spawnError("can't open master pty: ptsname()", errno, master);
        }
        const StringView text(name);
        const size_t length = text.length() < capacity - 1 ? text.length() : capacity - 1;
        memcpy(slaveName, text.data(), length);
        slaveName[length] = '\0';
        return master;
    }

    int openPtySlave(const char* name, int master) {
        const int slave = open(name, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            spawnError("can't open slave pty: open()", errno, master);
        }
        return slave;
    }

    void resizePty(int fd, const PtySize& requested) {
        struct winsize size{};
        size.ws_col = (unsigned short)(requested.columns);
        size.ws_row = (unsigned short)(requested.rows);
        size.ws_xpixel = (unsigned short)(requested.pixelWidth);
        size.ws_ypixel = (unsigned short)(requested.pixelHeight);
        if (ioctl(fd, TIOCSWINSZ, &size) < 0) {
            sysWarn("TIOCSWINSZ on pty");
        }
    }

    bool waitFor(plt::Scheduler& scheduler, int fd, bool readable) {
        if (scheduler.current() != nullptr) {
            return readable ? scheduler.awaitReadable(fd, 0) : scheduler.awaitWritable(fd, 0);
        }
        pollfd event{fd, (short)(readable ? POLLIN : POLLOUT), 0};
        int result;
        do {
            result = poll(&event, 1, -1);
        } while (result < 0 && errno == EINTR);
        return result > 0;
    }

    struct PtyHandleImpl;
    struct PtyImpl;

    // The swapchain block: a header and its payload in one variable-size
    // small-obj allocation, so a keystroke costs a keystroke and only a
    // flooding read fills the largest class. A read lands straight in the
    // payload and the parser consumes it in place; a spent block rides a
    // list back to the side whose allocator made it, because the
    // allocator's release is not thread safe - the header carries the
    // allocation size and the owner for exactly that type-erased return.
    enum class BlockKind : u8 {
        Data,
        Spent,
    };

    struct Block final: public PtyHandle::Chunk, public Newable {
        void* data() override;
        size_t length() override;
        Chunk* next() override;

        SmallObjAllocator* owner = nullptr;
        Block* link = nullptr;
        PtyHandleImpl* handle = nullptr;
        u32 allocated = 0;
        u32 used = 0;
        u32 written = 0;
        BlockKind kind = BlockKind::Data;
    };

    constexpr size_t blockPayloadLimit = smallObjMaxSize - sizeof(Block);

    static u8* blockPayload(Block* block) {
        return (u8*)(block + 1);
    }

    static Block* makeBlock(SmallObjAllocator& allocator, PtyHandleImpl& handle, size_t payload) {
        const size_t bytes = sizeof(Block) + payload;
        Block* const block = new (allocator.allocate(bytes)) Block;
        block->owner = &allocator;
        block->handle = &handle;
        block->allocated = (u32)(bytes);
        return block;
    }

    static void releaseBlock(Block* block) {
        block->owner->deallocate(block, block->allocated);
    }

    // The two directions of the swapchain are two lock-free lists shared
    // by every session: a producer prepends one block, the consumer takes
    // the whole list at once and routes blocks by their handle. A grab
    // comes out newest-first; one pointer reversal restores push order.
    static void stackPush(Block** head, Block* block) {
        Block* expected = stdAtomicFetch(head, MemoryOrder::Relaxed);
        do {
            block->link = expected;
        } while (!stdAtomicCAS(head, &expected, block, MemoryOrder::Release, MemoryOrder::Relaxed));
    }

    static Block* stackGrab(Block** head) {
        Block* expected = stdAtomicFetch(head, MemoryOrder::Acquire);
        while (expected != nullptr && !stdAtomicCAS(head, &expected, (Block*)(nullptr), MemoryOrder::Acquire, MemoryOrder::Acquire)) {
        }
        return expected;
    }

    static Block* reverseChain(Block* head) {
        Block* out = nullptr;
        while (head != nullptr) {
            Block* const following = head->link;
            head->link = out;
            out = head;
            head = following;
        }
        return out;
    }

    // The drain runs at most this many read blocks ahead of the parser
    // per session; the bound caps memory and the reply latency under a
    // flooding child.
    constexpr size_t readBudget = 448;
    // Queued child input; a paste parks its fiber past this.
    constexpr size_t writeBudget = 64;
    // One poll round reads at most this many blocks from one session, so
    // a flooding tab cannot starve its neighbours.
    constexpr size_t readBurst = 64;

    // A registry node; the freelist recycles them under the registry
    // mutex because nodes cross threads and nothing may free them.
    struct DrainEntry {
        PtyHandleImpl* handle = nullptr;
        DrainEntry* next = nullptr;
    };

    struct PtyHandleImpl final: public PtyHandle {
        PtyHandleImpl(PtyImpl& pty, int fd, pid_t pid);
        ~PtyHandleImpl() noexcept;

        void resize(const PtySize& size) override;
        void engage() override;
        Chunk* allocate(size_t len) override;
        void send(Chunk* chunk, size_t len) override;
        Chunk* acquire() override;
        void release(Chunk* chunks) override;
        pid_t foregroundProcessGroup() override;

        void returnRead(Block* block);

        PtyImpl& pty;
        plt::Scheduler& scheduler;
        int fd;
        pid_t pid;
        bool eof = false;
        bool engaged_ = false;
        bool writeWarned_ = false;

        // Each side keeps an inbox its dispatcher prepends to - a pure
        // LIFO whatever number of grabs landed; the consumer detaches it
        // wholesale and reverses once into chronological order. loaned_
        // remembers the chain acquire handed out, so an owner dying
        // between acquire and release can still balance the ledger.

        // Main side.
        Block* inbox_ = nullptr;
        Block* loaned_ = nullptr;
        size_t outstandingWrite_ = 0;
        size_t returnedSinceWake_ = 0;
        plt::Fiber* feedFiber_ = nullptr;
        plt::Fiber* writerFiber_ = nullptr;
        PtyHandleImpl* mainNext_ = nullptr;

        // Drain side.
        size_t outstandingRead_ = 0;
        Block* writeInbox_ = nullptr;
        Block* writeServing_ = nullptr;
        bool readStopped_ = false;
        bool writeBroken_ = false;

        // Death is a two-phase handshake: the owner raises dead, the
        // drain stops producing and answers quiet, the owner returns
        // every block it holds, and the drain answers detached once the
        // read ledger balances - from that store on it never touches the
        // handle again.
        u32 feedEof_ = 0;
        u32 dead_ = 0;
        u32 quiet_ = 0;
        u32 detached_ = 0;
    };

    struct PtyImpl final: public Pty, public plt::TimerCallback {
        PtyImpl(ObjPool& owner, plt::Scheduler& scheduler, plt::Platform* platform);

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command) override;
        // The doorbell on the platform thread: a spurious wake is safe by
        // the fiber contract, so it wakes everyone and lets them look.
        void ready() override;

        // Grabs toMain wholesale and routes it: spent write blocks die in
        // their allocator, read data lands in the handles' inboxes.
        void dispatchToMain();

        static void* drainThread(void* opaque);
        void engage(PtyHandleImpl& handle);
        void unregisterMain(PtyHandleImpl& handle);
        void selfWake();

        ObjPool& owner;
        plt::Scheduler& scheduler;
        plt::Platform* platform;
        SmallObjAllocator* mainAllocator_ = nullptr;
        plt::LoopWake* doorbell_ = nullptr;
        PtyHandleImpl* mainHandles_ = nullptr;
        pthread_mutex_t registryMutex_ = PTHREAD_MUTEX_INITIALIZER;
        DrainEntry* registry_ = nullptr;
        DrainEntry* freeEntries_ = nullptr;
        int wakeFds_[2] = {-1, -1};
        bool threadStarted_ = false;
        Block* toMain_ = nullptr;
        Block* toDrain_ = nullptr;
    };
}

void* Block::data() {
    return blockPayload(this);
}

size_t Block::length() {
    return used;
}

PtyHandle::Chunk* Block::next() {
    return link;
}

PtyHandle::Chunk* PtyHandleImpl::allocate(size_t len) {
    const size_t granted = len < blockPayloadLimit ? len : blockPayloadLimit;
    Block* const block = makeBlock(*pty.mainAllocator_, *this, granted);
    block->used = (u32)(granted);
    return block;
}

void PtyHandleImpl::send(Chunk* chunk, size_t len) {
    Block* const block = static_cast<Block*>(chunk);
    STD_INSIST(len <= block->used);
    block->used = (u32)(len);
    if (engaged_) {
        while (outstandingWrite_ >= writeBudget) {
            pty.dispatchToMain();
            if (outstandingWrite_ < writeBudget) {
                break;
            }
            plt::Fiber* const current = scheduler.current();
            if (current == nullptr) {
                // The loop itself cannot park; the overrun is bounded by
                // the input rate, and the drain trims it right back.
                break;
            }
            writerFiber_ = current;
            writerFiber_->park();
            writerFiber_ = nullptr;
        }
        ++outstandingWrite_;
        stackPush(&pty.toDrain_, block);
        pty.selfWake();
        return;
    }
    // The direct blocking write of the stream era; the block dies here.
    const u8* bytes = blockPayload(block);
    size_t remaining = len;
    while (remaining != 0) {
        const ssize_t count = ::write(fd, bytes, remaining);
        if (count > 0) {
            bytes += count;
            remaining -= (size_t)(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitFor(scheduler, fd, false)) {
                continue;
            }
        }
        // A dead child takes its input with it; drop what can never be
        // delivered.
        if (!writeWarned_ && count < 0 && errno != EIO && errno != EPIPE && errno != EBADF) {
            sysWarn("pty write");
            writeWarned_ = true;
        }
        break;
    }
    releaseBlock(block);
}

PtyHandleImpl::PtyHandleImpl(PtyImpl& pty_, int fd_, pid_t pid_)
    : pty(pty_)
    , scheduler(pty_.scheduler)
    , fd(fd_)
    , pid(pid_)
{
}

PtyHandleImpl::~PtyHandleImpl() noexcept {
    if (!engaged_ && loaned_ != nullptr) {
        Block* block = loaned_;
        loaned_ = nullptr;
        while (block != nullptr) {
            Block* const following = block->link;
            releaseBlock(block);
            block = following;
        }
    }
    if (engaged_) {
        // The detach handshake. The feed and writer fibers are already
        // gone - they live after the handle in the arena, and teardown is
        // LIFO - so the main side owns every block it ever held. Ask the
        // drain to stop producing first: only after quiet is the toMain
        // ring final and every read block accountable from here.
        pty.unregisterMain(*this);
        stdAtomicStore(&dead_, 1u, MemoryOrder::Release);
        pty.selfWake();
        while (stdAtomicFetch(&quiet_, MemoryOrder::Acquire) == 0) {
            sched_yield();
        }
        pty.dispatchToMain();
        Block* chains[2] = {loaned_, inbox_};
        loaned_ = inbox_ = nullptr;
        for (Block* chain : chains) {
            while (chain != nullptr) {
                Block* const block = chain;
                chain = block->link;
                returnRead(block);
            }
        }
        pty.selfWake();
        while (stdAtomicFetch(&detached_, MemoryOrder::Acquire) == 0) {
            sched_yield();
        }
        pty.dispatchToMain();
        STD_INSIST(outstandingWrite_ == 0);
        eof = stdAtomicFetch(&feedEof_, MemoryOrder::Acquire) != 0;
    }
    if (pid > 0 && !eof) {
        // The child called setsid(), so its pid is also the session group.
        // Signal both to cover destruction racing the child's setsid().
        kill(-pid, SIGHUP);
        kill(pid, SIGHUP);
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void PtyHandleImpl::engage() {
    STD_INSIST(!engaged_);
    pty.engage(*this);
    engaged_ = true;
}

PtyHandle::Chunk* PtyHandleImpl::acquire() {
    STD_INSIST(loaned_ == nullptr);
    if (!engaged_) {
        // The stream era's blocking read, one block per call: the fiber
        // parks in the poller, a plain thread parks in poll.
        Block* const block = makeBlock(*pty.mainAllocator_, *this, blockPayloadLimit);
        for (;;) {
            const ssize_t count = ::read(fd, blockPayload(block), blockPayloadLimit);
            if (count > 0) {
                block->used = (u32)(count);
                loaned_ = block;
                return block;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (waitFor(scheduler, fd, true)) {
                    continue;
                }
            } else if (count < 0 && errno != EIO) {
                sysWarn("pty read");
            }
            releaseBlock(block);
            eof = true;
            return nullptr;
        }
    }
    for (;;) {
        pty.dispatchToMain();
        if (inbox_ != nullptr) {
            loaned_ = reverseChain(inbox_);
            inbox_ = nullptr;
            return loaned_;
        }
        if (stdAtomicFetch(&feedEof_, MemoryOrder::Acquire) != 0) {
            // The eof store follows the final data push; one more look
            // collects any straggler the first dispatch raced with.
            pty.dispatchToMain();
            if (inbox_ == nullptr) {
                return nullptr;
            }
            continue;
        }
        feedFiber_ = scheduler.current();
        feedFiber_->park();
        feedFiber_ = nullptr;
    }
}

void PtyHandleImpl::release(Chunk* chunks) {
    STD_INSIST(chunks == loaned_);
    loaned_ = nullptr;
    Block* block = static_cast<Block*>(chunks);
    if (!engaged_) {
        while (block != nullptr) {
            Block* const following = block->link;
            releaseBlock(block);
            block = following;
        }
        return;
    }
    while (block != nullptr) {
        Block* const following = block->link;
        returnRead(block);
        block = following;
    }
    // Returns are freight the drain collects whenever it wakes for its
    // own reasons; the half-budget nudge only covers a drain that went
    // to sleep with this fd parked on the read budget.
    if (returnedSinceWake_ >= readBudget / 2) {
        returnedSinceWake_ = 0;
        pty.selfWake();
    }
}

void PtyHandleImpl::returnRead(Block* block) {
    block->kind = BlockKind::Spent;
    stackPush(&pty.toDrain_, block);
    ++returnedSinceWake_;
}

void PtyHandleImpl::resize(const PtySize& size) {
    if (fd >= 0) {
        resizePty(fd, size);
    }
}

pid_t PtyHandleImpl::foregroundProcessGroup() {
    if (fd < 0 || eof) {
        return 0;
    }
    const pid_t group = tcgetpgrp(fd);
    return group > 0 ? group : 0;
}

PtyImpl::PtyImpl(ObjPool& owner_, plt::Scheduler& scheduler_, plt::Platform* platform_)
    : owner(owner_)
    , scheduler(scheduler_)
    , platform(platform_)
{
    mainAllocator_ = SmallObjAllocator::create(&owner);
}

void PtyImpl::engage(PtyHandleImpl& handle) {
    if (!threadStarted_) {
        STD_INSIST(platform != nullptr);
        doorbell_ = platform->createLoopWake(owner, *this);
        if (pipe(wakeFds_) != 0) {
            sysError("pty drain wake pipe");
        }
        for (int end = 0; end < 2; ++end) {
            fcntl(wakeFds_[end], F_SETFD, FD_CLOEXEC);
            fcntl(wakeFds_[end], F_SETFL, O_NONBLOCK);
        }
        pthread_t drain;
        if (pthread_create(&drain, nullptr, drainThread, this) != 0) {
            sysError("pty drain thread");
        }
        pthread_detach(drain);
        threadStarted_ = true;
    }
    handle.mainNext_ = mainHandles_;
    mainHandles_ = &handle;
    pthread_mutex_lock(&registryMutex_);
    DrainEntry* entry = freeEntries_;
    if (entry != nullptr) {
        freeEntries_ = entry->next;
    } else {
        entry = owner.make<DrainEntry>();
    }
    entry->handle = &handle;
    entry->next = registry_;
    registry_ = entry;
    pthread_mutex_unlock(&registryMutex_);
    selfWake();
}

void PtyImpl::unregisterMain(PtyHandleImpl& handle) {
    PtyHandleImpl** link = &mainHandles_;
    while (*link != nullptr && *link != &handle) {
        link = &(*link)->mainNext_;
    }
    if (*link != nullptr) {
        *link = handle.mainNext_;
    }
}

void PtyImpl::selfWake() {
    const u8 token = 0;
    (void)!write(wakeFds_[1], &token, 1);
}

void PtyImpl::dispatchToMain() {
    // The reversed grab walks oldest-first, so prepending keeps every
    // inbox newest-first - a pure LIFO whatever number of grabs land
    // between the consumer's serving refills.
    Block* block = reverseChain(stackGrab(&toMain_));
    while (block != nullptr) {
        Block* const following = block->link;
        PtyHandleImpl& handle = *block->handle;
        if (block->kind == BlockKind::Spent) {
            releaseBlock(block);
            --handle.outstandingWrite_;
            if (handle.writerFiber_ != nullptr && handle.outstandingWrite_ < writeBudget) {
                handle.writerFiber_->wake();
            }
        } else {
            block->link = handle.inbox_;
            handle.inbox_ = block;
        }
        block = following;
    }
}

void PtyImpl::ready() {
    for (PtyHandleImpl* handle = mainHandles_; handle != nullptr; handle = handle->mainNext_) {
        if (handle->feedFiber_ != nullptr) {
            handle->feedFiber_->wake();
        }
        if (handle->writerFiber_ != nullptr) {
            handle->writerFiber_->wake();
        }
    }
}

namespace {

    // Everything below runs on the drain thread.

    // Grabs toDrain wholesale and routes it: spent read blocks die in
    // their allocator, write data lands in the handles' inboxes - the
    // mirror of dispatchToMain.
    static void drainDispatch(PtyImpl& pty) {
        Block* block = reverseChain(stackGrab(&pty.toDrain_));
        while (block != nullptr) {
            Block* const following = block->link;
            PtyHandleImpl& handle = *block->handle;
            if (block->kind == BlockKind::Spent) {
                releaseBlock(block);
                --handle.outstandingRead_;
            } else {
                block->link = handle.writeInbox_;
                handle.writeInbox_ = block;
            }
            block = following;
        }
    }

    // Fails the queue over to the main side unwritten; a dead child takes
    // its input with it, exactly like the stream path did.
    static void drainDropWrites(PtyImpl& pty, PtyHandleImpl& handle) {
        bool dropped = false;
        Block* chains[2] = {handle.writeServing_, reverseChain(handle.writeInbox_)};
        handle.writeServing_ = nullptr;
        handle.writeInbox_ = nullptr;
        for (Block* chain : chains) {
            while (chain != nullptr) {
                Block* const block = chain;
                chain = block->link;
                block->kind = BlockKind::Spent;
                stackPush(&pty.toMain_, block);
                dropped = true;
            }
        }
        if (dropped) {
            pty.doorbell_->signal();
        }
    }

    static void drainWrite(PtyImpl& pty, PtyHandleImpl& handle) {
        if (handle.writeBroken_) {
            drainDropWrites(pty, handle);
            return;
        }
        for (;;) {
            if (handle.writeServing_ == nullptr) {
                handle.writeServing_ = reverseChain(handle.writeInbox_);
                handle.writeInbox_ = nullptr;
                if (handle.writeServing_ == nullptr) {
                    return;
                }
            }
            Block* const block = handle.writeServing_;
            const ssize_t count = ::write(handle.fd, blockPayload(block) + block->written, block->used - block->written);
            if (count > 0) {
                block->written += (u32)(count);
                if (block->written < block->used) {
                    continue;
                }
                handle.writeServing_ = block->link;
                block->kind = BlockKind::Spent;
                stackPush(&pty.toMain_, block);
                pty.doorbell_->signal();
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            handle.writeBroken_ = true;
            drainDropWrites(pty, handle);
            return;
        }
    }

    static void drainRead(PtyImpl& pty, PtyHandleImpl& handle, SmallObjAllocator& allocator) {
        bool pushed = false;
        for (size_t burst = 0; burst < readBurst && handle.outstandingRead_ < readBudget; ++burst) {
            Block* const block = makeBlock(allocator, handle, blockPayloadLimit);
            const ssize_t count = ::read(handle.fd, blockPayload(block), blockPayloadLimit);
            if (count > 0) {
                block->used = (u32)(count);
                stackPush(&pty.toMain_, block);
                ++handle.outstandingRead_;
                pushed = true;
                continue;
            }
            releaseBlock(block);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            // EOF or EIO: the feed drains what is buffered, then closes.
            handle.readStopped_ = true;
            stdAtomicStore(&handle.feedEof_, 1u, MemoryOrder::Release);
            pushed = true;
            break;
        }
        if (pushed) {
            pty.doorbell_->signal();
        }
    }

    // The two-phase goodbye. First sighting of dead: re-grab toDrain -
    // every write push happened before the dead store - drop the queue,
    // stop producing for good and answer quiet; the owner then returns
    // every read block it holds. Once the ledger balances, unregister
    // and answer detached; from that store on the handle is never
    // touched. Returns true while the entry must stay registered.
    static bool drainFarewell(PtyImpl& pty, PtyHandleImpl& handle) {
        // Guarded by quiet itself: the read may have stopped long ago at
        // EOF, and the goodbye still owes the owner its answer.
        if (stdAtomicFetch(&handle.quiet_, MemoryOrder::Relaxed) == 0) {
            handle.readStopped_ = true;
            drainDispatch(pty);
            drainDropWrites(pty, handle);
            stdAtomicStore(&handle.quiet_, 1u, MemoryOrder::Release);
        }
        if (handle.outstandingRead_ != 0) {
            return true;
        }
        pthread_mutex_lock(&pty.registryMutex_);
        DrainEntry** link = &pty.registry_;
        while (*link != nullptr && (*link)->handle != &handle) {
            link = &(*link)->next;
        }
        DrainEntry* const entry = *link;
        *link = entry->next;
        entry->handle = nullptr;
        entry->next = pty.freeEntries_;
        pty.freeEntries_ = entry;
        pthread_mutex_unlock(&pty.registryMutex_);
        stdAtomicStore(&handle.detached_, 1u, MemoryOrder::Release);
        return false;
    }
}

// Runs forever on its own thread with its own arena: the blocks it makes
// come back to it by list before they are released. There is no teardown,
// the thread dies with the process.
void* PtyImpl::drainThread(void* opaque) {
    auto* const pty = (PtyImpl*)(opaque);
    ObjPool* const pool = ObjPool::fromMemoryRaw();
    SmallObjAllocator* const allocator = SmallObjAllocator::create(pool);
    Vector<PtyHandleImpl*> live;
    Vector<pollfd> polls;
    for (;;) {
        live.clear();
        pthread_mutex_lock(&pty->registryMutex_);
        for (DrainEntry* entry = pty->registry_; entry != nullptr; entry = entry->next) {
            live.pushBack(entry->handle);
        }
        pthread_mutex_unlock(&pty->registryMutex_);
        drainDispatch(*pty);

        polls.clear();
        polls.pushBack({pty->wakeFds_[0], POLLIN, 0});
        for (size_t at = 0; at < live.length(); ++at) {
            PtyHandleImpl& handle = *live[at];
            if (stdAtomicFetch(&handle.dead_, MemoryOrder::Acquire) != 0) {
                drainFarewell(*pty, handle);
                live.mut(at) = nullptr;
                polls.pushBack({-1, 0, 0});
                continue;
            }
            short events = 0;
            if (!handle.readStopped_ && handle.outstandingRead_ < readBudget) {
                events |= POLLIN;
            }
            if (handle.writeInbox_ != nullptr || handle.writeServing_ != nullptr) {
                events |= POLLOUT;
            }
            polls.pushBack({events != 0 ? handle.fd : -1, events, 0});
        }

        int result;
        do {
            result = poll(polls.mutData(), (nfds_t)(polls.length()), -1);
        } while (result < 0 && errno == EINTR);

        if (polls[0].revents & POLLIN) {
            u8 tokens[256];
            while (read(pty->wakeFds_[0], tokens, sizeof(tokens)) > 0) {
            }
        }
        for (size_t at = 0; at < live.length(); ++at) {
            if (live[at] == nullptr) {
                continue;
            }
            PtyHandleImpl& handle = *live[at];
            const short revents = polls[at + 1].revents;
            if (revents & (POLLOUT | POLLERR)) {
                drainWrite(*pty, handle);
            }
            if (revents & (POLLIN | POLLHUP)) {
                drainRead(*pty, handle, *allocator);
            }
        }
    }
    return nullptr;
}

PtyHandle* PtyImpl::spawn(ObjPool& owner, const LaunchCommand& command) {
    Vector<char*> arguments;
    for (size_t index = 0; index < command.offsets.length(); ++index) {
        arguments.pushBack(const_cast<char*>(command.argument(index)));
    }
    arguments.pushBack(nullptr);

    char slaveName[PATH_MAX];
    const int master = openPtyMaster(slaveName, sizeof(slaveName));
    const int slave = openPtySlave(slaveName, master);

    // A child that exits immediately must not outrun the caller's SIGCHLD
    // bookkeeping. The child restores the inherited mask before exec.
    sigset_t childSignal;
    sigemptyset(&childSignal);
    sigaddset(&childSignal, SIGCHLD);
    sigset_t previousMask;
    sigprocmask(SIG_BLOCK, &childSignal, &previousMask);
    const pid_t pid = fork();
    if (pid < 0) {
        const int error = errno;
        close(master);
        close(slave);
        sigprocmask(SIG_SETMASK, &previousMask, nullptr);
        spawnError("fork", error, -1);
    }
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &previousMask, nullptr);
        if (setsid() < 0) {
            childError("Error: setsid");
        }
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            childError("Error: TIOCSCTTY");
        }
        close(master);
        redirectFds(slave);

        struct termios term;
        if (tcgetattr(STDIN_FILENO, &term) < 0) {
            childError("Error: tcgetattr");
        }
        term.c_iflag |= IUTF8;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &term) < 0) {
            childError("Error: tcsetattr");
        }
        execvp(command.executable(), arguments.mutData());
        childError("Error: execvp");
    }
    sigprocmask(SIG_SETMASK, &previousMask, nullptr);
    close(slave);
    return owner.make<PtyHandleImpl>(*this, master, pid);
}

Pty* createPty(ObjPool& owner, plt::Scheduler& scheduler, plt::Platform* platform) {
    return owner.make<PtyImpl>(owner, scheduler, platform);
}
