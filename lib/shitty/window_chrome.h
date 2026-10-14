/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

#include <stddef.h>

// Where everything of a window this program draws itself goes (Wayland,
// ui_wayland_chrome.cpp): the window, the tab list down its left side with
// the window's buttons over it, and the terminal as a rounded panel with a
// title bar of its own. The Mac's layered window, in numbers - plain
// arithmetic, so the drawing, the clicks and the tests all read one answer.
//
// Logical units, relative to the toplevel surface's top-left corner.

struct ChromeRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
    bool empty() const {
        return width <= 0 || height <= 0;
    }
    float right() const {
        return x + width;
    }
    float bottom() const {
        return y + height;
    }
};

struct ChromeGeometry {
    // The toplevel surface, outer margin included.
    float width = 0;
    float height = 0;
    // The tab list is on the window's surface, or put away (Ctrl+Shift+B).
    bool listShown = true;
    // A free window, not maximized, tiled or fullscreen: it gets the outer
    // margin for its shadow and its resize handles, and round corners.
    bool floating = true;
    float sidebarWidth = 220;
    float panelGap = 8;
    float panelRadius = 12;
};

// The fixed numbers, the Mac's where it has one.
namespace ChromeMetrics {
    // Around a free window: its shadow, and where a drag resizes it.
    inline constexpr float margin = 16;
    inline constexpr float windowRadius = 12;
    // Resize handles reach this far in over the window's own edge too.
    inline constexpr float resizeInside = 4;
    // The panel's own title bar.
    inline constexpr float header = 36;
    // The window's buttons: 12 across, 8 apart, in from the corner.
    inline constexpr float buttonSize = 12;
    inline constexpr float buttonGap = 8;
    inline constexpr float buttonLeft = 18;
    inline constexpr float buttonTop = 17;
    // Where the list starts under the buttons, from the window's top.
    inline constexpr float listTop = 40;
}

struct ChromeLayout {
    float margin = 0;
    float windowRadius = 0;
    ChromeRect window;
    // The tab list's column; empty while it is put away.
    ChromeRect sidebar;
    // The terminal panel, title bar included, and its parts.
    ChromeRect panel;
    ChromeRect header;
    ChromeRect content;
    ChromeRect toggle;
    // Close, minimise, zoom; empty while the list is put away.
    ChromeRect buttons[3];
    float panelRadius = 0;
    // What Window::chrome()->setInsets() is handed.
    u32 insetLeft = 0;
    u32 insetTop = 0;
    u32 insetRight = 0;
    u32 insetBottom = 0;
    u32 insetMargin = 0;
};

void chromeLayout(const ChromeGeometry& geometry, ChromeLayout& out);

// The list that comes out at the window's edge while it is put away: its
// rectangle over the panel, and its buttons, in the same toplevel units.
void chromeRevealLayout(const ChromeLayout& layout, float sidebarWidth, float panelGap, ChromeRect& list, ChromeRect buttons[3]);

enum class ChromeHitKind : u8 {
    None,
    Close,
    Minimize,
    Maximize,
    Toggle,
    Row,
    NewTab,
    Move,
    Resize,
    Reveal
};

struct ChromeHit {
    ChromeHitKind kind = ChromeHitKind::None;
    // Resize: the plt::ChromeEdge* bits.
    u32 edges = 0;
    // Row: its index into the rows.
    size_t row = 0;
};

// What a point is over. `heights` are the list's rows (a folder's label is
// short), laid out in `list` from `listTop` down, the new-tab row under the
// last; `buttons` are the window's buttons over it.
ChromeHit chromeHitTest(const ChromeLayout& layout, float x, float y, const double* heights, size_t count);
// The same question over the list brought out at the edge.
ChromeHit chromeRevealHitTest(const ChromeRect& list, const ChromeRect buttons[3], float x, float y, const double* heights, size_t count);
