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

#include "screen.h"

#include "cell_extra_store.h"

#include <lib/vterm/utf8.h>
#include <lib/vterm/unicode.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/ios/sys.h>
#include <std/str/hash.h>
#include <std/sym/i_map.h>
#include <std/alg/minmax.h>
#include <std/dbg/assert.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <stdio.h>
#include <string.h>

using namespace stl;

struct RowMetadata {
    u8 lineAttribute = 0;
    u8 protection = 0;
    ScreenSemanticPrompt semanticPrompt = ScreenSemanticPrompt::None;
    bool wide = false;
};

// Every value handed out as a row identity comes from this sequence and
// never repeats: render-side caches key on it, and a mutated or recycled
// row simply presents a fresh value - a stale key cannot collide back to
// life.
static u64 rowIdentityCounter = 0;

static u64 nextRowIdentity() {
    return ++rowIdentityCounter;
}

struct alignas(16) Row {
    RowMetadata metadata;
    // Keeps the identity union 8-aligned; the header stays one cell.
    u32 reserved = 0;

    union {
        // A live row: its identity for render-side caches, fresh from
        // the global sequence on every mutation. Row rotation moves it
        // with the row, so an unchanged row keeps its identity through
        // scrolls.
        u64 id;
        // A free-listed row; reuse assigns a fresh id.
        Row* freeNext;
    };

    alignas(16) TerminalCell cells[0];
};

static_assert(offsetof(Row, cells) == 16, "Row cells must stay 16-byte aligned");
static_assert(sizeof(Row) == 16, "Row header must occupy one cell");
static_assert(alignof(Row) == 16, "Row must preserve cell alignment");

using RowSlot = Row*;

// The complete geometry-independent state of one screen: content in its
// source geometry (storage borrowed until the old pool is destroyed),
// view/selection anchors, and the
// presentation scalars the terminal does not re-push after a rebuild.
struct ResizeState {
    u16 columns = 0;
    u16 rows = 0;
    u32 saveLines = 0;
    bool active = false;
    RowSlot* rowRing = nullptr;
    const Row* zeroRow = nullptr;
    u32 rowCapacity = 0;
    u32 rowEnd = 0;
    u32 historyRows = 0;
    u32 viewOffset = 0;
    Rect selection;
    Screen::SelectSnapTo snapTo = Screen::SelectSnapTo::Char;
};

namespace {
    static TerminalCell* rowData(RowSlot slot) {
        return slot == nullptr ? nullptr : slot->cells;
    }

    static u8 rowProtection(const TerminalCell* row, u16 columns) {
        u8 result = 0;
        for (u16 column = 0; column < columns; ++column) {
            result |= row[column].protected_char;
        }
        return result;
    }

    static bool rowContainsWide(const TerminalCell* row, u16 columns) {
        for (u16 column = 0; column < columns; ++column) {
            if (row[column].dwidth || row[column].dwidth_cont) {
                return true;
            }
        }
        return false;
    }

    static u16 rowWrapColumn(const Row* row, u16 columns) {
        if (row != nullptr) {
            for (u16 column = 0; column < columns; ++column) {
                if (row->cells[column].wrap) {
                    return column;
                }
            }
        }
        return columns;
    }

    static void restoreRowWrap(Row* row, u16 columns, u16 wrapColumn) {
        if (row == nullptr) {
            return;
        }
        for (u16 column = 0; column < columns; ++column) {
            row->cells[column].wrap = 0;
        }
        if (wrapColumn < columns) {
            row->cells[wrapColumn].wrap = 1;
        }
    }

    [[gnu::always_inline]] inline void storeAsciiCells(u64* __restrict output, const u8* __restrict input, u16 count, u64 style, u64 content) {
        while (count >= 4) {
            output[0] = style;
            output[1] = content | input[0];
            output[2] = style;
            output[3] = content | input[1];
            output[4] = style;
            output[5] = content | input[2];
            output[6] = style;
            output[7] = content | input[3];
            output += 8;
            input += 4;
            count -= 4;
        }
        while (count != 0) {
            output[0] = style;
            output[1] = content | *input++;
            output += 2;
            --count;
        }
    }

    // Traits supplies the storage types selected by the factories from the
    // actual geometry; resize rebuilds convert between instantiations through
    // ResizeState.
    template <typename Traits>
    struct ScreenBase: public Screen {
        using Coord = typename Traits::Coord;

        ~ScreenBase() noexcept;

        ScreenBase(VtCellExtras& extras, ObjPool& pool);
        ScreenBase(VtCellExtras& extras, ObjPool& pool, u16 columns, u16 rows, const TerminalColors* colors, u16 saveLines);

