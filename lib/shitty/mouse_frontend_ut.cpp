/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include <lib/vterm/mouse_frontend.h>

#include "grid_geometry.h"
#include "pane_layout.h"
#include "options.h"

#include <std/alg/minmax.h>
#include <std/mem/obj_pool.h>
#include <std/tst/ut.h>

using namespace stl;

namespace {
    // grid_geometry.h speaks the embedder's Insets and a MouseGeometry
    // carries the core's VtInsets since T5.1. The same four numbers, so
    // the conversion is by name and never by position - which is also
    // what stops it from silently surviving the day one of the two
    // structs reorders its sides.
    constexpr Insets asInsets(const VtInsets& insets) {
        return Insets{.top = insets.top, .right = insets.right, .bottom = insets.bottom, .left = insets.left};
    }

    // A1: a content box whose four reserves share no value, so a helper
    // that took `top` where it meant `left` answers differently instead
    // of accidentally right. 194 x 146 pixels hold exactly 20 x 6 cells
    // of 8 x 16 once the reserves are out.
    constexpr MouseGeometry asymmetric{
        .framebufferWidth = 194,
        .framebufferHeight = 146,
        .insets = {.top = 17, .right = 23, .bottom = 33, .left = 11},
        .contentWidth = 194 - 11 - 23,
        .contentHeight = 146 - 17 - 33,
        .glyphWidth = 8,
        .glyphHeight = 16,
    };

    // The same four reserves around a surface three pixels wider and
    // three taller, so the content box ends in the middle of a cell on
    // both axes - 163 x 99 pixels over an 8 x 16 glyph.
    constexpr MouseGeometry partialCell{
        .framebufferWidth = 197,
        .framebufferHeight = 149,
        .insets = {.top = 17, .right = 23, .bottom = 33, .left = 11},
        .contentWidth = 197 - 11 - 23,
        .contentHeight = 149 - 17 - 33,
        .glyphWidth = 8,
        .glyphHeight = 16,
    };

    // A8: the same window, with the pane starting three columns in and
    // two rows down from the window's own content origin. The two offsets
    // are neither equal nor multiples of one another, so a mapping that
    // took the x origin for the y one answers differently rather than
    // accidentally right - and one that dropped the origin altogether
    // lands three cells and two rows off.
    //
    // T10 gives it the extent A8 could not: ten columns and three rows,
    // which is a box running from (35, 49) to (115, 97) - well short of
    // the window's own (171, 113) on both axes, so a mapping that reached
    // for the window's far inset instead of the pane's extent hands out
    // seven columns and one row this pane does not have. Neither extent
    // divides into the other's, and neither equals an inset.
    constexpr MouseGeometry panePlaced{
        .framebufferWidth = 194,
        .framebufferHeight = 146,
        .insets = {.top = 17, .right = 23, .bottom = 33, .left = 11},
        .paneOriginX = 24,
        .paneOriginY = 32,
        .contentWidth = 80,
        .contentHeight = 48,
        .glyphWidth = 8,
        .glyphHeight = 16,
    };
}

// A1 acceptance: with a uniform border - which is every build until T5 and
// T6 reserve a side - the four pointer mappings must answer exactly what
// the scalar-border code answered. The originals are transcribed here as
// the reference; the sweep below runs both over every pixel of a surface
// and twenty more in each direction.
namespace scalarBorder {
    constexpr int border = 7;
    constexpr int fbWidth = 2 * border + 20 * 8;
    constexpr int fbHeight = 2 * border + 6 * 16;
    constexpr int glyphWidth = 8;
    constexpr int glyphHeight = 16;
    constexpr int columns = 20;
    constexpr int rows = 6;

    constexpr MouseGeometry geometry{
        .framebufferWidth = fbWidth,
        .framebufferHeight = fbHeight,
        .insets = {border, border, border, border},
        .contentWidth = fbWidth - 2 * border,
        .contentHeight = fbHeight - 2 * border,
        .glyphWidth = glyphWidth,
        .glyphHeight = glyphHeight,
    };

    // resolveHyperlink, before T4.
    static bool oldCell(int pixelX, int pixelY, u16& column, u16& row) {
        if (pixelX < border || pixelY < border || pixelX >= fbWidth - border || pixelY >= fbHeight - border) {
            return false;
        }
        column = (u16)((pixelX - border) / glyphWidth);
        row = (u16)((pixelY - border) / glyphHeight);
        return true;
    }

    // selectionPoint, before T4.
    static Point oldSelection(int pX, int pY) {
        const int contentWidth = max(0, fbWidth - 2 * border);
        const int contentHeight = max(1, fbHeight - 2 * border);
        pX = min(max(0, pX - border), contentWidth);
        pY = min(max(0, pY - border), contentHeight - 1);
        return Point(min(pX / glyphWidth, columns), min(pY / glyphHeight, rows - 1));
    }

