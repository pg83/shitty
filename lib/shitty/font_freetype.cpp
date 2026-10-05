/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "font_freetype.h"

#include "options.h"
#include "composer.h"
#include "font_face.h"
#include "glyph_cache.h"
#include "font_renderer.h"

#include <lib/vterm/utf8.h>
#include <lib/vterm/grapheme.h>

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/typ/support.h>
#include <std/mem/obj_pool.h>

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include <hb-ft.h>
#include <hb.h>

#include <errno.h>
#include <math.h>

using namespace stl;

namespace {
    struct FontImpl final: public Font {
        FontImpl(Composer& composer, IntrusivePtr<FontFace> source, u16 size, FontKind kind, FontMetrics& metrics, FontStyle synthetic);
        ~FontImpl() noexcept;

        void render(const u32* codepoints, size_t count, u16 cells, void* buf) override;
        bool covers(u32 codepoint) override;
        bool colored() const override;
        Font* synthesize(ObjPool& owner, FontStyle style) override;
        FontFace* face() override;

        struct FitMeasure {
            int advance = 0;
            int ascent = 0;
            int descent = 0;
        };

        void configure();
        void configureFixed();
        void configureScaled();
        void reserveDescent(FontMetrics& metrics);
        u16 representativeAdvance();
        u32 fitRepresentative(u16 cells) const;
        FitMeasure measureAt(u16 pixelSize, u32 representative);
        u16 fitCells(u16 cells);
        bool applyCells(u16 cells);
        void renderShapedSpan(const u32* codepoints, size_t count, u16 cells, u8* out, size_t stride);
        void drawShapedRun(const u32* codepoints, size_t begin, size_t end, const u16* columns, u8* out, size_t stride);
        bool renderFittedSymbol(u32 codepoint, u8* out, size_t stride);
        void restoreSize();
        bool loadGlyph(FT_UInt glyph, bool color, bool render, FT_Pos fracX, FT_Pos fracY);
        bool strikeFor(FT_UInt glyph, u32 phaseX, u32 phaseY, GlyphStrike& strike);
        bool strikeFromOutline(FT_UInt glyph, FT_Pos fracX, FT_Pos fracY, GlyphStrike& strike);
        void drawStrike(const GlyphStrike& strike, u8* out, size_t stride, int destinationX, int destinationY);
        bool rasterize(const u32* codepoints, size_t count);
        bool rasterizeMask(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count);
        bool rasterizeColor(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count);
        void drawMask(const FT_Bitmap& source, int destinationX, int destinationY);
        void drawMaskInto(u8* destination, size_t canvas, const FT_Bitmap& source, int destinationX, int destinationY);
        void drawColor(const FT_Bitmap& source, int destinationX, int destinationY, int sourceWidth, int sourceHeight);
        void scaleColor(int sourceWidth, int sourceHeight);
        void close() noexcept;
        [[noreturn]] void fail(StringView message);
        [[noreturn]] void fail(StringBuilder&& message);

        Composer& composer_;
        // Keeps the mapped bytes alive for as long as the FT face uses them.
        IntrusivePtr<FontFace> source_;
        FT_Library library_ = nullptr;
        FT_Face face_ = nullptr;
        hb_font_t* harfbuzz_ = nullptr;
        hb_buffer_t* shape_ = nullptr;
        u16 size_;
        FontKind kind_;
        FontMetrics metrics_;
        u16 canvasWidth_ = 0;
        u16 fittedSize_[3] = {0, 0, 0};
        bool syntheticBold_ = false;
        bool syntheticItalic_ = false;
        bool soft_ = false;
        FT_Pos softEmbolden_ = 0;
        bool fitLogged_ = false;
        bool hasColor_ = false;
        bool glyphColor_ = false;
        Buffer bitmap_;
        Buffer columns_;
        Buffer sourceBitmap_;
        Buffer strike_;
        u32 strikeNamespace_ = 0;
    };

    struct FreeTypeRenderer final: public FontRenderer {
        explicit FreeTypeRenderer(Composer& composer_)
            : composer(composer_)
        {
        }

        Font* render(ObjPool& owner, IntrusivePtr<FontFace> face, u16 pixels, FontKind kind, FontMetrics& metrics) override;

        Composer& composer;
    };

    static int absolute(int value) {
        return value < 0 ? -value : value;
    }

    static int maximum(int left, int right) {
        return left > right ? left : right;
    }

    static int minimum(int left, int right) {
        return left < right ? left : right;
    }

    static u16 rounded(double value) {
        return (u16)(value + 0.5);
    }

