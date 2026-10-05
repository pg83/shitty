/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "composer.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/screen.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/tst/ut.h>
#include <std/str/view.h>
#include <std/mem/obj_pool.h>

#include <cstring>

using namespace stl;

namespace {
    struct DamageCanvas {
        Vector<TerminalCell> cells;
        Vector<u8> lineAttributes;
        u16 columns = 0;
        u16 rows = 0;
        const TerminalColors* colors = nullptr;
        u32 viewOffset = 0;
        u32 historyRows = 0;
        TerminalCursor cursor;
        Rect selection;
        Rect snappedSelection;
        Color selectionForeground;
        Color selectionBackground;
        u8 selectionColorMask = 0;
        bool screenReverse = false;
        bool blinkVisible = true;
        bool cursorBlink = false;
    };

    static void configureColors(TerminalColors& colors) {
        colors.defaultForeground = {1, 2, 3};
        colors.defaultBackground = {4, 5, 6};
    }

    static TerminalCell attributes() {
        TerminalCell cell{};
        cell.setForeground(CellColor::defaultForeground());
        cell.setBackground(CellColor::defaultBackground());
        return cell;
    }

    static bool equalRect(const Rect& left, const Rect& right) {
        return left.tl == right.tl && left.br == right.br && left.rectangular == right.rectangular;
    }

    static void clearCanvas(DamageCanvas& canvas, u16 columns, u16 rows) {
        const size_t cellCount = (size_t)(columns)*rows;
        canvas.cells.grow(cellCount);
        canvas.lineAttributes.grow(rows);
        memset(canvas.cells.mutData(), 0, cellCount * sizeof(TerminalCell));
        memset(canvas.lineAttributes.mutData(), 0, rows);
        canvas.columns = columns;
        canvas.rows = rows;
        canvas.colors = nullptr;
        canvas.viewOffset = 0;
        canvas.historyRows = 0;
        canvas.cursor = {};
        canvas.selection = {};
        canvas.snappedSelection = {};
        canvas.selectionForeground = {};
        canvas.selectionBackground = {};
        canvas.selectionColorMask = 0;
        canvas.screenReverse = false;
        canvas.blinkVisible = true;
        canvas.cursorBlink = false;
    }

    static TerminalUpdate takeUpdate(Screen& screen, const TerminalColors& colors, Vector<TerminalRow>& rows) {
        const ScreenInfo info = screen.info();
        rows.grow(info.rows);
        const ScreenFrame frame = screen.captureFrame(rows.mutData());
        STD_INSIST(frame.damagedRows <= info.rows);

        u32 previousRow = 0;
        for (size_t index = 0; index < frame.damagedRows; ++index) {
            const TerminalRow& row = rows[index];
            STD_INSIST(row.cells != nullptr);
            STD_INSIST(row.row < info.rows);
            STD_INSIST(index == 0 || row.row > previousRow);
            previousRow = row.row;
            for (u16 column = 0; column < info.columns; ++column) {
                STD_INSIST(row.cells[column] == screen.testCell(row.row, column));
            }
        }

        TerminalUpdate update{};
        update.rows = rows.data();
        update.rowCount = frame.damagedRows;
        update.colors = &colors;
        update.viewOffset = frame.viewOffset;
        update.historyRows = frame.historyRows;
        update.selection = frame.selection;
        update.snappedSelection = frame.snappedSelection;
        return update;
    }

    static bool hasDamage(Screen& screen) {
        const ScreenInfo info = screen.info();
        Vector<TerminalRow> rows;
        rows.grow(info.rows);
        return screen.captureFrame(rows.mutData()).damagedRows != 0;
    }

    static void applyUpdate(DamageCanvas& canvas, const TerminalUpdate& update) {
        STD_INSIST(update.colors != nullptr);
        for (size_t index = 0; index < update.rowCount; ++index) {
            const TerminalRow& row = update.rows[index];
            STD_INSIST(row.row < canvas.rows);
            memcpy(canvas.cells.mutData() + (size_t)(row.row) * canvas.columns, row.cells, (size_t)(canvas.columns) * sizeof(TerminalCell));
            canvas.lineAttributes.mut(row.row) = row.lineAttribute;
        }
        canvas.colors = update.colors;
        canvas.viewOffset = update.viewOffset;
        canvas.historyRows = update.historyRows;
        canvas.cursor = update.cursor;
        canvas.selection = update.selection;
        canvas.snappedSelection = update.snappedSelection;
        canvas.selectionForeground = update.selectionForeground;
        canvas.selectionBackground = update.selectionBackground;
        canvas.selectionColorMask = update.selectionColorMask;
        canvas.screenReverse = update.screenReverse;
        canvas.blinkVisible = update.blinkVisible;
        canvas.cursorBlink = update.cursorBlink;
    }

    static bool equalCanvas(const DamageCanvas& left, const DamageCanvas& right) {
        if (left.columns != right.columns || left.rows != right.rows || left.colors != right.colors || left.viewOffset != right.viewOffset || left.historyRows != right.historyRows) {
            return false;
        }
        if (memcmp(left.cells.data(), right.cells.data(), (size_t)(left.columns) * left.rows * sizeof(TerminalCell)) != 0 || memcmp(left.lineAttributes.data(), right.lineAttributes.data(), left.rows) != 0) {
            return false;
        }
        if (left.cursor.posX != right.cursor.posX || left.cursor.posY != right.cursor.posY || left.cursor.style != right.cursor.style || left.cursor.color != right.cursor.color) {
            return false;
        }
        return equalRect(left.selection, right.selection) && equalRect(left.snappedSelection, right.snappedSelection) && left.selectionForeground == right.selectionForeground && left.selectionBackground == right.selectionBackground && left.selectionColorMask == right.selectionColorMask && left.screenReverse == right.screenReverse && left.blinkVisible == right.blinkVisible && left.cursorBlink == right.cursorBlink;
    }

    static void renderFull(Screen& screen, const TerminalColors& colors, DamageCanvas& canvas) {
        const ScreenInfo info = screen.info();
        clearCanvas(canvas, info.columns, info.rows);
        screen.expose();
        Vector<TerminalRow> rows;
        const TerminalUpdate update = takeUpdate(screen, colors, rows);
        applyUpdate(canvas, update);
        screen.resetDamage();
        STD_INSIST(!hasDamage(screen));
    }

    static void fillDamagePattern(Screen& screen, Composer& composer) {
        const ScreenInfo info = screen.info();
        Vector<u8> text(info.columns);
        for (u16 row = 0; row < info.rows; ++row) {
            for (u16 column = 0; column < info.columns; ++column) {
                text.mut(column) = (u8)(33 + ((u32)(row) * 17 + column) % 90);
            }
            TerminalCell attrs = attributes();
            attrs.setForeground(CellColor::indexed((u8)(row + 1)));
            attrs.setBackground(CellColor::indexed((u8)(row + 17)));
            attrs.bold = row & 1;
            attrs.italic = (row & 2) != 0;
            screen.writeAsciiRun(row, 0, text.data(), info.columns, attrs, 0, row & 3, TerminalCell{});
            screen.setLineAttribute(row, row % 3);
        }

        TerminalCell protectedAttrs = attributes();
        protectedAttrs.protected_char = TerminalCell::isoProtection;
        screen.writeCodepoint(2, 3, 'P', false, protectedAttrs, 0, 1, TerminalCell{});
        screen.writeCodepoint(1, 2, 0x4e00, true, attributes(), 0, 2, TerminalCell{});
        screen.writeCodepoint(3, info.columns - 3, 0x4e01, true, attributes(), 0, 3, TerminalCell{});
        const u32 hyperlink = composer.extras.store->getOrCreateHyperlink(StringView(u8"damage"), StringView(u8"https://damage.test"), 7);
        screen.writeCodepoint(4, 4, 'H', false, attributes(), hyperlink, 2, TerminalCell{});
        screen.setWrapped(0, info.columns - 1);
    }

    static void prepareHistory(Screen& screen) {
        const ScreenInfo info = screen.info();
        screen.scrollRows(0, info.rows, -2, TerminalCell{});
        const u8 first[] = {'n', 'e', 'w', '1'};
        const u8 second[] = {'n', 'e', 'w', '2'};
        screen.writeAsciiRun(info.rows - 2, 0, first, sizeof(first), attributes(), 0, 0, TerminalCell{});
        screen.writeAsciiRun(info.rows - 1, 0, second, sizeof(second), attributes(), 0, 0, TerminalCell{});
    }

