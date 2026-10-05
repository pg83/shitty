/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_geometry.h"

#include <lib/vterm/num.h>

using namespace stl;
using namespace plt;

namespace {

    // A single "<digits>" or "<digits>%" component. Empty input, a sign,
    // a decimal point, or any other stray byte all fail through
    // parseU64. minValue lets the two callers below share this instead
    // of duplicating the percent/pixel split: W/H pass 1 (zero-sized is
    // useless), X/Y pass 0 (zero is the default offset).
    bool parseDim(StringView text, u32 minValue, QuickGeometryDim& out) {
        if (text.empty()) {
            return false;
        }
        const bool percent = text.back() == '%';
        const StringView digits = percent ? StringView(text.data(), text.length() - 1) : text;
        u64 value = 0;
        if (!parseU64(digits, value)) {
            return false;
        }
        if (percent && value > 100) {
            return false;
        }
        if (!percent && value > UINT16_MAX) {
            return false;
        }
        if (value < minValue) {
            return false;
        }
        out.percent = percent;
        out.value = (u32)(value);
        return true;
    }
}

bool parseQuickGeometry(StringView text, QuickGeometry& out) {
    StringView widthText;
    StringView afterWidth;
    if (!text.split('x', widthText, afterWidth)) {
        return false;
    }
    StringView heightText;
    StringView afterHeight;
    if (!afterWidth.split('+', heightText, afterHeight)) {
        return false;
    }
    StringView xText;
    StringView yText;
    if (!afterHeight.split('+', xText, yText)) {
        return false;
    }

    QuickGeometry parsed;
    if (!parseDim(widthText, 1, parsed.width)) {
        return false;
    }
    if (!parseDim(heightText, 1, parsed.height)) {
        return false;
    }
    if (!parseDim(xText, 0, parsed.x)) {
        return false;
    }
    if (!parseDim(yText, 0, parsed.y)) {
        return false;
    }

    out = parsed;
    return true;
}
