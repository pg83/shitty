/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>

namespace stl {
    class ObjPool;
}

struct FontFace {
    stl::StringView filename;
    int index = 0;

    bool empty() const {
        return filename.empty();
    }

    bool operator==(const FontFace& other) const {
        return filename == other.filename && index == other.index;
    }

    bool operator!=(const FontFace& other) const {
        return !(*this == other);
    }
};

struct FontVariants {
    FontFace regular;
    FontFace bold;
    FontFace italic;
    FontFace boldItalic;
};

FontVariants resolveFontconfig(stl::ObjPool* pool, stl::StringView fontname);
void finalizeFontconfig() noexcept;
