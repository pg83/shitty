/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/vector.h>
#include <std/str/view.h>
#include <std/sys/types.h>

#include <stddef.h>

struct SessionSet;
struct BookmarkShelf;
struct Bookmark;

namespace stl {
    class ObjPool;
}

// Split groups: the rows a tab list draws, one per pane rather than one
// per tab. A tab holding one pane is one plain row, exactly the row the
// list always drew; a split tab is a group, its panes' rows in visual
// order with the first and the last marked so the list can draw one frame
// round the whole run.
//
// Portable and free of AppKit on purpose: which row is which, and which
// of them is selected, is the part of the sidebar a headless test can
// reach. The strings a row shows stay the sidebar's own - they need the
// process's directory and the branch above it - and are asked by the row's
// pane.
struct TabRow {
    size_t tab = 0;
    u64 pane = 0;
    // The row belongs to a tab of two panes or more.
    bool grouped = false;
    bool groupFirst = false;
    bool groupLast = false;
    // The tab's focused pane - the row a tab's number and the selection
    // belong to.
    bool focused = false;
    // The tab is the window's active one.
    bool activeTab = false;
    // Where the pane sits in its tab, as fractions of the tab's box, 0..1
    // on each axis: the cell this row is in the group's map of its split.
    // The whole box for a tab of one pane.
    float left = 0;
    float top = 0;
    float width = 1;
    float height = 1;
    // Bookmarks (bookmarks.h): the bookmark this row's tab was opened
    // from, or 0. With `closed` the row stands for a bookmark that has no
    // tab at all, and `tab` and `pane` mean nothing - a click opens it.
    u64 bookmark = 0;
    bool closed = false;
    // The pane's child has exited and the pane was kept
    // (SessionSet::paneExited).
    bool exited = false;
    // The first loose ordinary row after the bookmarks and folders, where
    // the list draws the line between them; never set when nothing comes
    // before it.
    bool afterBookmarks = false;
    // Folders: a label row stands for a folder and no tab - `folder` names
    // it, `members` counts its bookmarks and tabs, `collapsed` says it is
    // shut, and `activeInside` that the tab in front is one of them. On
    // every other row `folder` is the folder the row is in, empty for none.
    bool label = false;
    stl::StringView folder;
    size_t members = 0;
    bool collapsed = false;
    bool activeInside = false;
};

// Every tab's rows, tabs in order; replaces what `out` held.
//
// With a shelf, the bookmarks come first, in the shelf's order: each
// either as the rows of the tab opened from it or, when it has none, as
// one closed row. The ordinary tabs follow. Bookmark tabs are the front of
// the tab model in the same order (SessionSet::openBookmark), so the rows
// still run in tab order - the closed ones fall in between.
void tabRows(const SessionSet& sessions, const BookmarkShelf* shelf, stl::Vector<TabRow>& out);
// The same with folders shut: the folders named in `collapsed` list their
// label and, when the tab in front is inside, that tab's rows - nothing else.
//
// The order, top to bottom, is the tab model's: the loose bookmarks, each
// folder (label, bookmarks, tabs) in SessionSet::folders() order, then the
// loose tabs.
void tabRows(const SessionSet& sessions, const BookmarkShelf* shelf, const stl::Vector<stl::StringView>& collapsed, stl::Vector<TabRow>& out);

// Pinning a tab: the bookmark that would open it again, from its focused
// pane. The directory is the pane's shell's; the command is what runs in
// the foreground when that is not the shell itself - `ssh prod` pinned
// mid-session is a bookmark for `ssh prod` - typed back as a shell would
// need it. The title is the command, else the directory's last name,
// else `fallback`. Strings interned in `pool`; the id is left 0.
void tabBookmarkDraft(const SessionSet& sessions, size_t tab, stl::StringView fallback, stl::ObjPool& pool, Bookmark& out);
