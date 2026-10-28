/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */
/* part of this file is part of Zutty.
 * Copyright (C) 2020 Tom Szilagyi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * See the file LICENSE.GPL3 for the full license.
 */

#include "utf8.h"

namespace {
    static size_t decodeScalar(const u8* input, size_t length, u32& codepoint) {
        codepoint = 0;
        if (length == 0) {
            return 0;
        }
        const u8 first = input[0];
        if (first < 0x80) {
            codepoint = first;
            return 1;
        }
        size_t count;
        u32 accumulator;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 2;
            accumulator = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 3;
            accumulator = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 4;
            accumulator = first & 0x07;
        } else {
            return 0;
        }
        if (length < count) {
            return 0;
        }
        for (size_t index = 1; index < count; ++index) {
            const u8 byte = input[index];
            if ((byte & 0xc0) != 0x80) {
                return 0;
            }
            accumulator = (accumulator << 6) | (byte & 0x3f);
        }
        if ((count == 3 && accumulator < 0x800) || (count == 4 && accumulator < 0x10000) || accumulator > 0x10ffff || (accumulator >= 0xd800 && accumulator <= 0xdfff)) {
            return 0;
        }
        codepoint = accumulator;
        return count;
    }
}

size_t Utf8Decoder::decodeOne(const u8* input, size_t length, u32& codepoint) {
    return decodeScalar(input, length, codepoint);
}

size_t Utf8Decoder::decodeRun(const u8* input, size_t length, u32* codepoints, size_t capacity, size_t& consumed) {
    consumed = 0;
    size_t count = 0;
    // While four bytes remain every sequence length can be read without a
    // bounds check, so each length gets its own straight-line path with
    // the well-formedness constraints folded into the second byte's range
    // (overlongs, surrogates, the 0x10ffff ceiling) the way the Unicode
    // table of well-formed byte sequences states them. The tail falls
    // back to the general decoder.
    while (count < capacity && length - consumed >= 4) {
        const u8* const at = input + consumed;
        const u8 lead = at[0];
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7f) {
                return count;
            }
            codepoints[count++] = lead;
            consumed += 1;
            continue;
        }
        const u8 second = at[1];
        if ((second & 0xc0) != 0x80) {
            return count;
        }
        if (lead < 0xe0) {
            if (lead < 0xc2) {
                return count;
            }
            codepoints[count++] = ((u32)(lead & 0x1f) << 6) | (second & 0x3f);
            consumed += 2;
            continue;
        }
        const u8 third = at[2];
        if ((third & 0xc0) != 0x80) {
            return count;
        }
        if (lead < 0xf0) {
            if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)) {
                return count;
            }
            codepoints[count++] = ((u32)(lead & 0x0f) << 12) | ((u32)(second & 0x3f) << 6) | (third & 0x3f);
            consumed += 3;
            continue;
        }
        const u8 fourth = at[3];
        if ((fourth & 0xc0) != 0x80 || lead > 0xf4 || (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) {
            return count;
        }
        codepoints[count++] = ((u32)(lead & 0x07) << 18) | ((u32)(second & 0x3f) << 12) | ((u32)(third & 0x3f) << 6) | (fourth & 0x3f);
        consumed += 4;
    }
    while (count < capacity && consumed < length) {
        if (input[consumed] < 0x20 || input[consumed] == 0x7f) {
            break;
        }
        const size_t bytes = decodeScalar(input + consumed, length - consumed, codepoints[count]);
        if (bytes == 0) {
            break;
        }
        consumed += bytes;
        ++count;
    }
    return count;
}

