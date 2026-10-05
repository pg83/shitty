/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "vt_geometry.h"

#include <std/alg/minmax.h>

// A1: the two directions of one formula - how many cells a rectangle
// holds once a pair of insets is taken out of it, and how many pixels a
// grid of that many cells needs with those insets put back. Every place
// that used to spell `2 * borderPixels` around a grid goes through
// these, so a per-side inset cannot be dropped, doubled, or applied to
// the wrong axis in one caller and not the next: the width pair reads
// `left`/`right` and the height pair reads `top`/`bottom`, never `2 *`
// one of them.
//
// Deliberately a file of its own rather than more of vt_geometry.h.
// That header is documented as the geometry of one *pane* (A8/T5.1),
// and three of the four core callers of this arithmetic are asking
// about the *window*: the XTWINOPS reports divide the window's pixels
// by the window's content insets. Putting window arithmetic inside a
// header that says "pane" would blur exactly the distinction A10
// guards. What is here is neither: it is arithmetic over a rectangle
// and a pair of insets, indifferent to whose they are - so the caller
// keeps saying which, and the sum is written once.
//
// The insets, whoever's they are, arrive already in physical pixels.
// The points-to-pixels conversion is the embedder's and stays there
// (A1); nothing below scales anything.
//
// lib/shitty/grid_geometry.h is the same four functions over the
// embedder's Insets, and is now a thin forwarding layer over these -
// its ninety-one call sites keep their spelling and this stays the one
// place the formula lives.

// What is left of a span once the two insets on its axis are taken out.
// Saturating rather than wrapping: a reserve wider than the surface is
// a reserve claimed before the first resize, and an unsigned difference
// would turn an empty content box into an enormous one.
inline u32 vtGridContentWidth(u32 pixelWidth, const VtInsets& insets) {
    const u32 reserved = (u32)(insets.left) + insets.right;
    return pixelWidth > reserved ? pixelWidth - reserved : 0;
}

inline u32 vtGridContentHeight(u32 pixelHeight, const VtInsets& insets) {
    const u32 reserved = (u32)(insets.top) + insets.bottom;
    return pixelHeight > reserved ? pixelHeight - reserved : 0;
}

// At least one cell, always: a grid of zero columns has no cursor
// position to be at, and every caller of these would have to invent the
// same floor if this did not carry it.
inline u32 vtGridColumns(u32 pixelWidth, const VtInsets& insets, u16 cellPixelWidth) {
    return stl::max<u32>(1, vtGridContentWidth(pixelWidth, insets) / stl::max<u32>(1, cellPixelWidth));
}

inline u32 vtGridRows(u32 pixelHeight, const VtInsets& insets, u16 cellPixelHeight) {
    return stl::max<u32>(1, vtGridContentHeight(pixelHeight, insets) / stl::max<u32>(1, cellPixelHeight));
}

// The inverse. `columns` of zero asks for just the reserve, which is
// what a resize increment or a minimum-size base wants.
inline u32 vtGridPixelWidth(u32 columns, const VtInsets& insets, u16 cellPixelWidth) {
    return (u32)(insets.left) + insets.right + columns * cellPixelWidth;
}

inline u32 vtGridPixelHeight(u32 rows, const VtInsets& insets, u16 cellPixelHeight) {
    return (u32)(insets.top) + insets.bottom + rows * cellPixelHeight;
}
