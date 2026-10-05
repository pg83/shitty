/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "sidebar_menu.h"

#include "sidebar_actions.h"

using namespace stl;

namespace {
    void add(Vector<SidebarMenuItem>& out, SidebarMenuAction action, StringView label) {
        SidebarMenuItem item;
        item.action = action;
        item.label = label;
        out.pushBack(item);
    }

    void separator(Vector<SidebarMenuItem>& out) {
        if (!out.empty() && out.back().action != SidebarMenuAction::Separator) {
            add(out, SidebarMenuAction::Separator, StringView());
        }
    }
}

void sidebarMenuItems(const TabRow* row, const Vector<StringView>& folders, bool pinned, Vector<SidebarMenuItem>& out) {
    out.clear();
    if (row != nullptr && row->label) {
        add(out, SidebarMenuAction::ToggleFolder, row->collapsed ? StringView(u8"Show Contents") : StringView(u8"Hide Contents"));
        add(out, SidebarMenuAction::RenameFolder, StringView(u8"Rename Folder…"));
        separator(out);
        add(out, SidebarMenuAction::DeleteFolder, StringView(u8"Delete Folder"));
        add(out, SidebarMenuAction::DeleteFolderAndCloseTabs, StringView(u8"Delete Folder and Close Tabs"));
        separator(out);
    } else if (row != nullptr) {
        add(out, SidebarMenuAction::Heading, StringView(u8"Move to Folder"));
        for (const StringView folder : folders) {
            SidebarMenuItem item;
            item.action = SidebarMenuAction::MoveToFolder;
            item.label = folder;
            item.folder = folder;
            item.checked = folder == row->folder;
            item.enabled = !item.checked;
            item.indented = true;
            out.pushBack(item);
        }
        add(out, SidebarMenuAction::MoveToNewFolder, StringView(u8"New Folder…"));
        out.mut(out.length() - 1).indented = true;
        separator(out);
        if (!row->folder.empty()) {
            add(out, SidebarMenuAction::RemoveFromFolder, StringView(u8"Remove from Folder"));
        }
        add(out, SidebarMenuAction::RenameRow, row->bookmark != 0 ? StringView(u8"Rename Bookmark…") : StringView(u8"Rename Tab…"));
        if (sidebarRowPinnable(*row)) {
            add(out, SidebarMenuAction::Pin, pinned ? (row->closed ? StringView(u8"Remove Bookmark") : StringView(u8"Unpin Tab")) : StringView(u8"Pin Tab"));
        }
        if (!row->closed) {
            separator(out);
            add(out, SidebarMenuAction::CloseTab, StringView(u8"Close Tab"));
        }
        separator(out);
    } else {
        add(out, SidebarMenuAction::NewTab, StringView(u8"New Tab"));
    }
    add(out, SidebarMenuAction::NewFolder, StringView(u8"New Folder"));
}

float sidebarMenuItemHeight(const SidebarMenuItem& item) {
    switch (item.action) {
        case SidebarMenuAction::Separator:
            return SidebarMenuMetrics::separatorHeight;
        case SidebarMenuAction::Heading:
            return SidebarMenuMetrics::headingHeight;
        default:
            return SidebarMenuMetrics::itemHeight;
    }
}

float sidebarMenuItemTop(const Vector<SidebarMenuItem>& items, size_t index) {
    float top = SidebarMenuMetrics::pad;
    for (size_t at = 0; at < index && at < items.length(); ++at) {
        top += sidebarMenuItemHeight(items[at]);
    }
    return top;
}

float sidebarMenuHeight(const Vector<SidebarMenuItem>& items) {
    return sidebarMenuItemTop(items, items.length()) + SidebarMenuMetrics::pad;
}

long long sidebarMenuItemAt(const Vector<SidebarMenuItem>& items, float y) {
    float top = SidebarMenuMetrics::pad;
    for (size_t at = 0; at < items.length(); ++at) {
        const float height = sidebarMenuItemHeight(items[at]);
        if (y >= top && y < top + height) {
            return items[at].pickable() ? (long long)(at) : -1;
        }
        top += height;
    }
    return -1;
}

void sidebarMenuPlace(float x, float y, float width, float height, float boundsX, float boundsY, float boundsWidth, float boundsHeight, float& outX, float& outY) {
    outX = x + width > boundsX + boundsWidth ? boundsX + boundsWidth - width : x;
    outY = y + height > boundsY + boundsHeight ? boundsY + boundsHeight - height : y;
    if (outX < boundsX) {
        outX = boundsX;
    }
    if (outY < boundsY) {
        outY = boundsY;
    }
}