    // currentSelectionAutoscrollDirection, before T4.
    static int oldAutoscroll(int pointerY) {
        const int top = border;
        const int bottom = max(top, fbHeight - border - 1);
        if (pointerY <= top) {
            return -1;
        }
        if (pointerY >= bottom) {
            return 1;
        }
        return 0;
    }

    // mouseProtocolPoint, before T4.
    static MouseProtocolPoint oldProtocol(MouseTrackingEnc encoding, int pixelX, int pixelY) {
        const int contentWidth = max(1, fbWidth - 2 * border);
        const int contentHeight = max(1, fbHeight - 2 * border);
        if (encoding == MouseTrackingEnc::SGRPixels) {
            return {
                min(max(pixelX - border + 1, 1), contentWidth),
                min(max(pixelY - border + 1, 1), contentHeight),
            };
        }
        const int c = max(1, contentWidth / max(1, glyphWidth));
        const int r = max(1, contentHeight / max(1, glyphHeight));
        return {
            min(max((pixelX - border) / max(1, glyphWidth) + 1, 1), c),
            min(max((pixelY - border) / max(1, glyphHeight) + 1, 1), r),
        };
    }
}

STD_TEST_SUITE(MouseFrontend) {
    STD_TEST(ConvertsLogicalCoordinatesToFramebufferPixels) {
        STD_INSIST(mouseFramebufferCoordinate(10.25, 2.0) == 21);
        STD_INSIST(mouseFramebufferCoordinate(-10.25, 2.0) == -21);
        STD_INSIST(mouseFramebufferCoordinate(10.0, 0.5) == 10);
        STD_INSIST(mouseFramebufferCoordinate(__builtin_inf(), 2.0) == 0);
        STD_INSIST(mouseFramebufferCoordinate(10.0, __builtin_nan("")) == 0);
    }

    STD_TEST(ConvertsPixelsToCellCoordinates) {
        const MouseGeometry geometry{
            .framebufferWidth = 84,
            .framebufferHeight = 68,
            .insets = {2, 2, 2, 2},
            .contentWidth = 80,
            .contentHeight = 64,
            .glyphWidth = 8,
            .glyphHeight = 16,
        };

        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 2, 2, geometry).column == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 2, 2, geometry).row == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 81, 65, geometry).column == 10);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 81, 65, geometry).row == 4);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, -100, -100, geometry).column == 1);

        // The first pixel of a cell belongs to that cell, the last one to
        // the same cell — no drift at the boundary.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 9, 2, geometry).column == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 10, 2, geometry).column == 2);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 2, 17, geometry).row == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 2, 18, geometry).row == 2);
    }

    STD_TEST(SgrPixelsUseContentPixelCoordinates) {
        const MouseGeometry geometry{
            .framebufferWidth = 104,
            .framebufferHeight = 54,
            .insets = {2, 2, 2, 2},
            .contentWidth = 100,
            .contentHeight = 50,
            .glyphWidth = 8,
            .glyphHeight = 16,
        };

        const MouseProtocolPoint first = mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 2, 2, geometry);
        const MouseProtocolPoint last = mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 101, 51, geometry);

        STD_INSIST(first.column == 1);
        STD_INSIST(first.row == 1);
        STD_INSIST(last.column == 100);
        STD_INSIST(last.row == 50);
    }

    // The composer is still where a pointer geometry comes from - as the
    // pane that fills the window, plus the window's own surface and
    // glyph - and the uniform border it reports today has to arrive on
    // all four sides.
    STD_TEST(ReadsItsGeometryFromTheComposer) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        options.border = 5;
        composer.setOptions(&options);
        composer.geometry.setCellPixelSize(8, 16);
        composer.resize(194, 146);

        const MouseGeometry geometry = mouseGeometry(windowPane(composer), composer.geometry);

        STD_INSIST(geometry.framebufferWidth == 194);
        STD_INSIST(geometry.framebufferHeight == 146);
        STD_INSIST(geometry.glyphWidth == 8);
        STD_INSIST(geometry.glyphHeight == 16);
        STD_INSIST(geometry.insets.left == composer.borderPixels());
        STD_INSIST(geometry.insets.top == composer.borderPixels());
        STD_INSIST(geometry.insets.right == composer.borderPixels());
        STD_INSIST(geometry.insets.bottom == composer.borderPixels());
    }

    // Hyperlink hover: the cell under the pointer, or nothing when the
    // pointer is over a reserve rather than over text.
    STD_TEST(FindsTheCellUnderThePointer) {
        u16 column = 0;
        u16 row = 0;

        STD_INSIST(mouseCell(11, 17, asymmetric, column, row));
        STD_INSIST(column == 0 && row == 0);

        STD_INSIST(mouseCell(170, 112, asymmetric, column, row));
        STD_INSIST(column == 19 && row == 5);
    }

    // Each edge of the content box has to come from its own side. Every
    // probe here is answered one way by the right side and the other way
    // by the side a mixup would reach for.
    STD_TEST(TakesEachEdgeFromItsOwnSide) {
        u16 column = 0;
        u16 row = 0;

        // Left of `top`, right of `left`: inside, and only `left` says so.
        STD_INSIST(mouseCell(15, 50, asymmetric, column, row));
        STD_INSIST(column == 0);
        // Above `top` but below `left`: outside, and only `top` says so.
        STD_INSIST(!mouseCell(50, 15, asymmetric, column, row));
        // Past the `bottom` edge measured across, still inside the width.
        STD_INSIST(mouseCell(165, 50, asymmetric, column, row));
        STD_INSIST(column == 19);
        // Past the `bottom` edge, short of where `right` would put it.
        STD_INSIST(!mouseCell(50, 115, asymmetric, column, row));

        // The offsets, too: the column counts from `left`, the row from
        // `top`, and neither from the other.
        STD_INSIST(mouseCell(19, 29, asymmetric, column, row));
        STD_INSIST(column == 1);
        STD_INSIST(row == 0);
    }

    // A pointer over a reserve leaves the cell alone rather than naming
    // a nearby one - the caller tells "no cell" from "cell 0,0" that way.
    STD_TEST(LeavesTheCellAloneOutsideTheContent) {
        u16 column = 7;
        u16 row = 9;

        STD_INSIST(!mouseCell(0, 0, asymmetric, column, row));
        STD_INSIST(!mouseCell(193, 145, asymmetric, column, row));
        STD_INSIST(column == 7 && row == 9);
    }

    STD_TEST(ReportsMouseProtocolCellsFromTheLeftAndTopReserves) {
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 19, 29, asymmetric).column == 2);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 19, 29, asymmetric).row == 1);

        // Far outside, the report clamps to the grid the content box
        // holds - 20 x 6 here, which neither reserve pair alone gives.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 10000, 10000, asymmetric).column == 20);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 10000, 10000, asymmetric).row == 6);

        // Pixel reporting counts from the same corner, in pixels.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 170, 112, asymmetric).column == 160);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 170, 112, asymmetric).row == 96);
    }

    STD_TEST(ClampsSelectionEndpointsIntoTheContentBox) {
        STD_INSIST(mouseSelectionCell(11, 17, asymmetric, 20, 6) == Point(0, 0));
        STD_INSIST(mouseSelectionCell(27, 45, asymmetric, 20, 6) == Point(2, 1));

        // The open end of an extent is allowed one column past the last
        // cell, but never one row past the last row.
        STD_INSIST(mouseSelectionCell(10000, 10000, asymmetric, 20, 6) == Point(20, 5));
        STD_INSIST(mouseSelectionCell(-10000, -10000, asymmetric, 20, 6) == Point(0, 0));
    }

    STD_TEST(ScrollsWhenTheDragLeavesTheContentBox) {
        STD_INSIST(mouseAutoscrollDirection(13, asymmetric) == -1);
        STD_INSIST(mouseAutoscrollDirection(17, asymmetric) == -1);
        STD_INSIST(mouseAutoscrollDirection(18, asymmetric) == 0);
        STD_INSIST(mouseAutoscrollDirection(111, asymmetric) == 0);
        STD_INSIST(mouseAutoscrollDirection(112, asymmetric) == 1);
        STD_INSIST(mouseAutoscrollDirection(115, asymmetric) == 1);
    }

    // The acceptance criterion for T4 in one test: without new options the
    // pointer lands in exactly the cell it landed in before, on every pixel
    // of the surface and twenty past every edge, at a border of seven.
    STD_TEST(EveryPixelMapsWhereItUsedTo) {
        size_t compared = 0;
        for (int y = -20; y < scalarBorder::fbHeight + 20; ++y) {
            for (int x = -20; x < scalarBorder::fbWidth + 20; ++x) {
                u16 oldColumn = 0xffff;
                u16 oldRow = 0xffff;
                u16 newColumn = 0xffff;
                u16 newRow = 0xffff;
                const bool oldInside = scalarBorder::oldCell(x, y, oldColumn, oldRow);
                const bool newInside = mouseCell(x, y, scalarBorder::geometry, newColumn, newRow);
                STD_INSIST(oldInside == newInside);
                STD_INSIST(oldColumn == newColumn);
                STD_INSIST(oldRow == newRow);

                STD_INSIST(scalarBorder::oldSelection(x, y) == mouseSelectionCell(x, y, scalarBorder::geometry, scalarBorder::columns, scalarBorder::rows));

                const MouseProtocolPoint oldSgr = scalarBorder::oldProtocol(MouseTrackingEnc::SGR, x, y);
                const MouseProtocolPoint newSgr = mouseProtocolPoint(MouseTrackingEnc::SGR, x, y, scalarBorder::geometry);
                STD_INSIST(oldSgr.column == newSgr.column && oldSgr.row == newSgr.row);

                const MouseProtocolPoint oldPix = scalarBorder::oldProtocol(MouseTrackingEnc::SGRPixels, x, y);
                const MouseProtocolPoint newPix = mouseProtocolPoint(MouseTrackingEnc::SGRPixels, x, y, scalarBorder::geometry);
                STD_INSIST(oldPix.column == newPix.column && oldPix.row == newPix.row);

                ++compared;
            }
            STD_INSIST(scalarBorder::oldAutoscroll(y) == mouseAutoscrollDirection(y, scalarBorder::geometry));
        }
        STD_INSIST(compared == (size_t)(scalarBorder::fbWidth + 40) * (size_t)(scalarBorder::fbHeight + 40));
    }

    // R3-test, the plan's "pointer mapping at non-zero reserves", done
    // exhaustively: every pixel of a content box whose four reserves
    // share no value, plus eight past each edge. Each probe is checked
    // against the sides spelled out by hand, so a helper that reached
    // for the wrong side answers differently rather than accidentally
    // right - and against grid_geometry, so the cell the pointer names
    // and the grid the layout counts cannot drift apart.
    STD_TEST(EveryPixelOfAnAsymmetricContentBoxAnswersFromItsOwnSide) {
        const int left = asymmetric.insets.left;
        const int top = asymmetric.insets.top;
        const int firstOutsideX = asymmetric.framebufferWidth - asymmetric.insets.right;
        const int firstOutsideY = asymmetric.framebufferHeight - asymmetric.insets.bottom;
        const u32 columns = gridColumns(asymmetric.framebufferWidth, asInsets(asymmetric.insets), (u16)(asymmetric.glyphWidth));
        const u32 rows = gridRows(asymmetric.framebufferHeight, asInsets(asymmetric.insets), (u16)(asymmetric.glyphHeight));

        STD_INSIST(columns == 20);
        STD_INSIST(rows == 6);

        size_t inside = 0;
        for (int y = -8; y < asymmetric.framebufferHeight + 8; ++y) {
            for (int x = -8; x < asymmetric.framebufferWidth + 8; ++x) {
                const bool expected = x >= left && x < firstOutsideX && y >= top && y < firstOutsideY;

                u16 column = 0xffff;
                u16 row = 0xffff;
                STD_INSIST(mouseCell(x, y, asymmetric, column, row) == expected);
                if (!expected) {
                    // A pointer over a reserve names no cell at all.
                    STD_INSIST(column == 0xffff && row == 0xffff);
                    continue;
                }
                ++inside;

                STD_INSIST(column == (x - left) / asymmetric.glyphWidth);
                STD_INSIST(row == (y - top) / asymmetric.glyphHeight);

                // The cell the pointer names exists in the grid the
                // layout counts out of the same insets.
                STD_INSIST(column < columns);
                STD_INSIST(row < rows);

                // The other three mappings agree with it inside the box:
                // the protocol reports the same cell one-based, and a
                // selection endpoint lands on it.
                const MouseProtocolPoint sgr = mouseProtocolPoint(MouseTrackingEnc::SGR, x, y, asymmetric);
                STD_INSIST(sgr.column == column + 1);
                STD_INSIST(sgr.row == row + 1);
                STD_INSIST(mouseSelectionCell(x, y, asymmetric, (int)(columns), (int)(rows)) == Point(column, row));
            }

            // Autoscroll takes the top reserve and the bottom one, and
            // neither of the horizontal pair.
            const int expectedScroll = y <= top ? -1 : (y >= firstOutsideY - 1 ? 1 : 0);
            STD_INSIST(mouseAutoscrollDirection(y, asymmetric) == expectedScroll);
        }

        STD_INSIST(inside == (size_t)(columns)*asymmetric.glyphWidth * (size_t)(rows)*asymmetric.glyphHeight);
    }

    // The columns and rows a caller passes are a second clamp, not a
    // restatement of the geometry: they come from the Composer's own
    // grid, which a caller may hold at a smaller size than the content
    // box would hold. Every probe here is answered one way by the
    // caller's grid and another by the box.
    STD_TEST(ClampsTheSelectionToTheGridTheCallerNames) {
        // The box holds 20 x 6; the caller names 12 x 3.
        STD_INSIST(mouseSelectionCell(10000, 10000, asymmetric, 12, 3) == Point(12, 2));
        STD_INSIST(mouseSelectionCell(170, 112, asymmetric, 12, 3) == Point(12, 2));

        // Inside the named grid the pixels still decide.
        STD_INSIST(mouseSelectionCell(27, 45, asymmetric, 12, 3) == Point(2, 1));

        // A one-row grid puts every endpoint on row zero, and a grid
        // with no columns has only the open end to offer.
        STD_INSIST(mouseSelectionCell(170, 112, asymmetric, 20, 1) == Point(19, 0));
        STD_INSIST(mouseSelectionCell(170, 112, asymmetric, 0, 6) == Point(0, 5));
    }

    // Characterization, not endorsement: when the content box ends in
    // the middle of a cell, the pixels of that sliver are inside the box
    // and name a cell the grid does not have. Callers range-check the
    // answer against their own grid - resolveHyperlink does - so this is
    // the contract they are checking against, and it predates A1.
    STD_TEST(NamesACellPastTheGridWhenTheContentBoxEndsMidCell) {
        STD_INSIST(gridColumns(partialCell.framebufferWidth, asInsets(partialCell.insets), (u16)(partialCell.glyphWidth)) == 20);
        STD_INSIST(gridRows(partialCell.framebufferHeight, asInsets(partialCell.insets), (u16)(partialCell.glyphHeight)) == 6);

        u16 column = 0;
        u16 row = 0;

        // The last pixel inside the box on each axis.
        STD_INSIST(mouseCell(173, 115, partialCell, column, row));
        STD_INSIST(column == 20);
        STD_INSIST(row == 6);

        // The mappings that carry their own clamp do not let it out.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 173, 115, partialCell).column == 20);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 173, 115, partialCell).row == 6);
        STD_INSIST(mouseSelectionCell(173, 115, partialCell, 20, 6) == Point(20, 5));
    }

    // A8: the pane's origin arrives as its own pair of numbers and stays
    // that way. The pane's border has to read back exactly as the
    // Composer reported it - an implementation that added the origin
    // into insets.left/top would answer every mapping below the same way
    // and still be wrong, because the border and the offset would no
    // longer be separable by the layer that owns each.
    STD_TEST(KeepsThePaneOriginApartFromItsOwnBorder) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        options.border = 5;
        composer.setOptions(&options);
        composer.geometry.setCellPixelSize(8, 16);
        composer.resize(194, 146);

        // Built by hand rather than through paneGeometry(): what is under
        // test is mouse_frontend's own arithmetic over an origin and an
        // extent, and a fixture that came out of the layout would only
        // ever offer the origins the layout happens to produce.
        const VtGeometry pane{
            .insets = vtInsets(composer.paneInsets()),
            .originX = 24,
            .originY = 32,
            .width = 80,
            .height = 48,
        };
        const MouseGeometry geometry = mouseGeometry(pane, composer.geometry);

        STD_INSIST(geometry.paneOriginX == 24);
        STD_INSIST(geometry.paneOriginY == 32);
        // T10: the extent arrives the same way and stays apart too - an
        // implementation that folded it into the insets would put the
        // pane's far edge at the window's.
        STD_INSIST(geometry.contentWidth == 80);
        STD_INSIST(geometry.contentHeight == 48);
        STD_INSIST(geometry.contentRight() == composer.borderPixels() + 24 + 80);
        STD_INSIST(geometry.contentBottom() == composer.borderPixels() + 32 + 48);
        STD_INSIST(geometry.insets.left == composer.borderPixels());
        STD_INSIST(geometry.insets.top == composer.borderPixels());
        STD_INSIST(geometry.insets.right == composer.borderPixels());
        STD_INSIST(geometry.insets.bottom == composer.borderPixels());
        STD_INSIST(geometry.contentLeft() == composer.borderPixels() + 24);
        STD_INSIST(geometry.contentTop() == composer.borderPixels() + 32);

        // The pane that fills the window starts where the window's own
        // content does, and its extent is the window's content box -
        // which is where the far edges of every mapping used to come
        // from unconditionally.
        const MouseGeometry whole = mouseGeometry(windowPane(composer), composer.geometry);
        STD_INSIST(whole.paneOriginX == 0);
        STD_INSIST(whole.paneOriginY == 0);
        STD_INSIST(whole.contentLeft() == composer.borderPixels());
        STD_INSIST(whole.contentTop() == composer.borderPixels());
        STD_INSIST(whole.contentRight() == 194 - composer.borderPixels());
        STD_INSIST(whole.contentBottom() == 146 - composer.borderPixels());
    }

    // The debt A8 left, retired rather than deleted. This test used to be
    // called TheFarEdgesAreStillTheWindowsWhileNoPaneHasAnExtent and read
    // the other way round: nothing handed out a pane's extent, so a pane
    // that began inside the window was told about every pixel up to the
    // window's own trailing inset - pixels that belong to whatever sits
    // to its right or below it. It was written as a test and not as a
    // comment so that the day someone gave a pane an extent, the old
    // answers would go red and force this decision to be made out loud.
    //
    // Here it is made: the far edges are the pane's, taken from the
    // extent beside the origin. Each assertion below is one the old code
    // answered the other way.
    STD_TEST(TheFarEdgesAreThePanesOwnOnceItHasAnExtent) {
        u16 column = 0;
        u16 row = 0;

        // panePlaced runs from (35, 49) to (115, 97); the window's own
        // content box runs to (171, 113). This pixel is between the two -
        // inside the window, past this pane - and used to name cell
        // (14, 3) of a pane that has ten columns and three rows.
        STD_INSIST(!mouseCell(150, 100, panePlaced, column, row));
        STD_INSIST(column == 0 && row == 0);

        // The last pixel that is still the pane's, and the first that is
        // not, on each axis.
        STD_INSIST(mouseCell(114, 96, panePlaced, column, row));
        STD_INSIST(column == 9);
        STD_INSIST(row == 2);
        STD_INSIST(!mouseCell(115, 96, panePlaced, column, row));
        STD_INSIST(!mouseCell(114, 97, panePlaced, column, row));

        // The clamps stop there too: ten columns and three rows, not the
        // seventeen and four that the distance to the window's far edge
        // used to give.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 10000, 10000, panePlaced).column == 10);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 10000, 10000, panePlaced).row == 3);

        // Autoscroll turns at the pane's bottom - 49 + 48 - 1 - and no
        // longer waits for the window's, which used to be 24 rows of
        // pixels further down.
        STD_INSIST(mouseAutoscrollDirection(95, panePlaced) == 0);
        STD_INSIST(mouseAutoscrollDirection(96, panePlaced) == 1);
        STD_INSIST(mouseAutoscrollDirection(112, panePlaced) == 1);
    }

    // Q1: both ends of every clamp are read off one surface - the near end
    // is the pane's origin, the far end is that origin plus the pane's
    // extent - so an extent is the difference of the two by construction.
    // Three of these four mappings used to subtract the pane's origin from
    // the pixel and then bound the result by the window's *content
    // extent*, which is measured from the window's origin: the two ends
    // came from two origins and the clamp overshot by exactly paneOriginX.
    //
    // panePlaced's box runs from (35, 49) to (115, 97): 80 px across and
    // 48 down, so 10 columns and 3 rows are all any mapping may hand out.
    // The mixed form answers the window's own 20 x 6 and puts the last
    // column at 35 + 20 * 8 = 195 - past the 194 px surface altogether.
    STD_TEST(ClampsStopAtThePanesFarEdgeCountedFromItsOwnOrigin) {
        const MouseProtocolPoint cell = mouseProtocolPoint(MouseTrackingEnc::SGR, 10000, 10000, panePlaced);
        STD_INSIST(cell.column == 10);
        STD_INSIST(cell.row == 3);

        // The same two numbers as a property rather than as constants: the
        // cell the clamp named has to begin inside the pane's box.
        STD_INSIST(panePlaced.contentLeft() + (cell.column - 1) * panePlaced.glyphWidth < panePlaced.contentRight());
        STD_INSIST(panePlaced.contentTop() + (cell.row - 1) * panePlaced.glyphHeight < panePlaced.contentBottom());

        const MouseProtocolPoint pixel = mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 10000, 10000, panePlaced);
        STD_INSIST(pixel.column == 80);
        STD_INSIST(pixel.row == 48);
        STD_INSIST(panePlaced.contentLeft() + pixel.column <= panePlaced.contentRight());
        STD_INSIST(panePlaced.contentTop() + pixel.row <= panePlaced.contentBottom());

        // A selection endpoint may land one column past the last cell -
        // that is what the open end of an extent means - and it counts
        // that from the pane as well: 10, not the window's 20.
        STD_INSIST(mouseSelectionCell(10000, 10000, panePlaced, 100, 100) == Point(10, 2));

        // mouseCell compared surface against surface all along; it is
        // asserted here so all four devices are stated in one place.
        u16 column = 0;
        u16 row = 0;
        STD_INSIST(mouseCell(114, 96, panePlaced, column, row));
        STD_INSIST(!mouseCell(115, 96, panePlaced, column, row));
        STD_INSIST(!mouseCell(114, 97, panePlaced, column, row));
        STD_INSIST(mouseAutoscrollDirection(96, panePlaced) == 1);
    }

    // A8: every pixel-to-cell mapping counts from the pane's origin, not
    // from the window's. Each probe here is answered one way with the
    // origin applied and another way without it, or with the two axes
    // exchanged.
    STD_TEST(EveryPointerMappingCountsFromThePaneOrigin) {
        u16 column = 0;
        u16 row = 0;

        // The pane's first cell: 11 + 24 across, 17 + 32 down.
        STD_INSIST(mouseCell(35, 49, panePlaced, column, row));
        STD_INSIST(column == 0);
        STD_INSIST(row == 0);
        STD_INSIST(mouseCell(43, 65, panePlaced, column, row));
        STD_INSIST(column == 1);
        STD_INSIST(row == 1);

        // One pixel short of the pane, but well inside the window's own
        // content box: no cell of this pane.
        STD_INSIST(!mouseCell(34, 49, panePlaced, column, row));
        STD_INSIST(!mouseCell(35, 48, panePlaced, column, row));

        // The protocol point, cell-encoded and pixel-encoded.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 35, 49, panePlaced).column == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 35, 49, panePlaced).row == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 40, 56, panePlaced).column == 6);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 40, 56, panePlaced).row == 8);

        // The selection endpoint.
        STD_INSIST(mouseSelectionCell(35, 49, panePlaced, 20, 6) == Point(0, 0));
        STD_INSIST(mouseSelectionCell(43, 65, panePlaced, 20, 6) == Point(1, 1));

        // Autoscroll: the top edge is the pane's, so a drag held at the
        // window's own top inset is already above this pane.
        STD_INSIST(mouseAutoscrollDirection(49, panePlaced) == -1);
        STD_INSIST(mouseAutoscrollDirection(50, panePlaced) == 0);
        STD_INSIST(mouseAutoscrollDirection(49, asymmetric) == 0);
    }

    STD_TEST(MapsModifiersAndButtons) {
        STD_INSIST(mouseProtocolModifiers(FrontendShift | FrontendControl | FrontendAlt) == (MouseShift | MouseControl | MouseAlt));
        STD_INSIST(mouseProtocolModifiers(FrontendShift | FrontendControl | FrontendAlt, false) == (MouseShift | MouseControl));
        STD_INSIST(mouseTerminalButton(0) == 1);
        STD_INSIST(mouseTerminalButton(2) == 2);
        STD_INSIST(mouseTerminalButton(1) == 3);
        STD_INSIST(mouseTerminalButton(3) == 8);
        STD_INSIST(mouseTerminalButton(6) == 11);
        STD_INSIST(mouseTerminalButton(-1) == 0);
    }

    STD_TEST(AppliesButtonReportingRules) {
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::Disabled, MouseEventType::Press, 1));
        STD_INSIST(mouseButtonReportAllowed(MouseTrackingMode::VT200, MouseEventType::Press, 1));
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::X10_Compat, MouseEventType::Release, 1));
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::VT200, MouseEventType::Release, 4));
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::VT200, MouseEventType::Press, 12));
    }

    STD_TEST(AccumulatesFractionalWheelSteps) {
        MouseWheelAccumulator wheel;

        STD_INSIST(wheel.consume(0.0, 0.4, false).y == 0);
        STD_INSIST(wheel.consume(0.0, 0.4, false).y == 0);
        STD_INSIST(wheel.consume(0.0, 0.4, false).y == 1);
        STD_INSIST(wheel.consume(0.0, -0.5, false).y == 0);
        STD_INSIST(wheel.consume(0.0, -0.8, false).y == -1);
    }

    STD_TEST(ResetsWheelRemaindersWhenModeChanges) {
        MouseWheelAccumulator wheel;
        wheel.consume(0.75, 0.75, false);

        const MouseWheelSteps reporting = wheel.consume(0.5, 0.5, true);

        STD_INSIST(reporting.x == 0);
        STD_INSIST(reporting.y == 0);
        STD_INSIST(wheel.consume(0.5, 0.5, true).x == 1);
        wheel.reset();
        STD_INSIST(wheel.consume(0.5, 0.5, true).x == 0);
    }

    STD_TEST(TracksPressedButtonsAndMotionButtonPriority) {
        MouseFrontendState state;
        state.updateButton(1, true);
        state.updateButton(2, true);
        state.updateButton(0, true);

        STD_INSIST(state.buttons() == 7);
        STD_INSIST(state.primaryButtonPressed());
        STD_INSIST(state.motionButton() == 1);

        state.updateButton(0, false);
        STD_INSIST(state.motionButton() == 2);
        state.clearButtons();
        STD_INSIST(state.buttons() == 0);
        STD_INSIST(!state.primaryButtonPressed());
    }

    STD_TEST(SuppressesProtocolDuringSelectionOrShift) {
        MouseFrontendState state;

        STD_INSIST(state.protocolActive(0, MouseTrackingMode::VT200));
        STD_INSIST(!state.protocolActive(FrontendShift, MouseTrackingMode::VT200));
        STD_INSIST(!state.protocolActive(0, MouseTrackingMode::Disabled));
        state.beginSelection();
        STD_INSIST(!state.protocolActive(0, MouseTrackingMode::VT200));
        state.endSelection();
        STD_INSIST(state.protocolActive(0, MouseTrackingMode::VT200));
    }

    STD_TEST(CountsOnlyNearbyRapidClicks) {
        MouseFrontendState state;

        STD_INSIST(state.registerClick(0, 10.0, 10.0, 1.0) == 1);
        STD_INSIST(state.registerClick(0, 12.0, 12.0, 1.4) == 2);
        STD_INSIST(state.registerClick(0, 12.0, 12.0, 1.8) == 3);
        STD_INSIST(state.registerClick(0, 20.0, 12.0, 2.0) == 1);
        STD_INSIST(state.registerClick(1, 20.0, 12.0, 2.1) == 1);
        STD_INSIST(state.registerClick(1, 20.0, 12.0, 1.0) == 1);
    }

    STD_TEST(DeduplicatesMotionWithinSameContext) {
        MouseFrontendState state;

        STD_INSIST(state.reportMotion(1, 1, MouseTrackingMode::VT200, MouseTrackingEnc::SGR, 1));
        STD_INSIST(!state.reportMotion(1, 1, MouseTrackingMode::VT200, MouseTrackingEnc::SGR, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200, MouseTrackingEnc::SGR, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::SGR, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::UTF8, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::UTF8, 2));
        state.resetMotion();
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::UTF8, 2));
    }

    // T5.1's whole reason for existing, asserted rather than described.
    //
    // Exactly one place is allowed to know how much window chrome takes
    // off the left, and that place is the embedder. What reaches a pane
    // is where the pane starts - the reserve is already spent inside the
    // origin - and what reaches it as insets is the border and only the
    // border. Two places knowing would not crash: the right pane of a
    // vertical split would simply be charged the sidebar a second time
    // and draw its text a sidebar's width to the right of where its own
    // pointer counts from.
    //
    // The mis-wiring this pins is a one-word substitution that compiles,
    // runs, and answers a plausible cell: paneGeometry() filling insets
    // from contentInsets() instead of paneInsets(). Under it, every
    // assertion below that names a left edge moves by exactly the
    // reserve, and the far pane's content box runs off the surface by
    // the same amount - checked here as pixels claimed, so the failure
    // is a count and not an opinion.
    //
    // Deliberately driven through the production functions - contentBox()
    // and paneGeometry(), the same two SessionSet::applyLayout() calls -
    // rather than through hand-built geometries, because what is under
    // test is which of Composer's three inset readings each one asks for.
    STD_TEST(TheChromeReserveIsCountedOnceOnTheWayIntoAPanesGeometry) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        options.border = 4;
        composer.setOptions(&options);
        composer.geometry.setCellPixelSize(8, 16);
        // A reserve on the left, which is the side an origin carries and
        // an inset must not: 30 pixels of it, wider than the 4-pixel
        // border and not a multiple of it, so neither can stand in for
        // the other and neither is a multiple of the 8-pixel glyph.
        composer.setChromeReserve(ChromeSide::Left, 30);
        composer.resize(200, 100);

        const u16 reserve = composer.chromeInsets().left;
        const u16 border = composer.paneInsets().left;
        STD_INSIST(reserve == 30);
        STD_INSIST(border == 4);
        // The substitution this test exists to catch is only visible
        // because these two differ.
        STD_INSIST(composer.contentInsets().left == reserve + border);

        PaneTree tree;
        tree.plant(1);
        STD_INSIST(tree.split(SplitDirection::Vertical, 2));
        Vector<PanePlacement> placements;
        tree.layout(contentBox(composer), 0, placements);
        STD_INSIST(placements.length() == 2);

        const PixelRect nearArea = placements[0].area;
        const PixelRect farArea = placements[1].area;
        // The box is the surface less the reserve, halved; the nearArea half
        // starts at the box's own origin and the farArea one after it.
        STD_INSIST(nearArea.x == 0);
        STD_INSIST(nearArea.width == 85);
        STD_INSIST(farArea.x == 85);
        STD_INSIST(farArea.width == 85);

        const VtGeometry nearPane = paneGeometry(composer, nearArea);
        const VtGeometry farPane = paneGeometry(composer, farArea);

        // What the core is told: a position that already carries the
        // reserve, and a border that never does.
        STD_INSIST(nearPane.originX == reserve);
        STD_INSIST(farPane.originX == reserve + farArea.x);
        STD_INSIST(nearPane.insets.left == border);
        STD_INSIST(farPane.insets.left == border);
        STD_INSIST(nearPane.insets.left != composer.contentInsets().left);

        const MouseGeometry nearPointer = mouseGeometry(nearPane, composer.geometry);
        const MouseGeometry farPointer = mouseGeometry(farPane, composer.geometry);

        // Where each pane's text starts, spelled out from Composer's own
        // numbers rather than from the geometry under test.
        STD_INSIST(nearPointer.contentLeft() == reserve + border);
        STD_INSIST(farPointer.contentLeft() == reserve + farArea.x + border);
        // And where it ends: inside the surface, both of them.
        STD_INSIST(nearPointer.contentRight() == reserve + nearArea.width - border);
        STD_INSIST(farPointer.contentRight() == reserve + farArea.x + farArea.width - border);
        STD_INSIST(farPointer.contentRight() <= 200);

        // The sweep. Every pixel of the surface is offered to both panes
        // on the cursor row: no pixel may belong to two panes, no pixel
        // left of the reserve may belong to either, and each pane must
        // claim exactly the extent it was given. Charging the reserve
        // twice moves the farArea pane's box 30 pixels right, which drops 30
        // pixels off the end of the surface and shows up here as a count.
        size_t nearClaimed = 0;
        size_t farClaimed = 0;
        for (int x = 0; x < 200; ++x) {
            u16 column = 0;
            u16 row = 0;
            const bool inNear = mouseCell(x, nearPointer.contentTop(), nearPointer, column, row);
            const bool inFar = mouseCell(x, farPointer.contentTop(), farPointer, column, row);
            STD_INSIST(!(inNear && inFar));
            if (inNear || inFar) {
                STD_INSIST(x >= reserve);
            }
            nearClaimed += inNear ? 1 : 0;
            farClaimed += inFar ? 1 : 0;
        }
        STD_INSIST(nearClaimed == (size_t)(nearPane.width));
        STD_INSIST(farClaimed == (size_t)(farPane.width));
        STD_INSIST(nearClaimed == 85 - 2 * border);
        STD_INSIST(farClaimed == 85 - 2 * border);
    }
}
