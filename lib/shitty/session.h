/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "pane_layout.h"

#include <std/lib/vector.h>
#include <std/str/view.h>

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

struct Composer;
struct Vterm;
struct Bookmark;

// A4/A5: one pane of the active tab - the terminal it holds, where it
// sits in the window's content box, and whether it is the one taking
// input. Every pane handed out here is visible; exactly one of them has
// `focused` set.
struct SessionPane {
    Vterm* terminal = nullptr;
    PixelRect area;
    u64 id = 0;
    bool focused = false;
};

// The terminals behind one window, arranged as tabs of panes (A4), and
// which of them the window shows.
//
// The tab half of this interface is unchanged from when a tab was a
// single terminal: an index still names a tab, count() still counts
// tabs, and title() still answers with what the user reads on one. What
// moved underneath is the meaning of "the tab's terminal" - it is now
// the focused pane of that tab's pane tree.
struct SessionSet {
    // A5: the focused pane's terminal - the one authoritative answer to
    // "which terminal is the window's". Session creation, selection and
    // death are driven by the tab actions registered at create().
    virtual Vterm* activeTerminal() const = 0;
    // The tab model a window chrome projects: the live tabs in visual
    // order. Every model mutation and every title change commits its
    // state first and then notifies
    // composer.sessionsChangedListeners.
    virtual size_t count() const = 0;
    virtual size_t activeIndex() const = 0;
    // The tab's focused pane's last published title; empty until its
    // shell set one.
    virtual stl::StringView title(size_t index) const = 0;
    // The shell process behind the same pane title() labels the tab by,
    // or -1 when the tab names none. Chrome that wants to say what a tab
    // is working on - its directory, and the branch above it - asks the
    // process rather than the shell's cooperation: OSC 7 is only ever
    // installed by Apple's own zshrc, under Apple's own terminal, so it
    // never reaches this one.
    virtual pid_t pid(size_t index) const = 0;
    virtual void activate(size_t index) = 0;

    // Split groups: every pane of every tab, for chrome that lists panes
    // and not only tabs (the sidebar shows a split tab as a group of
    // rows). A pane is named by its id, which is stable for its life and
    // unique across tabs; title() and pid() above are these asked of the
    // tab's focused pane.
    //
    // The ids of one tab's panes in visual order, near before far; empty
    // for an index that names no tab.
    virtual void panes(size_t tab, stl::Vector<u64>& out) const = 0;
    // The pane the tab's input goes to, or 0 for an index that names no
    // tab.
    virtual u64 focusedPane(size_t tab) const = 0;
    // One pane's last published title (empty until its shell set one),
    // and its shell's pid (-1 when there is none), whichever tab it is in.
    virtual stl::StringView paneTitle(u64 pane) const = 0;
    virtual pid_t panePid(u64 pane) const = 0;
    // Brings the pane's tab forward and gives the pane the focus, in one
    // commit and one notification; nothing for an id no tab holds.
    virtual void activatePane(u64 pane) = 0;
    // One tab's panes laid out in `box` the way the tab lays them out in
    // the window, shares included, with no seam between them - a
    // picture of the split's shape for chrome to draw small (the
    // sidebar's group map). Empty for an index that names no tab.
    virtual void paneLayout(size_t tab, const PixelRect& box, stl::Vector<PanePlacement>& out) const = 0;
    // Opens a tab holding one pane.
    virtual void newSession() = 0;
    // Bookmarks (bookmarks.h). The bookmark a tab was opened from, or 0
    // for an ordinary tab or an index that names none.
    virtual u64 tabBookmark(size_t tab) const = 0;
    // Brings forward the tab opened from this bookmark, or opens one: a
    // tab of one pane running the bookmark's command in its directory.
    // Bookmark tabs are kept at the front, in the order of the bookmarks
    // on the shelf (Composer::bookmarks), so they take the first cmd+1..9
    // and the sidebar lists them above the ordinary tabs in the same
    // order the tab model has them.
    virtual void openBookmark(const Bookmark& bookmark) = 0;
    // A new ordinary tab running `command` with the shell (empty: the
    // shell itself), in `directory` (empty: where a new tab would start;
    // `~` is home), named `title` when one is given - the command
    // palette's way to open a host, a folder or an app.
    virtual void openCommand(stl::StringView command, stl::StringView directory, stl::StringView title) = 0;
    // Pinning: the tab becomes the one opened from this bookmark and
    // moves to its place among the bookmark tabs; with 0 it is an
    // ordinary tab again and moves to just behind them. Either way it
    // stays open and stays in front if it was.
    virtual void adoptBookmark(size_t tab, u64 bookmark) = 0;
    // Folders: the sidebar's groups of tabs. A bookmark tab's folder is its
    // bookmark's, saved in bookmarks.toml; an ordinary tab's lives as long
    // as the window. Tabs are kept in the sidebar's order - the loose
    // bookmarks, then each folder (its bookmarks, then its tabs), then the
    // loose tabs - so cmd+1..9 count them as the list shows them.
    //
    // The tab's folder, empty for none.
    virtual stl::StringView tabFolder(size_t tab) const = 0;
    // The name the tab was given: a bookmark tab's is its bookmark's title
    // (renamed in bookmarks.toml), an ordinary tab's lasts as long as the
    // window. Empty for none - title() is then the focused pane's.
    virtual stl::StringView tabTitle(size_t tab) const = 0;
    // Names an ordinary tab; empty takes the name away. A bookmark tab is
    // renamed in its file (setBookmarkTitle(), bookmarks.h).
    virtual void setTabTitle(size_t tab, stl::StringView name) = 0;
    // Every folder, in the sidebar's order (folderOrder(), bookmarks.h).
    virtual void folders(stl::Vector<stl::StringView>& out) const = 0;
    // A new empty folder of this window's; nothing for a name already
    // there.
    virtual void addFolder(stl::StringView folder) = 0;
    // Renames a folder of this window's and moves its tabs with it. The
    // bookmarks naming it are the file's business (setBookmarkFolder()).
    virtual void renameFolder(stl::StringView from, stl::StringView to) = 0;
    // Deletes a folder of this window's: its tabs stay open, out of any
    // folder, in their order and at the head of the loose tabs. The bookmarks naming it are the
    // file's business (deleteFolderInFile()); closing its tabs instead is
    // close()'s, one tab at a time, before this.
    virtual void removeFolder(stl::StringView folder) = 0;
    // A tab dropped in the sidebar: into `folder` (empty: out of any), and
    // before the tab at index `before` when that one sorts with it, else at
    // the end of its folder. A bookmark tab keeps its bookmark's folder -
    // moving that is a change to the file first, then resort().
    virtual void dropTab(size_t tab, stl::StringView folder, size_t before) = 0;
    // Puts the tabs back in the sidebar's order after the shelf changed
    // under them (a bookmark moved between folders, pinned, unpinned).
    virtual void resort() = 0;
    // The foreground process group of the pane's pty - what is running in
    // it now, which is the shell itself when nothing else is - or 0.
    virtual pid_t paneForeground(u64 pane) const = 0;
    // A bookmark tab outlives its process: when the child of its last pane
    // exits, the pane stays on screen with what it last showed, marked
    // exited, instead of closing the tab. Whether this pane is one.
    virtual bool paneExited(u64 pane) const = 0;
    // Runs an exited pane's bookmark again in the same pane - its command
    // when the tab still names a bookmark on the shelf, the shell when it
    // was unpinned meanwhile. False when the pane is not exited.
    virtual bool reconnect(u64 pane) = 0;
    // Closes a whole tab, panes and all. False when the closed tab was
    // the last one: the caller owns the decision to close the window.
    virtual bool close(size_t index) = 0;

