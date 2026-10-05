/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pty.h"
#include "options.h"
#include "session.h"
#include "startup.h"
#include "composer.h"
#include "process_directory.h"
#include "vt_headless.h"

#include <lib/vterm/listener.h>

#include <std/tst/ut.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>
#include <std/str/builder.h>

#include <string>
#include <vector>
#include <limits.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command, const PtySize& size, StringView directory) override {
            if (spawns++ == 0) {
                doomed = real.spawn(owner, command, size, directory);
                return doomed;
            }
            return owner.make<SurvivorHandle>(composer);
        }

        Composer& composer;
        Pty& real;
        PtyHandle* doomed = nullptr;
        size_t spawns = 0;
    };

    // Everything the session's own reader takes off a real handle, copied
    // aside: the sessions own their handles, so this is the only place a
    // test can hear what a pane's child said.
    struct TeeHandle final: public PtyHandle {
        TeeHandle(PtyHandle& inner_, std::string& heard_)
            : inner(inner_)
            , heard(heard_)
        {
        }

        pid_t childPid() override {
            return inner.childPid();
        }

        pid_t foregroundProcessGroup() override {
            return inner.foregroundProcessGroup();
        }

        void resize(const PtySize& size) override {
            inner.resize(size);
        }

        void engage() override {
            inner.engage();
        }

        Chunk* allocate(size_t len) override {
            return inner.allocate(len);
        }

        void send(Chunk* chunk, size_t len) override {
            inner.send(chunk, len);
        }

        Chunk* acquire() override {
            Chunk* const chunks = inner.acquire();
            for (Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                heard.append((const char*)(chunk->data()), chunk->length());
            }
            return chunks;
        }

        void release(Chunk* chunks) override {
            inner.release(chunks);
        }

        PtyHandle& inner;
        std::string& heard;
    };

    // The child FirstLineDeadline is watching, and all a signal handler
    // is allowed to know.
    volatile sig_atomic_t deadlineChild = -1;

    void endFirstLineWait(int) {
        if (deadlineChild > 0) {
            kill((pid_t)(deadlineChild), SIGKILL);
        }
    }

    // A deadline on the wait below, because nothing under it has one:
    // acquire() on a handle nobody engaged is a blocking read, and its
    // EAGAIN path parks in poll(..., -1). A child that lives and says
    // nothing therefore hangs the whole unit_tests binary instead of
    // failing it, which in CI is a stuck job whose log does not say
    // which test is to blame. The child side of this same pty already
    // carries the same reasoning and the same remedy - alarm(10) in
    // tst/pty_test_helper.c, from R2-test I11: "A bounded wait keeps
    // that a test failure, which is what it is." This is the parent's
    // half of it.
    //
    // The child reports before it does anything else after exec, so the
    // file's own hung-test timeout is orders of magnitude more than this
    // wait ever needs. On expiry the child is killed, the master reaches
    // EOF, and readFirstLine() comes back with whatever it has - a
    // partial line reddens parseWinsize on its own.
    struct FirstLineDeadline {
        explicit FirstLineDeadline(pid_t child) {
            deadlineChild = child;
            previous = signal(SIGALRM, endFirstLineWait);
            alarm((unsigned)(testTimeoutUs / 1'000'000));
        }

        ~FirstLineDeadline() {
            alarm(0);
            signal(SIGALRM, previous);
            deadlineChild = -1;
        }

        void (*previous)(int) = nullptr;
    };

    // One line off a handle nobody has engaged yet, which is a blocking
    // read straight off the pty. A dead child ends the wait as an empty
    // string rather than as a hang: what it did or did not say is the
    // caller's assertion to make. A live but silent one is what
    // FirstLineDeadline above is for.
    void readFirstLine(PtyHandle& handle, std::string& into) {
        while (into.find('\n') == std::string::npos) {
            PtyHandle::Chunk* const chunks = handle.acquire();
            if (chunks == nullptr) {
                return;
            }
            for (PtyHandle::Chunk* chunk = chunks; chunk != nullptr; chunk = chunk->next()) {
                into.append((const char*)(chunk->data()), chunk->length());
            }
            handle.release(chunks);
        }
    }

    // Two panes' worth of the size each child was born with and of what
    // that child then said. Fixed slots rather than a vector because the
    // handles hold references into them for the pool's lifetime.
    struct BornSizePty final: public Pty {
        explicit BornSizePty(Pty& real_)
            : real(real_)
        {
        }

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command, const PtySize& size, StringView directory) override {
            STD_INSIST(spawns < 2);
            born[spawns] = size;
            PtyHandle* const inner = real.spawn(owner, command, size, directory);
            // The child's first line is taken here, still inside spawn(),
            // and not from the poller once the split has finished.
            // openSession() returns into applyLayout(), which resizes
            // every pane of the new layout - the newborn one included -
            // while the child still has a whole exec() to get through
            // before its first TIOCGWINSZ. The parent wins that race
            // every time, so a line read any later reports the size the
            // parent set *after* the fork, and a slave that was never
            // sized before the fork answers exactly the same. Reading
            // here puts the observation ahead of that resize, which is
            // the only moment at which the two states differ.
            const FirstLineDeadline deadline(inner->childPid());
            readFirstLine(*inner, heard[spawns]);
            return owner.make<TeeHandle>(*inner, heard[spawns++]);
        }

        Pty& real;
        PtySize born[2];
        std::string heard[2];
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
        explicit RealPtyFixture(const char* brand = "terminal")
            : pool(ObjPool::fromMemory())
            , poller(plt::PollerLoop::create(*pool))
            , scheduler(plt::Scheduler::create(*pool, *poller))
            , pty(createPty(*pool, *scheduler, nullptr, brand))
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
            // Task A: create() takes the Composer, not a pool and a
            // config, and the four assignments upstream makes here are
            // its own body - it installs the platform, the window and
            // the host, and counts the 80 by 24 grid out of
            // contentInsets() at one pixel per cell.
            VtermHeadless* const host = VtermHeadless::create(composer, nullptr);
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

    PtyHandle* spawnShell(Pty& pty, ObjPool& owner, char* script, StringView directory = StringView()) {
        char program[] = "pty_ut";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char* argv[] = {program, execute, shell, commandFlag, script, nullptr};
        const LaunchCommand command = buildLaunchCommand(5, argv, StringView(), false);
        return pty.spawn(owner, command, PtySize{}, directory);
    }

    PtyHandle* spawnHelper(Pty& pty, ObjPool& owner, char* mode, const PtySize& size = PtySize{}) {
        char program[] = "pty_ut";
        char execute[] = "-e";
        char* const helper = getenv("SHITTY_PTY_TEST_HELPER");
        STD_INSIST(helper != nullptr);
        char* argv[] = {program, execute, helper, mode, nullptr};
        const LaunchCommand command = buildLaunchCommand(4, argv, StringView(), false);
        return pty.spawn(owner, command, size, StringView());
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

    // "<rows> <cols>\n", the only thing the helper's winsize modes print.
    bool parseWinsize(const std::string& text, unsigned& rows, unsigned& columns) {
        return sscanf(text.c_str(), "%u %u", &rows, &columns) == 2;
    }

    // Reaping by pid, never by -1: this binary forks in more than one
    // suite, and a blind wait hands the caller whichever corpse is ready -
    // a foreign one passes or fails the caller's checks by accident.
    int reapChild(pid_t child) {
        STD_INSIST(child > 0);
        int status = 0;
        STD_INSIST(waitpid(child, &status, 0) == child);
        return status;
    }
}

