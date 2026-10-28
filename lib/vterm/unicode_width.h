/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

#include <stddef.h>

struct UnicodeCodepointProperties;

struct CodepointProperties {
    u8 width;
    // Every pair in this set has a boundary. Hangul syllables and emoji
    // bases qualify too: joining Jamo, marks and ZWJ still take the full
    // breaker, which retains the actual preceding codepoint.
    bool simpleGrapheme;
};

enum class GraphemeWidthEffect {
    Unchanged,
    Wide,
    Narrow,
};

// One cluster of a span's flat codepoint string: codepoints
// [begin, begin + count) covering cells grid cells.
struct SpanCluster {
    size_t begin = 0;
    size_t count = 0;
    u16 cells = 0;
};

// Which Unicode version's widths the cells emulate, resolved once into
// Options. East Asian Width reclassifications younger than the level
// are undone, so the grid agrees with the libc the shells at the pty's
// far end measure with; a level of 0 runs the full current tables.
// The visible format controls are the axis the level cannot express:
// libc implementations disagree about them independently of any Unicode
// version, so the constructor asks this system's wcwidth about each one
// and the instance carries the answers.
class UnicodeWidths {
public:
    explicit UnicodeWidths(u32 level);

    CodepointProperties codepointProperties(u32 codepoint) const;
    int codepointWidth(u32 codepoint) const;
    GraphemeWidthEffect graphemeWidthEffect(u32 previous, u32 codepoint) const;
    // As above for a caller that already holds codepointProperties(
    // codepoint).width from its own cache: skips the width derivation,
    // the expensive half of the answer, on every combining codepoint.
    GraphemeWidthEffect graphemeWidthEffect(u32 previous, u32 codepoint, u8 width) const;
    // Iterates the grapheme clusters of a span string with their grid
    // widths, by the same width rules the terminal used to place the
    // cells. position advances past the cluster; returns false at the
    // end of the string.
    bool nextSpanCluster(const u32* codepoints, size_t count, size_t& position, SpanCluster& cluster) const;
    // The effective version, for feature reporting: the configured
    // level, or the bundled Unicode major when running the full tables.
    u32 level() const;

private:
    CodepointProperties codepointProperties(u32 codepoint, const UnicodeCodepointProperties& property) const;

    u32 level_;
    // One bit per unicodeSpacingFormatControls() entry: whether this
    // system's wcwidth gives that format control a cell.
    u64 spacingFormats_;
};
