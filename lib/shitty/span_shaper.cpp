/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "span_shaper.h"

#include "font.h"
#include "brand.h"
#include "options.h"
#include "composer.h"
#include "font_pack.h"
#include "render_synthesis.h"

#include <lib/vterm/listener.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/ios/sys.h>
#include <std/str/hash.h>
#include <std/sym/i_map.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

using namespace stl;

namespace {
    static constexpr size_t shapeClusterLimit = 32;
    // As far as a glyph's ink can ever reach past its last cell: U+FDFD,
    // the widest ligature in Unicode, spans about five cells.
    static constexpr u16 shapeCaptureLimit = 5;

    constexpr u32 rowSpanColor = 0x80000000u;
    constexpr u32 rowSpanMissing = 0x40000000u;
    constexpr u32 rowSpanOffsetMask = 0x3fffffffu;
    constexpr u32 rowShapeHeap = 0x80000000u;

    static bool shapeBlankCell(const TerminalCell& cell) {
        if (cell.hasExtra()) {
            return false;
        }
        return cell.uc_pt == 0 || cell.uc_pt == ' ';
    }

    // A strip is a mask: ink that crosses into the next cell is painted
    // with that cell's own colors, so two cells paint the same ink when
    // they agree on what decides its color - the two colors themselves
    // (faint blends toward the background, inverse swaps them) and the
    // attributes that recolor the result. Everything else is shaping and
    // decoration, and a blank cell shapes to nothing: a space in bold is
    // still a space, and keeping it out of the span would clip the ink
    // that was supposed to land in it.
    static bool shapeSameInk(const TerminalCell& left, const TerminalCell& right) {
        return left.foreground() == right.foreground() && left.background() == right.background() && left.inverse == right.inverse && left.faint == right.faint && left.conceal == right.conceal && left.blink == right.blink;
    }

    static FontStyle shapeCellStyle(const TerminalCell& cell) {
        return (FontStyle)((cell.bold ? 1 : 0) | (cell.italic ? 2 : 0));
    }

    // Generations never repeat across resets and shaper lifetimes: a
    // renderer keying its device copies on the generation must never see
    // two different arenas under one value.
    static u32 shapeGenerationCounter = 0;

    static u32 nextShapeGeneration() {
        return ++shapeGenerationCounter;
    }

    static u64 shapeMixHash(u64 hash, u64 value) {
        hash ^= value;
        hash *= 0x100000001b3ULL;
        return hash;
    }

    // Coverage the renderers draw themselves: box drawing, scan lines,
    // and block elements. These bypass the fonts outright - a font's
    // fractional ink leaves background seams between cells, synthesized
    // geometry lands on exact cell pixels.
    static bool shapeSynthesizableCell(const TerminalCell& cell) {
        return !cell.hasExtra() && synthesizedCodepoint(cell.uc_pt);
    }

    // One rendered span of a row in strip units; the two top offset bits
    // carry the plane and the missing-cluster mark.
    struct RowSpanEntry {
        u64 hash = 0;
        u32 offset = 0;
        u16 begin = 0;
        u16 end = 0;
    };

    static_assert(sizeof(RowSpanEntry) == 16, "row span entries must stay dense");

    // A shaped row under the model's row identity. The count's top bit
    // marks a heap allocation too large for the small-object allocator.
    // The id repeats the map key: a reset walks the map and erases by it.
    struct RowShapeRef {
        RowSpanEntry* entries;
        u64 id;
        u32 count;
    };

    struct RawSpanRef {
        u64 materialized;
        u32 epoch;
    };

    // The hash repeats the map key, for the same reset walk.
    struct StripRef {
        u64 hash;
        u32 offset;
    };

    struct SpanShaperImpl final: public SpanShaper {
        SpanShaperImpl(Composer& composer_, ObjPool& pool_);

        size_t rowSpans(const TerminalCell* cells, u16 columns, u64 rowId, ScreenRowSpan* out) override;
        size_t shapeCells(const TerminalCell* cells, u16 count, u16 baseColumn, ScreenRowSpan* out) override;
        u32 spanGeneration() const override;
        const u8* spanMask() const override;
        size_t spanMaskUsed() const override;
        const u32* spanColor() const override;
        size_t spanColorUsed() const override;