STD_TEST_SUITE(Pty) {
    STD_TEST(ForegroundProcessGroupReportsTheChild) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf ready; sleep 5";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();

        // Wait for output first: only then has the child certainly
        // called setsid() and taken the controlling terminal.
        const std::string output = readUntil(*handle, "ready");
        const pid_t group = handle->foregroundProcessGroup();

        STD_INSIST(output == "ready");
        STD_INSIST(group > 0);
        delete owner;
        reapChild(child);
    }

    STD_TEST(ChildOutputReachesEof) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf pty-output";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();

        const std::string output = readAll(*handle);
        delete owner;
        const int status = reapChild(child);

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
        VtermHeadless* const host = VtermHeadless::create(composer, nullptr);
        (void)(host);

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
        const pid_t child = pty.doomed->childPid();

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

        const int status = reapChild(child);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(InputRoundTripsThroughTheSlave) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "stty -echo; IFS= read -r line; printf 'got:%s\\n' \"$line\"";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();

        const char input[] = "hello from master\n";
        sendAll(*handle, input, sizeof(input) - 1);
        const std::string output = readAll(*handle);
        delete owner;
        const int status = reapChild(child);

        STD_INSIST(output.find("got:hello from master") != std::string::npos);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(LargeChildOutputSurvivesBackpressure) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "head -c 1048576 /dev/zero";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();

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
        const int status = reapChild(child);

        STD_INSIST(total == 1024 * 1024);
        STD_INSIST(nonzero == 0);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    // This test spent a while guarded out on __APPLE__ as an undiagnosed
    // hang - a suppressed signal, or a child outside the foreground
    // process group. It is neither: SIGWINCH's default disposition is
    // "ignore", and XNU drops a signal whose disposition is SIG_IGN at
    // generation time, before it ever consults the blocked mask. A
    // sigwait() for it can therefore never return until the process
    // moves it off SIG_IGN, which is what tst/pty_test_helper.c's
    // handler exists to do (R2-test, I11). Upstream reached the same
    // answer from the other end - it probed a real Mac (tst/pty_probe.c)
    // and dropped its own guard in f482c269 - so both sides now run this
    // everywhere, and nothing should put the guard back.
    STD_TEST(ResizeReachesChildAsWinch) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "winsize";
        PtyHandle* const handle = spawnHelper(*fixture.pty, *owner, mode);
        const pid_t child = handle->childPid();

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
        const int status = reapChild(child);

        STD_INSIST(ready.find("ready") != std::string::npos);
        STD_INSIST(output.find("47 123") != std::string::npos);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    // The child reads TIOCGWINSZ as its first operation after exec, with
    // no SIGWINCH to wait for. Before the size was set on the slave ahead
    // of the fork, this answered "0 0" - the race ResizeReachesChildAsWinch
    // cannot see, because it prints ready before the resize it waits for.
    STD_TEST(TheChildIsBornWithTheSizeSpawnWasGiven) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "winsize-now";
        const PtySize born{
            .columns = 123,
            .rows = 47,
            .pixelWidth = 984,
            .pixelHeight = 752,
        };
        PtyHandle* const handle = spawnHelper(*fixture.pty, *owner, mode, born);
        const pid_t child = handle->childPid();

        const std::string output = readAll(*handle);
        delete owner;
        const int status = reapChild(child);

        unsigned rows = 0;
        unsigned columns = 0;
        STD_INSIST(parseWinsize(output, rows, columns));
        STD_INSIST(rows == 47);
        STD_INSIST(columns == 123);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    // A8 end to end: the pane a split creates is told its geometry the
    // same way the first one is - at spawn, before the fork - so both
    // children can read it with their first operation. BornSizePty reads
    // that first operation inside spawn(), which is what makes the test
    // able to fail: applyLayout() resizes the newborn pane the moment
    // splitFocused() gets its session back, and until the observation
    // was moved ahead of it a slave sized only by that resize looked no
    // different here. Both children hold after reporting, so neither
    // pane closes and rewrites its sibling's geometry mid-test - a
    // second belt now rather than the load-bearing one, since the
    // reading is taken before anything drives the loop that would notice
    // a death (R1a-test round 2, finding 4).
    STD_TEST(EveryPanesChildIsBornWithThatPanesSize) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        // splitFocused() refuses while panes are off, as they are by default.
        options.panes = true;
        composer.setOptions(&options);
        VtermHeadless* const host = VtermHeadless::create(composer, nullptr);
        (void)(host);

        char program[] = "pty_ut";
        char execute[] = "-e";
        char* const helper = getenv("SHITTY_PTY_TEST_HELPER");
        STD_INSIST(helper != nullptr);
        char mode[] = "winsize-now-hold";
        char* argv[] = {program, execute, helper, mode, nullptr};
        const LaunchCommand command = buildLaunchCommand(4, argv, StringView(), false);

        // The production drain thread and its arena live until process exit.
        ObjPool* const ptyOwner = ObjPool::fromMemoryRaw();
        Pty* const real = createPty(*ptyOwner, *composer.platform->scheduler(), composer.platform);
        BornSizePty pty(*real);
        composer.pty = &pty;
        composer.launch = &command;
        // create() already opens the first session; a new tab on top of it
        // would be a third spawn this test has no slot for.
        SessionSet* const sessions = SessionSet::create(composer);
        STD_INSIST(sessions->splitFocused(SplitDirection::Vertical));
        STD_INSIST(pty.spawns == 2);

        // A vertical split halves the width and leaves the height alone.
        STD_INSIST(pty.born[0].rows != 0);
        STD_INSIST(pty.born[0].columns != 0);
        STD_INSIST(pty.born[1].rows == pty.born[0].rows);
        STD_INSIST(pty.born[1].columns != 0);
        STD_INSIST(pty.born[1].columns < pty.born[0].columns);

        // Each child is held to the size its own spawn() was given, both
        // axes. The first pane's child answered before the split existed
        // and the second one's before the layout pass that follows it,
        // so neither number can be one the parent set after the fork.
        unsigned rows = 0;
        unsigned columns = 0;
        STD_INSIST(parseWinsize(pty.heard[0], rows, columns));
        STD_INSIST(rows == pty.born[0].rows);
        STD_INSIST(columns == pty.born[0].columns);
        STD_INSIST(parseWinsize(pty.heard[1], rows, columns));
        STD_INSIST(rows == pty.born[1].rows);
        STD_INSIST(columns == pty.born[1].columns);
    }

    // The engaged path's hairy exit: the arena dies while the drain is
    // mid-flood and the feed holds acquired blocks. The destructor's
    // handshake must balance the ledger and hang up the child.
    STD_TEST(EngagedOwnerDeathSurvivesAFloodingChild) {
        // An engaged PTY starts the process-lifetime drain thread, so its
        // platform, scheduler and arena follow the production lifetime too.
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        VtermHeadless* const host = VtermHeadless::create(composer, nullptr);
        (void)(host);
        Pty* const pty = createPty(*composer.pool, *composer.platform->scheduler(), composer.platform);
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "flood-hangup";
        PtyHandle* const handle = spawnHelper(*pty, *owner, mode);
        const pid_t child = handle->childPid();
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
        const int status = reapChild(child);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);
    }

    STD_TEST(StreamWriteRidesOutBackpressure) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "stty raw -echo; printf ready; sleep 1; exec cat >/dev/null";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();
        (void)(readUntil(*handle, "ready"));

        // The child sleeps on a raw slave first, so the kernel queue
        // fills within a few blocks: the blocking stream write must ride
        // EAGAIN through poll until cat starts draining, and still
        // deliver every byte.
        std::string input(256 * 1024, 'x');
        sendAll(*handle, input.data(), input.size());
        delete owner;
        const int status = reapChild(child);

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
        const pid_t child = handle->childPid();
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
        const int status = reapChild(child);
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
        const pid_t child = handle->childPid();
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
        reapChild(child);

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
        const pid_t firstChild = first->childPid();
        first->engage();
        ObjPool* const ownerB = ObjPool::fromMemoryRaw();
        PtyHandle* const second = spawnShell(*fixture.pty, *ownerB, script);
        const pid_t secondChild = second->childPid();
        second->engage();

        delete ownerA;
        reapChild(firstChild);
        ObjPool* const ownerC = ObjPool::fromMemoryRaw();
        PtyHandle* const third = spawnShell(*fixture.pty, *ownerC, script);
        const pid_t thirdChild = third->childPid();
        third->engage();
        delete ownerC;
        reapChild(thirdChild);
        delete ownerB;
        reapChild(secondChild);
    }

    STD_TEST(OwnerDeathReturnsTheStreamLoan) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf payload; sleep 5";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();

        // The loan dies with the owner: the destructor releases the
        // chain the client never gave back.
        PtyHandle::Chunk* const chunks = handle->acquire();
        STD_INSIST(chunks != nullptr);
        delete owner;
        const int status = reapChild(child);

        STD_INSIST(WIFSIGNALED(status));
        STD_INSIST(WTERMSIG(status) == SIGHUP);
    }

    STD_TEST(EngagedOwnerDeathReturnsTheLoan) {
        EngagedPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "printf payload; sleep 5";
        PtyHandle* const handle = spawnShell(*fixture.pty, *owner, script);
        const pid_t child = handle->childPid();
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
        const int status = reapChild(child);
        STD_INSIST(WIFSIGNALED(status));
        STD_INSIST(WTERMSIG(status) == SIGHUP);
    }

    STD_TEST(OwnerDeathReleasesBlockedIoAndHangsUpChild) {
        RealPtyFixture fixture;
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char mode[] = "hangup";
        PtyHandle* const handle = spawnHelper(*fixture.pty, *owner, mode);
        const pid_t child = handle->childPid();
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
        const int status = reapChild(child);

        STD_INSIST(!readerReturned);
        STD_INSIST(!writerReturned);
        STD_INSIST(WIFSIGNALED(status));
        STD_INSIST(WTERMSIG(status) == SIGHUP);
    }
}

