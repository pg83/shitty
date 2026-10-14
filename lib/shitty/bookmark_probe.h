/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    class StringBuilder;
}

struct Composer;
struct BookmarkShelf;

// Whether an ssh bookmark's host answers, asked in the background so the
// sidebar can say "unreachable" before the user clicks and waits out a
// connect timeout.
//
// One thread for the process, started by the first watch() with anything
// to watch. Every round it takes the watched hosts, looks each one up in
// ~/.ssh/config - the HostName and Port an alias stands for, which a
// bookmark written as `ssh prod` needs - and tries a TCP connection to it
// with a short timeout. A round runs every bookmarkProbeIntervalSeconds,
// and at once after a watch(). When an answer changes, the main loop is
// woken and the composer's sessionsChangedListeners hear it, which is
// what the sidebar redraws on.
//
// The config is read here rather than asked of `ssh -G`: a child of ours
// would be reaped by the SIGCHLD handler, whose recorded status is the
// one the terminal exits with.
struct BookmarkProbe {
    // Replaces the watched hosts: every bookmark on the shelf whose
    // command is an ssh one. Main thread.
    virtual void watch(const BookmarkShelf& shelf) = 0;
    // Whether the last round found this bookmark's host not answering.
    // False until a round has asked, and for a bookmark not watched.
    // Main thread.
    virtual bool unreachable(u64 bookmark) const = 0;

    static BookmarkProbe* create(stl::ObjPool& owner, Composer& composer);
};

constexpr int bookmarkProbeIntervalSeconds = 30;

// The host and port an ssh command line connects to, read the way ssh
// reads its arguments: options that take a value skip it, -p gives the
// port, the first word left is the destination - `user@host` or
// `ssh://user@host:port` - and whatever follows it is the remote command.
// The command may run ssh by a path. False when it is not an ssh command
// or names no destination. `port` is left empty when none was given.
bool sshTarget(stl::StringView command, stl::StringBuilder& host, stl::StringBuilder& port);

// Overrides host and port from an ssh_config(5) text for `alias`: the
// HostName and Port of the first Host block naming it exactly. Patterns,
// Match blocks and Include are not followed - a host behind one of those
// is probed under its own name, which answers or not as ssh would find
// it by DNS. Leaves either as it was when there is no such line.
//
// False when ssh would not connect straight to it: a ProxyJump or
// ProxyCommand in a block naming the alias or in `Host *`. A direct
// connection says nothing about a host behind a bastion, so such a
// bookmark is not probed at all rather than called unreachable.
bool sshConfigTarget(stl::StringView config, stl::StringView alias, stl::StringBuilder& host, stl::StringBuilder& port);

// A TCP connection to host:port, given up after timeoutMs. True when one
// of the host's addresses accepted it; a refusal is no answer here -
// there is no ssh behind it either.
bool hostAnswers(const char* host, const char* port, int timeoutMs);
