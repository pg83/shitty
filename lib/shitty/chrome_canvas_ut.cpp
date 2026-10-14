/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "chrome_canvas.h"

#include <std/tst/ut.h>

using namespace stl;

namespace {
    const ChromeColor red{1, 0, 0, 1};

    u8 alphaAt(const ChromeCanvas& canvas, u32 x, u32 y) {
        return canvas.pixel(x, y)[3];
    }
}

STD_TEST_SUITE(ChromeCanvas) {
    // A rounded rectangle covers its inside fully, leaves the corner outside
    // its arc empty, and shades the pixels its edge crosses.
    STD_TEST(ARoundedRectIsFilledInsideAndClearOutsideItsCorners) {
        ChromeCanvas canvas(40, 40);
        canvas.fillRoundedRect(4, 4, 32, 32, 10, red);
        // Premise: the probes are inside, in the corner outside the arc, and
        // on the straight edge - three places the shape treats differently.
        STD_INSIST(chromeRoundedRectDistance(20.5f, 20.5f, 4, 4, 32, 32, 10) < -1);
        STD_INSIST(chromeRoundedRectDistance(4.5f, 4.5f, 4, 4, 32, 32, 10) > 1);
        STD_INSIST(alphaAt(canvas, 20, 20) == 255);
        STD_INSIST(alphaAt(canvas, 4, 4) == 0);
        // A square corner would have covered it.
        STD_INSIST(alphaAt(canvas, 5, 5) == 0);
        STD_INSIST(alphaAt(canvas, 20, 4) == 255);
        STD_INSIST(alphaAt(canvas, 20, 3) == 0);
        // Premultiplied, B G R A: red lands in the third byte.
        STD_INSIST(canvas.pixel(20, 20)[2] == 255 && canvas.pixel(20, 20)[0] == 0);
    }

    // Half covered is half blended: an edge through the middle of a pixel.
    STD_TEST(AnEdgeThroughAPixelCoversHalfOfIt) {
        ChromeCanvas canvas(10, 10);
        canvas.fillRoundedRect(0, 0, 4.5f, 10, 0, red);
        const u8 edge = alphaAt(canvas, 4, 5);
        STD_INSIST(edge > 110 && edge < 145);
        STD_INSIST(alphaAt(canvas, 3, 5) == 255);
        STD_INSIST(alphaAt(canvas, 5, 5) == 0);
    }

    // Translucent over translucent composites, premultiplied.
    STD_TEST(ShapesCompositeOverWhatIsThere) {
        ChromeCanvas canvas(4, 4);
        canvas.fillRoundedRect(0, 0, 4, 4, 0, ChromeColor{0, 0, 1, 0.5f});
        canvas.fillRoundedRect(0, 0, 4, 4, 0, ChromeColor{1, 0, 0, 0.5f});
        const u8* p = canvas.pixel(1, 1);
        // 0.5 + 0.5 * 0.5 alpha; red 0.5, blue 0.25, premultiplied.
        STD_INSIST(p[3] >= 190 && p[3] <= 192);
        STD_INSIST(p[2] >= 127 && p[2] <= 128);
        STD_INSIST(p[0] >= 63 && p[0] <= 64);
        // Premultiplied means no channel exceeds alpha.
        STD_INSIST(p[2] <= p[3] && p[0] <= p[3]);
    }

    // A shadow is solid under its shape and fades over the blur outside it.
    STD_TEST(AShadowFadesOutsideItsShape) {
        ChromeCanvas canvas(60, 60);
        canvas.shadow(20, 20, 20, 20, 4, 12, ChromeColor{0, 0, 0, 1});
        const u8 inside = alphaAt(canvas, 30, 30);
        const u8 near = alphaAt(canvas, 30, 42);
        const u8 far = alphaAt(canvas, 30, 50);
        const u8 beyond = alphaAt(canvas, 30, 55);
        STD_INSIST(inside == 255);
        STD_INSIST(near < inside && near > far);
        STD_INSIST(far > 0);
        STD_INSIST(beyond == 0);
    }

    // A punched hole takes back what was drawn, and only inside the shape.
    STD_TEST(APunchClearsTheShapeAndKeepsTheRest) {
        ChromeCanvas canvas(30, 30);
        canvas.fillRoundedRect(0, 0, 30, 30, 0, red);
        canvas.punchRoundedRect(10, 10, 10, 10, 3);
        STD_INSIST(alphaAt(canvas, 15, 15) == 0);
        STD_INSIST(alphaAt(canvas, 5, 5) == 255);
        // The hole's corner is round: its square corner keeps some colour.
        STD_INSIST(alphaAt(canvas, 10, 10) > 0);
    }

    // A stroke is a band inside the edge, with the middle left alone.
    STD_TEST(AStrokeIsABandInsideTheEdge) {
        ChromeCanvas canvas(30, 30);
        canvas.strokeRoundedRect(5, 5, 20, 20, 4, 2, red);
        STD_INSIST(alphaAt(canvas, 15, 5) == 255);
        STD_INSIST(alphaAt(canvas, 15, 6) == 255);
        STD_INSIST(alphaAt(canvas, 15, 8) == 0);
        STD_INSIST(alphaAt(canvas, 15, 15) == 0);
    }

    // A mask lands where it is put, clipped on the right.
    STD_TEST(AMaskIsClippedAtItsRightEdge) {
        ChromeCanvas canvas(10, 4);
        const u8 mask[8] = {255, 255, 255, 255, 255, 255, 255, 255};
        canvas.blendMask(2, 1, mask, 8, 1, 8, red, 6);
        STD_INSIST(alphaAt(canvas, 1, 1) == 0);
        STD_INSIST(alphaAt(canvas, 2, 1) == 255);
        STD_INSIST(alphaAt(canvas, 5, 1) == 255);
        STD_INSIST(alphaAt(canvas, 6, 1) == 0);
        STD_INSIST(alphaAt(canvas, 3, 0) == 0);
    }
}