    // A terminal cell grid cannot absorb typographic ligatures: fi/fl
    // collapse two cells' codepoints into one narrow glyph and leave the
    // second cell blank. Coding fonts express their ligatures through
    // calt, which stays enabled.
    static const hb_feature_t droppedLigatures[] = {
        {HB_TAG('l', 'i', 'g', 'a'), 0, 0, (unsigned)(-1)},
        {HB_TAG('d', 'l', 'i', 'g'), 0, 0, (unsigned)(-1)},
    };

    static int pixels(hb_position_t value) {
        return value >= 0 ? (value + 32) / 64 : -((-value + 32) / 64);
    }

    // One process-wide library instead of one per font: FT_Library only
    // holds allocator and module state, and the faces keep it alive for
    // the process lifetime anyway.
    static FT_Library sharedFreeType() {
        static FT_Library library = [] {
            FT_Library value = nullptr;
            if (FT_Init_FreeType(&value) != 0) {
                value = nullptr;
            }
            return value;
        }();
        return library;
    }
}

FontImpl::FontImpl(Composer& composer, IntrusivePtr<FontFace> source, u16 size, FontKind kind, FontMetrics& metrics, FontStyle synthetic)
    : composer_(composer)
    , source_(source)
    , size_(size)
    , kind_(kind)
    , metrics_(metrics)
    , syntheticBold_(synthetic == FontStyle::Bold || synthetic == FontStyle::BoldItalic)
    , syntheticItalic_(synthetic == FontStyle::Italic || synthetic == FontStyle::BoldItalic)
{
    library_ = sharedFreeType();
    if (library_ == nullptr) {
        fail(StringView(u8"could not initialize FreeType"));
    }
    soft_ = composer_.opts->soft >= 0;
    // 100 thickens stems by four percent of the pixel size - the scale of
    // the Core Text darkening at text sizes.
    softEmbolden_ = soft_ ? (FT_Pos)(size_) * 64 * composer_.opts->soft / 2500 : 0;

    if (FT_New_Memory_Face(library_, (const FT_Byte*)(source_->data()), (FT_Long)(source_->size()), source_->faceIndex(), &face_)) {
        close();
        fail(StringBuilder() << StringView(u8"failed to open font face ") << source_->faceIndex());
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
    // The shared FT_Library lives for the process.
    library_ = nullptr;
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
        const int difference = absolute((int)(size_)-pixels);
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
    FontMetrics actual{
        .width = (u16)(size.width),
        .height = (u16)(size.height),
    };
    if (kind_ == FontKind::Primary) {
        const int pixels = size.y_ppem > 0 ? maximum(1, rounded(size.y_ppem / 64.0)) : size.height;
        const double scale = hasColor_ ? size_ / (double)(pixels) : 1;
        actual.width = rounded(size.width * scale);
        actual.height = rounded(size.height * scale);
    }
    if (face_->height != 0) {
        actual.baseline = rounded(actual.height * (double)(face_->ascender) / face_->height);
    } else if (face_->size->metrics.ascender != 0) {
        // A bitmap-only face carries no scalable ascender; the selected
        // strike's own metrics place the baseline. Without this every
        // glyph lands above the cell and clips to nothing.
        actual.baseline = (u16)(minimum((int)(actual.height), pixels(face_->size->metrics.ascender)));
    }
    if (kind_ == FontKind::Primary) {
        metrics_ = actual;
        return;
    }
    if (kind_ == FontKind::Overlay) {
        if (metrics_.height != actual.height) {
            fail(StringBuilder() << StringView(u8"font cell height mismatch: expected ") << metrics_.height << StringView(u8", got ") << actual.height);
        }
        if (metrics_.baseline != actual.baseline) {
            fail(StringBuilder() << StringView(u8"font baseline mismatch: expected ") << metrics_.baseline << StringView(u8", got ") << actual.baseline);
        }
        return;
    }
    if (hasColor_) {
        if (face_->height != 0) {
            metrics_.baseline = rounded(metrics_.height * (double)(face_->ascender) / face_->height);
        }
        return;
    }
    // A fallback mask strike is drawn at its own size and clipped into the
    // imposed cell box; there is nothing to validate.
}

u16 FontImpl::representativeAdvance() {
    const FT_UInt glyph = FT_Get_Char_Index(face_, 'M');
    if (glyph != 0 && !FT_Load_Glyph(face_, glyph, FT_LOAD_DEFAULT)) {
        const int advance = pixels(face_->glyph->advance.x);
        if (advance > 0 && advance <= UINT16_MAX) {
            return (u16)(advance);
        }
    }
    return rounded(size_ * (double)(face_->max_advance_width) / face_->units_per_EM);
}

// Below the baseline the cell keeps every row the underscore and the
// descenders ink as the hinter places them - the strip is cut at the
// cell's bottom, so a bar a row lower is not drawn at all. The
// proportional split above leaves DejaVu Sans Mono at 15px a bar one
// row past the cell. Core Text sizes its cell from ascent + descent and
// never had the gap. The cell grows downward; the baseline stays.
void FontImpl::reserveDescent(FontMetrics& metrics) {
    int below = 0;
    for (const u8 character : StringView(u8"_gjpqy")) {
        const FT_UInt glyph = FT_Get_Char_Index(face_, character);
        if (glyph != 0 && loadGlyph(glyph, false, true, 0, 0) && face_->glyph->bitmap.rows > 0) {
            below = maximum(below, (int)(face_->glyph->bitmap.rows) - face_->glyph->bitmap_top);
        }
    }
    const int room = (int)(metrics.height) - metrics.baseline;
    if (room < below) {
        metrics.height = (u16)(metrics.height + below - room);
    }
}

void FontImpl::configureScaled() {
    if (FT_Set_Pixel_Sizes(face_, size_, size_)) {
        fail(StringView(u8"could not select scalable font size"));
    }
    if (face_->units_per_EM == 0 || face_->max_advance_width == 0 || face_->height == 0) {
        fail(StringView(u8"font has unusable scalable metrics"));
    }

    const double height = size_ * (double)(face_->height) / face_->units_per_EM + 1;
    FontMetrics actual{
        .width = representativeAdvance(),
        .height = rounded(height),
        .baseline = rounded(height * face_->ascender / face_->height),
    };
    reserveDescent(actual);
    if (kind_ == FontKind::Primary) {
        metrics_ = actual;
        return;
    }
    if (kind_ == FontKind::Overlay) {
        // The overlay draws into the primary's cell: a bolder underscore
        // a row deeper still fits there, and must not cost the style.
        if (metrics_.height < actual.height) {
            fail(StringBuilder() << StringView(u8"font cell height mismatch: expected ") << metrics_.height << StringView(u8", got ") << actual.height);
        }
        if (metrics_.baseline != actual.baseline) {
            fail(StringBuilder() << StringView(u8"font baseline mismatch: expected ") << metrics_.baseline << StringView(u8", got ") << actual.baseline);
        }
        return;
    }
    // Fallback faces impose no cell of their own: the effective pixel size
    // is chosen per cell span by fitCells.
}

namespace {
    // Ink extent over a mask canvas; false when blank. The threshold
    // skips antialiasing dust.
    static bool maskInk(const Buffer& bitmap, u32 canvas, u32 rows, u32& left, u32& right) {
        const auto* const data = (const u8*)(bitmap.data());
        left = canvas;
        right = 0;
        for (u32 row = 0; row < rows; ++row) {
            const u8* const line = data + (size_t)(row)*canvas;
            for (u32 x = 0; x < canvas; ++x) {
                if (line[x] > 8) {
                    left = x < left ? x : left;
                    right = x > right ? x : right;
                }
            }
        }
        return left <= right;
    }
}

void FontImpl::restoreSize() {
    // A fallback face re-fits its size on every applyCells; the fixed
    // kinds render at their configured size.
    if (kind_ == FontKind::Fallback || face_->num_fixed_sizes > 0) {
        return;
    }
    FT_Set_Pixel_Sizes(face_, size_, size_);
    hb_ft_font_changed(harfbuzz_);
}

// A width-one private-use pictogram with no blank to capture re-renders
// at a smaller pixel size until its unclipped ink fits its single cell,
// then centers - eza --icons with text right next door. Returns false
// when the ink already fits (or nothing sensible fits), leaving the
// normal path to render the cell.
bool FontImpl::renderFittedSymbol(u32 codepoint, u8* out, size_t stride) {
    if (face_->num_fixed_sizes > 0) {
        return false;
    }
    // The true ink extent, probed on a two-cell canvas where it is whole.
    glyphColor_ = false;
    canvasWidth_ = (u16)(2 * metrics_.width);
    if (!applyCells(2) || !rasterize(&codepoint, 1) || glyphColor_) {
        restoreSize();
        return false;
    }
    u32 left = 0;
    u32 right = 0;
    if (!maskInk(bitmap_, canvasWidth_, metrics_.height, left, right)) {
        restoreSize();
        return false;
    }
    const u32 canvas = metrics_.width;
    const u32 ink = right - left + 1;
    if (ink <= canvas + 2) {
        // Any clipping is antialiasing slop; the normal render stands.
        restoreSize();
        return false;
    }
    u16 size = (u16)(maximum(4, (int)((u64)(size_)*canvas / ink)));
    for (; size >= 4; --size) {
        if (FT_Set_Pixel_Sizes(face_, 0, size)) {
            break;
        }
        hb_ft_font_changed(harfbuzz_);
        if (!rasterize(&codepoint, 1) || glyphColor_) {
            break;
        }
        u32 fittedLeft = 0;
        u32 fittedRight = 0;
        if (!maskInk(bitmap_, canvasWidth_, metrics_.height, fittedLeft, fittedRight)) {
            break;
        }
        if (fittedRight + 1 >= canvasWidth_ || fittedRight - fittedLeft + 1 > canvas - 2) {
            continue;
        }
        const u32 fittedInk = fittedRight - fittedLeft + 1;
        const u32 shift = (canvas - fittedInk) / 2;
        const auto* const source = (const u8*)(bitmap_.data());
        for (u16 row = 0; row < metrics_.height; ++row) {
            for (u32 x = 0; x < fittedInk; ++x) {
                out[(size_t)(row)*stride + shift + x] = source[(size_t)(row)*canvasWidth_ + fittedLeft + x];
            }
        }
        restoreSize();
        return true;
    }
    restoreSize();
    return false;
}

// One harfbuzz run over a stretch of the span text - this is where
// ligatures form. The pen snaps to the cluster's grid column at every
// cluster boundary: inside a cluster the shaper's offsets and advances
// rule, but a font advance disagreeing with the cell width (JetBrains
// Mono inks 9.6px in a 10px cell) must not accumulate - half a cell of
// drift across an mc frame line tears the panels apart.
void FontImpl::drawShapedRun(const u32* codepoints, size_t begin, size_t end, const u16* columns, u8* out, size_t stride) {
    if (begin >= end) {
        return;
    }
    hb_buffer_clear_contents(shape_);
    hb_buffer_add_codepoints(shape_, (const hb_codepoint_t*)(codepoints + begin), end - begin, 0, end - begin);
    hb_buffer_guess_segment_properties(shape_);
    hb_shape(harfbuzz_, shape_, droppedLigatures, 2);
    unsigned glyphCount = 0;
    const hb_glyph_info_t* glyphs = hb_buffer_get_glyph_infos(shape_, &glyphCount);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(shape_, &glyphCount);
    hb_position_t penX = 0;
    hb_position_t penY = 0;
    u32 cluster = ~0u;
    for (unsigned index = 0; index < glyphCount; ++index) {
        if (glyphs[index].cluster != cluster) {
            cluster = glyphs[index].cluster;
            penX = (hb_position_t)((i32)(columns[begin + cluster]) * metrics_.width) << 6;
            penY = 0;
        }
        if (glyphs[index].codepoint != 0) {
            const hb_position_t positionX = penX + positions[index].x_offset;
            const hb_position_t positionY = penY + positions[index].y_offset;
            int baseX;
            int baseY;
            u32 phaseX = 0;
            u32 phaseY = 0;
            if (soft_) {
                // Subpixel placement in four phases per axis: the floor
                // pixel anchors the blit and the quantized fraction is
                // rasterized into the strike itself.
                const u32 quadX = (u32)(((positionX & 63) + 8) >> 4);
                const u32 quadY = (u32)(((positionY & 63) + 8) >> 4);
                baseX = (int)(positionX >> 6) + (int)(quadX >> 2);
                baseY = (int)(positionY >> 6) + (int)(quadY >> 2);
                phaseX = quadX & 3;
                phaseY = quadY & 3;
            } else {
                baseX = pixels(positionX);
                baseY = pixels(positionY);
            }
            GlyphStrike strike;
            if (strikeFor(glyphs[index].codepoint, phaseX, phaseY, strike)) {
                const int destinationX = baseX + strike.left;
                const int destinationY = metrics_.baseline - baseY - strike.top;
                drawStrike(strike, out, stride, destinationX, destinationY);
            }
        }
        penX += positions[index].x_advance;
        penY += positions[index].y_advance;
    }
}

// The whole span shapes as one text, split only at pictograms that need
// fit-scaling (they never ligate, and they re-render at their own pixel
// size). A pictogram with a captured blank stays in the run: its ink
// simply overflows into the blank's cell.
void FontImpl::renderShapedSpan(const u32* codepoints, size_t count, u16 cells, u8* out, size_t stride) {
    columns_.reset();
    columns_.grow(count * sizeof(u16));
    columns_.seekAbsolute(count * sizeof(u16));
    auto* const columns = (u16*)(columns_.mutData());
    size_t position = 0;
    size_t runBegin = 0;
    u16 column = 0;
    SpanCluster cluster;
    SpanCluster next;
    bool haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
    while (haveNext) {
        cluster = next;
        haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
        for (size_t index = cluster.begin; index < cluster.begin + cluster.count; ++index) {
            columns[index] = column;
        }
        if (column < cells) {
            const bool nextBlank = haveNext && next.count == 1 && codepoints[next.begin] == ' ';
            const bool symbol = cluster.cells == 1 && cluster.count == 1 && puaSymbol(codepoints[cluster.begin]);
            if (symbol && !(nextBlank && column + 1 < cells)) {
                // No blank to overflow into: scale the pictogram to its
                // cell, outside the shaped run.
                drawShapedRun(codepoints, runBegin, cluster.begin, columns, out, stride);
                if (!renderFittedSymbol(codepoints[cluster.begin], out + (size_t)(column)*metrics_.width, stride)) {
                    drawShapedRun(codepoints, cluster.begin, cluster.begin + cluster.count, columns, out, stride);
                }
                runBegin = cluster.begin + cluster.count;
            }
        }
        column = (u16)(column + cluster.cells);
    }
    drawShapedRun(codepoints, runBegin, count, columns, out, stride);
}

void FontImpl::render(const u32* codepoints, size_t count, u16 cells, void* buf) {
    const size_t stride = (size_t)(cells)*metrics_.width;
    if (!hasColor_ && kind_ != FontKind::Fallback) {
        // The mask plane of a fixed-metric face draws the whole span as
        // shaped runs; the color plane scales per cluster and fallback
        // faces re-fit their size per cluster, so both keep the loop
        // below.
        renderShapedSpan(codepoints, count, cells, (u8*)(buf), stride);
        return;
    }
    size_t position = 0;
    u16 column = 0;
    SpanCluster cluster;
    SpanCluster next;
    bool haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
    while (haveNext && column < cells) {
        cluster = next;
        haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
        u16 width = cluster.cells;
        const bool blank = cluster.count == 1 && codepoints[cluster.begin] == ' ';
        if (blank) {
            column = (u16)(column + width);
            continue;
        }
        // A pictogram followed by a blank cell owns that cell's slice too.
        const bool nextBlank = haveNext && next.count == 1 && codepoints[next.begin] == ' ';
        if (width == 1 && cluster.count == 1 && puaSymbol(codepoints[cluster.begin]) && nextBlank && column + 1 < cells) {
            width = 2;
            haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
        }
        width = (u16)(minimum(width, cells - column));
        if (width == 1 && cluster.count == 1 && !hasColor_ && puaSymbol(codepoints[cluster.begin]) && renderFittedSymbol(codepoints[cluster.begin], (u8*)(buf) + (size_t)(column)*metrics_.width, stride)) {
            column = (u16)(column + width);
            continue;
        }
        glyphColor_ = false;
        canvasWidth_ = (u16)(minimum(width, 2) * metrics_.width);
        if (applyCells((u16)(minimum(width, 2))) && rasterize(codepoints + cluster.begin, cluster.count) && glyphColor_ == hasColor_) {
            const u8* const source = (const u8*)(bitmap_.data());
            u8* const out = (u8*)(buf);
            const size_t pixel = hasColor_ ? 4 : 1;
            for (u16 row = 0; row < metrics_.height; ++row) {
                __builtin_memcpy(out + ((size_t)(row)*stride + (size_t)(column)*metrics_.width) * pixel, source + (size_t)(row)*canvasWidth_ * pixel, (size_t)(canvasWidth_)*pixel);
            }
        }
        column = (u16)(column + width);
    }
}

bool FontImpl::colored() const {
    return hasColor_;
}

bool FontImpl::covers(u32 codepoint) {
    return FT_Get_Char_Index(face_, codepoint) != 0;
}

FontFace* FontImpl::face() {
    return source_.mutPtr();
}

Font* FontImpl::synthesize(ObjPool& owner, FontStyle style) {
    FontMetrics metrics = metrics_;
    try {
        return owner.make<FontImpl>(composer_, source_, size_, FontKind::Overlay, metrics, style);
    } catch (Exception&) {
        return nullptr;
    }
}

// Loads without rendering first so a synthetic style can embolden or
// shear the outline, then renders. In soft mode the hinter stays out of
// it, the outline is darkened by the -soft strength, and frac slides it
// to its subpixel phase before the rasterizer runs.
bool FontImpl::loadGlyph(FT_UInt glyph, bool color, bool render, FT_Pos fracX, FT_Pos fracY) {
    FT_Int32 flags = FT_LOAD_DEFAULT;
    if (color) {
        flags |= FT_LOAD_COLOR;
    }
    if (soft_) {
        flags |= FT_LOAD_NO_HINTING;
    }
    if (FT_Load_Glyph(face_, glyph, flags)) {
        return false;
    }
    if (syntheticBold_) {
        FT_GlyphSlot_Embolden(face_->glyph);
    }
    if (syntheticItalic_) {
        FT_GlyphSlot_Oblique(face_->glyph);
    }
    if (face_->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (softEmbolden_ != 0) {
            FT_Outline_Embolden(&face_->glyph->outline, softEmbolden_);
        }
        if (fracX != 0 || fracY != 0) {
            FT_Outline_Translate(&face_->glyph->outline, fracX, fracY);
        }
    }
    if (render && face_->glyph->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL)) {
        return false;
    }
    return true;
}

