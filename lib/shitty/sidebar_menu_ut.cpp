/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "sidebar_actions.h"
#include "sidebar_field.h"
#include "sidebar_menu.h"

#include <std/lib/vector.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

using namespace stl;

namespace {
    // The item for `action`, or -1.
    long long find(const Vector<SidebarMenuItem>& items, SidebarMenuAction action, StringView folder = StringView()) {
        for (size_t at = 0; at < items.length(); ++at) {
            if (items[at].action == action && (folder.empty() || items[at].folder == folder)) {
                return (long long)(at);
            }
        }
        return -1;
    }

    TabRow tabRow(size_t tab, StringView folder) {
        TabRow row;
        row.tab = tab;
        row.pane = 100 + tab;
        row.folder = folder;
        return row;
    }

    TabRow header(StringView folder) {
        TabRow row;
        row.label = true;
        row.folder = folder;
        return row;
    }

    plt::KeyInput press(plt::InputKey key, u16 modifiers = 0) {
        plt::KeyInput input;
        input.key = key;
        input.modifiers = modifiers;
        return input;
    }

    void type(SidebarField& field, StringView text) {
        for (size_t at = 0; at < text.length(); ++at) {
            field.insert((u8)(text.data()[at]));
        }
    }
}

STD_TEST_SUITE(SidebarMenu) {
    // A tab's menu lists the window's folders to move to, the one it is in
    // checked and not pickable, then Remove from Folder (it is in one),
    // Rename, Pin and Close; a loose tab has no Remove. A closed
    // bookmark's row says Remove Bookmark and has no Close Tab.
    STD_TEST(ATabsMenuMovesRenamesPinsAndCloses) {
        Vector<StringView> folders;
        folders.pushBack(StringView(u8"work"));
        folders.pushBack(StringView(u8"play"));
        const TabRow inWork = tabRow(0, StringView(u8"work"));
        Vector<SidebarMenuItem> items;
        sidebarMenuItems(&inWork, folders, false, items);
        const long long work = find(items, SidebarMenuAction::MoveToFolder, StringView(u8"work"));
        const long long play = find(items, SidebarMenuAction::MoveToFolder, StringView(u8"play"));
        STD_INSIST(work >= 0 && play >= 0 && work != play);
        STD_INSIST(items[(size_t)(work)].checked && !items[(size_t)(work)].pickable());
        STD_INSIST(!items[(size_t)(play)].checked && items[(size_t)(play)].pickable());
        STD_INSIST(find(items, SidebarMenuAction::MoveToNewFolder) > play);
        STD_INSIST(find(items, SidebarMenuAction::RemoveFromFolder) >= 0);
        STD_INSIST(items[(size_t)(find(items, SidebarMenuAction::RenameRow))].label == StringView(u8"Rename Tab…"));
        STD_INSIST(items[(size_t)(find(items, SidebarMenuAction::Pin))].label == StringView(u8"Pin Tab"));
        STD_INSIST(find(items, SidebarMenuAction::CloseTab) >= 0);
        STD_INSIST(find(items, SidebarMenuAction::DeleteFolder) < 0);

        const TabRow loose = tabRow(1, StringView());
        sidebarMenuItems(&loose, folders, false, items);
        STD_INSIST(find(items, SidebarMenuAction::RemoveFromFolder) < 0);

        TabRow closed = tabRow(2, StringView());
        closed.bookmark = 7;
        closed.closed = true;
        sidebarMenuItems(&closed, folders, true, items);
        STD_INSIST(items[(size_t)(find(items, SidebarMenuAction::Pin))].label == StringView(u8"Remove Bookmark"));
        STD_INSIST(items[(size_t)(find(items, SidebarMenuAction::RenameRow))].label == StringView(u8"Rename Bookmark…"));
        STD_INSIST(find(items, SidebarMenuAction::CloseTab) < 0);
    }

    // A folder's header offers Show/Hide, Rename, and the two deletes the
    // Mac asks about in a dialog; the empty space, New Tab. Both end in
    // New Folder.
    STD_TEST(AFoldersMenuAndTheEmptySpacesMenu) {
        const Vector<StringView> folders;
        TabRow head = header(StringView(u8"work"));
        Vector<SidebarMenuItem> items;
        sidebarMenuItems(&head, folders, false, items);
        STD_INSIST(items[0].action == SidebarMenuAction::ToggleFolder && items[0].label == StringView(u8"Hide Contents"));
        head.collapsed = true;
        sidebarMenuItems(&head, folders, false, items);
        STD_INSIST(items[0].label == StringView(u8"Show Contents"));
        STD_INSIST(find(items, SidebarMenuAction::RenameFolder) >= 0);
        STD_INSIST(find(items, SidebarMenuAction::DeleteFolder) >= 0 && find(items, SidebarMenuAction::DeleteFolderAndCloseTabs) >= 0);
        STD_INSIST(find(items, SidebarMenuAction::MoveToNewFolder) < 0 && find(items, SidebarMenuAction::CloseTab) < 0);
        STD_INSIST(items.back().action == SidebarMenuAction::NewFolder);

        sidebarMenuItems(nullptr, folders, false, items);
        STD_INSIST(items.length() == 2 && items[0].action == SidebarMenuAction::NewTab && items[1].action == SidebarMenuAction::NewFolder);
    }

    // Items are found where they are laid out, each of its own height; a
    // separator and a heading are found as nothing.
    STD_TEST(ItemsAreFoundWhereTheyAreLaidOut) {
        Vector<StringView> folders;
        folders.pushBack(StringView(u8"work"));
        const TabRow loose = tabRow(0, StringView());
        Vector<SidebarMenuItem> items;
        sidebarMenuItems(&loose, folders, false, items);
        // Premise: the three kinds have three different heights.
        STD_INSIST(SidebarMenuMetrics::itemHeight != SidebarMenuMetrics::headingHeight && SidebarMenuMetrics::headingHeight != SidebarMenuMetrics::separatorHeight);
        STD_INSIST(items[0].action == SidebarMenuAction::Heading);
        float expected = 0;
        for (const SidebarMenuItem& item : items) {
            expected += sidebarMenuItemHeight(item);
        }
        STD_INSIST(sidebarMenuHeight(items) == expected + 2 * SidebarMenuMetrics::pad);
        for (size_t at = 0; at < items.length(); ++at) {
            const float top = sidebarMenuItemTop(items, at);
            const float middle = top + sidebarMenuItemHeight(items[at]) / 2;
            STD_INSIST(sidebarMenuItemAt(items, middle) == (items[at].pickable() ? (long long)(at) : -1));
            STD_INSIST(sidebarMenuItemAt(items, top + sidebarMenuItemHeight(items[at]) - 0.25f) == sidebarMenuItemAt(items, top));
        }
        STD_INSIST(sidebarMenuItemAt(items, 1) == -1);
        STD_INSIST(sidebarMenuItemAt(items, sidebarMenuHeight(items) - 1) == -1);
    }

    // The menu opens at the click, moved in as far as it must to stay in
    // the window, and never past its top-left.
    STD_TEST(TheMenuStaysInsideTheWindow) {
        float x = 0;
        float y = 0;
        sidebarMenuPlace(50, 60, 200, 100, 10, 20, 800, 600, x, y);
        STD_INSIST(x == 50 && y == 60);
        sidebarMenuPlace(700, 580, 200, 100, 10, 20, 800, 600, x, y);
        STD_INSIST(x == 610 && y == 520);
        sidebarMenuPlace(700, 580, 2000, 1000, 10, 20, 800, 600, x, y);
        STD_INSIST(x == 10 && y == 20);
    }
}

