/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/pty.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;
    struct Scheduler;
}

struct LaunchCommand;

// Process-lifetime factory. It knows how to create OS pseudoterminals and
// children, but nothing about sessions, terminal parsers, windows or their
// lifetimes. The drain thread and its main-loop doorbell start on the
// first engage() and live until exit(); the platform may be null when no
// handle is ever engaged.
struct Pty {
    // The size is set on the slave before the fork, not by a resize()
    // after it: a child which reads TIOCGWINSZ as its first operation
    // after exec would otherwise race that resize and see 0x0.
    //
    // directory is where the child starts, entered by the child itself
    // between fork and exec; empty inherits this process's. A directory
    // that cannot be entered is not fatal: the child says so on its own
    // stderr - the terminal, where the user is looking - and starts
    // where it would have anyway. A terminal that fails to open over a
    // typo in the config is worse than one open in the wrong folder.
    virtual PtyHandle* spawn(stl::ObjPool& owner, const LaunchCommand& command, const PtySize& size, stl::StringView directory) = 0;
};

// brand prefixes what a child reports before exec, the way every other
// message of this process is prefixed; a NUL-terminated string that
// outlives the factory.
Pty* createPty(stl::ObjPool& owner, plt::Scheduler& scheduler, plt::Platform* platform = nullptr, const char* brand = "terminal");