u32 FontImpl::fitRepresentative(u16 cells) const {
    const u32 wide[] = {0x3000, 0x4e00};
    const u32 narrow[] = {'M', '0'};
    for (const u32 codepoint : cells > 1 ? wide : narrow) {
        if (FT_Get_Char_Index(face_, codepoint) != 0) {
            return codepoint;
        }
    }
    return 0;
}

FontImpl::FitMeasure FontImpl::measureAt(u16 pixelSize, u32 representative) {
    FitMeasure result;
    if (FT_Set_Pixel_Sizes(face_, 0, pixelSize)) {
        return result;
    }
    result.ascent = (int)(face_->size->metrics.ascender + 63) / 64;
    result.descent = (int)(-face_->size->metrics.descender + 63) / 64;
    result.advance = (int)(face_->size->metrics.max_advance + 63) / 64;
    if (representative != 0) {
        const FT_UInt glyph = FT_Get_Char_Index(face_, representative);
        if (glyph != 0 && !FT_Load_Glyph(face_, glyph, FT_LOAD_DEFAULT)) {
            const int advance = pixels(face_->glyph->advance.x);
            if (advance > 0) {
                result.advance = advance;
            }
        }
    }
    return result;
}

// Width-anchored fit: start where the representative advance matches the
// target span and let the engine re-render smaller until the vertical
// metrics stay inside the primary cell. The result depends only on the
// face and the span, so it is computed once per span.
u16 FontImpl::fitCells(u16 cells) {
    u16& cached = fittedSize_[cells];
    if (cached != 0) {
        return cached;
    }

    const int targetWidth = cells * metrics_.width;
    const int targetAscent = metrics_.baseline;
    const int targetDescent = metrics_.height - metrics_.baseline;
    const u32 representative = fitRepresentative(cells);
    u16 size = (u16)(maximum(1, metrics_.height));
    FitMeasure fit = measureAt(size, representative);
    if (fit.advance > 0 && fit.advance != targetWidth) {
        size = (u16)(maximum(1, size * targetWidth / fit.advance));
        fit = measureAt(size, representative);
    }
    while (size > 1 && (fit.advance > targetWidth || fit.ascent > targetAscent || fit.descent > targetDescent)) {
        --size;
        fit = measureAt(size, representative);
    }
    if (composer_.vtConfig.config->verbose && !fitLogged_) {
        sysO << StringView(u8"fitted fallback font to ") << size << StringView(u8"px for ") << (u64)(cells) << StringView(u8"-cell glyphs\n");
        fitLogged_ = true;
    }
    cached = size;
    return size;
}

