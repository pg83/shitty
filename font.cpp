/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "font.h"

#include "composer.h"
#include "font_resolver.h"
#include "grapheme.h"
#include "options.h"
#include "utf8.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/typ/support.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include <errno.h>
#include <math.h>

namespace stl {}

using namespace stl;

namespace {
    struct FontImpl final: public Font {
        FontImpl(const FontFace& font, FontKind kind, FontMetrics& metrics);
        ~FontImpl() noexcept;

        FontGlyph glyph(const u32* codepoints, size_t count) override;
        long faceIndex() const override;

        void configure();
        void configureFixed();
        void configureScaled();
        bool accepts(const u32* codepoints, size_t count) const;
        bool rasterize(const u32* codepoints, size_t count);
        bool rasterizeMask(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count);
        bool rasterizeColor(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count);
        void drawMask(const FT_Bitmap& source, int destinationX, int destinationY);
        void drawColor(const FT_Bitmap& source, int destinationX, int destinationY, int sourceWidth, int sourceHeight);
        void scaleColor(int sourceWidth, int sourceHeight);
        void close() noexcept;
        [[noreturn]] void fail(StringView message);
        [[noreturn]] void fail(StringBuilder&& message);

        FT_Library library_ = nullptr;
        FT_Face face_ = nullptr;
        hb_font_t* harfbuzz_ = nullptr;
        hb_buffer_t* shape_ = nullptr;
        FontKind kind_;
        FontMetrics metrics_;
        bool hasColor_ = false;
        bool glyphColor_ = false;
        Buffer bitmap_;
        Buffer source_;
    };

    int absolute(int value) {
        return value < 0 ? -value : value;
    }

    int maximum(int left, int right) {
        return left > right ? left : right;
    }

    int minimum(int left, int right) {
        return left < right ? left : right;
    }

    u16 rounded(double value) {
        return (u16)(value + 0.5);
    }

    int pixels(hb_position_t value) {
        return value >= 0 ? (value + 32) / 64 : -((-value + 32) / 64);
    }
}

FontImpl::FontImpl(const FontFace& font, FontKind kind, FontMetrics& metrics)
    : kind_(kind)
    , metrics_(metrics)
{
    if (FT_Init_FreeType(&library_)) {
        fail(StringView(u8"could not initialize FreeType"));
    }

    Buffer filenameBuffer(font.filename);
    if (FT_New_Face(library_, filenameBuffer.cStr(), font.index, &face_)) {
        close();
        fail(StringBuilder() << StringView(u8"failed to open font ") << font.filename);
    }

    try {
        hasColor_ = FT_HAS_COLOR(face_);
        configure();
        harfbuzz_ = hb_ft_font_create_referenced(face_);
        shape_ = hb_buffer_create();
        if (harfbuzz_ == nullptr || shape_ == nullptr || !hb_buffer_allocation_successful(shape_)) {
            fail(StringView(u8"could not initialize font shaping"));
        }
    } catch (...) {
        close();
        throw;
    }

    metrics = metrics_;
}

FontImpl::~FontImpl() noexcept {
    close();
}

void FontImpl::close() noexcept {
    if (shape_ != nullptr) {
        hb_buffer_destroy(shape_);
        shape_ = nullptr;
    }
    if (harfbuzz_ != nullptr) {
        hb_font_destroy(harfbuzz_);
        harfbuzz_ = nullptr;
    }
    if (face_ != nullptr) {
        FT_Done_Face(face_);
        face_ = nullptr;
    }
    if (library_ != nullptr) {
        FT_Done_FreeType(library_);
        library_ = nullptr;
    }
}

void FontImpl::fail(StringView message) {
    Errno(EINVAL).raise(Buffer(message));
}

void FontImpl::fail(StringBuilder&& message) {
    Errno(EINVAL).raise(move(message));
}

void FontImpl::configure() {
    if (face_->num_fixed_sizes > 0) {
        configureFixed();
    } else {
        configureScaled();
    }
    if (metrics_.width == 0 || metrics_.height == 0) {
        fail(StringView(u8"font has zero-sized glyph cells"));
    }
}

