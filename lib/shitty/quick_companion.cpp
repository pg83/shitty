/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_companion.h"

#include <std/ios/sys.h>
#include <std/str/builder.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace stl;

namespace {

    // Expands a leading "~/" against $HOME. Not the resolveImportPath()
    // already in options.cpp: that helper also resolves a plain relative
    // path against the importing file, which does not apply here - a
    // relative quickCompanion path is simply relative to this process's
    // own working directory, which realpath() resolves on its own - and
    // it is static to options.cpp's translation unit.
    static void expandTilde(StringView raw, StringBuilder& out) {
        if (raw.length() >= 2 && raw[0] == '~' && raw[1] == '/') {
            const char* const home = getenv("HOME");
            if (home != nullptr && home[0] != '\0') {
                out << StringView(home) << StringView(raw.data() + 1, raw.length() - 1);
                return;
            }
        }
        out << raw;
    }

}

bool resolveQuickCompanionConfig(StringView raw, StringView ownConfigPath, StringBuilder& out, bool& selfReference) {
    selfReference = false;

    StringBuilder expanded;
    expandTilde(raw, expanded);

    char resolved[PATH_MAX];
    if (realpath(expanded.cStr(), resolved) == nullptr) {
        return false;
    }

    if (!ownConfigPath.empty()) {
        // ownConfigPath names the file OptionsParser::loadConfigFile()
        // actually read for this process; canonicalize it too rather
        // than trusting it is already realpath-shaped, since a relative
        // -config argument on the command line would not be.
        char ownResolved[PATH_MAX];
        if (realpath((const char*)(ownConfigPath.data()), ownResolved) != nullptr && StringView(ownResolved) == StringView(resolved)) {
            selfReference = true;
            return false;
        }
    }

    out << StringView(resolved);
    return true;
}

pid_t spawnQuickCompanion(StringView quickCompanionRaw, StringView ownConfigPath, bool quick, const char* argv0, StringView identifier) {
    if (quickCompanionRaw.empty()) {
        return -1;
    }
    if (quick) {
        // The recursion guard the header documents: a quick window never
        // spawns one of its own.
        return -1;
    }

    StringBuilder resolvedPath;
    bool selfReference = false;
    if (!resolveQuickCompanionConfig(quickCompanionRaw, ownConfigPath, resolvedPath, selfReference)) {
        sysE << identifier << StringView(u8": quickCompanion: ");
        if (selfReference) {
            sysE << StringView(u8"refers to this process's own config file; not spawning a companion");
        } else {
            sysE << StringView(u8"cannot resolve ") << quickCompanionRaw << StringView(u8"; not spawning a companion");
        }
        sysE << endL;
        return -1;
    }

    // Carries this process's own pid to the child through the
    // environment, for platform_cocoa.mm's parent-death watch. Set right
    // before fork() and cleared right after in this (the parent) branch,
    // so the exposure is only the fork() call itself - no other thread
    // in this process touches getenv/setenv, and nothing here is meant
    // to persist once the child has its own copy of the environment.
    char parentPidText[32];
    snprintf(parentPidText, sizeof(parentPidText), "%d", (int)(getpid()));
    setenv(quickCompanionParentPidEnvName, parentPidText, 1);

    const pid_t pid = fork();
    if (pid < 0) {
        unsetenv(quickCompanionParentPidEnvName);
        sysE << identifier << StringView(u8": quickCompanion: fork() failed: ") << StringView(strerror(errno)) << endL;
        return -1;
    }
    if (pid == 0) {
        char configFlag[] = "-config";
        char* const childArgv[] = {
            const_cast<char*>(argv0),
            configFlag,
            resolvedPath.cStr(),
            nullptr,
        };
        execvp(argv0, childArgv);
        // The child's stdio is whatever this process inherited; that is
        // enough for a diagnostic here, unlike pty.cpp's spawned shells,
        // which redirect all three descriptors onto a pty first.
        static constexpr char message[] = "quickCompanion: execvp() failed\n";
        (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(127);
    }
    unsetenv(quickCompanionParentPidEnvName);
    return pid;
}
