/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "font.h"

#include <std/str/view.h>
#include <std/sys/types.h>

struct Composer;

enum class FontStyle : u8 {
    Regular,
    Bold,
    Italic,
    BoldItalic,
};

struct Fontpack {
    virtual u16 getPx() const = 0;
    virtual u16 getPy() const = 0;

    virtual bool hasBold() const = 0;
    virtual bool hasItalic() const = 0;
    virtual bool hasBoldItalic() const = 0;
    virtual bool hasDoubleWidth() const = 0;
    virtual long regularFaceIndex() const = 0;
    virtual long doubleWidthFaceIndex() const = 0;

    virtual FontGlyph glyph(const u32* codepoints, size_t count, FontStyle style, bool doubleWidth) = 0;

    static Fontpack* create(Composer& composer, stl::StringView fontname, stl::StringView dwfontname);
};
