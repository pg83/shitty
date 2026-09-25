/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "startup.h"

#include "brand.h"

#include <lib/vterm/fatal.h>
#include <lib/vterm/term_features.h>

#include <std/ios/sys.h>

#include <pwd.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace stl;

namespace {
    static bool executable(const char* path) {
        struct stat info{};
        return path != nullptr && stat(path, &info) == 0 && (info.st_mode & S_IXUSR);
    }

    // realpath and the char[PATH_MAX] carriers are the libc boundary here.
    static void copyPath(StringView text, char out[PATH_MAX]) {
        const size_t length = text.length() < PATH_MAX - 1 ? text.length() : PATH_MAX - 1;
        memcpy(out, text.data(), length);
        out[length] = '\0';
    }

    static void resolveShell(const char* path, char out[PATH_MAX]) {
        if (path[0] == '/') {
            copyPath(StringView(path), out);
            return;
        }
        if (path[0] == '.' && realpath(path, out) != nullptr) {
            return;
        }

        const char* pathValue = getenv("PATH");
        if (pathValue != nullptr) {
            StringView remaining(pathValue);
            const StringView name(path);
            Buffer candidate;
            while (!remaining.empty()) {
                StringView part;
                if (!remaining.split(':', part, remaining)) {
                    part = remaining;
                    remaining = StringView();
                }
                if (part.empty()) {
                    continue;
                }
                candidate.reset();
                candidate.append(part.data(), part.length());
                candidate.append("/", 1);
                candidate.append(name.data(), name.length());
                if (realpath(candidate.cStr(), out) != nullptr) {
                    return;
                }
            }
        }

        const char* fallback = getenv("SHELL");
        if (executable(fallback)) {
            copyPath(StringView(fallback), out);
            return;
        }
        const passwd* entry = getpwuid(getuid());
        fallback = entry != nullptr ? entry->pw_shell : nullptr;
        copyPath(StringView(executable(fallback) ? fallback : "/bin/sh"), out);
    }

    static void validateShell(const char* requested, char out[PATH_MAX]) {
        resolveShell(requested, out);
        for (char* permitted = getusershell(); permitted != nullptr; permitted = getusershell()) {
            if (StringView(out) == StringView(permitted)) {
                endusershell();
                setenv("SHELL", out, 1);
                return;
            }
        }
        endusershell();
        unsetenv("SHELL");
    }

    static u32 appendString(Buffer& storage, StringView text) {
        const u32 offset = (u32)(storage.used());
        storage.append(text.data(), text.length());
        storage.append("", 1);
        return offset;
    }
}

const char* LaunchCommand::executable() const {
    return (const char*)(storage.data()) + executableOffset;
}

const char* LaunchCommand::argument(size_t index) const {
    return (const char*)(storage.data()) + offsets[index];
}

LaunchCommand buildLaunchCommand(int argc, char* argv[], StringView defaultShell, bool login) {
    LaunchCommand command;
    if (argc > 2 && StringView(argv[1]) == StringView(u8"-e")) {
        for (int index = 2; index < argc; ++index) {
            command.offsets.pushBack(appendString(command.storage, StringView(argv[index])));
        }
        command.executableOffset = command.offsets[0];
        return command;
    }

    const StringView chosen = argc == 2 ? StringView(argv[1]) : defaultShell;
    if (chosen.empty()) {
        raiseError(StringView(u8"empty shell command"));
    }
    // Every stored option value is NUL terminated in its pool, and argv
    // strings are NUL terminated by definition.
    const char* selected = (const char*)(chosen.data());
    char path[PATH_MAX];
    validateShell(selected, path);
    const StringView resolved(path);
    command.executableOffset = appendString(command.storage, resolved);

    // argv0 is the shell's base name, '-' prefixed for a login shell.
    StringView name = resolved;
    for (StringView directory; name.split('/', directory, name);) {
    }
    const u32 argv0 = (u32)(command.storage.used());
    if (login) {
        command.storage.append("-", 1);
    }
    appendString(command.storage, name);
    command.offsets.pushBack(argv0);
    return command;
}

void configureTerminalChildEnvironment(const Brand& brand, const UnicodeWidths& widths) {
    StringBuilder features;
    appendTermFeatures(features, widths);
    features.append("", 1);
    if (setenv("TERM", "xterm-256color", 1) < 0 || setenv("TERM_FEATURES", (const char*)(features.data()), 1) < 0) {
        raiseError(StringView(u8"cannot configure terminal child environment"));
    }
    brand.configureVersionEnvironment();
}

bool launchedFromDesktop(pid_t parent) {
#if defined(__APPLE__)
    return parent == 1;
#else
    (void)(parent);
    return false;
#endif
}

void enterHomeWhenLaunchedFromDesktop() {
    if (!launchedFromDesktop(getppid())) {
        return;
    }
    char current[PATH_MAX];
    if (getcwd(current, sizeof(current)) == nullptr || StringView(current) != StringView(u8"/")) {
        return;
    }
    const char* home = getenv("HOME");
    if (home == nullptr || home[0] != '/') {
        const passwd* entry = getpwuid(getuid());
        home = entry != nullptr ? entry->pw_dir : nullptr;
    }
    if (home == nullptr || home[0] != '/') {
        return;
    }
    if (chdir(home) < 0) {
        sysO << StringView(u8"Warning: could not enter the home directory.") << endL;
    }
}