bool FontImpl::applyCells(u16 cells) {
    if (kind_ != FontKind::Fallback || face_->num_fixed_sizes > 0) {
        return true;
    }
    if (FT_Set_Pixel_Sizes(face_, 0, fitCells(cells))) {
        return false;
    }
    hb_ft_font_changed(harfbuzz_);
    return true;
}

void FontImpl::drawMask(const FT_Bitmap& source, int destinationX, int destinationY) {
    drawMaskInto((u8*)(bitmap_.mutData()), canvasWidth_, source, destinationX, destinationY);
}

void FontImpl::drawMaskInto(u8* destination, size_t canvas, const FT_Bitmap& source, int destinationX, int destinationY) {
    const int sourceWidth = source.width;
    const int sourceHeight = source.rows;
    const int sourceX = maximum(0, -destinationX);
    const int sourceY = maximum(0, -destinationY);
    destinationX = maximum(0, destinationX);
    destinationY = maximum(0, destinationY);
    const int copyWidth = minimum(sourceWidth - sourceX, (int)(canvas)-destinationX);
    const int copyHeight = minimum(sourceHeight - sourceY, (int)(metrics_.height) - destinationY);
    if (copyWidth <= 0 || copyHeight <= 0) {
        return;
    }

    const int pitch = source.pitch;
    const int rowStride = absolute(pitch);
    for (int row = 0; row < copyHeight; ++row) {
        const int sourceRow = sourceY + row;
        const int storedRow = pitch < 0 ? sourceHeight - sourceRow - 1 : sourceRow;
        const u8* sourcePixels = (const u8*)(source.buffer + storedRow * rowStride);
        u8* destinationPixels = destination + (size_t)(destinationY + row) * canvas + destinationX;
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

// The composer memo in front of the rasterizer. The key packs this
// font's namespace with the applied pixel size - a fallback face re-fits
// per span width - the soft subpixel phases, and the glyph id; ids past
// 24 bits render uncached rather than colliding. Hits and misses both
// come out as tight gray rows, so the blit is one code path and
// bit-identical either way.
bool FontImpl::strikeFor(FT_UInt glyph, u32 phaseX, u32 phaseY, GlyphStrike& strike) {
    GlyphCache* const cache = composer_.glyphs;
    const bool cacheable = cache != nullptr && glyph < (1u << 24);
    u64 key = 0;
    if (cacheable) {
        if (strikeNamespace_ == 0) {
            strikeNamespace_ = cache->makeNamespace();
        }
        key = ((u64)(strikeNamespace_) << 40) | ((u64)(face_->size->metrics.x_ppem & 0xfffu) << 28) | ((u64)(phaseX) << 26) | ((u64)(phaseY) << 24) | glyph;
        if (cache->find(key, strike)) {
            return true;
        }
    }
    const FT_Pos fracX = (FT_Pos)(phaseX) << 4;
    const FT_Pos fracY = (FT_Pos)(phaseY) << 4;
    if (loadGlyph(glyph, false, true, fracX, fracY)) {
        const FT_Bitmap& bitmap = face_->glyph->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
            return false;
        }
        const size_t bytes = (size_t)(bitmap.width) * bitmap.rows;
        strike_.reset();
        strike_.grow(bytes);
        strike_.seekAbsolute(bytes);
        u8* const rows = (u8*)(strike_.mutData());
        const int rowStride = absolute(bitmap.pitch);
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            const unsigned storedRow = bitmap.pitch < 0 ? bitmap.rows - row - 1 : row;
            const u8* const source = (const u8*)(bitmap.buffer) + storedRow * rowStride;
            u8* const destination = rows + (size_t)(row)*bitmap.width;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                __builtin_memcpy(destination, source, bitmap.width);
            } else {
                for (unsigned column = 0; column < bitmap.width; ++column) {
                    destination[column] = source[column >> 3] & (0x80 >> (column & 7)) ? 0xff : 0;
                }
            }
        }
        strike.data = rows;
        strike.width = (u16)(bitmap.width);
        strike.height = (u16)(bitmap.rows);
        strike.left = (i16)(face_->glyph->bitmap_left);
        strike.top = (i16)(face_->glyph->bitmap_top);
    } else if (!strikeFromOutline(glyph, fracX, fracY, strike)) {
        return false;
    }
    if (cacheable) {
        cache->insert(key, strike);
    }
    return true;
}

