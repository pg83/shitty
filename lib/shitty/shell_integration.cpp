/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "shell_integration.h"

#include "shell_integration_data.h"

#include <std/lib/buffer.h>
#include <std/str/builder.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace stl;

namespace {
    // One directory level, made for this user alone or found to be one:
    // a real directory, owned by us, that nobody else can write into.
    bool ownDirectory(const char* path) {
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            return false;
        }
        struct stat info;
        if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid()) {
            return false;
        }
        if ((info.st_mode & 0077) != 0 && chmod(path, 0700) != 0) {
            return false;
        }
        return true;
    }

    // The whole file, through a temporary and a rename, so a shell starting
    // at the same moment reads either the old script or the new one.
    bool writeScript(StringView directory, StringView name, const EmbeddedFontData& data) {
        StringBuilder path;
        path << directory << StringView(u8"/") << name;
        StringBuilder temporary;
        temporary << StringView(path) << StringView(u8".tmp");
        Buffer finalPath{StringView(path)};
        Buffer temporaryPath{StringView(temporary)};
        const int fd = open(temporaryPath.cStr(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            return false;
        }
        size_t written = 0;
        while (written < data.size) {
            const ssize_t step = write(fd, data.data + written, data.size - written);
            if (step < 0 && errno == EINTR) {
                continue;
            }
            if (step <= 0) {
                close(fd);
                unlink(temporaryPath.cStr());
                return false;
            }
            written += (size_t)(step);
        }
        if (close(fd) != 0 || rename(temporaryPath.cStr(), finalPath.cStr()) != 0) {
            unlink(temporaryPath.cStr());
            return false;
        }
        return true;
    }
}

bool shellIsZsh(StringView shell) {
    const u8* const data = (const u8*)(shell.data());
    size_t base = shell.length();
    while (base > 0 && data[base - 1] != '/') {
        --base;
    }
    StringView name((const u8*)(data + base), shell.length() - base);
    if (name.length() > 0 && data[base] == '-') {
        name = StringView((const u8*)(data + base + 1), name.length() - 1);
    }
    return name == StringView(u8"zsh");
}

void shellIntegrationDirectory(StringView identifier, StringBuilder& out) {
    out.reset();
    const char* const temporary = getenv("TMPDIR");
    StringView root(temporary != nullptr && temporary[0] != '\0' ? temporary : "/tmp");
    while (root.length() > 1 && ((const u8*)(root.data()))[root.length() - 1] == '/') {
        root = StringView((const u8*)(root.data()), root.length() - 1);
    }
    char uid[24];
    const int length = snprintf(uid, sizeof(uid), "%u", (unsigned)(getuid()));
    out << root << StringView(u8"/") << identifier << StringView(u8"-shell-") << StringView((const u8*)(uid), length > 0 ? (size_t)(length) : 0) << StringView(u8"/zsh");
}

bool installShellIntegration(StringView directory) {
    if (directory.empty()) {
        return false;
    }
    // Every level from the one past the root down: the last two are ours,
    // and the check on each of them is what keeps another user's directory
    // - or a link planted where ours would be - from getting our scripts.
    Buffer path{directory};
    char* const text = (char*)(path.cStr());
    char* const parent = strrchr(text, '/');
    if (parent == nullptr || parent == text) {
        return false;
    }
    *parent = '\0';
    const bool parentOk = ownDirectory(text);
    *parent = '/';
    if (!parentOk || !ownDirectory(text)) {
        return false;
    }
    if (!writeScript(directory, StringView(u8".zshenv"), zshIntegrationEnv) || !writeScript(directory, StringView(u8"integration.zsh"), zshIntegration)) {
        return false;
    }
    const char* const user = getenv("ZDOTDIR");
    if (user != nullptr) {
        if (setenv("TERMINAL_ZDOTDIR", user, 1) != 0) {
            return false;
        }
    } else {
        unsetenv("TERMINAL_ZDOTDIR");
    }
    if (setenv("ZDOTDIR", text, 1) != 0) {
        if (user == nullptr) {
            unsetenv("TERMINAL_ZDOTDIR");
        }
        return false;
    }
    return true;
}
