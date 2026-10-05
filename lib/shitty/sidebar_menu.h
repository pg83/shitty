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

// The context menu of the list pt draws on Wayland (ui_wayland_chrome.cpp):
// what it offers for a row, a folder's header, or the list's empty space,
// and where each item is. The Mac builds the same menu from NSMenu
// (ui_sidebar_tabs.mm); its submenus are sections here, and its delete
// dialog is two items, since a drawn menu has no sheet to ask from.

enum class SidebarMenuAction : u8 {
    // A line between groups of items; not an item.
    Separator,
    // A section's title, dim: "Move to Folder". Not an item either.
    Heading,
    MoveToFolder,
    MoveToNewFolder,
    RemoveFromFolder,
    RenameRow,
    Pin,
    CloseTab,
    ToggleFolder,
    RenameFolder,
    DeleteFolder,
    DeleteFolderAndCloseTabs,
    NewTab,
    NewFolder
};

struct SidebarMenuItem {
    SidebarMenuAction action = SidebarMenuAction::Separator;
    stl::StringView label;
    // MoveToFolder: the folder it moves to.
    stl::StringView folder;
    // A folder the row is already in is listed, checked, and not picked.
    bool enabled = true;
    bool checked = false;
    // Sits in under a heading.
    bool indented = false;

    bool pickable() const {
        return enabled && action != SidebarMenuAction::Separator && action != SidebarMenuAction::Heading;
    }
};

namespace SidebarMenuMetrics {
    // Around the items, inside the menu's rounded edge.
    inline constexpr float pad = 5;
    inline constexpr float itemHeight = 24;
    inline constexpr float headingHeight = 22;
    inline constexpr float separatorHeight = 9;
    // An item's text sits this far in from the menu's edge; a section's
    // items further, under its heading.
    inline constexpr float textInset = 12;
    inline constexpr float indent = 10;
    inline constexpr float minimumWidth = 180;
    inline constexpr float radius = 8;
}

// The items for `row`, or for the list's empty space when it is null.
// `folders` are the window's, in the list's order; `pinned` says the row's
// bookmark is on the shelf. Replaces what `out` held.
void sidebarMenuItems(const TabRow* row, const stl::Vector<stl::StringView>& folders, bool pinned, stl::Vector<SidebarMenuItem>& out);
float sidebarMenuItemHeight(const SidebarMenuItem& item);
// The menu's height, padding included.
float sidebarMenuHeight(const stl::Vector<SidebarMenuItem>& items);
// The top of item `index`, from the menu's top.
float sidebarMenuItemTop(const stl::Vector<SidebarMenuItem>& items, size_t index);
// The pickable item at `y` from the menu's top, or -1.
long long sidebarMenuItemAt(const stl::Vector<SidebarMenuItem>& items, float y);
// Where a menu `width` by `height` opens for a click at (x, y): there,
// moved left or up as far as it must to stay inside the bounds, never past
// their top-left.
void sidebarMenuPlace(float x, float y, float width, float height, float boundsX, float boundsY, float boundsWidth, float boundsHeight, float& outX, float& outY);