namespace {
    // Mirrors quick_frame_store_ut.cpp's makeTempDir(): a mkdtemp()
    // directory this process owns, torn down by the caller.
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/pty_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }

    // The kernel's spelling of a path: mkdtemp() spells it the way
    // TMPDIR did, and on macOS /tmp is a link to /private/tmp, so every
    // comparison against what a child reports goes through here.
    std::string canonical(const char* path) {
        char out[PATH_MAX];
        STD_INSIST(realpath(path, out) != nullptr);
        return out;
    }

    std::string ownDirectory() {
        char cwd[PATH_MAX];
        STD_INSIST(getcwd(cwd, sizeof(cwd)) != nullptr);
        return canonical(cwd);
    }

    std::string launched(StringView option, StringView inherited, StringView home) {
        Buffer out;
        launchDirectory(option, inherited, home, out);
        return std::string((const char*)(out.data()), out.used());
    }

    // What a shell child spawned into `directory` reports as its
    // directory: `pwd -P`, the physical path, and not $PWD - which the
    // child inherits from this process, and which a shell that had not
    // moved would print straight back. The whole transcript is kept for
    // the callers that want to see what was said before the answer.
    std::string childDirectory(Pty& pty, StringView directory, std::string& transcript) {
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        char script[] = "pwd -P";
        PtyHandle* const handle = spawnShell(pty, *owner, script, directory);
        const pid_t child = handle->childPid();
        transcript = readAll(*handle);
        delete owner;
        const int status = reapChild(child);
        STD_INSIST(WIFEXITED(status));
        STD_INSIST(WEXITSTATUS(status) == 0);

        // The last non-empty line, with the slave's ONLCR undone.
        std::string answer;
        size_t end = transcript.size();
        while (end > 0 && (transcript[end - 1] == '\n' || transcript[end - 1] == '\r')) {
            --end;
        }
        size_t begin = end;
        while (begin > 0 && transcript[begin - 1] != '\n' && transcript[begin - 1] != '\r') {
            --begin;
        }
        return transcript.substr(begin, end - begin);
    }

    std::string childDirectory(Pty& pty, StringView directory) {
        std::string transcript;
        return childDirectory(pty, directory, transcript);
    }

    // Blocks until the kernel reports `child` in `directory`, so the test
    // waits on the fact and not on a timer. A child that never gets there
    // fails here, in time.
    void waitUntilDirectory(pid_t child, const std::string& directory) {
        Buffer out;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (processDirectory(child, out) && StringView(out) == StringView(directory.c_str())) {
                return;
            }
            usleep(10 * 1000);
        }
        STD_INSIST(!"the child never reached the directory");
    }

    // Records what each spawn() was told about the directory, so the
    // decision SessionSet makes for a new tab is observable at the seam
    // it crosses and not only through a second child's cooperation.
    struct DirectoryRecordingPty final: public Pty {
        explicit DirectoryRecordingPty(Pty& real_)
            : real(real_)
        {
        }

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command, const PtySize& size, StringView directory) override {
            given.push_back(std::string((const char*)(directory.data()), directory.length()));
            PtyHandle* const inner = real.spawn(owner, command, size, directory);
            children.push_back(inner->childPid());
            return inner;
        }

        Pty& real;
        std::vector<std::string> given;
        std::vector<pid_t> children;
    };
}