        u32 cutShapeRow(const TerminalCell* cells, u16 columns, RowSpanEntry* out);
        void convertShapeEntries(const RowSpanEntry* entries, u32 count, u16 baseColumn, ScreenRowSpan* out) const;
        size_t shapeCluster(const TerminalCell& cell, u32* codepoints) const;
        bool sixelCell(const TerminalCell& cell) const;
        void buildShapeText(const TerminalCell* cells, u16 begin, u16 end);
        u64 materializedSpanHash(FontStyle style, u16 cells) const;
        u32 renderShapeStrip(Font* font, bool color, const TerminalCell* cells, u16 begin, u16 end);
        u32 renderSixelStrip(const TerminalCell* cells, u16 begin, u16 end);
        u32 fillShapeEntries(const TerminalCell* cells, u16 columns, u32 spans, Buffer& scratch);
        size_t arenaBudget(size_t pixel) const;
        void releaseRowShape(RowShapeRef& row) noexcept;
        void reset();
        void onFontChanged();
        void onExtrasCollected();

        Composer& composer;
        ObjPool& pool;
        IntMap<RowShapeRef>* rows_ = nullptr;
        IntMap<RawSpanRef>* rawSpans_ = nullptr;
        IntMap<StripRef>* strips_ = nullptr;
        u32 rawEpoch_ = 0;
        u32 spanGeneration_ = nextShapeGeneration();
        Buffer shapeMask_;
        Buffer shapeColor_;
        Buffer overlayShape_;
        Buffer rowShape_;
        // Reusable flat codepoint string of the span being rendered.
        Buffer shapeText_;
        // The widest row currently being cut: the arena budget must fit
        // one whole row, or the refill loops would reset forever.
        u16 shapeColumns_ = 0;
    };

    struct CallShaperFontChanged final: public Listener {
        explicit CallShaperFontChanged(SpanShaperImpl* shaper_);

        void onListen(void*) override;

        SpanShaperImpl* shaper;
    };

    // The shaper owns no refs of its own - the cells belong to the
    // terminals behind the window - so it takes only the second half of
    // the client interface: the shaping cache keyed by ref is what a
    // rebuilt store voids.
    struct CallShaperExtrasCollected final: public CellExtraClient {
        explicit CallShaperExtrasCollected(SpanShaperImpl* shaper_);

        void extrasCollected() override;

        SpanShaperImpl* shaper;
    };
}

CallShaperFontChanged::CallShaperFontChanged(SpanShaperImpl* shaper_)
    : shaper(shaper_)
{
}

void CallShaperFontChanged::onListen(void*) {
    shaper->onFontChanged();
}

CallShaperExtrasCollected::CallShaperExtrasCollected(SpanShaperImpl* shaper_)
    : shaper(shaper_)
{
}

void CallShaperExtrasCollected::extrasCollected() {
    shaper->onExtrasCollected();
}

SpanShaperImpl::SpanShaperImpl(Composer& composer_, ObjPool& pool_)
    : composer(composer_)
    , pool(pool_)
{
    rows_ = pool.make<IntMap<RowShapeRef>>(&pool);
    rawSpans_ = pool.make<IntMap<RawSpanRef>>(&pool);
    strips_ = pool.make<IntMap<StripRef>>(&pool);
}

u32 SpanShaperImpl::spanGeneration() const {
    return spanGeneration_;
}

const u8* SpanShaperImpl::spanMask() const {
    return (const u8*)(shapeMask_.data());
}

size_t SpanShaperImpl::spanMaskUsed() const {
    return shapeMask_.used();
}

const u32* SpanShaperImpl::spanColor() const {
    return (const u32*)(shapeColor_.data());
}

size_t SpanShaperImpl::spanColorUsed() const {
    return shapeColor_.used() / sizeof(u32);
}

void SpanShaperImpl::releaseRowShape(RowShapeRef& row) noexcept {
    const u32 count = row.count & ~rowShapeHeap;
    if (row.count & rowShapeHeap) {
        delete[] row.entries;
    } else {
        composer.smallObjects->deallocate(row.entries, (size_t)(count) * sizeof(RowSpanEntry));
    }
    row.entries = nullptr;
    row.count = 0;
}

void SpanShaperImpl::reset() {
    // The whole cache dies at once: rows, strips and arenas. The moved
    // generation makes every renderer pull its visible rows again, and
    // they reshape into the fresh arenas - a rare, bounded cost, the
    // same one a font change pays. The raw-bytes memo survives: it maps
    // cell content to shaped identity and references no arena.
    Vector<u64> rowIds;
    rows_->visit([&](RowShapeRef& row) {
        releaseRowShape(row);
        rowIds.pushBack(row.id);
    });
    for (size_t index = 0; index < rowIds.length(); ++index) {
        rows_->erase(rowIds[index]);
    }
    Vector<u64> stripHashes;
    strips_->visit([&](StripRef& strip) {
        stripHashes.pushBack(strip.hash);
    });
    for (size_t index = 0; index < stripHashes.length(); ++index) {
        strips_->erase(stripHashes[index]);
    }
    shapeMask_.reset();
    shapeColor_.reset();
    spanGeneration_ = nextShapeGeneration();
}