void FontImpl::configureFixed() {
    int bestIndex = -1;
    int bestDifference = 0x7fffffff;
    for (int index = 0; index < face_->num_fixed_sizes; ++index) {
        const FT_Bitmap_Size& size = face_->available_sizes[index];
        const int pixels = size.y_ppem > 0 ? maximum(1, rounded(size.y_ppem / 64.0)) : size.height;
        const int difference = absolute((int)(opts.fontsize) - pixels);
        if (difference < bestDifference) {
            bestIndex = index;
            bestDifference = difference;
        }
    }
    if (bestIndex < 0) {
        fail(StringView(u8"font advertises no usable fixed size"));
    }
    if (bestDifference > 1 && face_->units_per_EM > 0 && !hasColor_) {
        configureScaled();
        return;
    }
    if (FT_Select_Size(face_, bestIndex)) {
        fail(StringView(u8"could not select fixed font size"));
    }

    const FT_Bitmap_Size& size = face_->available_sizes[bestIndex];
    const int strikePixels = size.y_ppem > 0 ? maximum(1, rounded(size.y_ppem / 64.0)) : size.height;
    const double scale = hasColor_ ? opts.fontsize / (double)(strikePixels) : 1;
    FontMetrics actual{
        .width = hasColor_ ? rounded(size.width * scale) : (u16)(size.width),
        .height = hasColor_ ? rounded(size.height * scale) : (u16)(size.height),
        .baseline = 0,
    };
    if (!hasColor_ && face_->size != nullptr) {
        actual.baseline = rounded(face_->size->metrics.ascender / 64.0);
    }
    if (kind_ == FontKind::Primary) {
        metrics_ = actual;
    } else if (kind_ == FontKind::DoubleWidth && !hasColor_) {
        if (metrics_.width != actual.width || metrics_.height != actual.height) {
            fail(StringBuilder() << StringView(u8"font cell mismatch: expected ") << metrics_.width << StringView(u8"x") << metrics_.height << StringView(u8", got ") << actual.width << StringView(u8"x") << actual.height);
        }
        metrics_.baseline = actual.baseline;
    } else if (kind_ == FontKind::Overlay && !hasColor_) {
        if (metrics_.height != actual.height) {
            fail(StringBuilder() << StringView(u8"font cell mismatch: expected ") << metrics_.width << StringView(u8"x") << metrics_.height << StringView(u8", got ") << actual.width << StringView(u8"x") << actual.height);
        }
        if (metrics_.baseline != actual.baseline) {
            fail(StringBuilder() << StringView(u8"font baseline mismatch: expected ") << metrics_.baseline << StringView(u8", got ") << actual.baseline);
        }
    }
    if (hasColor_ && kind_ != FontKind::Overlay && face_->height != 0) {
        metrics_.baseline = rounded(metrics_.height * (double)(face_->ascender) / face_->height);
    }
}

void FontImpl::configureScaled() {
    if (FT_Set_Pixel_Sizes(face_, opts.fontsize, opts.fontsize)) {
        fail(StringView(u8"could not select scalable font size"));
    }
    if (face_->units_per_EM == 0 || face_->height == 0) {
        fail(StringView(u8"font has unusable scalable metrics"));
    }

    double width = metrics_.width;
    if (kind_ != FontKind::Overlay) {
        const FT_ULong widthCodepoint = kind_ == FontKind::DoubleWidth ? 0x3000 : 'M';
        const bool hasRepresentativeAdvance =
            FT_Get_Char_Index(face_, widthCodepoint) != 0 &&
            FT_Load_Char(face_, widthCodepoint, FT_LOAD_DEFAULT) == 0 &&
            face_->glyph->advance.x > 0;
        if (hasRepresentativeAdvance) {
            width = face_->glyph->advance.x / 64.0;
        } else {
            if (face_->max_advance_width <= 0) {
                fail(StringView(u8"font has unusable scalable width metrics"));
            }
            width = opts.fontsize * (double)(face_->max_advance_width) / face_->units_per_EM;
        }
    }
    const double height = opts.fontsize * (double)(face_->height) / face_->units_per_EM + 1;
    const FontMetrics actual{
        .width = kind_ == FontKind::Overlay ? metrics_.width : rounded(width),
        .height = rounded(height),
        .baseline = rounded(height * face_->ascender / face_->height),
    };
    if (kind_ == FontKind::DoubleWidth && !hasColor_ && (metrics_.width != actual.width || metrics_.height != actual.height)) {
        fail(StringBuilder() << StringView(u8"font cell mismatch: expected ") << metrics_.width << StringView(u8"x") << metrics_.height << StringView(u8", got ") << actual.width << StringView(u8"x") << actual.height);
    }
    if (kind_ == FontKind::Overlay && !hasColor_ && metrics_.height != actual.height) {
        fail(StringBuilder() << StringView(u8"font cell mismatch: expected ") << metrics_.width << StringView(u8"x") << metrics_.height << StringView(u8", got ") << actual.width << StringView(u8"x") << actual.height);
    }
    if (kind_ == FontKind::Overlay && metrics_.baseline != actual.baseline) {
        fail(StringBuilder() << StringView(u8"font baseline mismatch: expected ") << metrics_.baseline << StringView(u8", got ") << actual.baseline);
    }
    if (kind_ == FontKind::Primary) {
        metrics_ = actual;
    } else if (kind_ == FontKind::DoubleWidth && !hasColor_) {
        metrics_.baseline = actual.baseline;
    }
}

