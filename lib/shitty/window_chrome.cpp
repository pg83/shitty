/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "window_chrome.h"

#include "sidebar_rows.h"

#include <plt/window.h>

namespace {
    void placeButtons(float left, float top, ChromeRect buttons[3]) {
        for (int at = 0; at < 3; ++at) {
            buttons[at] = ChromeRect{left + at * (ChromeMetrics::buttonSize + ChromeMetrics::buttonGap), top, ChromeMetrics::buttonSize, ChromeMetrics::buttonSize};
        }
    }

    ChromeHitKind buttonKind(int at) {
        return at == 0 ? ChromeHitKind::Close : at == 1 ? ChromeHitKind::Minimize : ChromeHitKind::Maximize;
    }

    // A button answers a little outside its circle, the way a Mac's does.
    bool onButton(const ChromeRect& button, float x, float y) {
        const float slack = ChromeMetrics::buttonGap / 2;
        return x >= button.x - slack && x < button.right() + slack && y >= button.y - slack && y < button.bottom() + slack;
    }

    // The list's rows from `listTop` in `list`, and the new-tab row under
    // them - the arithmetic the Mac's sidebar clicks and draws by.
    bool rowHit(const ChromeRect& list, float listTop, float x, float y, const double* heights, size_t count, ChromeHit& hit) {
        if (!list.contains(x, y)) {
            return false;
        }
        const long long row = sidebarTabsRowAtHeights(list.height, y - list.y, heights, count, listTop - SidebarMetrics::listTop);
        if (row < 0) {
            return false;
        }
        hit.kind = (size_t)(row) < count ? ChromeHitKind::Row : ChromeHitKind::NewTab;
        hit.row = (size_t)(row);
        return true;
    }
}

void chromeLayout(const ChromeGeometry& geometry, ChromeLayout& out) {
    out = ChromeLayout();
    const float margin = geometry.floating ? ChromeMetrics::margin : 0;
    out.margin = margin;
    out.windowRadius = geometry.floating ? ChromeMetrics::windowRadius : 0;
    out.window = ChromeRect{margin, margin, geometry.width - 2 * margin, geometry.height - 2 * margin};
    const float gap = geometry.panelGap;
    const float listWidth = geometry.listShown ? geometry.sidebarWidth : 0;
    if (geometry.listShown) {
        out.sidebar = ChromeRect{out.window.x, out.window.y, listWidth, out.window.height};
        placeButtons(out.window.x + ChromeMetrics::buttonLeft, out.window.y + ChromeMetrics::buttonTop, out.buttons);
    }
    const float panelLeft = out.window.x + (geometry.listShown ? listWidth : gap);
    out.panel = ChromeRect{panelLeft, out.window.y + gap, out.window.right() - gap - panelLeft, out.window.height - 2 * gap};
    // The panel's corners never outgrow it, or the window's round corner
    // around it.
    out.panelRadius = geometry.panelRadius;
    out.header = ChromeRect{out.panel.x, out.panel.y, out.panel.width, ChromeMetrics::header};
    out.content = ChromeRect{out.panel.x, out.panel.y + ChromeMetrics::header, out.panel.width, out.panel.height - ChromeMetrics::header};
    out.toggle = ChromeRect{out.panel.x + 6, out.panel.y + 4, 28, 28};
    out.insetLeft = (u32)(out.content.x + 0.5f);
    out.insetTop = (u32)(out.content.y + 0.5f);
    out.insetRight = (u32)(geometry.width - out.content.right() + 0.5f);
    out.insetBottom = (u32)(geometry.height - out.content.bottom() + 0.5f);
    out.insetMargin = (u32)(margin);
}

void chromeRevealLayout(const ChromeLayout& layout, float sidebarWidth, float panelGap, ChromeRect& list, ChromeRect buttons[3]) {
    list = ChromeRect{layout.window.x + panelGap, layout.window.y + panelGap, sidebarWidth, layout.window.height - 2 * panelGap};
    placeButtons(list.x + ChromeMetrics::buttonLeft - panelGap, list.y + ChromeMetrics::buttonTop - panelGap, buttons);
}

ChromeHit chromeHitTest(const ChromeLayout& layout, float x, float y, const double* heights, size_t count) {
    ChromeHit hit;
    // Resize first: the margin and a strip inside the window's edge, for a
    // free window only - one the shell has placed is not the user's to size.
    if (layout.margin > 0) {
        const float reach = ChromeMetrics::resizeInside;
        const ChromeRect& window = layout.window;
        u32 edges = 0;
        if (x < window.x + reach) {
            edges |= plt::ChromeEdgeLeft;
        } else if (x >= window.right() - reach) {
            edges |= plt::ChromeEdgeRight;
        }
        if (y < window.y + reach) {
            edges |= plt::ChromeEdgeTop;
        } else if (y >= window.bottom() - reach) {
            edges |= plt::ChromeEdgeBottom;
        }
        const bool near = x >= window.x - layout.margin && x < window.right() + layout.margin && y >= window.y - layout.margin && y < window.bottom() + layout.margin;
        if (edges != 0 && near) {
            hit.kind = ChromeHitKind::Resize;
            hit.edges = edges;
            return hit;
        }
        if (!near) {
            return hit;
        }
    }
    if (!layout.window.contains(x, y)) {
        return hit;
    }
    for (int at = 0; at < 3; ++at) {
        if (!layout.buttons[at].empty() && onButton(layout.buttons[at], x, y)) {
            hit.kind = buttonKind(at);
            return hit;
        }
    }
    if (layout.toggle.contains(x, y)) {
        hit.kind = ChromeHitKind::Toggle;
        return hit;
    }
    // Put away, the strip between the window's edge and the panel is where
    // the list comes back from - inside the resize handle, which keeps the
    // outermost pixels.
    if (layout.sidebar.empty() && x < layout.panel.x && y >= layout.panel.y) {
        hit.kind = ChromeHitKind::Reveal;
        return hit;
    }
    if (!layout.sidebar.empty() && rowHit(layout.sidebar, ChromeMetrics::listTop, x, y, heights, count, hit)) {
        return hit;
    }
    // Everything else of the chrome - the surface, the title bar, the
    // list's empty space - moves the window, like a Mac's title bar.
    hit.kind = ChromeHitKind::Move;
    return hit;
}

ChromeHit chromeRevealHitTest(const ChromeRect& list, const ChromeRect buttons[3], float x, float y, const double* heights, size_t count) {
    ChromeHit hit;
    if (!list.contains(x, y)) {
        return hit;
    }
    for (int at = 0; at < 3; ++at) {
        if (onButton(buttons[at], x, y)) {
            hit.kind = buttonKind(at);
            return hit;
        }
    }
    // The rows sit as far under the buttons as on the window's own list.
    const float listTop = ChromeMetrics::listTop - ChromeMetrics::buttonTop + (buttons[0].y - list.y);
    if (rowHit(list, listTop, x, y, heights, count, hit)) {
        return hit;
    }
    hit.kind = ChromeHitKind::Move;
    return hit;
}