void SpanShaperImpl::onFontChanged() {
    // Different metrics, different pixels: everything restarts, and the
    // raw memo goes too - which face covers a cluster is part of the
    // shaped identity.
    const u32 previous = spanGeneration_;
    ++rawEpoch_;
    reset();
    if (composer.opts->vt.verbose) {
        sysE << composer.brand->identifier() << StringView(u8": shape: font change, generation ") << previous << StringView(u8" -> ") << spanGeneration_ << endL;
    }
}

void SpanShaperImpl::onExtrasCollected() {
    // Extra refs died with their store: the raw-bytes level is void. The
    // strips and the shaped rows keep their offsets; recovery is a
    // re-materialization and a second-level hit.
    ++rawEpoch_;
}

size_t SpanShaperImpl::shapeCluster(const TerminalCell& cell, u32* codepoints) const {
    // A stored grapheme holds the whole cluster, lead codepoint included.
    if (cell.hasExtra() && composer.extras.store != nullptr) {
        const GraphemeView grapheme = composer.extras.store->view(cell).grapheme;
        if (!grapheme.empty()) {
            size_t count = 0;
            for (size_t index = 0; index < grapheme.size() && count < shapeClusterLimit; ++index) {
                codepoints[count++] = grapheme.data()[index];
            }
            return count;
        }
    }
    codepoints[0] = cell.uc_pt ? cell.uc_pt : ' ';
    return 1;
}

bool SpanShaperImpl::sixelCell(const TerminalCell& cell) const {
    return cell.hasExtra() && composer.extras.store != nullptr && composer.extras.store->view(cell).sixelPixels != nullptr;
}

void SpanShaperImpl::buildShapeText(const TerminalCell* cells, u16 begin, u16 end) {
    // The flat codepoint string of the span: cluster codepoints of every
    // lead cell, a captured blank cell as a space.
    shapeText_.reset();
    u32 cluster[shapeClusterLimit];
    for (u16 column = begin; column < end; ++column) {
        const TerminalCell& cell = cells[column];
        if (cell.dwidth_cont) {
            continue;
        }
        if (shapeBlankCell(cell)) {
            const u32 space = ' ';
            shapeText_.append(&space, sizeof(space));
            continue;
        }
        const size_t count = shapeCluster(cell, cluster);
        shapeText_.append(cluster, count * sizeof(u32));
    }
}

u64 SpanShaperImpl::materializedSpanHash(FontStyle style, u16 cells) const {
    u64 hash = shash64(shapeText_.data(), shapeText_.used());
    hash = shapeMixHash(hash, (u64)(style));
    hash = shapeMixHash(hash, cells);
    return hash;
}

// The live set is bounded by the viewport, so thrice its pixel size
// always leaves room after a reset - and the floor of one whole row,
// wider than the viewport during a shrinking resize, is what keeps the
// callers' refill loops terminating.
size_t SpanShaperImpl::arenaBudget(size_t pixel) const {
    const size_t viewport = (size_t)(composer.geometry.columns) * composer.geometry.rows;
    const size_t floor = (size_t)(shapeColumns_);
    return 3u * (viewport > floor ? viewport : floor) * composer.fonts->getPx() * composer.fonts->getPy() * pixel;
}