bool FontImpl::accepts(const u32* codepoints, size_t count) const {
    if (count == 0) {
        return false;
    }
    if (codepoints[0] == Missing_Glyph_Marker || codepoints[0] == Unicode_Replacement_Character || count > 1) {
        return true;
    }
    const int width = codepointWidth(codepoints[0]);
    return kind_ == FontKind::DoubleWidth ? width == 2 : width < 2;
}

void FontImpl::drawMask(const FT_Bitmap& source, int destinationX, int destinationY) {
    const int sourceWidth = source.width;
    const int sourceHeight = source.rows;
    const int sourceX = maximum(0, -destinationX);
    const int sourceY = maximum(0, -destinationY);
    destinationX = maximum(0, destinationX);
    destinationY = maximum(0, destinationY);
    const int copyWidth = minimum(sourceWidth - sourceX, (int)(metrics_.width) - destinationX);
    const int copyHeight = minimum(sourceHeight - sourceY, (int)(metrics_.height) - destinationY);
    if (copyWidth <= 0 || copyHeight <= 0) {
        return;
    }

    const int pitch = source.pitch;
    const int rowStride = absolute(pitch);
    auto* destination = (u8*)(bitmap_.mutData());
    for (int row = 0; row < copyHeight; ++row) {
        const int sourceRow = sourceY + row;
        const int storedRow = pitch < 0 ? sourceHeight - sourceRow - 1 : sourceRow;
        const u8* sourcePixels = (const u8*)(source.buffer + storedRow * rowStride);
        u8* destinationPixels = destination + (destinationY + row) * metrics_.width + destinationX;
        if (source.pixel_mode == FT_PIXEL_MODE_GRAY) {
            for (int column = 0; column < copyWidth; ++column) {
                destinationPixels[column] = maximum(destinationPixels[column], sourcePixels[sourceX + column]);
            }
        } else if (source.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (int column = 0; column < copyWidth; ++column) {
                const int sourceColumn = sourceX + column;
                const u8 coverage = sourcePixels[sourceColumn >> 3] & (0x80 >> (sourceColumn & 7)) ? 0xff : 0;
                destinationPixels[column] = maximum(destinationPixels[column], coverage);
            }
        }
    }
}

bool FontImpl::rasterizeMask(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count) {
    bitmap_.zero((size_t)(metrics_.width) * metrics_.height);
    hb_position_t penX = 0;
    hb_position_t penY = 0;
    for (unsigned index = 0; index < count; ++index) {
        if (FT_Load_Glyph(face_, glyphs[index].codepoint, FT_LOAD_RENDER)) {
            return false;
        }
        const int destinationX = pixels(penX + positions[index].x_offset) + face_->glyph->bitmap_left;
        const int destinationY = metrics_.baseline - pixels(penY + positions[index].y_offset) - face_->glyph->bitmap_top;
        const FT_Bitmap& source = face_->glyph->bitmap;
        if (source.pixel_mode != FT_PIXEL_MODE_GRAY && source.pixel_mode != FT_PIXEL_MODE_MONO) {
            return false;
        }
        drawMask(source, destinationX, destinationY);
        penX += positions[index].x_advance;
        penY += positions[index].y_advance;
    }
    return true;
}

