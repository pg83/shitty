/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pty.h"
#include "session.h"
#include "startup.h"
#include "composer.h"

#include <lib/vterm/listener.h>
#include <lib/vterm/vt_headless.h>

#include <std/tst/ut.h>
#include <std/lib/vector.h>
#include <std/sys/throw.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <string>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <plt/fiber.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>
#include <plt/poller_loop.h>

using namespace stl;

namespace {
    constexpr u64 testTimeoutUs = 5'000'000;

    struct Timeout final: public plt::TimerCallback {
        void ready() override {
            fired = true;
        }

        bool fired = false;
    };

    struct WakeMarker final: public plt::TimerCallback {
        void ready() override {
            delivered = true;
        }

        bool delivered = false;
    };

    // A trivially owned chunk for the fakes: header and payload in one
    // small-obj allocation, released on send.
    struct StubChunk final: public PtyHandle::Chunk, public stl::Newable {
        void* data() override {
            return this + 1;
        }

        size_t length() override {
            return used;
        }

        Chunk* next() override {
            return nullptr;
        }

        SmallObjAllocator* owner = nullptr;
        u32 allocated = 0;
        u32 used = 0;
    };

    PtyHandle::Chunk* makeStubChunk(SmallObjAllocator& allocator, size_t len) {
        constexpr size_t cap = smallObjMaxSize - sizeof(StubChunk);
        const size_t granted = len < cap ? len : cap;
        auto* const chunk = new (allocator.allocate(sizeof(StubChunk) + granted)) StubChunk;
        chunk->owner = &allocator;
        chunk->allocated = (u32)(sizeof(StubChunk) + granted);
        chunk->used = (u32)(granted);
        return chunk;
    }

    struct SurvivorHandle final: public PtyHandle {
        explicit SurvivorHandle(Composer& composer_)
            : composer(composer_)
        {
        }

        void resize(const PtySize&) override {
        }

        void engage() override {
        }

        Chunk* allocate(size_t len) override {
            return makeStubChunk(*composer.smallObjects, len);
        }

        void send(Chunk* chunk, size_t) override {
            auto* const block = static_cast<StubChunk*>(chunk);
            block->owner->deallocate(block, block->allocated);
        }

        Chunk* acquire() override {
            for (;;) {
                composer.scheduler->current()->park();
            }
        }

        void release(Chunk*) override {
        }

        pid_t foregroundProcessGroup() override {
            return 0;
        }

        Composer& composer;
    };

    struct TwoSessionPty final: public Pty {
        TwoSessionPty(Composer& composer_, Pty& real_)
            : composer(composer_)
            , real(real_)
        {
        }

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command) override {
            if (spawns++ == 0) {
                doomed = real.spawn(owner, command);
                return doomed;
            }
            return owner.make<SurvivorHandle>(composer);
        }