u32 SpanShaperImpl::renderShapeStrip(Font* font, bool color, const TerminalCell* cells, u16 begin, u16 end) {
    Buffer& arena = color ? shapeColor_ : shapeMask_;
    const u16 spanCells = (u16)(end - begin);
    const u16 cellWidth = composer.fonts->getPx();
    const u16 cellHeight = composer.fonts->getPy();
    const size_t pixel = color ? sizeof(u32) : 1;
    const size_t bytes = (size_t)(spanCells)*cellWidth * cellHeight * pixel;
    const size_t budget = arenaBudget(pixel);
    if (arena.used() + bytes > budget && arena.used() != 0) {
        reset();
    }
    const size_t offset = arena.used();
    arena.grow(offset + bytes);
    arena.seekAbsolute(offset + bytes);
    u8* const out = (u8*)(arena.mutData()) + offset;
    __builtin_memset(out, 0, bytes);
    if (font != nullptr) {
        font->render((const u32*)(shapeText_.data()), shapeText_.used() / sizeof(u32), spanCells, out);
        return (u32)(offset / pixel);
    }
    // No face covers the span: a hollow box per cluster, the shape every
    // renderer historically drew for lost glyphs.
    const size_t stride = (size_t)(spanCells)*cellWidth;
    for (u16 column = begin; column < end;) {
        const TerminalCell& cell = cells[column];
        if (cell.dwidth_cont || shapeBlankCell(cell)) {
            ++column;
            continue;
        }
        const u16 width = cell.dwidth && column + 1 < end && cells[column + 1].dwidth_cont ? 2 : 1;
        const size_t x0 = (size_t)(column - begin) * cellWidth;
        const int boxWidth = width * cellWidth;
        for (int y = 1; y + 1 < cellHeight; ++y) {
            for (int x = 1; x + 1 < boxWidth; ++x) {
                if (x == 1 || x + 2 == boxWidth || y == 1 || y + 2 == cellHeight) {
                    out[(size_t)(y)*stride + x0 + x] = 179;
                }
            }
        }
        column = (u16)(column + width);
    }
    return (u32)(offset);
}

u32 SpanShaperImpl::renderSixelStrip(const TerminalCell* cells, u16 begin, u16 end) {
    Buffer& arena = shapeColor_;
    const u16 spanCells = (u16)(end - begin);
    const u16 cellWidth = composer.fonts->getPx();
    const u16 cellHeight = composer.fonts->getPy();
    const size_t bytes = (size_t)(spanCells)*cellWidth * cellHeight * sizeof(u32);
    const size_t budget = arenaBudget(sizeof(u32));
    if (arena.used() + bytes > budget && arena.used() != 0) {
        reset();
    }
    const size_t offset = arena.used();
    arena.grow(offset + bytes);
    arena.seekAbsolute(offset + bytes);
    u8* const out = (u8*)(arena.mutData()) + offset;
    __builtin_memset(out, 0, bytes);

    // Premultiplied RGBA like a color font strip: painted pixels are
    // opaque palette entries scaled to the cell by nearest neighbor,
    // transparent pixels stay zero and show the cell background.
    const size_t stride = (size_t)(spanCells)*cellWidth;
    for (u16 column = begin; column < end; ++column) {
        const CellExtraView view = composer.extras.store->view(cells[column]);
        if (view.sixelPixels == nullptr) {
            continue;
        }
        const size_t x0 = (size_t)(column - begin) * cellWidth;
        for (int y = 0; y < cellHeight; ++y) {
            const u32 sourceY = (u32)(y)*SixelPatch::height / cellHeight;
            u8* const rowOut = out + ((size_t)(y)*stride + x0) * 4;
            for (int x = 0; x < cellWidth; ++x) {
                const u32 sourceX = (u32)(x)*SixelPatch::width / cellWidth;
                const u8 value = view.sixelPixels[sourceY * SixelPatch::width + sourceX];
                if (value != 0) {
                    const u8* const rgb = view.sixelPalette + (size_t)(value - 1) * 3;
                    rowOut[4 * x + 0] = rgb[0];
                    rowOut[4 * x + 1] = rgb[1];
                    rowOut[4 * x + 2] = rgb[2];
                    rowOut[4 * x + 3] = 255;
                }
            }
        }
    }
    return (u32)(offset / sizeof(u32));
}

