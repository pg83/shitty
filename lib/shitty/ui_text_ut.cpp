/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_text.h"

#include "font_embedded.h"

#include <std/tst/ut.h>

using namespace stl;

#if defined(HAVE_FREETYPE) && defined(HAVE_HARFBUZZ)
namespace {
    bool openEmbedded(UiText& text, float pixels) {
        const void* data = nullptr;
        unsigned long size = 0;
        embeddedMonoFont(data, size);
        return text.openMemory(data, size, 0, pixels);
    }

    // The leftmost and rightmost columns with any ink on row band [y0, y1).
    bool inkSpan(const ChromeCanvas& canvas, u32 y0, u32 y1, u32& first, u32& last) {
        bool any = false;
        for (u32 x = 0; x < canvas.width(); ++x) {
            for (u32 y = y0; y < y1; ++y) {
                if (canvas.pixel(x, y)[3] != 0) {
                    if (!any) {
                        first = x;
                    }
                    last = x;
                    any = true;
                    break;
                }
            }
        }
        return any;
    }
}

STD_TEST_SUITE(UiText) {
    // A longer string is wider, by the advances of what was added.
    STD_TEST(TextIsMeasuredByItsAdvances) {
        UiText text;
        STD_INSIST(openEmbedded(text, 14));
        const float one = text.measure(StringView(u8"a"));
        const float five = text.measure(StringView(u8"aaaaa"));
        STD_INSIST(one > 4 && one < 14);
        STD_INSIST(five > one * 4.9f && five < one * 5.1f);
        STD_INSIST(text.ascent() > 0 && text.descent() > 0);
    }

    // Drawn where it fits, the whole string is there; where it does not,
    // it stops inside the width and ends in an ellipsis.
    STD_TEST(TextPastItsWidthEndsInAnEllipsis) {
        UiText text;
        STD_INSIST(openEmbedded(text, 14));
        const StringView sample(u8"kibomibo@machine:~/Projects/shitty");
        const float full = text.measure(sample);
        // Premise: the width given is well short of what the string needs.
        const float room = 80;
        STD_INSIST(full > room * 2);

        ChromeCanvas wide(400, 30);
        const float drawnWide = text.draw(wide, 2, 20, sample, ChromeColor{1, 1, 1, 1}, 390);
        STD_INSIST(drawnWide > full - 0.5f && drawnWide < full + 0.5f);

        ChromeCanvas narrow(400, 30);
        const float drawnNarrow = text.draw(narrow, 2, 20, sample, ChromeColor{1, 1, 1, 1}, room);
        STD_INSIST(drawnNarrow <= room);
        u32 first = 0;
        u32 last = 0;
        STD_INSIST(inkSpan(narrow, 0, 30, first, last));
        STD_INSIST(last < 2 + (u32)(room));
        // The ellipsis sits on the baseline: its dots are ink low on the
        // line, right at the end of what was drawn.
        u32 dotFirst = 0;
        u32 dotLast = 0;
        STD_INSIST(inkSpan(narrow, 17, 21, dotFirst, dotLast));
        STD_INSIST(dotLast + 12 > last);
    }

    // Colour is the colour asked for.
    STD_TEST(TextIsDrawnInItsColour) {
        UiText text;
        STD_INSIST(openEmbedded(text, 20));
        ChromeCanvas canvas(60, 30);
        text.draw(canvas, 2, 22, StringView(u8"M"), ChromeColor{0, 0, 1, 1}, 50);
        bool solid = false;
        for (u32 y = 0; y < 30 && !solid; ++y) {
            for (u32 x = 0; x < 60; ++x) {
                const u8* p = canvas.pixel(x, y);
                if (p[3] == 255) {
                    STD_INSIST(p[0] == 255 && p[1] == 0 && p[2] == 0);
                    solid = true;
                    break;
                }
            }
        }
        STD_INSIST(solid);
    }
}
#endif
