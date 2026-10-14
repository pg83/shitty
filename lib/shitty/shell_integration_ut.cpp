/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "shell_integration.h"

#include <std/ios/fs_utils.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace stl;

namespace {
    // A mkdtemp() directory of this test's own; TMPDIR when the runner gives
    // one, which is inside the tree under ./build.
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/shell_integration_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }

    bool readAll(StringView path, Buffer& out) {
        out.reset();
        Buffer pathBuf{path};
        if (access(pathBuf.cStr(), R_OK) != 0) {
            return false;
        }
        readFileContent(pathBuf, out);
        return true;
    }

    bool contains(StringView text, const char* needle) {
        return memmem(text.data(), text.length(), needle, strlen(needle)) != nullptr;
    }

    // The environment the installer changes, put back as it was.
    struct SavedEnvironment {
        Buffer zdotdir;
        Buffer saved;
        bool hadZdotdir = false;
        bool hadSaved = false;

        SavedEnvironment() {
            const char* const a = getenv("ZDOTDIR");
            const char* const b = getenv("TERMINAL_ZDOTDIR");
            hadZdotdir = a != nullptr;
            hadSaved = b != nullptr;
            if (a != nullptr) {
                zdotdir.append(a, strlen(a) + 1);
            }
            if (b != nullptr) {
                saved.append(b, strlen(b) + 1);
            }
        }

        ~SavedEnvironment() {
            if (hadZdotdir) {
                setenv("ZDOTDIR", (const char*)(zdotdir.data()), 1);
            } else {
                unsetenv("ZDOTDIR");
            }
            if (hadSaved) {
                setenv("TERMINAL_ZDOTDIR", (const char*)(saved.data()), 1);
            } else {
                unsetenv("TERMINAL_ZDOTDIR");
            }
        }
    };
}

STD_TEST_SUITE(ShellIntegration) {
    STD_TEST(OnlyZshIsZsh) {
        STD_INSIST(shellIsZsh(StringView(u8"/bin/zsh")));
        STD_INSIST(shellIsZsh(StringView(u8"zsh")));
        STD_INSIST(shellIsZsh(StringView(u8"-zsh")));
        STD_INSIST(shellIsZsh(StringView(u8"/opt/homebrew/bin/zsh")));
        STD_INSIST(!shellIsZsh(StringView(u8"/bin/bash")));
        STD_INSIST(!shellIsZsh(StringView(u8"/usr/bin/zsh5")));
        STD_INSIST(!shellIsZsh(StringView(u8"/zsh/bin/fish")));
        STD_INSIST(!shellIsZsh(StringView()));
    }

    // The scripts land in the directory, ZDOTDIR points there, and the
    // user's own ZDOTDIR is kept for the shipped .zshenv to put back - or,
    // when there was none, nothing is kept, so it unsets it.
    STD_TEST(TheScriptsAreWrittenAndZdotdirPointsAtThem) {
        const SavedEnvironment saved;
        StringBuilder root;
        makeTempDir(root);
        StringBuilder directory;
        directory << StringView(root) << StringView(u8"/terminal-shell-1/zsh");

        // A parent left open to others by an older run is closed again.
        StringBuilder openParent;
        openParent << StringView(root) << StringView(u8"/terminal-shell-1");
        Buffer openParentPath{StringView(openParent)};
        STD_INSIST(mkdir(openParentPath.cStr(), 0700) == 0 && chmod(openParentPath.cStr(), 0755) == 0);
        struct stat before;
        STD_INSIST(stat(openParentPath.cStr(), &before) == 0 && (before.st_mode & 0777) == 0755);

        setenv("ZDOTDIR", "/home/someone/.config/zsh", 1);
        unsetenv("TERMINAL_ZDOTDIR");
        STD_INSIST(installShellIntegration(StringView(directory)));
        struct stat after;
        STD_INSIST(stat(openParentPath.cStr(), &after) == 0 && (after.st_mode & 0777) == 0700);
        STD_INSIST(StringView(getenv("ZDOTDIR")) == StringView(directory));
        STD_INSIST(getenv("TERMINAL_ZDOTDIR") != nullptr && StringView(getenv("TERMINAL_ZDOTDIR")) == StringView(u8"/home/someone/.config/zsh"));

        Buffer env;
        Buffer integration;
        StringBuilder envPath;
        envPath << StringView(directory) << StringView(u8"/.zshenv");
        StringBuilder integrationPath;
        integrationPath << StringView(directory) << StringView(u8"/integration.zsh");
        STD_INSIST(readAll(StringView(envPath), env));
        STD_INSIST(readAll(StringView(integrationPath), integration));
        // What the shell will read: the .zshenv that restores ZDOTDIR and the
        // integration it sources.
        STD_INSIST(contains(StringView(env), "TERMINAL_ZDOTDIR"));
        STD_INSIST(contains(StringView(integration), "7701"));
        // For this user alone.
        struct stat info;
        Buffer dirPath{StringView(directory)};
        STD_INSIST(stat(dirPath.cStr(), &info) == 0 && (info.st_mode & 0777) == 0700);

        // Again, with no ZDOTDIR of the user's: none is kept.
        unsetenv("ZDOTDIR");
        STD_INSIST(installShellIntegration(StringView(directory)));
        STD_INSIST(getenv("TERMINAL_ZDOTDIR") == nullptr);
        STD_INSIST(StringView(getenv("ZDOTDIR")) == StringView(directory));

        Buffer a{StringView(envPath)};
        Buffer b{StringView(integrationPath)};
        StringBuilder parent;
        parent << StringView(root) << StringView(u8"/terminal-shell-1");
        Buffer c{StringView(parent)};
        Buffer d{StringView(root)};
        STD_INSIST(unlink(a.cStr()) == 0 && unlink(b.cStr()) == 0);
        STD_INSIST(rmdir(dirPath.cStr()) == 0 && rmdir(c.cStr()) == 0 && rmdir(d.cStr()) == 0);
    }

    // A link where the directory would be is not followed, and the
    // environment is left as it was.
    STD_TEST(ALinkInItsPlaceIsRefused) {
        const SavedEnvironment saved;
        StringBuilder root;
        makeTempDir(root);
        StringBuilder target;
        target << StringView(root) << StringView(u8"/elsewhere");
        StringBuilder parent;
        parent << StringView(root) << StringView(u8"/terminal-shell-1");
        Buffer targetPath{StringView(target)};
        Buffer parentPath{StringView(parent)};
        STD_INSIST(mkdir(targetPath.cStr(), 0700) == 0);
        STD_INSIST(symlink(targetPath.cStr(), parentPath.cStr()) == 0);
        StringBuilder directory;
        directory << StringView(parent) << StringView(u8"/zsh");

        setenv("ZDOTDIR", "/kept", 1);
        STD_INSIST(!installShellIntegration(StringView(directory)));
        STD_INSIST(StringView(getenv("ZDOTDIR")) == StringView(u8"/kept"));
        // Nothing written through the link.
        StringBuilder through;
        through << StringView(target) << StringView(u8"/zsh");
        Buffer throughPath{StringView(through)};
        STD_INSIST(access(throughPath.cStr(), F_OK) != 0);

        Buffer rootPath{StringView(root)};
        STD_INSIST(unlink(parentPath.cStr()) == 0 && rmdir(targetPath.cStr()) == 0 && rmdir(rootPath.cStr()) == 0);
    }
}
