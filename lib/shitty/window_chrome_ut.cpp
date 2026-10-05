/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "window_chrome.h"

#include "sidebar_rows.h"

#include <plt/window.h>

#include <std/tst/ut.h>

using namespace stl;

namespace {
    ChromeGeometry window(bool listShown, bool floating) {
        ChromeGeometry geometry;
        geometry.width = 1000;
        geometry.height = 600;
        geometry.listShown = listShown;
        geometry.floating = floating;
        geometry.sidebarWidth = 220;
        geometry.panelGap = 8;
        geometry.panelRadius = 12;
        return geometry;
    }

    float centreX(const ChromeRect& rect) {
        return rect.x + rect.width / 2;
    }

    float centreY(const ChromeRect& rect) {
        return rect.y + rect.height / 2;
    }
}

STD_TEST_SUITE(WindowChrome) {
    // The terminal sits right of the list, under the panel's title bar, a
    // gap clear of the window's other edges - and the insets the window is
    // handed say exactly that.
    STD_TEST(TheTerminalSitsBesideTheListUnderItsTitleBar) {
        ChromeLayout layout;
        chromeLayout(window(true, true), layout);
        // Premise: a margin, a list width and a gap that differ, so no
        // inset can pass by being one of the others.
        STD_INSIST(layout.margin == ChromeMetrics::margin);
        STD_INSIST(layout.margin != 8 && layout.margin != 220);
        STD_INSIST(layout.insetLeft == 16 + 220);
        STD_INSIST(layout.insetTop == 16 + 8 + (u32)(ChromeMetrics::header));
        STD_INSIST(layout.insetRight == 16 + 8);
        STD_INSIST(layout.insetBottom == 16 + 8);
        STD_INSIST(layout.insetMargin == 16);
        STD_INSIST(layout.content.x == layout.panel.x && layout.content.y == layout.header.bottom());
        STD_INSIST(layout.sidebar.right() == layout.panel.x);
        // The buttons are on the list, over its top.
        STD_INSIST(layout.sidebar.contains(centreX(layout.buttons[2]), centreY(layout.buttons[2])));
    }

    // Put away, the list takes its buttons with it and the panel widens to
    // a gap from the window's left edge.
    STD_TEST(PutAwayTheListLeavesTheWholeWindowToThePanel) {
        ChromeLayout shown;
        ChromeLayout hidden;
        chromeLayout(window(true, true), shown);
        chromeLayout(window(false, true), hidden);
        STD_INSIST(hidden.sidebar.empty());
        STD_INSIST(hidden.buttons[0].empty());
        STD_INSIST(hidden.insetLeft == 16 + 8);
        STD_INSIST(hidden.panel.width == shown.panel.width + 220 - 8);
        STD_INSIST(hidden.insetTop == shown.insetTop);
    }

    // A window the shell placed has no margin, no round corners and no
    // resize handles.
    STD_TEST(APlacedWindowHasNoMarginAndCannotBeResized) {
        ChromeLayout layout;
        chromeLayout(window(true, false), layout);
        STD_INSIST(layout.margin == 0 && layout.windowRadius == 0);
        STD_INSIST(layout.insetLeft == 220);
        const ChromeHit corner = chromeHitTest(layout, 1, 1, nullptr, 0);
        STD_INSIST(corner.kind != ChromeHitKind::Resize);
    }

    // Each control answers where it is drawn.
    STD_TEST(ThePointerFindsTheButtonsTheRowsAndTheEdges) {
        ChromeLayout layout;
        chromeLayout(window(true, true), layout);
        const double heights[3] = {SidebarMetrics::labelRowHeight, SidebarMetrics::rowHeight, SidebarMetrics::rowHeight};
        STD_INSIST(chromeHitTest(layout, centreX(layout.buttons[0]), centreY(layout.buttons[0]), heights, 3).kind == ChromeHitKind::Close);
        STD_INSIST(chromeHitTest(layout, centreX(layout.buttons[1]), centreY(layout.buttons[1]), heights, 3).kind == ChromeHitKind::Minimize);
        STD_INSIST(chromeHitTest(layout, centreX(layout.buttons[2]), centreY(layout.buttons[2]), heights, 3).kind == ChromeHitKind::Maximize);
        STD_INSIST(chromeHitTest(layout, centreX(layout.toggle), centreY(layout.toggle), heights, 3).kind == ChromeHitKind::Toggle);

        // The rows run down from the list's top: a short label, then two
        // full rows, then the new-tab row.
        const float listTop = layout.window.y + ChromeMetrics::listTop;
        const float x = layout.sidebar.x + 60;
        const ChromeHit label = chromeHitTest(layout, x, listTop + 10, heights, 3);
        STD_INSIST(label.kind == ChromeHitKind::Row && label.row == 0);
        const ChromeHit second = chromeHitTest(layout, x, listTop + (float)(heights[0] + heights[1]) + 5, heights, 3);
        STD_INSIST(second.kind == ChromeHitKind::Row && second.row == 2);
        const ChromeHit plus = chromeHitTest(layout, x, listTop + (float)(heights[0] + heights[1] + heights[2]) + 5, heights, 3);
        STD_INSIST(plus.kind == ChromeHitKind::NewTab);

        // The surface elsewhere moves the window; its corner resizes it.
        STD_INSIST(chromeHitTest(layout, layout.panel.x + 200, layout.header.y + 18, heights, 3).kind == ChromeHitKind::Move);
        const ChromeHit corner = chromeHitTest(layout, layout.window.x - 4, layout.window.y - 4, heights, 3);
        STD_INSIST(corner.kind == ChromeHitKind::Resize);
        STD_INSIST(corner.edges == (plt::ChromeEdgeTop | plt::ChromeEdgeLeft));
        const ChromeHit right = chromeHitTest(layout, layout.window.right() + 2, layout.window.y + 300, heights, 3);
        STD_INSIST(right.kind == ChromeHitKind::Resize && right.edges == plt::ChromeEdgeRight);
        // Past the margin is outside the window altogether.
        STD_INSIST(chromeHitTest(layout, layout.window.right() + layout.margin + 1, 300, heights, 3).kind == ChromeHitKind::None);
    }

    // Put away, the window's left edge is where the list comes back from,
    // and the list that comes out answers like the window's own.
    STD_TEST(ThePutAwayListComesOutAtTheEdgeAndAnswersLikeTheOther) {
        ChromeLayout layout;
        chromeLayout(window(false, true), layout);
        const double heights[2] = {SidebarMetrics::rowHeight, SidebarMetrics::rowHeight};
        STD_INSIST(chromeHitTest(layout, layout.window.x + 5, layout.panel.y + 100, heights, 2).kind == ChromeHitKind::Reveal);
        STD_INSIST(chromeHitTest(layout, layout.window.x + 30, layout.panel.y + 100, heights, 2).kind != ChromeHitKind::Reveal);

        ChromeLayout shown;
        chromeLayout(window(true, true), shown);
        ChromeRect list;
        ChromeRect buttons[3];
        chromeRevealLayout(layout, 220, 8, list, buttons);
        // The buttons and the first row sit where they do on the window's
        // own list: the same distance from the window's corner.
        STD_INSIST(buttons[0].x == shown.buttons[0].x && buttons[0].y == shown.buttons[0].y);
        const float firstRow = shown.window.y + ChromeMetrics::listTop + 5;
        const ChromeHit own = chromeHitTest(shown, shown.sidebar.x + 60, firstRow, heights, 2);
        const ChromeHit out = chromeRevealHitTest(list, buttons, list.x + 60, firstRow, heights, 2);
        STD_INSIST(own.kind == ChromeHitKind::Row && own.row == 0);
        STD_INSIST(out.kind == ChromeHitKind::Row && out.row == 0);
        STD_INSIST(chromeRevealHitTest(list, buttons, centreX(buttons[0]), centreY(buttons[0]), heights, 2).kind == ChromeHitKind::Close);
    }
}
