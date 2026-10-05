/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

#include <sys/types.h>

namespace stl {
    class StringBuilder;
}

// The environment variable spawnQuickCompanion() sets on the child,
// carrying this process's own pid, right before exec() - and clears
// again in the parent right after fork() returns, so it never leaks
// into the shell this process spawns for itself later. The companion
// reads it once at startup (ext/plt/platform_cocoa.mm's parent-death
// watch) to learn who to watch, then unsets it too, for the same
// reason. Named here so both sides spell it identically; this is not a
// public option and has no entry in optionsTable.
//
// The name carries no brand on purpose. Every brand built out of this
// tree shares this file, and tst/pretty_binary_branding.py fails a
// binary that ships a sibling's name; the prefix is the neutral one
// brand.cpp's GenericBrand already answers with. Only the two
// processes of one launch ever read or write it - parent and the
// companion it forked - so it needs no brand to stay unambiguous, and
// both sides unset it before either spawns a shell.
inline constexpr const char* quickCompanionParentPidEnvName = "TERMINAL_QUICK_COMPANION_PARENT_PID";

// Spawns the quick-terminal companion process: this same binary,
// re-exec'd (via argv0) with -config pointing at quickCompanionRaw's
// target, so one launch of a normal terminal also brings up a quick
// panel in a second, independent process. Owned by application.cpp's
// ApplicationImpl::run(), which also owns killing it again on exit -
// this file only starts it.
//
// Two guards live here rather than in options.cpp, because both need
// the filesystem and argv0, neither of which the option parser has:
//
//  - never spawns when the window being started is itself a quick
//    window (quick == true). The companion's own recommended config
//    imports the main config to inherit its colors and fonts, and that
//    import carries quickCompanion right along with it; without this
//    guard every companion would spawn a companion of its own, forever.
//  - refuses a quickCompanion path that resolves (after ~ expansion and
//    realpath()) to the same file ownConfigPath names - a config
//    pointing at itself would recurse the same way.
//
// Every failure - the option unset, the quick guard, self-reference, a
// path realpath() cannot resolve, fork()/exec() failure - prints one
// line to stderr (except the plain "unset" case, which is the normal,
// silent way to run without a companion) and returns -1; the caller
// keeps the main terminal running regardless in every case, since the
// companion is never critical to it. Only a successful fork() returns a
// live pid.
//
// The child also gets this process's pid through
// quickCompanionParentPidEnvName, so it can outlive an explicit kill()
// from this process's own exit path (application.cpp's close()) by
// noticing the death itself - see ext/plt/platform_cocoa.mm. That is a
// second, independent way the companion stops existing; this function
// does not wait for either one.
pid_t spawnQuickCompanion(stl::StringView quickCompanionRaw, stl::StringView ownConfigPath, bool quick, const char* argv0, stl::StringView identifier);

// The filesystem half of the guard above, split out so it is reachable
// from a plain unit test the way quick_geometry.h's parser is: expands
// a leading "~/" against $HOME, then canonicalizes with realpath().
// Returns false - out left untouched - when the path cannot be resolved
// at all, or when it resolves to the same file as ownConfigPath (already
// expected canonical, or empty when no config file is in use); the two
// cases are told apart by selfReference so the caller can print the
// right diagnostic.
bool resolveQuickCompanionConfig(stl::StringView raw, stl::StringView ownConfigPath, stl::StringBuilder& out, bool& selfReference);
