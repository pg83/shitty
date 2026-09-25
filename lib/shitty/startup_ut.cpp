/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "startup.h"

#include <lib/vterm/unicode_width.h>

#include "brand.h"
#include <std/str/view.h>
#include <std/tst/ut.h>

#include <cstdlib>

using namespace stl;

STD_TEST_SUITE(Startup) {
    STD_TEST(ExplicitCommandBypassesShellResolution) {
        char program[] = "st";
        char option[] = "-e";
        char executable[] = "relative-command";
        char argument[] = "--flag";
        char* arguments[] = {program, option, executable, argument};

        const LaunchCommand command = buildLaunchCommand(4, arguments, nullptr, true);

        STD_INSIST(StringView(command.executable()) == StringView(u8"relative-command"));
        STD_INSIST(command.offsets.length() == 2);
        STD_INSIST(StringView(command.argument(0)) == StringView(u8"relative-command"));
        STD_INSIST(StringView(command.argument(1)) == StringView(u8"--flag"));
    }

    STD_TEST(UsesDefaultShellBasenameAsArgvZero) {
        char program[] = "st";
        char* arguments[] = {program};

        const LaunchCommand command = buildLaunchCommand(1, arguments, "/bin/sh", false);

        STD_INSIST(StringView(command.executable()) == StringView(u8"/bin/sh"));
        STD_INSIST(command.offsets.length() == 1);
        STD_INSIST(StringView(command.argument(0)) == StringView(u8"sh"));
    }

    STD_TEST(PrefixesLoginShellArgvZero) {
        char program[] = "st";
        char* arguments[] = {program};

        const LaunchCommand command = buildLaunchCommand(1, arguments, "/bin/sh", true);

        STD_INSIST(command.offsets.length() == 1);
        STD_INSIST(StringView(command.argument(0)) == StringView(u8"-sh"));
    }

    STD_TEST(CommandLineShellOverridesDefault) {
        char program[] = "st";
        char shell[] = "/bin/sh";
        char* arguments[] = {program, shell};

        const LaunchCommand command = buildLaunchCommand(2, arguments, "/does/not/exist", false);

        STD_INSIST(StringView(command.executable()) == StringView(u8"/bin/sh"));
    }

    STD_TEST(RejectsMissingShell) {
        char program[] = "st";
        char* arguments[] = {program};
        bool threw = false;

        try {
            buildLaunchCommand(1, arguments, nullptr, false);
        } catch (...) {
            threw = true;
        }

        STD_INSIST(threw);
    }

    STD_TEST(ConfiguresChildEnvironment) {
        configureTerminalChildEnvironment(*Brand::generic(), UnicodeWidths(15));

        STD_INSIST(StringView(getenv("TERM")) == StringView("xterm-256color"));
        STD_INSIST(StringView(getenv("TERMINAL_VERSION")) == StringView(SHITTY_VERSION));
        // The width level lands in the reported features verbatim.
        STD_INSIST(StringView(getenv("TERM_FEATURES")) == StringView("T3CwLrMSc7UUw15Ts3BFGsGoSyHNoSxP"));
    }

    STD_TEST(OnlyLaunchdAsParentMeansADesktopLaunch) {
        STD_INSIST(!launchedFromDesktop(2));
        STD_INSIST(!launchedFromDesktop(4711));
#if defined(__APPLE__)
        STD_INSIST(launchedFromDesktop(1));
#else
        STD_INSIST(!launchedFromDesktop(1));
#endif
    }
}
