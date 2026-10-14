/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_text.h"

#include <std/lib/buffer.h>
#include <std/lib/vector.h>

#include <math.h>
#include <string.h>

#if defined(HAVE_FREETYPE) && defined(HAVE_HARFBUZZ)
    #include <ft2build.h>
    #include FT_FREETYPE_H
    #include <hb.h>
    #include <hb-ft.h>
#endif

#if defined(HAVE_FONTCONFIG)
    #include <fontconfig/fontconfig.h>
#endif

using namespace stl;

#if defined(HAVE_FREETYPE) && defined(HAVE_HARFBUZZ)

struct UiText::Impl {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    hb_font_t* font = nullptr;
    hb_buffer_t* buffer = nullptr;
    float ascent = 0;
    float descent = 0;

    ~Impl() {
        if (buffer != nullptr) {
            hb_buffer_destroy(buffer);
        }
        if (font != nullptr) {
            hb_font_destroy(font);
        }
        if (face != nullptr) {
            FT_Done_Face(face);
        }
        if (library != nullptr) {
            FT_Done_FreeType(library);
        }
    }

    bool finish(float pixelSize) {
        if (FT_Set_Char_Size(face, 0, (FT_F26Dot6)(pixelSize * 64.0f), 72, 72) != 0) {
            return false;
        }
        font = hb_ft_font_create_referenced(face);
        buffer = hb_buffer_create();
        ascent = face->size->metrics.ascender / 64.0f;
        descent = -face->size->metrics.descender / 64.0f;
        return font != nullptr && buffer != nullptr;
    }

    // Shapes `utf8` into the buffer and returns its glyph count.
    unsigned shape(StringView utf8) {
        hb_buffer_reset(buffer);
        hb_buffer_add_utf8(buffer, (const char*)(utf8.data()), (int)(utf8.length()), 0, (int)(utf8.length()));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(font, buffer, nullptr, 0);
        return hb_buffer_get_length(buffer);
    }

    float advanceOf(StringView utf8) {
        const unsigned count = shape(utf8);
        const hb_glyph_position_t* const positions = hb_buffer_get_glyph_positions(buffer, nullptr);
        float width = 0;
        for (unsigned at = 0; at < count; ++at) {
            width += positions[at].x_advance / 64.0f;
        }
        return width;
    }

    void drawGlyph(ChromeCanvas& canvas, u32 glyph, float penX, float baseline, ChromeColor color, i32 clipRight) {
        // The pen's fraction goes into the outline, so glyphs keep their
        // proportional spacing instead of snapping to whole pixels.
        const float whole = floorf(penX);
        FT_Vector shift{(FT_Pos)((penX - whole) * 64.0f), 0};
        FT_Set_Transform(face, nullptr, &shift);
        if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0 || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LIGHT) != 0) {
            return;
        }
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.buffer == nullptr) {
            return;
        }
        canvas.blendMask((i32)(whole) + face->glyph->bitmap_left, (i32)(roundf(baseline)) - face->glyph->bitmap_top, (const u8*)(bitmap.buffer), bitmap.width, bitmap.rows, (u32)(bitmap.pitch), color, clipRight);
    }
};

UiText::UiText()
    : impl_(nullptr)
{
}

UiText::~UiText() {
    delete impl_;
}

