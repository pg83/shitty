/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render_reference.h"

#include "options.h"
#include "composer.h"
#include "font_pack.h"
#include "span_shaper.h"
#include "render_blend.h"
#include "grid_geometry.h"
#include "render_synthesis.h"

#include <lib/vterm/hex.h>
#include <lib/vterm/vterm.h>
#include <lib/vterm/screen.h>
#include <lib/vterm/vt_test.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/alg/xchg.h>
#include <std/str/view.h>
#include <std/dbg/assert.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <string.h>
#include <plt/window.h>
#include <plt/platform_headless.h>

using namespace stl;

namespace {
    template <typename T>
    static void resizeVector(Vector<T>& vector, size_t count) {
        while (vector.length() > count) {
            vector.popBack();
        }
        while (vector.length() < count) {
            vector.pushBack(T{});
        }
    }

    // The slice of one arena strip a cell renders: base is a byte offset
    // into the renderer's copy of the strips, stride the strip width in
    // pixels.
    struct CellStrip {
        u32 base = 0;
        u32 stride = 0;
        u8 kind = 0;
    };

    static constexpr u8 stripNone = 0;
    static constexpr u8 stripMask = 1;
    static constexpr u8 stripColor = 2;
    static constexpr u8 stripSynthesized = 3;

    struct ReferenceCell {
        TerminalCell source{};
        Color foreground;
        Color background;
        Color underlineColor;
        u32 hyperlink = 0;
        u32 grapheme = 0;
        u8 lineAttribute = 0;
    };

    template <typename Cell>
    static unsigned cellUnderline(const Cell& cell) {
        return cell.underline;
    }

    template <>
    unsigned cellUnderline(const TerminalCell& cell) {
        return cell.underlined();
    }

    template <typename Cell>
    static unsigned cellFlags(const Cell& cell, u8 lineAttribute) {
        return (cell.dwidth << 0) | (cell.dwidth_cont << 1) | (cell.bold << 2) | (cell.italic << 3) | (cellUnderline(cell) << 4) | (cell.inverse << 5) | (cell.wrap << 6) | (cell.faint << 7) | (cell.blink << 8) | (cell.conceal << 9) | (cell.strike << 10) | (cell.overline << 11) | (cell.underline_style << 12) | ((cell.protected_char != 0) << 15) | (lineAttribute << 16) | (cell.drawn << 18);
    }

    static unsigned cellFlags(const ReferenceCell& cell) {
        return cellFlags(cell.source, cell.lineAttribute);
    }

    // A6-3: one pane's grid inside the retained store - which pane it
    // is, what shape it had, and where its cells begin. The panes lie
    // back to back in one vector, the way the Metal backend lays them
    // out, so a frame retains every pane it drew instead of the last one
    // of them.
    struct PaneRetain {
        // The Screen the pane shapes through, as an opaque handle: the
        // same identity the arena mirror keys on (render_arena.h). A
        // frame whose panes changed identity has nothing to retain, even
        // when it has as many panes of the same shape as the one before
        // it.
        const void* shapes = nullptr;
        u16 columns = 0;
        u16 rows = 0;
        size_t offset = 0;
    };

    struct ModelDigest {
        u64 first = 14695981039346656037ull;
        u64 second = 1099511628211ull;

