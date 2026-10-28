/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "grapheme.h"

#include "unicode.h"

namespace {
    static bool graphemeBreakSimple(GraphemeClass left, GraphemeClass right) {
        if (left == GraphemeClass::Cr && right == GraphemeClass::Lf) {
            return false;
        }
        if (left == GraphemeClass::Cr || left == GraphemeClass::Lf || left == GraphemeClass::Control) {
            return true;
        }
        if (right == GraphemeClass::Cr || right == GraphemeClass::Lf || right == GraphemeClass::Control) {
            return true;
        }
        if (left == GraphemeClass::HangulL && (right == GraphemeClass::HangulL || right == GraphemeClass::HangulV || right == GraphemeClass::HangulLv || right == GraphemeClass::HangulLvt)) {
            return false;
        }
        if ((left == GraphemeClass::HangulLv || left == GraphemeClass::HangulV) && (right == GraphemeClass::HangulV || right == GraphemeClass::HangulT)) {
            return false;
        }
        if ((left == GraphemeClass::HangulLvt || left == GraphemeClass::HangulT) && right == GraphemeClass::HangulT) {
            return false;
        }
        if (right == GraphemeClass::Extend || right == GraphemeClass::Zwj || right == GraphemeClass::SpacingMark || left == GraphemeClass::Prepend) {
            return false;
        }
        if (left == GraphemeClass::EmojiZwj && right == GraphemeClass::ExtendedPictographic) {
            return false;
        }
        if (left == GraphemeClass::RegionalIndicator && right == GraphemeClass::RegionalIndicator) {
            return false;
        }
        return true;
    }
}

bool emojiPresentation(u32 codepoint) {
    // The supplementary emoji planes render as emoji by default; the BMP
    // set is exactly the bases the variation-sequence registry lets VS15
    // downgrade to text.
    if (codepoint >= 0x1f000 && codepoint <= 0x1faff) {
        return true;
    }
    return unicodeCodepointProperties(codepoint).narrowsWithVs15;
}

bool GraphemeBreaker::breakBeforeSlow(u32 codepoint, bool simple) {
    const UnicodeCodepointProperties right = unicodeCodepointProperties(codepoint);
    return breakBeforeSlow(codepoint, simple, right.graphemeClass, right.indicConjunctClass);
}

bool GraphemeBreaker::breakBeforeSlow(u32 codepoint, bool simple, GraphemeClass rightClass, IndicConjunctClass rightIndic) {
    GraphemeClass stateClass;
    IndicConjunctClass stateIndic;
    if (state_ == 0) {
        const UnicodeCodepointProperties left = unicodeCodepointProperties((u32)(previous_));
        stateClass = left.graphemeClass;
        stateIndic = left.indicConjunctClass == IndicConjunctClass::Consonant ? IndicConjunctClass::Consonant : IndicConjunctClass::None;
    } else {
        stateClass = (GraphemeClass)((state_ & 0xff) - 1);
        stateIndic = (IndicConjunctClass)(state_ >> 8);
    }

    const bool boundary = graphemeBreakSimple(stateClass, rightClass) && !(stateIndic == IndicConjunctClass::Linker && rightIndic == IndicConjunctClass::Consonant);
    if (rightIndic == IndicConjunctClass::Consonant || stateIndic == IndicConjunctClass::Consonant || stateIndic == IndicConjunctClass::Extend) {
        stateIndic = rightIndic;
    } else if (stateIndic == IndicConjunctClass::Linker) {
        stateIndic = rightIndic == IndicConjunctClass::Extend ? IndicConjunctClass::Linker : rightIndic;
    }

    if (stateClass == rightClass && rightClass == GraphemeClass::RegionalIndicator) {
        stateClass = GraphemeClass::Other;
    } else if (stateClass == GraphemeClass::ExtendedPictographic) {
        if (rightClass == GraphemeClass::Extend) {
            stateClass = GraphemeClass::ExtendedPictographic;
        } else if (rightClass == GraphemeClass::Zwj) {
            stateClass = GraphemeClass::EmojiZwj;
        } else {
            stateClass = rightClass;
        }
    } else {
        stateClass = rightClass;
    }
    if (boundary) {
        // The new cluster is this codepoint alone: seed the state exactly
        // as the state_ == 0 branch above would derive it from previous_
        // on the next call, so a run of independent clusters - CJK text -
        // pays one property lookup per codepoint, not two.
        stateClass = rightClass;
        stateIndic = rightIndic == IndicConjunctClass::Consonant ? IndicConjunctClass::Consonant : IndicConjunctClass::None;
    }
    state_ = ((i32)(stateClass) + 1) | ((i32)(stateIndic) << 8);
    previous_ = (i32)(codepoint);
    previousSimple_ = simple;
    return boundary;
}
