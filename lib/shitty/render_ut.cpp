/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render.h"

#include "render_blend.h"

#include <lib/vterm/terminal_types.h>

#include <std/tst/ut.h>

using namespace stl;

STD_TEST_SUITE(Renderer) {
    STD_TEST(PacksCellAttributeBits) {
        TerminalCell cell{};
        cell.bold = true;
        cell.italic = true;
        cell.underline_style = 1;
        cell.inverse = true;
        cell.wrap = true;
        cell.faint = true;
        cell.blink = true;
        cell.conceal = true;
        cell.strike = true;
        cell.overline = true;
        cell.underline_style = 5;
        cell.dwidth = true;
        cell.dwidth_cont = true;

        const u32 expected = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (5u << 13) | (1u << 16) | (1u << 17);
        STD_INSIST(Renderer::cellAttributes(cell) == expected);
    }
}

// T10. The premultiplication hazard, pinned by arithmetic rather than by
// eye. Every number below is written out rather than recomputed from the
// same expression the code uses, which is the difference between a test
// and a restatement.
STD_TEST_SUITE(BackgroundBlending) {
    // The percentage the user types, as the alpha the blending wants.
    // 100 landing on exactly 255 is the load-bearing one: it is the
    // default of a fork whose upstream has no such option.
    STD_TEST(ThePercentageBecomesAnAlphaAndAHundredIsWholly255) {
        STD_INSIST(backgroundAlphaFromPercent(100) == 255);
        STD_INSIST(backgroundAlphaFromPercent(0) == 0);
        STD_INSIST(backgroundAlphaFromPercent(50) == 128);
        STD_INSIST(backgroundAlphaFromPercent(25) == 64);
        // Above the range the option already refuses; the clamp is here
        // so a caller that reaches this with garbage cannot produce an
        // alpha larger than opaque.
        STD_INSIST(backgroundAlphaFromPercent(65535) == 255);
    }

    // The whole default: at 100 nothing in this file may change a single
    // byte of what the renderer drew before it existed.
    STD_TEST(AnOpaqueBackgroundPassesThroughByteForByte) {
        for (u32 channel = 0; channel <= 255; ++channel) {
            STD_INSIST(premultiplyChannel((u8)(channel), 255) == (u8)(channel));
        }
        const Color foreground{250, 200, 10};
        const Color background{16, 32, 48};
        for (u32 coverage = 0; coverage <= 255; ++coverage) {
            const BlendedPixel blended = blendOverBackground(foreground, background, (u8)(coverage), 255);
            STD_INSIST(blended.alpha == 255);
            STD_INSIST(blended.color.red == mixChannel(foreground.red, background.red, (u8)(coverage)));
            STD_INSIST(blended.color.green == mixChannel(foreground.green, background.green, (u8)(coverage)));
            STD_INSIST(blended.color.blue == mixChannel(foreground.blue, background.blue, (u8)(coverage)));
        }
    }

    // The case that separates premultiplied from merely labelled. A
    // half-transparent white background is 128, not 255: writing 255 and
    // attaching alpha 128 is what a compositor reads as a background
    // twice as bright as asked, and it is the light rim around dark text.
    STD_TEST(AHalfTransparentBackgroundIsMultipliedDownNotMerelyLabelled) {
        const Color white{255, 255, 255};
        STD_INSIST(premultiplyChannel(255, 128) == 128);
        STD_INSIST(premultiply(white, 128).red == 128);

        // Nothing but background: the pixel is the background, multiplied
        // down, at the background's alpha.
        const BlendedPixel bare = blendOverBackground(Color{0, 0, 0}, white, 0, 128);
        STD_INSIST(bare.color.red == 128);
        STD_INSIST(bare.alpha == 128);

        // The edge of a black glyph over that background - the pixel the
        // halo lives on. 64 is the premultiplied answer; the straight one
        // is 127, and the two are far enough apart to be seen.
        const BlendedPixel edge = blendOverBackground(Color{0, 0, 0}, white, 128, 128);
        STD_INSIST(edge.color.red == 64);
        STD_INSIST(edge.alpha == 192);
        STD_INSIST(edge.color.red != mixChannel(0, 255, 128));

        // And the mirror image, so a sign error cannot pass: a white
        // glyph's edge over a half-transparent black background must be
        // darker than the straight arithmetic gives, not lighter.
        const Color black{0, 0, 0};
        const BlendedPixel mirror = blendOverBackground(white, black, 128, 128);
        STD_INSIST(mirror.color.red == 128);
        STD_INSIST(mirror.alpha == 192);
    }

    // Glyphs stay solid: a fully covered pixel is opaque whatever the
    // background's alpha, and it keeps the foreground's own colour.
    STD_TEST(FullCoverageIsOpaqueAndUnmultiplied) {
        const Color foreground{200, 100, 50};
        const Color background{255, 255, 255};
        const u8 alphas[] = {0, 1, 128, 254, 255};
        for (u8 alpha : alphas) {
            const BlendedPixel blended = blendOverBackground(foreground, background, 255, alpha);
            STD_INSIST(blended.alpha == 255);
            STD_INSIST(blended.color.red == foreground.red);
            STD_INSIST(blended.color.green == foreground.green);
            STD_INSIST(blended.color.blue == foreground.blue);
        }
    }

    // The alpha channel is a function of coverage and the background's
    // alpha alone. If a colour ever reaches it, a dark theme and a light
    // one stop being equally transparent.
    STD_TEST(TheAlphaDoesNotDependOnAnyColour) {
        const Color pairs[][2] = {
            {{0, 0, 0}, {255, 255, 255}},
            {{255, 255, 255}, {0, 0, 0}},
            {{10, 240, 90}, {90, 10, 240}},
        };
        const u8 coverages[] = {0, 1, 77, 128, 254, 255};
        for (u8 coverage : coverages) {
            const u8 expected = blendOverBackground(pairs[0][0], pairs[0][1], coverage, 128).alpha;
            for (const Color* pair : pairs) {
                STD_INSIST(blendOverBackground(pair[0], pair[1], coverage, 128).alpha == expected);
            }
            // And it is not simply constant, which is what a broken
            // expression collapsing to one term would look like.
            STD_INSIST(blendOverBackground(pairs[0][0], pairs[0][1], coverage, 255).alpha == 255);
        }
        // The two ends, named: no coverage leaves the background's own
        // alpha, full coverage is opaque.
        STD_INSIST(blendOverBackground(pairs[0][0], pairs[0][1], 0, 128).alpha == 128);
        STD_INSIST(blendOverBackground(pairs[0][0], pairs[0][1], 255, 128).alpha == 255);
    }
}
