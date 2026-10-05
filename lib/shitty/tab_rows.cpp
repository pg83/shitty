/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "tab_rows.h"

#include "session.h"
#include "bookmarks.h"
#include "process_directory.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>

using namespace stl;

namespace {
    void appendTab(const SessionSet& sessions, size_t tab, u64 bookmark, Vector<u64>& panes, Vector<PanePlacement>& placements, Vector<TabRow>& out) {
        // Laid out in a box of this many units a side and divided back
        // down: fine enough that a share's rounding does not show in a map
        // a couple of dozen points wide.
        const u16 unit = 1000;
        panes.clear();
        sessions.panes(tab, panes);
        placements.clear();
        sessions.paneLayout(tab, PixelRect{0, 0, unit, unit}, placements);
        const u64 focused = sessions.focusedPane(tab);
        const size_t active = sessions.activeIndex();
        const size_t n = panes.length();
        for (size_t at = 0; at < n; ++at) {
            TabRow row;
            row.tab = tab;
            row.pane = panes[at];
            row.grouped = n > 1;
            row.groupFirst = row.grouped && at == 0;
            row.groupLast = row.grouped && at + 1 == n;
            row.focused = panes[at] == focused;
            row.activeTab = tab == active;
            row.bookmark = bookmark;
            row.exited = sessions.paneExited(panes[at]);
            for (const PanePlacement& placement : placements) {
                if (placement.pane == row.pane) {
                    row.left = (float)(placement.area.x) / unit;
                    row.top = (float)(placement.area.y) / unit;
                    row.width = (float)(placement.area.width) / unit;
                    row.height = (float)(placement.area.height) / unit;
                    break;
                }
            }
            out.pushBack(row);
        }
    }
}

void tabRows(const SessionSet& sessions, const BookmarkShelf* shelf, Vector<TabRow>& out) {
    const Vector<StringView> none;
    tabRows(sessions, shelf, none, out);
}

namespace {
    bool named(const Vector<StringView>& names, StringView name) {
        for (const StringView candidate : names) {
            if (candidate == name) {
                return true;
            }
        }
        return false;
    }

    // One bookmark's rows: its tab's, or one closed row.
    void appendBookmark(const SessionSet& sessions, const Bookmark& bookmark, Vector<u64>& panes, Vector<PanePlacement>& placements, Vector<bool>& listed, Vector<TabRow>& out) {
        const size_t count = sessions.count();
        size_t tab = 0;
        while (tab < count && sessions.tabBookmark(tab) != bookmark.id) {
            ++tab;
        }
        if (tab < count) {
            appendTab(sessions, tab, bookmark.id, panes, placements, out);
            listed.mut(tab) = true;
            return;
        }
        TabRow row;
        row.bookmark = bookmark.id;
        row.closed = true;
        row.folder = bookmark.folder;
        out.pushBack(row);
    }
}

void tabRows(const SessionSet& sessions, const BookmarkShelf* shelf, const Vector<StringView>& collapsed, Vector<TabRow>& out) {
    out.clear();
    const size_t count = sessions.count();
    const size_t active = sessions.activeIndex();
    Vector<u64> panes;
    Vector<PanePlacement> placements;
    Vector<bool> listed;
    for (size_t tab = 0; tab < count; ++tab) {
        listed.pushBack(false);
    }
    // The loose bookmarks.
    if (shelf != nullptr) {
        for (const Bookmark& bookmark : shelf->items) {
            if (bookmark.folder.empty()) {
                appendBookmark(sessions, bookmark, panes, placements, listed, out);
            }
        }
    }
    // Each folder: its label, then its bookmarks, then its tabs - or, shut,
    // only the tab in front when it is in there, so the list never loses
    // the tab the user is looking at.
    Vector<StringView> order;
    sessions.folders(order);
    Vector<TabRow> members;
    for (const StringView folder : order) {
        members.clear();
        size_t memberCount = 0;
        bool activeInside = false;
        if (shelf != nullptr) {
            for (const Bookmark& bookmark : shelf->items) {
                if (bookmark.folder == folder) {
                    appendBookmark(sessions, bookmark, panes, placements, listed, members);
                    ++memberCount;
                }
            }
        }
        for (size_t tab = 0; tab < count; ++tab) {
            if (!listed[tab] && sessions.tabBookmark(tab) == 0 && sessions.tabFolder(tab) == folder) {
                appendTab(sessions, tab, 0, panes, placements, members);
                listed.mut(tab) = true;
                ++memberCount;
            }
        }
        const bool shut = named(collapsed, folder);
        TabRow label;
        label.label = true;
        label.folder = folder;
        label.members = memberCount;
        label.collapsed = shut;
        for (const TabRow& member : members) {
            activeInside = activeInside || (!member.closed && member.tab == active);
        }
        label.activeInside = activeInside;
        out.pushBack(label);
        for (TabRow member : members) {
            member.folder = folder;
            if (!shut || (!member.closed && member.tab == active)) {
                out.pushBack(member);
            }
        }
    }
    const size_t before = out.length();
    for (size_t tab = 0; tab < count; ++tab) {
        if (listed[tab]) {
            continue;
        }
        // A tab whose bookmark has left the shelf is an ordinary tab now.
        const size_t first = out.length();
        appendTab(sessions, tab, 0, panes, placements, out);
        if (first == before && before != 0 && first < out.length()) {
            out.mut(first).afterBookmarks = true;
        }
    }
}

void tabBookmarkDraft(const SessionSet& sessions, size_t tab, StringView fallback, ObjPool& pool, Bookmark& out) {
    out = Bookmark();
    // Pinned where it is: a tab in a folder makes a bookmark in it.
    out.folder = sessions.tabFolder(tab);
    const u64 pane = sessions.focusedPane(tab);
    if (pane == 0) {
        out.title = fallback;
        return;
    }
    const pid_t shell = sessions.panePid(pane);
    Buffer directory;
    if (processDirectory(shell, directory)) {
        out.directory = pool.intern(StringView(directory));
    }
    const pid_t foreground = sessions.paneForeground(pane);
    Buffer arguments;
    if (foreground > 0 && foreground != shell && processCommandLine(foreground, arguments)) {
        StringBuilder command;
        shellCommandLine(StringView(arguments), command);
        out.command = pool.intern(StringView(command));
    }
    // A name the user gave the tab is the bookmark's name too.
    const StringView given = sessions.tabTitle(tab);
    if (!given.empty()) {
        out.title = pool.intern(given);
        return;
    }
    if (!out.command.empty()) {
        out.title = out.command;
        return;
    }
    StringView name = out.directory;
    for (size_t at = name.length(); at > 1; --at) {
        if (name[at - 1] == '/') {
            name = StringView(name.data() + at, name.length() - at);
            break;
        }
    }
    out.title = name.empty() ? fallback : name;
}
