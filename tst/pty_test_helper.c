/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#define _POSIX_C_SOURCE 200809L
// cfmakeraw() is a BSD extension, not POSIX, so _POSIX_C_SOURCE alone hides
// it: glibc gates it on __USE_MISC and musl on _BSD_SOURCE, and naming any
// _POSIX_C_SOURCE turns off the default that would otherwise set those.
// _DEFAULT_SOURCE turns them back on in both libcs, and means nothing on
// Darwin, where _DARWIN_C_SOURCE below does the same job.
#define _DEFAULT_SOURCE
#if defined(__APPLE__)
// SIGWINCH is a BSD extension, and _POSIX_C_SOURCE above lowers
// __DARWIN_C_LEVEL far enough to hide it - so this file never compiled on
// macOS, the unit_tests nodes that depend on it were never in the build
// graph, and three rounds of "./build test is green" measured a baseline
// with no C++ tests in it at all (R2-test, I11).
#define _DARWIN_C_SOURCE
#endif

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t signal_seen = 0;

static void note_signal(int received) {
    (void)(received);
    signal_seen = 1;
}

/* No SA_RESTART: a signal must interrupt a blocked write. */
static int arm_signal(int which) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = note_signal;
    return sigaction(which, &action, NULL);
}

static int write_all(const char* data, size_t size) {
    while (size != 0) {
        const ssize_t written = write(STDOUT_FILENO, data, size);
        if (written <= 0) {
            return 1;
        }
        data += written;
        size -= (size_t)(written);
    }
    return 0;
}

static int ready(void) {
    static const char message[] = "ready\n";
    return write_all(message, sizeof(message) - 1);
}

static int report_winsize(void) {
    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0) {
        return 1;
    }
    char message[64];
    const int length = snprintf(message, sizeof(message), "%u %u\n", (unsigned)(size.ws_row), (unsigned)(size.ws_col));
    if (length <= 0 || (size_t)(length) >= sizeof(message)) {
        return 1;
    }
    return write_all(message, (size_t)(length));
}

static int wait_for_winsize(void) {
    sigset_t signals;
    // SIGWINCH's default disposition is "ignore", and XNU drops a signal
    // whose disposition is SIG_IGN at generation time - psignal_internal()
    // tests p_sigignore before it ever looks at the blocked mask, and
    // siginit() puts SIGWINCH there for every process. So on macOS a
    // sigwait() for SIGWINCH never returns unless the process first moves
    // the signal off SIG_IGN, and this handler exists for nothing else:
    // sigwait() accepts the signal itself and the handler never runs.
    //
    // Linux hides the bug entirely - sig_ignored() returns false for any
    // blocked signal, so the same code works there. That is why this was
    // invisible until wave 2 made this file compile on macOS at all
    // (R2-test, I11), and why the failure looked like "resize does not
    // reach the child" rather than "the child cannot receive it".
    if (arm_signal(SIGWINCH) != 0) {
        return 1;
    }

    // SIGALRM rides along so the wait below is bounded. macOS has no
    // sigtimedwait(), and an unbounded sigwait() here is what a resize
    // that never reaches the child looks like from the outside: the
    // parent blocks in poll() waiting for output that will never come,
    // and the whole unit_tests binary hangs instead of failing. A bounded
    // wait keeps that a test failure, which is what it is.
    if (sigemptyset(&signals) != 0 ||
        sigaddset(&signals, SIGWINCH) != 0 ||
        sigaddset(&signals, SIGALRM) != 0 ||
        sigprocmask(SIG_BLOCK, &signals, NULL) != 0 ||
        ready() != 0) {
        return 1;
    }

    alarm(10);
    int received = 0;
    if (sigwait(&signals, &received) != 0 || received != SIGWINCH) {
        return 1;
    }
    alarm(0);
    return report_winsize();
}

static int wait_for_hangup(void) {
    sigset_t signals;
    // The raw mode this mode needs is main()'s, applied to every mode.
    if (signal(SIGHUP, SIG_DFL) == SIG_ERR || sigemptyset(&signals) != 0 || sigaddset(&signals, SIGHUP) != 0 || sigprocmask(SIG_UNBLOCK, &signals, NULL) != 0 || ready() != 0) {
        return 1;
    }
    for (;;) {
        pause();
    }
}

static int flood_until_hangup(void) {
    /* Same xnu sigwait trap as the winsize wait: a handler notes the
     * hangup, and without SA_RESTART it interrupts a blocked write. */
    if (arm_signal(SIGHUP) != 0) {
        return 1;
    }
    static const char payload[] = "engaged-flood\n";
    for (;;) {
        if (write_all(payload, sizeof(payload) - 1) != 0) {
            while (!signal_seen) {
                pause();
            }
            return 0;
        }
        if (signal_seen) {
            return 0;
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    // Raw mode, like any full-screen application, because the flood modes
    // need a writer that blocks mid-send. A canonical pty on macOS never
    // blocks the writer: ptcwrite() hands bytes to ttyinput(), which
    // discards everything past TTYHOG and rings the bell instead of
    // pushing back - measured at 64 MiB accepted with no EAGAIN, against
    // 1022 bytes in raw mode. Linux bounds the canonical queue too, which
    // is why the flood blocked there and this file's own test asserted
    // that it would.
    //
    // A real child does this for itself: every shell that draws its own
    // line takes the tty out of canonical mode. Doing it here keeps the
    // tests measuring the teardown they are named for instead of a
    // platform's line discipline. It has to be main() and not the hangup
    // mode alone: with the flood writer parked in a blocking write, only
    // a handler without SA_RESTART wakes it, which is why arm_signal and
    // this belong together.
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &raw) == 0) {
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    if (strcmp(argv[1], "winsize") == 0) {
        return wait_for_winsize();
    }
    if (strcmp(argv[1], "winsize-now") == 0) {
        // The size the child was born with, read as its first operation
        // and without waiting for a SIGWINCH. The "winsize" mode above
        // cannot observe it: it prints ready before the resize it waits
        // for, so a 0x0 slave and a correctly sized one look the same.
        return report_winsize();
    }
    if (strcmp(argv[1], "winsize-now-hold") == 0) {
        // Same first read as "winsize-now", but the child then stays
        // alive. A caller which watches two panes needs that: the pane
        // whose child exits is closed, and the survivor is laid out over
        // the whole content box - so a child that dies while its sibling
        // is still starting up rewrites the very size the sibling is
        // about to report.
        //
        // Its one caller no longer depends on this to pass: it takes
        // both readings inside spawn(), before anything drives the loop
        // that would notice a death, so swapping this mode for
        // "winsize-now" reddens nothing today (R1a-test round 2,
        // finding 4). Kept because that is an accident of where the
        // reading is taken rather than a property of the two panes, and
        // this is what says out loud that a test watching two children
        // wants both of them alive.
        if (report_winsize() != 0) {
            return 1;
        }
        for (;;) {
            pause();
        }
    }
    if (strcmp(argv[1], "hangup") == 0) {
        return wait_for_hangup();
    }
    if (strcmp(argv[1], "flood-hangup") == 0) {
        return flood_until_hangup();
    }
    return 2;
}
