/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

/* part of this file is part of Zutty.
 * Copyright (C) 2020 Tom Szilagyi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * See the file LICENSE.GPL3 for the full license.
 */

/* The source code in this file is inspired by code samples in the book
 *   Advanced Programming in the UNIX Environment, 3rd Edition
 *   by W. Richard Stevens & Stephen A. Rago
 *   Addison-Wesley, 2013
 *
 * The original example code of the book is available from
 *   http://www.apuebook.com/code3e.html
 */

#include "pty.h"

#include "composer.h"
#include "fd_redirect.h"
#include "listener.h"
#include "poller.h"
#include "vterm.h"
#include "window.h"

#include <std/mem/obj_pool.h>
#include <std/str/view.h>
#include <std/sys/crt.h>

#define _POSIX_C_SOURCE 200809L

#if defined(SOLARIS) /* Solaris 10 */
    #define _XOPEN_SOURCE 600
#else
    #define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>

#include <stdexcept>
#include <string>

using namespace stl;

#if defined(BSD) || defined(MACOS) || !defined(TIOCGWINSZ)
    #include <sys/ioctl.h>
#endif

#if defined(SOLARIS)
    #include <stropts.h>
#endif

namespace {

    struct PtyImpl;

    struct CallPtyResize final: public Listener {
        explicit CallPtyResize(PtyImpl* pty);

        void onListen(void*) override;

        PtyImpl* pty;
    };

    struct CallPtyReady final: public Listener {
        explicit CallPtyReady(PtyImpl* pty);

        void onListen(void* argument) override;

        PtyImpl* pty;
    };

    struct PtyImpl final: public Pty {
        PtyImpl(Composer& composer, int fd);
        ~PtyImpl();

        int fd() const override;
        ssize_t read(u8* buffer, size_t size) override;
        ssize_t write(const u8* buffer, size_t size) override;
        void outputReady() override;

        void applySize();
        void ready(const FDReady& event);
        void updateInterest();
        bool flushOutput();
        bool readInput();

        Composer& composer_;
        int fd_;
        bool handlingReady = false;
        bool finished = false;
    };

}

CallPtyResize::CallPtyResize(PtyImpl* pty_)
    : pty(pty_)
{
}

void CallPtyResize::onListen(void*) {
    pty->applySize();
}

CallPtyReady::CallPtyReady(PtyImpl* pty_)
    : pty(pty_)
{
}

void CallPtyReady::onListen(void* argument) {
    pty->ready(*(const FDReady*)(argument));
}

PtyImpl::PtyImpl(Composer& composer, int fd)
    : composer_(composer)
    , fd_(fd)
{
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        const int error = errno;
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("cannot make PTY nonblocking: " + std::string(strerror(error)));
    }
    composer_.resizedListeners.pushBack(composer_.pool->make<CallPtyResize>(this));
    composer_.onFDReady.pushFront(composer_.pool->make<CallPtyReady>(this));
    composer_.poller->arm(fd_, PollRead);
}

PtyImpl::~PtyImpl() {
    if (fd_ >= 0) {
        composer_.poller->disarm(fd_);
        close(fd_);
    }
    composer_.pty = nullptr;
}

int PtyImpl::fd() const {
    return fd_;
}

ssize_t PtyImpl::read(u8* buffer, size_t size) {
    return ::read(fd_, buffer, size);
}

ssize_t PtyImpl::write(const u8* buffer, size_t size) {
    return ::write(fd_, buffer, size);
}

void PtyImpl::outputReady() {
    if (!handlingReady && !finished) {
        flushOutput();
        updateInterest();
    }
}

void PtyImpl::applySize() {
    pty_resize(fd_, composer_.columns, composer_.rows);
}

void PtyImpl::ready(const FDReady& event) {
    if (event.fd != fd_ || finished) {
        return;
    }

    handlingReady = true;
    if (event.what & PollWrite) {
        flushOutput();
    }
    if (event.what & (PollRead | PollError | PollHangup)) {
        finished = readInput();
    }
    flushOutput();
    handlingReady = false;

    if (finished) {
        composer_.poller->disarm(fd_);
        composer_.window->requestClose();
    } else {
        updateInterest();
    }
}

void PtyImpl::updateInterest() {
    int mode = PollRead;
    if (composer_.vterm != nullptr && !composer_.vterm->ptyOutput().empty()) {
        mode |= PollWrite;
    }
    composer_.poller->arm(fd_, mode);
}