void FontImpl::drawColor(const FT_Bitmap& source, int destinationX, int destinationY, int sourceWidth, int sourceHeight) {
    const int pitch = source.pitch;
    const int rowStride = absolute(pitch);
    auto* destination = (u8*)(source_.mutData());
    for (int row = 0; row < source.rows; ++row) {
        const int storedRow = pitch < 0 ? source.rows - row - 1 : row;
        const u8* sourcePixels = (const u8*)(source.buffer + storedRow * rowStride);
        for (int column = 0; column < source.width; ++column) {
            const int x = destinationX + column;
            const int y = destinationY + row;
            if (x < 0 || y < 0 || x >= sourceWidth || y >= sourceHeight) {
                continue;
            }
            u8 red = 0;
            u8 green = 0;
            u8 blue = 0;
            u8 alpha = 0;
            if (source.pixel_mode == FT_PIXEL_MODE_BGRA) {
                const u8* pixel = sourcePixels + 4 * column;
                blue = pixel[0];
                green = pixel[1];
                red = pixel[2];
                alpha = pixel[3];
            } else if (source.pixel_mode == FT_PIXEL_MODE_GRAY) {
                alpha = sourcePixels[column];
                red = alpha;
                green = alpha;
                blue = alpha;
            } else if (source.pixel_mode == FT_PIXEL_MODE_MONO) {
                alpha = sourcePixels[column >> 3] & (0x80 >> (column & 7)) ? 0xff : 0;
                red = alpha;
                green = alpha;
                blue = alpha;
            } else {
                continue;
            }

            u8* target = destination + 4 * ((size_t)(y)*sourceWidth + x);
            const unsigned inverse = 255 - alpha;
            target[0] = (u8)(red + (unsigned)(target[0]) * inverse / 255);
            target[1] = (u8)(green + (unsigned)(target[1]) * inverse / 255);
            target[2] = (u8)(blue + (unsigned)(target[2]) * inverse / 255);
            target[3] = (u8)(alpha + (unsigned)(target[3]) * inverse / 255);
        }
    }
}

void FontImpl::scaleColor(int sourceWidth, int sourceHeight) {
    bitmap_.zero((size_t)(metrics_.width) * metrics_.height * 4);
    double scale = minimum(metrics_.width, metrics_.height) / (double)(maximum(sourceWidth, sourceHeight));
    if (scale > 1) {
        scale = 1;
    }
    const int targetWidth = maximum(1, rounded(sourceWidth * scale));
    const int targetHeight = maximum(1, rounded(sourceHeight * scale));
    const int originX = ((int)(metrics_.width) - targetWidth) / 2;
    const int originY = ((int)(metrics_.height) - targetHeight) / 2;
    const auto* source = (const u8*)(source_.data());
    auto* destination = (u8*)(bitmap_.mutData());
    for (int y = 0; y < targetHeight; ++y) {
        const double sourceY = (y + 0.5) / scale - 0.5;
        const int firstY = maximum(0, minimum(sourceHeight - 1, (int)(floor(sourceY))));
        const int secondY = minimum(sourceHeight - 1, firstY + 1);
        const double fractionY = sourceY - floor(sourceY);
        for (int x = 0; x < targetWidth; ++x) {
            const double sourceX = (x + 0.5) / scale - 0.5;
            const int firstX = maximum(0, minimum(sourceWidth - 1, (int)(floor(sourceX))));
            const int secondX = minimum(sourceWidth - 1, firstX + 1);
            const double fractionX = sourceX - floor(sourceX);
            const u8* topLeft = source + 4 * ((size_t)(firstY)*sourceWidth + firstX);
            const u8* topRight = source + 4 * ((size_t)(firstY)*sourceWidth + secondX);
            const u8* bottomLeft = source + 4 * ((size_t)(secondY)*sourceWidth + firstX);
            const u8* bottomRight = source + 4 * ((size_t)(secondY)*sourceWidth + secondX);
            u8* target = destination + 4 * ((size_t)(originY + y) * metrics_.width + originX + x);
            for (int channel = 0; channel < 4; ++channel) {
                const double top = topLeft[channel] * (1 - fractionX) + topRight[channel] * fractionX;
                const double bottom = bottomLeft[channel] * (1 - fractionX) + bottomRight[channel] * fractionX;
                target[channel] = (u8)(top * (1 - fractionY) + bottom * fractionY + 0.5);
            }
        }
    }
}

