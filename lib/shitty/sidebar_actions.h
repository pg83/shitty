/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "tab_rows.h"

#include <std/lib/vector.h>
#include <std/str/view.h>

#include <stddef.h>

struct Composer;

// What the sidebar's menus, clicks and drags do to the model and the
// bookmarks file - the same on the Mac (ui_sidebar_tabs.mm) and in the
// window pt draws on Wayland (ui_wayland_chrome.cpp). Plain functions of
// the composer and a row as the list drew it, no window toolkit, so both
// lists act alike and a test reaches every one. None of them repaints:
// the caller redraws its list after.

// Whether a row can be pinned or unpinned: a tab's (first) row or a
// bookmark's, never a folder's header.
bool sidebarRowPinnable(const TabRow& row);
// Pins the row's tab as a bookmark, or unpins the row's bookmark - its tab,
// if open, stays open as an ordinary one.
void sidebarPinRow(Composer& composer, const TabRow& row);
// Closes the row's tab. False when it was the window's last, which the
// caller closes the window for.
bool sidebarCloseRow(Composer& composer, const TabRow& row);
// A new folder, "New Folder" or "New Folder N", added to the window and
// saved in the bookmarks file so a restart keeps it. Its name.
stl::StringView sidebarCreateFolder(Composer& composer);
// Renames a folder: in the file, in the window, and in `collapsed`.
void sidebarRenameFolder(Composer& composer, stl::Vector<stl::StringView>& collapsed, stl::StringView from, stl::StringView to);
// Renames the row's tab, or its bookmark (in the file). An empty name
// clears a tab's name and leaves a bookmark's alone.
void sidebarRenameRow(Composer& composer, const TabRow& row, stl::StringView name);
// Deletes a folder: its bookmarks leave it, or with `closeTabs` go, and its
// tabs leave it or close. False when that closed the window's last tab.
bool sidebarDeleteFolder(Composer& composer, stl::Vector<stl::StringView>& collapsed, stl::StringView folder, bool closeTabs);
// Moves the row into `folder` (empty: none), its tab before tab `before`.
void sidebarDropRow(Composer& composer, const TabRow& row, stl::StringView folder, size_t before);
// Where a dragged row lands when let go at gap `at` - the gap above row
// `at`, rows.length() past the last - or, with `onLabel`, on folder header
// `at`: the folder it joins and the tab it goes before (`tabCount` for the
// end). A gap's folder is the row below's when that is in one, else the
// row above's: the gap at a folder's end joins it, the one under the line
// before the loose tabs does not.
void sidebarDropDestination(const stl::Vector<TabRow>& rows, size_t at, bool onLabel, size_t tabCount, stl::StringView& folder, size_t& before);
// Shuts a folder that is open, opens one that is shut.
void sidebarToggleFolder(Composer& composer, stl::Vector<stl::StringView>& collapsed, stl::StringView folder);
// The rows a folder holds, whether it is shut or not: one per tab and per
// bookmark.
void sidebarFolderMembers(Composer& composer, const stl::Vector<stl::StringView>& collapsed, stl::StringView folder, stl::Vector<TabRow>& out);
// What the row is called in the list: the bookmark's title, the tab's
// name, the pane's title, or the brand.
stl::StringView sidebarRowTitle(Composer& composer, const TabRow& row);