size_t Utf8Decoder::decodeText(const u8* input, size_t length, Utf8Text& text) {
    size_t consumed = 0;
    text.count = 0;
    text.resetBefore = 0;
    text.resetAfter = false;
    text.full = false;
    text.simple = true;
    u8 tracePending = text.pendingTrace;
    // A complete valid prefix has neither a decoder tail nor trace debt.
    // Everything else uses the same streaming state as pushByte: no replay
    // or switch to scalar placement is needed at a block/feed boundary.
    if (remaining == 0 && tracePending == 0 && length >= 2 && input[0] >= 0xc2 && input[0] <= 0xf4 && (input[1] & 0xc0) == 0x80) {
        text.count = decodeRun(input, length, text.codepoints, Utf8Text::capacity, consumed);
        if (text.count != 0) {
            unicode = text.codepoints[text.count - 1];
            text.simple = false;
        }
    }
    bool reset = false;
    const auto append = [&](u32 codepoint) __attribute__((always_inline)) {
        text.resetBefore |= (u64)(reset) << text.count;
        reset = false;
        text.codepoints[text.count++] = codepoint;
        text.simple &= codepoint < 0x80 || codepoint == Unicode_Replacement_Character;
    };
    while (consumed < length && text.count < Utf8Text::capacity - 1) {
        const u8 byte = input[consumed];
        if (byte == 0x7f) {
            ++consumed;
            continue;
        }
        if (byte < 0x20) {
            if ((byte >= 7 && byte <= 15) || byte == 0x1b) {
                break;
            }
            if (checkPrematureEOS()) {
                append(unicode);
            }
            reset = true;
        } else if (byte < 0x80) {
            if (checkPrematureEOS()) {
                append(unicode);
            }
            unicode = byte;
            append(byte);
            tracePending = 0;
        } else {
            // Re-enter the bulk decoder whenever the stream is clean again:
            // without this, one bad byte near the start of a block demotes
            // every valid sequence after it to the byte-at-a-time machine,
            // and a lightly corrupted stream parses almost entirely on the
            // slow path.
            if (remaining == 0 && tracePending == 0 && byte >= 0xc2 && byte <= 0xf4 && consumed + 1 < length && (input[consumed + 1] & 0xc0) == 0x80) {
                size_t runConsumed = 0;
                const size_t run = decodeRun(input + consumed, length - consumed, text.codepoints + text.count, Utf8Text::capacity - 1 - text.count, runConsumed);
                if (run != 0) {
                    text.resetBefore |= (u64)(reset) << text.count;
                    reset = false;
                    text.count += run;
                    unicode = text.codepoints[text.count - 1];
                    text.simple = false;
                    consumed += runConsumed;
                    continue;
                }
            }
            if (byte <= 0x9f && tracePending == 0) {
                if (checkPrematureEOS()) {
                    append(unicode);
                }
                reset = true;
            }
            for (int completed = pushByte(byte); completed != 0; --completed) {
                append(unicode);
            }
            // The protocol trace counts continuation bytes even after an
            // early decoding error or an intervening C0 flushed the tail.
            if ((byte & 0xc0) == 0x80 && tracePending != 0) {
                --tracePending;
            } else if (byte >= 0xc2 && byte <= 0xdf) {
                tracePending = 1;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                tracePending = 2;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                tracePending = 3;
            } else {
                tracePending = 0;
            }
        }
        ++consumed;
    }
    text.full = text.count >= Utf8Text::capacity - 1;
    text.pendingTrace = tracePending;
    text.resetAfter = reset;
    return consumed;
}

bool Utf8Decoder::checkPrematureEOS() {
    if (remaining > 0) {
        remaining = 0;
        unicode = Unicode_Replacement_Character;
        return true;
    }
    return false;
}

void Utf8Decoder::reset() {
    accumulator = 0;
    unicode = 0;
    minimum = 0;
    remaining = 0;
}

bool Utf8Decoder::onUnicode(u32 ch) {
    if (!ch) {
        return false;
    }

    unicode = ch;
    return true;
}

int Utf8Decoder::pushByte(unsigned char ch) {
    if ((ch & 0xc0) == 0x80) {
        if (remaining == 0) {
            unicode = Unicode_Replacement_Character;
            return 1;
        }
        const bool invalidFirstContinuation = (remaining == 2 && accumulator == 0 && ch < 0xa0) || (remaining == 2 && accumulator == 0x0d && ch >= 0xa0) || (remaining == 3 && accumulator == 0 && ch < 0x90) || (remaining == 3 && accumulator == 4 && ch >= 0x90);
        if (invalidFirstContinuation) {
            remaining = 0;
            unicode = Unicode_Replacement_Character;
            return 2;
        }
        accumulator = (accumulator << 6) | (ch & 0x3f);
        if (--remaining != 0) {
            return 0;
        }
        if (accumulator < minimum || accumulator > 0x10ffff || (accumulator >= 0xd800 && accumulator <= 0xdfff)) {
            unicode = Unicode_Replacement_Character;
        } else {
            unicode = accumulator;
        }
        return 1;
    }

    int completed = 0;
    if (remaining > 0) {
        remaining = 0;
        unicode = Unicode_Replacement_Character;
        completed = 1;
    }
    if (ch >= 0xc2 && ch <= 0xdf) {
        accumulator = ch & 0x1f;
        remaining = 1;
        minimum = 0x80;
    } else if (ch >= 0xe0 && ch <= 0xef) {
        accumulator = ch & 0x0f;
        remaining = 2;
        minimum = 0x800;
    } else if (ch >= 0xf0 && ch <= 0xf4) {
        accumulator = ch & 0x07;
        remaining = 3;
        minimum = 0x10000;
    } else {
        unicode = Unicode_Replacement_Character;
        ++completed;
    }
    return completed;
}