STD_TEST_SUITE(SidebarDrop) {
    // Rows: a bookmark, folder "work" with two tabs, then two loose tabs
    // after the line. Each gap and each header says where a drop lands.
    STD_TEST(EachGapAndHeaderSaysWhereADropLands) {
        Vector<TabRow> rows;
        TabRow mark = tabRow(0, StringView());
        mark.bookmark = 3;
        rows.pushBack(mark);
        rows.pushBack(header(StringView(u8"work")));
        rows.pushBack(tabRow(1, StringView(u8"work")));
        rows.pushBack(tabRow(2, StringView(u8"work")));
        TabRow firstLoose = tabRow(3, StringView());
        rows.pushBack(firstLoose);
        rows.pushBack(tabRow(4, StringView()));
        const size_t tabs = 5;
        StringView folder;
        size_t before = 0;

        sidebarDropDestination(rows, 1, true, tabs, folder, before);
        STD_INSIST(folder == StringView(u8"work") && before == tabs);
        // Inside the folder, before its second tab.
        sidebarDropDestination(rows, 3, false, tabs, folder, before);
        STD_INSIST(folder == StringView(u8"work") && before == 2);
        // The gap at the folder's end, above the first loose tab: loose,
        // since the row below is in no folder.
        sidebarDropDestination(rows, 4, false, tabs, folder, before);
        STD_INSIST(folder.empty() && before == 3);
        // Past the last row: loose, at the end.
        sidebarDropDestination(rows, rows.length(), false, tabs, folder, before);
        STD_INSIST(folder.empty() && before == tabs);
        // Above the folder's header: the gap's row below is a header, so
        // the row above decides - the bookmark, in none.
        sidebarDropDestination(rows, 1, false, tabs, folder, before);
        STD_INSIST(folder.empty() && before == tabs);
    }
}

