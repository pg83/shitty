/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/vector.h>
#include <std/str/view.h>

namespace stl {
    class ObjPool;
}

// The hosts the command palette offers to connect to: the names in the
// user's ssh config, and the nodes Teleport knows.

struct SshHost {
    // What `ssh <alias>` takes: a Host line's name.
    stl::StringView alias;
    // What it resolves to, when the config says; empty when it does not.
    stl::StringView hostName;
    stl::StringView user;
    stl::StringView port;
};

// The concrete names in one ssh config text, in order and once each: a
// pattern (* ? or a ! negation) is a rule, not a host, and is left out.
// The first value of HostName, User and Port in a name's blocks wins, as
// ssh has it. `Include` lines are handed to `includes`, when given, with
// the file names they glob to resolved: relative ones are against
// `sshDirectory`. Strings are interned in `pool`.
struct SshIncludes {
    virtual void include(stl::StringView pattern) = 0;
};
void sshConfigHosts(stl::StringView text, stl::ObjPool& pool, stl::Vector<SshHost>& out, SshIncludes* includes = nullptr);

// ~/.ssh/config and what it includes, to a depth of 8.
void sshConfigHostsFromHome(stl::StringView home, stl::ObjPool& pool, stl::Vector<SshHost>& out);

struct TeleportHost {
    stl::StringView hostname;
    stl::StringView address;
    // "key=value" pairs joined with " · ", for the detail pane.
    stl::StringView labels;
};

// The nodes in what `tsh ls --format=json` printed: an array of objects
// whose spec carries hostname and addr and whose metadata carries labels.
// Anything else - a broken line, an error text - gives no hosts.
void teleportHosts(stl::StringView json, stl::ObjPool& pool, stl::Vector<TeleportHost>& out);
