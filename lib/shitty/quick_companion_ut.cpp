/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_companion.h"

#include "quick_frame_store.h"

#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

using namespace stl;

namespace {

    // Mirrors options_ut.cpp's writeTempConfig(): an mkstemp() file this
    // process actually owns, so realpath() has something real to
    // canonicalize. Content is irrelevant here - only the path matters -
    // so the file is left empty.
    void makeTempFile(StringBuilder& path) {
        const char* const directory = getenv("TMPDIR");
        path << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/quick_companion_ut.XXXXXX");
        const int fd = mkstemp(path.cStr());
        STD_INSIST(fd >= 0);
        close(fd);
    }

    // A directory this process owns, plus a named empty file inside it -
    // for the one test that cares what the config is *called*, which
    // makeTempFile()'s random mkstemp() suffix cannot express.
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/quick_companion_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }

    void makeNamedFile(StringView dir, StringView name, StringBuilder& path) {
        path << dir << StringView(u8"/") << name;
        const int fd = ::open(path.cStr(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        STD_INSIST(fd >= 0);
        close(fd);
    }

    // Rewrites ".../name" to "..././name": the same file under a
    // different, non-canonical spelling, the way "~/x" and its expanded
    // $HOME form are the same file under different spellings too.
    void withExtraDot(StringView original, StringBuilder& out) {
        size_t lastSlash = 0;
        for (size_t at = 0; at < original.length(); ++at) {
            if (original[at] == '/') {
                lastSlash = at;
            }
        }
        out << StringView(original.data(), lastSlash + 1) << StringView(u8"./") << StringView(original.data() + lastSlash + 1, original.length() - lastSlash - 1);
    }

}

STD_TEST_SUITE(QuickCompanion) {
    // mkstemp()'s own path is not necessarily its own canonical form -
    // TMPDIR routes through /var on macOS, itself a symlink to /private -
    // so the expected value is realpath()'d exactly like resolved is,
    // rather than compared against the raw mkstemp() text.
    STD_TEST(ExistingFileResolvesWithoutSelfReference) {
        StringBuilder file;
        makeTempFile(file);
        char expected[PATH_MAX];
        STD_INSIST(realpath(file.cStr(), expected) != nullptr);

        StringBuilder resolved;
        bool selfReference = true;
        STD_INSIST(resolveQuickCompanionConfig(StringView(file), StringView(), resolved, selfReference));
        STD_INSIST(!selfReference);
        STD_INSIST(StringView(resolved) == StringView(expected));

        unlink(file.cStr());
    }

    STD_TEST(MissingFileIsRejectedAndOutUntouched) {
        StringBuilder resolved;
        resolved << StringView(u8"untouched");
        bool selfReference = true;

        STD_INSIST(!resolveQuickCompanionConfig(StringView(u8"/nonexistent/quick_companion_ut_missing"), StringView(), resolved, selfReference));
        STD_INSIST(!selfReference);
        STD_INSIST(StringView(resolved) == StringView(u8"untouched"));
    }

    // The canonical-path comparison, not a string comparison: the raw
    // option text and ownConfigPath name the same file through two
    // different, non-normalized spellings.
    STD_TEST(SelfReferenceDetectedThroughDifferentSpelling) {
        StringBuilder file;
        makeTempFile(file);
        StringBuilder spelledDifferently;
        withExtraDot(StringView(file), spelledDifferently);

        StringBuilder resolved;
        bool selfReference = false;
        STD_INSIST(!resolveQuickCompanionConfig(StringView(spelledDifferently), StringView(file), resolved, selfReference));
        STD_INSIST(selfReference);
        STD_INSIST(resolved.used() == 0);

        unlink(file.cStr());
    }

    STD_TEST(DifferentFilesAreNotFlaggedAsSelfReference) {
        StringBuilder first;
        makeTempFile(first);
        StringBuilder second;
        makeTempFile(second);

        StringBuilder resolved;
        bool selfReference = true;
        STD_INSIST(resolveQuickCompanionConfig(StringView(first), StringView(second), resolved, selfReference));
        STD_INSIST(!selfReference);

        unlink(first.cStr());
        unlink(second.cStr());
    }

    STD_TEST(EmptyOwnConfigPathNeverSelfReferences) {
        StringBuilder file;
        makeTempFile(file);

        StringBuilder resolved;
        bool selfReference = true;
        STD_INSIST(resolveQuickCompanionConfig(StringView(file), StringView(), resolved, selfReference));
        STD_INSIST(!selfReference);

        unlink(file.cStr());
    }

    // Where this module meets quick_frame_store: quickRememberFrame
    // derives its state file from the config the process actually
    // resolved (defaultQuickFramePath), and a companion is this same
    // binary re-exec'd with a *different* -config. Two live processes
    // therefore keep two separate frames, and neither can overwrite the
    // other's - which is what makes "the companion remembers its own
    // placement" true rather than a race between two writers of one
    // file. The guarantee is not a convention either side has to
    // remember: the self-reference guard rejects the one configuration
    // that would collapse the two paths into one, so this pins both
    // halves together.
    //
    // The two configs are named, not mkstemp()'d, and that is the point:
    // defaultQuickFramePath() replaces the extension, so two files whose
    // names differ only after the last dot - which is exactly what two
    // random mkstemp() suffixes are - would resolve to one and the same
    // state file. The arrangement here is the documented one instead
    // (shitty.toml alongside quick.toml, bin/st/shitty.toml).
    STD_TEST(CompanionKeepsAFrameStoreSeparateFromItsParent) {
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder own;
        makeNamedFile(StringView(dir), StringView(u8"shitty.toml"), own);
        StringBuilder companion;
        makeNamedFile(StringView(dir), StringView(u8"quick.toml"), companion);

        StringBuilder resolved;
        bool selfReference = true;
        STD_INSIST(resolveQuickCompanionConfig(StringView(companion), StringView(own), resolved, selfReference));
        STD_INSIST(!selfReference);

        // Both sides canonical before they are compared: resolved came
        // back from realpath(), and TMPDIR reaches the same directory
        // through /var on macOS and /private/var underneath it. Comparing
        // one spelling against the other would make these two paths
        // differ for a reason that has nothing to do with the file names
        // - and leave the test green even if the state file stopped
        // depending on the config name at all.
        char ownCanonical[PATH_MAX];
        STD_INSIST(realpath(own.cStr(), ownCanonical) != nullptr);
        StringBuilder ownFrame;
        StringBuilder companionFrame;
        STD_INSIST(defaultQuickFramePath(StringView(ownCanonical), ownFrame));
        STD_INSIST(defaultQuickFramePath(StringView(resolved), companionFrame));
        STD_INSIST(StringView(ownFrame) != StringView(companionFrame));

        // And the configuration that would have given them one shared
        // file is refused outright, rather than left to chance.
        StringBuilder shared;
        bool sharedSelfReference = false;
        STD_INSIST(!resolveQuickCompanionConfig(StringView(own), StringView(own), shared, sharedSelfReference));
        STD_INSIST(sharedSelfReference);

        unlink(own.cStr());
        unlink(companion.cStr());
        rmdir(dir.cStr());
    }
}