        Composer& composer;
        Pty& real;
        PtyHandle* doomed = nullptr;
        size_t spawns = 0;
    };

    void publish(IntrusiveList& listeners) {
        for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }
    }

    struct RealPtyFixture {
        RealPtyFixture()
            : pool(ObjPool::fromMemory())
            , poller(plt::PollerLoop::create(*pool))
            , scheduler(plt::Scheduler::create(*pool, *poller))
            , pty(createPty(*pool, *scheduler))
        {
        }

        ObjPool::Ref pool;
        plt::PollerLoop* poller;
        plt::Scheduler* scheduler;
        Pty* pty;
    };

    // An engaged PTY starts the process-lifetime drain thread, so the
    // fixture's platform, scheduler and arena deliberately share that
    // lifetime; only the per-test owner arena below is ever torn down.
    struct EngagedPtyFixture {
        EngagedPtyFixture()
            : pool(ObjPool::fromMemoryRaw())
            , composer(*pool->make<Composer>(pool))
        {
            VtermHeadless* const host = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
            composer.platform = host->platform();
            composer.window = host->window();
            composer.installVtHost();
            composer.geometry.setCellPixelSize(1, 1);
            composer.resizeWindow(80, 24);
            pty = createPty(*composer.pool, *composer.scheduler, host->platform());
            poller = static_cast<plt::PollerLoop*>(composer.platform->poller());
        }

        // Drives the loop until check() holds; insists it does in time.
        template <typename Check>
        void driveUntil(Check check) {
            Timeout timeout;
            poller->timeout(testTimeoutUs, timeout);
            while (!check() && !timeout.fired) {
                poller->dispatchTimers();
                if (!check() && !timeout.fired) {
                    poller->wait(poller->nextDeadline());
                }
            }
            poller->cancel(timeout);
            STD_INSIST(!timeout.fired);
        }

        ObjPool* pool;
        Composer& composer;
        Pty* pty = nullptr;
        plt::PollerLoop* poller = nullptr;
    };

    PtyHandle* spawnShell(Pty& pty, ObjPool& owner, char* script) {
        char program[] = "pty_ut";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char* argv[] = {program, execute, shell, commandFlag, script, nullptr};
        const LaunchCommand command = buildLaunchCommand(5, argv, StringView(), false);
        return pty.spawn(owner, command);
    }

    PtyHandle* spawnHelper(Pty& pty, ObjPool& owner, char* mode) {
        char program[] = "pty_ut";
        char execute[] = "-e";
        char* const helper = getenv("SHITTY_PTY_TEST_HELPER");
        STD_INSIST(helper != nullptr);
        char* argv[] = {program, execute, helper, mode, nullptr};
        const LaunchCommand command = buildLaunchCommand(4, argv, StringView(), false);
        return pty.spawn(owner, command);
    }

    std::string readAll(PtyHandle& handle) {
        std::string result;
        for (;;) {
            PtyHandle::Chunk* const chunks = handle.acquire();
            if (chunks == nullptr) {
                return result;
            }
            for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                result.append((const char*)(chunk->data()), chunk->length());
            }
            handle.release(chunks);
        }
    }

    std::string readUntil(PtyHandle& handle, const char* needle) {
        std::string result;
        while (result.find(needle) == std::string::npos) {
            PtyHandle::Chunk* const chunks = handle.acquire();
            STD_INSIST(chunks != nullptr);
            for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                result.append((const char*)(chunk->data()), chunk->length());
            }
            handle.release(chunks);
        }
        return result;
    }

    void sendAll(PtyHandle& handle, const void* data, size_t len) {
        const u8* bytes = (const u8*)(data);
        size_t remaining = len;
        while (remaining != 0) {
            PtyHandle::Chunk* const chunk = handle.allocate(remaining);
            const size_t count = chunk->length() < remaining ? chunk->length() : remaining;
            __builtin_memcpy(chunk->data(), bytes, count);
            handle.send(chunk, count);
            bytes += count;
            remaining -= count;
        }
    }

    int reapChild() {
        int status = 0;
        const pid_t child = waitpid(-1, &status, 0);
        STD_INSIST(child > 0);
        return status;
    }

    // Opens terminals until the descriptor limit refuses one, releases them
    // all and returns how many there were.
    size_t spawnUntilRefused(Pty& pty) {
        Vector<ObjPool*> owners;
        bool refused = false;
        for (size_t attempt = 0; attempt < 64 && !refused; ++attempt) {
            ObjPool* const owner = ObjPool::fromMemoryRaw();
            char script[] = "sleep 60";
            try {
                spawnShell(pty, *owner, script);
                owners.pushBack(owner);
            } catch (Exception&) {
                delete owner;
                refused = true;
            }
        }
        const size_t opened = owners.length();
        for (size_t at = 0; at < opened; ++at) {
            delete owners[at];
        }
        for (size_t at = 0; at < opened; ++at) {
            reapChild();
        }
        STD_INSIST(refused);
        return opened;
    }
}

