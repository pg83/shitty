/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "grid_geometry.h"

#include <std/tst/ut.h>

using namespace stl;

namespace {
    // A1: four different, non-zero sides. Production still asks for a
    // uniform border on all four, so only insets built here can tell an
    // Insets apart from the scalar it replaced.
    //
    // The values are chosen so that every wrong formula lands on a
    // different cell count than the right one - see the mutant lines in
    // the tests below, which spell each wrong formula out. That needs
    // every side to be at least one glyph, and the two sides of an axis
    // to differ by more than one glyph, or the division rounds the
    // mistake away and the test stops being a test.
    constexpr Insets asymmetric{
        .top = 25,
        .right = 21,
        .bottom = 45,
        .left = 11,
    };
    constexpr u16 glyphWidth = 8;
    constexpr u16 glyphHeight = 16;

    constexpr u32 horizontal = (u32)(asymmetric.left) + asymmetric.right; // 32
    constexpr u32 vertical = (u32)(asymmetric.top) + asymmetric.bottom;   // 70

    constexpr u32 tenColumns = horizontal + 10 * glyphWidth; // 112
    constexpr u32 fourRows = vertical + 4 * glyphHeight;     // 134
}

STD_TEST_SUITE(GridGeometry) {
    STD_TEST(TakesTheHorizontalPairOutOfTheWidth) {
        STD_INSIST(gridColumns(tenColumns, asymmetric, glyphWidth) == 10);

        // Every way of getting the horizontal reserve wrong answers with
        // a different column count on this surface.
        STD_INSIST((tenColumns - asymmetric.left) / glyphWidth != 10);
        STD_INSIST((tenColumns - asymmetric.right) / glyphWidth != 10);
        STD_INSIST((tenColumns - 2 * asymmetric.left) / glyphWidth != 10);
        STD_INSIST((tenColumns - 2 * asymmetric.right) / glyphWidth != 10);
        STD_INSIST((tenColumns - vertical) / glyphWidth != 10);
    }

    STD_TEST(TakesTheVerticalPairOutOfTheHeight) {
        STD_INSIST(gridRows(fourRows, asymmetric, glyphHeight) == 4);

        STD_INSIST((fourRows - asymmetric.top) / glyphHeight != 4);
        STD_INSIST((fourRows - asymmetric.bottom) / glyphHeight != 4);
        STD_INSIST((fourRows - 2 * asymmetric.top) / glyphHeight != 4);
        STD_INSIST((fourRows - 2 * asymmetric.bottom) / glyphHeight != 4);
        STD_INSIST((fourRows - horizontal) / glyphHeight != 4);
    }

    // Neither axis can answer for the other: the same surface size gives
    // a different count on each.
    STD_TEST(KeepsTheAxesApart) {
        STD_INSIST(gridColumns(tenColumns, asymmetric, glyphWidth) != gridRows(tenColumns, asymmetric, glyphHeight));
        STD_INSIST(gridRows(fourRows, asymmetric, glyphHeight) != gridColumns(fourRows, asymmetric, glyphWidth));
    }

    // A partial cell at the far edge is not a cell - which is also what
    // pins the reserve to the exact pixel rather than the right ballpark.
    STD_TEST(CountsOnlyWholeCells) {
        STD_INSIST(gridColumns(tenColumns - 1, asymmetric, glyphWidth) == 9);
        STD_INSIST(gridRows(fourRows - 1, asymmetric, glyphHeight) == 3);
    }

    // A surface smaller than its own insets still reports a usable grid;
    // a terminal with zero columns has no meaning downstream.
    STD_TEST(ClampsToOneCell) {
        STD_INSIST(gridColumns(1, asymmetric, glyphWidth) == 1);
        STD_INSIST(gridRows(1, asymmetric, glyphHeight) == 1);
        STD_INSIST(gridColumns(0, asymmetric, glyphWidth) == 1);
        STD_INSIST(gridRows(0, asymmetric, glyphHeight) == 1);
    }

    // Glyph metrics are zero until the first font is rasterized, and a
    // size can arrive before that.
    STD_TEST(SurvivesUnmeasuredGlyphs) {
        STD_INSIST(gridColumns(tenColumns, asymmetric, 0) >= 1);
        STD_INSIST(gridRows(fourRows, asymmetric, 0) >= 1);
    }

    STD_TEST(PutsTheSamePairBackIntoThePixels) {
        STD_INSIST(gridPixelWidth(10, asymmetric, glyphWidth) == tenColumns);
        STD_INSIST(gridPixelHeight(4, asymmetric, glyphHeight) == fourRows);

        // Zero cells is the bare reserve - a resize increment's base and
        // a minimum window size are both asked for that way.
        STD_INSIST(gridPixelWidth(0, asymmetric, glyphWidth) == horizontal);
        STD_INSIST(gridPixelHeight(0, asymmetric, glyphHeight) == vertical);

        // Whichever pair the width takes, the height takes the other.
        STD_INSIST(gridPixelWidth(0, asymmetric, glyphWidth) != gridPixelHeight(0, asymmetric, glyphHeight));
    }

    // The pair is the contract: a surface sized for N cells reports N
    // cells back, on both axes, with insets that share no value.
    STD_TEST(RoundTripsThroughBothDirections) {
        for (u32 cells = 1; cells <= 200; ++cells) {
            STD_INSIST(gridColumns(gridPixelWidth(cells, asymmetric, glyphWidth), asymmetric, glyphWidth) == cells);
            STD_INSIST(gridRows(gridPixelHeight(cells, asymmetric, glyphHeight), asymmetric, glyphHeight) == cells);
        }
    }

    // The pair the window requests take. Production hands these to
    // requestMinimumSize()/requestResizeUnit()/requestResize(), and the
    // headless platform used by every test drops all three on the floor -
    // so a transposed pair is unobservable at the call site, and this is
    // the only place it can be caught.
    STD_TEST(PairsTheWidthWithTheColumnsAndTheHeightWithTheRows) {
        const GridPixelSize smallest = gridPixelSize(1, 1, asymmetric, glyphWidth, glyphHeight);
        STD_INSIST(smallest.width == gridPixelWidth(1, asymmetric, glyphWidth));
        STD_INSIST(smallest.height == gridPixelHeight(1, asymmetric, glyphHeight));

        // The transposition that survives everywhere else: one cell wide
        // and one cell tall are different numbers of pixels, so a pair
        // handed over the wrong way round lands somewhere else.
        STD_INSIST(smallest.width != smallest.height);
        STD_INSIST(smallest.width != gridPixelHeight(1, asymmetric, glyphHeight));
        STD_INSIST(smallest.height != gridPixelWidth(1, asymmetric, glyphWidth));

        // The bare reserve - the base a resize increment counts from.
        const GridPixelSize reserve = gridPixelSize(0, 0, asymmetric, glyphWidth, glyphHeight);
        STD_INSIST(reserve.width == horizontal);
        STD_INSIST(reserve.height == vertical);

        // Neither cell count may answer for the other either.
        const GridPixelSize grid = gridPixelSize(2, 5, asymmetric, glyphWidth, glyphHeight);
        STD_INSIST(grid.width == horizontal + 2 * glyphWidth);
        STD_INSIST(grid.height == vertical + 5 * glyphHeight);
        STD_INSIST(grid.width != horizontal + 5 * glyphWidth);
        STD_INSIST(grid.height != vertical + 2 * glyphHeight);
    }

    // A cell's top-left pixel: the input method's anchor, and where the
    // reference renderer starts a glyph. `left` is the column's side and
    // `top` is the row's, and this is the one place that says so.
    STD_TEST(AnchorsACellAtItsOwnSideOfEachAxis) {
        const CellOrigin home = cellOrigin(0, 0, asymmetric, glyphWidth, glyphHeight);
        STD_INSIST(home.x == asymmetric.left);
        STD_INSIST(home.y == asymmetric.top);

        // Every other side is a different number, so borrowing one for
        // the wrong axis is visible here.
        STD_INSIST(home.x != asymmetric.top);
        STD_INSIST(home.x != asymmetric.right);
        STD_INSIST(home.y != asymmetric.left);
        STD_INSIST(home.y != asymmetric.bottom);

        const CellOrigin cell = cellOrigin(3, 1, asymmetric, glyphWidth, glyphHeight);
        STD_INSIST(cell.x == (i32)(asymmetric.left + 3 * glyphWidth));
        STD_INSIST(cell.y == (i32)(asymmetric.top + 1 * glyphHeight));

        // Column and row do not answer for each other, and neither does
        // one glyph metric for the other.
        STD_INSIST(cell.x != (i32)(asymmetric.left + 1 * glyphWidth));
        STD_INSIST(cell.y != (i32)(asymmetric.top + 3 * glyphHeight));
        STD_INSIST(cell.x != (i32)(asymmetric.left + 3 * glyphHeight));
        STD_INSIST(cell.y != (i32)(asymmetric.top + 1 * glyphWidth));
    }

    // The uniform border production still asks for has to keep behaving
    // exactly as the scalar did - this is the "nothing visibly changed"
    // half of T4.
    STD_TEST(MatchesTheScalarBorderWhenUniform) {
        constexpr u16 border = 7;
        constexpr Insets uniform{border, border, border, border};

        STD_INSIST(gridColumns(2 * border + 10 * glyphWidth + 3, uniform, glyphWidth) == 10);
        STD_INSIST(gridRows(2 * border + 4 * glyphHeight + 5, uniform, glyphHeight) == 4);
        STD_INSIST(gridPixelWidth(10, uniform, glyphWidth) == 2 * border + 10 * glyphWidth);
        STD_INSIST(gridPixelHeight(4, uniform, glyphHeight) == 2 * border + 4 * glyphHeight);
    }
}