u32 SpanShaperImpl::cutShapeRow(const TerminalCell* cells, u16 columns, RowSpanEntry* out) {
    shapeColumns_ = columns;
    u32 spans = 0;
    u32 cluster[shapeClusterLimit];
    for (u16 column = 0; column < columns;) {
        if (shapeBlankCell(cells[column])) {
            ++column;
            continue;
        }

        const u16 begin = column;
        if (sixelCell(cells[column])) {
            while (column < columns && sixelCell(cells[column])) {
                ++column;
            }
            const u16 end = column;
            if (out != nullptr) {
                u64 materialized = shapeMixHash(0, 0x534958454cULL);
                for (u16 cell = begin; cell < end; ++cell) {
                    const CellExtraView view = composer.extras.store->view(cells[cell]);
                    materialized = shapeMixHash(materialized, shash64(view.sixelPixels, SixelPatch::pixelCount));
                    materialized = shapeMixHash(materialized, shash64(view.sixelPalette, SixelPatch::paletteBytes));
                }
                RowSpanEntry& entry = out[spans];
                entry.begin = begin;
                entry.end = end;
                entry.hash = materialized;
                const StripRef* const strip = strips_->find(materialized);
                if (strip != nullptr) {
                    entry.offset = strip->offset;
                } else {
                    entry.offset = renderSixelStrip(cells, begin, end) | rowSpanColor;
                    // The render may have reset: the map reference is
                    // stale, so look up again.
                    StripRef* const fresh = strips_->find(materialized);
                    if (fresh != nullptr) {
                        fresh->offset = entry.offset;
                    } else {
                        strips_->insert(materialized, materialized, entry.offset);
                    }
                }
            }
            ++spans;
            continue;
        }
        const FontStyle style = shapeCellStyle(cells[column]);
        Font* font = nullptr;
        bool started = false;
        bool synthesized = false;
        while (column < columns && !shapeBlankCell(cells[column])) {
            if (shapeCellStyle(cells[column]) != style) {
                break;
            }
            if (sixelCell(cells[column])) {
                break;
            }
            const bool cellSynthesized = shapeSynthesizableCell(cells[column]);
            Font* cellFont = nullptr;
            if (!cellSynthesized) {
                const size_t count = shapeCluster(cells[column], cluster);
                cellFont = composer.fonts->resolveFace(cluster, count);
            }
            if (!started) {
                font = cellFont;
                synthesized = cellSynthesized;
                started = true;
            } else if (cellFont != font || cellSynthesized != synthesized) {
                break;
            }
            const u16 width = cells[column].dwidth && column + 1 < columns && cells[column + 1].dwidth_cont ? 2 : 1;
            column = (u16)(column + width);
        }
        u16 end = column;

        // Ink is not bounded by the cell advance: an italic shear, a
        // hook, U+FDFD at its natural size all reach past the last cell
        // of their span, and a strip is only as wide as the span it
        // belongs to - the overflow was simply clipped away. The span
        // takes blank cells behind it to catch that ink, up to
        // shapeCaptureLimit - the bismillah ligature is the widest ink
        // in Unicode at roughly five cells (issue 91); only cells that
        // would paint the ink the way the span itself does; and never
        // for synthesized coverage, which is generated inside its own
        // cell. In running text the gap between words is one blank, so
        // the capture degenerates to the single cell it always took;
        // past the last word every span takes the same full capture -
        // either way the span cache keeps interning whole words.
        if (!synthesized && column < columns && shapeBlankCell(cells[column])) {
            const TerminalCell& last = cells[end - 1];
            u16 captured = 0;
            while (captured < shapeCaptureLimit && column < columns && shapeBlankCell(cells[column]) && shapeSameInk(last, cells[column])) {
                ++captured;
                end = (u16)(column + 1);
                column = end;
            }
        }

        if (out != nullptr) {
            RowSpanEntry& entry = out[spans];
            entry.begin = begin;
            entry.end = end;
            // Synthesized runs stay missing - the renderer draws their
            // coverage from the codepoint; any uncovered cluster gets the
            // hollow box strip instead.
            if (synthesized) {
                entry.hash = 0;
                entry.offset = rowSpanMissing;
            } else {
                const bool color = font != nullptr && font->colored();
                const u16 spanCells = (u16)(end - begin);
                const u64 raw = shash64(cells + begin, (size_t)(spanCells) * sizeof(TerminalCell));
                u64 materialized = 0;
                bool haveText = false;
                RawSpanRef* const rawRef = rawSpans_->find(raw);
                if (rawRef != nullptr && rawRef->epoch == rawEpoch_) {
                    materialized = rawRef->materialized;
                } else {
                    buildShapeText(cells, begin, end);
                    haveText = true;
                    materialized = materializedSpanHash(style, spanCells);
                    if (font == nullptr) {
                        materialized = shapeMixHash(materialized, 0x426f78);
                    }
                    if (rawRef != nullptr) {
                        rawRef->materialized = materialized;
                        rawRef->epoch = rawEpoch_;
                    } else {
                        rawSpans_->insert(raw, materialized, rawEpoch_);
                    }
                }
                entry.hash = materialized;
                const StripRef* const strip = strips_->find(materialized);
                if (strip != nullptr) {
                    entry.offset = strip->offset;
                } else {
                    if (!haveText) {
                        buildShapeText(cells, begin, end);
                    }
                    Font* const styled = font != nullptr ? composer.fonts->styledFace(font, style) : nullptr;
                    entry.offset = renderShapeStrip(styled != nullptr ? styled : font, color, cells, begin, end) | (color ? rowSpanColor : 0);
                    // The render may have reset the strip map wholesale;
                    // look up again before publishing.
                    StripRef* const fresh = strips_->find(materialized);
                    if (fresh != nullptr) {
                        fresh->offset = entry.offset;
                    } else {
                        strips_->insert(materialized, materialized, entry.offset);
                    }
                }
            }
        }
        ++spans;
    }
    return spans;
}

