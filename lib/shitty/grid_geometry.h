/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "composer.h"

#include <lib/vterm/vt_grid.h>

#include <std/alg/minmax.h>

// A1: the two directions of the same formula - how many cells a surface
// holds once its content insets are taken out, and how many pixels a
// grid of that many cells needs with those insets put back. Every place
// that used to spell `2 * borderPixels()` around a grid goes through
// these, so a per-side inset cannot be dropped, doubled, or applied to
// the wrong axis in one caller and not the next.
//
// The insets are per side (see Insets in composer.h) and in backing
// pixels, so the width pair reads `left`/`right` and the height pair
// reads `top`/`bottom` - never `2 *` one of them.
//
// The arithmetic itself moved into the core - lib/vterm/vt_grid.h, over
// the core's VtInsets - the day the window's insets started reaching
// the terminal through VtHost::contentInsets(). These four stay as the
// embedder's spelling of it, over the embedder's Insets: ninety-one
// call sites in lib/shitty and bin/main_fuzz keep the names and the
// argument type they already have, and there is still exactly one place
// the formula is written down.

inline u32 gridColumns(u32 pixelWidth, const Insets& insets, u16 glyphWidth) {
    return vtGridColumns(pixelWidth, vtInsets(insets), glyphWidth);
}

inline u32 gridRows(u32 pixelHeight, const Insets& insets, u16 glyphHeight) {
    return vtGridRows(pixelHeight, vtInsets(insets), glyphHeight);
}

// The inverse. `columns` of zero asks for just the reserve, which is
// what a resize increment or a minimum-size base wants.
inline u32 gridPixelWidth(u32 columns, const Insets& insets, u16 glyphWidth) {
    return vtGridPixelWidth(columns, vtInsets(insets), glyphWidth);
}

inline u32 gridPixelHeight(u32 rows, const Insets& insets, u16 glyphHeight) {
    return vtGridPixelHeight(rows, vtInsets(insets), glyphHeight);
}

// The two above, taken as a pair. Every window request names a width and
// a height, in that order, and nothing in the scalar helpers stops a
// caller from handing them over the wrong way round: the axes only
// differ once the insets do, and the reserves are zero until T5/T6, so
// a swapped pair is invisible everywhere in the tree today. Building the
// pair here means a swap is a change to a function tests can see, not an
// unobservable transposition at a call site.
struct GridPixelSize {
    u32 width = 0;
    u32 height = 0;
};

inline GridPixelSize gridPixelSize(u32 columns, u32 rows, const Insets& insets, u16 glyphWidth, u16 glyphHeight) {
    return GridPixelSize{gridPixelWidth(columns, insets, glyphWidth), gridPixelHeight(rows, insets, glyphHeight)};
}

// The top-left pixel of one cell on the surface, in backing pixels: where
// the glyph is drawn, and where the input method anchors its candidate
// window. `left` belongs to the column and `top` to the row - this is the
// one place that decides that, so taking `x` from the wrong side is a
// change to a tested function rather than to a line nothing reads.
struct CellOrigin {
    i32 x = 0;
    i32 y = 0;
};

inline CellOrigin cellOrigin(u32 column, u32 row, const Insets& insets, u16 glyphWidth, u16 glyphHeight) {
    return CellOrigin{(i32)(insets.left + column * glyphWidth), (i32)(insets.top + row * glyphHeight)};
}
