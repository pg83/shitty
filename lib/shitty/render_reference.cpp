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

        bool update(const TerminalUpdate& update) override;
        bool updateOnce(const TerminalUpdate& update);
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
        void clearTarget(Color background);
        void putPixel(int x, int y, Color color);
        ReferenceCell materialize(const TerminalCell& cell, u8 lineAttribute, const TerminalColors& colors) const;
        void captureStrips(const TerminalUpdate& update);
        void captureSpan(SpanShaper& shaper, u16 row, const ScreenRowSpan& span);
        void renderCell(const TerminalUpdate& update, const ReferenceCell& cell, u16 column, u16 row);
        bool render(const TerminalUpdate& update, const Vector<ReferenceCell>& cells);
        void captureModel();
        void captureState(const TerminalUpdate& update);

        Composer& composer_;
        plt::HeadlessRenderTarget* target_;
        Buffer coverage_;
        Buffer color_;
        Buffer stripStore_;
        Vector<CellStrip> cellStrips_;
        Vector<ScreenRowSpan> spanScratch_;
        Vector<ReferenceCell> cells_;
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
    if (target_->width != composer_.layout.pixelWidth || target_->height != composer_.layout.pixelHeight || target_->stride < target_->width * 3) {
        return false;
    }
    return target_->length >= (size_t)(target_->stride) * target_->height;
}

void ReferenceRendererImpl::clearTarget(Color background) {
    for (u32 y = 0; y < target_->height; ++y) {
        u8* row = target_->pixels + (size_t)(y)*target_->stride;
        for (u32 x = 0; x < target_->width; ++x) {
            row[3 * x] = background.red;
            row[3 * x + 1] = background.green;
            row[3 * x + 2] = background.blue;
        }
    }
}

void ReferenceRendererImpl::putPixel(int x, int y, Color color) {
    if (x < 0 || y < 0 || x >= (int)(target_->width) || y >= (int)(target_->height)) {
        return;
    }
    u8* const pixel = target_->pixels + (size_t)(y)*target_->stride + 3 * x;
    pixel[0] = color.red;
    pixel[1] = color.green;
    pixel[2] = color.blue;
}

void ReferenceRendererImpl::captureSpan(SpanShaper& shaper, u16 row, const ScreenRowSpan& span) {
    if (span.end <= span.begin || span.end > composer_.geometry.columns) {
        return;
    }
    if (span.missing) {
        // A synthesized run: renderCell draws its coverage from the
        // codepoint, matching the GPU shader.
        for (u16 column = span.begin; column < span.end; ++column) {
            cellStrips_.mut((size_t)(row)*composer_.geometry.columns + column) = {0, 0, stripSynthesized};
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
        cellStrips_.mut((size_t)(row)*composer_.geometry.columns + column) = {
            (u32)(base + (size_t)(column - span.begin) * width * pixel),
            (u32)(span.end - span.begin) * width,
            span.color ? stripColor : stripMask,
        };
    }
}

void ReferenceRendererImpl::captureStrips(const TerminalUpdate& update) {
    const size_t count = (size_t)(composer_.geometry.columns) * composer_.geometry.rows;
    cellStrips_.clear();
    cellStrips_.zero(count);
    stripStore_.reset();
    if (update.shapes == nullptr || composer_.shaper == nullptr) {
        return;
    }
    Screen& shapes = *update.shapes;
    SpanShaper& shaper = *composer_.shaper;
    resizeVector(spanScratch_, composer_.geometry.columns);
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
                const size_t spans = shaper.shapeCells(row.cells, composer_.geometry.columns, 0, spanScratch_.mutData());
                for (size_t entry = 0; entry < spans; ++entry) {
                    captureSpan(shaper, row.row, spanScratch_[entry]);
                }
            }
            continue;
        }
        for (u16 row = 0; row < composer_.geometry.rows; ++row) {
            const ScreenRowRef rowRef = shapes.viewRow(row);
            const size_t spans = shaper.rowSpans(rowRef.cells, composer_.geometry.columns, rowRef.id, spanScratch_.mutData());
            for (size_t index = 0; index < spans; ++index) {
                captureSpan(shaper, row, spanScratch_[index]);
            }
        }
        if (update.overlayCount != 0) {
            // The preedit preview covers the underlying strips wholesale:
            // its blank cells hide the text below them.
            const size_t base = (size_t)(update.overlayRow) * composer_.geometry.columns + update.overlayColumn;
            for (u16 index = 0; index < update.overlayCount; ++index) {
                cellStrips_.mut(base + index) = {};
            }
            const size_t spans = shaper.shapeCells(update.overlayCells, update.overlayCount, update.overlayColumn, spanScratch_.mutData());
            for (size_t index = 0; index < spans; ++index) {
                captureSpan(shaper, update.overlayRow, spanScratch_[index]);
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

void ReferenceRendererImpl::renderCell(const TerminalUpdate& update, const ReferenceCell& cell, u16 column, u16 row) {
    const TerminalCell& source = cell.source;
    const bool doubleLine = cell.lineAttribute != 0;
    const int cellWidth = composer_.geometry.cellPixelWidth;
    const int cellHeight = composer_.geometry.cellPixelHeight;
    coverage_.zero((size_t)(cellWidth)*cellHeight);
    color_.zero((size_t)(cellWidth)*cellHeight * 4);
    hasColor_ = false;
    const CellStrip strip = cellStrips_[(size_t)(row)*composer_.geometry.columns + column];
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

    const int outputX = composer_.layout.originX + column * composer_.geometry.cellPixelWidth;
    const int outputY = composer_.layout.originY + row * composer_.geometry.cellPixelHeight;
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
                        (u8)((unsigned)(color[4 * index]) * strength / 255 + (unsigned)(background.red) * (255 - alpha) / 255),
                        (u8)((unsigned)(color[4 * index + 1]) * strength / 255 + (unsigned)(background.green) * (255 - alpha) / 255),
                        (u8)((unsigned)(color[4 * index + 2]) * strength / 255 + (unsigned)(background.blue) * (255 - alpha) / 255),
                    }
                );
            } else {
                putPixel(outputX + x, outputY + y, blend(foreground, background, coverage[index]));
            }
        }
    }

    const u32 cellIndex = (u32)(row)*composer_.geometry.columns + column;
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