bool FontImpl::rasterizeColor(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count) {
    hb_position_t penX = 0;
    hb_position_t penY = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool haveBounds = false;
    for (unsigned index = 0; index < count; ++index) {
        if (FT_Load_Glyph(face_, glyphs[index].codepoint, FT_LOAD_RENDER | FT_LOAD_COLOR)) {
            return false;
        }
        const int glyphLeft = pixels(penX + positions[index].x_offset) + face_->glyph->bitmap_left;
        const int glyphTop = -pixels(penY + positions[index].y_offset) - face_->glyph->bitmap_top;
        const int glyphRight = glyphLeft + face_->glyph->bitmap.width;
        const int glyphBottom = glyphTop + face_->glyph->bitmap.rows;
        if (!haveBounds) {
            left = glyphLeft;
            top = glyphTop;
            right = glyphRight;
            bottom = glyphBottom;
            haveBounds = true;
        } else {
            left = minimum(left, glyphLeft);
            top = minimum(top, glyphTop);
            right = maximum(right, glyphRight);
            bottom = maximum(bottom, glyphBottom);
        }
        penX += positions[index].x_advance;
        penY += positions[index].y_advance;
    }
    const int sourceWidth = right - left;
    const int sourceHeight = bottom - top;
    if (!haveBounds || sourceWidth <= 0 || sourceHeight <= 0) {
        return false;
    }

    source_.zero((size_t)(sourceWidth)*sourceHeight * 4);
    penX = 0;
    penY = 0;
    for (unsigned index = 0; index < count; ++index) {
        if (FT_Load_Glyph(face_, glyphs[index].codepoint, FT_LOAD_RENDER | FT_LOAD_COLOR)) {
            return false;
        }
        const int glyphLeft = pixels(penX + positions[index].x_offset) + face_->glyph->bitmap_left - left;
        const int glyphTop = -pixels(penY + positions[index].y_offset) - face_->glyph->bitmap_top - top;
        drawColor(face_->glyph->bitmap, glyphLeft, glyphTop, sourceWidth, sourceHeight);
        penX += positions[index].x_advance;
        penY += positions[index].y_advance;
    }
    scaleColor(sourceWidth, sourceHeight);
    return true;
}

bool FontImpl::rasterize(const u32* codepoints, size_t count) {
    hb_glyph_info_t missing{};
    hb_glyph_position_t missingPosition{};
    if (count == 1 && codepoints[0] == Missing_Glyph_Marker) {
        if (FT_Load_Glyph(face_, 0, FT_LOAD_RENDER | FT_LOAD_COLOR)) {
            return false;
        }
        glyphColor_ = face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
        return glyphColor_ ? rasterizeColor(&missing, &missingPosition, 1) : rasterizeMask(&missing, &missingPosition, 1);
    }

    hb_buffer_clear_contents(shape_);
    hb_buffer_add_codepoints(shape_, (const hb_codepoint_t*)(codepoints), count, 0, count);
    hb_buffer_guess_segment_properties(shape_);
    hb_shape(harfbuzz_, shape_, nullptr, 0);
    unsigned glyphCount = 0;
    const hb_glyph_info_t* glyphs = hb_buffer_get_glyph_infos(shape_, &glyphCount);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(shape_, &glyphCount);
    if (glyphCount == 0) {
        return false;
    }
    for (unsigned index = 0; index < glyphCount; ++index) {
        if (glyphs[index].codepoint == 0) {
            return false;
        }
        if (FT_Load_Glyph(face_, glyphs[index].codepoint, FT_LOAD_RENDER | FT_LOAD_COLOR)) {
            return false;
        }
        if (face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            glyphColor_ = true;
        }
    }
    return glyphColor_ ? rasterizeColor(glyphs, positions, glyphCount) : rasterizeMask(glyphs, positions, glyphCount);
}

FontGlyph FontImpl::glyph(const u32* codepoints, size_t count) {
    glyphColor_ = false;
    if (!accepts(codepoints, count) || !rasterize(codepoints, count)) {
        return {};
    }
    return {
        .data = bitmap_.data(),
        .len = bitmap_.used(),
        .color = glyphColor_,
    };
}

long FontImpl::faceIndex() const {
    return face_->face_index;
}

Font* Font::create(Composer& composer, const FontFace& face, FontKind kind, FontMetrics& metrics) {
    return composer.pool->make<FontImpl>(face, kind, metrics);
}
