/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "unicode_width.h"

#include "unicode.h"
#include "grapheme.h"

#include <wchar.h>

namespace {
    static bool isDefaultWideCjk(u32 codepoint) {
        // UAX #11 section 6.1 assigns Wide to unassigned codepoints in blocks
        // reserved for CJK ideographs. The base property table deliberately
        // keeps the historical width-one fallback for unassigned characters,
        // so retain the Unicode default for these holes here.
        return (codepoint >= 0x3400 && codepoint <= 0x4dbf) || (codepoint >= 0x4e00 && codepoint <= 0x9fff) || (codepoint >= 0xf900 && codepoint <= 0xfaff) || (codepoint >= 0x20000 && codepoint <= 0x2fffd) || (codepoint >= 0x30000 && codepoint <= 0x3fffd);
    }

    static u64 probeSpacingFormats() {
        size_t count = 0;
        const u32* const controls = unicodeSpacingFormatControls(count);
        u64 mask = 0;
        for (size_t index = 0; index < count; ++index) {
            if (wcwidth((wchar_t)(controls[index])) > 0) {
                mask |= (u64)(1) << index;
            }
        }
        return mask;
    }

    static size_t spacingFormatIndex(const u32* controls, size_t count, u32 codepoint) {
        size_t first = 0;
        size_t last = count;
        while (first < last) {
            const size_t middle = (first + last) / 2;
            if (controls[middle] < codepoint) {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        return first;
    }
}

UnicodeWidths::UnicodeWidths(u32 level)
    : level_(level)
    , spacingFormats_(probeSpacingFormats())
{
}

u32 UnicodeWidths::level() const {
    return level_ != 0 ? level_ : unicodeVersion();
}

CodepointProperties UnicodeWidths::codepointProperties(u32 codepoint) const {
    return codepointProperties(codepoint, unicodeCodepointProperties(codepoint));
}

CodepointProperties UnicodeWidths::codepointProperties(u32 codepoint, const UnicodeCodepointProperties& property) const {
    int width = property.width;
    if (codepoint >= 0x1160 && codepoint <= 0x11ff) {
        // Medial and trailing Hangul Jamo combine with the leading Jamo and
        // do not advance a terminal cursor independently.
        width = 0;
    } else if (width == 1 && (isDefaultWideCjk(codepoint) || (codepoint >= 0x1f1e6 && codepoint <= 0x1f1ff))) {
        width = 2;
    }
    if (property.category == GeneralCategory::Format) {
        size_t count = 0;
        const u32* const controls = unicodeSpacingFormatControls(count);
        const size_t index = spacingFormatIndex(controls, count, codepoint);
        if (index < count && controls[index] == codepoint) {
            width = (int)((spacingFormats_ >> index) & 1);
        }
    }
    if (width == 2 && level_ != 0 && level_ < 16 && codepoint < 0x20000) {
        // A lowered level undoes the East Asian Width reclassifications
        // younger than it: the 15.1 trigram batch, and below 9 the emoji
        // batch too. Only this cold path pays; every caller sits behind
        // a per-terminal property cache.
        if (unicodeWideSince16(codepoint) || (level_ < 9 && unicodeWideSince9(codepoint))) {
            width = 1;
        }
    }
    return {
        .width = (u8)(width),
        .simpleGrapheme = (property.graphemeClass == GraphemeClass::Other || property.graphemeClass == GraphemeClass::HangulLv || property.graphemeClass == GraphemeClass::HangulLvt || property.graphemeClass == GraphemeClass::ExtendedPictographic) && property.indicConjunctClass == IndicConjunctClass::None,
    };
}

int UnicodeWidths::codepointWidth(u32 codepoint) const {
    return codepointProperties(codepoint).width;
}

GraphemeWidthEffect UnicodeWidths::graphemeWidthEffect(u32 previous, u32 codepoint) const {
    return graphemeWidthEffect(previous, codepoint, (u8)(codepointProperties(codepoint).width));
}

GraphemeWidthEffect UnicodeWidths::graphemeWidthEffect(u32 previous, u32 codepoint, u8 width) const {
    if (codepoint == 0xfe0f || codepoint == 0xfe0e) {
        const UnicodeCodepointProperties previousProperties = unicodeCodepointProperties(previous);
        if (codepoint == 0xfe0f && previousProperties.widensWithVs16) {
            return GraphemeWidthEffect::Wide;
        }
        if (codepoint == 0xfe0e && previousProperties.narrowsWithVs15) {
            return GraphemeWidthEffect::Narrow;
        }
    }

    // Spacing combining marks have positive advance inside a cluster even
    // though their standalone wcwidth is zero.  Viramas and invisible
    // stackers are the exception: they request conjunct formation and the
    // following consonant is what widens the cluster.
    const UnicodeCodepointProperties properties = unicodeCodepointProperties(codepoint);
    if (!properties.virama && (width > 0 || properties.category == GeneralCategory::SpacingMark)) {
        return GraphemeWidthEffect::Wide;
    }
    return GraphemeWidthEffect::Unchanged;
}

bool UnicodeWidths::nextSpanCluster(const u32* codepoints, size_t count, size_t& position, SpanCluster& cluster) const {
    if (position >= count) {
        return false;
    }
    GraphemeBreaker breaker;
    cluster.begin = position;
    u32 previous = codepoints[position];
    breaker.breakBefore(previous, codepointProperties(previous).simpleGrapheme);
    int width = codepointWidth(previous);
    ++position;
    while (position < count) {
        const u32 codepoint = codepoints[position];
        if (breaker.breakBefore(codepoint, codepointProperties(codepoint).simpleGrapheme)) {
            break;
        }
        switch (graphemeWidthEffect(previous, codepoint)) {
            case GraphemeWidthEffect::Wide:
                width = 2;
                break;
            case GraphemeWidthEffect::Narrow:
                width = 1;
                break;
            case GraphemeWidthEffect::Unchanged:
                break;
        }
        previous = codepoint;
        ++position;
    }
    cluster.count = position - cluster.begin;
    cluster.cells = (u16)(width < 1 ? 1 : width > 2 ? 2 : width);
    return true;
}
