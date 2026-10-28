/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

#include <stdint.h>

enum class GraphemeClass : u8;
enum class IndicConjunctClass : u8;

// Whether the codepoint's default presentation is emoji.
bool emojiPresentation(u32 codepoint);

// The codepoints of one stored grapheme cluster, viewed in place.
struct GraphemeView {
    const u32* values = nullptr;
    u32 count = 0;

    const u32* begin() const noexcept {
        return values;
    }

    const u32* end() const noexcept {
        return count == 0 ? values : values + count;
    }

    const u32* data() const noexcept {
        return values;
    }

    size_t size() const noexcept {
        return count;
    }

    bool empty() const noexcept {
        return count == 0;
    }

    const u32& operator[](size_t index) const noexcept {
        return values[index];
    }
};

class GraphemeBreaker {
public:
    [[gnu::always_inline]] bool breakBefore(u32 codepoint) {
        return breakBefore(codepoint, codepoint >= 0x20 && codepoint < 0x7f);
    }

    [[gnu::always_inline]] bool breakBefore(u32 codepoint, bool simple) {
        if (!hasPrevious_) {
            hasPrevious_ = true;
            previous_ = (i32)(codepoint);
            previousSimple_ = simple;
            return true;
        }

        if (previousSimple_ && simple) {
            previous_ = (i32)(codepoint);
            previousSimple_ = true;
            state_ = 0;
            return true;
        }

        return breakBeforeSlow(codepoint, simple);
    }

    // For callers holding the codepoint's classes from their own cache:
    // the slow path skips the property table for it.
    [[gnu::always_inline]] bool breakBefore(u32 codepoint, bool simple, GraphemeClass graphemeClass, IndicConjunctClass indicClass) {
        if (!hasPrevious_) {
            hasPrevious_ = true;
            previous_ = (i32)(codepoint);
            previousSimple_ = simple;
            return true;
        }

        if (previousSimple_ && simple) {
            previous_ = (i32)(codepoint);
            previousSimple_ = true;
            state_ = 0;
            return true;
        }

        return breakBeforeSlow(codepoint, simple, graphemeClass, indicClass);
    }

    [[gnu::always_inline]] void setBoundaryAfter(u32 codepoint) {
        setBoundaryAfter(codepoint, codepoint >= 0x20 && codepoint < 0x7f);
    }

    // For callers that already know the codepoint's grapheme simplicity and
    // batch their boundary checks: equivalent to a breakBefore(codepoint,
    // simple) that returned true.
    [[gnu::always_inline]] void setBoundaryAfter(u32 codepoint, bool simple) {
        hasPrevious_ = true;
        previous_ = (i32)(codepoint);
        previousSimple_ = simple;
        state_ = 0;
    }

    // True when the next breakBefore of a simple codepoint is guaranteed to
    // report a boundary through the fast path.
    [[gnu::always_inline]] bool simpleBoundary() const {
        return !hasPrevious_ || previousSimple_;
    }

    [[gnu::always_inline]] void reset() {
        hasPrevious_ = false;
        previous_ = 0;
        previousSimple_ = false;
        state_ = 0;
    }

private:
    bool breakBeforeSlow(u32 codepoint, bool simple);
    bool breakBeforeSlow(u32 codepoint, bool simple, GraphemeClass graphemeClass, IndicConjunctClass indicClass);

    bool hasPrevious_ = false;
    bool previousSimple_ = false;
    i32 previous_ = 0;
    i32 state_ = 0;
};
