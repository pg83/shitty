/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

struct VtHost;

// A1: the air around one pane's text, per side, in physical pixels.
// Four sides and not a scalar, because the left of a window is not its
// right the moment anything reserves an edge - and a named type rather
// than four loose u16 arguments, because pairing a side with the wrong
// axis is the one mistake that compiles and then answers plausibly.
//
// Field order is deliberately the embedder's Insets order (composer.h),
// so carrying one across the boundary is a copy field for field and
// never a re-spelling by position.
//
// This is the *border* and nothing else. Whatever window chrome reserves
// - a sidebar, a titlebar strip - never reaches here: the reserve is
// already spent by the time the embedder cut the window into panes, and
// it comes to the core as originX/originY instead (A10). A core that
// also knew the reserve would be a second place that knows how much is
// taken on the left, and the two would disagree at the first vertical
// split - the right pane would be charged the sidebar a second time.
// Nothing crashes; the text just sits in the wrong column.
struct VtInsets {
    u16 top = 0;
    u16 right = 0;
    u16 bottom = 0;
    u16 left = 0;
};

// The grid geometry the terminal serves to its applications: the cell
// counts, the pixel quantities the protocols report (winsize, XTWINOPS,
// pixel mouse), and nothing an embedder would not have to answer for.
//
// A8/T5.1: one of these describes one *pane*. The embedder holds a
// second one for the window itself - its surface and the cell size the
// font gives it - and commits window changes through resize(); the
// panes it cuts out of that window each get their own, delivered
// through Vterm::paneResized(). The two are different instances on
// purpose: a pane's rectangle is the layout's and the surface is the
// window's, and an instance that carried both would have to be rewritten
// for every pane every time either moved.
struct VtGeometry {
    void setCellPixelSize(u16 width, u16 height);
    // Commits all fields, then tells the host - every terminal behind
    // the window must hear a geometry change, and only the embedder
    // knows them all.
    void resize(u16 pixelWidth, u16 pixelHeight, VtHost* host);

    u16 columns = 0;
    u16 rows = 0;
    // The pixel size of one cell - what the terminal reports to its
    // applications (CSI 16t, winsize, pixel mouse). The embedder derives
    // it from whatever it draws with; the core only serves it.
    //
    // The window's, not a pane's: every pane of a window draws with the
    // same font. A pane instance leaves it at zero and whoever needs it
    // reads the window's own instance, which is what keeps a font change
    // from having to reach every pane before a click lands right.
    u16 cellPixelWidth = 0;
    u16 cellPixelHeight = 0;
    // The whole drawing surface, likewise the window's and shared by
    // every pane on it.
    u16 pixelWidth = 0;
    u16 pixelHeight = 0;

    // A1: the border around this pane's text, on four sides. Replaces
    // upstream's scalar borderPixels, which could not tell a left from a
    // right and had no room for the asymmetry a sidebar creates. The
    // points-to-pixels conversion stays the embedder's, exactly as the
    // scalar's did.
    VtInsets insets;

    // A8: this pane's top-left on the surface, in physical pixels,
    // counted from (0, 0) of the surface itself - not from the window's
    // content box.
    //
    // That is the whole of what the core is told about window chrome:
    // whatever a sidebar or a titlebar strip reserved is already inside
    // this number, spent by the embedder when it cut the window into
    // panes. Add insets to it and you have where the text starts; there
    // is no third term, and no reserve to remember.
    i32 originX = 0;
    i32 originY = 0;

    // T10: how far this pane's *text* reaches from where the text
    // starts - the rectangle less this pane's own border, in physical
    // pixels. So the far edge is originX + insets.left + width, which is
    // the near edge plus the extent and never an inset arithmetic of its
    // own.
    //
    // Carried rather than recovered from columns x cellPixelWidth: a
    // division throws away whatever did not fill a whole cell, and the
    // pointer mappings have always counted that sliver as inside the
    // box, so the two numbers would disagree about the last few pixels
    // of every pane whose box does not divide evenly.
    i32 width = 0;
    i32 height = 0;
};