// FreeType 2.14.2 grew a glyph-bomb guard: the slot refuses to render
// anything wider or taller than ten times the ppem, and U+FDFD at its
// natural size trips it. The raster itself has no such limit - render
// the loaded outline into the strike buffer directly, with our own
// bound standing in for the one we sidestep.
bool FontImpl::strikeFromOutline(FT_UInt glyph, FT_Pos fracX, FT_Pos fracY, GlyphStrike& strike) {
    if (!loadGlyph(glyph, false, false, fracX, fracY)) {
        return false;
    }
    if (face_->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        return false;
    }
    FT_Outline& outline = face_->glyph->outline;
    FT_BBox box;
    FT_Outline_Get_CBox(&outline, &box);
    const FT_Pos xMin = box.xMin & ~63;
    const FT_Pos yMin = box.yMin & ~63;
    const FT_Pos xMax = (box.xMax + 63) & ~63;
    const FT_Pos yMax = (box.yMax + 63) & ~63;
    const FT_Pos width = (xMax - xMin) >> 6;
    const FT_Pos height = (yMax - yMin) >> 6;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return false;
    }
    const size_t bytes = (size_t)(width)*height;
    strike_.reset();
    strike_.grow(bytes);
    strike_.seekAbsolute(bytes);
    u8* const rows = (u8*)(strike_.mutData());
    __builtin_memset(rows, 0, bytes);
    FT_Bitmap bitmap{};
    bitmap.width = (unsigned)(width);
    bitmap.rows = (unsigned)(height);
    bitmap.pitch = (int)(width);
    bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
    bitmap.num_grays = 256;
    bitmap.buffer = (unsigned char*)(rows);
    FT_Outline_Translate(&outline, -xMin, -yMin);
    if (FT_Outline_Get_Bitmap(face_->glyph->library, &outline, &bitmap) != 0) {
        return false;
    }
    strike.data = rows;
    strike.width = (u16)(width);
    strike.height = (u16)(height);
    strike.left = (i16)(xMin >> 6);
    strike.top = (i16)(yMax >> 6);
    return true;
}