bool ReferenceRendererImpl::render(const TerminalUpdate& update, const Vector<ReferenceCell>& cells) {
    if (!targetReady()) {
        return false;
    }
    // The padding follows the live default background (OSC 11), matching
    // xterm, kitty, foot, and the rest.
    clearTarget(update.colors != nullptr ? update.colors->defaultBackground : composer_.opts->vt.bg);
    for (u16 row = 0; row < composer_.geometry.rows; ++row) {
        for (u16 column = 0; column < composer_.geometry.columns; ++column) {
            const ReferenceCell& cell = cells[(size_t)(row)*composer_.geometry.columns + column];
            renderCell(update, cell, column, row);
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

void ReferenceRendererImpl::captureState(const TerminalUpdate& update) {
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
    for (const ReferenceCell& cell : cells_) {
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

bool ReferenceRendererImpl::update(const TerminalUpdate& update) {
    for (;;) {
        try {
            return updateOnce(update);
        } catch (const FontFaceMiss& miss) {
            // Lost-surface style: adopt a face for the missed cluster (or
            // record that nothing serves it) and re-run the frame.
            composer_.fonts->adoptFaceFor(miss);
        }
    }
}

bool ReferenceRendererImpl::updateOnce(const TerminalUpdate& update) {
    if (!targetReady() || update.colors == nullptr) {
        return false;
    }
    const size_t count = (size_t)(composer_.geometry.columns) * composer_.geometry.rows;
    const bool shapeChanged = columns_ != composer_.geometry.columns || rows_ != composer_.geometry.rows;
    if (shapeChanged) {
        // A reshaped grid needs every row before the retained cells mean
        // anything.
        if (update.rowCount != composer_.geometry.rows) {
            return false;
        }
        for (size_t index = 0; index < update.rowCount; ++index) {
            if (update.rows[index].cells == nullptr || update.rows[index].row != index) {
                return false;
            }
        }
    }
    for (size_t index = 0; index < update.rowCount; ++index) {
        const TerminalRow& row = update.rows[index];
        if (row.cells == nullptr || row.row >= composer_.geometry.rows) {
            return false;
        }
    }
    if (update.overlayCount != 0 && (update.overlayCells == nullptr || update.overlayRow >= composer_.geometry.rows || (size_t)(update.overlayColumn) + update.overlayCount > composer_.geometry.columns)) {
        return false;
    }

    Vector<ReferenceCell> next(cells_);
    if (shapeChanged) {
        next.clear();
        next.zero(count);
    }
    for (size_t index = 0; index < update.rowCount; ++index) {
        const TerminalRow& row = update.rows[index];
        for (u16 column = 0; column < composer_.geometry.columns; ++column) {
            next.mut((size_t)(row.row) * composer_.geometry.columns + column) = materialize(row.cells[column], row.lineAttribute, *update.colors);
        }
    }
    if (update.overlayCount != 0) {
        // The preview covers the row content beneath it.
        const size_t base = (size_t)(update.overlayRow) * composer_.geometry.columns + update.overlayColumn;
        for (u16 index = 0; index < update.overlayCount; ++index) {
            next.mut(base + index) = materialize(update.overlayCells[index], 0, *update.colors);
        }
    }
    captureStrips(update);
    if (!render(update, next)) {
        return false;
    }

    columns_ = composer_.geometry.columns;
    rows_ = composer_.geometry.rows;
    cells_.xchg(next);
    colors_ = update.colors;
    lastUpdateCells_ = update.rowCount * columns_;
    lastUpdateSpans_ = update.rowCount;
    lastUpdateRows_.clear();
    for (size_t index = 0; index < update.rowCount; ++index) {
        lastUpdateRows_.pushBack(update.rows[index].row);
    }
    captureModel();
    captureState(update);
    havePresentation_ = true;
    ++refreshCount_;
    return true;
}

bool ReferenceRendererImpl::repaint() {
    if (!havePresentation_ || !targetReady()) {
        return false;
    }
    TerminalUpdate update = renderUpdate();
    return render(update, cells_);
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
    resizeVector(renderCells_, cells_.length());
    for (size_t index = 0; index < cells_.length(); ++index) {
        renderCells_.mut(index) = cells_[index].source;
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
            cells_[offset].lineAttribute,
        });
    }
    return {
        .rows = renderRows_.data(),
        .rowCount = renderRows_.length(),
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
    for (const ReferenceCell& cell : cells_) {
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
    for (size_t index = 0; index < cells_.length(); ++index) {
        const ReferenceCell& cell = cells_[index];
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
    digest.add(cells_.length());
    for (size_t index = 0; index < cells_.length(); ++index) {
        const ReferenceCell& cell = cells_[index];
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
    for (size_t index = 0; index < cells_.length(); ++index) {
        const u32 codepoint = cells_[index].source.uc_pt;
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
