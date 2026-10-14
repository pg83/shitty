/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/buffer.h>
#include <std/str/view.h>
#include <std/lib/vector.h>
#include <std/sys/types.h>

#include <sys/types.h>

struct Brand;
class UnicodeWidths;

// The exec image: NUL-terminated strings appended back to back in
// storage, addressed by offsets so the structure stays valid across
// moves. offsets lists argv in order; executableOffset names the path
// to exec.
struct LaunchCommand {
    stl::Buffer storage;
    stl::Vector<u32> offsets;
    u32 executableOffset = 0;

    const char* executable() const;
    const char* argument(size_t index) const;

    // Where the first session's child starts, resolved once at startup
    // by launchDirectory() below; empty means "wherever this process
    // is", with no chdir at all. Later sessions start where the active
    // one's foreground process is and come back here only when that
    // cannot be read (SessionSet::openSession()).
    stl::Buffer directory;
};

LaunchCommand buildLaunchCommand(int argc, char* argv[], stl::StringView defaultShell, bool login);

// The -directory rule, kept pure so a test can hand it any launcher
// directory without moving the test process anywhere. option is the
// -directory value, inherited this process's own working directory,
// home the user's; out is left empty when the child should simply
// inherit.
//
// A set option wins, with `~` and a leading `~/` standing for home. An
// unset option inherits - `st -e vim file` from a shell opens the file
// where the shell was - except when what was inherited is `/`: that is
// what launchd hands a bundled app on macOS, nobody keeps their work in
// the root directory, and a terminal that opens there is a terminal
// that opens in the wrong place. kitty makes the same call on macOS.
void launchDirectory(stl::StringView option, stl::StringView inherited, stl::StringView home, stl::Buffer& out);

// $HOME, or the passwd entry's directory when the environment has none.
// Empty when neither knows.
void homeDirectory(stl::Buffer& out);

void configureTerminalChildEnvironment(const Brand& brand, const UnicodeWidths& widths);

// True when launchd itself started this process - a Finder, Dock or open(1)
// launch of the .app on macOS - so no shell handed it an environment.
// parent is the parent process id; always false outside macOS.
bool launchedFromDesktop(pid_t parent);

// A desktop launch starts in "/", which is no place for a shell to begin.
// desktopLaunch is the caller's captured launchedFromDesktop() answer;
// false makes this a no-op.
void enterHomeWhenLaunchedFromDesktop(bool desktopLaunch);