void FontImpl::drawStrike(const GlyphStrike& strike, u8* out, size_t stride, int destinationX, int destinationY) {
    FT_Bitmap view;
    view.rows = strike.height;
    view.width = strike.width;
    view.pitch = (int)(strike.width);
    view.buffer = (unsigned char*)(strike.data);
    view.num_grays = 256;
    view.pixel_mode = FT_PIXEL_MODE_GRAY;
    view.palette_mode = 0;
    view.palette = nullptr;
    drawMaskInto(out, stride, view, destinationX, destinationY);
}

bool FontImpl::rasterizeMask(const hb_glyph_info_t* glyphs, const hb_glyph_position_t* positions, unsigned count) {
    bitmap_.zero((size_t)(canvasWidth_)*metrics_.height);
    hb_position_t penX = 0;
    hb_position_t penY = 0;
    if (kind_ == FontKind::Fallback) {
        // A fitted glyph can come out narrower than its span; center it.
        hb_position_t total = 0;
        for (unsigned index = 0; index < count; ++index) {
            total += positions[index].x_advance;
        }
        const hb_position_t canvas = (hb_position_t)(canvasWidth_) << 6;
        if (total > 0 && total < canvas) {
            penX = (canvas - total) / 2;
        }
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!loadGlyph(glyphs[index].codepoint, false, true, 0, 0)) {
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
    auto* destination = (u8*)(sourceBitmap_.mutData());
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
    bitmap_.zero((size_t)(canvasWidth_)*metrics_.height * 4);
    double scale = minimum(canvasWidth_, metrics_.height) / (double)(maximum(sourceWidth, sourceHeight));
    if (scale > 1) {
        scale = 1;
    }
    const int targetWidth = maximum(1, rounded(sourceWidth * scale));
    const int targetHeight = maximum(1, rounded(sourceHeight * scale));
    const int originX = ((int)(canvasWidth_)-targetWidth) / 2;
    const int originY = ((int)(metrics_.height) - targetHeight) / 2;
    const auto* source = (const u8*)(sourceBitmap_.data());
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
            u8* target = destination + 4 * ((size_t)(originY + y) * canvasWidth_ + originX + x);
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
        if (!loadGlyph(glyphs[index].codepoint, true, true, 0, 0)) {
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

    sourceBitmap_.zero((size_t)(sourceWidth)*sourceHeight * 4);
    penX = 0;
    penY = 0;
    for (unsigned index = 0; index < count; ++index) {
        if (!loadGlyph(glyphs[index].codepoint, true, true, 0, 0)) {
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
        if (!loadGlyph(0, true, true, 0, 0)) {
            return false;
        }
        glyphColor_ = face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
        return glyphColor_ ? rasterizeColor(&missing, &missingPosition, 1) : rasterizeMask(&missing, &missingPosition, 1);
    }

    hb_buffer_clear_contents(shape_);
    hb_buffer_add_codepoints(shape_, (const hb_codepoint_t*)(codepoints), count, 0, count);
    hb_buffer_guess_segment_properties(shape_);
    hb_shape(harfbuzz_, shape_, droppedLigatures, 2);
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
        if (!loadGlyph(glyphs[index].codepoint, true, true, 0, 0)) {
            return false;
        }
        if (face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            glyphColor_ = true;
        }
    }
    return glyphColor_ ? rasterizeColor(glyphs, positions, glyphCount) : rasterizeMask(glyphs, positions, glyphCount);
}

Font* FreeTypeRenderer::render(ObjPool& owner, IntrusivePtr<FontFace> face, u16 pixels, FontKind kind, FontMetrics& metrics) {
    Font* const font = owner.make<FontImpl>(composer, face, pixels, kind, metrics, FontStyle::Regular);
    if (composer.vtConfig.config->verbose) {
        sysO << StringView(u8"freetype face: kind ") << (u64)((u8)(kind)) << StringView(u8" at ") << pixels << StringView(u8"px, cell ") << metrics.width << StringView(u8"x") << metrics.height << StringView(u8" baseline ") << metrics.baseline << StringView(u8"\n");
    }
    return font;
}

FontRenderer* createFreeTypeFontRenderer(Composer& composer) {
    return composer.pool->make<FreeTypeRenderer>(composer);
}
