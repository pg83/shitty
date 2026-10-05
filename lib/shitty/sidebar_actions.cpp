/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "sidebar_actions.h"

#include "bookmark_probe.h"
#include "bookmarks.h"
#include "brand.h"
#include "composer.h"
#include "session.h"

#include <plt/window.h>

#include <std/mem/obj_pool.h>
#include <std/str/builder.h>

using namespace stl;

namespace {
    StringView identifierOf(Composer& composer) {
        return composer.brand != nullptr ? composer.brand->identifier() : StringView();
    }
}

bool sidebarRowPinnable(const TabRow& row) {
    return !row.label && (!row.grouped || row.groupFirst);
}

void sidebarPinRow(Composer& composer, const TabRow& row) {
    SessionSet* const sessions = composer.sessions;
    BookmarkShelf* const shelf = composer.bookmarks;
    if (sessions == nullptr || shelf == nullptr || !sidebarRowPinnable(row)) {
        return;
    }
    if (row.bookmark != 0 && shelf->find(row.bookmark) != nullptr) {
        // Unpinned, an open bookmark's tab stays open as an ordinary tab.
        if (unpinBookmark(*shelf, *composer.pool, identifierOf(composer), row.bookmark) && !row.closed) {
            sessions->adoptBookmark(row.tab, 0);
        }
    } else {
        Bookmark draft;
        tabBookmarkDraft(*sessions, row.tab, composer.brand != nullptr ? composer.brand->displayName() : StringView(u8"Terminal"), *composer.pool, draft);
        u64 id = 0;
        if (pinBookmark(*shelf, *composer.pool, identifierOf(composer), draft, id)) {
            sessions->adoptBookmark(row.tab, id);
        }
    }
    if (composer.bookmarkProbe != nullptr) {
        composer.bookmarkProbe->watch(*shelf);
    }
}

bool sidebarCloseRow(Composer& composer, const TabRow& row) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || row.label || row.closed) {
        return true;
    }
    return sessions->close(row.tab);
}

StringView sidebarCreateFolder(Composer& composer) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr) {
        return StringView();
    }
    // "New Folder", then "New Folder 2" and on: never one already there.
    Vector<StringView> order;
    sessions->folders(order);
    StringBuilder name;
    for (unsigned n = 1;; ++n) {
        name.reset();
        name << StringView(u8"New Folder");
        if (n > 1) {
            name << StringView(u8" ") << (i64)(n);
        }
        if (folderIndex(order, StringView(name)) == order.length()) {
            break;
        }
    }
    const StringView folder = composer.pool->intern(StringView(name));
    sessions->addFolder(folder);
    // Saved, so it is there after a restart, empty or holding only tabs.
    if (composer.bookmarks != nullptr) {
        saveFolder(*composer.bookmarks, *composer.pool, identifierOf(composer), folder);
    }
    return folder;
}

void sidebarRenameFolder(Composer& composer, Vector<StringView>& collapsed, StringView from, StringView to) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || from.empty() || to.empty() || to == from) {
        return;
    }
    const StringView kept = composer.pool->intern(to);
    // The bookmarks naming it move in their file, the window's tabs in
    // the model; a shut folder stays shut under its new name.
    if (composer.bookmarks != nullptr) {
        renameFolderInFile(*composer.bookmarks, *composer.pool, identifierOf(composer), from, kept);
    }
    for (size_t at = 0; at < collapsed.length(); ++at) {
        if (collapsed[at] == from) {
            collapsed.mut(at) = kept;
        }
    }
    sessions->renameFolder(from, kept);
}

void sidebarRenameRow(Composer& composer, const TabRow& row, StringView name) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || row.label) {
        return;
    }
    BookmarkShelf* const shelf = composer.bookmarks;
    if (row.bookmark != 0 && shelf != nullptr && shelf->find(row.bookmark) != nullptr) {
        // A bookmark's name is saved: it is its title in the file.
        if (!name.empty()) {
            setBookmarkTitle(*shelf, *composer.pool, row.bookmark, name);
        }
        return;
    }
    if (row.closed) {
        return;
    }
    // By its pane rather than its index: the list may have moved since.
    Vector<u64> panes;
    for (size_t tab = 0; tab < sessions->count(); ++tab) {
        sessions->panes(tab, panes);
        for (const u64 candidate : panes) {
            if (candidate == row.pane) {
                sessions->setTabTitle(tab, name);
                return;
            }
        }
    }
}

