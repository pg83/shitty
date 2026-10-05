/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "sidebar_rows.h"

#include <std/tst/ut.h>

using namespace stl;

// The sidebar's shared arithmetic, on every platform: the Mac's list and
// the one pt draws on Wayland both place their rows from it.
STD_TEST_SUITE(SidebarRows) {
    // A folder's rows sit in from its header, and the pin in a row's gutter
    // moves with the row: a click where the loose row's pin is lands on
    // nothing in a folder's row, and one where the folder's row pin is
    // lands on it.
    STD_TEST(AFoldersRowsSitInAndTakeTheirPinWithThem) {
        const double loose = sidebarTabsIndent(false);
        const double member = sidebarTabsIndent(true);
        // Premise: the indent is there to see.
        STD_INSIST(loose == 0);
        STD_INSIST(member > SidebarMetrics::pillInset);

        const double loosePin = SidebarMetrics::pillInset + 1;
        STD_INSIST(sidebarTabsPinAt(loosePin, loose));
        STD_INSIST(!sidebarTabsPinAt(loosePin, member));
        STD_INSIST(sidebarTabsPinAt(loosePin + member, member));
        // Past the gutter, on the text, is not the pin for either.
        const double text = SidebarMetrics::textInset + SidebarMetrics::numberGutter + 1;
        STD_INSIST(!sidebarTabsPinAt(text, loose));
        STD_INSIST(!sidebarTabsPinAt(text + member, member));
    }

    // The first loose row after the folders, or the first tab after the
    // bookmarks, starts a section and gets air above it; nothing else does.
    STD_TEST(ASectionStartsAtTheFirstLooseRowAfterTheFolders) {
        // Premise: the gap is there to see.
        STD_INSIST(SidebarMetrics::sectionGap > 0);
        STD_INSIST(sidebarTabsStartsSection(false, false, true, false));
        STD_INSIST(sidebarTabsStartsSection(false, false, false, true));
        // A folder's own rows, a header, and loose rows after loose rows
        // run on without a break.
        STD_INSIST(!sidebarTabsStartsSection(false, true, true, false));
        STD_INSIST(!sidebarTabsStartsSection(true, false, true, false));
        STD_INSIST(!sidebarTabsStartsSection(false, false, false, false));
    }

    // Two lines to a row, title and where, inside a row two lines tall
    // plus even padding - and a folder's header a height of its own.
    STD_TEST(ARowIsTwoLinesAndAHeaderIsItsOwnHeight) {
        const double row = sidebarTabsRowHeight();
        const double bottom = sidebarTabsLineTop(1) + sidebarTabsLineHeight(1);
        STD_INSIST(sidebarTabsLineTop(0) + sidebarTabsLineHeight(0) <= sidebarTabsLineTop(1));
        STD_INSIST(row - bottom == sidebarTabsLineTop(0));
        STD_INSIST(row == SidebarMetrics::rowPad * 2 + SidebarMetrics::titleLine + SidebarMetrics::subLine);
        STD_INSIST(SidebarMetrics::labelRowHeight != row);
    }
}