STD_TEST_SUITE(Pty) {
    STD_TEST(ForegroundProcessGroupReportsTheChild) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf ready; sleep 5";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);

        // Wait for output first: only then has the child certainly
        // called setsid() and taken the controlling terminal.
        const std::string output = readUntil(*handle, "ready");
        const pid_t group = handle->foregroundProcessGroup();

        STD_INSIST(output == "ready");
        STD_INSIST(group > 0);
        delete owner;
        reapChild();
    }

    STD_TEST(ChildOutputReachesEof) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf pty-output";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);

        const std::string output = readAll(*handle);
        delete owner;
        const int status = reapChild();

        STD_INSIST(output == "pty-output");
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    // EOF in one of two sessions ends the client-owned read fiber. The
    // session arena is then deleted on the deferred EOF wake, and the loop
    // must still be able to dispatch another independent wake afterwards.
    STD_TEST(EofClosesOneSessionBeforeItsFollowupWake) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless* const host = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
        composer.platform = host->platform();
        composer.window = host->window();
        composer.installVtHost();
        composer.geometry.setCellPixelSize(1, 1);
        composer.resizeWindow(80, 24);

        char program[] = "pty_ut";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char commandText[] = "read ignored; exit 0";
        char* argv[] = {program, execute, shell, commandFlag, commandText, nullptr};
        const LaunchCommand command = buildLaunchCommand(5, argv, StringView(), false);
        // The production drain thread and its arena live until process exit.
        // Keep that contract here while the ordinary test arena still tears
        // down the sessions and their handles below.
        ObjPool* const ptyOwner = ObjPool::fromMemoryRaw();
        Pty* const real = createPty(*ptyOwner, *composer.platform->scheduler(), composer.platform);
        TwoSessionPty pty(composer, *real);
        composer.pty = &pty;
        composer.launch = &command;
        SessionSet* const sessions = SessionSet::create(composer);
        publish(composer.newTabListeners);
        publish(composer.prevTabListeners);

        // EOT makes the shell's canonical read return EOF, just like Ctrl+D.
        const u8 eot = 0x04;
        sendAll(*pty.doomed, &eot, 1);

        auto* const poller = static_cast<plt::PollerLoop*>(composer.platform->poller());
        Timeout closeTimeout;
        poller->timeout(testTimeoutUs, closeTimeout);
        while (SessionSet::liveSessions != 1 && !closeTimeout.fired) {
            poller->dispatchTimers();
            if (SessionSet::liveSessions != 1 && !closeTimeout.fired) {
                poller->wait(poller->nextDeadline());
            }
        }
        poller->cancel(closeTimeout);
        STD_INSIST(SessionSet::liveSessions == 1);
        STD_INSIST(sessions->activeTerminal() != nullptr);
        STD_INSIST(!closeTimeout.fired);

        // The EOF callback has removed the tab and its arena, including
        // the finished reader's owned handle and stack.
        plt::Scheduler* const scheduler = composer.platform->scheduler();
        plt::Fiber* sentinelFiber = nullptr;
        bool sentinelWoke = false;
        auto sentinel = makeRunable([&] {
            sentinelFiber = scheduler->current();
            sentinelFiber->park();
            sentinelWoke = true;
        });
        sentinelFiber = scheduler->create(*composer.pool, sentinel);
        STD_INSIST(sentinelFiber != nullptr);
        STD_INSIST(!sentinelWoke);

        // This is deliberately a later loop wake, after the session pool
        // was removed, rather than merely observing the EOF callback.
        WakeMarker marker;
        plt::LoopWake* const markerWake = composer.platform->createLoopWake(*composer.pool, marker);
        markerWake->signal();
        Timeout wakeTimeout;
        poller->timeout(testTimeoutUs, wakeTimeout);
        while (!marker.delivered && !wakeTimeout.fired) {
            poller->dispatchTimers();
            if (!marker.delivered && !wakeTimeout.fired) {
                poller->wait(poller->nextDeadline());
            }
        }
        poller->cancel(wakeTimeout);

        const bool wokeUnrelatedFiber = sentinelWoke;
        if (!wokeUnrelatedFiber) {
            sentinelFiber->release();
        }
        STD_INSIST(marker.delivered);
        STD_INSIST(!wakeTimeout.fired);
        STD_INSIST(!wokeUnrelatedFiber);

        int status = 0;
        const pid_t child = waitpid(-1, &status, 0);
        STD_INSIST(child > 0);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(InputRoundTripsThroughTheSlave) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "stty -echo; IFS= read -r line; printf 'got:%s\\n' \"$line\"";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);

        const char input[] = "hello from master\n";
        sendAll(*handle, input, sizeof(input) - 1);
        const std::string output = readAll(*handle);
        delete owner;
        const int status = reapChild();

        STD_INSIST(output.find("got:hello from master") != std::string::npos);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(LargeChildOutputSurvivesBackpressure) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "head -c 1048576 /dev/zero";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);

        // Let the child fill the finite slave-to-master queue before the
        // first read, then drain it through repeated readiness waits.
        usleep(50'000);
        size_t total = 0;
        size_t nonzero = 0;
        for (;;) {
            PtyHandle::Chunk* const chunks = handle->acquire();
            if (chunks == nullptr) {
                break;
            }
            for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                const u8* const bytes = (const u8*)(chunk->data());
                total += chunk->length();
                for (size_t index = 0; index < chunk->length(); ++index) {
                    nonzero += bytes[index] != 0;
                }
            }
            handle->release(chunks);
        }
        delete owner;
        const int status = reapChild();

        STD_INSIST(total == 1024 * 1024);
        STD_INSIST(nonzero == 0);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(ResizeReachesChildAsWinch) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "winsize";
        PtyHandle* const handle = spawnHelper(*fixture.pty, *owner, mode);

        // Wait for the marker, not the first line: an instrumented build's
        // profile runtime may write its own diagnostics onto the pty first.
        const std::string ready = readUntil(*handle, "ready");
        handle->resize({
            .columns = 123,
            .rows = 47,
            .pixelWidth = 984,
            .pixelHeight = 752,
        });
        const std::string output = readAll(*handle);
        delete owner;
        const int status = reapChild();

        STD_INSIST(ready.find("ready") != std::string::npos);
        STD_INSIST(output.find("47 123") != std::string::npos);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    // The engaged path's hairy exit: the arena dies while the drain is
    // mid-flood and the feed holds acquired blocks. The destructor's
    // handshake must balance the ledger and hang up the child.
    STD_TEST(EngagedOwnerDeathSurvivesAFloodingChild) {
        // An engaged PTY starts the process-lifetime drain thread, so its
        // platform, scheduler and arena follow the production lifetime too.
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        VtermHeadless* const host = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
        composer.platform = host->platform();
        composer.window = host->window();
        composer.installVtHost();
        composer.geometry.setCellPixelSize(1, 1);
        composer.resizeWindow(80, 24);
        Pty* const pty = createPty(*composer.pool, *composer.scheduler, host->platform());
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "flood-hangup";
        PtyHandle* const handle = spawnHelper(*pty, *owner, mode);
        handle->engage();

        size_t consumed = 0;
        auto feed = makeRunable([&] {
            for (;;) {
                PtyHandle::Chunk* const chunks = handle->acquire();
                if (chunks == nullptr) {
                    return;
                }
                for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                    consumed += chunk->chunk().length();
                }
                handle->release(chunks);
            }
        });
        composer.platform->scheduler()->create(*owner, feed, 64 * 1024);

        auto* const poller = static_cast<plt::PollerLoop*>(composer.platform->poller());
        Timeout floodTimeout;
        poller->timeout(testTimeoutUs, floodTimeout);
        while (consumed < 512 * 1024 && !floodTimeout.fired) {
            poller->dispatchTimers();
            if (consumed < 512 * 1024 && !floodTimeout.fired) {
                poller->wait(poller->nextDeadline());
            }
        }
        poller->cancel(floodTimeout);
        STD_INSIST(!floodTimeout.fired);

        // Mid-flood: the feed fiber is released first (LIFO), then the
        // handle walks the two-phase goodbye with the drain. The helper
        // blocks SIGHUP while flooding and reports receiving it with zero.
        delete owner;
        const int status = reapChild();
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(StreamWriteRidesOutBackpressure) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "stty raw -echo; printf ready; sleep 1; exec cat >/dev/null";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        (void)(readUntil(*handle, "ready"));

        // The child sleeps on a raw slave first, so the kernel queue
        // fills within a few blocks: the blocking stream write must ride
        // EAGAIN through poll until cat starts draining, and still
        // deliver every byte.
        std::string input(256 * 1024, 'x');
        sendAll(*handle, input.data(), input.size());
        delete owner;
        const int status = reapChild();

        STD_INSIST(WIFSIGNALED(status) ? WTERMSIG(status) == SIGHUP : WIFEXITED(status));
    }

    // The engaged writer's budget: hundreds of queued blocks against a
    // sleeping child park the sender fiber, and the drain's progress
    // after the child wakes must resume it to completion.
    STD_TEST(EngagedWriterParksOnBudgetAndResumes) {
        EngagedPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "stty raw -echo; printf ready; sleep 1; exec cat >/dev/null";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        handle->engage();

        std::string seen;
        auto feed = makeRunable([&] {
            for (;;) {
                PtyHandle::Chunk* const chunks = handle->acquire();
                if (chunks == nullptr) {
                    return;
                }
                for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                    seen.append((const char*)(chunk->data()), chunk->length());
                }
                handle->release(chunks);
            }
        });
        fixture.composer.scheduler->create(*owner, feed, 64 * 1024);
        fixture.driveUntil([&] {
            return seen.find("ready") != std::string::npos;
        });

        std::string input(1024 * 1024, 'x');
        bool writerDone = false;
        auto writer = makeRunable([&] {
            sendAll(*handle, input.data(), input.size());
            writerDone = true;
        });
        fixture.composer.scheduler->create(*owner, writer, 64 * 1024);
        fixture.driveUntil([&] {
            return writerDone;
        });

        delete owner;
        const int status = reapChild();
        STD_INSIST(WIFSIGNALED(status) ? WTERMSIG(status) == SIGHUP : WIFEXITED(status));
    }

    // Writes queued after the child died fail the queue over to the main
    // side unwritten; the ledger still balances at destruction. The
    // sends come from the plain test thread on purpose: past the budget
    // a threadbound sender takes the bounded overrun, not the park.
    STD_TEST(EngagedWriteToDeadChildDropsTheQueue) {
        EngagedPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf ready";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        handle->engage();

        std::string seen;
        bool sawEof = false;
        auto feed = makeRunable([&] {
            for (;;) {
                PtyHandle::Chunk* const chunks = handle->acquire();
                if (chunks == nullptr) {
                    sawEof = true;
                    return;
                }
                for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                    seen.append((const char*)(chunk->data()), chunk->length());
                }
                handle->release(chunks);
            }
        });
        fixture.composer.scheduler->create(*owner, feed, 64 * 1024);
        fixture.driveUntil([&] {
            return sawEof;
        });
        STD_INSIST(seen.find("ready") != std::string::npos);
        reapChild();

        std::string input(1024 * 1024, 'x');
        sendAll(*handle, input.data(), input.size());
        delete owner;
    }

    // Registry bookkeeping across handle lifetimes on one shared drain:
    // the second handle makes the goodbye walk a two-entry chain, and
    // the third engage reuses the first one's recycled entry.
    STD_TEST(EngagedRegistryRecyclesAcrossHandles) {
        EngagedPtyFixture fixture;
        char script[] = "sleep 5";

        ObjPool* const ownerA = ObjPool::fromMemoryRaw();
        PtyHandle* const first = spawnShell(*fixture.pty, *ownerA, script);
        first->engage();
        ObjPool* const ownerB = ObjPool::fromMemoryRaw();
        PtyHandle* const second = spawnShell(*fixture.pty, *ownerB, script);
        second->engage();

        delete ownerA;
        reapChild();
        ObjPool* const ownerC = ObjPool::fromMemoryRaw();
        PtyHandle* const third = spawnShell(*fixture.pty, *ownerC, script);
        third->engage();
        delete ownerC;
        reapChild();
        delete ownerB;
        reapChild();
    }

    STD_TEST(OwnerDeathReturnsTheStreamLoan) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf payload; sleep 5";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);

        // The loan dies with the owner: the destructor releases the
        // chain the client never gave back.
        PtyHandle::Chunk* const chunks = handle->acquire();
        STD_INSIST(chunks != nullptr);
        delete owner;
        const int status = reapChild();

        STD_INSIST(WIFSIGNALED(status));
        STD_INSIST(WTERMSIG(status) == SIGHUP);
    }

    STD_TEST(EngagedOwnerDeathReturnsTheLoan) {
        EngagedPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf payload; sleep 5";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        handle->engage();

        // The feed keeps its acquired chain and parks; teardown releases
        // the fiber first, then the destructor's handshake must return
        // the loan to the drain's ledger itself.
        bool holding = false;
        auto feed = makeRunable([&] {
            PtyHandle::Chunk* const chunks = handle->acquire();
            STD_INSIST(chunks != nullptr);
            holding = true;
            fixture.composer.scheduler->current()->park();
        });
        fixture.composer.scheduler->create(*owner, feed, 64 * 1024);
        fixture.driveUntil([&] {
            return holding;
        });

        delete owner;
        const int status = reapChild();
        STD_INSIST(WIFSIGNALED(status));
        STD_INSIST(WTERMSIG(status) == SIGHUP);
    }

    STD_TEST(OwnerDeathReleasesBlockedIoAndHangsUpChild) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "hangup";
        PtyHandle* const handle = spawnHelper(*fixture.pty, *owner, mode);
        (void)(readUntil(*handle, "ready"));

        bool readerReturned = false;
        auto reader = makeRunable([&] {
            (void)!handle->acquire();
            readerReturned = true;
        });
        fixture.scheduler->create(*owner, reader);
        STD_INSIST(!readerReturned);

        // The child never reads, so an unbounded stream must park the
        // writer once the kernel buffering fills - whatever that amounts
        // to on the host: caller-stack create() only returns once the
        // fiber parks, no size calibration involved.
        std::string input(64 * 1024, 'x');
        bool writerReturned = false;
        auto writer = makeRunable([&] {
            for (;;) {
                sendAll(*handle, input.data(), input.size());
            }
            writerReturned = true;
        });
        fixture.scheduler->create(*owner, writer, 64 * 1024);
        STD_INSIST(!writerReturned);

        // LIFO pool teardown releases both client-owned fibers before the
        // handle closes the master and sends SIGHUP. A later poll round sees
        // only scheduler tombstones, never the freed stacks.
        delete owner;
        fixture.poller->wait(0);
        const int status = reapChild();

        STD_INSIST(!readerReturned);
        STD_INSIST(!writerReturned);
        STD_INSIST(WIFSIGNALED(status));
        STD_INSIST(WTERMSIG(status) == SIGHUP);
    }

    STD_TEST(RefusedSpawnThrowsAndLeaksNothing) {
        RealPtyFixture fixture;
        struct rlimit saved{};
        STD_INSIST(getrlimit(RLIMIT_NOFILE, &saved) == 0);
        // The lowest free descriptor number is what the process already
        // holds, so a limit a few above it leaves room for only a few ptys.
        const int lowestFree = open("/dev/null", O_RDONLY);
        close(lowestFree);
        struct rlimit tight = saved;
        tight.rlim_cur = (rlim_t)(lowestFree + 8);
        STD_INSIST(setrlimit(RLIMIT_NOFILE, &tight) == 0);

        const size_t firstRound = spawnUntilRefused(*fixture.pty);
        const size_t secondRound = spawnUntilRefused(*fixture.pty);
        setrlimit(RLIMIT_NOFILE, &saved);

        // Opening one more tab at the limit used to exit the whole
        // terminal. A master left open by the refused attempt would make
        // the second round smaller than the first.
        STD_INSIST(firstRound > 0);
        STD_INSIST(secondRound >= firstRound);
    }
}
