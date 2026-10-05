/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

// How opaque a piece of window chrome should paint itself, so that the
// title bar and the sidebar cannot disagree about it.
//
// S10 is why this is a header and not a static in each file. The title
// bar grew this rule in wave 10 and the sidebar did not, which is the
// whole of the defect a user found by looking at the window: a
// see-through terminal with a solid panel down its left edge. Copying
// the four lines across would have fixed the picture and left two places
// deciding one thing - and the next option, or the next reload path,
// would only have to reach one of them to put the two back out of step.

// AppKit is NOT imported here, and must be imported by the including
// file before this header. Both callers bracket that import with
// `#define Point MacLegacyPoint` / `#define Rect MacLegacyRect`, because
// AppKit drags in Carbon's Point and Rect and the project has its own
// of both. A header that imported AppKit for itself would land outside
// those brackets and break the file that included it - which is exactly
// what it did on the first build of S10.

#include "composer.h"
#include "options.h"
#include "render_blend.h"

namespace {

    // Asked of the live content layer, and deliberately not of
    // -backgroundOpacity nor of window.opaque.
    //
    // Not the option: the window's transparency is established once, at
    // creation (platform_cocoa.mm), so a reload can move the option
    // under chrome that cannot follow it.
    //
    // Not window.opaque either, though this file's neighbour uses that
    // flag for a different question: a quick window rounds its corners
    // by going transparent while its background stays perfectly opaque,
    // and chrome faded on that account would be a defect. The content
    // layer is marked non-opaque by exactly one decision, and it is this
    // one.
    inline CGFloat windowTintAlpha(const Composer& composer, NSWindow* window) {
        NSView* const content = window == nil ? nil : window.contentView;
        if (content == nil || content.layer == nil || content.layer.opaque) {
            return 1.0;
        }
        return backgroundAlphaFromPercent(composer.opts->backgroundOpacity) / 255.0;
    }

    // Whether the window is drawn as two layers - the terminal a panel over
    // the window's own surface (ui_sidebar_tabs.mm). Here beside the tint
    // for the reason the tint is here: two modules answer to it, the
    // sidebar that builds the layers and the title bar that has to stand
    // its strip down for them, and a condition copied into each would be
    // one reload away from the two disagreeing. On for a Cocoa window with
    // the sidebar and -layeredWindow, and only when the content layer can
    // be seen through: outside the panel the renderer clears to nothing,
    // and an opaque CAMetalLayer would turn that black.
    inline bool layeredWindowShown(const Composer& composer, NSWindow* window) {
        return composer.opts->layeredWindow && composer.opts->sidebarTabs && window != nil && windowTintAlpha(composer, window) < 1.0;
    }

}