        void add(u64 value) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                const u8 byte = (u8)(value >> shift);
                first = (first ^ byte) * 1099511628211ull;
                second = (second ^ (byte + 0x9d)) * 14029467366897019727ull;
            }
        }
    };

    struct ReferenceRendererImpl final: ReferenceRenderer {
        ReferenceRendererImpl(Composer& composer, const plt::RenderContext& context);

        bool update(const PaneUpdate* panes, size_t count) override;
        bool update(const TerminalUpdate& update) override;
        void setSeams(const PixelRect* seams, size_t count, Color ink) override;
        bool reshape(const PaneUpdate* panes, size_t count);
        bool updateOnce(const PaneUpdate& pane, size_t index);
        bool repaint() override;
        void attach(TestApi& testApi) override;
        ReferenceImage image() const override;
        TerminalUpdate renderUpdate() const override;
        void snapshot(Buffer& out) const override;
        void modelSnapshot(Buffer& out) const override;
        void modelDigest(Buffer& out) const override;
        void renderState(Buffer& out) const override;
        void selectionState(Buffer& out) const override;
        void scrollbackState(Buffer& out) const override;
        void screenText(Buffer& out) const override;
        void lastUpdate(Buffer& out) const override;
        void lastUpdateRows(Buffer& out) const override;
        void resetUpdateStats() override;
        u16 columns() const override;
        u16 rows() const override;
        u32 historyRows() const override;
        u32 hoveredHyperlink() const override;
        u32 hoveredLinkBegin() const override;
        u32 hoveredLinkEnd() const override;

        static int minimum(int left, int right);
        static int maximum(int left, int right);
        static bool selected(const TerminalUpdate& update, const TerminalCell& cell, int column, int row);
        static u8 mix(u8 foreground, u8 background, u8 coverage);
        static Color blend(Color foreground, Color background, u8 coverage);
        static bool sameColor(Color left, Color right);
        bool targetReady() const;
        // T10: the alpha the rectangle is filled at, and the reason
        // this takes one at all - the seam between two panes is filled
        // by the same call and must stay solid while the panes it
        // separates go see-through.
        void clearPane(Color background, u8 alpha);
        // The background's alpha for this frame, from -backgroundOpacity.
        u8 backgroundAlpha() const;
        void paintSeams();
        void putPixel(int x, int y, Color color);
        ReferenceCell materialize(const TerminalCell& cell, u8 lineAttribute, const TerminalColors& colors) const;
        void captureStrips(const TerminalUpdate& update);
        void captureSpan(SpanShaper& shaper, u16 columns, u16 row, const ScreenRowSpan& span);
        void renderCell(const TerminalUpdate& update, const ReferenceCell& cell, u16 columns, u16 column, u16 row, const Insets& insets);
        bool render(const TerminalUpdate& update, const ReferenceCell* cells, u16 columns, u16 rows, const PixelRect& area);
        void captureModel();
        void captureState(const TerminalUpdate& update, const ReferenceCell* cells, size_t count);

        // A6-3: the retained cells of the pane drawn last - what the
        // probes below (the snapshots, the model digest, renderUpdate())
        // answer with. They are questions about a terminal, not about a
        // window, and the terminal they mean is the one drawn last, as
        // it was before panes existed.
        const ReferenceCell* retained() const;
        size_t retainedCount() const;

        Composer& composer_;
        plt::HeadlessRenderTarget* target_;
        // A2: the pane being drawn - where its grid starts and the only
        // pixels it may touch. repaint() redraws the last pane through
        // it. A pane-less update() makes this the whole surface, which
        // is what every pixel this renderer placed before panes existed
        // was clipped to anyway.
        PixelRect pane_;
        // The pane rectangle met with the target, as putPixel wants to
        // ask it: origin and extent, so each axis is one unsigned
        // compare (a coordinate below the origin wraps past the extent).
        // The shape of this test is worth the trouble - it runs once per
        // pixel of every cell of every frame, and a pane clip *added* to
        // the target clip, as eight comparisons where there were four,
        // cost 18% of the frame of an 80x24 grid when measured.
        int clipLeft_ = 0;
        int clipTop_ = 0;
        u32 clipWidth_ = 0;
        u32 clipHeight_ = 0;
        // F9: the seams of the frame being drawn, and their colour.
        Vector<PixelRect> seams_;
        Color seamInk_;
        Buffer coverage_;
        Buffer color_;
        Buffer stripStore_;
        Vector<CellStrip> cellStrips_;
        Vector<ScreenRowSpan> spanScratch_;
        // Every pane's retained grid, one after another; panes_ says
        // which pane owns which run. nextCells_ is the frame being
        // built: a whole-store copy that only replaces the retain when
        // every pane of the frame drew, so a refused frame retains what
        // the last accepted one did.
        Vector<ReferenceCell> cells_;
        Vector<ReferenceCell> nextCells_;
        Vector<PaneRetain> panes_;
        Vector<TerminalCell> modelCells_;
        Vector<u8> modelLineAttributes_;

        // Grapheme codepoints of every cell, flattened: the slice list refers
        // into one shared store, sidestepping a vector-of-vectors.
        struct GraphemeSlice {
            u32 offset;
            u32 count;
        };

        Vector<GraphemeSlice> cellGraphemes_;
        Vector<u32> graphemeStore_;
        Vector<CellColor> modelUnderlineColors_;
        mutable Vector<TerminalRow> renderRows_;
        mutable Vector<TerminalCell> renderCells_;
        Vector<u16> lastUpdateRows_;
        TestApi* testApi_ = nullptr;
        const TerminalColors* colors_ = nullptr;
        TerminalCursor cursor_;
        Rect selection_;
        Rect snappedSelection_;
        Color selectionForeground_;
        Color selectionBackground_;
        u32 viewOffset_ = 0;
        u32 historyRows_ = 0;
        u32 hoveredHyperlink_ = 0;
        u32 hoveredLinkBegin_ = 0;
        u32 hoveredLinkEnd_ = 0;
        u64 refreshCount_ = 0;
        size_t graphemeCells_ = 0;
        size_t graphemeCodepoints_ = 0;
        size_t lastUpdateCells_ = 0;
        size_t lastUpdateSpans_ = 0;
        // Where the pane drawn last begins in cells_, and the shape it
        // was drawn with: the pair the probes read through retained().
        size_t lastOffset_ = 0;
        u16 columns_ = 0;
        u16 rows_ = 0;
        u8 selectionColorMask_ = 0;
        bool hasColor_ = false;
        bool screenReverse_ = false;
        bool blinkVisible_ = true;
        bool cursorBlink_ = false;
        bool havePresentation_ = false;
    };
}

ReferenceRendererImpl::ReferenceRendererImpl(Composer& composer, const plt::RenderContext& context)
    : composer_(composer)
    , target_(static_cast<plt::HeadlessRenderTarget*>(context.window))
{
    STD_ASSERT(context.backend == plt::RenderBackend::Headless);
}

int ReferenceRendererImpl::minimum(int left, int right) {
    return left < right ? left : right;
}

int ReferenceRendererImpl::maximum(int left, int right) {
    return left > right ? left : right;
}

