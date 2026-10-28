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
#include <std/sys/crt.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#if defined(__APPLE__)
    #include <pthread/qos.h>
#endif
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

    // The write-direction block: a header and its payload in one
    // variable-size small-obj allocation, so a keystroke costs a
    // keystroke. The main side fills it, the drain writes it to the pty,
    // and the spent block rides a list back to the main side whose
    // allocator made it - the allocator's release is not thread safe, and
    // the header carries the allocation size for exactly that type-erased
    // return. Engaged reads do not use blocks: they flow through the
    // per-handle ring (see PtyHandleImpl). Only the stream-era fallback
    // read allocates one block per call, on its own thread.
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

    // The two lock-free lists shared by every session carry write blocks
    // only - child input toward the drain, spent returns toward the main
    // side: a producer prepends one block, the consumer takes the whole
    // list at once and routes blocks by their handle. A grab comes out
    // newest-first; one pointer reversal restores push order.
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

    // Queued child input; a paste parks its fiber past this.
    constexpr size_t writeBudget = 64;
    // The read ring per session: the drain reads straight into it, the
    // feed parses out of it in place. The capacity is the whole read-side
    // backpressure - a full ring stops the drain, the kernel queue fills,
    // the child blocks in write. A power of two for mask arithmetic.
    constexpr size_t ringBytes = 1u << 20;
    constexpr u64 ringMask = ringBytes - 1;
    // How much ring the feed must consume before nudging a drain that may
    // have parked on a full ring, and how much the drain produces between
    // doorbell rings while a slice is running.
    constexpr size_t ringNudgeBytes = ringBytes / 2;
    constexpr size_t doorbellBytes = 64 * 1024;
    // One poll round drains one session for at most this long, so a
    // flooding tab cannot starve its neighbours. A time slice instead of
    // a block count: leaving for poll() parks the thread, and on macOS
    // every park costs the WRITER a selwakeup on the next refill, so the
    // slice must be long enough to make parks rare under a flood while
    // staying well under anything a human can feel.
    constexpr u64 drainSliceUs = 2000;

    // A registry node; the freelist recycles them under the registry
    // mutex because nodes cross threads and nothing may free them.
    struct DrainEntry {
        PtyHandleImpl* handle = nullptr;
        DrainEntry* next = nullptr;
    };

    // One contiguous ring segment handed to the feed; acquire() builds at
    // most two of these (the ring wraps once per lap) out of the handle's
    // own preallocated pair, so a read delivery allocates nothing.
    struct RingChunk final: public PtyHandle::Chunk {
        void* data() override;
        size_t length() override;
        Chunk* next() override;

        u8* bytes = nullptr;
        size_t count = 0;
        RingChunk* tail = nullptr;
    };

    struct PtyHandleImpl final: public PtyHandle {
        PtyHandleImpl(PtyImpl& pty, ObjPool& owner, int fd, pid_t pid);
        ~PtyHandleImpl() noexcept;

        void resize(const PtySize& size) override;
        void engage() override;
        Chunk* allocate(size_t len) override;
        void send(Chunk* chunk, size_t len) override;
        Chunk* acquire() override;
        void release(Chunk* chunks) override;
        pid_t foregroundProcessGroup() override;

        PtyImpl& pty;
        plt::Scheduler& scheduler;
        int fd;
        pid_t pid;
        bool eof = false;
        bool engaged_ = false;
        bool writeWarned_ = false;

        // Main side.
        Block* loaned_ = nullptr;
        size_t outstandingWrite_ = 0;
        size_t consumedSinceWake_ = 0;
        u64 ringLoaned_ = 0;
        bool ringLoanActive_ = false;
        RingChunk ringChunks_[2];
        plt::Fiber* feedFiber_ = nullptr;
        plt::Fiber* writerFiber_ = nullptr;
        PtyHandleImpl* mainNext_ = nullptr;

        // Drain side.
        Block* writeInbox_ = nullptr;
        Block* writeServing_ = nullptr;
        bool readStopped_ = false;
        bool writeBroken_ = false;

        // The read ring. The drain reads from the kernel straight into the
        // ring and advances written_; the feed parses in place and
        // advances consumed_. The cursors are monotonic u64 byte counts -
        // they never wrap, positions are cursor & ringMask - and each is
        // stored by exactly one side, so the exchange has no
        // read-modify-write anywhere. Each cursor sits on its own cache
        // line: they are the only fields both hot threads touch per lap,
        // and sharing a line would make every advance evict the peer.
        u8* ring_ = nullptr;
        alignas(64) u64 ringWritten_ = 0;
        alignas(64) u64 ringConsumed_ = 0;

        // Death is a two-phase handshake: the owner raises dead, the
        // drain stops producing, fails the write queue over and answers
        // quiet; the owner then collects the spent write blocks, and the
        // drain answers detached - from that store on it never touches
        // the handle again.
        alignas(64) u32 feedEof_ = 0;
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

void* RingChunk::data() {
    return bytes;
}

size_t RingChunk::length() {
    return count;
}

PtyHandle::Chunk* RingChunk::next() {
    return tail;
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

PtyHandleImpl::PtyHandleImpl(PtyImpl& pty_, ObjPool& owner, int fd_, pid_t pid_)
    : pty(pty_)
    , scheduler(pty_.scheduler)
    , fd(fd_)
    , pid(pid_)
{
    ring_ = (u8*)(owner.allocate(ringBytes));
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
        // LIFO. Ask the drain to stop first: after quiet it has failed the
        // write queue over and will never touch the ring again, so the
        // spent write blocks in toMain are final and collectable here.
        pty.unregisterMain(*this);
        stdAtomicStore(&dead_, 1u, MemoryOrder::Release);
        pty.selfWake();
        while (stdAtomicFetch(&quiet_, MemoryOrder::Acquire) == 0) {
            sched_yield();
        }
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
    STD_INSIST(!ringLoanActive_);
    for (;;) {
        const u64 written = stdAtomicFetch(&ringWritten_, MemoryOrder::Acquire);
        const u64 consumed = ringConsumed_;
        if (written != consumed) {
            // The span [consumed, written) as one or two contiguous
            // segments; the second one exists when the span wraps the
            // ring's end.
            const size_t position = (size_t)(consumed & ringMask);
            const size_t span = (size_t)(written - consumed);
            const size_t first = span < ringBytes - position ? span : ringBytes - position;
            ringChunks_[0].bytes = ring_ + position;
            ringChunks_[0].count = first;
            ringChunks_[0].tail = nullptr;
            if (first < span) {
                ringChunks_[0].tail = &ringChunks_[1];
                ringChunks_[1].bytes = ring_;
                ringChunks_[1].count = span - first;
                ringChunks_[1].tail = nullptr;
            }
            ringLoaned_ = written;
            ringLoanActive_ = true;
            return &ringChunks_[0];
        }
        if (stdAtomicFetch(&feedEof_, MemoryOrder::Acquire) != 0) {
            // The eof store follows the final written advance; one more
            // cursor look collects any straggler the first load raced
            // with, and the loop above serves it before this returns.
            if (stdAtomicFetch(&ringWritten_, MemoryOrder::Acquire) == consumed) {
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
    if (!engaged_) {
        STD_INSIST(chunks == loaned_);
        loaned_ = nullptr;
        Block* block = static_cast<Block*>(chunks);
        while (block != nullptr) {
            Block* const following = block->link;
            releaseBlock(block);
            block = following;
        }
        return;
    }
    STD_INSIST(ringLoanActive_ && chunks == &ringChunks_[0]);
    ringLoanActive_ = false;
    consumedSinceWake_ += (size_t)(ringLoaned_ - ringConsumed_);
    stdAtomicStore(&ringConsumed_, ringLoaned_, MemoryOrder::Release);
    // Freed ring is freight the drain notices whenever it wakes for its
    // own reasons; the half-ring nudge only covers a drain that went to
    // sleep with this fd parked on a full ring.
    if (consumedSinceWake_ >= ringNudgeBytes) {
        consumedSinceWake_ = 0;
        pty.selfWake();
    }
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
    // Everything coming back is a spent write block: read data travels
    // through the per-handle ring, never through this stack.
    Block* block = stackGrab(&toMain_);
    while (block != nullptr) {
        Block* const following = block->link;
        PtyHandleImpl& handle = *block->handle;
        STD_INSIST(block->kind == BlockKind::Spent);
        releaseBlock(block);
        --handle.outstandingWrite_;
        if (handle.writerFiber_ != nullptr && handle.outstandingWrite_ < writeBudget) {
            handle.writerFiber_->wake();
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

    // Grabs toDrain wholesale: every block is child input, prepended to
    // its handle's write inbox. The reversed grab walks oldest-first, so
    // prepending keeps the inbox newest-first; drainWrite reverses once
    // into write order when it refills its serving chain.
    static void drainDispatch(PtyImpl& pty) {
        Block* block = reverseChain(stackGrab(&pty.toDrain_));
        while (block != nullptr) {
            Block* const following = block->link;
            PtyHandleImpl& handle = *block->handle;
            STD_INSIST(block->kind == BlockKind::Data);
            block->link = handle.writeInbox_;
            handle.writeInbox_ = block;
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

    // On macOS the kernel hands the master at most about 1 KiB per read,
    // however large the buffer: a full kernel queue therefore reads back
    // as exactly that chunk. Gathering at least one such chunk in a round
    // marks the stream as saturated - a writer that filled the queue, not
    // a keystroke - and only a saturated stream is worth bridging.
    constexpr size_t saturatedBytes = 1024;
    // After EAGAIN on a saturated stream the writer refills the drained
    // queue within a few microseconds, while parking in poll costs the
    // WRITER a selwakeup on every refill - measured at +13% of the
    // child's own sys time, on top of this thread's poll syscalls. Keep
    // re-reading through this much continuous silence before going back
    // to poll; data resets the clock. A keystroke never enters the spin,
    // so an interactive terminal pays nothing.
    constexpr u64 bridgeSilenceUs = 150;

    // Whether the handle's ring can take more; the drain's poll loop asks
    // this to decide on POLLIN, and drainRead asks it per lap.
    static size_t drainRingSpace(PtyHandleImpl& handle) {
        const u64 consumed = stdAtomicFetch(&handle.ringConsumed_, MemoryOrder::Acquire);
        return ringBytes - (size_t)(handle.ringWritten_ - consumed);
    }

    static void drainRead(PtyImpl& pty, PtyHandleImpl& handle) {
        bool produced = false;
        size_t gathered = 0;
        size_t sinceSignal = 0;
        u64 silenceDeadline = 0;
        const u64 sliceDeadline = monotonicNowUs() + drainSliceUs;
        u64 written = handle.ringWritten_;
        for (;;) {
            const size_t space = drainRingSpace(handle);
            if (space == 0) {
                // Full ring is the whole read-side backpressure: stop, let
                // the kernel queue fill and the child block in write. The
                // feed nudges the wake pipe once it frees half a ring.
                break;
            }
            // One contiguous lap at most; the next lap continues at the
            // ring's start.
            const size_t position = (size_t)(written & ringMask);
            const size_t room = space < ringBytes - position ? space : ringBytes - position;
            const ssize_t count = ::read(handle.fd, handle.ring_ + position, room);
            if (count > 0) {
                written += (u64)(count);
                gathered += (size_t)(count);
                silenceDeadline = 0;
                stdAtomicStore(&handle.ringWritten_, written, MemoryOrder::Release);
                // The feed parses in parallel with this slice: ring the
                // doorbell on the first bytes so a parked fiber starts
                // immediately, then once per doorbellBytes so it never
                // starves mid-slice. The doorbell coalesces, so the cost
                // is one signal per batch.
                sinceSignal += (size_t)(count);
                if (!produced || sinceSignal >= doorbellBytes) {
                    sinceSignal = 0;
                    pty.doorbell_->signal();
                }
                produced = true;
                if (monotonicNowUs() >= sliceDeadline) {
                    break;
                }
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (gathered >= saturatedBytes) {
                    const u64 now = monotonicNowUs();
                    if (silenceDeadline == 0) {
                        silenceDeadline = now + bridgeSilenceUs;
                    }
                    if (now < silenceDeadline) {
                        continue;
                    }
                }
                break;
            }
            // EOF or EIO: the feed drains what the ring holds, then
            // closes. The eof store follows the final written advance -
            // that order is what lets acquire()'s straggler pass trust
            // the cursor it re-reads.
            handle.readStopped_ = true;
            stdAtomicStore(&handle.feedEof_, 1u, MemoryOrder::Release);
            pty.doorbell_->signal();
            return;
        }
        if (produced) {
            pty.doorbell_->signal();
        }
    }

    // The goodbye. On sighting dead: re-grab toDrain - every write push
    // happened before the dead store - fail the queue over to the main
    // side, stop producing for good and answer quiet, then unregister and
    // answer detached; from that store on the handle - the ring included
    // - is never touched.
    static void drainFarewell(PtyImpl& pty, PtyHandleImpl& handle) {
        // Guarded by quiet itself: the read may have stopped long ago at
        // EOF, and the goodbye still owes the owner its answer.
        if (stdAtomicFetch(&handle.quiet_, MemoryOrder::Relaxed) == 0) {
            handle.readStopped_ = true;
            drainDispatch(pty);
            drainDropWrites(pty, handle);
            stdAtomicStore(&handle.quiet_, 1u, MemoryOrder::Release);
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
    }
}

// Runs forever on its own thread; it allocates nothing and owns nothing,
// so there is no teardown - the thread dies with the process.
void* PtyImpl::drainThread(void* opaque) {
    auto* const pty = (PtyImpl*)(opaque);
#if defined(__APPLE__)
    // Terminal output is user-facing: without an explicit class the
    // scheduler ranks this thread below the UI and stretches the very
    // refill gaps the read bridge exists to hide.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
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
            if (!handle.readStopped_ && drainRingSpace(handle) != 0) {
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
                drainRead(*pty, handle);
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
    return owner.make<PtyHandleImpl>(*this, owner, master, pid);
}

Pty* createPty(ObjPool& owner, plt::Scheduler& scheduler, plt::Platform* platform) {
    return owner.make<PtyImpl>(owner, scheduler, platform);
}
