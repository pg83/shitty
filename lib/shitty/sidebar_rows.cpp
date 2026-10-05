/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "sidebar_rows.h"

#include <std/ios/fs_utils.h>
#include <std/lib/buffer.h>
#include <std/str/view.h>
#include <std/sys/throw.h>

#include <sys/stat.h>
#include <sys/types.h>

using namespace stl;

// What a row shows instead of the raw window title, and the row a click
// lands in. Both are plain functions of plain types, and both are
// non-static and declared again in ui_sidebar_tabs_ut.cpp, for the
// reason F4 hoisted csdTabsChromeAlpha() out of ui_csd_tabs.mm: inside
// drawRect: or an NSEvent handler no headless test can reach them, and
// that is exactly how an inverted decision stayed green through a whole
// suite once already (R4-test, N13).

// Shells set the title to the whole of
// "user@host:~/Projects/github.com/shitty", which in a 220pt column
// truncates to "...ects/github.com/shitty" - the complaint this
// replaces. The last path component is the part that differs between
// tabs, and it is what iTerm2 and Ghostty show too. A title with no
// slash in it is a command line and is left alone; so is one ending in
// a slash, where the component would be empty.
StringView sidebarTabsShortTitle(StringView title) {
    const size_t length = title.length();
    if (length == 0 || title[length - 1] == '/') {
        return title;
    }
    for (size_t at = length; at > 0; --at) {
        if (title[at - 1] == '/') {
            return title.suffix(length - at);
        }
    }
    return title;
}

// The git branch a row shows on its third line, and the two pure halves
// of working it out. Both are non-static and declared again in
// ui_sidebar_tabs_ut.cpp for the reason the one above is: a decision
// reachable only through the filesystem is a decision no test pins down.
//
// False here means "a directory, and no repository above it". That is
// deliberately a different answer from processDirectory()'s false, "no
// directory to be had" - no such process, or one this user may not
// inspect: the row renders the first as "no git" and the second as
// nothing at all, so an empty line can never stand for both at once.

// ".git" is a directory in an ordinary clone and a *file* in a linked
// worktree, holding "gitdir: <path>\n". Returns that path, or an empty
// view when the contents are not a link. Not a hypothetical case: this
// repository has half a dozen linked worktrees open right now, and the
// panel is being built inside one of them.
StringView sidebarTabsGitDirLink(StringView contents) {
    static const StringView marker(u8"gitdir: ");
    if (!contents.startsWith(marker)) {
        return StringView();
    }
    StringView path = contents.suffix(contents.length() - marker.length());
    while (path.length() != 0 && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) {
        path = path.prefix(path.length() - 1);
    }
    return path;
}

// HEAD is "ref: refs/heads/<branch>\n" while a branch is checked out and
// the bare object id when the head is detached. Returns the branch name,
// or the id abbreviated the way git itself abbreviates it, or an empty
// view when the file is neither - a corrupt or half-written HEAD reads
// as "no repository" rather than as a row of garbage.
StringView sidebarTabsHeadBranch(StringView head) {
    while (head.length() != 0 && (head.back() == '\n' || head.back() == '\r' || head.back() == ' ')) {
        head = head.prefix(head.length() - 1);
    }
    static const StringView ref(u8"ref: ");
    if (head.startsWith(ref)) {
        StringView name = head.suffix(head.length() - ref.length());
        // A symbolic HEAD normally points into refs/heads/; anything
        // else is shown as written rather than guessed at.
        static const StringView heads(u8"refs/heads/");
        if (name.startsWith(heads)) {
            name = name.suffix(name.length() - heads.length());
        }
        return name;
    }
    // Seven characters is git's own abbreviation, and a whole object id
    // would not fit the column anyway.
    if (head.length() < 7) {
        return StringView();
    }
    for (size_t at = 0; at < head.length(); ++at) {
        const u8 ch = head[at];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return StringView();
        }
    }
    return head.prefix(7);
}

namespace {
    void appendPath(Buffer& out, StringView directory, StringView leaf) {
        out.reset();
        out.append(directory.data(), directory.length());
        if (directory.length() != 0 && directory.back() != '/') {
            static const StringView slash(u8"/");
            out.append(slash.data(), 1);
        }
        out.append(leaf.data(), leaf.length());
    }

    // Reads a small file if it is there. The house idiom is
    // readFileContent() inside a catch (quick_frame_store.cpp), and that
    // stays - but existence is a stat rather than a caught throw,
    // because walking up from a directory outside any repository would
    // otherwise raise once per level, per tab, per projection.
    bool readSmallFile(Buffer& path, Buffer& out) {
        struct stat info;
        if (::stat(path.cStr(), &info) != 0 || !S_ISREG(info.st_mode)) {
            return false;
        }
        out.reset();
        try {
            readFileContent(path, out);
        } catch (Exception&) {
            return false;
        }
        return true;
    }
}