    template <typename Setup, typename Operation>
    static void verifyDamageGeometry(u16 columns, u16 rows, Setup setup, Operation operation, bool expectsDamage) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, (size_t)(columns)*rows * 2));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, columns, rows, &colors, 8);
        fillDamagePattern(*screen, composer);
        setup(*screen);

        DamageCanvas incremental;
        renderFull(*screen, colors, incremental);
        operation(*screen);
        STD_INSIST(hasDamage(*screen) == expectsDamage);
        const ScreenInfo info = screen->info();
        if (incremental.columns != info.columns || incremental.rows != info.rows) {
            clearCanvas(incremental, info.columns, info.rows);
        }

        Vector<TerminalRow> damagedRows;
        const TerminalUpdate update = takeUpdate(*screen, colors, damagedRows);
        STD_INSIST((update.rowCount != 0) == expectsDamage);
        applyUpdate(incremental, update);

        DamageCanvas expected;
        renderFull(*screen, colors, expected);
        STD_INSIST(equalCanvas(incremental, expected));
    }

    template <typename Setup, typename Operation>
    static void verifyDamage(Setup setup, Operation operation, bool expectsDamage = true) {
        verifyDamageGeometry(8, 5, setup, operation, expectsDamage);
        verifyDamageGeometry(260, 5, setup, operation, expectsDamage);
    }

    template <typename Operation>
    static void verifyDamage(Operation operation, bool expectsDamage = true) {
        verifyDamage([](Screen&) {}, operation, expectsDamage);
    }

    static void verifyResizeDamageGeometry(u16 columns, bool primary) {
        auto composerPool = ObjPool::fromMemory();
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, (size_t)(columns) * 10));
        TerminalColors colors;
        configureColors(colors);
        Screen* source = primary ? Screen::createPrimary(composer.extras, *sourcePool, columns, 5, &colors, 8) : Screen::createAlternate(composer.extras, *sourcePool, columns, 5, &colors);
        fillDamagePattern(*source, composer);

        DamageCanvas incremental;
        renderFull(*source, colors, incremental);
        Screen::Cursor cursor;
        cursor.position = Point(columns - 2, 3);
        const u16 destinationColumns = primary ? columns - 2 : columns + 2;
        Screen* const destination = source->resized(*destinationPool, destinationColumns, 6, cursor);
        STD_INSIST(hasDamage(*destination));
        const ScreenInfo destinationInfo = destination->info();
        clearCanvas(incremental, destinationInfo.columns, destinationInfo.rows);

        Vector<TerminalRow> damagedRows;
        const TerminalUpdate update = takeUpdate(*destination, colors, damagedRows);
        applyUpdate(incremental, update);

        DamageCanvas expected;
        renderFull(*destination, colors, expected);
        STD_INSIST(equalCanvas(incremental, expected));
    }

    static void verifyResizeDamage(bool primary) {
        verifyResizeDamageGeometry(8, primary);
        verifyResizeDamageGeometry(260, primary);
    }

    struct SlowScreenModel {
        SlowScreenModel(u16 columns_, u16 rows_)
            : columns(columns_)
            , rows(rows_)
        {
            cells.zero((size_t)(columns)*rows);
        }

        TerminalCell& at(u16 row, u16 column) {
            return cells.mut((size_t)(row)*columns + column);
        }

        const TerminalCell& at(u16 row, u16 column) const {
            return cells[(size_t)(row)*columns + column];
        }

        void resize(u16 newColumns, u16 newRows) {
            Vector<TerminalCell> replacement;
            replacement.zero((size_t)(newColumns)*newRows);
            const u16 copiedRows = rows < newRows ? rows : newRows;
            const u16 copiedColumns = columns < newColumns ? columns : newColumns;
            for (u16 row = 0; row < copiedRows; ++row) {
                memcpy(replacement.mutData() + (size_t)(row)*newColumns, cells.data() + (size_t)(row)*columns, (size_t)(copiedColumns) * sizeof(TerminalCell));
            }
            cells.xchg(replacement);
            columns = newColumns;
            rows = newRows;
        }

        void scroll(u16 top, u16 left, u16 bottom, u16 right, i32 amount, const TerminalCell& eraseAttrs) {
            Vector<TerminalCell> previous(cells);
            for (u16 row = top; row < bottom; ++row) {
                const i32 sourceRow = (i32)(row)-amount;
                for (u16 column = left; column < right; ++column) {
                    at(row, column) = sourceRow >= top && sourceRow < bottom ? previous[(size_t)(sourceRow)*columns + column] : eraseAttrs;
                }
            }
        }

        void insistMatches(const Screen& screen) const {
            const ScreenInfo info = screen.info();
            STD_INSIST(info.columns == columns);
            STD_INSIST(info.rows == rows);
            for (u16 row = 0; row < rows; ++row) {
                for (u16 column = 0; column < columns; ++column) {
                    STD_INSIST(screen.testCell(row, column) == at(row, column));
                }
            }
        }

        Vector<TerminalCell> cells;
        u16 columns;
        u16 rows;
    };

    static u32 randomValue(u32& state) {
        state = state * 747796405u + 2891336453u;
        u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    static u16 randomBelow(u32& state, u16 limit) {
        return (u16)(randomValue(state) % limit);
    }
}