bool PtyImpl::flushOutput() {
    Vterm* const vterm = composer_.vterm;
    if (vterm == nullptr) {
        return true;
    }
    while (true) {
        const StringView output = vterm->ptyOutput();
        if (output.empty()) {
            return true;
        }
        const ssize_t count = write(output.data(), output.length());
        if (count > 0) {
            vterm->consumePtyOutput((size_t)(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            sysWarn("pty write");
        }
        return false;
    }
}

bool PtyImpl::readInput() {
    constexpr size_t maxDrainBytes = 256 * 1024;
    constexpr u64 maxDrainMicroseconds = 4'000;
    Vterm* const vterm = composer_.vterm;
    u8 buffer[8192];
    size_t drained = 0;
    const u64 deadline = monotonicNowUs() + maxDrainMicroseconds;
    while (drained < maxDrainBytes && monotonicNowUs() < deadline) {
        const ssize_t count = read(buffer, sizeof(buffer));
        if (count > 0) {
            vterm->feedPty(StringView(buffer, count));
            drained += (size_t)(count);
            continue;
        }
        if (count == 0 || (count < 0 && errno == EIO)) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        sysWarn("pty read");
        return true;
    }
    return false;
}

Pty* Pty::adopt(Composer& composer, int fd) {
    return composer.pool->make<PtyImpl>(composer, fd);
}

int ptym_open(char* pts_name, int pts_namesz) {
    char* ptr;
    int fdm;

    if ((fdm = posix_openpt(O_RDWR)) < 0) {
        sysError("can't open master pty: posix_openpt()");
    }
    if (grantpt(fdm) < 0) {
        sysError("can't open master pty: grantpt()");
    }
    if (unlockpt(fdm) < 0) {
        sysError("can't open master pty: unlockpt()");
    }
    if ((ptr = ptsname(fdm)) == nullptr) {
        sysError("can't open master pty: ptsname()");
    }

    strncpy(pts_name, ptr, pts_namesz);
    pts_name[pts_namesz - 1] = '\0';
    return fdm;
}

int ptys_open(char* pts_name) {
    int fds = open(pts_name, O_RDWR);
    if (fds < 0) {
        sysError("can't open slave pty: open()");
    }

#if defined(SOLARIS)

    int setup;
    if ((setup = ioctl(fds, I_FIND, "ldterm")) < 0) {
        sysError("can't open slave pty: ioctl(I_FIND, ldterm)");
    }

    if (setup == 0) {
        if (ioctl(fds, I_PUSH, "ptem") < 0) {
            sysError("can't open slave pty: ioctl(I_PUSH, ptem)");
        }
        if (ioctl(fds, I_PUSH, "ldterm") < 0) {
            sysError("can't open slave pty: ioctl(I_PUSH, ldterm)");
        }
        if (ioctl(fds, I_PUSH, "ttcompat") < 0) {
            sysError("can't open slave pty: ioctl(I_PUSH, ttcompat)");
        }
    }
#endif
    return fds;
}

pid_t pty_fork(int& o_ptyFd, int cols, int rows) {
    pid_t pid;
    char pts_name[20];
    int fdm = ptym_open(pts_name, sizeof(pts_name));

    pid = fork();

    if (pid < 0) {
        return pid;
    } else if (pid == 0) {
        if (setsid() < 0) {
            sysError("setsid");
        }

        int fds = ptys_open(pts_name);

        close(fdm);

#if defined(BSD)

        if (ioctl(fds, TIOCSCTTY, nullptr) < 0) {
            sysError("TIOCSCTTY");
        }
#endif

        pty_resize(fds, cols, rows);

        redirectFds(fds);

#if defined(LINUX) || defined(MACOS)

        struct termios term;
        if (tcgetattr(STDIN_FILENO, &term) < 0) {
            sysError("tcgetattr");
        }
        term.c_iflag |= IUTF8;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &term) < 0) {
            sysError("tcsetattr");
        }
#endif
    } else {
        o_ptyFd = fdm;
    }
    return pid;
}

void pty_resize(int ptyFd, int cols, int rows) {
    struct winsize wsize{};
    wsize.ws_col = cols;
    wsize.ws_row = rows;
    if (ioctl(ptyFd, TIOCSWINSZ, &wsize) < 0) {
        sysError("TIOCSWINSZ on pty");
    }
}