// Walks up from `directory` to the first .git, resolves a worktree link
// if that is what it turns out to be, and writes the branch into `out`.
// False means "no repository above this directory", which the row shows
// as "no git" - and which the caller must not confuse with "not looked
// yet": a row that has never been resolved shows neither.
bool sidebarTabsBranch(StringView directory, Buffer& out) {
    out.reset();
    // Absolute paths only. A relative one would be resolved against this
    // process's directory, which is not the tab's, and would answer with
    // a branch belonging to somebody else entirely.
    if (directory.length() == 0 || directory[0] != '/') {
        return false;
    }
    Buffer path;
    Buffer contents;
    StringView here = directory;
    for (;;) {
        while (here.length() > 1 && here.back() == '/') {
            here = here.prefix(here.length() - 1);
        }
        static const StringView dotGit(u8".git");
        appendPath(path, here, dotGit);
        struct stat info;
        if (::stat(path.cStr(), &info) == 0) {
            Buffer gitDir;
            if (S_ISDIR(info.st_mode)) {
                gitDir.append(path.data(), path.used());
            } else if (readSmallFile(path, contents)) {
                const StringView link = sidebarTabsGitDirLink(StringView(contents));
                if (link.length() == 0) {
                    return false;
                }
                if (link[0] == '/') {
                    gitDir.append(link.data(), link.length());
                } else {
                    // Submodules write the link relative to the
                    // directory holding it; worktrees write it absolute.
                    appendPath(gitDir, here, link);
                }
            } else {
                return false;
            }
            static const StringView head(u8"HEAD");
            appendPath(path, StringView(gitDir), head);
            if (!readSmallFile(path, contents)) {
                return false;
            }
            const StringView branch = sidebarTabsHeadBranch(StringView(contents));
            if (branch.length() == 0) {
                return false;
            }
            out.append(branch.data(), branch.length());
            return true;
        }
        if (here.length() <= 1) {
            return false;
        }
        size_t cut = here.length();
        while (cut > 1 && here[cut - 1] != '/') {
            --cut;
        }
        here = here.prefix(cut == 1 ? 1 : cut - 1);
    }
}

// One of a row's two lines, measured down from the row's own top edge:
// 0 is what is running, 1 where - the folder, then the git branch. Drawing and the
// row height come out of the same arithmetic, which is what stops a row
// being too short for its own contents - the defect a written-down height
// invites the moment a line's size changes.
double sidebarTabsLineTop(size_t line) {
    return SidebarMetrics::rowPad + (line == 0 ? 0 : SidebarMetrics::titleLine + SidebarMetrics::subLine * (double)(line - 1));
}

double sidebarTabsLineHeight(size_t line) {
    return line == 0 ? SidebarMetrics::titleLine : SidebarMetrics::subLine;
}

double sidebarTabsRowHeight() {
    return SidebarMetrics::rowHeight;
}

double sidebarTabsListTop() {
    return SidebarMetrics::listTop;
}

// Where a row's line starts. Line 0 is the title and sits flush; the
// folder line starts past the folder icon's column, and at the same place
// on every row whether or not an icon is drawn in it.
double sidebarTabsLineLeft(size_t line, double textLeft, bool iconsAvailable) {
    if (line == 0) {
        return textLeft;
    }
    return textLeft + (iconsAvailable ? SidebarMetrics::iconColumn : 0);
}

// The row an offset down from the panel's top edge falls in: an index
// into the list, `count` for the new-tab row under it, or -1 for panel
// that answers nothing. One function, so drawing and clicking can never
// disagree about where a row is - including at the bottom edge, where a
// row that does not fit whole is drawn nowhere and so answers nothing
// either.
// How far down from the list's top a row starts: the heights of the rows
// above it. Rows past `count` - the "+" row - are the ordinary height, and
// so is every row when `heights` is null.
double sidebarTabsRowOffset(const double* heights, size_t count, size_t at) {
    double offset = 0;
    for (size_t row = 0; row < at; ++row) {
        offset += heights != nullptr && row < count ? heights[row] : SidebarMetrics::rowHeight;
    }
    return offset;
}

// The row an offset down from the panel's top edge falls in: an index
// into the list, `count` for the new-tab row under it, or -1 for panel
// that answers nothing. One function, so drawing and clicking can never
// disagree about where a row is - including at the bottom edge, where a
// row that does not fit whole is drawn nowhere and so answers nothing
// either. Rows are as tall as `heights` says (a folder's label is short);
// null is every row the ordinary height.
long long sidebarTabsRowAtHeights(double panelHeight, double offsetFromTop, const double* heights, size_t count, double topInset) {
    // C10: `topInset` is how far down the list starts, which is no
    // longer the top of the panel. The panel now runs the whole height
    // of the window and the title bar is drawn over its top; the rows
    // must not be, or the first one sits under the window buttons where
    // it cannot be read and can barely be clicked.
    //
    // It arrives here rather than being applied by each caller because
    // that is the whole reason this function exists: drawing and
    // clicking share it so they cannot disagree about where a row is,
    // and two call sites each subtracting their own inset is exactly how
    // they would start to.
    const double listTop = topInset + SidebarMetrics::listTop;
    if (offsetFromTop < listTop) {
        return -1;
    }
    double top = listTop;
    for (size_t row = 0; row <= count; ++row) {
        const double height = heights != nullptr && row < count ? heights[row] : SidebarMetrics::rowHeight;
        if (offsetFromTop < top + height) {
            return top + height <= panelHeight ? (long long)(row) : -1;
        }
        top += height;
    }
    return -1;
}

long long sidebarTabsRowAt(double panelHeight, double offsetFromTop, size_t count, double topInset) {
    return sidebarTabsRowAtHeights(panelHeight, offsetFromTop, nullptr, count, topInset);
}

// Whether an offset in from the panel's leading edge is on a row's pin,
// which stands in the number gutter while the pointer is over the row.
// One function for the drawing and the click, like sidebarTabsRowAt().
bool sidebarTabsStartsSection(bool label, bool inFolder, bool previousInFolder, bool afterBookmarks) {
    return !label && !inFolder && (previousInFolder || afterBookmarks);
}

double sidebarTabsIndent(bool inFolder) {
    return inFolder ? SidebarMetrics::folderIndent : 0;
}

// The gutter moves with the row: a folder's row has its pin where its
// indented pill starts, not at the list's edge.
bool sidebarTabsPinAt(double offsetFromLeft, double indent) {
    const double from = offsetFromLeft - indent;
    return from >= SidebarMetrics::pillInset && from < SidebarMetrics::textInset + SidebarMetrics::numberGutter;
}