bool sidebarDeleteFolder(Composer& composer, Vector<StringView>& collapsed, StringView folder, bool closeTabs) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || folder.empty()) {
        return true;
    }
    const StringView kept = composer.pool->intern(folder);
    // Its tabs first, while tabFolder() still names it for a bookmark tab;
    // from the back, since a close moves the tabs behind it.
    bool open = true;
    if (closeTabs) {
        for (size_t tab = sessions->count(); tab-- > 0;) {
            if (tab < sessions->count() && sessions->tabFolder(tab) == kept && !sessions->close(tab)) {
                open = false;
                break;
            }
        }
    }
    if (composer.bookmarks != nullptr) {
        deleteFolderInFile(*composer.bookmarks, *composer.pool, identifierOf(composer), kept, closeTabs);
    }
    size_t shut = 0;
    for (size_t at = 0; at < collapsed.length(); ++at) {
        if (collapsed[at] != kept) {
            collapsed.mut(shut++) = collapsed[at];
        }
    }
    while (collapsed.length() > shut) {
        collapsed.popBack();
    }
    sessions->removeFolder(kept);
    return open;
}

void sidebarDropRow(Composer& composer, const TabRow& row, StringView folder, size_t before) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || row.label) {
        return;
    }
    BookmarkShelf* const shelf = composer.bookmarks;
    const Bookmark* const bookmark = row.bookmark != 0 && shelf != nullptr ? shelf->find(row.bookmark) : nullptr;
    if (bookmark != nullptr) {
        // A bookmark is moved in its file; its tab, if open, follows.
        if (bookmark->folder != folder) {
            setBookmarkFolder(*shelf, *composer.pool, identifierOf(composer), row.bookmark, folder);
            sessions->resort();
        }
    } else if (!row.closed) {
        sessions->dropTab(row.tab, folder, before);
    }
}

void sidebarDropDestination(const Vector<TabRow>& rows, size_t at, bool onLabel, size_t tabCount, StringView& folder, size_t& before) {
    folder = StringView();
    before = tabCount;
    const size_t count = rows.length();
    if (onLabel && at < count) {
        folder = rows[at].folder;
        return;
    }
    const TabRow* const below = at < count ? &rows[at] : nullptr;
    const TabRow* const above = at > 0 && at - 1 < count ? &rows[at - 1] : nullptr;
    if (below != nullptr && !below->label && !below->afterBookmarks) {
        folder = below->folder;
    } else if (above != nullptr) {
        folder = above->folder;
    }
    if (below != nullptr && !below->label && !below->closed) {
        before = below->tab;
    }
}

void sidebarToggleFolder(Composer& composer, Vector<StringView>& collapsed, StringView folder) {
    if (folder.empty()) {
        return;
    }
    size_t kept = 0;
    bool wasShut = false;
    for (size_t at = 0; at < collapsed.length(); ++at) {
        if (collapsed[at] == folder) {
            wasShut = true;
        } else {
            collapsed.mut(kept++) = collapsed[at];
        }
    }
    while (collapsed.length() > kept) {
        collapsed.popBack();
    }
    if (!wasShut) {
        collapsed.pushBack(composer.pool->intern(folder));
    }
}

void sidebarFolderMembers(Composer& composer, const Vector<StringView>& collapsed, StringView folder, Vector<TabRow>& out) {
    out.clear();
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr) {
        return;
    }
    Vector<StringView> open;
    for (const StringView name : collapsed) {
        if (name != folder) {
            open.pushBack(name);
        }
    }
    Vector<TabRow> all;
    tabRows(*sessions, composer.bookmarks, open, all);
    for (const TabRow& row : all) {
        if (!row.label && row.folder == folder && (!row.grouped || row.groupFirst)) {
            out.pushBack(row);
        }
    }
}

StringView sidebarRowTitle(Composer& composer, const TabRow& row) {
    const Bookmark* const bookmark = row.bookmark != 0 && composer.bookmarks != nullptr ? composer.bookmarks->find(row.bookmark) : nullptr;
    if (bookmark != nullptr) {
        return bookmark->title;
    }
    SessionSet* const sessions = composer.sessions;
    StringView title = sessions != nullptr && !row.closed ? sessions->tabTitle(row.tab) : StringView();
    if (title.length() == 0 && sessions != nullptr && !row.closed) {
        title = sessions->paneTitle(row.pane);
    }
    if (title.length() != 0 || composer.brand == nullptr) {
        return title;
    }
    return composer.brand->displayName();
}