bool UiText::openFile(const char* path, i32 index, float pixelSize) {
    delete impl_;
    impl_ = new Impl;
    if (FT_Init_FreeType(&impl_->library) != 0 || FT_New_Face(impl_->library, path, index, &impl_->face) != 0 || !impl_->finish(pixelSize)) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

bool UiText::openMemory(const void* data, size_t size, i32 index, float pixelSize) {
    delete impl_;
    impl_ = new Impl;
    if (FT_Init_FreeType(&impl_->library) != 0 || FT_New_Memory_Face(impl_->library, (const FT_Byte*)(data), (FT_Long)(size), index, &impl_->face) != 0 || !impl_->finish(pixelSize)) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

bool UiText::ready() const {
    return impl_ != nullptr;
}

float UiText::ascent() const {
    return impl_ == nullptr ? 0 : impl_->ascent;
}

float UiText::descent() const {
    return impl_ == nullptr ? 0 : impl_->descent;
}

float UiText::measure(StringView utf8) {
    return impl_ == nullptr ? 0 : impl_->advanceOf(utf8);
}

float UiText::draw(ChromeCanvas& canvas, float x, float baseline, StringView utf8, ChromeColor color, float maxWidth) {
    if (impl_ == nullptr || utf8.empty() || maxWidth <= 0) {
        return 0;
    }
    const StringView ellipsis(u8"…");
    const float ellipsisWidth = impl_->advanceOf(ellipsis);
    const unsigned count = impl_->shape(utf8);
    // Copied out: shaping the ellipsis below reuses the buffer.
    Vector<u32> glyphs;
    Vector<float> advances;
    Vector<u32> clusters;
    const hb_glyph_info_t* const infos = hb_buffer_get_glyph_infos(impl_->buffer, nullptr);
    const hb_glyph_position_t* const positions = hb_buffer_get_glyph_positions(impl_->buffer, nullptr);
    float total = 0;
    for (unsigned at = 0; at < count; ++at) {
        glyphs.pushBack(infos[at].codepoint);
        advances.pushBack(positions[at].x_advance / 64.0f);
        clusters.pushBack(infos[at].cluster);
        total += positions[at].x_advance / 64.0f;
    }
    const i32 clipRight = (i32)(ceilf(x + maxWidth));
    float pen = x;
    if (total <= maxWidth) {
        for (unsigned at = 0; at < count; ++at) {
            impl_->drawGlyph(canvas, glyphs[at], pen, baseline, color, clipRight);
            pen += advances[at];
        }
        return total;
    }
    // Whole clusters only, then the ellipsis in the room left.
    unsigned cut = 0;
    float used = 0;
    for (unsigned at = 0; at < count; ++at) {
        if (used + advances[at] + ellipsisWidth > maxWidth) {
            break;
        }
        used += advances[at];
        if (at + 1 == count || clusters[at + 1] != clusters[at]) {
            cut = at + 1;
        }
    }
    used = 0;
    for (unsigned at = 0; at < cut; ++at) {
        impl_->drawGlyph(canvas, glyphs[at], pen, baseline, color, clipRight);
        pen += advances[at];
        used += advances[at];
    }
    const unsigned dots = impl_->shape(ellipsis);
    const hb_glyph_info_t* const dotInfos = hb_buffer_get_glyph_infos(impl_->buffer, nullptr);
    const hb_glyph_position_t* const dotPositions = hb_buffer_get_glyph_positions(impl_->buffer, nullptr);
    for (unsigned at = 0; at < dots; ++at) {
        impl_->drawGlyph(canvas, dotInfos[at].codepoint, pen, baseline, color, clipRight);
        pen += dotPositions[at].x_advance / 64.0f;
    }
    return used + ellipsisWidth;
}

#else

struct UiText::Impl {
};

UiText::UiText()
    : impl_(nullptr)
{
}

UiText::~UiText() {
}

bool UiText::openFile(const char*, i32, float) {
    return false;
}

bool UiText::openMemory(const void*, size_t, i32, float) {
    return false;
}

bool UiText::ready() const {
    return false;
}

float UiText::ascent() const {
    return 0;
}

float UiText::descent() const {
    return 0;
}

float UiText::measure(StringView) {
    return 0;
}

float UiText::draw(ChromeCanvas&, float, float, StringView, ChromeColor, float) {
    return 0;
}

#endif

bool findUiFont(bool bold, Buffer& path, i32& index) {
#if defined(HAVE_FONTCONFIG)
    // One configuration for every UI font: loading it reads the whole of
    // /etc/fonts again, and the bundled fontconfig says what it cannot parse
    // in a newer system's files each time.
    static FcConfig* const config = FcInitLoadConfigAndFonts();
    if (config == nullptr) {
        return false;
    }
    FcPattern* const pattern = FcNameParse((const FcChar8*)("sans-serif"));
    bool found = false;
    if (pattern != nullptr) {
        FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_SEMIBOLD : FC_WEIGHT_REGULAR);
        FcConfigSubstitute(config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result = FcResultNoMatch;
        FcPattern* const match = FcFontMatch(config, pattern, &result);
        if (match != nullptr) {
            FcChar8* file = nullptr;
            int faceIndex = 0;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
                FcPatternGetInteger(match, FC_INDEX, 0, &faceIndex);
                path.reset();
                path.append((const char*)(file), strlen((const char*)(file)));
                index = faceIndex;
                found = true;
            }
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
    }
    return found;
#else
    (void)(bold);
    (void)(path);
    (void)(index);
    return false;
#endif
}