STD_TEST_SUITE(LaunchDirectory) {
    // The rule, in the pure form application.cpp calls it in: nothing
    // here moves this process anywhere.
    STD_TEST(ASetOptionWinsAndATildeMeansHome) {
        STD_INSIST(launched(StringView(u8"/srv/work"), StringView(u8"/"), StringView(u8"/home/u")) == "/srv/work");
        STD_INSIST(launched(StringView(u8"/srv/work"), StringView(u8"/home/u/elsewhere"), StringView(u8"/home/u")) == "/srv/work");
        STD_INSIST(launched(StringView(u8"~"), StringView(u8"/"), StringView(u8"/home/u")) == "/home/u");
        STD_INSIST(launched(StringView(u8"~/src"), StringView(u8"/"), StringView(u8"/home/u")) == "/home/u/src");
        // Only `~` and `~/`: another user's `~name` is not a spelling
        // this terminal expands, and a relative path is left relative.
        STD_INSIST(launched(StringView(u8"~name/src"), StringView(u8"/"), StringView(u8"/home/u")) == "~name/src");
        STD_INSIST(launched(StringView(u8"src"), StringView(u8"/"), StringView(u8"/home/u")) == "src");
        // No home to expand into: passed on as written, for the child to
        // fail on out loud rather than for the parent to guess.
        STD_INSIST(launched(StringView(u8"~/src"), StringView(u8"/"), StringView()) == "~/src");
    }

    STD_TEST(AnUnsetOptionInheritsExceptFromTheRootWhichBecomesHome) {
        STD_INSIST(launched(StringView(), StringView(u8"/home/u/elsewhere"), StringView(u8"/home/u")) == "");
        STD_INSIST(launched(StringView(), StringView(u8"/"), StringView(u8"/home/u")) == "/home/u");
        // Exactly the root, not anything that starts like it.
        STD_INSIST(launched(StringView(), StringView(u8"/srv"), StringView(u8"/home/u")) == "");
        STD_INSIST(launched(StringView(), StringView(u8"//"), StringView(u8"/home/u")) == "");
        // No home known: the root is inherited, because there is nothing
        // better to say and nothing to complain about.
        STD_INSIST(launched(StringView(), StringView(u8"/"), StringView()) == "");
        // No launcher directory known at all - getcwd() failed - is not
        // the root either.
        STD_INSIST(launched(StringView(), StringView(), StringView(u8"/home/u")) == "");
    }

    STD_TEST(HomeDirectoryFollowsTheEnvironment) {
        Buffer home;
        homeDirectory(home);
        const char* const environment = getenv("HOME");
        if (environment != nullptr && environment[0] != '\0') {
            STD_INSIST(StringView(home) == StringView(environment));
        } else {
            // The passwd fallback: a user running this suite has one.
            STD_INSIST(home.used() != 0);
        }
    }

    // The seam itself: the child enters the directory spawn() was given,
    // between fork and exec, and reports it from there. Removing the
    // chdir from PtyImpl::spawn() reddens this one by name.
    STD_TEST(TheChildStartsInTheDirectorySpawnWasGiven) {
        RealPtyFixture fixture;
        StringBuilder dir;
        makeTempDir(dir);
        const std::string expected = canonical(dir.cStr());
        // Premise: somewhere this process is not, or a child that never
        // moved would pass.
        STD_INSIST(expected != ownDirectory());

        STD_INSIST(childDirectory(*fixture.pty, StringView(dir)) == expected);
        rmdir(dir.cStr());
    }

    STD_TEST(AnEmptyDirectoryInheritsThisProcesss) {
        RealPtyFixture fixture;
        STD_INSIST(childDirectory(*fixture.pty, StringView()) == ownDirectory());
    }

    // End to end through the rule: `~/sub` with the test's own home
    // stands in for the user's, and the child lands in sub.
    STD_TEST(ATildeIsExpandedAndTheChildLandsThere) {
        RealPtyFixture fixture;
        StringBuilder home;
        makeTempDir(home);
        StringBuilder sub;
        sub << StringView(home) << StringView(u8"/sub");
        STD_INSIST(mkdir(sub.cStr(), 0700) == 0);
        const std::string expected = canonical(sub.cStr());
        STD_INSIST(expected != ownDirectory());

        Buffer directory;
        launchDirectory(StringView(u8"~/sub"), StringView(u8"/whatever"), StringView(home), directory);
        STD_INSIST(childDirectory(*fixture.pty, StringView(directory)) == expected);
        rmdir(sub.cStr());
        rmdir(home.cStr());
    }

    // The launchd case end to end: a launcher in `/` and no option, and
    // the child starts at home - here a directory of the test's making,
    // so that "home" and "/" are two different places for certain.
    STD_TEST(ALauncherInTheRootDirectoryStartsTheChildAtHome) {
        RealPtyFixture fixture;
        StringBuilder home;
        makeTempDir(home);
        const std::string expected = canonical(home.cStr());
        STD_INSIST(expected != "/");
        STD_INSIST(expected != ownDirectory());

        Buffer directory;
        launchDirectory(StringView(), StringView(u8"/"), StringView(home), directory);
        // The rule hands home over as spelled; the child's answer below
        // is the kernel's spelling of the same place.
        STD_INSIST(StringView(directory) == StringView(home));
        STD_INSIST(childDirectory(*fixture.pty, StringView(directory)) == expected);
        rmdir(home.cStr());
    }

    // A directory that is not there is a complaint in the terminal and a
    // shell where it would have started anyway - never a terminal that
    // fails to open. The complaint carries the brand the factory was
    // given, the way every other message of the process does.
    STD_TEST(AnUnenterableDirectoryIsReportedAndTheChildStartsWhereItWouldHave) {
        RealPtyFixture fixture("pty_ut");
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder missing;
        missing << StringView(dir) << StringView(u8"/missing");

        std::string transcript;
        STD_INSIST(childDirectory(*fixture.pty, StringView(missing), transcript) == ownDirectory());
        std::string complaint = "pty_ut: cannot change directory to ";
        complaint += missing.cStr();
        complaint += ": No such file or directory";
        STD_INSIST(transcript.find(complaint) != std::string::npos);
        rmdir(dir.cStr());
    }

    // The decision a new tab makes, observed at the spawn() seam: the
    // first session gets the launch directory, the next one the directory
    // the active tab's foreground process is in, and a directory removed
    // under that process sends the next tab back to the launch rule with
    // no complaint. Three answers, all different, all of the test's own
    // making.
    STD_TEST(ANewTabStartsWhereTheActiveTabsForegroundProcessIs) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        composer.setOptions(&options);
        VtermHeadless* const host = VtermHeadless::create(composer, nullptr);
        (void)(host);

        StringBuilder work;
        makeTempDir(work);
        StringBuilder launch;
        makeTempDir(launch);
        const std::string workCanonical = canonical(work.cStr());
        const std::string launchCanonical = canonical(launch.cStr());
        // Premise: three distinguishable places, or a session set that
        // always answered one of them would pass below.
        STD_INSIST(workCanonical != launchCanonical);
        STD_INSIST(workCanonical != ownDirectory());
        STD_INSIST(launchCanonical != ownDirectory());

        // Every child moves into work while it exists; exec keeps the
        // shell's pid, so the foreground group's leader is the process
        // that moved.
        StringBuilder script;
        script << StringView(u8"cd ") << StringView(workCanonical.c_str()) << StringView(u8" 2>/dev/null; exec sleep 30");
        char program[] = "pty_ut";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char* argv[] = {program, execute, shell, commandFlag, script.cStr(), nullptr};
        LaunchCommand command = buildLaunchCommand(5, argv, StringView(), false);
        command.directory.append(launchCanonical.data(), launchCanonical.size());

        // The production drain thread and its arena live until process exit.
        ObjPool* const ptyOwner = ObjPool::fromMemoryRaw();
        Pty* const real = createPty(*ptyOwner, *composer.platform->scheduler(), composer.platform);
        DirectoryRecordingPty pty(*real);
        composer.pty = &pty;
        composer.launch = &command;
        // create() opens the first session: rule A, the launch directory.
        SessionSet* const sessions = SessionSet::create(composer);
        STD_INSIST(pty.given.size() == 1);
        STD_INSIST(pty.given[0] == launchCanonical);

        waitUntilDirectory(pty.children[0], workCanonical);
        sessions->newSession();
        STD_INSIST(pty.given.size() == 2);
        STD_INSIST(pty.given[1] == workCanonical);

        // The first tab's process is still in work when work goes away.
        // Back to that tab, and the next new tab falls back to rule A.
        STD_INSIST(rmdir(work.cStr()) == 0);
        sessions->activate(0);
        sessions->newSession();
        STD_INSIST(pty.given.size() == 3);
        STD_INSIST(pty.given[2] == launchCanonical);

        for (const pid_t child : pty.children) {
            STD_INSIST(::kill(child, SIGKILL) == 0);
            reapChild(child);
        }
        rmdir(launch.cStr());
    }
}