bool ReferenceRendererImpl::sameColor(Color left, Color right) {
    return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

bool ReferenceRendererImpl::selected(const TerminalUpdate& update, const TerminalCell& cell, int column, int row) {
    const Rect& selection = update.snappedSelection;
    if (selection.empty()) {
        return false;
    }
    const auto contains = [&](int x) {
        if (selection.rectangular) {
            return row >= selection.tl.y && row <= selection.br.y && x >= selection.tl.x && x < selection.br.x;
        }
        return (row > selection.tl.y && row < selection.br.y) || (row == selection.tl.y && x >= selection.tl.x && (row < selection.br.y || x < selection.br.x)) || (row == selection.br.y && x < selection.br.x && (row > selection.tl.y || x > selection.tl.x));
    };
    return contains(column) || (cell.dwidth && contains(column + 1));
}

u8 ReferenceRendererImpl::mix(u8 foreground, u8 background, u8 coverage) {
    return (u8)(((unsigned)(foreground)*coverage + (unsigned)(background) * (255 - coverage) + 127) / 255);
}

Color ReferenceRendererImpl::blend(Color foreground, Color background, u8 coverage) {
    return {
        mix(foreground.red, background.red, coverage),
        mix(foreground.green, background.green, coverage),
        mix(foreground.blue, background.blue, coverage),
    };
}

bool ReferenceRendererImpl::targetReady() const {
    if (target_ == nullptr || target_->pixels == nullptr || target_->format != plt::HeadlessPixelFormat::RGB8) {
        return false;
    }
    if (target_->width != composer_.geometry.pixelWidth || target_->height != composer_.geometry.pixelHeight || target_->stride < target_->width * 3) {
        return false;
    }
    return target_->length >= (size_t)(target_->stride) * target_->height;
}

void ReferenceRendererImpl::setSeams(const PixelRect* seams, size_t count, Color ink) {
    seams_.clear();
    for (size_t at = 0; at < count; ++at) {
        seams_.pushBack(seams[at]);
    }
    seamInk_ = ink;
}

void ReferenceRendererImpl::paintSeams() {
    // F9: last, and on purpose. The band lies in the air two neighbours
    // leave between their grids, which each of them has just cleared with
    // its own background - so the seam has to go on after both, or the
    // second pane would wipe half of it.
    for (const PixelRect& seam : seams_) {
        clipLeft_ = seam.x;
        clipTop_ = seam.y;
        const u32 right = min<u32>(target_->width, (u32)(seam.x) + seam.width);
        const u32 bottom = min<u32>(target_->height, (u32)(seam.y) + seam.height);
        clipWidth_ = right > (u32)(seam.x) ? right - seam.x : 0;
        clipHeight_ = bottom > (u32)(seam.y) ? bottom - seam.y : 0;
        clearPane(seamInk_, 255);
    }
}

u8 ReferenceRendererImpl::backgroundAlpha() const {
    return backgroundAlphaFromPercent(composer_.opts->backgroundOpacity);
}

void ReferenceRendererImpl::clearPane(Color background, u8 alpha) {
    // A2: the pane's rectangle, not the frame's. A pane carries its own
    // background (its terminal's live OSC 11), so a clear that covered
    // the whole target would paint over every pane drawn before it with
    // this pane's colour. One pane filling the surface clears the
    // surface, which is what this did before there were panes.
    //
    // T10: premultiplied, like every colour that leaves this renderer.
    // The target has no alpha channel to carry the other half - a
    // headless RGB buffer has no desktop behind it - but the colour
    // bytes are the same ones the shader writes, which is what keeps
    // the two comparable at all.
    const Color fill = premultiply(background, alpha);
    const int bottom = clipTop_ + (int)(clipHeight_);
    const int right = clipLeft_ + (int)(clipWidth_);
    for (int y = clipTop_; y < bottom; ++y) {
        u8* row = target_->pixels + (size_t)(y)*target_->stride;
        for (int x = clipLeft_; x < right; ++x) {
            row[3 * x] = fill.red;
            row[3 * x + 1] = fill.green;
            row[3 * x + 2] = fill.blue;
        }
    }
}

void ReferenceRendererImpl::putPixel(int x, int y, Color color) {
    // A2: outside the pane is another pane's business. The grid fits
    // inside the pane by construction, but a double-width line in the
    // last column draws one glyph box further right than its own cell,
    // which is a pixel of padding today and the neighbour's first column
    // once panes exist. The GPU backends clip the same way, by handing
    // the shader the pane's edge as the output bounds it already tests.
    //
    // The pane's rectangle replaces the target's here rather than being
    // asked after it: the bounds below are already the two met (see
    // clipLeft_), so a pane cannot escape the target either.
    if ((u32)(x - clipLeft_) >= clipWidth_ || (u32)(y - clipTop_) >= clipHeight_) {
        return;
    }
    u8* const pixel = target_->pixels + (size_t)(y)*target_->stride + 3 * x;
    pixel[0] = color.red;
    pixel[1] = color.green;
    pixel[2] = color.blue;
}

void ReferenceRendererImpl::captureSpan(SpanShaper& shaper, u16 columns, u16 row, const ScreenRowSpan& span) {
    if (span.end <= span.begin || span.end > columns) {
        return;
    }
    if (span.missing) {
        // A synthesized run: renderCell draws its coverage from the
        // codepoint, matching the GPU shader.
        for (u16 column = span.begin; column < span.end; ++column) {
            cellStrips_.mut((size_t)(row)*columns + column) = {0, 0, stripSynthesized};
        }
        return;
    }
    const u16 width = composer_.geometry.cellPixelWidth;
    const size_t pixels = (size_t)(span.end - span.begin) * width * composer_.geometry.cellPixelHeight;
    const size_t pixel = span.color ? sizeof(u32) : 1;
    const u8* source;
    if (span.color) {
        // spanColorUsed counts u32 pixels, matching the offsets.
        if (span.offset + pixels > shaper.spanColorUsed()) {
            return;
        }
        source = (const u8*)(shaper.spanColor() + span.offset);
    } else {
        if (span.offset + pixels > shaper.spanMaskUsed()) {
            return;
        }
        source = shaper.spanMask() + span.offset;
    }
    const size_t base = stripStore_.used();
    stripStore_.append(source, pixels * pixel);
    for (u16 column = span.begin; column < span.end; ++column) {
        cellStrips_.mut((size_t)(row)*columns + column) = {
            (u32)(base + (size_t)(column - span.begin) * width * pixel),
            (u32)(span.end - span.begin) * width,
            span.color ? stripColor : stripMask,
        };
    }
}

void ReferenceRendererImpl::captureStrips(const TerminalUpdate& update) {
    // A9: the pane's own grid, which is the shape of the cells this
    // update carries - not the window's.
    const u16 columns = update.gridColumns;
    const size_t count = (size_t)(columns)*update.gridRows;
    cellStrips_.clear();
    cellStrips_.zero(count);
    stripStore_.reset();
    if (update.shapes == nullptr || composer_.shaper == nullptr) {
        return;
    }
    Screen& shapes = *update.shapes;
    SpanShaper& shaper = *composer_.shaper;
    resizeVector(spanScratch_, columns);
    // Shaping a row can reset the arenas and move every strip shaped so
    // far; the byte copies stay valid, the held offsets do not, so redo
    // the pass until it completes within one arena generation.
    u32 generation;
    do {
        generation = shaper.spanGeneration();
        memset(cellStrips_.mutData(), 0, cellStrips_.length() * sizeof(CellStrip));
        stripStore_.reset();
        if (update.shapeFromCells) {
            // Retained cells re-rendered through a foreign fontpack (the
            // RENDER_IMAGE probe): every row shapes its own cells, the
            // screen rows do not participate.
            for (size_t index = 0; index < update.rowCount; ++index) {
                const TerminalRow& row = update.rows[index];
                const size_t spans = shaper.shapeCells(row.cells, columns, 0, spanScratch_.mutData());
                for (size_t entry = 0; entry < spans; ++entry) {
                    captureSpan(shaper, columns, row.row, spanScratch_[entry]);
                }
            }
            continue;
        }
        for (u16 row = 0; row < update.gridRows; ++row) {
            const ScreenRowRef rowRef = shapes.viewRow(row);
            const size_t spans = shaper.rowSpans(rowRef.cells, columns, rowRef.id, spanScratch_.mutData());
            for (size_t index = 0; index < spans; ++index) {
                captureSpan(shaper, columns, row, spanScratch_[index]);
            }
        }
        if (update.overlayCount != 0) {
            // The preedit preview covers the underlying strips wholesale:
            // its blank cells hide the text below them.
            const size_t base = (size_t)(update.overlayRow) * columns + update.overlayColumn;
            for (u16 index = 0; index < update.overlayCount; ++index) {
                cellStrips_.mut(base + index) = {};
            }
            const size_t spans = shaper.shapeCells(update.overlayCells, update.overlayCount, update.overlayColumn, spanScratch_.mutData());
            for (size_t index = 0; index < spans; ++index) {
                captureSpan(shaper, columns, update.overlayRow, spanScratch_[index]);
            }
        }
    } while (generation != shaper.spanGeneration());
}

ReferenceCell ReferenceRendererImpl::materialize(const TerminalCell& cell, u8 lineAttribute, const TerminalColors& colors) const {
    ReferenceCell result;
    result.source = cell;
    result.foreground = colors.resolveForeground(cell);
    result.background = colors.resolveBackground(cell);
    result.underlineColor = result.foreground;
    result.lineAttribute = lineAttribute;
    if (cell.hasExtra()) {
        const CellExtraView extra = composer_.extras.store->view(cell);
        result.hyperlink = extra.hyperlinkDisplayId;
        result.grapheme = extra.grapheme.empty() ? 0 : cell.extraRef();
        if (extra.underlineColor != cell.foreground()) {
            result.underlineColor = colors.resolve(extra.underlineColor);
        }
    } else if (cell.inlineUnderlineColor() != cell.foreground()) {
        result.underlineColor = colors.resolve(cell.inlineUnderlineColor());
    }
    return result;
}

void ReferenceRendererImpl::renderCell(const TerminalUpdate& update, const ReferenceCell& cell, u16 columns, u16 column, u16 row, const Insets& insets) {
    const TerminalCell& source = cell.source;
    const bool doubleLine = cell.lineAttribute != 0;
    const int cellWidth = composer_.geometry.cellPixelWidth;
    const int cellHeight = composer_.geometry.cellPixelHeight;
    coverage_.zero((size_t)(cellWidth)*cellHeight);
    color_.zero((size_t)(cellWidth)*cellHeight * 4);
    hasColor_ = false;
    const CellStrip strip = cellStrips_[(size_t)(row)*columns + column];
    if (strip.kind == stripSynthesized) {
        for (int y = 0; y < cellHeight; ++y) {
            for (int x = 0; x < cellWidth; ++x) {
                const float value = synthesizedCoverage(source.uc_pt, x, y, cellWidth, cellHeight, composer_.boxDrawingStroke());
                ((u8*)(coverage_.mutData()))[(size_t)(y)*cellWidth + x] = (u8)(value * 255.0f + 0.5f);
            }
        }
    } else if (strip.kind != stripNone) {
        const auto* store = (const u8*)(stripStore_.data());
        for (int y = 0; y < cellHeight; ++y) {
            // Double-size lines pixel-double the strip slice; the arenas
            // keep single-density strips only.
            const int sourceY = cell.lineAttribute == 2 ? (y + cellHeight) / 2 : cell.lineAttribute == 3 ? y / 2 : y;
            for (int x = 0; x < cellWidth; ++x) {
                const int sourceX = doubleLine ? x / 2 : x;
                const size_t sourceIndex = (size_t)(sourceY)*strip.stride + sourceX;
                if (strip.kind == stripMask) {
                    ((u8*)(coverage_.mutData()))[(size_t)(y)*cellWidth + x] = store[strip.base + sourceIndex];
                } else {
                    hasColor_ = true;
                    memcpy((u8*)(color_.mutData()) + 4 * ((size_t)(y)*cellWidth + x), store + strip.base + 4 * sourceIndex, 4);
                }
            }
        }
    }

    Color foreground = cell.foreground;
    Color background = cell.background;
    if ((source.inverse != 0) != update.screenReverse) {
        xchg(foreground, background);
    }
    const bool selectedCell = selected(update, source, column, row);
    if (selectedCell) {
        if (update.selectionColorMask == 0) {
            xchg(foreground, background);
        } else {
            if (update.selectionColorMask & 1) {
                foreground = update.selectionForeground;
            }
            if (update.selectionColorMask & 2) {
                background = update.selectionBackground;
            }
        }
    }
    if (source.faint) {
        foreground = blend(foreground, background, 128);
    }
    if (source.conceal || (source.blink && !update.blinkVisible)) {
        foreground = background;
    }

    Color cursor = update.cursor.color;
    if (sameColor(cursor, background)) {
        cursor = {(u8)(255 - cursor.red), (u8)(255 - cursor.green), (u8)(255 - cursor.blue)};
    }
    const bool cursorHere = column == update.cursor.posX && row == update.cursor.posY && (!update.cursorBlink || update.blinkVisible);
    if (cursorHere && update.cursor.style == TerminalCursor::Style::filled_block) {
        if (!(selectedCell || source.inverse) || !composer_.opts->cursorKeepSelectionFg) {
            foreground = background;
        }
        background = cursor;
    }

    // T10. Only the *background* goes translucent, and these two are not
    // it even though they arrive as one: a selection and a block cursor
    // are marks placed on top of the text, and a mark you can see the
    // desktop through stops marking anything. Everything else the shader
    // treats as background - an SGR colour, a reversed cell - follows
    // the option, because there is no way to tell "the default
    // background" apart from "a colour that happens to equal it" by the
    // time either reaches a pixel.
    const bool solidCell = selectedCell || (cursorHere && update.cursor.style == TerminalCursor::Style::filled_block);
    const u8 cellAlpha = solidCell ? (u8)(255) : backgroundAlpha();
    const Color premultipliedBackground = premultiply(background, cellAlpha);

    // The insets place the grid inside the pane; the pane places it on
    // the surface. cellOrigin() stays the one place that pairs `left`
    // with x and `top` with y (R4-test, wave 3's debt).
    const CellOrigin origin = cellOrigin(column, row, insets, composer_.geometry.cellPixelWidth, composer_.geometry.cellPixelHeight);
    const int outputX = (int)(pane_.x) + origin.x;
    const int outputY = (int)(pane_.y) + origin.y;
    const auto* coverage = (const u8*)(coverage_.data());
    const auto* color = (const u8*)(color_.data());
    const bool hidden = source.conceal || (source.blink && !update.blinkVisible);
    for (int y = 0; y < cellHeight; ++y) {
        for (int x = 0; x < cellWidth; ++x) {
            const size_t index = (size_t)(y)*cellWidth + x;
            if (hasColor_ && !hidden && color[4 * index + 3] != 0) {
                const unsigned strength = source.faint ? 128 : 255;
                const unsigned alpha = (unsigned)(color[4 * index + 3]) * strength / 255;
                putPixel(
                    outputX + x,
                    outputY + y,
                    {
                        (u8)((unsigned)(color[4 * index]) * strength / 255 + (unsigned)(premultipliedBackground.red) * (255 - alpha) / 255),
                        (u8)((unsigned)(color[4 * index + 1]) * strength / 255 + (unsigned)(premultipliedBackground.green) * (255 - alpha) / 255),
                        (u8)((unsigned)(color[4 * index + 2]) * strength / 255 + (unsigned)(premultipliedBackground.blue) * (255 - alpha) / 255),
                    }
                );
            } else {
                putPixel(outputX + x, outputY + y, blendOverBackground(foreground, background, coverage[index], cellAlpha).color);
            }
        }
    }

    const u32 cellIndex = (u32)(row)*columns + column;
    const bool explicitLink = cell.hyperlink != 0 && cell.hyperlink == update.hoveredHyperlink;
    const bool plainLink = cellIndex >= update.hoveredLinkBegin && cellIndex < update.hoveredLinkEnd;
    const bool hyperlinkUnderline = !source.underlined() && (explicitLink || plainLink);
    if (source.underlined() || hyperlinkUnderline) {
        const u8 underlineStyle = hyperlinkUnderline ? 1 : source.underline_style;
        const Color underlineColor = hyperlinkUnderline ? foreground : cell.underlineColor;
        for (int x = 0; x < cellWidth; ++x) {
            const bool draw = underlineStyle != 4 || (x & 1) == 0;
            const bool patterned = underlineStyle != 5 || x % 6 < 4;
            const int waveY = underlineStyle == 3 ? x & 1 : 0;
            if (draw && patterned) {
                putPixel(outputX + x, outputY + cellHeight - 1 - waveY, underlineColor);
            }
            if (underlineStyle == 2 && cellHeight > 2) {
                putPixel(outputX + x, outputY + cellHeight - 3, underlineColor);
            }
        }
    }
    if (source.strike) {
        for (int x = 0; x < cellWidth; ++x) {
            putPixel(outputX + x, outputY + cellHeight / 2, foreground);
        }
    }
    if (source.overline) {
        for (int x = 0; x < cellWidth; ++x) {
            putPixel(outputX + x, outputY, foreground);
        }
    }
    if (composer_.opts->showWraps && source.wrap) {
        for (int y = 0; y < cellHeight; y += 2) {
            putPixel(outputX + cellWidth - 1, outputY + y, foreground);
        }
    }
    if (cursorHere && update.cursor.style == TerminalCursor::Style::hollow_block) {
        for (int x = 0; x < cellWidth; ++x) {
            putPixel(outputX + x, outputY, cursor);
            putPixel(outputX + x, outputY + cellHeight - 1, cursor);
        }
        for (int y = 1; y + 1 < cellHeight; ++y) {
            putPixel(outputX, outputY + y, cursor);
            putPixel(outputX + cellWidth - 1, outputY + y, cursor);
        }
    } else if (cursorHere && update.cursor.style == TerminalCursor::Style::underline) {
        const int thickness = maximum(1, cellHeight / 8);
        for (int y = cellHeight - thickness; y < cellHeight; ++y) {
            for (int x = 0; x < cellWidth; ++x) {
                putPixel(outputX + x, outputY + y, cursor);
            }
        }
    } else if (cursorHere && update.cursor.style == TerminalCursor::Style::bar) {
        const int thickness = maximum(1, composer_.geometry.cellPixelWidth / 6);
        for (int y = 0; y < cellHeight; ++y) {
            for (int x = 0; x < thickness; ++x) {
                putPixel(outputX + x, outputY + y, cursor);
            }
        }
    }
}

bool ReferenceRendererImpl::render(const TerminalUpdate& update, const ReferenceCell* cells, u16 columns, u16 rows, const PixelRect& area) {
    if (!targetReady()) {
        return false;
    }
    pane_ = area;
    // Saturating, both of them: a pane whose origin is off the target
    // (a resize caught mid-flight) has an empty extent, and an extent
    // that went negative through an unsigned subtraction would be an
    // enormous one - a pane clip that clips nothing at all.
    const u32 right = min<u32>(target_->width, (u32)(area.x) + area.width);
    const u32 bottom = min<u32>(target_->height, (u32)(area.y) + area.height);
    clipLeft_ = area.x;
    clipTop_ = area.y;
    clipWidth_ = right > (u32)(area.x) ? right - area.x : 0;
    clipHeight_ = bottom > (u32)(area.y) ? bottom - area.y : 0;
    // The padding follows the live default background (OSC 11), matching
    // xterm, kitty, foot, and the rest.
    clearPane(update.colors != nullptr ? update.colors->defaultBackground : composer_.vtConfig.config->bg, backgroundAlpha());
    // The insets belong to the frame, not to the cell: they cannot change
    // between two cells of the same frame, and reading them per cell cost
    // one call and a four-field struct on every one of them.
    // A10: the pane's rectangle plus the border, and nothing else. The
    // chrome reserve came off the window before the rectangle was cut,
    // so charging it again here would push every pane but the first one
    // inwards by the width of a sidebar it is not next to.
    const Insets insets = composer_.paneInsets();
    for (u16 row = 0; row < rows; ++row) {
        for (u16 column = 0; column < columns; ++column) {
            const ReferenceCell& cell = cells[(size_t)(row)*columns + column];
            renderCell(update, cell, columns, column, row, insets);
        }
    }
    return true;
}

void ReferenceRendererImpl::captureModel() {
    if (testApi_ == nullptr) {
        return;
    }
    const size_t count = (size_t)(columns_)*rows_;
    resizeVector(modelCells_, count);
    resizeVector(modelLineAttributes_, count);
    resizeVector(cellGraphemes_, count);
    resizeVector(modelUnderlineColors_, count);
    graphemeStore_.clear();
    for (u16 row = 0; row < rows_; ++row) {
        for (u16 column = 0; column < columns_; ++column) {
            const size_t index = (size_t)(row)*columns_ + column;
            const VtermTestCell inspected = testApi_->cell(row, column);
            modelCells_.mut(index) = inspected.cell;
            modelLineAttributes_.mut(index) = inspected.lineAttribute;
            cellGraphemes_.mut(index) = {(u32)(graphemeStore_.length()), (u32)(inspected.graphemeSize)};
            graphemeStore_.append(inspected.grapheme, inspected.graphemeSize);
            modelUnderlineColors_.mut(index) = inspected.underlineColor;
        }
    }
}

void ReferenceRendererImpl::captureState(const TerminalUpdate& update, const ReferenceCell* cells, size_t count) {
    cursor_ = update.cursor;
    selection_ = update.selection;
    snappedSelection_ = update.snappedSelection;
    viewOffset_ = update.viewOffset;
    historyRows_ = update.historyRows;
    screenReverse_ = update.screenReverse;
    blinkVisible_ = update.blinkVisible;
    cursorBlink_ = update.cursorBlink;
    selectionForeground_ = update.selectionForeground;
    selectionBackground_ = update.selectionBackground;
    selectionColorMask_ = update.selectionColorMask;
    hoveredHyperlink_ = update.hoveredHyperlink;
    hoveredLinkBegin_ = update.hoveredLinkBegin;
    hoveredLinkEnd_ = update.hoveredLinkEnd;
    graphemeCells_ = 0;
    graphemeCodepoints_ = 0;
    for (size_t index = 0; index < count; ++index) {
        const ReferenceCell& cell = cells[index];
        if (cell.grapheme == 0) {
            continue;
        }
        const GraphemeView grapheme = composer_.extras.store->grapheme(cell.grapheme);
        if (!grapheme.empty()) {
            ++graphemeCells_;
            graphemeCodepoints_ += grapheme.size();
        }
    }
}

bool ReferenceRendererImpl::update(const PaneUpdate* panes, size_t count) {
    for (;;) {
        try {
            if (count == 0 || !reshape(panes, count)) {
                return false;
            }
            for (size_t index = 0; index < count; ++index) {
                if (!updateOnce(panes[index], index)) {
                    return false;
                }
            }
            paintSeams();
            // Every pane of the frame drew: the frame becomes the retain
            // in one step, so a frame refused halfway retains what the
            // last accepted one did rather than half of two frames.
            cells_.xchg(nextCells_);
            return true;
        } catch (const FontFaceMiss& miss) {
            // Lost-surface style: adopt a face for the missed cluster (or
            // record that nothing serves it) and re-run the frame. The
            // whole frame, panes already drawn included: drawing a pane
            // twice costs a frame nobody presented yet, and picking up
            // where the miss happened would leave the earlier panes
            // shaped through the fontpack that lost.
            composer_.fonts->adoptFaceFor(miss);
        }
    }
}

bool ReferenceRendererImpl::update(const TerminalUpdate& update) {
    const PaneUpdate pane = surfacePane(composer_, update);
    return this->update(&pane, 1);
}

// A6-3, A9: the frame's shape, settled once for the whole frame before
// a single pane draws. A pane keeps the cells it was retaining only when
// it is the same pane (the same Screen), of the same grid, in the same
// place in the store as in the frame before it. Anything else and
// nothing in the store is where it was, so the frame owes every row of
// every pane - the same rule the Metal backend states for its flat cell
// vector, and for the same reason.
//
// Before this, one `cells_` served every pane and each pane began from
// the cells of the pane drawn before it. A partially damaged frame -
// which is the ordinary case, TerminalUpdate::rows being the damaged
// rows - then showed pane 0's undamaged rows inside pane 1.
bool ReferenceRendererImpl::reshape(const PaneUpdate* panes, size_t count) {
    if (!targetReady()) {
        return false;
    }
    size_t total = 0;
    bool shapeChanged = panes_.length() != count;
    for (size_t index = 0; index < count; ++index) {
        const TerminalUpdate& update = panes[index].update;
        if (update.colors == nullptr) {
            return false;
        }
        // A9: zero is a refused frame, not a window-sized default. The
        // grid the update names is the width of TerminalRow::cells and
        // the height row.row indexes into; without it every read below
        // is a read of an unknown length.
        if (update.gridColumns == 0 || update.gridRows == 0) {
            return false;
        }
        if (!shapeChanged) {
            const PaneRetain& previous = panes_[index];
            shapeChanged = previous.shapes != update.shapes || previous.columns != update.gridColumns || previous.rows != update.gridRows || previous.offset != total;
        }
        total += (size_t)(update.gridColumns) * update.gridRows;
    }
    shapeChanged = shapeChanged || cells_.length() != total;
    if (shapeChanged) {
        // A reshaped grid needs every row of every pane before the
        // retained cells mean anything.
        for (size_t index = 0; index < count; ++index) {
            const TerminalUpdate& update = panes[index].update;
            if (update.rowCount != update.gridRows) {
                return false;
            }
            for (size_t row = 0; row < update.rowCount; ++row) {
                if (update.rows[row].cells == nullptr || update.rows[row].row != row) {
                    return false;
                }
            }
        }
    }
    panes_.clear();
    size_t offset = 0;
    for (size_t index = 0; index < count; ++index) {
        const TerminalUpdate& update = panes[index].update;
        panes_.pushBack({update.shapes, update.gridColumns, update.gridRows, offset});
        offset += (size_t)(update.gridColumns) * update.gridRows;
    }
    nextCells_.clear();
    if (shapeChanged) {
        nextCells_.zero(total);
        return true;
    }
    // The frame is built beside the retain rather than on top of it, so
    // a pane that refuses halfway leaves the last accepted frame whole.
    nextCells_.append(cells_.data(), cells_.length());
    return true;
}

// The captured model and the presentation state belong to the pane drawn
// last: the probes this renderer answers (columns(), screenText(), the
// model snapshots) are questions about a terminal, not about a window,
// and the terminal they mean is that one. The retained *cells*, since
// A6-3, belong to every pane at once - see reshape() above.
bool ReferenceRendererImpl::updateOnce(const PaneUpdate& pane, size_t index) {
    const TerminalUpdate& update = pane.update;
    const PaneRetain& retain = panes_[index];
    for (size_t entry = 0; entry < update.rowCount; ++entry) {
        const TerminalRow& row = update.rows[entry];
        if (row.cells == nullptr || row.row >= retain.rows) {
            return false;
        }
    }
    if (update.overlayCount != 0 && (update.overlayCells == nullptr || update.overlayRow >= retain.rows || (size_t)(update.overlayColumn) + update.overlayCount > retain.columns)) {
        return false;
    }

    ReferenceCell* const cells = nextCells_.mutData() + retain.offset;
    for (size_t entry = 0; entry < update.rowCount; ++entry) {
        const TerminalRow& row = update.rows[entry];
        for (u16 column = 0; column < retain.columns; ++column) {
            cells[(size_t)(row.row) * retain.columns + column] = materialize(row.cells[column], row.lineAttribute, *update.colors);
        }
    }
    if (update.overlayCount != 0) {
        // The preview covers the row content beneath it.
        const size_t base = (size_t)(update.overlayRow) * retain.columns + update.overlayColumn;
        for (u16 entry = 0; entry < update.overlayCount; ++entry) {
            cells[base + entry] = materialize(update.overlayCells[entry], 0, *update.colors);
        }
    }
    captureStrips(update);
    if (!render(update, cells, retain.columns, retain.rows, pane.area)) {
        return false;
    }

    lastOffset_ = retain.offset;
    columns_ = retain.columns;
    rows_ = retain.rows;
    colors_ = update.colors;
    lastUpdateCells_ = update.rowCount * columns_;
    lastUpdateSpans_ = update.rowCount;
    lastUpdateRows_.clear();
    for (size_t entry = 0; entry < update.rowCount; ++entry) {
        lastUpdateRows_.pushBack(update.rows[entry].row);
    }
    captureModel();
    captureState(update, cells, (size_t)(retain.columns) * retain.rows);
    havePresentation_ = true;
    ++refreshCount_;
    return true;
}

const ReferenceCell* ReferenceRendererImpl::retained() const {
    return cells_.data() + lastOffset_;
}

size_t ReferenceRendererImpl::retainedCount() const {
    return (size_t)(columns_)*rows_;
}

bool ReferenceRendererImpl::repaint() {
    if (!havePresentation_ || !targetReady()) {
        return false;
    }
    TerminalUpdate update = renderUpdate();
    return render(update, retained(), columns_, rows_, pane_);
}

void ReferenceRendererImpl::attach(TestApi& testApi) {
    testApi_ = &testApi;
}

ReferenceImage ReferenceRendererImpl::image() const {
    if (target_ == nullptr) {
        return {};
    }
    return {
        .pixels = target_->pixels,
        .length = target_->length,
        .width = (u16)(target_->width),
        .height = (u16)(target_->height),
    };
}

TerminalUpdate ReferenceRendererImpl::renderUpdate() const {
    renderRows_.clear();
    renderRows_.grow(rows_);
    const size_t count = retainedCount();
    resizeVector(renderCells_, count);
    for (size_t index = 0; index < count; ++index) {
        renderCells_.mut(index) = retained()[index].source;
        // Extra-cell references age out with store collections; the
        // grapheme codepoints captured at update time re-materialize so a
        // shaping consumer of these retained cells resolves the full
        // cluster.
        if (index < cellGraphemes_.length() && cellGraphemes_[index].count != 0) {
            const GraphemeSlice slice = cellGraphemes_[index];
            composer_.extras.store->setGrapheme(renderCells_.mut(index), graphemeStore_.data() + slice.offset, slice.count);
        }
    }
    for (u16 row = 0; row < rows_; ++row) {
        const size_t offset = (size_t)(row)*columns_;
        renderRows_.pushBack({
            renderCells_.data() + offset,
            row,
            retained()[offset].lineAttribute,
        });
    }
    return {
        .rows = renderRows_.data(),
        .rowCount = renderRows_.length(),
        // A9: this update carries the grid of the pane drawn last, which
        // is the shape of the cells it hands out. test_mode.cpp's shadow
        // mirror forwards it to another renderer verbatim, so the two
        // fields have to be here and not filled in by the consumer.
        .gridColumns = columns_,
        .gridRows = rows_,
        .colors = colors_,
        .viewOffset = viewOffset_,
        .historyRows = historyRows_,
        .cursor = cursor_,
        .selection = selection_,
        .snappedSelection = snappedSelection_,
        .selectionForeground = selectionForeground_,
        .selectionBackground = selectionBackground_,
        .selectionColorMask = selectionColorMask_,
        .hoveredHyperlink = hoveredHyperlink_,
        .hoveredLinkBegin = hoveredLinkBegin_,
        .hoveredLinkEnd = hoveredLinkEnd_,
        .screenReverse = screenReverse_,
        .blinkVisible = blinkVisible_,
        .cursorBlink = cursorBlink_,
    };
}

void ReferenceRendererImpl::snapshot(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK ") << columns_ << StringView(u8" ") << rows_ << StringView(u8" ") << cursor_.posX << StringView(u8" ") << cursor_.posY << StringView(u8" ") << (unsigned)(cursor_.style) << StringView(u8" ") << viewOffset_ << StringView(u8" ") << refreshCount_ << StringView(u8" ") << selection_.tl.x << StringView(u8" ") << selection_.tl.y << StringView(u8" ") << selection_.br.x << StringView(u8" ") << selection_.br.y << StringView(u8" ") << (unsigned)(selection_.rectangular) << StringView(u8" ");
    for (size_t index = 0; index < retainedCount(); ++index) {
        const ReferenceCell& cell = retained()[index];
        const unsigned flags = cellFlags(cell);
        const u32 codepoint = cell.source.uc_pt ? cell.source.uc_pt : ' ';
        output << Hex{codepoint, 8} << Hex{flags, 8} << Hex{cell.foreground.red, 2} << Hex{cell.foreground.green, 2} << Hex{cell.foreground.blue, 2} << Hex{cell.background.red, 2} << Hex{cell.background.green, 2} << Hex{cell.background.blue, 2} << Hex{cell.underlineColor.red, 2} << Hex{cell.underlineColor.green, 2} << Hex{cell.underlineColor.blue, 2} << Hex{cell.hyperlink, 8} << Hex{cell.source.semantic, 8};
    }
    output << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::modelSnapshot(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK ") << columns_ << StringView(u8" ") << rows_ << StringView(u8" ") << cursor_.posX << StringView(u8" ") << cursor_.posY << StringView(u8" ") << (unsigned)(cursor_.style) << StringView(u8" ") << viewOffset_ << StringView(u8" ") << refreshCount_ << StringView(u8" ") << selection_.tl.x << StringView(u8" ") << selection_.tl.y << StringView(u8" ") << selection_.br.x << StringView(u8" ") << selection_.br.y << StringView(u8" ") << (unsigned)(selection_.rectangular) << StringView(u8" ");
    for (size_t index = 0; index < retainedCount(); ++index) {
        const ReferenceCell& cell = retained()[index];
        const TerminalCell& modelCell = modelCells_[index];
        const unsigned flags = cellFlags(modelCell, modelLineAttributes_[index]);
        const u32 codepoint = cell.source.uc_pt ? cell.source.uc_pt : ' ';
        output << Hex{codepoint, 8} << Hex{flags, 8} << Hex{cell.foreground.red, 2} << Hex{cell.foreground.green, 2} << Hex{cell.foreground.blue, 2} << Hex{cell.background.red, 2} << Hex{cell.background.green, 2} << Hex{cell.background.blue, 2} << Hex{cell.underlineColor.red, 2} << Hex{cell.underlineColor.green, 2} << Hex{cell.underlineColor.blue, 2} << Hex{cell.hyperlink, 8} << Hex{cell.source.semantic, 8} << Hex{(u32)(modelCell.foreground().legacyIndex()), 8} << Hex{(u32)(modelCell.background().legacyIndex()), 8} << Hex{(u32)(modelUnderlineColors_[index].legacyIndex()), 8} << Hex{(size_t)(cellGraphemes_[index].count), 8};
        for (u32 unit = 0; unit < cellGraphemes_[index].count; ++unit) {
            output << Hex{graphemeStore_[cellGraphemes_[index].offset + unit], 8};
        }
    }
    output << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::modelDigest(Buffer& out) const {
    ModelDigest digest;
    digest.add(columns_);
    digest.add(rows_);
    digest.add(cursor_.style == TerminalCursor::Style::hidden ? (u64)-1 : cursor_.posX);
    digest.add(cursor_.style == TerminalCursor::Style::hidden ? (u64)-1 : cursor_.posY);
    digest.add((u8)(cursor_.style));
    digest.add(viewOffset_);
    digest.add(selection_.tl.x);
    digest.add(selection_.tl.y);
    digest.add(selection_.br.x);
    digest.add(selection_.br.y);
    digest.add(selection_.rectangular);
    digest.add(retainedCount());
    for (size_t index = 0; index < retainedCount(); ++index) {
        const ReferenceCell& cell = retained()[index];
        const TerminalCell& modelCell = modelCells_[index];
        digest.add(cell.source.uc_pt ? cell.source.uc_pt : ' ');
        digest.add(cellFlags(modelCell, modelLineAttributes_[index]));
        digest.add(cell.foreground.red);
        digest.add(cell.foreground.green);
        digest.add(cell.foreground.blue);
        digest.add(cell.background.red);
        digest.add(cell.background.green);
        digest.add(cell.background.blue);
        digest.add(cell.underlineColor.red);
        digest.add(cell.underlineColor.green);
        digest.add(cell.underlineColor.blue);
        digest.add(cell.hyperlink);
        digest.add(cell.source.semantic);
        digest.add((u32)(modelCell.foreground().legacyIndex()));
        digest.add((u32)(modelCell.background().legacyIndex()));
        digest.add((u32)(modelUnderlineColors_[index].legacyIndex()));
        digest.add(cellGraphemes_[index].count);
        for (u32 unit = 0; unit < cellGraphemes_[index].count; ++unit) {
            digest.add(graphemeStore_[cellGraphemes_[index].offset + unit]);
        }
    }
    StringBuilder output;
    output << StringView(u8"OK ") << Hex{digest.first, 16} << StringView(u8" ") << Hex{digest.second, 16} << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::renderState(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK ") << (unsigned)(screenReverse_) << StringView(u8" ") << (unsigned)(blinkVisible_) << StringView(u8" ") << (unsigned)(cursorBlink_) << StringView(u8" ") << (unsigned)(selectionColorMask_) << StringView(u8" ") << (unsigned)(selectionForeground_.red) << StringView(u8" ") << (unsigned)(selectionForeground_.green) << StringView(u8" ") << (unsigned)(selectionForeground_.blue) << StringView(u8" ") << (unsigned)(selectionBackground_.red) << StringView(u8" ") << (unsigned)(selectionBackground_.green) << StringView(u8" ") << (unsigned)(selectionBackground_.blue) << StringView(u8" ") << graphemeCells_ << StringView(u8" ") << graphemeCodepoints_ << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::selectionState(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK ") << selection_.tl.x << StringView(u8" ") << selection_.tl.y << StringView(u8" ") << selection_.br.x << StringView(u8" ") << selection_.br.y << StringView(u8" ") << (unsigned)(selection_.rectangular) << StringView(u8" ") << snappedSelection_.tl.x << StringView(u8" ") << snappedSelection_.tl.y << StringView(u8" ") << snappedSelection_.br.x << StringView(u8" ") << snappedSelection_.br.y << StringView(u8" ") << (unsigned)(snappedSelection_.rectangular) << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::scrollbackState(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK ") << historyRows_ << StringView(u8" ") << historyRows_ + rows_ << StringView(u8" ") << rows_ << StringView(u8" ") << historyRows_ - viewOffset_ << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::screenText(Buffer& out) const {
    out.reset();
    for (size_t index = 0; index < retainedCount(); ++index) {
        const u32 codepoint = retained()[index].source.uc_pt;
        const char printable = codepoint >= 0x20 && codepoint <= 0x7e ? (char)(codepoint) : ' ';
        out.append(&printable, 1);
        if ((index + 1) % columns_ == 0) {
            out.append("\n", 1);
        }
    }
}

void ReferenceRendererImpl::lastUpdate(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK ") << lastUpdateCells_ << StringView(u8" ") << lastUpdateSpans_ << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::lastUpdateRows(Buffer& out) const {
    StringBuilder output;
    output << StringView(u8"OK");
    for (u16 row : lastUpdateRows_) {
        output << StringView(u8" ") << row;
    }
    output << StringView(u8"\n");
    out.xchg(output);
}

void ReferenceRendererImpl::resetUpdateStats() {
    lastUpdateCells_ = 0;
    lastUpdateSpans_ = 0;
    lastUpdateRows_.clear();
}

u16 ReferenceRendererImpl::columns() const {
    return columns_;
}

u16 ReferenceRendererImpl::rows() const {
    return rows_;
}

u32 ReferenceRendererImpl::historyRows() const {
    return historyRows_;
}

u32 ReferenceRendererImpl::hoveredHyperlink() const {
    return hoveredHyperlink_;
}

u32 ReferenceRendererImpl::hoveredLinkBegin() const {
    return hoveredLinkBegin_;
}

u32 ReferenceRendererImpl::hoveredLinkEnd() const {
    return hoveredLinkEnd_;
}

ReferenceRenderer* ReferenceRenderer::create(Composer& composer, stl::ObjPool& pool, const plt::RenderContext& context) {
    return pool.make<ReferenceRendererImpl>(composer, context);
}
