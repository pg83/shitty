/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "process_directory.h"

#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

using namespace stl;

namespace {
    // Mirrors quick_frame_store_ut.cpp's makeTempDir(): a mkdtemp()
    // directory this process owns, torn down by the caller. TMPDIR is
    // taken on trust here - nothing below cares what sits above it.
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/process_directory_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }
}

STD_TEST_SUITE(ProcessDirectory) {
    // This process is the one whose directory is known for certain, so
    // it is the one the positive control uses. Both platforms answer
    // with the kernel's own spelling of the path, which is getcwd()'s.
    STD_TEST(ALiveProcessAnswersWithItsOwnWorkingDirectory) {
        Buffer out;
        STD_INSIST(processDirectory(getpid(), out));

        char expected[PATH_MAX];
        STD_INSIST(getcwd(expected, sizeof(expected)) != nullptr);
        STD_INSIST(StringView(out) == StringView(expected));
    }

    // A pid that certainly named a process and certainly does not any
    // more: forked, exited, reaped. Anything else either might still be
    // alive or might never have existed. No crash, no answer, and out
    // left empty rather than holding whatever the last call put there.
    STD_TEST(ADeadProcessHasNoDirectoryAndLeavesTheBufferEmpty) {
        Buffer out;
        out.append("stale", 5);

        const pid_t dead = fork();
        STD_INSIST(dead >= 0);
        if (dead == 0) {
            _exit(0);
        }
        int status = 0;
        STD_INSIST(waitpid(dead, &status, 0) == dead);

        STD_INSIST(!processDirectory(dead, out));
        STD_INSIST(out.used() == 0);

        // Not a pid at all, either of them.
        out.append("stale", 5);
        STD_INSIST(!processDirectory(0, out));
        STD_INSIST(out.used() == 0);
        out.append("stale", 5);
        STD_INSIST(!processDirectory(-1, out));
        STD_INSIST(out.used() == 0);
    }

    // The answer is the other process's directory and not this one's:
    // a child parked in a directory of this test's making, one that this
    // process is provably not in. This is the shape a new tab relies on
    // - the active tab's child has cd'd somewhere, and the lookup has to
    // follow it there.
    STD_TEST(AChildInAnotherDirectoryIsReportedThere) {
        StringBuilder dir;
        makeTempDir(dir);
        // mkdtemp() spells the path the way TMPDIR did; the kernel
        // spells it canonically (/private/tmp for /tmp on macOS), so the
        // expectation is canonical too.
        char canonical[PATH_MAX];
        STD_INSIST(realpath(dir.cStr(), canonical) != nullptr);

        // Premise: the directory is somewhere this process is not, or a
        // lookup that always answered getcwd() would pass below.
        Buffer ours;
        STD_INSIST(processDirectory(getpid(), ours));
        STD_INSIST(StringView(ours) != StringView(canonical));

        int ready[2];
        STD_INSIST(pipe(ready) == 0);
        const pid_t child = fork();
        STD_INSIST(child >= 0);
        if (child == 0) {
            // A bound on how long the child can outlive a failed
            // assertion below: pause() would otherwise hold the
            // inherited stdout open past this binary's exit.
            ::alarm(20);
            // Only async-signal-safe calls before _exit: this binary may
            // be running its suite on more than one thread.
            if (::chdir(canonical) != 0) {
                _exit(1);
            }
            const char byte = 'x';
            if (::write(ready[1], &byte, 1) != 1) {
                _exit(1);
            }
            ::pause();
            _exit(0);
        }
        ::close(ready[1]);
        char byte = 0;
        STD_INSIST(::read(ready[0], &byte, 1) == 1);

        Buffer theirs;
        STD_INSIST(processDirectory(child, theirs));
        STD_INSIST(StringView(theirs) == StringView(canonical));

        STD_INSIST(::kill(child, SIGKILL) == 0);
        int status = 0;
        STD_INSIST(waitpid(child, &status, 0) == child);
        ::close(ready[0]);
        rmdir(dir.cStr());
    }
}