STD_TEST_SUITE(SidebarField) {
    // It starts with the old name selected: typing replaces it whole.
    STD_TEST(TypingReplacesTheOldNameWhole) {
        SidebarField field;
        field.begin(StringView(u8"New Folder"));
        STD_INSIST(field.selected() && field.anchor() == 0 && field.caret() == 10);
        type(field, StringView(u8"ops"));
        STD_INSIST(field.text() == StringView(u8"ops") && field.caret() == 3 && !field.selected());
    }

    // Keys edit by codepoint, not by byte: a two-byte letter goes whole.
    STD_TEST(KeysMoveAndEraseByCodepoint) {
        SidebarField field;
        field.begin(StringView(u8"aжb"));
        // Premise: three codepoints in four bytes.
        STD_INSIST(field.length() == 3 && field.text().length() == 4);
        STD_INSIST(field.key(press(plt::InputKey::End)) == SidebarField::Outcome::Edited);
        field.key(press(plt::InputKey::Left));
        field.key(press(plt::InputKey::Backspace));
        STD_INSIST(field.text() == StringView(u8"ab") && field.caret() == 1);
        field.insert(0x0436);
        STD_INSIST(field.text() == StringView(u8"aжb") && field.caret() == 2);
        field.key(press(plt::InputKey::Home));
        field.key(press(plt::InputKey::Delete));
        STD_INSIST(field.text() == StringView(u8"жb") && field.caret() == 0);
        StringBuilder prefix;
        field.utf8(0, 1, prefix);
        STD_INSIST(StringView(prefix) == StringView(u8"ж"));
    }

    // Shift extends, a plain arrow collapses a selection to its edge, and
    // Ctrl+A takes it all.
    STD_TEST(SelectionGrowsWithShiftAndCollapsesWithoutIt) {
        SidebarField field;
        field.begin(StringView(u8"abcd"));
        field.key(press(plt::InputKey::Left));
        STD_INSIST(!field.selected() && field.caret() == 0);
        field.key(press(plt::InputKey::Right, plt::InputShift));
        field.key(press(plt::InputKey::Right, plt::InputShift));
        STD_INSIST(field.anchor() == 0 && field.caret() == 2);
        field.key(press(plt::InputKey::Backspace));
        STD_INSIST(field.text() == StringView(u8"cd") && field.caret() == 0);
        plt::KeyInput all = press(plt::InputKey::Unknown, plt::InputControl);
        all.baseCodepoint = 'a';
        STD_INSIST(field.key(all) == SidebarField::Outcome::Edited);
        STD_INSIST(field.anchor() == 0 && field.caret() == 2);
        field.key(press(plt::InputKey::Right));
        STD_INSIST(!field.selected() && field.caret() == 2);
    }

    // Enter keeps, Escape puts back; a key it has no use for is the
    // caller's, and a control character is not typed.
    STD_TEST(EnterCommitsEscapeCancelsAndTheRestIsIgnored) {
        SidebarField field;
        field.begin(StringView(u8"x"));
        STD_INSIST(field.key(press(plt::InputKey::Enter)) == SidebarField::Outcome::Commit);
        STD_INSIST(field.key(press(plt::InputKey::Escape)) == SidebarField::Outcome::Cancel);
        STD_INSIST(field.key(press(plt::InputKey::Tab)) == SidebarField::Outcome::Ignored);
        field.insert(0x09);
        STD_INSIST(field.text() == StringView(u8"x"));
    }
}
