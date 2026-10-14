/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_geometry.h"

#include <std/str/view.h>
#include <std/tst/ut.h>

using namespace stl;
using namespace plt;

STD_TEST_SUITE(QuickGeometry) {
    STD_TEST(DefaultStringParsesToTheOriginalUnconfigurablePlacement) {
        QuickGeometry geometry;

        STD_INSIST(parseQuickGeometry(StringView(u8"100%x40%+0+0"), geometry));
        STD_INSIST(geometry.width.percent);
        STD_INSIST(geometry.width.value == 100);
        STD_INSIST(geometry.height.percent);
        STD_INSIST(geometry.height.value == 40);
        STD_INSIST(!geometry.x.percent);
        STD_INSIST(geometry.x.value == 0);
        STD_INSIST(!geometry.y.percent);
        STD_INSIST(geometry.y.value == 0);
    }

    STD_TEST(AbsolutePixelsParse) {
        QuickGeometry geometry;

        STD_INSIST(parseQuickGeometry(StringView(u8"1200x600+100+50"), geometry));
        STD_INSIST(!geometry.width.percent);
        STD_INSIST(geometry.width.value == 1200);
        STD_INSIST(!geometry.height.percent);
        STD_INSIST(geometry.height.value == 600);
        STD_INSIST(!geometry.x.percent);
        STD_INSIST(geometry.x.value == 100);
        STD_INSIST(!geometry.y.percent);
        STD_INSIST(geometry.y.value == 50);
    }

    // Each of the four components picks its own unit independently -
    // this is the case that would break first if a future change
    // resolved the '%' suffix once for the whole string instead of per
    // component.
    STD_TEST(PercentAndPixelsMixFreely) {
        QuickGeometry geometry;

        STD_INSIST(parseQuickGeometry(StringView(u8"80%x500+10%+0"), geometry));
        STD_INSIST(geometry.width.percent);
        STD_INSIST(geometry.width.value == 80);
        STD_INSIST(!geometry.height.percent);
        STD_INSIST(geometry.height.value == 500);
        STD_INSIST(geometry.x.percent);
        STD_INSIST(geometry.x.value == 10);
        STD_INSIST(!geometry.y.percent);
        STD_INSIST(geometry.y.value == 0);
    }

    STD_TEST(EmptyStringIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(), geometry));
    }

    STD_TEST(GarbageIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"!!!garbage!!!"), geometry));
    }

    STD_TEST(MissingHeightIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"100%x"), geometry));
    }

    STD_TEST(MissingXAndYAreRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"100%x40%"), geometry));
    }

    STD_TEST(MissingYIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"100%x40%+0"), geometry));
    }

    // Width and height reject zero in either unit: a window with no
    // extent along an axis is never a useful outcome, unlike X/Y where
    // zero is the documented default offset.
    STD_TEST(ZeroWidthOrHeightIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"0x40%+0+0"), geometry));
        STD_INSIST(!parseQuickGeometry(StringView(u8"100%x0+0+0"), geometry));
        STD_INSIST(!parseQuickGeometry(StringView(u8"0%x40%+0+0"), geometry));
    }

    STD_TEST(ZeroOffsetIsAccepted) {
        QuickGeometry geometry;

        STD_INSIST(parseQuickGeometry(StringView(u8"100%x40%+0+0"), geometry));
    }

    STD_TEST(NegativeComponentIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"100%x40%+-5+0"), geometry));
        STD_INSIST(!parseQuickGeometry(StringView(u8"-100%x40%+0+0"), geometry));
    }

    STD_TEST(PercentOverOneHundredIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"101%x40%+0+0"), geometry));
        STD_INSIST(!parseQuickGeometry(StringView(u8"100%x40%+0+101%"), geometry));
    }

    STD_TEST(PixelOverflowIsRejected) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"70000x600+0+0"), geometry));
        STD_INSIST(!parseQuickGeometry(StringView(u8"18446744073709551616x600+0+0"), geometry));
    }

    // A percent sign is only meaningful as the last byte of a component;
    // one stuck in the middle is just an unrecognized digit to
    // parseU64's underlying accumulator.
    STD_TEST(PercentSignMustTrailTheComponent) {
        QuickGeometry geometry;

        STD_INSIST(!parseQuickGeometry(StringView(u8"10%0x40%+0+0"), geometry));
    }

    STD_TEST(OutIsUntouchedOnFailure) {
        QuickGeometry geometry;
        geometry.width.value = 7;

        STD_INSIST(!parseQuickGeometry(StringView(u8"garbage"), geometry));
        STD_INSIST(geometry.width.value == 7);
    }
}