    // A4/A5: the panes of the active tab in visual order, each with the
    // rectangle it occupies inside the window's content box. This is the
    // list of live panes - what a frame draws, what a pointer hit-tests
    // against, and what A11 sums a window-wide budget over.
    virtual void visiblePanes(stl::Vector<SessionPane>& out) const = 0;
    // Divides the focused pane, giving the new one the far half and the
    // focus. False when the `panes` option is off or there is nothing to
    // divide.
    virtual bool splitFocused(SplitDirection direction) = 0;
    // Closes the focused pane. When it was the tab's last one the tab
    // goes with it, and the answer is then close()'s: false when that
    // was the last tab.
    virtual bool closeFocusedPane() = 0;
    // Moves the focus to the neighbouring pane of the active tab. False
    // when there is none on that side.
    virtual bool focusNeighbour(PaneSide side) = 0;
    // Moves the focus to one named pane of the active tab; ignored when
    // the pane is not there, which a hit test on a pane that has just
    // died will ask for.
    virtual void focusPane(u64 pane) = 0;
    // The terminal of the pane occupying this surface pixel, in the same
    // space the pointer events use. Falls back to activeTerminal() for a
    // pixel outside every pane - the window's borders and, before any tab
    // exists, all of it. With one pane per tab this is always the active
    // terminal, which is why everything that delivers to "the window's
    // terminal" was right until panes arrived and has to ask here now.
    virtual Vterm* terminalAt(int pixelX, int pixelY) const = 0;
    // F9: the seams of the active tab, as bands of pixels to paint, in
    // the same content-box coordinates visiblePanes() answers in.
    //
    // A band and not a line: two neighbouring panes already leave air
    // between their grids - each carries its own border inside its own
    // rectangle - and the seam is painted into that air rather than
    // taking pixels from either pane. So a window with a divider is laid
    // out exactly like a window without one, and the width is clamped to
    // the air there is. Empty when the panes option is off, when the tab
    // holds one pane, or when there is no air to paint into - which is
    // what a border of zero means.
    virtual void visibleSeams(stl::Vector<PixelRect>& out) const = 0;

    // A11: the cells held by every live pane except one, which is how a
    // store shared by the whole window gets sized by the sum over its
    // panes instead of by whoever wrote to it last. The exception is the
    // caller, which adds its own count: it may not be in the set yet
    // when it asks, because a terminal sizes the store while it is still
    // being built.
    virtual size_t cellCapacityExcept(const Vterm* except) const = 0;

    // The number of live panes, readable from a signal handler.
    static volatile sig_atomic_t liveSessions;

    static SessionSet* create(Composer& composer);
};