STD_TEST_SUITE(Screen) {
    STD_TEST(EmptyRectangleChecksumsToZero) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 4));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 2, 2, &colors);

        STD_INSIST(screen->checksum(0, 0, 0, 0, 0) == 0);
        STD_INSIST(screen->checksum(0, 0, 0, 0, ChecksumKeepBlanks) == 0);
        STD_INSIST(screen->checksum(0, 0, 0, 0, ChecksumPositive) == 0);
    }

    STD_TEST(InitializesGeometryCapacityAndDamage) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 32));
        TerminalColors colors;
        configureColors(colors);

        Screen* screen = Screen::createPrimary(composer.extras, *pool, 4, 3, &colors, 5);
        const ScreenInfo info = screen->info();

        STD_INSIST(info.columns == 4);
        STD_INSIST(info.rows == 3);
        STD_INSIST(info.cellCapacity == 32);
        STD_INSIST(!hasDamage(*screen));

        screen->expose();
        STD_INSIST(hasDamage(*screen));
        screen->resetDamage();
        STD_INSIST(!hasDamage(*screen));
    }

    // R5: row identities come from one process-wide sequence, not one
    // per screen. The window shapes every pane through a single
    // SpanShaper (Composer::shaper), and that shaper caches a shaped row
    // under this value and nothing else - no screen, no pane. Two
    // screens handing out the same number would therefore have one of
    // them drawn from the other's glyphs, silently, and only in the
    // cells where their contents differ.
    //
    // Nothing about a per-screen counter fails to compile, and no test
    // that holds one screen can notice it: the numbers look perfectly
    // well-behaved from inside either screen. Hence this one - two live
    // screens, and no identity shared between them or reused within
    // them. render_arena.h names it as the assumption the Metal arena
    // mirror rests on.
    STD_TEST(RowIdentitiesNeverRepeatAcrossScreens) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 16));
        TerminalColors colors;
        configureColors(colors);

        Screen* const first = Screen::createPrimary(composer.extras, *pool, 4, 2, &colors, 0);
        Screen* const second = Screen::createPrimary(composer.extras, *pool, 4, 2, &colors, 0);

        // Different text on the second pass, so that a row rewritten
        // with the content it already holds cannot be what mints the
        // fresh identity.
        const u8 text[2][4] = {{'a', 'b', 'c', 'd'}, {'e', 'f', 'g', 'h'}};
        u64 ids[8] = {};
        size_t count = 0;
        for (u16 pass = 0; pass < 2; ++pass) {
            for (u16 row = 0; row < 2; ++row) {
                first->writeAsciiRun(row, 0, text[pass], 4, attributes(), 0, 0, TerminalCell{});
                ids[count++] = first->viewRow(row).id;
                second->writeAsciiRun(row, 0, text[pass], 4, attributes(), 0, 0, TerminalCell{});
                ids[count++] = second->viewRow(row).id;
            }
        }

        STD_INSIST(count == 8);
        for (size_t index = 0; index < count; ++index) {
            // Zero is what viewRow() answers for a row that is not
            // there, and an identity is never it.
            STD_INSIST(ids[index] != 0);
            for (size_t other = index + 1; other < count; ++other) {
                STD_INSIST(ids[index] != ids[other]);
            }
        }
    }

    STD_TEST(WritesSixelCellsWithSharedPalette) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 32));
        TerminalColors colors;
        configureColors(colors);

        Screen* screen = Screen::createPrimary(composer.extras, *pool, 4, 3, &colors, 5);
        u8 patches[2 * SixelPatch::pixelCount] = {};
        // First patch paints its top-left pixel; the second stays
        // fully transparent and must not allocate an extra.
        patches[0] = 7;
        u8 palette[SixelPatch::paletteBytes] = {};
        palette[18] = 250;
        const u8* interned = composer.extras.store->internSixelPalette(palette);
        TerminalCell attrs{};
        TerminalCell eraseAttrs{};
        screen->resetDamage();

        screen->writeSixelCells(1, 1, 2, patches, interned, attrs, 0, eraseAttrs);

        const TerminalCell painted = screen->testCell(1, 1);
        const TerminalCell blank = screen->testCell(1, 2);
        STD_INSIST(painted.hasExtra());
        STD_INSIST(!blank.hasExtra());
        const CellExtraView view = composer.extras.store->view(painted);
        STD_INSIST(view.sixelPixels != nullptr);
        STD_INSIST(view.sixelPixels[0] == 7);
        STD_INSIST(view.sixelPalette == interned);
        STD_INSIST(hasDamage(*screen));
    }

    STD_TEST(RevisionTracksVisibleState) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 32));
        TerminalColors colors;
        configureColors(colors);

        Screen* screen = Screen::createPrimary(composer.extras, *pool, 4, 3, &colors, 5);
        const u32 initial = screen->info().revision;
        screen->resetDamage();
        STD_INSIST(screen->info().revision == initial);

        screen->expose();
        const u32 exposed = screen->info().revision;
        STD_INSIST(exposed != initial);
        screen->resetDamage();
        STD_INSIST(screen->info().revision == exposed);

        const u8 text[] = {'x'};
        screen->writeAsciiRun(0, 0, text, 1, attributes(), 0, 0, TerminalCell{});
        const u32 written = screen->info().revision;
        STD_INSIST(written != exposed);

        screen->beginSelection(Point(0, 0));
        STD_INSIST(screen->info().revision != written);
    }

    STD_TEST(KeepsRequestedScrollbackWithinPowerOfTwoRing) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 32));
        TerminalColors colors;
        configureColors(colors);

        Screen* screen = Screen::createPrimary(composer.extras, *pool, 2, 3, &colors, 2);

        STD_INSIST(screen->info().cellCapacity == 10);
        for (u16 index = 0; index < 6; ++index) {
            screen->scrollRows(0, 3, -1, TerminalCell{});
        }
        STD_INSIST(screen->info().historyRows == 2);
    }

    STD_TEST(KeepsZeroScrollbackDisabled) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);

        Screen* screen = Screen::createAlternate(composer.extras, *pool, 2, 3, &colors);
        screen->scrollRows(0, 3, -1, TerminalCell{});

        STD_INSIST(screen->info().cellCapacity == 6);
        STD_INSIST(screen->info().historyRows == 0);
    }

    STD_TEST(AlternateResizeKeepsCursorRowsWithoutHistory) {
        auto composerPool = ObjPool::fromMemory();
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *sourcePool, 1, 4, &colors);
        const u8 text[] = {'A', 'B', 'C', 'D'};
        for (u16 row = 0; row < 4; ++row) {
            screen->writeAsciiRun(row, 0, text + row, 1, attributes(), 0, 0, TerminalCell{});
        }
        Screen::Cursor cursor{Point(0, 3), false};
        Screen::Cursor tracked{Point(0, 2), true};

        Screen* resized = screen->resized(*destinationPool, 1, 3, cursor, &tracked);

        STD_INSIST(resized->testCell(0, 0).uc_pt == 'B');
        STD_INSIST(resized->testCell(1, 0).uc_pt == 'C');
        STD_INSIST(resized->testCell(2, 0).uc_pt == 'D');
        STD_INSIST(cursor.position.y == 2);
        STD_INSIST(tracked.position == Point(0, 1));
        STD_INSIST(!tracked.pendingWrap);
        STD_INSIST(resized->info().historyRows == 0);
    }

    STD_TEST(PrimaryResizeKeepsCursorRowsAndCapturesHistory) {
        auto composerPool = ObjPool::fromMemory();
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *sourcePool, 1, 4, &colors, 4);
        const u8 text[] = {'A', 'B', 'C', 'D'};
        for (u16 row = 0; row < 4; ++row) {
            screen->writeAsciiRun(row, 0, text + row, 1, attributes(), 0, 0, TerminalCell{});
        }
        Screen::Cursor cursor{Point(0, 3), false};

        Screen* resized = screen->resized(*destinationPool, 1, 3, cursor);

        STD_INSIST(resized->testCell(0, 0).uc_pt == 'B');
        STD_INSIST(resized->testCell(1, 0).uc_pt == 'C');
        STD_INSIST(resized->testCell(2, 0).uc_pt == 'D');
        STD_INSIST(cursor.position.y == 2);
        STD_INSIST(resized->info().historyRows == 1);
    }

    STD_TEST(PrimaryReflowTracksAnAdditionalCursor) {
        auto composerPool = ObjPool::fromMemory();
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *sourcePool, 2, 3, &colors, 4);
        const u8 first[] = {'1', 'A'};
        const u8 second[] = {'2', 'B'};
        screen->writeAsciiRun(0, 0, first, 2, attributes(), 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, second, 2, attributes(), 0, 0, TerminalCell{});
        screen->setWrapped(0, 1);
        Screen::Cursor cursor{Point(1, 1), true};
        Screen::Cursor tracked{Point(1, 1), true};

        Screen* resized = screen->resized(*destinationPool, 5, 3, cursor, &tracked);

        STD_INSIST(resized->testCell(0, 0).uc_pt == '1');
        STD_INSIST(resized->testCell(0, 3).uc_pt == 'B');
        STD_INSIST(tracked.position == Point(4, 0));
        STD_INSIST(!tracked.pendingWrap);
    }

    STD_TEST(WritesAsciiAndExposesOnlyDamagedCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 2, &colors);
        TerminalCell attrs = attributes();
        attrs.bold = true;
        const u8 text[] = {'a', 'b'};
        TerminalRow rows[2];

        screen->expose();
        size_t count = screen->captureFrame(rows).damagedRows;
        STD_INSIST(count == 2);
        STD_INSIST(rows[0].row == 0);
        STD_INSIST(rows[1].row == 1);
        screen->resetDamage();
        screen->writeAsciiRun(1, 1, text, 2, attrs, 0, 3, TerminalCell{});
        count = screen->captureFrame(rows).damagedRows;

        STD_INSIST(count == 1);
        STD_INSIST(rows[0].row == 1);
        STD_INSIST(rows[0].cells[1].uc_pt == 'a');
        STD_INSIST(rows[0].cells[2].uc_pt == 'b');
        STD_INSIST(rows[0].cells[1].bold);
        STD_INSIST(rows[0].cells[1].semantic == 3);
    }

    STD_TEST(DamagedRowReportsWholly) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 8, 2, &colors);
        const TerminalCell attrs = attributes();
        const u8 left[] = {'L'};
        const u8 right[] = {'R'};
        TerminalRow rows[2];

        screen->writeAsciiRun(1, 6, right, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 1, left, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 6, right, 1, attrs, 0, 0, TerminalCell{});

        size_t count = screen->captureFrame(rows).damagedRows;
        STD_INSIST(count == 1);
        STD_INSIST(rows[0].row == 1);
        STD_INSIST(rows[0].cells[1].uc_pt == 'L');
        STD_INSIST(rows[0].cells[6].uc_pt == 'R');

        screen->resetDamage();
        screen->writeAsciiRun(1, 3, left, 1, attrs, 0, 0, TerminalCell{});
        count = screen->captureFrame(rows).damagedRows;
        STD_INSIST(count == 1);
        STD_INSIST(rows[0].row == 1);
        STD_INSIST(rows[0].cells[3].uc_pt == 'L');
    }

    STD_TEST(WritesAsciiLinesAndRecyclesFullHistory) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 16));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, 3, 2, &colors, 2);
        const TerminalCell attrs = attributes();
        const u8 text[] = {'A', 'B', '\r', '\n', 'C', '\r', '\n', 'D', 'E', '\r', '\n', 'F', '\r', '\n'};
        const u16 lengths[] = {2, 1, 2, 1};

        screen->writeAsciiLines(0, text, lengths, 4, attrs, 0, 0, TerminalCell{});

        STD_INSIST(screen->info().historyRows == 2);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'F');
        STD_INSIST(screen->testCell(0, 1).uc_pt == 0);
        STD_INSIST(screen->testCell(1, 0).uc_pt == 0);
        screen->scrollView(2);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'C');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'D');
        STD_INSIST(screen->testCell(1, 1).uc_pt == 'E');
    }

    STD_TEST(WritesAsciiLinesIntoClearedRowsWithEraseAttributes) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 16));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 2, &colors);
        const TerminalCell attrs = attributes();
        TerminalCell eraseAttrs{};
        eraseAttrs.bold = true;
        const u8 text[] = {'A', '\r', '\n', 'B', '\r', '\n', 'C', '\r', '\n'};
        const u16 lengths[] = {1, 1, 1};

        screen->writeAsciiLines(0, text, lengths, 3, attrs, 0, 0, eraseAttrs);

        STD_INSIST(screen->testCell(0, 0).uc_pt == 'C');
        STD_INSIST(!screen->testCell(0, 0).bold);
        STD_INSIST(screen->testCell(0, 1).uc_pt == 0);
        STD_INSIST(screen->testCell(0, 1).bold);
        STD_INSIST(screen->testCell(1, 0).uc_pt == 0);
        STD_INSIST(screen->testCell(1, 0).bold);
    }

    STD_TEST(WritesAsciiLinesWithoutTouchingOtherRows) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 16));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 3, 4, &colors);
        const TerminalCell attrs = attributes();
        const u8 original[] = {'x', 'y', 'z'};
        const u8 text[] = {'A', '\r', '\n', 'B', '\r', '\n'};
        const u16 lengths[] = {1, 1};
        screen->writeAsciiRun(3, 0, original, 3, attrs, 0, 0, TerminalCell{});
        screen->resetDamage();

        screen->writeAsciiLines(0, text, lengths, 2, attrs, 0, 0, TerminalCell{});

        TerminalRow rows[4];
        const size_t count = screen->captureFrame(rows).damagedRows;
        STD_INSIST(count == 2);
        STD_INSIST(rows[0].row == 0);
        STD_INSIST(rows[1].row == 1);
        STD_INSIST(screen->testCell(2, 0).uc_pt == 0);
        STD_INSIST(screen->testCell(3, 0).uc_pt == 'x');
        STD_INSIST(screen->testCell(3, 2).uc_pt == 'z');
    }

    STD_TEST(StoresLineAttributesInRowMetadata) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 4));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 1, &colors);
        TerminalRow rows[1];

        screen->setLineAttribute(0, 2);
        STD_INSIST(screen->lineAttribute(0) == 2);
        const size_t count = screen->captureFrame(rows).damagedRows;
        STD_INSIST(count == 1);
        STD_INSIST(rows[0].lineAttribute == 2);
        screen->setLineAttribute(0, 0);
        STD_INSIST(screen->lineAttribute(0) == 0);
    }

    STD_TEST(BlankCaptureUsesSparseRows) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 30));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 10, 3, &colors);
        TerminalRow rows[3];
        STD_INSIST(screen->materializedRows() == 0);

        screen->expose();
        const size_t count = screen->captureFrame(rows).damagedRows;

        STD_INSIST(count == 3);
        STD_INSIST(screen->materializedRows() == 0);
        for (u16 row = 0; row < 3; ++row) {
            STD_INSIST(rows[row].row == row);
            for (u16 column = 0; column < 10; ++column) {
                STD_INSIST(rows[row].cells[column] == TerminalCell{});
            }
        }
    }

    STD_TEST(LargeBlankHistoryStaysSparseAcrossResize) {
        auto composerPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 80 * 24));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *sourcePool, 80, 24, &colors, 50000);
        Screen::Cursor cursor{};
        STD_INSIST(screen->materializedRows() == 0);

        screen = screen->resized(*destinationPool, 132, 43, cursor);

        STD_INSIST(screen->materializedRows() == 0);
        STD_INSIST(screen->info().historyRows == 0);
        STD_INSIST(screen->info().columns == 132);
        STD_INSIST(screen->info().rows == 43);
    }

    STD_TEST(NonNormalLineIsClippedInsteadOfReflowed) {
        auto composerPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 6));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *sourcePool, 3, 2, &colors, 4);
        const u8 first[] = {'A', 'B', 'C'};
        const u8 second[] = {'D', 'E', 'F'};
        screen->writeAsciiRun(0, 0, first, 3, attributes(), 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, second, 3, attributes(), 0, 0, TerminalCell{});
        screen->setLineAttribute(0, 1);
        Screen::Cursor cursor{Point(2, 1), false};

        screen = screen->resized(*destinationPool, 2, 2, cursor);

        STD_INSIST(screen->info().historyRows == 1);
        STD_INSIST(screen->testLogicalCell(-1, 0).uc_pt == 'A');
        STD_INSIST(screen->testLogicalCell(-1, 1).uc_pt == 'B');
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'D');
        STD_INSIST(screen->testCell(0, 1).uc_pt == 'E');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'F');
    }

    STD_TEST(TracksProtectedCellsInRowMetadata) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 4));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 1, &colors);
        TerminalCell attrs = attributes();
        attrs.protected_char = TerminalCell::isoProtection;
        const u8 text[] = {'x'};

        screen->writeAsciiRun(0, 1, text, 1, attrs, 0, 0, TerminalCell{});
        STD_INSIST(screen->hasProtection(0, TerminalCell::isoProtection));
        screen->selectiveEraseCells(0, 0, 4, TerminalCell{}, TerminalCell::isoProtection);
        STD_INSIST(screen->testCell(0, 1).uc_pt == 'x');
        screen->eraseCells(0, 0, 4, TerminalCell{});
        STD_INSIST(!screen->hasProtection(0, TerminalCell::isoProtection));
    }

    STD_TEST(ScrollsPartialRectanglesAsOneOperation) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 15));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 5, 3, &colors);
        const TerminalCell attrs = attributes();
        const u8 first[] = {'A', 'B', 'C', 'D', 'E'};
        const u8 second[] = {'F', 'G', 'H', 'I', 'J'};
        const u8 third[] = {'K', 'L', 'M', 'N', 'O'};
        screen->writeAsciiRun(0, 0, first, 5, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, second, 5, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(2, 0, third, 5, attrs, 0, 0, TerminalCell{});

        screen->scrollRectangle(0, 1, 3, 4, -1, TerminalCell{});

        STD_INSIST(screen->testCell(0, 0).uc_pt == 'A');
        STD_INSIST(screen->testCell(0, 1).uc_pt == 'G');
        STD_INSIST(screen->testCell(0, 3).uc_pt == 'I');
        STD_INSIST(screen->testCell(0, 4).uc_pt == 'E');
        STD_INSIST(screen->testCell(1, 1).uc_pt == 'L');
        STD_INSIST(screen->testCell(1, 3).uc_pt == 'N');
        STD_INSIST(screen->testCell(2, 0).uc_pt == 'K');
        STD_INSIST(screen->testCell(2, 1).uc_pt == 0);
        STD_INSIST(screen->testCell(2, 3).uc_pt == 0);
        STD_INSIST(screen->testCell(2, 4).uc_pt == 'O');
    }

    STD_TEST(PartialRectangleScrollPreservesBlankCellAttributes) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 20));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 10, 4, &colors);
        TerminalCell red{};
        red.setBackground(CellColor::indexed(1));
        TerminalCell blue{};
        blue.setBackground(CellColor::indexed(4));
        screen->eraseCells(1, 0, 10, red);
        screen->eraseCells(2, 0, 10, blue);

        screen->scrollRectangle(1, 2, 3, 8, -1, TerminalCell{});

        for (u16 column = 0; column < 10; ++column) {
            const TerminalCell expected = column >= 2 && column < 8 ? blue : red;
            STD_INSIST(screen->testCell(1, column) == expected);
        }
        for (u16 column = 2; column < 8; ++column) {
            STD_INSIST(screen->testCell(2, column) == TerminalCell{});
        }
    }

    STD_TEST(PartialRectangleScrollDownPreservesBlankCellAttributes) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 20));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 10, 4, &colors);
        TerminalCell red{};
        red.setBackground(CellColor::indexed(1));
        TerminalCell blue{};
        blue.setBackground(CellColor::indexed(4));
        screen->eraseCells(1, 0, 10, red);
        screen->eraseCells(2, 0, 10, blue);

        screen->scrollRectangle(1, 2, 3, 8, 1, TerminalCell{});

        for (u16 column = 0; column < 10; ++column) {
            const TerminalCell expected = column >= 2 && column < 8 ? red : blue;
            STD_INSIST(screen->testCell(2, column) == expected);
        }
        for (u16 column = 2; column < 8; ++column) {
            STD_INSIST(screen->testCell(1, column) == TerminalCell{});
        }
    }

    STD_TEST(PartialRectangleScrollKeepsMatchingBlankRowsSparse) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 20));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 10, 4, &colors);
        STD_INSIST(screen->materializedRows() == 0);

        screen->scrollRectangle(1, 2, 3, 8, -1, TerminalCell{});

        STD_INSIST(screen->materializedRows() == 0);
    }

    STD_TEST(PartialScrollUpClearsWideGlyphsAtBothBoundaries) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 14));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 7, 2, &colors);
        const TerminalCell attrs = attributes();
        constexpr u32 wide = 0x4e00;
        const u8 middle[] = {'x'};

        screen->writeCodepoint(0, 1, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(0, 4, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(1, 1, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(1, 4, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 3, middle, 1, attrs, 0, 0, TerminalCell{});

        screen->scrollRectangle(0, 2, 2, 5, -1, TerminalCell{});

        STD_INSIST(screen->testCell(0, 1) == TerminalCell{});
        STD_INSIST(screen->testCell(0, 2) == TerminalCell{});
        STD_INSIST(screen->testCell(0, 3).uc_pt == 'x');
        STD_INSIST(screen->testCell(0, 4) == TerminalCell{});
        STD_INSIST(screen->testCell(0, 5) == TerminalCell{});
    }

    STD_TEST(PartialScrollDownClearsWideGlyphsAtBothBoundaries) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 14));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 7, 2, &colors);
        const TerminalCell attrs = attributes();
        constexpr u32 wide = 0x4e00;
        const u8 middle[] = {'x'};

        screen->writeCodepoint(0, 1, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(0, 4, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(0, 3, middle, 1, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(1, 1, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(1, 4, wide, true, attrs, 0, 0, TerminalCell{});

        screen->scrollRectangle(0, 2, 2, 5, 1, TerminalCell{});

        STD_INSIST(screen->testCell(1, 1) == TerminalCell{});
        STD_INSIST(screen->testCell(1, 2) == TerminalCell{});
        STD_INSIST(screen->testCell(1, 3).uc_pt == 'x');
        STD_INSIST(screen->testCell(1, 4) == TerminalCell{});
        STD_INSIST(screen->testCell(1, 5) == TerminalCell{});
    }

    STD_TEST(PartialScrollCarriesProtectionWhileRepairingWideSourceEdges) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 14));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 7, 2, &colors);
        const TerminalCell attrs = attributes();
        TerminalCell protectedAttrs = attrs;
        protectedAttrs.protected_char = TerminalCell::isoProtection;
        constexpr u32 wide = 0x4e00;

        // Both copied rectangle edges cut through a wide source cell. The
        // middle protected cell must survive the same copy and keep the row
        // metadata useful to the selective-erase fast path.
        screen->writeCodepoint(1, 1, wide, true, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(1, 3, 'P', false, protectedAttrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(1, 4, wide, true, attrs, 0, 0, TerminalCell{});

        screen->scrollRectangle(0, 2, 2, 5, -1, TerminalCell{});

        STD_INSIST(screen->testCell(0, 2) == TerminalCell{});
        STD_INSIST(screen->testCell(0, 3).uc_pt == 'P');
        STD_INSIST(screen->testCell(0, 3).protected_char == TerminalCell::isoProtection);
        STD_INSIST(screen->testCell(0, 4) == TerminalCell{});
        STD_INSIST(screen->hasProtection(0, TerminalCell::isoProtection));
        screen->selectiveEraseCells(0, 0, 7, TerminalCell{}, TerminalCell::isoProtection);
        STD_INSIST(screen->testCell(0, 3).uc_pt == 'P');
    }

    STD_TEST(RotatesMultipleRowsInOnePass) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 5));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 1, 5, &colors);
        const TerminalCell attrs = attributes();
        for (u16 row = 0; row < 5; ++row) {
            const u8 value = (u8)('A' + row);
            screen->writeAsciiRun(row, 0, &value, 1, attrs, 0, 0, TerminalCell{});
        }

        screen->rotateRows(0, 5, -2);
        for (u16 row = 0; row < 5; ++row) {
            STD_INSIST(screen->testCell(row, 0).uc_pt == (u32)("CDEAB"[row]));
        }

        screen->rotateRows(0, 5, 2);
        for (u16 row = 0; row < 5; ++row) {
            STD_INSIST(screen->testCell(row, 0).uc_pt == (u32)("ABCDE"[row]));
        }
    }

    STD_TEST(InsertsAsciiRunsWithOneRowShift) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 5));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 5, 1, &colors);
        const TerminalCell attrs = attributes();
        const u8 initial[] = {'a', 'b', 'c', 'd', 'e'};
        const u8 inserted[] = {'X', 'Y'};
        screen->writeAsciiRun(0, 0, initial, 5, attrs, 0, 0, TerminalCell{});

        screen->writeAsciiRunInsert(0, 1, 5, inserted, 2, attrs, 0, 0, TerminalCell{});

        const u8 expected[] = {'a', 'X', 'Y', 'b', 'c'};
        for (u16 column = 0; column < 5; ++column) {
            STD_INSIST(screen->testCell(0, column).uc_pt == expected[column]);
        }
    }

    STD_TEST(OverwritingWideContinuationClearsItsLead) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 4));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 1, &colors);
        const TerminalCell attrs = attributes();
        constexpr u32 wide = 0x4e00;
        const u8 replacement[] = {'x'};

        screen->writeCodepoint(0, 1, wide, true, attrs, 0, 0, TerminalCell{});
        STD_INSIST(screen->testCell(0, 1).dwidth);
        STD_INSIST(screen->testCell(0, 2).dwidth_cont);

        screen->writeAsciiRun(0, 2, replacement, 1, attrs, 0, 0, TerminalCell{});

        STD_INSIST(!screen->testCell(0, 1).dwidth);
        STD_INSIST(screen->testCell(0, 1).uc_pt == 0);
        STD_INSIST(!screen->testCell(0, 2).dwidth_cont);
        STD_INSIST(screen->testCell(0, 2).uc_pt == 'x');
    }

    STD_TEST(InsertAndDeleteCellsPreserveWideGlyph) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 16));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 8, 1, &colors);
        const TerminalCell attrs = attributes();
        constexpr u32 wide = 0x4e00;
        const u8 text[] = {'a', 'b'};
        screen->writeAsciiRun(0, 0, text, 2, attrs, 0, 0, TerminalCell{});
        screen->writeCodepoint(0, 2, wide, true, attrs, 0, 0, TerminalCell{});

        screen->insertCells(0, 1, 8, 1, TerminalCell{});

        STD_INSIST(screen->testCell(0, 0).uc_pt == 'a');
        STD_INSIST(screen->testCell(0, 1).uc_pt == 0);
        STD_INSIST(screen->testCell(0, 2).uc_pt == 'b');
        STD_INSIST(screen->testCell(0, 3).dwidth);
        STD_INSIST(screen->testCell(0, 4).dwidth_cont);

        screen->deleteCells(0, 1, 8, 1, TerminalCell{});

        STD_INSIST(screen->testCell(0, 1).uc_pt == 'b');
        STD_INSIST(screen->testCell(0, 2).dwidth);
        STD_INSIST(screen->testCell(0, 3).dwidth_cont);
    }

    STD_TEST(InsertInsideWideGlyphRemovesBothHalves) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 1, &colors);
        const TerminalCell attrs = attributes();
        constexpr u32 wide = 0x4e00;
        screen->writeCodepoint(0, 1, wide, true, attrs, 0, 0, TerminalCell{});

        screen->insertCells(0, 2, 4, 1, TerminalCell{});

        for (u16 column = 0; column < 4; ++column) {
            const TerminalCell cell = screen->testCell(0, column);
            STD_INSIST(!cell.dwidth);
            STD_INSIST(!cell.dwidth_cont);
        }
    }

    STD_TEST(ScrollbackRetainsRowsAndChangesView) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, 2, 3, &colors, 1);
        const TerminalCell attrs = attributes();
        const u8 first[] = {'A'};
        const u8 second[] = {'B'};
        const u8 third[] = {'C'};
        screen->writeAsciiRun(0, 0, first, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, second, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(2, 0, third, 1, attrs, 0, 0, TerminalCell{});

        screen->scrollRows(0, 3, -1, TerminalCell{});

        STD_INSIST(screen->info().historyRows == 1);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'B');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'C');

        screen->scrollView(1);
        STD_INSIST(screen->info().viewOffset == 1);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'A');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'B');
        STD_INSIST(screen->testCell(2, 0).uc_pt == 'C');

        STD_INSIST(screen->scrollView(-0x7fffffff));
        STD_INSIST(screen->info().viewOffset == 0);
        STD_INSIST(!screen->scrollView(-0x7fffffff));
    }

    STD_TEST(FullHistoryRingKeepsNewestRowsAndRestoresThem) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, 2, 2, &colors, 2);
        const TerminalCell attrs = attributes();
        const u8 first[] = {'A'};
        const u8 second[] = {'B'};
        const u8 third[] = {'C'};
        const u8 fourth[] = {'D'};
        const u8 fifth[] = {'E'};
        screen->writeAsciiRun(0, 0, first, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, second, 1, attrs, 0, 0, TerminalCell{});

        screen->scrollRows(0, 2, -1, TerminalCell{});
        screen->eraseCells(1, 0, 2, TerminalCell{});
        screen->writeAsciiRun(1, 0, third, 1, attrs, 0, 0, TerminalCell{});
        screen->scrollRows(0, 2, -1, TerminalCell{});
        screen->eraseCells(1, 0, 2, TerminalCell{});
        screen->writeAsciiRun(1, 0, fourth, 1, attrs, 0, 0, TerminalCell{});
        screen->scrollRows(0, 2, -1, TerminalCell{});
        screen->eraseCells(1, 0, 2, TerminalCell{});
        screen->writeAsciiRun(1, 0, fifth, 1, attrs, 0, 0, TerminalCell{});

        STD_INSIST(screen->info().historyRows == 2);
        screen->scrollView(2);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'B');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'C');
        STD_INSIST(screen->scrollView(-0x7fffffff));
        STD_INSIST(screen->info().historyRows == 2);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'D');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'E');
    }

    STD_TEST(TopAnchoredPartialScrollPreservesRowsBelowRegion) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, 2, 4, &colors, 2);
        const TerminalCell attrs = attributes();
        const u8 first[] = {'A'};
        const u8 second[] = {'B'};
        const u8 third[] = {'C'};
        const u8 fourth[] = {'D'};
        screen->writeAsciiRun(0, 0, first, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, second, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(2, 0, third, 1, attrs, 0, 0, TerminalCell{});
        screen->writeAsciiRun(3, 0, fourth, 1, attrs, 0, 0, TerminalCell{});

        screen->scrollRows(0, 3, -1, TerminalCell{});
        screen->eraseCells(2, 0, 2, TerminalCell{});

        STD_INSIST(screen->info().historyRows == 1);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'B');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'C');
        STD_INSIST(screen->testCell(2, 0).uc_pt == 0);
        STD_INSIST(screen->testCell(3, 0).uc_pt == 'D');
        screen->scrollView(1);
        STD_INSIST(screen->testCell(0, 0).uc_pt == 'A');
        STD_INSIST(screen->testCell(1, 0).uc_pt == 'B');
        STD_INSIST(screen->testCell(2, 0).uc_pt == 'C');
    }

    STD_TEST(ReturnsExplicitAndDetectedHyperlinks) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 64));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 32, 2, &colors);
        const TerminalCell attrs = attributes();
        const u32 link = composer.extras.store->getOrCreateHyperlink(StringView(u8"id"), StringView(u8"https://explicit.test"), 17);
        const u8 explicitText[] = {'x'};
        const u8 detected[] = {'s', 'e', 'e', ' ', 'h', 't', 't', 'p', 's', ':', '/', '/', 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 't', 'e', 's', 't', ',', ' ', 'n', 'o', 'w'};
        screen->writeAsciiRun(0, 0, explicitText, 1, attrs, link, 0, TerminalCell{});
        screen->writeAsciiRun(1, 0, detected, sizeof(detected), attrs, 0, 0, TerminalCell{});

        const ScreenHyperlink explicitLink = screen->hyperlinkAt(0, 0);
        const ScreenHyperlink detectedLink = screen->hyperlinkAt(1, 8);

        STD_INSIST(explicitLink.displayId == 17);
        STD_INSIST(explicitLink.payload == StringView(u8"https://explicit.test"));
        STD_INSIST(detectedLink.displayId == 0);
        STD_INSIST(detectedLink.payload == StringView(u8"https://example.test"));
        STD_INSIST(detectedLink.scheme == StringView(u8"https"));
        STD_INSIST(detectedLink.begin == 36);
        STD_INSIST(detectedLink.end == 56);
    }

    STD_TEST(CollectsSentinelEncodedExtraCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 16));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 4, 1, &colors);
        TerminalCell ordinary = attributes();
        ordinary.setInlineUnderlineColor(CellColor::direct({TerminalCell::extraRefSentinel, 1, 2}));
        const u8 filler[] = {'a', 'b', 'c', 'd'};
        screen->writeAsciiRun(0, 0, filler, 4, ordinary, 0, 0, TerminalCell{});
        const u32 link = composer.extras.store->getOrCreateHyperlink(StringView(u8"id"), StringView(u8"https://sentinel.test"), 1);
        const u8 first[] = {'x'};
        const u8 last[] = {'y'};
        screen->writeAsciiRun(0, 0, first, 1, attributes(), link, 0, TerminalCell{});
        screen->writeAsciiRun(0, 3, last, 1, attributes(), link, 0, TerminalCell{});
        Vector<TerminalCell*> cells;

        screen->collectExtraCells(cells);

        STD_INSIST(cells.length() == 2);
        STD_INSIST(cells[0]->uc_pt == 'x');
        STD_INSIST(cells[1]->uc_pt == 'y');
        STD_INSIST(cells[0]->extraRef() == link);
        STD_INSIST(cells[1]->extraRef() == link);
    }

    STD_TEST(ResizeTransfersContentToReplacement) {
        auto composerPool = ObjPool::fromMemory();
        auto sourcePool = ObjPool::fromMemory();
        auto destinationPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 8));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *sourcePool, 4, 2, &colors);
        const TerminalCell attrs = attributes();
        const u8 text[] = {'a', 'b', 'c'};
        screen->writeAsciiRun(0, 0, text, 3, attrs, 0, 0, TerminalCell{});

        Screen::Cursor cursor;
        Screen* replacement = screen->resized(*destinationPool, 4, 2, cursor);

        STD_INSIST(replacement->testCell(0, 0).uc_pt == 'a');
        STD_INSIST(replacement->testCell(0, 1).uc_pt == 'b');
        STD_INSIST(replacement->testCell(0, 2).uc_pt == 'c');
    }

    STD_TEST(FindsBlinkingTextInVisibleCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 4));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *pool, 2, 2, &colors);
        TerminalCell attrs = attributes();

        STD_INSIST(!screen->hasBlinkingText());
        attrs.blink = true;
        screen->writeCodepoint(1, 1, 'x', false, attrs, 0, 0, TerminalCell{});
        STD_INSIST(screen->hasBlinkingText());
        screen->eraseCells(1, 1, 1, TerminalCell{});
        STD_INSIST(!screen->hasBlinkingText());
    }

    STD_TEST(FillCellsProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            TerminalCell attrs = attributes();
            attrs.bold = true;
            screen.fillCells('Z', attrs);
        });
    }

    STD_TEST(SetLineAttributeProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.setLineAttribute(2, 7);
        });
    }

    STD_TEST(SetWrappedProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.setWrapped(2, 6);
        });
    }

    STD_TEST(WriteCodepointProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.writeCodepoint(1, 3, 'x', false, attributes(), 0, 1, TerminalCell{});
        });
    }

    STD_TEST(WriteWideCodepointProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.writeCodepoint(2, 5, 0x4e02, true, attributes(), 0, 2, TerminalCell{});
        });
    }

    STD_TEST(WriteGraphemeProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            const u32 grapheme[] = {'e', 0x301};
            screen.writeGrapheme(3, 1, grapheme, 2, false, attributes(), 0, 3, TerminalCell{});
        });
    }

    STD_TEST(WriteAsciiRunProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            const u8 text[] = {'r', 'u', 'n', '!'};
            screen.writeAsciiRun(1, 1, text, sizeof(text), attributes(), 0, 1, TerminalCell{});
        });
    }

    STD_TEST(WriteAsciiLinesProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            const u8 text[] = {'a', 'b', '\r', '\n', 'c', '\r', '\n'};
            const u16 lengths[] = {2, 1};
            screen.writeAsciiLines(1, text, lengths, 2, attributes(), 0, 2, TerminalCell{});
        });
    }

    STD_TEST(WriteAsciiLinesWhileScrolledProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            prepareHistory(screen);
            screen.scrollView(2);
        }, [](Screen& screen) {
            const u8 text[] = {'a', 'b', '\r', '\n', 'c', '\r', '\n'};
            const u16 lengths[] = {2, 1};
            screen.writeAsciiLines(1, text, lengths, 2, attributes(), 0, 2, TerminalCell{});
        });
    }

    STD_TEST(WriteAsciiRunInsertProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            const u8 text[] = {'i', 'n', 's'};
            screen.writeAsciiRunInsert(1, 1, screen.info().columns, text, sizeof(text), attributes(), 0, 1, TerminalCell{});
        });
    }

    STD_TEST(WriteRunProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            const u32 text[] = {'R', 0x4e03, 'N'};
            screen.writeRun(2, 1, text, 3, attributes(), 0, 1, TerminalCell{});
        });
    }

    STD_TEST(WriteRepeatedCodepointProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.writeRepeatedCodepoint(2, 1, 4, 'R', attributes(), 0, 1, TerminalCell{});
        });
    }

    STD_TEST(WriteGlyphRunProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            const u32 text[] = {'G', 0x4e04, 'R'};
            const u8 widths[] = {1, 2, 1};
            screen.writeGlyphRun(2, 1, text, widths, 3, 4, attributes(), 0, 1, TerminalCell{});
        });
    }

    STD_TEST(FillRectangleProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.fillRectangle(1, 1, 4, screen.info().columns - 1, 'F', attributes(), TerminalCell{});
        });
    }

    STD_TEST(CopyRectangleProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.copyRectangle(0, 1, 2, 2, 2, 4, TerminalCell{});
        });
    }

    STD_TEST(ChangeRectangleAttributesProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            CellAttributeChange change;
            change.toggle(CellAttributeChange::Bold | CellAttributeChange::Inverse);
            screen.changeRectangleAttributes(1, 1, 4, screen.info().columns - 1, change);
        });
    }

    STD_TEST(ChangeRectangleAttributesAppliesFullSgrState) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 12));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, 4, 3, &colors, 0);
        TerminalCell initial = attributes();
        initial.faint = true;
        initial.setInlineUnderlineColor(CellColor::indexed(6));
        const u8 text[] = {'a', 'b', 'c', 'd'};
        for (u16 row = 0; row < 3; ++row) {
            screen->writeAsciiRun(row, 0, text, sizeof(text), initial, 0, 0, TerminalCell{});
        }

        CellAttributeChange change;
        change.set(CellAttributeChange::Bold | CellAttributeChange::Italic | CellAttributeChange::Strike | CellAttributeChange::Overline, true);
        change.set(CellAttributeChange::Faint, false);
        change.setUnderline(3);
        change.setForeground(CellColor::indexed(10));
        change.setBackground(CellColor::direct({1, 2, 3}));
        change.setUnderlineColor(CellColor::direct({4, 5, 6}));
        screen->changeRectangleAttributes(0, 0, 3, 4, change);

        for (u16 row = 0; row < 3; ++row) {
            for (u16 column = 0; column < 4; ++column) {
                const TerminalCell& cell = screen->testCell(row, column);
                STD_INSIST(cell.bold);
                STD_INSIST(!cell.faint);
                STD_INSIST(cell.italic);
                STD_INSIST(cell.underline_style == 3);
                STD_INSIST(cell.strike);
                STD_INSIST(cell.overline);
                STD_INSIST(cell.foreground() == CellColor::indexed(10));
                STD_INSIST(cell.background() == CellColor::direct({1, 2, 3}));
                STD_INSIST(composer.extras.store->underlineColor(cell) == CellColor::direct({4, 5, 6}));
            }
        }
    }

    STD_TEST(ChangeRectangleAttributesCanResetEveryVisualAttribute) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 1));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createPrimary(composer.extras, *pool, 1, 1, &colors, 0);
        TerminalCell initial = attributes();
        initial.bold = true;
        initial.faint = true;
        initial.italic = true;
        initial.underline_style = 5;
        initial.blink = true;
        initial.inverse = true;
        initial.conceal = true;
        initial.strike = true;
        initial.overline = true;
        initial.setForeground(CellColor::indexed(3));
        initial.setBackground(CellColor::indexed(4));
        initial.setInlineUnderlineColor(CellColor::indexed(5));
        screen->writeCodepoint(0, 0, 'x', false, initial, 0, 0, TerminalCell{});

        CellAttributeChange change;
        constexpr u16 allAttributes = CellAttributeChange::Bold | CellAttributeChange::Faint | CellAttributeChange::Italic | CellAttributeChange::Underline | CellAttributeChange::Blink | CellAttributeChange::Inverse | CellAttributeChange::Conceal | CellAttributeChange::Strike | CellAttributeChange::Overline;
        change.set(allAttributes, false);
        change.setUnderline(0);
        change.setForeground(CellColor::defaultForeground());
        change.setBackground(CellColor::defaultBackground());
        change.setUnderlineFromForeground();
        screen->changeRectangleAttributes(0, 0, 1, 1, change);

        const TerminalCell& cell = screen->testCell(0, 0);
        STD_INSIST(!(cell.bold || cell.faint || cell.italic || cell.underline_style || cell.blink || cell.inverse || cell.conceal || cell.strike || cell.overline));
        STD_INSIST(cell.foreground() == CellColor::defaultForeground());
        STD_INSIST(cell.background() == CellColor::defaultBackground());
        STD_INSIST(composer.extras.store->underlineColor(cell) == CellColor::defaultForeground());
    }

    STD_TEST(CollectExtraCellsProducesValidIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            Vector<TerminalCell*> cells;
            screen.collectExtraCells(cells);
        });
    }

    STD_TEST(EraseCellsProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.eraseCells(1, 2, 4, TerminalCell{});
        });
    }

    STD_TEST(SelectiveEraseCellsProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.selectiveEraseCells(2, 1, 5, TerminalCell{}, TerminalCell::isoProtection);
        });
    }

    STD_TEST(InsertCellsProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.insertCells(1, 2, screen.info().columns - 1, 2, TerminalCell{});
        });
    }

    STD_TEST(DeleteCellsProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.deleteCells(1, 2, screen.info().columns - 1, 2, TerminalCell{});
        });
    }

    STD_TEST(CopyRowProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.copyRow(4, 1, 0, screen.info().columns, TerminalCell{});
        });
    }

    STD_TEST(ScrollPartialRectangleUpProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRectangle(0, 1, 5, screen.info().columns - 1, -1, TerminalCell{});
        });
    }

    STD_TEST(ScrollFullRectangleUpProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRectangle(0, 0, 5, screen.info().columns, -2, TerminalCell{});
        });
    }

    STD_TEST(ScrollPartialRectangleDownProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRectangle(0, 1, 5, screen.info().columns - 1, 1, TerminalCell{});
        });
    }

    STD_TEST(ScrollFullRectangleDownProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRectangle(0, 0, 5, screen.info().columns, 2, TerminalCell{});
        });
    }

    STD_TEST(RotateRowsUpProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.rotateRows(0, 5, -2);
        });
    }

    STD_TEST(RotateRowsDownProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.rotateRows(0, 5, 2);
        });
    }

    STD_TEST(ScrollUpProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRows(0, 5, -2, TerminalCell{});
        });
    }

    STD_TEST(ScrollPartialRegionUpProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRows(1, 4, -1, TerminalCell{});
        });
    }

    STD_TEST(ScrollDownProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            screen.scrollRows(0, 5, 2, TerminalCell{});
        });
    }

    STD_TEST(DropScrollbackHistoryProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            prepareHistory(screen);
            screen.scrollView(2);
        }, [](Screen& screen) {
            screen.dropHistory();
        });
    }

    STD_TEST(PageUpProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            prepareHistory(screen);
        }, [](Screen& screen) {
            screen.scrollView(2);
        });
    }

    STD_TEST(PageDownProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            prepareHistory(screen);
            screen.scrollView(2);
        }, [](Screen& screen) {
            screen.scrollView(-1);
        });
    }

    STD_TEST(PageToBottomProducesCompleteIncrementalUpdate) {
        verifyDamage([](Screen& screen) {
            prepareHistory(screen);
            screen.scrollView(2);
        }, [](Screen& screen) {
            STD_INSIST(screen.scrollView(-0x7fffffff));
        });
    }

    STD_TEST(ExposeProducesValidFullUpdate) {
        verifyDamage([](Screen& screen) {
            screen.expose();
        });
    }

    STD_TEST(CycleSelectionSnapProducesCompleteMetadataUpdate) {
        verifyDamage([](Screen& screen) {
            screen.beginSelection(Point(2, 1));
            screen.updateSelection(Rect(2, 1, 4, 2));
        }, [](Screen& screen) {
            screen.cycleSelectionSnap();
        }, false);
    }

    STD_TEST(UpdateSelectionProducesCompleteMetadataUpdate) {
        verifyDamage([](Screen& screen) {
            screen.beginSelection(Point(2, 1));
        }, [](Screen& screen) {
            screen.updateSelection(Rect(2, 1, 4, 2));
        }, false);
    }

    STD_TEST(BeginSelectionProducesCompleteMetadataUpdate) {
        verifyDamage([](Screen& screen) {
            screen.beginSelection(Point(1, 1));
            screen.updateSelection(Rect(1, 1, 5, 3));
        }, false);
    }

    STD_TEST(ResizeWithoutReflowProducesCompleteIncrementalUpdate) {
        verifyResizeDamage(false);
    }

    STD_TEST(ResizeWithReflowProducesCompleteIncrementalUpdate) {
        verifyResizeDamage(true);
    }

    STD_TEST(RandomOperationsMatchSlowModelAndFullFrames) {
        auto composerPool = ObjPool::fromMemory();
        ObjPool::Ref screenPool = ObjPool::fromMemory();
        Composer& composer = *composerPool->make<Composer>(composerPool.mutPtr());
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, 4096));
        TerminalColors colors;
        configureColors(colors);
        Screen* screen = Screen::createAlternate(composer.extras, *screenPool, 8, 5, &colors);
        SlowScreenModel model(8, 5);
        DamageCanvas incremental;
        renderFull(*screen, colors, incremental);
        const u32 links[]{
            composer.extras.store->getOrCreateHyperlink(StringView(u8"one"), StringView(u8"https://one.test"), 1),
            composer.extras.store->getOrCreateHyperlink(StringView(u8"two"), StringView(u8"https://two.test"), 2),
        };
        u32 random = 0xc0ffee42u;

        for (u32 step = 0; step < 600; ++step) {
            const u32 operation = randomValue(random) % 6;
            bool resized = false;
            if (operation == 0) {
                const u16 row = randomBelow(random, model.rows);
                const u16 column = randomBelow(random, model.columns);
                TerminalCell attrs = attributes();
                attrs.bold = randomValue(random) & 1;
                attrs.protected_char = (randomValue(random) % 5 == 0) ? TerminalCell::isoProtection : 0;
                attrs.setForeground(CellColor::indexed((u8)(randomValue(random) & 0xff)));
                const u32 hyperlink = randomValue(random) % 4 == 0 ? links[randomValue(random) & 1] : 0;
                const u32 codepoint = 'A' + randomValue(random) % 26;
                const u32 semantic = randomValue(random) & 3;
                screen->writeCodepoint(row, column, codepoint, false, attrs, hyperlink, semantic, TerminalCell{});
                TerminalCell expected = attrs;
                expected.uc_pt = codepoint;
                expected.drawn = 1;
                expected.semantic = semantic;
                if (hyperlink != 0) {
                    composer.extras.store->setHyperlink(expected, hyperlink);
                }
                model.at(row, column) = expected;
            } else if (operation == 1) {
                const u16 top = randomBelow(random, model.rows);
                const u16 left = randomBelow(random, model.columns);
                const u16 bottom = top + 1 + randomBelow(random, model.rows - top);
                const u16 right = left + 1 + randomBelow(random, model.columns - left);
                TerminalCell attrs = attributes();
                attrs.italic = randomValue(random) & 1;
                attrs.protected_char = (randomValue(random) % 4 == 0) ? TerminalCell::isoProtection : 0;
                const u32 codepoint = 'a' + randomValue(random) % 26;
                screen->fillRectangle(top, left, bottom, right, codepoint, attrs, TerminalCell{});
                TerminalCell expected = attrs;
                expected.uc_pt = codepoint;
                expected.drawn = 1;
                for (u16 row = top; row < bottom; ++row) {
                    for (u16 column = left; column < right; ++column) {
                        model.at(row, column) = expected;
                    }
                }
            } else if (operation == 2 || operation == 3) {
                const u16 row = randomBelow(random, model.rows);
                const u16 begin = randomBelow(random, model.columns);
                const u16 count = 1 + randomBelow(random, model.columns - begin);
                if (operation == 2) {
                    screen->eraseCells(row, begin, count, TerminalCell{});
                } else {
                    screen->selectiveEraseCells(row, begin, count, TerminalCell{}, TerminalCell::isoProtection);
                }
                for (u16 column = begin; column < begin + count; ++column) {
                    if (operation == 2 || model.at(row, column).protected_char != TerminalCell::isoProtection) {
                        model.at(row, column) = {};
                    }
                }
            } else if (operation == 4) {
                const u16 top = randomBelow(random, model.rows);
                const u16 left = randomBelow(random, model.columns);
                const u16 bottom = top + 1 + randomBelow(random, model.rows - top);
                const u16 right = left + 1 + randomBelow(random, model.columns - left);
                const i32 count = 1 + randomBelow(random, (u16)(bottom - top + 1));
                const i32 amount = randomValue(random) & 1 ? count : -count;
                screen->scrollRectangle(top, left, bottom, right, amount, TerminalCell{});
                model.scroll(top, left, bottom, right, amount, TerminalCell{});
            } else {
                const u16 columns = 3 + randomBelow(random, 10);
                const u16 rows = 2 + randomBelow(random, 6);
                ObjPool::Ref replacementPool = ObjPool::fromMemory();
                Screen::Cursor cursor{Point(model.columns < columns ? model.columns - 1 : columns - 1, model.rows < rows ? model.rows - 1 : rows - 1), false};
                Screen* const replacement = screen->resized(*replacementPool, columns, rows, cursor);
                screen = replacement;
                screenPool = replacementPool;
                model.resize(columns, rows);
                resized = true;
            }

            model.insistMatches(*screen);
            const ScreenInfo info = screen->info();
            if (resized || incremental.columns != info.columns || incremental.rows != info.rows) {
                clearCanvas(incremental, info.columns, info.rows);
            }
            Vector<TerminalRow> damagedRows;
            const TerminalUpdate update = takeUpdate(*screen, colors, damagedRows);
            applyUpdate(incremental, update);
            DamageCanvas expected;
            renderFull(*screen, colors, expected);
            STD_INSIST(equalCanvas(incremental, expected));
        }
    }
}