void SpanShaperImpl::convertShapeEntries(const RowSpanEntry* entries, u32 count, u16 baseColumn, ScreenRowSpan* out) const {
    for (u32 index = 0; index < count; ++index) {
        const RowSpanEntry& entry = entries[index];
        out[index] = {
            .begin = (u16)(entry.begin + baseColumn),
            .end = (u16)(entry.end + baseColumn),
            .offset = entry.offset & rowSpanOffsetMask,
            .color = (entry.offset & rowSpanColor) != 0,
            .missing = (entry.offset & rowSpanMissing) != 0,
        };
    }
}

u32 SpanShaperImpl::fillShapeEntries(const TerminalCell* cells, u16 columns, u32 spans, Buffer& scratch) {
    // The fill can trigger a reset from a later span of the same row,
    // stranding the earlier offsets in a dead arena; refill until the
    // pass completes within one generation. The refill re-renders the
    // swept strips into fresh arenas, so it terminates.
    scratch.reset();
    scratch.grow((size_t)(spans) * sizeof(RowSpanEntry));
    scratch.seekAbsolute((size_t)(spans) * sizeof(RowSpanEntry));
    auto* const entries = (RowSpanEntry*)(scratch.mutData());
    u32 generation;
    do {
        generation = spanGeneration_;
        __builtin_memset(entries, 0, (size_t)(spans) * sizeof(RowSpanEntry));
        cutShapeRow(cells, columns, entries);
    } while (generation != spanGeneration_);
    return spans;
}

size_t SpanShaperImpl::rowSpans(const TerminalCell* cells, u16 columns, u64 rowId, ScreenRowSpan* out) {
    if (composer.fonts == nullptr || cells == nullptr || columns == 0) {
        return 0;
    }
    if (const RowShapeRef* const cached = rows_->find(rowId)) {
        convertShapeEntries(cached->entries, cached->count & ~rowShapeHeap, 0, out);
        return cached->count & ~rowShapeHeap;
    }
    const u32 spans = cutShapeRow(cells, columns, nullptr);
    if (spans == 0) {
        return 0;
    }
    fillShapeEntries(cells, columns, spans, rowShape_);
    const size_t bytes = (size_t)(spans) * sizeof(RowSpanEntry);
    RowShapeRef row;
    row.id = rowId;
    if (bytes <= smallObjMaxSize) {
        row.entries = (RowSpanEntry*)(composer.smallObjects->allocate(bytes));
        row.count = spans;
    } else {
        // A row of more spans than the small-object bound is pathological
        // but must not corrupt it.
        row.entries = new RowSpanEntry[spans];
        row.count = spans | rowShapeHeap;
    }
    __builtin_memcpy(row.entries, rowShape_.data(), bytes);
    rows_->insert(rowId, row);
    convertShapeEntries(row.entries, spans, 0, out);
    return spans;
}

size_t SpanShaperImpl::shapeCells(const TerminalCell* cells, u16 count, u16 baseColumn, ScreenRowSpan* out) {
    if (composer.fonts == nullptr || count == 0) {
        return 0;
    }
    const u32 spans = cutShapeRow(cells, count, nullptr);
    if (spans == 0) {
        return 0;
    }
    // Nothing roots these strips under a row identity, so they refill
    // through the same loop and convert directly.
    fillShapeEntries(cells, count, spans, overlayShape_);
    convertShapeEntries((const RowSpanEntry*)(overlayShape_.data()), spans, baseColumn, out);
    return spans;
}

SpanShaper* SpanShaper::create(Composer& composer, ObjPool& pool) {
    SpanShaperImpl* const shaper = pool.make<SpanShaperImpl>(composer, pool);
    composer.fontChangedListeners.pushBack(pool.make<CallShaperFontChanged>(shaper));
    composer.extras.changedListeners.pushBack(pool.make<CallShaperExtrasCollected>(shaper));
    return shaper;
}