        void fillCells(u16 ch, const TerminalCell& attrs) override;
        void setLineAttribute(u16 row, u8 attribute) override;
        u8 lineAttribute(u16 row) const noexcept override;
        void setSemanticPrompt(u16 row, ScreenSemanticPrompt prompt) override;
        ScreenSemanticPrompt semanticPrompt(i32 row) const noexcept override;
        bool hasProtection(u16 row, u8 mask) const noexcept override;
        bool wrapped(u16 row, u16 column) const noexcept override;
        void setWrapped(u16 row, u16 column) override;
        void writeGrapheme(u16 row, u16 column, const u32* codepoints, size_t count, bool wide, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void writeSixelCells(u16 row, u16 column, u16 count, const u8* patches, const u8* palette, const TerminalCell& attrs, u32 hyperlink, const TerminalCell& eraseAttrs) override;
        WriteResult writeAsciiRun(u16 row, u16 column, u16 normalEnd, u16 doubleEnd, const u8* input, u16 count, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        WriteResult writeAsciiRunInsert(u16 row, u16 column, u16 normalEnd, u16 doubleEnd, const u8* input, u16 count, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void writeRun(u16 row, u16 column, const u32* codepoints, u16 count, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void writeRepeatedCodepoint(u16 row, u16 column, u16 count, u32 codepoint, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void writeGlyphRun(u16 row, u16 column, const u32* codepoints, const u8* widths, u16 glyphCount, u16 cellCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void writeGraphemeRun(u16 row, u16 column, const u32* codepoints, const ScreenGrapheme* graphemes, u16 count, u16 cellCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void fillRectangle(u16 top, u16 left, u16 bottom, u16 right, u32 codepoint, const TerminalCell& attrs, const TerminalCell& eraseAttrs) override;
        void copyRectangle(u16 sourceTop, u16 sourceLeft, u16 targetTop, u16 targetLeft, u16 height, u16 width, const TerminalCell& eraseAttrs) override;
        void changeRectangleAttributes(u16 top, u16 left, u16 bottom, u16 right, CellAttributeChange change) override;
        u16 checksum(u16 top, u16 left, u16 bottom, u16 right, u8 flags) const noexcept override;
        ScreenHyperlink hyperlinkAt(u16 row, u16 column) const override;
        TerminalCell testCell(u16 row, u16 column) const noexcept override;
        TerminalCell testLogicalCell(i32 row, u16 column) const noexcept override;
        u32 materializedRows() const noexcept override;
        ScreenFrame captureFrame(TerminalRow* rows) const override;
        ScreenInfo info() const noexcept override;

        void collectExtraCells(Vector<TerminalCell*>& cells) override;

        void eraseCells(u16 row, u16 start, u16 count, const TerminalCell& attrs) override;
        void selectiveEraseCells(u16 row, u16 start, u16 count, const TerminalCell& attrs, u8 protectionMask) override;
        void insertCells(u16 row, u16 start, u16 end, u16 count, const TerminalCell& attrs) override;
        void deleteCells(u16 row, u16 start, u16 end, u16 count, const TerminalCell& attrs) override;
        void copyRow(u16 destinationRow, u16 sourceRow, u16 start, u16 count, const TerminalCell& attrs) override;
        void scrollRectangle(u16 top, u16 left, u16 bottom, u16 right, i32 rows, const TerminalCell& attrs) override;
        void rotateRows(u16 top, u16 bottom, i32 rows) override;

        void expose() override;
        void resetDamage() override;
        bool hasBlinkingText() const noexcept override;

        Rect currentSelection() const noexcept override;
        bool hasSelection() const override;
        void beginSelection(Point point) override;
        void updateSelection(Rect selection) override;
        void cycleSelectionSnap() override;
        void clearSelection() override;
        bool selectedText(Buffer& text) const override;
        Point logicalPoint(Point point) const override;
        Rect logicalSelection() const override;

        Coord nCols = 0;
        Coord nRows = 0;
        u32 saveLines = 0;
        u32 viewOffset = 0;
        u32 rowCapacity = 0;
        u32 rowEnd = 0;
        u32 historyRows = 0;
        RowSlot* rowRing = nullptr;
        Row* zeroRow = nullptr;
        const TerminalColors* colors = nullptr;
        VtCellExtras& extras;
        ObjPool& pool;
        u32 contentRevision = 1;
        Vector<TerminalCell> erasedRowTemplate;
        TerminalCell erasedRowCell{};
        bool erasedRowTemplateValid = false;
        Row* freeRows = nullptr;
        Rect selection;
        SelectSnapTo snapTo = SelectSnapTo::Char;
        Buffer damageStorage;

        struct LinkPosition {
            i32 row = 0;
            u16 column = 0;
        };

        struct LinkPart {
            LinkPosition position;
            size_t begin = 0;
            size_t end = 0;
        };

        mutable Vector<LinkPosition> linkLeft;
        mutable Vector<LinkPart> linkParts;
        mutable Buffer linkScratch;
        mutable Vector<TerminalCell> selectionScratch;

        // Damage is row-granular: shaping context spreads any edit across
        // its whole row, so a damaged row re-renders wholly.
        struct Damage {
            u8* rows = nullptr;
            u16 height = 0;
            u16 dirtyRows = 0;

            void configure(void* storage, u16 rows);
            void reset();
            void expose();
            void addRow(u16 row);
        };

        Damage damage;

        // Two cache levels in front of the strip arenas. The first maps a
        // fast hash of the raw cell bytes to the identity of the
        // materialized span; extra refs die with their store, so the
        // cellExtrasChangedListeners hook drops this whole level by
        // epoch. The second maps the materialized identity to the
        // rendered strip and survives collections: recovery is a
        // re-materialization and a hit, not a re-render. A presentation
        // invalidation resets everything including the arenas.
        ScreenRowRef viewRow(i32 viewRow) const override;

        u32 wrapRow(i64 row) const noexcept;
        RowSlot& logicalRowSlot(int row);
        bool logicalRowHasWide(int row) const noexcept;
        void markLogicalRowWide(int row);
        Row* rawLogicalRowObject(int row) const noexcept;
        const Row* getLogicalRowObject(int row) const noexcept;
        const Row* getViewRowObject(int row) const noexcept;
        TerminalCell* rawLogicalRow(int row) const noexcept;
        const TerminalCell* getLogicalRowPtr(int row) const;
        const TerminalCell* getViewRowPtr(int row) const;
        TerminalCell* mutableRow(RowSlot& slot);
        TerminalCell* mutableLogicalRow(int row);
        Row* allocateRow();
        void releaseRow(Row* row);
        void initializeRows(u16 columns, u16 rows, u32 history);
        void installRow(int row, const TerminalCell* source, u16 sourceColumns, u8 lineAttribute, u8 protection, ScreenSemanticPrompt semanticPrompt);
        bool emptyRow(const TerminalCell* row, u16 columns) const;

        void eraseRange(TerminalCell* start, TerminalCell* end, const TerminalCell& attrs);
        void copyCells(TerminalCell* destination, const TerminalCell* source, u32 count);
        void moveCells(TerminalCell* destination, const TerminalCell* source, u32 count);
        void eraseInRow(RowSlot& slot, u16 row, u16 start, u16 count, const TerminalCell& attrs);
        [[gnu::always_inline]] inline void clearWideBoundary(u16 row, u16 boundary, const TerminalCell& attrs);
        [[gnu::always_inline]] inline void clearWideBoundary(RowSlot& slot, u16 row, u16 boundary, const TerminalCell& attrs);
        [[gnu::always_inline]] inline void repairWideBoundary(u16 row, u16 boundary, const TerminalCell& attrs);
        [[gnu::always_inline]] inline void repairWideBoundary(RowSlot& slot, u16 row, u16 boundary, const TerminalCell& attrs);
        [[gnu::noinline]] void clearWideBoundarySlow(TerminalCell* cells, u16 row, u16 boundary, const TerminalCell& attrs);
        [[gnu::noinline]] void repairWideBoundarySlow(TerminalCell* cells, u16 row, u16 boundary, const TerminalCell& attrs, bool eraseLeft);
        void moveWrap(u16 row, u16 sourceColumn, u16 destinationColumn);
        void moveInRow(u16 row, u16 destination, u16 source, u16 count);
        void scrollRectangleImpl(u16 top, u16 left, u16 bottom, u16 right, u16 count, const TerminalCell& attrs, bool down);
        void scrollPartialRectangleUpOne(u16 top, u16 left, u16 bottom, u16 right, const TerminalCell& attrs);
        [[gnu::always_inline]] inline void scrollPartialRectangleRow(RowSlot& destinationObject, const Row* sourceObject, u16 destinationRow, u16 left, u16 right, const TerminalCell& attrs);
        void rotateRowPointersUp(u16 top, u16 bottom, u16 count);
        void rotateRowPointersDown(u16 top, u16 bottom, u16 count);
        void clearRows(u16 begin, u16 end, const TerminalCell& attrs);
        void changeContent();
        void damageRow(u16 row);
        void damageRows(u16 top, u16 bottom);
        void resizeDamage(u16 rows);
        TerminalCell* overwriteWideSpan(u16 row, u16 start, u16 count, const TerminalCell& eraseAttrs);
        TerminalCell* prepareSpan(RowSlot& slot, u16 row, u16 start, u16 count, const TerminalCell& eraseAttrs);
        TerminalCell* prepareSpan(u16 row, u16 start, u16 count, const TerminalCell& eraseAttrs);
        TerminalCell& prepareCell(RowSlot& slot, u16 row, u16 column, const TerminalCell& eraseAttrs);
        TerminalCell& prepareCell(u16 row, u16 column, const TerminalCell& eraseAttrs);
        [[gnu::always_inline]] inline void writePreparedCell(u16 row, u16 column, const TerminalCell& lead, bool wide, const TerminalCell& attrs, const TerminalCell& eraseAttrs);

        ResizeState* moveIntoState();
        void restoreLayoutState(ResizeState& state, u16 rows, const TerminalColors* colors);
        void layoutPrimary(ResizeState& state, u16 columns, u16 rows, const TerminalColors* colors, Cursor& cursor, Cursor* trackedCursor);
        void layoutAlternate(ResizeState& state, u16 columns, u16 rows, const TerminalColors* colors, Cursor& cursor, Cursor* trackedCursor);

        template <bool primary>
        void layoutCopy(ResizeState& state, u16 columns, u16 rows, Cursor& cursor, Cursor* trackedCursor);

        void layoutReflow(ResizeState& state, u16 columns, u16 rows, Cursor& cursor, Cursor* trackedCursor);
        void dropScrollbackHistoryImpl();
        void scrollUpVisible(u16 top, u16 bottom, u16 count, const TerminalCell& attrs);
        void scrollUpWithHistory(u16 top, u16 bottom, u16 count, const TerminalCell& attrs);
        void scrollDownVisible(u16 top, u16 bottom, u16 count, const TerminalCell& attrs);
        void pageUpImpl(u16 count);
        void pageDownImpl(u16 count);
        bool pageToBottomImpl();

        template <bool captureHistory>
        void writeAsciiLinesImpl(u16 row, const u8* input, const u16* lengths, u16 lineCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs);

        static SelectSnapTo nextSelectSnapTo(SelectSnapTo snapTo);

        void vscrollSelection(u16 top, u16 bottom, int verticalOffset, bool captureHistory);
        void invalidateSelection(const Rect&& changed);
        bool selectionValid() const;
        Rect selectionForView() const;
        Rect snappedSelection() const;
        Rect computeSnappedSelection() const;

        // Word snapping walks whole wrapped-line chains; captureFrame asks
        // for the snapped rectangle every frame of a drag, so memoize it
        // against the selection, the snap mode, the view and the content.
        mutable Rect snappedCache;
        mutable Rect snappedCacheSelection;
        mutable SelectSnapTo snappedCacheSnapTo = SelectSnapTo::Char;
        mutable u32 snappedCacheRevision = 0;
        mutable i32 snappedCacheViewOffset = 0;
        mutable bool snappedCacheValid = false;

        CellExtraStore& cellExtras() const noexcept;
        size_t copyDamage(TerminalRow* rows) const;
        static constexpr size_t cellSize = sizeof(TerminalCell);
    };

    template <typename Traits>
    struct PrimaryScreenImpl final: public ScreenBase<Traits> {
        using ScreenBase<Traits>::ScreenBase;

        Screen* resized(ObjPool& pool, u16 columns, u16 rows, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) override;
        Screen* resizedWithHistory(ObjPool& pool, u16 columns, u16 rows, u16 saveLines, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) override;
        void dropHistory() override;
        void writeAsciiLines(u16 row, const u8* input, const u16* lengths, u16 lineCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void scrollRows(u16 top, u16 bottom, i32 rows, const TerminalCell& attrs) override;
        bool scrollView(i32 rows) override;
    };

    template <typename Traits>
    struct AlternateScreenImpl final: public ScreenBase<Traits> {
        using ScreenBase<Traits>::ScreenBase;

        Screen* resized(ObjPool& pool, u16 columns, u16 rows, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) override;
        Screen* resizedWithHistory(ObjPool& pool, u16 columns, u16 rows, u16 saveLines, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) override;
        void dropHistory() override;
        bool scrollView(i32 rows) override;
        void writeAsciiLines(u16 row, const u8* input, const u16* lengths, u16 lineCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) override;
        void scrollRows(u16 top, u16 bottom, i32 rows, const TerminalCell& attrs) override;
    };

    static constexpr u32 whitespaceClass = 0x110000;
    static constexpr u32 identifierClass = 0x110001;

    static u32 wordClass(u32 codepoint) {
        switch (unicodeCodepointProperties(codepoint).category) {
            case GeneralCategory::UppercaseLetter:
            case GeneralCategory::LowercaseLetter:
            case GeneralCategory::TitlecaseLetter:
            case GeneralCategory::ModifierLetter:
            case GeneralCategory::OtherLetter:
            case GeneralCategory::NonspacingMark:
            case GeneralCategory::SpacingMark:
            case GeneralCategory::EnclosingMark:
            case GeneralCategory::DecimalNumber:
            case GeneralCategory::LetterNumber:
            case GeneralCategory::OtherNumber:
            case GeneralCategory::ConnectorPunctuation:
                return identifierClass;
            case GeneralCategory::SpaceSeparator:
            case GeneralCategory::LineSeparator:
            case GeneralCategory::ParagraphSeparator:
                return whitespaceClass;
            default:
                // Adjacent repetitions of one punctuation/symbol codepoint form
                // a useful selectable run, while unlike punctuation stays split.
                return codepoint;
        }
    }

    struct SelectionRow {
        const TerminalCell* cells;
        int columns;
    };

    struct TokenBounds {
        int left;
        int right;
    };

    static int selectionCellLead(const SelectionRow& row, int column) {
        column = max(0, min(column, row.columns - 1));
        return row.cells[column].dwidth_cont && column > 0 ? column - 1 : column;
    }

    static u32 selectionCodepoint(const SelectionRow& row, int column) {
        const u32 codepoint = row.cells[selectionCellLead(row, column)].uc_pt;
        return codepoint == 0 ? (u32)(' ') : codepoint;
    }

    static int nextSelectionCell(const SelectionRow& row, int column) {
        const int lead = selectionCellLead(row, column);
        return min(row.columns, lead + (row.cells[lead].dwidth ? 2 : 1));
    }

    static int previousSelectionCell(const SelectionRow& row, int column) {
        return selectionCellLead(row, max(0, column - 1));
    }

    static bool identifierCodepoint(u32 codepoint) {
        return wordClass(codepoint) == identifierClass;
    }

    static bool asciiAlpha(u32 codepoint) {
        return (codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z');
    }

    static bool uriCodepoint(u32 codepoint) {
        if (identifierCodepoint(codepoint)) {
            return true;
        }
        switch (codepoint) {
            case '-':
            case '.':
            case '_':
            case '~':
            case ':':
            case '/':
            case '?':
            case '#':
            case '[':
            case ']':
            case '@':
            case '!':
            case '$':
            case '&':
            case '\'':
            case '(':
            case ')':
            case '*':
            case '+':
            case ',':
            case ';':
            case '=':
            case '%':
                return true;
            default:
                return false;
        }
    }

    static bool uriSchemeCodepoint(u32 codepoint) {
        return asciiAlpha(codepoint) || (codepoint >= '0' && codepoint <= '9') || codepoint == '+' || codepoint == '-' || codepoint == '.';
    }

    static bool compoundCodepoint(u32 codepoint) {
        return identifierCodepoint(codepoint) || codepoint == '.' || codepoint == '-' || codepoint == '/' || codepoint == ':' || codepoint == '@' || codepoint == '\\' || codepoint == '~';
    }

    static TokenBounds expandTokenRun(const SelectionRow& row, int column, bool (*included)(u32)) {
        int left = selectionCellLead(row, column);
        if (!included(selectionCodepoint(row, left))) {
            return {left, left};
        }
        while (left > 0) {
            const int previous = previousSelectionCell(row, left);
            if (!included(selectionCodepoint(row, previous))) {
                break;
            }
            left = previous;
        }

        int right = left;
        while (right < row.columns && included(selectionCodepoint(row, right))) {
            right = nextSelectionCell(row, right);
        }
        return {left, right};
    }

    static bool unmatchedClosingDelimiter(const SelectionRow& row, int left, int right, u32 opening, u32 closing) {
        int balance = 0;
        for (int column = left; column < right; column = nextSelectionCell(row, column)) {
            const u32 codepoint = selectionCodepoint(row, column);
            balance += codepoint == opening;
            balance -= codepoint == closing;
        }
        return balance < 0;
    }

    static int schemeLessUriLeft(const SelectionRow& row, const TokenBounds& run) {
        int candidate = -1;
        bool hasDot = false;
        bool previousDot = false;
        for (int cursor = run.left; cursor < run.right; cursor = nextSelectionCell(row, cursor)) {
            const u32 codepoint = selectionCodepoint(row, cursor);
            if (identifierCodepoint(codepoint)) {
                if (candidate < 0) {
                    candidate = cursor;
                }
                previousDot = false;
                continue;
            }
            if (codepoint == '-' && candidate >= 0) {
                previousDot = false;
                continue;
            }
            if (codepoint == '.' && candidate >= 0 && !previousDot) {
                hasDot = true;
                previousDot = true;
                continue;
            }
            if (codepoint == '/' && candidate >= 0 && hasDot && !previousDot) {
                return candidate;
            }
            candidate = -1;
            hasDot = false;
            previousDot = false;
        }
        return -1;
    }

    static TokenBounds uriTokenBounds(const SelectionRow& row, int column) {
        const int clicked = selectionCellLead(row, column);
        const TokenBounds run = expandTokenRun(row, clicked, uriCodepoint);
        if (run.left == run.right) {
            return {clicked, clicked};
        }

        int schemeLeft = -1;
        int uriLeft = -1;
        for (int cursor = run.left; cursor < run.right; cursor = nextSelectionCell(row, cursor)) {
            const u32 codepoint = selectionCodepoint(row, cursor);
            if (schemeLeft < 0) {
                if (asciiAlpha(codepoint)) {
                    schemeLeft = cursor;
                }
                continue;
            }
            if (uriSchemeCodepoint(codepoint)) {
                continue;
            }
            if (codepoint != ':') {
                schemeLeft = -1;
                continue;
            }
            const int firstSlash = nextSelectionCell(row, cursor);
            const int secondSlash = firstSlash < run.right ? nextSelectionCell(row, firstSlash) : run.right;
            if (firstSlash < run.right && selectionCodepoint(row, firstSlash) == '/' && secondSlash < run.right && selectionCodepoint(row, secondSlash) == '/') {
                uriLeft = schemeLeft;
                break;
            }
            schemeLeft = -1;
        }
        if (uriLeft < 0) {
            uriLeft = schemeLessUriLeft(row, run);
        }
        if (uriLeft < 0 || clicked < uriLeft) {
            return {clicked, clicked};
        }

        int uriRight = run.right;
        while (uriRight > uriLeft) {
            const int last = previousSelectionCell(row, uriRight);
            const u32 codepoint = selectionCodepoint(row, last);
            const bool trailingPunctuation = codepoint == '.' || codepoint == ',' || codepoint == ';' || codepoint == ':' || codepoint == '!' || codepoint == '?' || codepoint == '\'';
            const bool unmatchedBracket = (codepoint == ')' && unmatchedClosingDelimiter(row, uriLeft, uriRight, '(', ')')) || (codepoint == ']' && unmatchedClosingDelimiter(row, uriLeft, uriRight, '[', ']'));
            if (!trailingPunctuation && !unmatchedBracket) {
                break;
            }
            uriRight = last;
        }
        return clicked < uriRight ? TokenBounds{uriLeft, uriRight} : TokenBounds{clicked, clicked};
    }

    static bool validCompoundPrefix(const SelectionRow& row, int left, int firstIdentifier) {
        const u32 first = selectionCodepoint(row, left);
        const int secondColumn = nextSelectionCell(row, left);
        if (secondColumn == firstIdentifier) {
            return first == '/' || first == '\\' || first == '.' || first == '-' || first == '@';
        }

        const u32 second = selectionCodepoint(row, secondColumn);
        const int thirdColumn = nextSelectionCell(row, secondColumn);
        if (thirdColumn == firstIdentifier) {
            return (first == '~' && (second == '/' || second == '\\')) || (first == '.' && (second == '/' || second == '\\')) || (first == '-' && second == '-');
        }

        const u32 third = selectionCodepoint(row, thirdColumn);
        const int fourthColumn = nextSelectionCell(row, thirdColumn);
        return fourthColumn == firstIdentifier && first == '.' && second == '.' && (third == '/' || third == '\\');
    }

    static TokenBounds compoundTokenBounds(const SelectionRow& row, int column) {
        const int clicked = selectionCellLead(row, column);
        if (!identifierCodepoint(selectionCodepoint(row, clicked))) {
            return {clicked, clicked};
        }

        TokenBounds token = expandTokenRun(row, clicked, compoundCodepoint);
        while (token.right > token.left) {
            const int last = previousSelectionCell(row, token.right);
            const u32 codepoint = selectionCodepoint(row, last);
            if (identifierCodepoint(codepoint) || codepoint == '/' || codepoint == '\\') {
                break;
            }
            token.right = last;
        }

        int firstIdentifier = token.left;
        while (firstIdentifier < token.right && !identifierCodepoint(selectionCodepoint(row, firstIdentifier))) {
            firstIdentifier = nextSelectionCell(row, firstIdentifier);
        }
        if (firstIdentifier == token.right || clicked >= token.right) {
            return {clicked, clicked};
        }

        if (firstIdentifier != token.left && !validCompoundPrefix(row, token.left, firstIdentifier)) {
            token.left = firstIdentifier;
        }
        return token;
    }

    static TokenBounds semanticTokenBounds(const SelectionRow& row, int column) {
        const TokenBounds uri = uriTokenBounds(row, column);
        if (uri.left != uri.right) {
            return uri;
        }
        return compoundTokenBounds(row, column);
    }

    static TokenBounds wordTokenBounds(const SelectionRow& row, int column) {
        int left = selectionCellLead(row, column);
        const u32 selectedClass = wordClass(selectionCodepoint(row, left));
        while (left > 0) {
            const int previous = previousSelectionCell(row, left);
            if (wordClass(selectionCodepoint(row, previous)) != selectedClass) {
                break;
            }
            left = previous;
        }

        int right = left;
        while (right < row.columns && wordClass(selectionCodepoint(row, right)) == selectedClass) {
            right = nextSelectionCell(row, right);
        }
        return {left, right};
    }

    // A column shrink can copy the leading half of a wide glyph while
    // clipping its continuation.  Never publish or retain such a partial
    // cell: editing code relies on the same lead/continuation invariant.
    static void normalizeWideRow(TerminalCell* row, u16 columns) {
        for (u16 column = 0; column < columns; ++column) {
            const bool orphanLead = row[column].dwidth && (column + 1 == columns || !row[column + 1].dwidth_cont);
            const bool orphanContinuation = row[column].dwidth_cont && (column == 0 || !row[column - 1].dwidth);
            if (orphanLead || orphanContinuation) {
                row[column] = TerminalCell{};
            }
        }
    }

    static u32 nextPowerOfTwo(u32 value) {
        u32 result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

    static u32 wrapStateRow(const ResizeState& state, i64 row) {
        return (u32)(row) & (state.rowCapacity - 1);
    }

    static const Row* stateRowObject(const ResizeState& state, int row) {
        const u32 slot = wrapStateRow(state, (i64)(state.rowEnd) - state.rows + row);
        const Row* const result = state.rowRing[slot];
        return result != nullptr ? result : state.zeroRow;
    }

}

template <typename Traits>
ScreenBase<Traits>::ScreenBase(VtCellExtras& extras_, ObjPool& pool_)
    : extras(extras_)
    , pool(pool_)
{
}

namespace {

    struct SmallScreenTraits {
        using Coord = u8;
    };

    struct LargeScreenTraits {
        using Coord = u16;
    };

    using SmallPrimaryScreen = PrimaryScreenImpl<SmallScreenTraits>;
    using LargePrimaryScreen = PrimaryScreenImpl<LargeScreenTraits>;
    using SmallAlternateScreen = AlternateScreenImpl<SmallScreenTraits>;
    using LargeAlternateScreen = AlternateScreenImpl<LargeScreenTraits>;

    static constexpr bool smallScreenGeometry(u32 columns, u32 rows, u32) {
        return columns <= 0xff && rows <= 0xff;
    }

    template <typename Impl>
    static Impl* makeScreen(VtCellExtras& extras, ObjPool& pool) {
        return pool.make<Impl>(extras, pool);
    }

    template <typename Impl>
    static Screen* makePrimaryScreenFromState(VtCellExtras& extras, ObjPool& pool, ResizeState& resizeState, u16 columns, u16 rows, const TerminalColors* colors, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
        Impl* const result = makeScreen<Impl>(extras, pool);
        if (resizeState.active) {
            result->layoutPrimary(resizeState, columns, rows, colors, cursor, trackedCursor);
        }
        return result;
    }

    template <typename Impl>
    static Screen* makeAlternateScreenFromState(VtCellExtras& extras, ObjPool& pool, ResizeState& resizeState, u16 columns, u16 rows, const TerminalColors* colors, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
        Impl* const result = makeScreen<Impl>(extras, pool);
        if (resizeState.active) {
            result->layoutAlternate(resizeState, columns, rows, colors, cursor, trackedCursor);
        }
        return result;
    }

    static Screen* makePrimaryFromState(VtCellExtras& extras, ObjPool& pool, ResizeState& resizeState, u16 columns, u16 rows, const TerminalColors* colors, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
        if (smallScreenGeometry(columns, rows, resizeState.saveLines)) {
            return makePrimaryScreenFromState<SmallPrimaryScreen>(extras, pool, resizeState, columns, rows, colors, cursor, trackedCursor);
        }
        return makePrimaryScreenFromState<LargePrimaryScreen>(extras, pool, resizeState, columns, rows, colors, cursor, trackedCursor);
    }

    static Screen* makeAlternateFromState(VtCellExtras& extras, ObjPool& pool, ResizeState& resizeState, u16 columns, u16 rows, const TerminalColors* colors, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
        if (smallScreenGeometry(columns, rows, 0)) {
            return makeAlternateScreenFromState<SmallAlternateScreen>(extras, pool, resizeState, columns, rows, colors, cursor, trackedCursor);
        }
        return makeAlternateScreenFromState<LargeAlternateScreen>(extras, pool, resizeState, columns, rows, colors, cursor, trackedCursor);
    }

}

Screen* Screen::createPrimary(VtCellExtras& extras, ObjPool& pool, u16 columns, u16 rows, const TerminalColors* colors, u16 saveLines) {
    if (smallScreenGeometry(columns, rows, saveLines)) {
        return pool.make<SmallPrimaryScreen>(extras, pool, columns, rows, colors, saveLines);
    }
    return pool.make<LargePrimaryScreen>(extras, pool, columns, rows, colors, saveLines);
}

Screen* Screen::createAlternate(VtCellExtras& extras, ObjPool& pool, u16 columns, u16 rows, const TerminalColors* colors) {
    if (smallScreenGeometry(columns, rows, 0)) {
        return pool.make<SmallAlternateScreen>(extras, pool, columns, rows, colors, 0);
    }
    return pool.make<LargeAlternateScreen>(extras, pool, columns, rows, colors, 0);
}

Screen* Screen::createInactiveAlternate(VtCellExtras& extras, ObjPool& pool) {
    return makeScreen<SmallAlternateScreen>(extras, pool);
}

template <typename Traits>
Screen* PrimaryScreenImpl<Traits>::resized(ObjPool& destination, u16 columns, u16 rows, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
    return this->resizedWithHistory(destination, columns, rows, (u16)(this->saveLines), cursor, trackedCursor);
}

template <typename Traits>
Screen* PrimaryScreenImpl<Traits>::resizedWithHistory(ObjPool& destination, u16 columns, u16 rows, u16 saveLines, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
    ResizeState* const resizeState = this->moveIntoState();
    resizeState->saveLines = saveLines;
    return makePrimaryFromState(this->extras, destination, *resizeState, columns, rows, this->colors, cursor, trackedCursor);
}

template <typename Traits>
Screen* AlternateScreenImpl<Traits>::resizedWithHistory(ObjPool& destination, u16 columns, u16 rows, u16, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
    return this->resized(destination, columns, rows, cursor, trackedCursor);
}

template <typename Traits>
Screen* AlternateScreenImpl<Traits>::resized(ObjPool& destination, u16 columns, u16 rows, Screen::Cursor& cursor, Screen::Cursor* trackedCursor) {
    ResizeState* const resizeState = this->moveIntoState();
    Screen* const result = makeAlternateFromState(this->extras, destination, *resizeState, columns, rows, this->colors, cursor, trackedCursor);
    cursor.position.x = min<int>(cursor.position.x, columns - 1);
    cursor.position.y = min<int>(cursor.position.y, rows - 1);
    if (trackedCursor != nullptr) {
        trackedCursor->position.x = min<int>(trackedCursor->position.x, columns - 1);
        trackedCursor->position.y = min<int>(trackedCursor->position.y, rows - 1);
        trackedCursor->pendingWrap = false;
    }
    return result;
}

template <typename Traits>
ScreenInfo ScreenBase<Traits>::info() const noexcept {
    return {
        .revision = contentRevision,
        .cellCapacity = (size_t)(nCols) * (nRows + saveLines),
        .historyRows = historyRows,
        .viewOffset = viewOffset,
        .columns = nCols,
        .rows = nRows,
        .saveLines = (u16)(saveLines),
    };
}

template <typename Traits>
void ScreenBase<Traits>::expose() {
    changeContent();
    damage.expose();
}

template <typename Traits>
void ScreenBase<Traits>::resetDamage() {
    damage.reset();
}

template <typename Traits>
Rect ScreenBase<Traits>::currentSelection() const noexcept {
    return selection;
}

template <typename Traits>
bool ScreenBase<Traits>::hasSelection() const {
    return !snappedSelection().empty();
}

template <typename Traits>
Screen::SelectSnapTo ScreenBase<Traits>::nextSelectSnapTo(SelectSnapTo value) {
    return (SelectSnapTo)(((u8)(value) + 1) % (u8)(SelectSnapTo::COUNT));
}

template <typename Traits>
void ScreenBase<Traits>::beginSelection(Point point) {
    selection = {point, point};
    snapTo = SelectSnapTo::Char;
    changeContent();
}

template <typename Traits>
void ScreenBase<Traits>::updateSelection(Rect value) {
    selection = value;
    changeContent();
}

template <typename Traits>
void ScreenBase<Traits>::cycleSelectionSnap() {
    snapTo = nextSelectSnapTo(snapTo);
    changeContent();
}

template <typename Traits>
void ScreenBase<Traits>::clearSelection() {
    if (!selection.null()) {
        selection.clear();
        changeContent();
    }
}

template <typename Traits>
bool ScreenBase<Traits>::hasBlinkingText() const noexcept {
    for (Coord row = 0; row != nRows; ++row) {
        const TerminalCell* const cells = getViewRowPtr(row);
        for (Coord column = 0; column != nCols; ++column) {
            if (cells[column].blink) {
                return true;
            }
        }
    }
    return false;
}

template <typename Traits>
CellExtraStore& ScreenBase<Traits>::cellExtras() const noexcept {
    return *extras.store;
}

template <typename Traits>
ScreenBase<Traits>::ScreenBase(VtCellExtras& extras_, ObjPool& pool_, u16 nCols_, u16 nRows_, const TerminalColors* colors_, u16 saveLines_)
    : nCols((Coord)(nCols_))
    , nRows((Coord)(nRows_))
    , saveLines(saveLines_)
    , viewOffset(0)
    , colors(colors_)
    , extras(extras_)
    , pool(pool_)
{
    initializeRows(nCols_, nRows_, 0);
    resizeDamage(nRows);
}

template <typename Traits>
u32 ScreenBase<Traits>::wrapRow(i64 row) const noexcept {
    return (u32)(row) & (rowCapacity - 1);
}

template <typename Traits>
RowSlot& ScreenBase<Traits>::logicalRowSlot(int row) {
    return rowRing[wrapRow((i64)(rowEnd)-nRows + row)];
}

template <typename Traits>
TerminalCell* ScreenBase<Traits>::rawLogicalRow(int row) const noexcept {
    return rowData(rawLogicalRowObject(row));
}

template <typename Traits>
bool ScreenBase<Traits>::logicalRowHasWide(int row) const noexcept {
    const Row* const object = rawLogicalRowObject(row);
    return object != nullptr && object->metadata.wide;
}

template <typename Traits>
void ScreenBase<Traits>::markLogicalRowWide(int row) {
    RowSlot& slot = logicalRowSlot(row);
    if (slot == nullptr) {
        mutableRow(slot);
    }
    slot->metadata.wide = true;
}

template <typename Traits>
Row* ScreenBase<Traits>::rawLogicalRowObject(int row) const noexcept {
    return rowRing[wrapRow((i64)(rowEnd)-nRows + row)];
}

template <typename Traits>
const Row* ScreenBase<Traits>::getLogicalRowObject(int row) const noexcept {
    const Row* const result = rawLogicalRowObject(row);
    return result != nullptr ? result : zeroRow;
}

template <typename Traits>
const Row* ScreenBase<Traits>::getViewRowObject(int row) const noexcept {
    return getLogicalRowObject(row - viewOffset);
}

template <typename Traits>
Row* ScreenBase<Traits>::allocateRow() {
    Row* result;
    if (freeRows == nullptr) {
        result = static_cast<Row*>(pool.allocateOverAligned(sizeof(Row) + (size_t)(nCols)*cellSize, alignof(Row)));
    } else {
        result = freeRows;
        freeRows = freeRows->freeNext;
    }
    memset(result, 0, sizeof(Row) + (size_t)(nCols)*cellSize);
    result->id = nextRowIdentity();
    return result;
}

template <typename Traits>
ScreenRowRef ScreenBase<Traits>::viewRow(i32 viewRow_) const {
    const Row* const row = rawLogicalRowObject(viewRow_ - (i32)(viewOffset));
    if (row == nullptr) {
        return {};
    }
    return {row->cells, row->id};
}

template <typename Traits>
void ScreenBase<Traits>::releaseRow(Row* row) {
    if (row != nullptr) {
        // id and freeNext share their slot; reuse assigns a fresh id.
        row->freeNext = freeRows;
        freeRows = row;
    }
}

template <typename Traits>
TerminalCell* ScreenBase<Traits>::mutableLogicalRow(int row) {
    return mutableRow(logicalRowSlot(row));
}

template <typename Traits>
TerminalCell* ScreenBase<Traits>::mutableRow(RowSlot& slot) {
    if (slot == nullptr) {
        slot = allocateRow();
    }
    slot->id = nextRowIdentity();
    return slot->cells;
}

template <typename Traits>
void ScreenBase<Traits>::initializeRows(u16 columns, u16 rows, u32 history) {
    nCols = (Coord)(columns);
    nRows = (Coord)(rows);
    rowCapacity = nextPowerOfTwo((u32)(rows) + saveLines);
    rowRing = static_cast<RowSlot*>(pool.allocate((size_t)(rowCapacity) * sizeof(RowSlot)));
    memset(rowRing, 0, (size_t)(rowCapacity) * sizeof(RowSlot));
    zeroRow = static_cast<Row*>(pool.allocateOverAligned(sizeof(Row) + (size_t)(columns)*cellSize, alignof(Row)));
    memset(zeroRow, 0, sizeof(Row) + (size_t)(columns)*cellSize);
    historyRows = history;
    rowEnd = ((u32)(history) + rows) & (rowCapacity - 1);
}

template <typename Traits>
bool ScreenBase<Traits>::emptyRow(const TerminalCell* row, u16 columns) const {
    return memcmp(row, zeroRow->cells, (size_t)(columns)*cellSize) == 0;
}

template <typename Traits>
void ScreenBase<Traits>::installRow(int row, const TerminalCell* source, u16 sourceColumns, u8 lineAttribute_, u8 protection, ScreenSemanticPrompt semanticPrompt_) {
    const u16 count = min<u16>(sourceColumns, nCols);
    if (lineAttribute_ == 0 && semanticPrompt_ == ScreenSemanticPrompt::None && emptyRow(source, count)) {
        return;
    }
    Row* const destination = allocateRow();
    memcpy(destination->cells, source, (size_t)(count)*cellSize);
    normalizeWideRow(destination->cells, nCols);
    if (lineAttribute_ == 0 && semanticPrompt_ == ScreenSemanticPrompt::None && emptyRow(destination->cells, nCols)) {
        releaseRow(destination);
        return;
    }
    destination->metadata.lineAttribute = lineAttribute_;
    destination->metadata.protection = rowProtection(destination->cells, nCols) | protection;
    destination->metadata.semanticPrompt = semanticPrompt_;
    destination->metadata.wide = rowContainsWide(destination->cells, nCols);
    logicalRowSlot(row) = destination;
}

template <typename Traits>
void ScreenBase<Traits>::dropScrollbackHistoryImpl() {
    viewOffset = 0;
    if (!selection.null() && selection.tl.y < 0) {
        selection.clear();
    }
    for (int row = -(int)(historyRows); row < 0; ++row) {
        RowSlot& slot = logicalRowSlot(row);
        releaseRow(slot);
        slot = 0;
    }
    historyRows = 0;
    expose();
}

template <typename Traits>
void ScreenBase<Traits>::collectExtraCells(Vector<TerminalCell*>& cells) {
    bool changed = false;
    const auto collectRow = [&](TerminalCell* first, int visibleRow) {
        if (first == nullptr) {
            return;
        }

        STD_ASSERT((size_t)(first) % cellSize == 0);
        u8* const begin = reinterpret_cast<u8*>(first);
        u8* const end = begin + (size_t)(nCols)*cellSize;
        u8* cursor = begin;
        while (cursor != end) {
            auto* hit = static_cast<u8*>(memchr(cursor, TerminalCell::extraRefSentinel, end - cursor));
            if (hit == nullptr) {
                break;
            }
            const size_t offset = (hit - begin) & ~(cellSize - 1);
            TerminalCell* const cell = reinterpret_cast<TerminalCell*>(begin + offset);
            if (cell->hasExtra()) {
                cells.pushBack(cell);
                if (visibleRow >= 0 && visibleRow < nRows) {
                    damage.addRow((u16)(visibleRow));
                    changed = true;
                }
            }
            cursor = reinterpret_cast<u8*>(cell + 1);
        }
    };
    for (int row = -(int)(historyRows); row < nRows; ++row) {
        collectRow(rawLogicalRow(row), row + viewOffset);
    }
    if (changed) {
        changeContent();
    }
}

template <typename Traits>
ScreenBase<Traits>::~ScreenBase() noexcept {
}

template <typename Traits>
ResizeState* ScreenBase<Traits>::moveIntoState() {
    ResizeState* const state = pool.make<ResizeState>();
    state->columns = nCols;
    state->rows = nRows;
    state->saveLines = saveLines;
    state->active = rowRing != nullptr;
    state->rowRing = rowRing;
    state->zeroRow = zeroRow;
    state->rowCapacity = rowCapacity;
    state->rowEnd = rowEnd;
    state->historyRows = historyRows;
    state->viewOffset = viewOffset;
    state->selection = selection;
    state->snapTo = snapTo;
    rowRing = nullptr;
    zeroRow = nullptr;
    rowCapacity = 0;
    rowEnd = 0;
    historyRows = 0;
    return state;
}

template <typename Traits>
void ScreenBase<Traits>::restoreLayoutState(ResizeState& state, u16 nRows_, const TerminalColors* colors_) {
    colors = colors_;
    saveLines = state.saveLines;
    viewOffset = state.viewOffset;
    selection = state.selection;
    snapTo = state.snapTo;
}

template <typename Traits>
void ScreenBase<Traits>::layoutPrimary(ResizeState& state, u16 nCols_, u16 nRows_, const TerminalColors* colors_, Cursor& cursorState, Cursor* trackedCursor) {
    restoreLayoutState(state, nRows_, colors_);
    if (state.columns != nCols_) {
        layoutReflow(state, nCols_, nRows_, cursorState, trackedCursor);
    } else {
        layoutCopy<true>(state, nCols_, nRows_, cursorState, trackedCursor);
    }
    resizeDamage(nRows);
    expose();
}

template <typename Traits>
void ScreenBase<Traits>::layoutAlternate(ResizeState& state, u16 nCols_, u16 nRows_, const TerminalColors* colors_, Cursor& cursorState, Cursor* trackedCursor) {
    restoreLayoutState(state, nRows_, colors_);
    saveLines = 0;
    viewOffset = 0;
    layoutCopy<false>(state, nCols_, nRows_, cursorState, trackedCursor);
    resizeDamage(nRows);
    expose();
}

template <typename Traits>
template <bool primary>
void ScreenBase<Traits>::layoutCopy(ResizeState& state, u16 nCols_, u16 nRows_, Cursor& cursorState, Cursor* trackedCursor) {
    Vector<const Row*> sourceHistory;
    // A rebuild may be lowering the history cap, and the ring is sized
    // for the new one: carrying every old row would claim more history
    // than there are slots to hold it, and the surplus would wrap over
    // rows still in use. Keep the newest that fit and drop the rest,
    // which is what ageing them out would have done anyway.
    const u32 carriedHistory = min<u32>(state.historyRows, (u32)(saveLines));
    sourceHistory.grow(carriedHistory);
    for (int row = -(int)(carriedHistory); row < 0; ++row) {
        sourceHistory.pushBack(stateRowObject(state, row));
    }
    Vector<const Row*> sourceScreen;
    sourceScreen.grow(state.rows);
    for (u16 row = 0; row < state.rows; ++row) {
        sourceScreen.pushBack(stateRowObject(state, row));
    }
    size_t visibleStart = 0;
    size_t screenRows = sourceScreen.length();

    if constexpr (!primary) {
        // An alternate-screen shrink first drops trailing rows that carry
        // no text - styling alone does not hold a row - matching the
        // ghostty pagelist. The cursor does not anchor a blank tail; it
        // clamps into the surviving rows.
        if ((size_t)(nRows_) < screenRows) {
            const auto rowHasText = [&](const Row* row) {
                for (u16 column = 0; column < state.columns; ++column) {
                    const TerminalCell& cell = row->cells[column];
                    if ((cell.uc_pt != 0 && cell.uc_pt != ' ') || cell.hasExtra()) {
                        return true;
                    }
                }
                return false;
            };
            while (screenRows > nRows_ && !rowHasText(sourceScreen[screenRows - 1])) {
                --screenRows;
            }
            if (cursorState.position.y >= (int)(screenRows)) {
                cursorState.position.y = (int)(screenRows)-1;
            }
            if (trackedCursor != nullptr && trackedCursor->position.y >= (int)(screenRows)) {
                trackedCursor->position.y = (int)(screenRows)-1;
            }
        }
    }

    // An interactive shrink first removes rows below the cursor, then moves
    // the smallest necessary prefix above it out of view.  Primary screens
    // retain that prefix as history; alternate screens discard it.
    if (cursorState.position.y + 1 > (int)(nRows_)) {
        const u16 preScroll = (u16)(cursorState.position.y + 1 - nRows_);
        if constexpr (primary) {
            if (saveLines != 0) {
                for (u16 k = 0; k < preScroll; ++k) {
                    sourceHistory.pushBack(sourceScreen[visibleStart + k]);
                    if (sourceHistory.length() > saveLines) {
                        memmove(sourceHistory.mutData(), sourceHistory.data() + 1, (sourceHistory.length() - 1) * sizeof(Row*));
                        sourceHistory.popBack();
                    }
                }
                if (!selection.null()) {
                    if (selection.br.y >= (int)(state.rows)) {
                        if (selection.tl.y < (int)(state.rows)) {
                            selection.clear();
                        }
                    } else {
                        selection.tl.y -= preScroll;
                        selection.br.y -= preScroll;
                        if (selection.tl.y < -(int)(sourceHistory.length())) {
                            selection.clear();
                        }
                    }
                }
                if (viewOffset != 0) {
                    viewOffset = min<size_t>(viewOffset + preScroll, sourceHistory.length());
                }
            } else if (!selection.null()) {
                const bool topInside = selection.tl.y >= 0 && selection.tl.y < (int)(state.rows);
                const bool bottomInside = selection.br.y >= 0 && selection.br.y < (int)(state.rows);
                if (topInside != bottomInside) {
                    selection.clear();
                } else if (topInside) {
                    selection.tl.y -= preScroll;
                    selection.br.y -= preScroll;
                    if (selection.tl.y < 0) {
                        selection.clear();
                    }
                } else if (!(selection.br.y < 0 || selection.tl.y >= (int)(state.rows))) {
                    selection.clear();
                }
            }
        } else if (!selection.null()) {
            const bool topInside = selection.tl.y >= 0 && selection.tl.y < (int)(state.rows);
            const bool bottomInside = selection.br.y >= 0 && selection.br.y < (int)(state.rows);
            if (topInside != bottomInside) {
                selection.clear();
            } else if (topInside) {
                selection.tl.y -= preScroll;
                selection.br.y -= preScroll;
                if (selection.tl.y < 0) {
                    selection.clear();
                }
            } else if (!(selection.br.y < 0 || selection.tl.y >= (int)(state.rows))) {
                selection.clear();
            }
        }
        visibleStart = preScroll;
        cursorState.position.y -= preScroll;
        if (trackedCursor != nullptr) {
            trackedCursor->position.y = max(0, trackedCursor->position.y - preScroll);
        }
    }

    viewOffset = min<size_t>(viewOffset, sourceHistory.length());

    // An interactive growth restores rows from history above the screen.
    Vector<const Row*> restored;
    if constexpr (primary) {
        if (nRows_ > state.rows) {
            const u16 restore = (u16)(min<size_t>(nRows_ - state.rows, sourceHistory.length()));
            restored.grow(restore);
            for (u16 k = 0; k < restore; ++k) {
                restored.pushBack(sourceHistory.popBack());
            }
            for (size_t left = 0, right = restored.length(); left < right && left < --right; ++left) {
                const Row* const value = restored[left];
                restored.mut(left) = restored[right];
                restored.mut(right) = value;
            }
            cursorState.position.y += restore;
            if (trackedCursor != nullptr) {
                trackedCursor->position.y = min<int>(trackedCursor->position.y + restore, nRows_ - 1);
            }
            if (!selection.null()) {
                selection.tl.y += restore;
                selection.br.y += restore;
            }
            viewOffset = viewOffset > restore ? viewOffset - restore : 0;
        }
    }

    const u32 historyCount = (u32)(sourceHistory.length());
    initializeRows(nCols_, nRows_, historyCount);

    u16 outRow = 0;
    for (const Row* row : restored) {
        installRow(outRow++, row->cells, state.columns, row->metadata.lineAttribute, row->metadata.protection, row->metadata.semanticPrompt);
    }
    for (size_t k = visibleStart; k < screenRows && outRow < nRows_; ++k) {
        const Row* const row = sourceScreen[k];
        installRow(outRow++, row->cells, state.columns, row->metadata.lineAttribute, row->metadata.protection, row->metadata.semanticPrompt);
    }
    for (u32 k = 0; k < historyCount; ++k) {
        const Row* const row = sourceHistory[k];
        installRow((int)(k) - (int)(historyCount), row->cells, state.columns, row->metadata.lineAttribute, row->metadata.protection, row->metadata.semanticPrompt);
    }
    if (!selectionValid()) {
        selection.clear();
    }
    if (trackedCursor != nullptr) {
        trackedCursor->position.x = min<int>(trackedCursor->position.x, nCols_ - 1);
        trackedCursor->position.y = min<int>(trackedCursor->position.y, nRows_ - 1);
    }
}

template <typename Traits>
void ScreenBase<Traits>::layoutReflow(ResizeState& state, u16 nCols_, u16 nRows_, Cursor& cursorState, Cursor* trackedCursor) {
    struct Anchor {
        int oldRow = 0;
        int oldColumn = 0;
        size_t offset = 0;
        Point mapped;
        bool inLine = false;
        bool done = false;
    };

    struct Segment {
        const TerminalCell* cells;
        int count;
    };

    const int oldHistoryCount = state.historyRows;
    const bool wasScrolled = viewOffset != 0;
    const int oldTotalRows = oldHistoryCount + state.rows;
    Anchor cursorAnchor{
        oldHistoryCount + cursorState.position.y,
        cursorState.position.x + (cursorState.pendingWrap ? 1 : 0),
    };
    Anchor trackedCursorAnchor;
    if (trackedCursor != nullptr) {
        trackedCursorAnchor.oldRow = oldHistoryCount + trackedCursor->position.y;
        trackedCursorAnchor.oldColumn = trackedCursor->position.x + (trackedCursor->pendingWrap ? 1 : 0);
    }
    Anchor viewAnchor{oldHistoryCount - (int)(viewOffset), 0};
    Anchor selectionStart;
    Anchor selectionEnd;
    const bool keepSelection = !selection.null() && !selection.rectangular;
    if (keepSelection) {
        selectionStart.oldRow = oldHistoryCount + selection.tl.y;
        selectionStart.oldColumn = selection.tl.x;
        selectionEnd.oldRow = oldHistoryCount + selection.br.y;
        selectionEnd.oldColumn = selection.br.x;
    }
    Anchor* anchors[5];
    size_t anchorCount = 0;
    anchors[anchorCount++] = &cursorAnchor;
    if (trackedCursor != nullptr) {
        anchors[anchorCount++] = &trackedCursorAnchor;
    }
    anchors[anchorCount++] = &viewAnchor;
    if (keepSelection) {
        anchors[anchorCount++] = &selectionStart;
        anchors[anchorCount++] = &selectionEnd;
    }

    const auto cellHasContent = [](const TerminalCell& source) {
        TerminalCell cell = source;
        cell.wrap = 0;
        return cell != TerminalCell{};
    };

    // The old content is walked twice with identical arithmetic and one
    // reused row buffer: the first pass measures the re-wrapped height and
    // maps the anchors, the second streams the surviving rows straight
    // into the fresh storage. Nothing materializes the whole scrollback.
    Vector<TerminalCell> rowBuffer;
    rowBuffer.zero(nCols_);
    Vector<Segment> segments;
    size_t lastMaterializedRow = 0;

    const auto walk = [&](bool mapAnchors, auto&& emitRow) -> size_t {
        size_t globalRow = 0;
        int oldRow = 0;
        while (oldRow < oldTotalRows) {
            // Gather one logical line: the chain of soft-wrapped rows.
            segments.clear();
            const Row* const firstRow = stateRowObject(state, oldRow - oldHistoryCount);
            const bool reflowable = firstRow->metadata.lineAttribute == 0;
            const u8 lineAttribute = firstRow->metadata.lineAttribute;
            ScreenSemanticPrompt prompt = firstRow->metadata.semanticPrompt;
            size_t lineLength = 0;
            for (size_t index = 0; index != anchorCount; ++index) {
                anchors[index]->inLine = false;
            }
            while (oldRow < oldTotalRows) {
                const Row* const sourceRow = stateRowObject(state, oldRow - oldHistoryCount);
                const bool normalWidth = sourceRow->metadata.lineAttribute == 0;
                if (!segments.empty() && !normalWidth) {
                    break;
                }
                if (prompt == ScreenSemanticPrompt::None) {
                    prompt = sourceRow->metadata.semanticPrompt;
                }
                const TerminalCell* const row = sourceRow->cells;
                int contentEnd = 0;
                int wrapEnd = 0;
                for (int column = 0; column < state.columns; ++column) {
                    if (cellHasContent(row[column])) {
                        contentEnd = column + 1;
                    }
                    if (row[column].wrap) {
                        wrapEnd = column + 1;
                    }
                }
                int copyEnd = wrapEnd ? wrapEnd : contentEnd;
                if (!normalWidth) {
                    copyEnd = state.columns;
                }
                for (size_t index = 0; index != anchorCount; ++index) {
                    Anchor& anchor = *anchors[index];
                    if (anchor.oldRow == oldRow) {
                        anchor.offset = lineLength + min(anchor.oldColumn, (int)(state.columns));
                        anchor.inLine = true;
                        copyEnd = max(copyEnd, min(anchor.oldColumn, (int)(state.columns)));
                    }
                }
                segments.pushBack({row, copyEnd});
                lineLength += copyEnd;
                ++oldRow;
                if (!(wrapEnd && normalWidth)) {
                    break;
                }
            }

            // Re-wrap the line into output rows through the shared buffer.
            const auto cellAt = [&](size_t offset) -> const TerminalCell& {
                for (const Segment& segment : segments) {
                    if (offset < (size_t)(segment.count)) {
                        return segment.cells[offset];
                    }
                    offset -= segment.count;
                }
                static const TerminalCell blank{};
                return blank;
            };
            memset(rowBuffer.mutData(), 0, (size_t)(nCols_) * sizeof(TerminalCell));
            int column = 0;
            size_t lineRows = 0;
            const auto flushRow = [&]() {
                const bool continuation = lineRows != 0 && prompt != ScreenSemanticPrompt::None;
                const ScreenSemanticPrompt rowPrompt = continuation ? ScreenSemanticPrompt::Continuation : prompt;
                if (mapAnchors) {
                    bool materialized = lineAttribute != 0 || rowPrompt != ScreenSemanticPrompt::None;
                    for (u16 index = 0; index < nCols_ && !materialized; ++index) {
                        materialized = cellHasContent(rowBuffer[index]);
                    }
                    if (materialized) {
                        lastMaterializedRow = globalRow + 1;
                    }
                }
                emitRow(globalRow, rowBuffer.data(), lineAttribute, rowPrompt);
                ++globalRow;
                ++lineRows;
                memset(rowBuffer.mutData(), 0, (size_t)(nCols_) * sizeof(TerminalCell));
                column = 0;
            };
            const auto mapAnchor = [&](size_t offset) {
                if (!mapAnchors) {
                    return;
                }
                for (size_t index = 0; index != anchorCount; ++index) {
                    Anchor& anchor = *anchors[index];
                    if (anchor.inLine && !anchor.done && anchor.offset == offset) {
                        anchor.mapped = Point(column, (int)(globalRow));
                        anchor.done = true;
                    }
                }
            };

            if (reflowable) {
                size_t offset = 0;
                while (offset < lineLength) {
                    const TerminalCell& lead = cellAt(offset);
                    const bool wide = lead.dwidth && offset + 1 < lineLength && cellAt(offset + 1).dwidth_cont;
                    const size_t width = wide ? 2 : 1;
                    if (width > nCols_) {
                        mapAnchor(offset);
                        offset += width;
                        mapAnchor(offset);
                        continue;
                    }
                    if (column + (int)(width) > nCols_) {
                        rowBuffer.mut(column ? column - 1 : nCols_ - 1).wrap = 1;
                        flushRow();
                    }
                    for (size_t cellIndex = 0; cellIndex < width; ++cellIndex) {
                        mapAnchor(offset + cellIndex);
                        TerminalCell cell = cellAt(offset + cellIndex);
                        cell.wrap = 0;
                        rowBuffer.mut(column++) = cell;
                    }
                    offset += width;
                    if (column == nCols_ && offset < lineLength) {
                        rowBuffer.mut(nCols_ - 1).wrap = 1;
                        flushRow();
                    }
                }
            } else {
                const size_t count = min<size_t>(lineLength, nCols_);
                for (size_t offset = 0; offset < count; ++offset) {
                    mapAnchor(offset);
                    TerminalCell cell = cellAt(offset);
                    cell.wrap = 0;
                    rowBuffer.mut(column++) = cell;
                }
                if (mapAnchors) {
                    for (size_t index = 0; index != anchorCount; ++index) {
                        Anchor& anchor = *anchors[index];
                        if (anchor.inLine && !anchor.done && anchor.offset > count) {
                            anchor.mapped = Point((int)(count), (int)(globalRow));
                            anchor.done = true;
                        }
                    }
                }
            }
            mapAnchor(lineLength);
            if (mapAnchors) {
                // Anchors that never matched a boundary land at line end.
                for (size_t index = 0; index != anchorCount; ++index) {
                    Anchor& anchor = *anchors[index];
                    if (anchor.inLine && !anchor.done) {
                        anchor.mapped = Point(column, (int)(globalRow));
                        anchor.done = true;
                    }
                }
            }
            flushRow();
        }
        return globalRow;
    };

    walk(true, [](size_t, const TerminalCell*, u8, ScreenSemanticPrompt) {});

    // A reflow may create more physical rows than fit on screen.  Keep the
    // materialized active area bottom-anchored: rows before it are scrollback,
    // while rows after a cursor near the top must not silently disappear.
    // Untouched blank tail rows are only viewport capacity, not history.
    size_t retainedRows = lastMaterializedRow;
    const auto retainAnchor = [&](const Anchor& anchor) {
        if (anchor.mapped.y >= 0) {
            retainedRows = max(retainedRows, (size_t)(anchor.mapped.y + 1));
        }
    };
    retainAnchor(cursorAnchor);
    if (trackedCursor != nullptr) {
        retainAnchor(trackedCursorAnchor);
    }
    if (keepSelection) {
        retainAnchor(selectionStart);
        retainAnchor(selectionEnd);
    }
    const size_t screenStart = retainedRows > nRows_ ? retainedRows - nRows_ : 0;
    const size_t retainedStart = screenStart > saveLines ? screenStart - saveLines : 0;
    const size_t historyCount = screenStart - retainedStart;

    initializeRows(nCols_, nRows_, historyCount);
    walk(false, [&](size_t globalRow, const TerminalCell* cells, u8 lineAttribute, ScreenSemanticPrompt prompt) {
        if (globalRow < retainedStart || globalRow >= screenStart + nRows_) {
            return;
        }
        const int destination = globalRow < screenStart ? (int)(globalRow) - (int)(screenStart) : (int)(globalRow - screenStart);
        installRow(destination, cells, nCols_, lineAttribute, 0, prompt);
    });

    if (!wasScrolled) {
        viewOffset = 0;
    } else if (viewAnchor.mapped.y >= (int)(retainedStart) && viewAnchor.mapped.y < (int)(screenStart)) {
        viewOffset = screenStart - viewAnchor.mapped.y;
    } else if (viewAnchor.mapped.y < (int)(retainedStart)) {
        viewOffset = historyCount;
    } else {
        viewOffset = 0;
    }
    if (keepSelection && selectionStart.mapped.y >= (int)(retainedStart) && selectionStart.mapped.y < (int)(screenStart + nRows_) && selectionEnd.mapped.y >= (int)(retainedStart) && selectionEnd.mapped.y < (int)(screenStart + nRows_)) {
        selection.tl = Point(selectionStart.mapped.x, selectionStart.mapped.y - screenStart);
        selection.br = Point(selectionEnd.mapped.x, selectionEnd.mapped.y - screenStart);
    } else {
        selection.clear();
    }

    if (cursorAnchor.mapped.y < (int)(screenStart)) {
        cursorState.position = Point(0, 0);
        cursorState.pendingWrap = false;
    } else {
        cursorState.pendingWrap = cursorAnchor.mapped.x == nCols_;
        cursorState.position.x = cursorState.pendingWrap ? nCols_ - 1 : min(cursorAnchor.mapped.x, (int)(nCols_ - 1));
        cursorState.position.y = max(0, min(cursorAnchor.mapped.y - (int)(screenStart), (int)(nRows_ - 1)));
    }
    if (trackedCursor != nullptr) {
        if (trackedCursorAnchor.mapped.y < (int)(screenStart)) {
            trackedCursor->position = Point(0, 0);
            trackedCursor->pendingWrap = false;
        } else {
            trackedCursor->pendingWrap = trackedCursorAnchor.mapped.x == nCols_;
            trackedCursor->position.x = trackedCursor->pendingWrap ? nCols_ - 1 : min(trackedCursorAnchor.mapped.x, (int)(nCols_ - 1));
            trackedCursor->position.y = max(0, min(trackedCursorAnchor.mapped.y - (int)(screenStart), (int)(nRows_ - 1)));
        }
    }
}

template <typename Traits>
size_t ScreenBase<Traits>::copyDamage(TerminalRow* rows) const {
    size_t count = 0;
    for (u16 row = 0; row < damage.height; ++row) {
        if (damage.rows[row] == 0) {
            continue;
        }
        const Row* const source = getViewRowObject(row);
        rows[count++] = {
            .cells = source->cells,
            .row = row,
            .lineAttribute = source->metadata.lineAttribute,
        };
    }
    return count;
}

template <typename Traits>
ScreenFrame ScreenBase<Traits>::captureFrame(TerminalRow* rows) const {
    return {
        .damagedRows = copyDamage(rows),
        .selection = selectionForView(),
        .snappedSelection = snappedSelection(),
        .historyRows = historyRows,
        .viewOffset = viewOffset,
    };
}

template <typename Traits>
Rect ScreenBase<Traits>::selectionForView() const {
    if (!selectionValid()) {
        return {};
    }

    Rect ret = selection;
    if (!ret.null()) {
        ret.tl.y += viewOffset;
        ret.br.y += viewOffset;
    }
    return ret;
}

template <typename Traits>
Rect ScreenBase<Traits>::snappedSelection() const {
    const bool sameKey = snappedCacheValid && snappedCacheRevision == contentRevision && snappedCacheViewOffset == (i32)(viewOffset) && snappedCacheSnapTo == snapTo && snappedCacheSelection.tl == selection.tl && snappedCacheSelection.br == selection.br && snappedCacheSelection.rectangular == selection.rectangular;
    if (sameKey) {
        return snappedCache;
    }
    snappedCache = computeSnappedSelection();
    snappedCacheSelection = selection;
    snappedCacheSnapTo = snapTo;
    snappedCacheRevision = contentRevision;
    snappedCacheViewOffset = (i32)(viewOffset);
    snappedCacheValid = true;
    return snappedCache;
}

template <typename Traits>
Rect ScreenBase<Traits>::computeSnappedSelection() const {
    Rect ret = selection;

    if (ret.null()) {
        return ret;
    }
    if (!selectionValid()) {
        return {};
    }

    if (selection.rectangular) {
        ret.tl.y += viewOffset;
        ret.br.y += viewOffset;
        return ret;
    }

    const auto wrapLength = [this](int logicalRow) {
        const TerminalCell* cells = getLogicalRowPtr(logicalRow);
        for (int x = 0; x < nCols; ++x) {
            if (cells[x].wrap) {
                return x + 1;
            }
        }
        return 0;
    };

    switch (snapTo) {
        case SelectSnapTo::Char:
            break;
        case SelectSnapTo::Word: {
            const auto expand = [this, &wrapLength](int rowIndex, int column) {
                const int currentWrapLength = wrapLength(rowIndex);
                if (currentWrapLength != 0 && column >= currentWrapLength) {
                    const SelectionRow row{getLogicalRowPtr(rowIndex), (int)(nCols)};
                    const TokenBounds word = wordTokenBounds(row, column);
                    return Rect(word.left, rowIndex, word.right, rowIndex);
                }

                int firstRow = rowIndex;
                while (firstRow > -(int)(historyRows) && wrapLength(firstRow - 1) != 0) {
                    --firstRow;
                }
                int lastRow = rowIndex;
                while (lastRow + 1 < nRows && wrapLength(lastRow) != 0) {
                    ++lastRow;
                }

                selectionScratch.clear();
                int clicked = -1;
                for (int logicalRow = firstRow; logicalRow <= lastRow; ++logicalRow) {
                    const int wrapped = wrapLength(logicalRow);
                    const int length = wrapped != 0 ? wrapped : (int)(nCols);
                    const int offset = (int)(selectionScratch.length());
                    const TerminalCell* source = getLogicalRowPtr(logicalRow);
                    selectionScratch.append(source, length);
                    if (logicalRow == rowIndex) {
                        clicked = offset + column;
                    }
                }

                const SelectionRow logicalLine{selectionScratch.data(), (int)(selectionScratch.length())};
                const TokenBounds semantic = semanticTokenBounds(logicalLine, clicked);
                if (semantic.left == semantic.right) {
                    const SelectionRow row{getLogicalRowPtr(rowIndex), (int)(nCols)};
                    const TokenBounds word = wordTokenBounds(row, column);
                    return Rect(word.left, rowIndex, word.right, rowIndex);
                }

                Point left;
                Point right;
                int offset = 0;
                for (int logicalRow = firstRow; logicalRow <= lastRow; ++logicalRow) {
                    const int wrapped = wrapLength(logicalRow);
                    const int length = wrapped != 0 ? wrapped : (int)(nCols);
                    const int end = offset + length;
                    if (semantic.left >= offset && semantic.left < end) {
                        left = Point(semantic.left - offset, logicalRow);
                    }
                    if (semantic.right >= offset && semantic.right <= end) {
                        right = Point(semantic.right - offset, logicalRow);
                        break;
                    }
                    offset = end;
                }
                return Rect(left, right);
            };

            const Rect start = expand(ret.tl.y, ret.tl.x);
            const Rect end = expand(ret.br.y, ret.br.x);
            ret.tl = start.tl;
            ret.br = end.br;
        } break;
        case SelectSnapTo::Line:
            while (ret.tl.y > -(int)(historyRows) && wrapLength(ret.tl.y - 1) != 0) {
                --ret.tl.y;
            }
            while (ret.br.y + 1 < nRows && wrapLength(ret.br.y) != 0) {
                ++ret.br.y;
            }
            ret.tl.x = 0;
            ret.br.x = nCols;
            break;
        default:
            break;
    }

    if (!ret.rectangular) {
        if (ret.tl.x < nCols && getLogicalRowPtr(ret.tl.y)[ret.tl.x].dwidth_cont) {
            --ret.tl.x;
        }
        if (ret.br.x < nCols && getLogicalRowPtr(ret.br.y)[ret.br.x].dwidth_cont) {
            ++ret.br.x;
        }
    }

    ret.tl.y += viewOffset;
    ret.br.y += viewOffset;
    return ret;
}

template <typename Traits>
bool ScreenBase<Traits>::selectedText(Buffer& utf8_selection) const {
    Rect sel = snappedSelection();

    if (sel.empty()) {
        return false;
    }
    sel.tl.y -= viewOffset;
    sel.br.y -= viewOffset;

    utf8_selection.reset();
    CellExtraStore& extras = cellExtras();
    Vector<u32> line;
    bool wrap = false;
    bool first = true;

    auto sinkFn = [&](char ch) {
        utf8_selection.append(&ch, 1);
    };
    auto addLine = [&](int y, u16 x1, u16 x2) {
        line.clear();
        size_t contentEnd = 0;
        const bool wrapBack = wrap;
        wrap = false;
        const auto* cp = getLogicalRowPtr(y);
        for (u16 x = x1; x < x2; ++x) {
            const auto& cell = cp[x];
            if (!cell.dwidth_cont) {
                const auto grapheme = extras.grapheme(cell);
                if (grapheme.empty()) {
                    line.pushBack(cell.uc_pt ? cell.uc_pt : ' ');
                } else {
                    line.append(grapheme.begin(), grapheme.size());
                }
                if (cell.drawn || (cell.uc_pt != 0 && cell.uc_pt != ' ') || !grapheme.empty()) {
                    contentEnd = line.length();
                }
            }
            if (cell.wrap) {
                wrap = true;
                break;
            }
        }

        // Empty cells past the written end of a hard line are screen padding,
        // not selected text.  contentEnd includes drawn spaces, so explicitly
        // written whitespace survives even when the pointer ends in padding.
        const size_t count = wrap ? line.length() : contentEnd;
        if (!wrapBack && !first) {
            sinkFn('\n');
        }
        for (size_t index = 0; index < count; ++index) {
            Utf8Encoder::pushUnicode(line[index], sinkFn);
        }
        first = false;
    };

    if (sel.tl.y == sel.br.y) {
        addLine(sel.tl.y, sel.tl.x, sel.br.x);
    } else if (sel.rectangular) {
        for (int y = sel.tl.y; y <= sel.br.y; ++y) {
            addLine(y, sel.tl.x, sel.br.x);
        }
    } else {
        addLine(sel.tl.y, sel.tl.x, nCols);
        for (int y = sel.tl.y + 1; y < sel.br.y; ++y) {
            addLine(y, 0, nCols);
        }
        addLine(sel.br.y, 0, sel.br.x);
    }

    while (!utf8_selection.empty() && ((const char*)(utf8_selection.data()))[utf8_selection.used() - 1] == '\n') {
        utf8_selection.seekNegative(1);
    }

    return true;
}

template <typename Traits>
Rect ScreenBase<Traits>::logicalSelection() const {
    Rect selected = snappedSelection();
    if (selected.empty()) {
        return {};
    }
    selected.tl.y -= viewOffset;
    selected.br.y -= viewOffset;
    return selected;
}

template <typename Traits>
Point ScreenBase<Traits>::logicalPoint(Point point) const {
    point.y -= viewOffset;
    return point;
}

template <typename Traits>
void ScreenBase<Traits>::pageUpImpl(u16 count) {
    u32 viewOffset_ = min<u32>(viewOffset + count, historyRows);
    viewOffset = viewOffset_;
    expose();
}

template <typename Traits>
void ScreenBase<Traits>::pageDownImpl(u16 count) {
    viewOffset = viewOffset > count ? viewOffset - count : 0;
    expose();
}

template <typename Traits>
bool ScreenBase<Traits>::pageToBottomImpl() {
    if (!viewOffset) {
        return false;
    }

    viewOffset = 0;
    expose();
    return true;
}

template <typename Traits>
void ScreenBase<Traits>::scrollUpWithHistory(u16 top, u16 bottom, u16 count, const TerminalCell& attrs) {
    count = min<u16>(count, bottom - top);
    if (count == 0) {
        return;
    }
    const bool capture = top == 0 && saveLines != 0;
    const bool logicalScroll = top == 0;
    const u32 previousViewOffset = viewOffset;
    if (!logicalScroll) {
        vscrollSelection(top, bottom, -count, false);
    }

    if (capture) {
        for (u16 k = 0; k < count; ++k) {
            RowSlot incoming = 0;
            if (historyRows == saveLines) {
                RowSlot& oldest = logicalRowSlot(-(int)(historyRows));
                incoming = oldest;
                oldest = 0;
            } else {
                ++historyRows;
            }
            rowEnd = (rowEnd + 1) & (rowCapacity - 1);
            RowSlot& last = logicalRowSlot(nRows - 1);
            STD_ASSERT(last == 0);
            last = incoming;
            for (u16 row = nRows - 1; row >= bottom; --row) {
                logicalRowSlot(row) = logicalRowSlot(row - 1);
            }
            logicalRowSlot(bottom - 1) = incoming;
        }
    } else {
        rotateRowPointersUp(top, bottom, count);
    }
    if (logicalScroll) {
        vscrollSelection(top, bottom, -count, true);
    }
    clearRows(bottom - count, bottom, attrs);

    if (capture && viewOffset) {
        viewOffset = min<u32>(viewOffset + count, historyRows);
    }
    if (capture && previousViewOffset) {
        expose();
    } else {
        damageRows(top, bottom);
    }
}

template <typename Traits>
void ScreenBase<Traits>::scrollDownVisible(u16 top, u16 bottom, u16 count, const TerminalCell& attrs) {
    count = min<u16>(count, bottom - top);
    if (count == 0) {
        return;
    }
    vscrollSelection(top, bottom, count, false);
    rotateRowPointersDown(top, bottom, count);
    clearRows(top, top + count, attrs);
    damageRows(top, bottom);
}

template <typename Traits>
void ScreenBase<Traits>::scrollUpVisible(u16 top, u16 bottom, u16 count, const TerminalCell& attrs) {
    count = min<u16>(count, bottom - top);
    if (count == 0) {
        return;
    }
    vscrollSelection(top, bottom, -count, false);
    rotateRowPointersUp(top, bottom, count);
    clearRows(bottom - count, bottom, attrs);
    damageRows(top, bottom);
}

template <typename Traits>
void PrimaryScreenImpl<Traits>::dropHistory() {
    this->dropScrollbackHistoryImpl();
}

template <typename Traits>
void PrimaryScreenImpl<Traits>::scrollRows(u16 top, u16 bottom, i32 rows, const TerminalCell& attrs) {
    if (rows < 0) {
        this->scrollUpWithHistory(top, bottom, min<u32>(-(i64)(rows), 0xffff), attrs);
    } else if (rows > 0) {
        this->scrollDownVisible(top, bottom, min<u32>(rows, 0xffff), attrs);
    }
}

template <typename Traits>
bool PrimaryScreenImpl<Traits>::scrollView(i32 rows) {
    const u32 previous = this->viewOffset;
    if (rows > 0) {
        this->pageUpImpl(min<u32>(rows, 0xffff));
    } else if (rows < 0) {
        this->pageDownImpl(min<u32>(-(i64)(rows), 0xffff));
    }
    return this->viewOffset != previous;
}

template <typename Traits>
void AlternateScreenImpl<Traits>::dropHistory() {
}

template <typename Traits>
bool AlternateScreenImpl<Traits>::scrollView(i32) {
    return false;
}

template <typename Traits>
void AlternateScreenImpl<Traits>::scrollRows(u16 top, u16 bottom, i32 rows, const TerminalCell& attrs) {
    if (rows < 0) {
        this->scrollUpVisible(top, bottom, min<u32>(-(i64)(rows), 0xffff), attrs);
    } else if (rows > 0) {
        this->scrollDownVisible(top, bottom, min<u32>(rows, 0xffff), attrs);
    }
}

template <typename Traits>
void ScreenBase<Traits>::fillCells(u16 ch, const TerminalCell& attrs) {
    TerminalCell fill = attrs;
    fill.uc_pt = ch == ' ' ? 0 : ch;
    fill.drawn = ch != ' ';
    for (u16 r = 0; r < nRows; ++r) {
        RowSlot& slot = logicalRowSlot(r);
        if (fill == TerminalCell{}) {
            releaseRow(slot);
            slot = 0;
            continue;
        }
        TerminalCell* const row = mutableLogicalRow(r);
        for (u16 column = 0; column < nCols; ++column) {
            row[column] = fill;
        }
        slot->metadata.lineAttribute = 0;
        slot->metadata.protection = fill.protected_char;
        slot->metadata.semanticPrompt = ScreenSemanticPrompt::None;
        slot->metadata.wide = fill.dwidth || fill.dwidth_cont;
    }
    if (!selection.empty()) {
        invalidateSelection(Rect(0, 0, nCols, nRows));
    }
    damageRows(0, nRows);
}

template <typename Traits>
void ScreenBase<Traits>::setLineAttribute(u16 row, u8 attribute) {
    RowSlot& slot = logicalRowSlot(row);
    if (slot == nullptr) {
        if (attribute == 0) {
            return;
        }
        mutableRow(slot);
    }
    if (slot->metadata.lineAttribute == attribute) {
        return;
    }
    slot->metadata.lineAttribute = attribute;
    damageRow(row);
    if (!selection.empty()) {
        invalidateSelection(Rect(0, row, nCols, row));
    }
}

template <typename Traits>
u8 ScreenBase<Traits>::lineAttribute(u16 row) const noexcept {
    return getLogicalRowObject(row)->metadata.lineAttribute;
}

template <typename Traits>
void ScreenBase<Traits>::setSemanticPrompt(u16 row, ScreenSemanticPrompt prompt) {
    RowSlot& slot = logicalRowSlot(row);
    if (slot == nullptr) {
        if (prompt == ScreenSemanticPrompt::None) {
            return;
        }
        mutableRow(slot);
    }
    slot->metadata.semanticPrompt = prompt;
    if (prompt == ScreenSemanticPrompt::None && slot->metadata.lineAttribute == 0 && emptyRow(slot->cells, nCols)) {
        releaseRow(slot);
        slot = nullptr;
    }
}

template <typename Traits>
ScreenSemanticPrompt ScreenBase<Traits>::semanticPrompt(i32 row) const noexcept {
    return getLogicalRowObject(row)->metadata.semanticPrompt;
}

template <typename Traits>
bool ScreenBase<Traits>::hasProtection(u16 row, u8 mask) const noexcept {
    return (getLogicalRowObject(row)->metadata.protection & mask) != 0;
}

template <typename Traits>
bool ScreenBase<Traits>::wrapped(u16 row, u16 column) const noexcept {
    return getLogicalRowPtr(row)[column].wrap;
}

template <typename Traits>
void ScreenBase<Traits>::setWrapped(u16 row, u16 column) {
    TerminalCell* cells_ = mutableLogicalRow(row);
    cells_[column].wrap = 1;
    damageRow(row);
    if (!selection.empty()) {
        invalidateSelection(Rect(column, row));
    }
}

template <typename Traits>
void ScreenBase<Traits>::moveWrap(u16 row, u16 sourceColumn, u16 destinationColumn) {
    const TerminalCell* cells_ = getLogicalRowPtr(row);
    if (!cells_[sourceColumn].wrap || sourceColumn == destinationColumn) {
        return;
    }
    TerminalCell* mutableCells = mutableLogicalRow(row);
    mutableCells[sourceColumn].wrap = 0;
    mutableCells[destinationColumn].wrap = 1;
    damageRow(row);
    if (!selection.empty()) {
        const u16 begin = sourceColumn < destinationColumn ? sourceColumn : destinationColumn;
        const u16 end = sourceColumn > destinationColumn ? sourceColumn + 1 : destinationColumn + 1;
        invalidateSelection(Rect(begin, row, end, row));
    }
}

template <typename Traits>
TerminalCell* ScreenBase<Traits>::prepareSpan(u16 row, u16 start, u16 count, const TerminalCell& eraseAttrs) {
    RowSlot& slot = logicalRowSlot(row);
    return prepareSpan(slot, row, start, count, eraseAttrs);
}

template <typename Traits>
TerminalCell* ScreenBase<Traits>::prepareSpan(RowSlot& slot, u16 row, u16 start, u16 count, const TerminalCell& eraseAttrs) {
    if (slot == nullptr || !slot->metadata.wide) {
        damageRow(row);
        if (!selection.empty()) {
            invalidateSelection(Rect(start, row, start + count, row));
        }
        return mutableRow(slot) + start;
    }
    const u16 end = start + count;
    const TerminalCell* cells_ = slot->cells;
    const bool splitLeft = start > 0 && (cells_[start - 1].dwidth || cells_[start].dwidth_cont);
    const bool splitRight = end < nCols && (cells_[end - 1].dwidth || cells_[end].dwidth_cont);
    if (!splitLeft && !splitRight) {
        damageRow(row);
        if (!selection.empty()) {
            invalidateSelection(Rect(start, row, end, row));
        }
        slot->id = nextRowIdentity();
        return slot->cells + start;
    }
    return overwriteWideSpan(row, start, count, eraseAttrs);
}

template <typename Traits>
TerminalCell& ScreenBase<Traits>::prepareCell(u16 row, u16 column, const TerminalCell& eraseAttrs) {
    RowSlot& slot = logicalRowSlot(row);
    return prepareCell(slot, row, column, eraseAttrs);
}

template <typename Traits>
TerminalCell& ScreenBase<Traits>::prepareCell(RowSlot& slot, u16 row, u16 column, const TerminalCell& eraseAttrs) {
    if (slot == nullptr || !slot->metadata.wide) {
        damageRow(row);
        if (!selection.empty()) {
            invalidateSelection(Rect(column, row));
        }
        return mutableRow(slot)[column];
    }
    const TerminalCell* cells_ = slot->cells;
    if (cells_[column].dwidth_cont) {
        clearWideBoundary(slot, row, column, eraseAttrs);
    } else if (cells_[column].dwidth) {
        clearWideBoundary(slot, row, column + 1, eraseAttrs);
    }
    damageRow(row);
    if (!selection.empty()) {
        invalidateSelection(Rect(column, row));
    }
    return mutableRow(slot)[column];
}

template <typename Traits>
[[gnu::always_inline]] inline void ScreenBase<Traits>::writePreparedCell(u16 row, u16 column, const TerminalCell& lead, bool wide, const TerminalCell& attrs, const TerminalCell& eraseAttrs) {
    if (!wide) {
        RowSlot& slot = logicalRowSlot(row);
        const TerminalCell* const previous = rowData(slot);
        if (previous == nullptr || (!previous[column].dwidth && !previous[column].dwidth_cont)) {
            damageRow(row);
            if (!selection.empty()) {
                invalidateSelection(Rect(column, row));
            }
            TerminalCell* const cells = mutableRow(slot);
            cells[column] = lead;
            slot->metadata.protection |= lead.protected_char;
        } else {
            prepareCell(slot, row, column, eraseAttrs) = lead;
            slot->metadata.protection |= lead.protected_char;
        }
        return;
    }

    TerminalCell continuation = attrs;
    continuation.dwidth_cont = 1;
    continuation.drawn = 1;
    continuation.semantic = lead.semantic;
    if (lead.extraRef() != 0 || continuation.hasExtra()) {
        cellExtras().setHyperlink(continuation, lead.extraRef());
    }
    RowSlot& slot = logicalRowSlot(row);
    TerminalCell* const cells = prepareSpan(slot, row, column, 2, eraseAttrs);
    cells[0] = lead;
    cells[1] = continuation;
    slot->metadata.wide = true;
    slot->metadata.protection |= lead.protected_char | continuation.protected_char;
}

template <typename Traits>
void ScreenBase<Traits>::writeGrapheme(u16 row, u16 column, const u32* codepoints, size_t count, bool wide, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    STD_ASSERT(count != 0);
    TerminalCell lead = attrs;
    lead.uc_pt = codepoints[0];
    lead.drawn = 1;
    lead.dwidth = wide;
    lead.semantic = semantic;
    if (count == 1) {
        if (hyperlink != 0 || lead.hasExtra()) {
            cellExtras().setHyperlink(lead, hyperlink);
        }
    } else {
        CellExtraStore& extras = cellExtras();
        if (hyperlink != 0 || lead.hasExtra()) {
            extras.setHyperlink(lead, hyperlink);
        }
        extras.setGrapheme(lead, codepoints, count);
    }
    writePreparedCell(row, column, lead, wide, attrs, eraseAttrs);
}

template <typename Traits>
void ScreenBase<Traits>::writeSixelCells(u16 row, u16 column, u16 count, const u8* patches, const u8* palette, const TerminalCell& attrs, u32 hyperlink, const TerminalCell& eraseAttrs) {
    CellExtraStore& extras = cellExtras();
    for (u16 index = 0; index < count; ++index) {
        const u8* patch = patches + (size_t)(index)*SixelPatch::pixelCount;
        u8 painted = 0;
        for (size_t pixel = 0; pixel < SixelPatch::pixelCount; ++pixel) {
            painted |= patch[pixel];
        }
        TerminalCell cell = attrs;
        cell.uc_pt = 0;
        cell.drawn = 1;
        if (hyperlink != 0 || cell.hasExtra()) {
            extras.setHyperlink(cell, hyperlink);
        }
        if (painted != 0) {
            extras.setSixel(cell, patch, palette);
        }
        writePreparedCell(row, column + index, cell, false, attrs, eraseAttrs);
    }
}

template <typename Traits>
auto ScreenBase<Traits>::writeAsciiRun(u16 row, u16 column, u16 normalEnd, u16 doubleEnd, const u8* input, u16 count, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) -> WriteResult {
    RowSlot& slot = logicalRowSlot(row);
    const u16 end = slot != nullptr && slot->metadata.lineAttribute != 0 ? doubleEnd : normalEnd;
    if (column >= end) {
        return {0, end};
    }
    count = min<u16>(count, end - column);
    TerminalCell linkedAttrs = attrs;
    if (hyperlink != 0 || linkedAttrs.hasExtra()) {
        cellExtras().setHyperlink(linkedAttrs, hyperlink);
    }
    linkedAttrs.uc_pt = 0;
    linkedAttrs.drawn = 1;
    linkedAttrs.semantic = semantic;
    TerminalCell* const cells = prepareSpan(slot, row, column, count, eraseAttrs);
    u64 content;
    memcpy(&content, &linkedAttrs.content, sizeof(content));
    storeAsciiCells(reinterpret_cast<u64*>(cells), input, count, linkedAttrs.style, content);
    slot->metadata.protection |= linkedAttrs.protected_char;
    return {count, end};
}

template <typename Traits>
template <bool captureHistory>
void ScreenBase<Traits>::writeAsciiLinesImpl(u16 row, const u8* input, const u16* lengths, u16 lineCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    if (!selection.null() || viewOffset != 0) {
        for (u16 line = 0; line < lineCount; ++line) {
            const u16 count = lengths[line];
            STD_ASSERT(input[count] == '\r' && input[count + 1] == '\n');
            if (count != 0) {
                writeAsciiRun(row, 0, nCols, nCols, input, count, attrs, hyperlink, semantic, eraseAttrs);
            }
            input += count + 2;
            if (row + 1 < nRows) {
                ++row;
            } else {
                scrollRows(0, nRows, -1, eraseAttrs);
            }
            if (semantic == 1 || semantic == 2) {
                setSemanticPrompt(row, ScreenSemanticPrompt::Continuation);
            }
        }
        return;
    }

    TerminalCell linkedAttrs = attrs;
    if (hyperlink != 0 || linkedAttrs.hasExtra()) {
        cellExtras().setHyperlink(linkedAttrs, hyperlink);
    }
    linkedAttrs.uc_pt = 0;
    linkedAttrs.drawn = 1;
    linkedAttrs.semantic = semantic;
    u64 style;
    u64 content;
    memcpy(&style, &linkedAttrs.style, sizeof(style));
    memcpy(&content, &linkedAttrs.content, sizeof(content));
    const bool eraseZero = eraseAttrs == TerminalCell{};
    const bool semanticContinuation = semantic == 1 || semantic == 2;
    u64 eraseStyle;
    u64 eraseContent;
    memcpy(&eraseStyle, &eraseAttrs.style, sizeof(eraseStyle));
    memcpy(&eraseContent, &eraseAttrs.content, sizeof(eraseContent));

    const auto storeText = [style, content](TerminalCell* cells, const u8* text, u16 count) {
        storeAsciiCells(reinterpret_cast<u64*>(cells), text, count, style, content);
    };
    const auto overwriteClearedRow = [&](RowSlot& slot, const u8* text, u16 count) {
        // An empty row inside a prompt continuation still carries the
        // semantic mark, so it cannot take the released-row shortcut.
        if (count == 0 && eraseZero && !semanticContinuation) {
            releaseRow(slot);
            slot = nullptr;
            return;
        }
        TerminalCell* const cells = mutableRow(slot);
        storeText(cells, text, count);
        if (eraseZero) {
            memset(cells + count, 0, (size_t)(nCols - count) * cellSize);
        } else {
            auto* output = reinterpret_cast<u64*>(cells);
            for (u16 index = count; index < nCols; ++index) {
                output[2 * index] = eraseStyle;
                output[2 * index + 1] = eraseContent;
            }
        }
        slot->metadata.lineAttribute = 0;
        slot->metadata.protection = eraseAttrs.protected_char | (count != 0 ? linkedAttrs.protected_char : 0);
        slot->metadata.semanticPrompt = semanticContinuation ? ScreenSemanticPrompt::Continuation : ScreenSemanticPrompt::None;
        slot->metadata.wide = eraseAttrs.dwidth || eraseAttrs.dwidth_cont;
    };
    const auto advanceRing = [&]() {
        RowSlot incoming = nullptr;
        if constexpr (captureHistory) {
            if (saveLines != 0) {
                if (historyRows == saveLines) {
                    RowSlot& oldest = logicalRowSlot(-(int)(historyRows));
                    incoming = oldest;
                    oldest = nullptr;
                } else {
                    ++historyRows;
                }
            } else {
                RowSlot& first = logicalRowSlot(0);
                incoming = first;
                first = nullptr;
            }
        } else {
            RowSlot& first = logicalRowSlot(0);
            incoming = first;
            first = nullptr;
        }
        rowEnd = (rowEnd + 1) & (rowCapacity - 1);
        RowSlot& last = logicalRowSlot(nRows - 1);
        STD_ASSERT(last == nullptr);
        last = incoming;
    };

    const bool scrolls = lineCount > nRows - row - 1;
    bool cleared = false;
    for (u16 line = 0; line < lineCount; ++line) {
        const u16 count = lengths[line];
        STD_ASSERT(input[count] == '\r' && input[count + 1] == '\n');
        RowSlot& slot = logicalRowSlot(row);
        if (cleared) {
            overwriteClearedRow(slot, input, count);
        } else if (count != 0) {
            TerminalCell* cells;
            if (slot == nullptr || !slot->metadata.wide) {
                cells = mutableRow(slot);
            } else {
                cells = overwriteWideSpan(row, 0, count, eraseAttrs);
            }
            storeText(cells, input, count);
            slot->metadata.protection |= linkedAttrs.protected_char;
            if (!scrolls) {
                damageRow(row);
            }
        }
        input += count + 2;
        if (row + 1 < nRows) {
            ++row;
            cleared = false;
        } else {
            advanceRing();
            cleared = true;
        }
        if (semanticContinuation && !cleared) {
            setSemanticPrompt(row, ScreenSemanticPrompt::Continuation);
        }
    }
    if (cleared) {
        overwriteClearedRow(logicalRowSlot(nRows - 1), nullptr, 0);
    }
    if (scrolls) {
        expose();
    }
}

template <typename Traits>
void PrimaryScreenImpl<Traits>::writeAsciiLines(u16 row, const u8* input, const u16* lengths, u16 lineCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    this->template writeAsciiLinesImpl<true>(row, input, lengths, lineCount, attrs, hyperlink, semantic, eraseAttrs);
}

template <typename Traits>
void AlternateScreenImpl<Traits>::writeAsciiLines(u16 row, const u8* input, const u16* lengths, u16 lineCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    this->template writeAsciiLinesImpl<false>(row, input, lengths, lineCount, attrs, hyperlink, semantic, eraseAttrs);
}

template <typename Traits>
auto ScreenBase<Traits>::writeAsciiRunInsert(u16 row, u16 column, u16 normalEnd, u16 doubleEnd, const u8* input, u16 count, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) -> WriteResult {
    RowSlot& slot = logicalRowSlot(row);
    const u16 end = slot != nullptr && slot->metadata.lineAttribute != 0 ? doubleEnd : normalEnd;
    if (column >= end) {
        return {0, end};
    }
    count = min<u16>(count, end - column);
    if (count == 0) {
        return {0, end};
    }
    if (slot != nullptr && slot->metadata.wide) {
        insertCells(row, column, end, count, eraseAttrs);
        return writeAsciiRun(row, column, end, end, input, count, attrs, hyperlink, semantic, eraseAttrs);
    }
    TerminalCell linkedAttrs = attrs;
    if (hyperlink != 0 || linkedAttrs.hasExtra()) {
        cellExtras().setHyperlink(linkedAttrs, hyperlink);
    }
    linkedAttrs.uc_pt = 0;
    linkedAttrs.drawn = 1;
    linkedAttrs.semantic = semantic;
    const u16 moved = end - column - count;
    TerminalCell* cells = rowData(slot);
    if (moved != 0 && cells != nullptr) {
        if (cells[end - 1].wrap) {
            cells[end - 1].wrap = 0;
            cells[end - count - 1].wrap = 1;
        }
        moveCells(cells + column + count, cells + column, moved);
    }
    cells = mutableRow(slot);
    u64 content;
    memcpy(&content, &linkedAttrs.content, sizeof(content));
    auto* const output = reinterpret_cast<u64*>(cells + column);
    storeAsciiCells(output, input, count, linkedAttrs.style, content);
    slot->metadata.protection |= linkedAttrs.protected_char;
    damageRow(row);
    if (!selection.empty()) {
        invalidateSelection(Rect(column, row, end, row));
    }
    return {count, end};
}

template <typename Traits>
void ScreenBase<Traits>::writeRun(u16 row, u16 column, const u32* codepoints, u16 count, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    TerminalCell linkedAttrs = attrs;
    if (hyperlink != 0 || linkedAttrs.hasExtra()) {
        cellExtras().setHyperlink(linkedAttrs, hyperlink);
    }
    RowSlot& slot = logicalRowSlot(row);
    TerminalCell* cells_ = prepareSpan(slot, row, column, count, eraseAttrs);
    for (u16 index = 0; index < count; ++index) {
        TerminalCell& cell = cells_[index];
        cell = linkedAttrs;
        cell.uc_pt = codepoints[index];
        cell.drawn = 1;
        cell.semantic = semantic;
    }
    slot->metadata.protection |= linkedAttrs.protected_char;
}

template <typename Traits>
void ScreenBase<Traits>::writeRepeatedCodepoint(u16 row, u16 column, u16 count, u32 codepoint, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    TerminalCell cell = attrs;
    if (hyperlink != 0 || cell.hasExtra()) {
        cellExtras().setHyperlink(cell, hyperlink);
    }
    cell.uc_pt = codepoint;
    cell.drawn = 1;
    cell.semantic = semantic;
    RowSlot& slot = logicalRowSlot(row);
    TerminalCell* const cells = prepareSpan(slot, row, column, count, eraseAttrs);
    for (u16 index = 0; index < count; ++index) {
        cells[index] = cell;
    }
    slot->metadata.protection |= cell.protected_char;
}

template <typename Traits>
void ScreenBase<Traits>::writeGlyphRun(u16 row, u16 column, const u32* codepoints, const u8* widths, u16 glyphCount, u16 cellCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    TerminalCell linkedAttrs = attrs;
    if (hyperlink != 0 || linkedAttrs.hasExtra()) {
        cellExtras().setHyperlink(linkedAttrs, hyperlink);
    }
    RowSlot& slot = logicalRowSlot(row);
    TerminalCell* const cells = prepareSpan(slot, row, column, cellCount, eraseAttrs);
    TerminalCell narrow = linkedAttrs;
    narrow.drawn = 1;
    narrow.semantic = semantic;
    TerminalCell continuation = narrow;
    continuation.dwidth_cont = 1;
    narrow.dwidth = 0;
    TerminalCell wide = narrow;
    wide.dwidth = 1;
    TerminalCell* output = cells;
    for (u16 index = 0; index < glyphCount; ++index) {
        if (widths[index] == 2) {
            TerminalCell lead = wide;
            lead.uc_pt = codepoints[index];
            *output++ = lead;
            *output++ = continuation;
        } else {
            TerminalCell lead = narrow;
            lead.uc_pt = codepoints[index];
            *output++ = lead;
        }
    }
    STD_ASSERT(output == cells + cellCount);
    slot->metadata.protection |= linkedAttrs.protected_char;
    slot->metadata.wide |= cellCount != glyphCount;
}

template <typename Traits>
void ScreenBase<Traits>::writeGraphemeRun(u16 row, u16 column, const u32* codepoints, const ScreenGrapheme* graphemes, u16 count, u16 cellCount, const TerminalCell& attrs, u32 hyperlink, u32 semantic, const TerminalCell& eraseAttrs) {
    CellExtraStore& extras = cellExtras();
    TerminalCell linkedAttrs = attrs;
    if (hyperlink != 0 || linkedAttrs.hasExtra()) {
        extras.setHyperlink(linkedAttrs, hyperlink);
    }
    linkedAttrs.drawn = 1;
    linkedAttrs.semantic = semantic;
    TerminalCell continuation = linkedAttrs;
    continuation.dwidth_cont = 1;
    linkedAttrs.dwidth = 0;
    TerminalCell wide = linkedAttrs;
    wide.dwidth = 1;
    RowSlot& slot = logicalRowSlot(row);
    TerminalCell* const cells = prepareSpan(slot, row, column, cellCount, eraseAttrs);
    TerminalCell* output = cells;
    for (u16 index = 0; index < count; ++index) {
        const ScreenGrapheme grapheme = graphemes[index];
        TerminalCell lead = grapheme.width == 2 ? wide : linkedAttrs;
        lead.uc_pt = codepoints[0];
        if (grapheme.count != 1) {
            extras.setGrapheme(lead, codepoints, grapheme.count);
        }
        *output++ = lead;
        if (grapheme.width == 2) {
            *output++ = continuation;
        }
        codepoints += grapheme.count;
    }
    STD_ASSERT(output == cells + cellCount);
    slot->metadata.protection |= linkedAttrs.protected_char;
    slot->metadata.wide |= cellCount != count;
}

template <typename Traits>
void ScreenBase<Traits>::fillRectangle(u16 top, u16 left, u16 bottom, u16 right, u32 codepoint, const TerminalCell& attrs, const TerminalCell& eraseAttrs) {
    for (u16 row = top; row < bottom; ++row) {
        clearWideBoundary(row, left, eraseAttrs);
        clearWideBoundary(row, right, eraseAttrs);
        TerminalCell* cells_ = mutableLogicalRow(row) + left;
        for (u16 column = left; column < right; ++column) {
            TerminalCell& cell = cells_[column - left];
            cell = attrs;
            cell.uc_pt = codepoint;
            // A fill writes the character: DECRQCRA counts it like text.
            cell.drawn = 1;
        }
        logicalRowSlot(row)->metadata.protection |= attrs.protected_char;
    }
    damageRows(top, bottom);
    if (!selection.empty()) {
        invalidateSelection(Rect(left, top, right, bottom));
    }
}

template <typename Traits>
void ScreenBase<Traits>::copyRectangle(u16 sourceTop, u16 sourceLeft, u16 targetTop, u16 targetLeft, u16 height, u16 width, const TerminalCell& eraseAttrs) {
    Vector<TerminalCell> copied((size_t)(height)*width);
    Vector<u16> copiedWidths(height);
    for (u16 row = 0; row < height; ++row) {
        const Row* const sourceObject = getLogicalRowObject(sourceTop + row);
        const Row* const targetObject = getLogicalRowObject(targetTop + row);
        const u16 sourceColumns = sourceObject->metadata.lineAttribute == 0 ? nCols : max<Coord>((Coord)(1), nCols / 2);
        const u16 targetColumns = targetObject->metadata.lineAttribute == 0 ? nCols : max<Coord>((Coord)(1), nCols / 2);
        const u16 sourceAvailable = sourceLeft < sourceColumns ? sourceColumns - sourceLeft : 0;
        const u16 targetAvailable = targetLeft < targetColumns ? targetColumns - targetLeft : 0;
        const u16 rowWidth = min(width, min(sourceAvailable, targetAvailable));
        copiedWidths.pushBack(rowWidth);
        copied.append(sourceObject->cells + sourceLeft, rowWidth);
    }
    const TerminalCell* source = copied.data();
    for (u16 row = 0; row < height; ++row) {
        const u16 rowWidth = copiedWidths[row];
        if (rowWidth == 0) {
            continue;
        }
        clearWideBoundary(targetTop + row, targetLeft, eraseAttrs);
        clearWideBoundary(targetTop + row, targetLeft + rowWidth, eraseAttrs);
        TerminalCell* destination = mutableLogicalRow(targetTop + row) + targetLeft;
        // The copied cells carry the source rows' wrap bits; the target
        // row keeps its own soft-wrap state.
        const u16 wrapColumn = rowWrapColumn(logicalRowSlot(targetTop + row), nCols);
        for (u16 column = 0; column < rowWidth; ++column) {
            destination[column] = source[column];
        }
        restoreRowWrap(logicalRowSlot(targetTop + row), nCols, wrapColumn);
        logicalRowSlot(targetTop + row)->metadata.protection |= rowProtection(source, rowWidth);
        if (rowContainsWide(source, rowWidth)) {
            markLogicalRowWide(targetTop + row);
        }
        repairWideBoundary(targetTop + row, targetLeft, eraseAttrs);
        repairWideBoundary(targetTop + row, targetLeft + rowWidth, eraseAttrs);
        damageRow(targetTop + row);
        if (!selection.empty()) {
            invalidateSelection(Rect(targetLeft, targetTop + row, targetLeft + rowWidth, targetTop + row));
        }
        source += rowWidth;
    }
}

template <typename Traits>
void ScreenBase<Traits>::changeRectangleAttributes(u16 top, u16 left, u16 bottom, u16 right, CellAttributeChange change) {
    const auto apply = [change](u8 value, u16 bit) {
        return change.toggleMask & bit ? (u8)(!value) : change.setMask & bit ? (u8)(1) : change.clearMask & bit ? (u8)(0) : value;
    };
    CellExtraStore& extras = cellExtras();
    for (u16 row = top; row < bottom; ++row) {
        TerminalCell* cells_ = mutableLogicalRow(row) + left;
        for (u16 column = left; column < right; ++column) {
            TerminalCell& cell = cells_[column - left];
            cell.bold = apply(cell.bold, CellAttributeChange::Bold);
            cell.faint = apply(cell.faint, CellAttributeChange::Faint);
            cell.italic = apply(cell.italic, CellAttributeChange::Italic);
            if (change.underlineStyleChanged) {
                cell.underline_style = change.underlineStyle;
            } else {
                cell.underline_style = apply(cell.underline_style, CellAttributeChange::Underline);
            }
            cell.blink = apply(cell.blink, CellAttributeChange::Blink);
            cell.inverse = apply(cell.inverse, CellAttributeChange::Inverse);
            cell.conceal = apply(cell.conceal, CellAttributeChange::Conceal);
            cell.strike = apply(cell.strike, CellAttributeChange::Strike);
            cell.overline = apply(cell.overline, CellAttributeChange::Overline);
            if (change.colorMask & CellAttributeChange::Foreground) {
                cell.setForeground(change.foreground);
            }
            if (change.colorMask & CellAttributeChange::Background) {
                cell.setBackground(change.background);
            }
            if (change.colorMask & CellAttributeChange::UnderlineColor) {
                extras.setUnderlineColor(cell, change.colorMask & CellAttributeChange::UnderlineFromForeground ? cell.foreground() : change.underlineColor);
            }
        }
    }
    damageRows(top, bottom);
    if (!selection.empty()) {
        invalidateSelection(Rect(left, top, right, bottom));
    }
}

template <typename Traits>
u16 ScreenBase<Traits>::checksum(u16 top, u16 left, u16 bottom, u16 right, u8 flags) const noexcept {
    u32 total = 0;
    u32 trimmed = 0;
    bool first = true;
    CellExtraStore& extras = cellExtras();
    for (u16 row = top; row < bottom; ++row) {
        const TerminalCell* cells_ = getLogicalRowPtr(row);
        for (u16 column = left; column < right; ++column) {
            const TerminalCell& cell = cells_[column];
            const bool written = cell.drawn;
            if (!written && !(flags & (ChecksumKeepBlanks | ChecksumIncludeUndrawn))) {
                continue;
            }

            u32 value;
            if (!written) {
                value = ' ';
            } else if (flags & ChecksumRawCodepoint) {
                value = cell.uc_pt;
            } else if (cell.uc_pt >= 0x20 && cell.uc_pt <= 0xff) {
                value = cell.uc_pt & 0x7f;
            } else {
                value = 0x1b;
            }

            u32 attributes = 0;
            if (!(flags & ChecksumNoAttributes)) {
                attributes += (cell.protected_char & TerminalCell::decProtection) ? 0x04 : 0;
                attributes += cell.conceal ? 0x08 : 0;
                attributes += cell.underlined() ? 0x10 : 0;
                attributes += cell.inverse ? 0x20 : 0;
                attributes += cell.blink ? 0x40 : 0;
                attributes += cell.bold ? 0x80 : 0;
                value += attributes;
            }

            if (first || value != ' ' || written || attributes != 0) {
                trimmed += value;
            }
            total += value;

            if (written && !(flags & ChecksumRawCodepoint)) {
                // A written cell always lands in trimmed too, so the
                // cluster continuation codepoints count in both sums.
                const GraphemeView grapheme = extras.grapheme(cell);
                for (size_t index = 1; index < grapheme.size(); ++index) {
                    total += grapheme[index];
                    trimmed += grapheme[index];
                }
            }
            first = flags & ChecksumKeepBlanks;
        }
        if (!(flags & ChecksumKeepBlanks)) {
            first = false;
        }
    }
    u32 result = flags & ChecksumKeepBlanks ? total : trimmed;
    if (!(flags & ChecksumPositive)) {
        result = 0u - result;
    }
    return result & 0xffff;
}

template <typename Traits>
ScreenHyperlink ScreenBase<Traits>::hyperlinkAt(u16 row, u16 column) const {
    static constexpr size_t scanLimit = 4096;
    if (row >= nRows || column >= nCols) {
        return {};
    }
    CellExtraStore& extras = cellExtras();

    LinkPosition pointed{
        .row = (i32)(row) - (i32)(viewOffset),
        .column = column,
    };
    const Row* const pointedObject = getLogicalRowObject(pointed.row);
    const TerminalCell* pointedRow = pointedObject->cells;
    if (pointedObject->metadata.lineAttribute != 0) {
        pointed.column /= 2;
    }
    if (pointedRow[pointed.column].dwidth_cont && pointed.column != 0) {
        --pointed.column;
    }

    const TerminalCell& pointedCell = getLogicalRowPtr(pointed.row)[pointed.column];
    const u32 explicitId = extras.hyperlinkDisplayId(pointedCell);
    if (explicitId != 0) {
        return {
            .payload = extras.hyperlink(pointedCell),
            .displayId = explicitId,
        };
    }

    const auto boundaryCodepoint = [](u32 codepoint) {
        if (codepoint == '"' || codepoint == '\'' || codepoint == '`' || codepoint == '<' || codepoint == '>') {
            return true;
        }
        switch (unicodeCodepointProperties(codepoint).category) {
            case GeneralCategory::Control:
            case GeneralCategory::SpaceSeparator:
            case GeneralCategory::LineSeparator:
            case GeneralCategory::ParagraphSeparator:
                return true;
            default:
                return false;
        }
    };
    const auto boundary = [&](LinkPosition position) {
        const TerminalCell& cell = getLogicalRowPtr(position.row)[position.column];
        if (cell.conceal || extras.hyperlinkDisplayId(cell) != 0) {
            return true;
        }
        const GraphemeView grapheme = extras.grapheme(cell);
        if (grapheme.empty()) {
            return boundaryCodepoint(cell.uc_pt == 0 ? ' ' : cell.uc_pt);
        }
        for (const u32 codepoint : grapheme) {
            if (boundaryCodepoint(codepoint)) {
                return true;
            }
        }
        return false;
    };
    if (boundary(pointed)) {
        return {};
    }

    const auto validColumns = [&](i32 logicalRow) {
        return getLogicalRowObject(logicalRow)->metadata.lineAttribute == 0 ? (u16)(nCols) : (u16)(max<Coord>((Coord)(1), nCols / 2));
    };
    const auto previous = [&](LinkPosition& position) {
        if (position.column != 0) {
            --position.column;
            const TerminalCell* cells_ = getLogicalRowPtr(position.row);
            if (cells_[position.column].dwidth_cont && position.column != 0) {
                --position.column;
            }
            return true;
        }
        const i32 minimumRow = -(i32)(historyRows);
        if (position.row <= minimumRow) {
            return false;
        }
        const i32 previousRow = position.row - 1;
        const TerminalCell* cells_ = getLogicalRowPtr(previousRow);
        const u16 columns_ = validColumns(previousRow);
        for (u16 previousColumn = columns_; previousColumn != 0; --previousColumn) {
            if (cells_[previousColumn - 1].wrap) {
                position = {
                    .row = previousRow,
                    .column = (u16)(previousColumn - 1),
                };
                if (cells_[position.column].dwidth_cont && position.column != 0) {
                    --position.column;
                }
                return true;
            }
        }
        return false;
    };
    const auto next = [&](LinkPosition& position) {
        const TerminalCell* cells_ = getLogicalRowPtr(position.row);
        const TerminalCell& cell = cells_[position.column];
        if (cell.wrap) {
            if (position.row + 1 >= nRows) {
                return false;
            }
            position = {
                .row = position.row + 1,
                .column = 0,
            };
            return true;
        }
        const u16 nextColumn = position.column + (cell.dwidth ? 2 : 1);
        if (nextColumn >= validColumns(position.row)) {
            return false;
        }
        position.column = nextColumn;
        return true;
    };

    linkLeft.clear();
    linkParts.clear();
    linkScratch.reset();

    LinkPosition position = pointed;
    size_t count = 1;
    while (previous(position)) {
        if (boundary(position)) {
            break;
        }
        if (count == scanLimit) {
            return {};
        }
        linkLeft.pushBack(position);
        ++count;
    }

    const auto append = [&](LinkPosition source) {
        LinkPart part{
            .position = source,
            .begin = linkScratch.used(),
        };
        const TerminalCell& cell = getLogicalRowPtr(source.row)[source.column];
        const GraphemeView grapheme = extras.grapheme(cell);
        const auto sink = [&](u8 byte) {
            linkScratch.append(&byte, 1);
        };
        if (grapheme.empty()) {
            Utf8Encoder::pushUnicode(cell.uc_pt == 0 ? ' ' : cell.uc_pt, sink);
        } else {
            for (const u32 codepoint : grapheme) {
                Utf8Encoder::pushUnicode(codepoint, sink);
            }
        }
        part.end = linkScratch.used();
        linkParts.pushBack(part);
    };

    for (size_t index = linkLeft.length(); index != 0; --index) {
        append(linkLeft[index - 1]);
    }
    const size_t pointedPart = linkParts.length();
    append(pointed);

    position = pointed;
    while (next(position)) {
        if (boundary(position)) {
            break;
        }
        if (count == scanLimit) {
            return {};
        }
        append(position);
        ++count;
    }

    const auto* bytes = (const u8*)(linkScratch.data());
    size_t begin = 0;
    size_t end = linkScratch.used();
    const auto asciiAlpha = [](u8 byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
    };
    const auto schemeByte = [&](u8 byte) {
        return asciiAlpha(byte) || (byte >= '0' && byte <= '9') || byte == '+' || byte == '-' || byte == '.';
    };
    size_t schemeEnd = begin;
    while (schemeEnd < end && bytes[schemeEnd] != ':') {
        ++schemeEnd;
    }
    if (schemeEnd == end) {
        return {};
    }
    begin = schemeEnd;
    while (begin != 0 && schemeByte(bytes[begin - 1])) {
        --begin;
    }
    if (schemeEnd == begin || schemeEnd - begin > 64 || !asciiAlpha(bytes[begin])) {
        return {};
    }
    for (size_t index = begin + 1; index < schemeEnd; ++index) {
        if (!schemeByte(bytes[index])) {
            return {};
        }
    }
    const auto unmatchedClosing = [&](u8 opening, u8 closing) {
        size_t openings = 0;
        size_t closings = 0;
        for (size_t index = begin; index < end; ++index) {
            openings += bytes[index] == opening;
            closings += bytes[index] == closing;
        }
        return closings > openings;
    };
    bool trimmed = true;
    while (begin < end && trimmed) {
        trimmed = false;
        const u8 last = bytes[end - 1];
        if (last == '.' || last == ',' || last == ';' || last == ':' || last == '!' || last == '?') {
            --end;
            trimmed = true;
        } else if (last == ')' && unmatchedClosing('(', ')')) {
            --end;
            trimmed = true;
        } else if (last == ']' && unmatchedClosing('[', ']')) {
            --end;
            trimmed = true;
        } else if (last == '}' && unmatchedClosing('{', '}')) {
            --end;
            trimmed = true;
        }
    }
    if (begin == end) {
        return {};
    }
    if (schemeEnd + 1 >= end) {
        return {};
    }

    const LinkPart& pointedRecord = linkParts[pointedPart];
    if (pointedRecord.end <= begin || pointedRecord.begin >= end) {
        return {};
    }

    u32 rangeBegin = (u32)(nCols)*nRows;
    u32 rangeEnd = 0;
    for (const LinkPart& part : linkParts) {
        if (part.end <= begin || part.begin >= end) {
            continue;
        }
        const i32 visibleRow = part.position.row + (i32)(viewOffset);
        if (visibleRow < 0 || visibleRow >= nRows) {
            continue;
        }
        const TerminalCell& cell = getLogicalRowPtr(part.position.row)[part.position.column];
        const u32 cellBegin = (u32)(visibleRow)*nCols + part.position.column;
        const u32 cellEnd = cellBegin + (cell.dwidth ? 2 : 1);
        rangeBegin = min(rangeBegin, cellBegin);
        rangeEnd = max(rangeEnd, min<u32>((u32)(nCols)*nRows, cellEnd));
    }
    if (rangeBegin >= rangeEnd) {
        return {};
    }
    return {
        .payload = StringView(bytes + begin, end - begin),
        .scheme = StringView(bytes + begin, schemeEnd - begin),
        .begin = rangeBegin,
        .end = rangeEnd,
    };
}

template <typename Traits>
TerminalCell ScreenBase<Traits>::testCell(u16 row, u16 column) const noexcept {
    return getViewRowPtr(row)[column];
}

template <typename Traits>
TerminalCell ScreenBase<Traits>::testLogicalCell(i32 row, u16 column) const noexcept {
    return getLogicalRowPtr(row)[column];
}

template <typename Traits>
u32 ScreenBase<Traits>::materializedRows() const noexcept {
    u32 result = 0;
    for (u32 slot = 0; slot < rowCapacity; ++slot) {
        result += rowRing[slot] != nullptr;
    }
    return result;
}

template <typename Traits>
void ScreenBase<Traits>::eraseInRow(RowSlot& slot, u16 pY, u16 startX, u16 count, const TerminalCell& attrs) {
    if (!count) {
        return;
    }

    TerminalCell erased = attrs;
    Row* object = slot;
    TerminalCell* row = rowData(object);
    if (startX == 0 && count == nCols) {
        const u8 lineAttribute_ = object == nullptr ? 0 : object->metadata.lineAttribute;
        const ScreenSemanticPrompt semanticPrompt = object == nullptr ? ScreenSemanticPrompt::None : object->metadata.semanticPrompt;
        if (erased == TerminalCell{} && lineAttribute_ == 0 && semanticPrompt == ScreenSemanticPrompt::None) {
            releaseRow(object);
            slot = nullptr;
        } else {
            if (!erasedRowTemplateValid || erasedRowTemplate.length() != nCols || erasedRowCell != erased) {
                erasedRowTemplate.clear();
                while (erasedRowTemplate.length() < nCols) {
                    erasedRowTemplate.pushBack(erased);
                }
                erasedRowCell = erased;
                erasedRowTemplateValid = true;
            }
            memcpy(mutableRow(slot), erasedRowTemplate.data(), nCols * cellSize);
            slot->metadata.protection = erased.protected_char;
            slot->metadata.wide = false;
        }
    } else if (row != nullptr || erased != TerminalCell{}) {
        TerminalCell* const start = mutableRow(slot) + startX;
        eraseRange(start, start + count, erased);
        slot->metadata.protection |= erased.protected_char;
    }
    damageRow(pY);
    if (!selection.empty()) {
        invalidateSelection(Rect(startX, pY, startX + count, pY));
    }
}

template <typename Traits>
void ScreenBase<Traits>::eraseCells(u16 pY, u16 startX, u16 count, const TerminalCell& attrs) {
    if (!count) {
        return;
    }
    RowSlot& slot = logicalRowSlot(pY);
    if (slot == nullptr || !slot->metadata.wide) {
        eraseInRow(slot, pY, startX, count, attrs);
        return;
    }
    const u16 endX = startX + count;
    const TerminalCell* source = rowData(slot);
    TerminalCell erased = attrs;
    const bool eraseLeft = startX > 0 && (source[startX - 1].dwidth || source[startX].dwidth_cont);
    const bool eraseRight = endX < nCols && (source[endX - 1].dwidth || source[endX].dwidth_cont);
    if (!eraseLeft && !eraseRight) {
        eraseInRow(slot, pY, startX, count, attrs);
        return;
    }
    TerminalCell* row = rowData(slot);
    if (eraseLeft) {
        row[startX - 1] = erased;
    }
    if (eraseRight) {
        row[endX] = erased;
    }
    if (startX == 0 && count == nCols) {
        if (!erasedRowTemplateValid || erasedRowTemplate.length() != nCols || erasedRowCell != erased) {
            erasedRowTemplate.clear();
            while (erasedRowTemplate.length() < nCols) {
                erasedRowTemplate.pushBack(erased);
            }
            erasedRowCell = erased;
            erasedRowTemplateValid = true;
        }
        memcpy(row, erasedRowTemplate.data(), nCols * cellSize);
    } else {
        for (u16 x = startX; x < endX; ++x) {
            row[x] = erased;
        }
    }
    slot->metadata.protection |= erased.protected_char;
    if (startX == 0 && count == nCols) {
        slot->metadata.protection = erased.protected_char;
        slot->metadata.wide = false;
    }
    const u16 damageStart = eraseLeft ? startX - 1 : startX;
    const u16 damageEnd = eraseRight ? endX + 1 : endX;
    damageRow(pY);
    if (!selection.empty()) {
        invalidateSelection(Rect(damageStart, pY, damageEnd, pY));
    }
}

template <typename Traits>
TerminalCell* ScreenBase<Traits>::overwriteWideSpan(u16 pY, u16 startX, u16 count, const TerminalCell& eraseAttrs) {
    const u16 endX = startX + count;
    TerminalCell* row = mutableLogicalRow(pY);
    TerminalCell erased = eraseAttrs;
    const bool eraseLeft = startX > 0 && (row[startX - 1].dwidth || row[startX].dwidth_cont);
    const bool eraseRight = endX < nCols && (row[endX - 1].dwidth || row[endX].dwidth_cont);
    if (eraseLeft) {
        row[startX - 1] = erased;
    }
    if (eraseRight) {
        row[endX] = erased;
    }
    logicalRowSlot(pY)->metadata.protection |= erased.protected_char;
    const u16 damageStart = eraseLeft ? startX - 1 : startX;
    const u16 damageEnd = eraseRight ? endX + 1 : endX;
    damageRow(pY);
    if (!selection.empty()) {
        invalidateSelection(Rect(damageStart, pY, damageEnd, pY));
    }
    return row + startX;
}

template <typename Traits>
[[gnu::always_inline]] inline void ScreenBase<Traits>::clearWideBoundary(u16 pY, u16 boundary, const TerminalCell& attrs) {
    RowSlot& slot = logicalRowSlot(pY);
    clearWideBoundary(slot, pY, boundary, attrs);
}

template <typename Traits>
[[gnu::always_inline]] inline void ScreenBase<Traits>::clearWideBoundary(RowSlot& slot, u16 pY, u16 boundary, const TerminalCell& attrs) {
    if (slot == nullptr || !slot->metadata.wide) {
        return;
    }
    if (boundary == 0 || boundary >= nCols) {
        return;
    }
    const TerminalCell* source = rowData(slot);
    if (!source[boundary - 1].dwidth) {
        return;
    }
    clearWideBoundarySlow(rowData(slot), pY, boundary, attrs);
}

template <typename Traits>
[[gnu::noinline]] void ScreenBase<Traits>::clearWideBoundarySlow(TerminalCell* row, u16 pY, u16 boundary, const TerminalCell& attrs) {
    TerminalCell erased = attrs;
    row[boundary - 1] = erased;
    damageRow(pY);
    if (!selection.empty()) {
        invalidateSelection(Rect(boundary - 1, pY));
    }
    row[boundary] = erased;
    damageRow(pY);
    if (!selection.empty()) {
        invalidateSelection(Rect(boundary, pY));
    }
    logicalRowSlot(pY)->metadata.protection |= erased.protected_char;
}

template <typename Traits>
[[gnu::always_inline]] inline void ScreenBase<Traits>::repairWideBoundary(u16 pY, u16 boundary, const TerminalCell& attrs) {
    RowSlot& slot = logicalRowSlot(pY);
    repairWideBoundary(slot, pY, boundary, attrs);
}

template <typename Traits>
[[gnu::always_inline]] inline void ScreenBase<Traits>::repairWideBoundary(RowSlot& slot, u16 pY, u16 boundary, const TerminalCell& attrs) {
    if (slot == nullptr || !slot->metadata.wide) {
        return;
    }
    const TerminalCell* source = rowData(slot);
    const bool leftLead = boundary > 0 && source[boundary - 1].dwidth;
    const bool rightContinuation = boundary < nCols && source[boundary].dwidth_cont;
    if (leftLead == rightContinuation) {
        return;
    }
    repairWideBoundarySlow(rowData(slot), pY, boundary, attrs, leftLead);
}

template <typename Traits>
[[gnu::noinline]] void ScreenBase<Traits>::repairWideBoundarySlow(TerminalCell* row, u16 pY, u16 boundary, const TerminalCell& attrs, bool eraseLeft) {
    TerminalCell erased = attrs;
    if (eraseLeft) {
        row[boundary - 1] = erased;
        damageRow(pY);
        if (!selection.empty()) {
            invalidateSelection(Rect(boundary - 1, pY));
        }
    } else {
        row[boundary] = erased;
        damageRow(pY);
        if (!selection.empty()) {
            invalidateSelection(Rect(boundary, pY));
        }
    }
    logicalRowSlot(pY)->metadata.protection |= erased.protected_char;
}

template <typename Traits>
void ScreenBase<Traits>::selectiveEraseCells(u16 pY, u16 startX, u16 count, const TerminalCell& attrs, u8 protectionMask) {
    CellExtraStore& extras = cellExtras();
    TerminalCell erased = attrs;
    erased.uc_pt = 0;
    erased.protected_char = 0;
    extras.clearExtra(erased, extras.underlineColor(attrs));
    if (rawLogicalRow(pY) == nullptr && erased == TerminalCell{}) {
        damageRow(pY);
        if (!selection.empty()) {
            invalidateSelection(Rect(startX, pY, startX + count, pY));
        }
        repairWideBoundary(pY, startX, attrs);
        repairWideBoundary(pY, startX + count, attrs);
        return;
    }
    TerminalCell* row = mutableLogicalRow(pY);
    bool changed = false;
    u16 changedStart = nCols;
    for (u16 x = startX; x < startX + count; ++x) {
        TerminalCell& cell = row[x];
        if (!(cell.protected_char & protectionMask)) {
            if (changedStart == nCols) {
                changedStart = x;
            }
            cell = erased;
            changed = true;
        } else if (changedStart != nCols) {
            if (!selection.empty()) {
                invalidateSelection(Rect(changedStart, pY, x, pY));
            }
            changedStart = nCols;
        }
    }
    if (changedStart != nCols && !selection.empty()) {
        invalidateSelection(Rect(changedStart, pY, startX + count, pY));
    }
    if (changed) {
        logicalRowSlot(pY)->metadata.protection = rowProtection(row, nCols);
        damageRow(pY);
    }
    repairWideBoundary(pY, startX, attrs);
    repairWideBoundary(pY, startX + count, attrs);
}

template <typename Traits>
void ScreenBase<Traits>::moveInRow(u16 pY, u16 dstX, u16 srcX, u16 count) {
    if (!count) {
        return;
    }

    TerminalCell* const row = rawLogicalRow(pY);
    if (row != nullptr) {
        moveCells(row + dstX, row + srcX, count);
    }
    damageRow(pY);
    if (!selection.empty()) {
        invalidateSelection(Rect(dstX, pY, dstX + count, pY));
    }
}

template <typename Traits>
void ScreenBase<Traits>::insertCells(u16 row, u16 start, u16 end, u16 count, const TerminalCell& attrs) {
    count = min<u16>(count, end - start);
    if (count == 0) {
        return;
    }
    const u16 moved = end - start - count;
    RowSlot& slot = logicalRowSlot(row);
    const u16 wrapColumn = rowWrapColumn(slot, nCols);
    u16 restoredWrapColumn = wrapColumn;
    if (wrapColumn >= start && wrapColumn < end) {
        restoredWrapColumn = count > end - 1 - wrapColumn ? end - 1 : wrapColumn + count;
    }
    if (slot == nullptr || !slot->metadata.wide) {
        TerminalCell* cells = rowData(slot);
        if (moved != 0 && cells != nullptr) {
            if (cells[end - 1].wrap) {
                cells[end - 1].wrap = 0;
                cells[end - count - 1].wrap = 1;
            }
            moveCells(cells + start + count, cells + start, moved);
        }
        if (moved == 0) {
            eraseInRow(slot, row, start, count, attrs);
            restoreRowWrap(slot, nCols, restoredWrapColumn);
            return;
        }
        TerminalCell erased = attrs;
        if (cells != nullptr || erased != TerminalCell{}) {
            cells = mutableRow(slot);
            eraseRange(cells + start, cells + start + count, erased);
            slot->metadata.protection |= erased.protected_char;
        }
        damageRow(row);
        if (!selection.empty()) {
            invalidateSelection(Rect(start, row, end, row));
        }
        restoreRowWrap(slot, nCols, restoredWrapColumn);
        return;
    }
    if (moved != 0) {
        moveWrap(row, end - 1, end - count - 1);
        clearWideBoundary(row, start, attrs);
        clearWideBoundary(row, end - count, attrs);
        moveInRow(row, start + count, start, moved);
        repairWideBoundary(row, start + count, attrs);
        repairWideBoundary(row, end, attrs);
    }
    eraseCells(row, start, count, attrs);
    restoreRowWrap(slot, nCols, restoredWrapColumn);
}

template <typename Traits>
void ScreenBase<Traits>::deleteCells(u16 row, u16 start, u16 end, u16 count, const TerminalCell& attrs) {
    count = min<u16>(count, end - start);
    if (count == 0) {
        return;
    }
    const u16 moved = end - start - count;
    RowSlot& slot = logicalRowSlot(row);
    const u16 wrapColumn = rowWrapColumn(slot, nCols);
    u16 restoredWrapColumn = wrapColumn;
    if (wrapColumn >= start && wrapColumn < end) {
        const bool fullBoundaryWrap = end == nCols && (wrapColumn == end - 1 || (end > 1 && wrapColumn == end - 2));
        if (wrapColumn < start + count) {
            restoredWrapColumn = nCols;
        } else if (fullBoundaryWrap) {
            restoredWrapColumn = nCols;
        } else {
            restoredWrapColumn = wrapColumn - count;
        }
    }
    TerminalCell* const wrapCells = rowData(slot);
    if (wrapCells != nullptr) {
        // A normal wrap marks the right boundary.  Pre-wrapping a wide
        // glyph marks the preceding cell because the last cell stays empty.
        wrapCells[end - 1].wrap = 0;
        if (end > 1) {
            wrapCells[end - 2].wrap = 0;
        }
    }
    if (slot == nullptr || !slot->metadata.wide) {
        TerminalCell* cells = rowData(slot);
        if (moved != 0 && cells != nullptr) {
            moveCells(cells + start, cells + start + count, moved);
        }
        if (moved == 0) {
            eraseInRow(slot, row, start, count, attrs);
            restoreRowWrap(slot, nCols, restoredWrapColumn);
            return;
        }
        TerminalCell erased = attrs;
        if (cells != nullptr || erased != TerminalCell{}) {
            cells = mutableRow(slot);
            eraseRange(cells + start + moved, cells + end, erased);
            slot->metadata.protection |= erased.protected_char;
        }
        damageRow(row);
        if (!selection.empty()) {
            invalidateSelection(Rect(start, row, end, row));
        }
        restoreRowWrap(slot, nCols, restoredWrapColumn);
        return;
    }
    if (moved != 0) {
        clearWideBoundary(row, start + count, attrs);
        clearWideBoundary(row, end, attrs);
        moveInRow(row, start, start + count, moved);
        repairWideBoundary(row, start, attrs);
        repairWideBoundary(row, start + moved, attrs);
    }
    eraseCells(row, start + moved, count, attrs);
    restoreRowWrap(slot, nCols, restoredWrapColumn);
}

template <typename Traits>
void ScreenBase<Traits>::copyRow(u16 dstY, u16 srcY, u16 startX, u16 count, const TerminalCell& attrs) {
    if (!count) {
        return;
    }

    clearWideBoundary(dstY, startX, attrs);
    clearWideBoundary(dstY, startX + count, attrs);
    const Row* const sourceObject = getLogicalRowObject(srcY);
    const bool sourceWide = sourceObject->metadata.wide;
    const TerminalCell* const source = sourceObject->cells + startX;
    TerminalCell* destinationRow = rawLogicalRow(dstY);
    if (destinationRow == nullptr && (startX != 0 || sourceObject->metadata.lineAttribute == 0) && (sourceObject == zeroRow || emptyRow(source, count))) {
        damageRow(dstY);
        if (!selection.empty()) {
            invalidateSelection(Rect(startX, dstY, startX + count, dstY));
        }
        repairWideBoundary(dstY, startX, attrs);
        repairWideBoundary(dstY, startX + count, attrs);
        return;
    }
    destinationRow = mutableLogicalRow(dstY);
    TerminalCell* const destination = destinationRow + startX;
    copyCells(destination, source, count);
    RowSlot& destinationObject = logicalRowSlot(dstY);
    if (startX == 0) {
        destinationObject->metadata.lineAttribute = sourceObject->metadata.lineAttribute;
    }
    destinationObject->metadata.protection |= sourceObject->metadata.protection;
    if (startX == 0 && count == nCols) {
        destinationObject->metadata.wide = sourceWide;
        destinationObject->metadata.protection = rowProtection(destinationRow, nCols);
    } else if (sourceWide) {
        markLogicalRowWide(dstY);
    }
    if (startX == 0 && count == nCols && destinationObject->metadata.lineAttribute == 0 && destinationObject->metadata.semanticPrompt == ScreenSemanticPrompt::None && emptyRow(destinationRow, nCols)) {
        RowSlot& slot = logicalRowSlot(dstY);
        releaseRow(slot);
        slot = nullptr;
    }
    damageRow(dstY);
    if (!selection.empty()) {
        invalidateSelection(Rect(startX, dstY, startX + count, dstY));
    }
    repairWideBoundary(dstY, startX, attrs);
    repairWideBoundary(dstY, startX + count, attrs);
}

template <typename Traits>
void ScreenBase<Traits>::scrollRectangle(u16 top, u16 left, u16 bottom, u16 right, i32 rows, const TerminalCell& attrs) {
    if (rows == 0) {
        return;
    }
    const bool down = rows > 0;
    const u16 count = min<u32>(down ? rows : -(i64)(rows), 0xffff);
    if (!down && count == 1 && top < bottom && left < right && (left != 0 || right != nCols)) {
        scrollPartialRectangleUpOne(top, left, bottom, right, attrs);
        return;
    }
    scrollRectangleImpl(top, left, bottom, right, count, attrs, down);
}

template <typename Traits>
void ScreenBase<Traits>::scrollPartialRectangleUpOne(u16 top, u16 left, u16 bottom, u16 right, const TerminalCell& attrs) {
    const i64 rowBase = (i64)(rowEnd)-nRows;
    for (u16 destination = top; destination + 1 < bottom; ++destination) {
        RowSlot& destinationObject = rowRing[wrapRow(rowBase + destination)];
        const Row* const sourceObject = rowRing[wrapRow(rowBase + destination + 1)];
        scrollPartialRectangleRow(destinationObject, sourceObject != nullptr ? sourceObject : zeroRow, destination, left, right, attrs);
    }

    const u16 row = bottom - 1;
    RowSlot& object = rowRing[wrapRow(rowBase + row)];
    const u16 wrapColumn = rowWrapColumn(object, nCols);
    if (object != nullptr && object->metadata.wide) {
        clearWideBoundary(object, row, left, attrs);
        clearWideBoundary(object, row, right, attrs);
    }
    const TerminalCell empty{};
    if (object != nullptr || attrs != empty) {
        TerminalCell* const cells = mutableRow(object);
        eraseRange(cells + left, cells + right, attrs);
        object->metadata.protection |= attrs.protected_char;
    }
    restoreRowWrap(object, nCols, wrapColumn);
    damageRows(top, bottom);
    if (!selection.empty()) {
        invalidateSelection(Rect(left, top, right, bottom));
    }
}

template <typename Traits>
[[gnu::always_inline]] inline void ScreenBase<Traits>::scrollPartialRectangleRow(RowSlot& destinationObject, const Row* sourceObject, u16 destinationRow, u16 left, u16 right, const TerminalCell& attrs) {
    const u16 width = right - left;
    const TerminalCell* const source = sourceObject->cells + left;
    if (destinationObject == nullptr && (sourceObject == zeroRow || emptyRow(source, width))) {
        return;
    }
    const u16 wrapColumn = rowWrapColumn(destinationObject, nCols);
    if (!sourceObject->metadata.wide && (destinationObject == nullptr || !destinationObject->metadata.wide)) {
        TerminalCell* const destination = mutableRow(destinationObject);
        copyCells(destination + left, source, width);
        destinationObject->metadata.protection |= sourceObject->metadata.protection;
        restoreRowWrap(destinationObject, nCols, wrapColumn);
        return;
    }
    TerminalCell* const destination = mutableRow(destinationObject);
    const bool eraseOutsideLeft = left != 0 && destination[left - 1].dwidth;
    const bool eraseInsideLeft = source[0].dwidth_cont;
    const bool eraseInsideRight = source[width - 1].dwidth;
    const bool eraseOutsideRight = right != nCols && destination[right - 1].dwidth;
    if (!eraseOutsideLeft && !eraseInsideLeft && !eraseInsideRight && !eraseOutsideRight) {
        copyCells(destination + left, source, width);
        destinationObject->metadata.protection |= sourceObject->metadata.protection;
        if (sourceObject->metadata.wide) {
            destinationObject->metadata.wide = true;
        }
        restoreRowWrap(destinationObject, nCols, wrapColumn);
        return;
    }
    copyCells(destination + left, source, width);

    bool erasedBoundary = false;
    if (eraseOutsideLeft) {
        destination[left - 1] = attrs;
        damageRow(destinationRow);
        if (!selection.empty()) {
            invalidateSelection(Rect(left - 1, destinationRow));
        }
        erasedBoundary = true;
    }
    if (eraseInsideLeft) {
        destination[left] = attrs;
        erasedBoundary = true;
    }
    if (eraseInsideRight) {
        destination[right - 1] = attrs;
        erasedBoundary = true;
    }
    if (eraseOutsideRight) {
        destination[right] = attrs;
        damageRow(destinationRow);
        if (!selection.empty()) {
            invalidateSelection(Rect(right, destinationRow));
        }
        erasedBoundary = true;
    }

    destinationObject->metadata.protection |= sourceObject->metadata.protection;
    if (erasedBoundary) {
        destinationObject->metadata.protection |= attrs.protected_char;
    }
    if (sourceObject->metadata.wide) {
        destinationObject->metadata.wide = true;
    }
    restoreRowWrap(destinationObject, nCols, wrapColumn);
}

template <typename Traits>
void ScreenBase<Traits>::scrollRectangleImpl(u16 top, u16 left, u16 bottom, u16 right, u16 count, const TerminalCell& attrs, bool down) {
    count = min<u16>(count, bottom - top);
    if (count == 0 || right <= left) {
        return;
    }
    if (left != 0 || right != nCols) {
        const i64 rowBase = (i64)(rowEnd)-nRows;
        if (down) {
            for (u16 destination = bottom; destination-- > top + count;) {
                RowSlot& destinationObject = rowRing[wrapRow(rowBase + destination)];
                const Row* const sourceObject = rowRing[wrapRow(rowBase + destination - count)];
                scrollPartialRectangleRow(destinationObject, sourceObject != nullptr ? sourceObject : zeroRow, destination, left, right, attrs);
            }
        } else {
            for (u16 destination = top; destination < bottom - count; ++destination) {
                RowSlot& destinationObject = rowRing[wrapRow(rowBase + destination)];
                const Row* const sourceObject = rowRing[wrapRow(rowBase + destination + count)];
                scrollPartialRectangleRow(destinationObject, sourceObject != nullptr ? sourceObject : zeroRow, destination, left, right, attrs);
            }
        }
        const u16 eraseTop = down ? top : bottom - count;
        const u16 eraseBottom = eraseTop + count;
        const TerminalCell empty{};
        for (u16 row = eraseTop; row < eraseBottom; ++row) {
            RowSlot& object = rowRing[wrapRow(rowBase + row)];
            const u16 wrapColumn = rowWrapColumn(object, nCols);
            if (object != nullptr && object->metadata.wide) {
                clearWideBoundary(object, row, left, attrs);
                clearWideBoundary(object, row, right, attrs);
            }
            if (object != nullptr || attrs != empty) {
                TerminalCell* const cells = mutableRow(object);
                eraseRange(cells + left, cells + right, attrs);
                object->metadata.protection |= attrs.protected_char;
            }
            restoreRowWrap(object, nCols, wrapColumn);
        }
        damageRows(top, bottom);
        if (!selection.empty()) {
            invalidateSelection(Rect(left, top, right, bottom));
        }
        return;
    }
    const u16 width = right - left;
    const auto copy = [&](u16 destinationRow, u16 sourceRow) {
        RowSlot& destinationObject = logicalRowSlot(destinationRow);
        const Row* const sourceObject = getLogicalRowObject(sourceRow);
        const TerminalCell* const source = sourceObject->cells + left;
        clearWideBoundary(destinationObject, destinationRow, left, attrs);
        clearWideBoundary(destinationObject, destinationRow, right, attrs);
        if (destinationObject == nullptr && (left != 0 || sourceObject->metadata.lineAttribute == 0) && (sourceObject == zeroRow || emptyRow(source, width))) {
            return;
        }
        TerminalCell* const destination = mutableRow(destinationObject);
        copyCells(destination + left, source, width);
        if (left == 0) {
            destinationObject->metadata.lineAttribute = sourceObject->metadata.lineAttribute;
        }
        destinationObject->metadata.protection |= sourceObject->metadata.protection;
        if (left == 0 && right == nCols) {
            destinationObject->metadata.protection = rowProtection(destination, nCols);
            destinationObject->metadata.wide = sourceObject->metadata.wide;
            if (destinationObject->metadata.lineAttribute == 0 && destinationObject->metadata.semanticPrompt == ScreenSemanticPrompt::None && emptyRow(destination, nCols)) {
                releaseRow(destinationObject);
                destinationObject = nullptr;
                return;
            }
        } else if (sourceObject->metadata.wide) {
            destinationObject->metadata.wide = true;
        }
        repairWideBoundary(destinationObject, destinationRow, left, attrs);
        repairWideBoundary(destinationObject, destinationRow, right, attrs);
    };
    if (down) {
        for (u16 destination = bottom; destination-- > top + count;) {
            copy(destination, destination - count);
        }
    } else {
        for (u16 destination = top; destination < bottom - count; ++destination) {
            copy(destination, destination + count);
        }
    }
    const u16 eraseTop = down ? top : bottom - count;
    const u16 eraseBottom = eraseTop + count;
    for (u16 row = eraseTop; row < eraseBottom; ++row) {
        RowSlot& object = logicalRowSlot(row);
        clearWideBoundary(object, row, left, attrs);
        clearWideBoundary(object, row, right, attrs);
        if (left == 0 && right == nCols && attrs == TerminalCell{} && (object == nullptr || object->metadata.lineAttribute == 0)) {
            releaseRow(object);
            object = nullptr;
            continue;
        }
        if (object != nullptr || attrs != TerminalCell{}) {
            TerminalCell* const cells = mutableRow(object);
            eraseRange(cells + left, cells + right, attrs);
            object->metadata.protection |= attrs.protected_char;
            if (left == 0 && right == nCols) {
                object->metadata.protection = attrs.protected_char;
                object->metadata.semanticPrompt = ScreenSemanticPrompt::None;
                object->metadata.wide = attrs.dwidth || attrs.dwidth_cont;
            }
        }
    }
    damageRows(top, bottom);
    if (!selection.empty()) {
        invalidateSelection(Rect(left, top, right, bottom));
    }
}

template <typename Traits>
void ScreenBase<Traits>::rotateRows(u16 top, u16 bottom, i32 rows) {
    if (rows == 0) {
        return;
    }
    const bool down = rows > 0;
    u16 count = min<u32>(down ? rows : -(i64)(rows), 0xffff);
    count = min<u16>(count, bottom - top);
    if (!count) {
        return;
    }
    if (!selection.empty()) {
        invalidateSelection(Rect(0, top, nCols, bottom));
    }
    if (down) {
        rotateRowPointersDown(top, bottom, count);
    } else {
        rotateRowPointersUp(top, bottom, count);
    }
    for (u16 row = top; row < bottom; ++row) {
        restoreRowWrap(logicalRowSlot(row), nCols, nCols);
    }
    damageRows(top, bottom);
}

template <typename Traits>
void ScreenBase<Traits>::clearRows(u16 begin, u16 end, const TerminalCell& attrs) {
    for (u16 row = begin; row < end; ++row) {
        RowSlot& slot = logicalRowSlot(row);
        if (attrs == TerminalCell{}) {
            releaseRow(slot);
            slot = nullptr;
            continue;
        }
        TerminalCell* const cells = mutableRow(slot);
        eraseRange(cells, cells + nCols, attrs);
        slot->metadata.lineAttribute = 0;
        slot->metadata.protection = attrs.protected_char;
        slot->metadata.semanticPrompt = ScreenSemanticPrompt::None;
        slot->metadata.wide = attrs.dwidth || attrs.dwidth_cont;
    }
    if (!selection.empty()) {
        invalidateSelection(Rect(0, begin, nCols, end));
    }
}

template <typename Traits>
void ScreenBase<Traits>::rotateRowPointersUp(u16 top, u16 bottom, u16 count) {
    const u16 length = bottom - top;
    count %= length;
    if (count == 0) {
        return;
    }

    const u32 mask = rowCapacity - 1;
    const u32 firstIndex = wrapRow((i64)(rowEnd)-nRows + top);
    if (count == 1) {
        RowSlot first = rowRing[firstIndex];
        u32 destination = firstIndex;
        for (u16 offset = 1; offset < length; ++offset) {
            const u32 source = (destination + 1) & mask;
            rowRing[destination] = rowRing[source];
            destination = source;
        }
        rowRing[destination] = first;
        return;
    }
    if (count == length - 1) {
        u32 source = (firstIndex + length - 1) & mask;
        RowSlot last = rowRing[source];
        for (u16 offset = length - 1; offset != 0; --offset) {
            const u32 destination = source;
            source = (source - 1) & mask;
            rowRing[destination] = rowRing[source];
        }
        rowRing[firstIndex] = last;
        return;
    }

    u16 divisor = count;
    u16 cycles = length;
    while (divisor != 0) {
        const u16 remainder = cycles % divisor;
        cycles = divisor;
        divisor = remainder;
    }

    for (u16 start = 0; start < cycles; ++start) {
        const RowSlot first = logicalRowSlot(top + start);
        u16 current = start;
        while (true) {
            u16 next = current + count;
            if (next >= length) {
                next -= length;
            }
            if (next == start) {
                break;
            }
            logicalRowSlot(top + current) = logicalRowSlot(top + next);
            current = next;
        }
        logicalRowSlot(top + current) = first;
    }
}

template <typename Traits>
void ScreenBase<Traits>::rotateRowPointersDown(u16 top, u16 bottom, u16 count) {
    const u16 length = bottom - top;
    count %= length;
    if (count != 0) {
        rotateRowPointersUp(top, bottom, length - count);
    }
}

template <typename Traits>
void ScreenBase<Traits>::invalidateSelection(const Rect&& damage) {
    if (selection.empty()) {
        return;
    }

    if (selection.rectangular) {
        const bool outsideRows = damage.tl.y > selection.br.y || damage.br.y < selection.tl.y;
        const bool outsideColumns = damage.br.x <= selection.tl.x || selection.br.x <= damage.tl.x;
        if (outsideRows || outsideColumns) {
            return;
        }
        selection.clear();
        return;
    }

    if (selection.br <= damage.tl || damage.br <= selection.tl) {
        return;
    }

    selection.clear();
}

template <typename Traits>
bool ScreenBase<Traits>::selectionValid() const {
    if (selection.null()) {
        return true;
    }

    const int firstRow = -(int)(historyRows);
    const auto valid = [&](Point point) {
        return point.x >= 0 && point.x <= nCols && point.y >= firstRow && point.y < nRows;
    };
    return valid(selection.tl) && valid(selection.br);
}

template <typename Traits>
void ScreenBase<Traits>::vscrollSelection(u16 top, u16 bottom, int vertOffset, bool captureHistory) {
    if (selection.null()) {
        return;
    }

    if (captureHistory) {
        if (selection.tl.y >= bottom) {
            return;
        }
        if (selection.br.y >= bottom) {
            selection.clear();
            return;
        }
        selection.tl.y += vertOffset;
        selection.br.y += vertOffset;
        const int firstRow = -(int)(historyRows);
        if (selection.br.y < firstRow) {
            selection.clear();
        } else if (selection.tl.y < firstRow) {
            selection.tl = Point(0, firstRow);
        }
        return;
    }

    const bool topInside = selection.tl.y >= top && selection.tl.y < bottom;
    const bool bottomInside = selection.br.y >= top && selection.br.y < bottom;
    if (!topInside && !bottomInside) {
        if (selection.br.y < top || selection.tl.y >= bottom) {
            return;
        }
        selection.clear();
        return;
    }
    if (topInside != bottomInside) {
        selection.clear();
        return;
    }

    selection.tl.y += vertOffset;
    selection.br.y += vertOffset;
    if (selection.tl.y < top || selection.br.y >= bottom) {
        selection.clear();
    }
}

template <typename Traits>
const TerminalCell* ScreenBase<Traits>::getLogicalRowPtr(int pY) const {
    return getLogicalRowObject(pY)->cells;
}

template <typename Traits>
const TerminalCell* ScreenBase<Traits>::getViewRowPtr(int pY) const {
    return getLogicalRowPtr(pY - viewOffset);
}

template <typename Traits>
void ScreenBase<Traits>::eraseRange(TerminalCell* start, TerminalCell* end, const TerminalCell& attrs) {
    while (start < end) {
        *start++ = attrs;
    }
}

template <typename Traits>
void ScreenBase<Traits>::copyCells(TerminalCell* destination, const TerminalCell* source, u32 count) {
    memcpy(destination, source, count * cellSize);
}

template <typename Traits>
void ScreenBase<Traits>::moveCells(TerminalCell* destination, const TerminalCell* source, u32 count) {
    memmove(destination, source, count * cellSize);
}

template <typename Traits>
void ScreenBase<Traits>::changeContent() {
    if (++contentRevision == 0) {
        contentRevision = 1;
    }
}

template <typename Traits>
void ScreenBase<Traits>::damageRow(u16 row) {
    changeContent();
    const u32 viewRow = (u32)(row) + viewOffset;
    if (viewRow < nRows) {
        damage.addRow(viewRow);
    }
}

template <typename Traits>
void ScreenBase<Traits>::damageRows(u16 top, u16 bottom) {
    changeContent();
    const u32 viewTop = (u32)(top) + viewOffset;
    const u32 viewBottom = min<u32>((u32)(bottom) + viewOffset, nRows);
    for (u32 row = viewTop; row < viewBottom; ++row) {
        damage.addRow(row);
    }
}

template <typename Traits>
void ScreenBase<Traits>::resizeDamage(u16 rows) {
    damageStorage.grow(rows);
    damage.configure(damageStorage.mutData(), rows);
}

template <typename Traits>
void ScreenBase<Traits>::Damage::configure(void* storage, u16 rows_) {
    height = rows_;
    dirtyRows = 0;
    rows = static_cast<u8*>(storage);
    if (height != 0) {
        memset(rows, 0, height);
    }
}

template <typename Traits>
void ScreenBase<Traits>::Damage::reset() {
    if (dirtyRows != 0 && height != 0) {
        memset(rows, 0, height);
    }
    dirtyRows = 0;
}

template <typename Traits>
void ScreenBase<Traits>::Damage::expose() {
    if (height != 0) {
        memset(rows, 1, height);
    }
    dirtyRows = height;
}

template <typename Traits>
void ScreenBase<Traits>::Damage::addRow(u16 row) {
    if (rows[row] == 0) {
        rows[row] = 1;
        ++dirtyRows;
    }
}
