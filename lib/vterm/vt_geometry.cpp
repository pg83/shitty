/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "vt_geometry.h"

#include "vt_grid.h"
#include "vt_host.h"

#include <std/alg/minmax.h>
#include <std/dbg/assert.h>

using namespace stl;

void VtGeometry::setCellPixelSize(u16 width, u16 height) {
    STD_ASSERT(width != 0);
    STD_ASSERT(height != 0);
    if (cellPixelWidth == width && cellPixelHeight == height) {
        return;
    }
    cellPixelWidth = width;
    cellPixelHeight = height;
}

void VtGeometry::resize(u16 pixelWidth_, u16 pixelHeight_, VtHost* host) {
    STD_ASSERT(cellPixelWidth != 0);
    STD_ASSERT(cellPixelHeight != 0);

    // A1: per side, never `2 *` one of them. The pane resize() serves is
    // the one that fills the window, so its rectangle is the surface and
    // the only thing between the two is this pane's own border.
    //
    // The formula itself is vt_grid.h's, and it is called rather than
    // repeated: this function and lib/shitty/grid_geometry.h used to
    // carry the same three lines apiece, which is two places for a
    // per-side inset to be dropped in one and not the other.
    const u32 contentWidth = vtGridContentWidth(pixelWidth_, insets);
    const u32 contentHeight = vtGridContentHeight(pixelHeight_, insets);
    const u16 columns_ = (u16)(vtGridColumns(pixelWidth_, insets, cellPixelWidth));
    const u16 rows_ = (u16)(vtGridRows(pixelHeight_, insets, cellPixelHeight));

    if (columns == columns_ && rows == rows_ && pixelWidth == pixelWidth_ && pixelHeight == pixelHeight_) {
        return;
    }

    columns = columns_;
    rows = rows_;
    pixelWidth = pixelWidth_;
    pixelHeight = pixelHeight_;
    // The extent the pointer mappings clamp against, committed beside
    // the grid it was divided into rather than recovered from it: the
    // division just threw away whatever did not fill a whole cell.
    width = (i32)(contentWidth);
    height = (i32)(contentHeight);

    if (host != nullptr) {
        host->resized();
    }
}
