/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render_arena.h"

#include <std/tst/ut.h>

using namespace stl;

namespace {
    static bool sends(const ArenaCopy& copy, size_t from, size_t to) {
        return copy.from == from && copy.to == to;
    }

    static bool sendsNothing(const ArenaCopy& copy) {
        return copy.from == copy.to;
    }
}

STD_TEST_SUITE(ArenaMirror) {
    // The path every frame takes: the arena grew by what the shaper
    // appended, and that tail is all the device is owed.
    STD_TEST(SendsTheTailWhileTheGenerationHolds) {
        ArenaMirror mirror;

        STD_INSIST(sends(mirror.plan(7, 100), 0, 100));
        STD_INSIST(sends(mirror.plan(7, 140), 100, 140));
        STD_INSIST(sendsNothing(mirror.plan(7, 140)));
    }

    // A3, the whole point of keeping a generation here at all: when the
    // shaper collects or the font changes, every strip offset in the
    // arena means something else, so the tail from last frame names
    // bytes that are no longer there. Compare only the sizes and this
    // frame sends nothing at all, leaving the device drawing the glyphs
    // of the arena that died.
    STD_TEST(SendsTheWholeArenaWhenTheGenerationMoves) {
        ArenaMirror mirror;

        mirror.plan(7, 100);
        STD_INSIST(sends(mirror.plan(8, 100), 0, 100));
        // And the frame after it is a tail again, against the new
        // generation - the mirror adopts the generation it was handed
        // rather than re-sending forever.
        STD_INSIST(sends(mirror.plan(8, 160), 100, 160));
    }

    // An arena cannot shrink inside a generation - it only grows until
    // it dies whole - so a smaller arena under the same number means a
    // generation went by unseen. Trusting the number would copy a tail
    // that starts past the end of what the shaper holds.
    STD_TEST(SendsTheWholeArenaWhenItShrankUnderOneGeneration) {
        ArenaMirror mirror;

        mirror.plan(7, 100);
        STD_INSIST(sends(mirror.plan(7, 40), 0, 40));
    }

    // The caller that could not make the copies says so, and the next
    // frame starts from nothing rather than from a mirror describing a
    // device buffer that was replaced or never written.
    STD_TEST(ResetOwesTheWholeArenaAgain) {
        ArenaMirror mirror;

        mirror.plan(7, 100);
        mirror.reset();
        // The same generation, and still owed whole: reset is about the
        // device having lost the bytes, not about the strips moving.
        STD_INSIST(sends(mirror.plan(7, 100), 0, 100));
    }

    // A window whose panes have shaped nothing yet - every split starts
    // there - must not ask for a copy of an empty arena.
    STD_TEST(AnEmptyArenaOwesNothing) {
        ArenaMirror mirror;

        STD_INSIST(sendsNothing(mirror.plan(7, 0)));
        STD_INSIST(sends(mirror.plan(7, 60), 0, 60));
    }
}
