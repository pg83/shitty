/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */
/*
 * Differential, invariant-checking fuzz target.
 *
 * The input is decoded as a stream of [op, len, payload] records. Most
 * records feed pty bytes; the rest drive resize, keyboard, mouse, selection,
 * scrolling and timers through the same TestApi the unit tests use. Every
 * record is applied to two independent terminals (rig A and rig B) which must
 * remain bit-identical forever: equal pty responses, equal presentation
 * state, equal cell grids. Any divergence is a determinism bug. On top of
 * that, each rig is validated against the standalone invariants below.
 *
 * v3 strategies:
 * - Wild instances: records may create and destroy extra terminals. Global
 *   or shared state must never reference a destroyed instance; AddressSanitizer
 *   judges. This is the tab/lifetime bug class.
 * - Fake clock: monotonicNowUs is overridden and advanced deterministically
 *   per record, so timer deadlines (synchronized output, blink, animation)
 *   fire reproducibly inside short fuzz inputs. The linked
 *   libstd_external_clock.a is built with
 *   -DSTL_EXTERNAL_MONOTONIC_NOW_US=1, which deliberately leaves this
 *   function for its client to define.
 * - Scheduler pump: records may run the headless platform's timer loop once,
 *   firing those deadline timers on demand.
 */

#include "options.h"
#include "composer.h"
#include "grid_geometry.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/vt_test.h>
#include <lib/vterm/vt_trace.h>
#include <lib/shitty/vt_headless.h>
#include <lib/vterm/terminal_types.h>

#include <std/ios/sys.h>
#include <std/str/view.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <plt/poller.h>
#include <plt/platform.h>
#include <plt/poller_loop.h>

using namespace stl;
using namespace plt;

// Client definition required by libstd_external_clock.a. The harness advances
// this deterministic clock once per record.
constexpr u64 FuzzClockInitialUs = 1'000'000'000'000;
static u64 fuzzClockUs = FuzzClockInitialUs;

namespace stl {
    u64 monotonicNowUs() noexcept {
        return fuzzClockUs;
    }
}

namespace {
    static u64 sequence = 0;

    [[noreturn]] static void invariantViolation(const char* what, u64 a, u64 b) {
        sysE << StringView(u8"fuzz invariant violated: ") << StringView(what) << StringView(u8" (") << (u64)(a) << StringView(u8", ") << (u64)(b) << StringView(u8") seq=") << (u64)(sequence) << endL;
        abort();
    }

    static void check(bool condition, const char* what, u64 a = 0, u64 b = 0) {
        if (!condition) {
            invariantViolation(what, a, b);
        }
    }

    static bool validCodepoint(u32 codepoint) {
        return codepoint <= 0x10ffff && (codepoint < 0xd800 || codepoint > 0xdfff);
    }

    struct CaptureOutput final: public Output {
        size_t writeImpl(const void* data, size_t size) override {
            bytes.append(data, size);
            return size;
        }

        Buffer bytes;
    };

    struct CaptureTestApi final: public VtermTraceFactory {
        VtermTrace* construct(TestApi* testApi) override {
            api = testApi;
            return nullptr;
        }

        TestApi* api = nullptr;
    };

    struct Rig {
        ObjPool::Ref pool;
        Composer* composer;
        VtermHeadless* headless;
        Vterm* term;
        TestApi* api;
        CaptureOutput* pty;
        bool splitFeeds = false;

        Rig()
            : pool(ObjPool::fromMemory())
        {
            composer = pool->make<Composer>(pool.mutPtr());
            pty = pool->make<CaptureOutput>();
            CaptureTestApi capture;
            headless = VtermHeadless::create(*composer, &capture, pty);
            term = headless->terminal();
            api = capture.api;
        }
    };

    // Extra terminals a record may create and destroy mid-stream. Anything
    // shared across instances must not reference a destroyed one.
    struct WildSet {
        Rig* rigs[2] = {};
        size_t count = 0;

        void create() {
            if (count < 2) {
                rigs[count++] = new Rig();
            }
        }

        void destroy(u8 which) {
            if (count == 0) {
                return;
            }
            const size_t index = which % count;
            delete rigs[index];
            rigs[index] = rigs[--count];
            rigs[count] = nullptr;
        }

        ~WildSet() {
            for (size_t i = 0; i < count; ++i) {
                delete rigs[i];
            }
        }
    };

    static void checkUpdate(const Rig& rig, const TerminalUpdate& update) {
        const u64 columns = rig.headless->geometry().columns;
        const u64 rows = rig.headless->geometry().rows;
        // The cursor never leaves the screen; posY is in view coordinates.
        check(update.cursor.posX < columns, "cursor column out of bounds", update.cursor.posX, columns);
        check(update.cursor.posY >= update.viewOffset, "cursor above the view", update.cursor.posY, update.viewOffset);
        check((u64)(update.cursor.posY) - update.viewOffset < rows, "cursor row out of bounds", update.cursor.posY, update.viewOffset);
        check((u8)(update.cursor.style) <= (u8)(TerminalCursor::Style::bar), "invalid cursor style", (u8)(update.cursor.style), 0);
        // The view can scroll back at most to the start of the scrollback.
        check(update.viewOffset <= update.historyRows, "view offset exceeds history", update.viewOffset, update.historyRows);
        // The palette generation wraps around but is never republished as zero.
        check(update.colors != nullptr && update.colors->generation != 0, "invalid color generation", 0, 0);
        // Damaged rows reference whole rows of the visible frame, in
        // ascending order.
        u64 previousRow = 0;
        for (size_t i = 0; i < update.rowCount; ++i) {
            const TerminalRow& row = update.rows[i];
            check(row.cells != nullptr, "damaged row without cells", i, 0);
            check(row.row < rows, "damaged row out of frame", row.row, rows);
            check(i == 0 || row.row > previousRow, "damaged rows out of order", row.row, previousRow);
            previousRow = row.row;
            // The damage view and the random-access view must agree.
            check(row.cells[0] == rig.api->cell(row.row, 0).cell, "row disagrees with testCell", row.row, 0);
        }
        // Selection rectangles stay inside the addressable frame.
        const Rect* rects[] = {&update.selection, &update.snappedSelection};
        for (const Rect* rect : rects) {
            if (rect->null()) {
                continue;
            }
            check(rect->tl.x >= 0 && rect->br.x >= 0, "negative selection column", (u64)(rect->tl.x), 0);
            check(rect->tl.y >= 0 && rect->br.y >= 0, "negative selection row", (u64)(rect->tl.y), 0);
            check(rect->tl.x <= (int)(columns) && rect->br.x <= (int)(columns), "selection column out of bounds", (u64)(rect->br.x), columns);
            check(rect->tl.y <= (int)(rows + update.historyRows) && rect->br.y <= (int)(rows + update.historyRows), "selection row out of bounds", (u64)(rect->br.y), rows);
        }
    }

    static void checkState(Rig& rig) {
        const VtermTestState state = rig.api->inspect();
        check((u8)(state.cursorStyle) <= (u8)(TerminalCursor::Style::bar), "invalid pen cursor style", (u8)(state.cursorStyle), 0);
        // Margins are never inverted and never exceed the screen.
        check(state.rectangleOrigin.rowBase <= state.rectangleOrigin.rowLimit, "inverted vertical margins", state.rectangleOrigin.rowBase, state.rectangleOrigin.rowLimit);
        check(state.rectangleOrigin.columnBase <= state.rectangleOrigin.columnLimit, "inverted horizontal margins", state.rectangleOrigin.columnBase, state.rectangleOrigin.columnLimit);
        check(state.rectangleOrigin.rowLimit <= rig.headless->geometry().rows, "vertical margin out of bounds", state.rectangleOrigin.rowLimit, rig.headless->geometry().rows);
        check(state.rectangleOrigin.columnLimit <= rig.headless->geometry().columns, "horizontal margin out of bounds", state.rectangleOrigin.columnLimit, rig.headless->geometry().columns);
    }

    static void checkCells(Rig& rig) {
        TestApi* const api = rig.api;
        const u16 columns = rig.headless->geometry().columns;
        const u16 rows = rig.headless->geometry().rows;
        for (u16 row = 0; row < rows; ++row) {
            VtermTestCell previous = api->cell(row, 0);
            check(!previous.cell.dwidth_cont, "row starts with a wide continuation cell", row, 0);
            for (u16 column = 0; column < columns; ++column) {
                const VtermTestCell current = column == 0 ? previous : api->cell(row, column);
                check(current.lineAttribute <= 3, "invalid line attribute", row, current.lineAttribute);
                if (current.cell.hasExtra()) {
                    // Grapheme clusters live in the extra cell store. The
                    // grapheme is legitimately empty when the extra record
                    // only carries an underline color or a hyperlink; a
                    // dangling ref segfaults in underlineColor() under ASan.
                    for (size_t i = 0; i < current.graphemeSize; ++i) {
                        check(validCodepoint(current.grapheme[i]), "invalid grapheme codepoint", row, current.grapheme[i]);
                    }
                } else if (current.cell.drawn && !current.cell.dwidth_cont) {
                    // Continuation and erased cells carry no codepoint: their
                    // content word is inherited from the attribute template.
                    check(validCodepoint(current.cell.uc_pt), "invalid cell codepoint", row, current.cell.uc_pt);
                }
                // A wide glyph is exactly a (dwidth, dwidth_cont) cell pair.
                check(current.cell.dwidth_cont == (column != 0 && previous.cell.dwidth), "broken wide glyph pair", row, column);
                previous = current;
            }
            check(!previous.cell.dwidth, "wide glyph truncated at the right edge", row, columns);
        }
    }

    static bool equalColor(Color a, Color b) {
        return a.red == b.red && a.green == b.green && a.blue == b.blue;
    }

    static bool equalCell(const TerminalCell& a, const TerminalCell& b) {
        // Reserved content bits hold no meaning, and the wrap bit is
        // geometry-dependent reflow bookkeeping: resize re-emits it at the new
        // boundaries and drops it on non-reflowable (double-width) lines.
        // The payload word carries internal extra-store refs whose numbering
        // is allocation-order dependent, so resolved extras are compared
        // instead of refs.
        return a.style == b.style && (a.content & 0x87ffffffu) == (b.content & 0x87ffffffu);
    }

    static bool equalHyperlink(const Rig& a, const Rig& b, u16 row, u16 column) {
        const Insets insets = a.composer->contentInsets();
        const int x = insets.left + column * a.composer->geometry.cellPixelWidth;
        const int y = insets.top + row * a.composer->geometry.cellPixelHeight;
        const StringView la = a.api->hyperlinkAt(x, y);
        const StringView lb = b.api->hyperlinkAt(x, y);
        return la.length() == lb.length() && (la.empty() || memcmp(la.data(), lb.data(), la.length()) == 0);
    }

    static void compareCells(const Rig& a, const Rig& b) {
        const u16 columns = a.headless->geometry().columns;
        const u16 rows = a.headless->geometry().rows;
        for (u16 row = 0; row < rows; ++row) {
            for (u16 column = 0; column < columns; ++column) {
                const VtermTestCell ca = a.api->cell(row, column);
                const VtermTestCell cb = b.api->cell(row, column);
                check(equalCell(ca.cell, cb.cell), "cell grids diverge", row, column);
                check(ca.lineAttribute == cb.lineAttribute, "line attributes diverge", row, column);
                check(ca.underlineColor == cb.underlineColor, "underline colors diverge", row, column);
                check(ca.graphemeSize == cb.graphemeSize, "graphemes diverge", row, column);
                if (ca.graphemeSize != 0) {
                    check(memcmp(ca.grapheme, cb.grapheme, ca.graphemeSize * sizeof(u32)) == 0, "grapheme bytes diverge", row, column);
                }
                if (ca.cell.hasExtra() || cb.cell.hasExtra()) {
                    check(equalHyperlink(a, b, row, column), "hyperlinks diverge", row, column);
                }
            }
        }
    }

    static void compareState(const Rig& a, const Rig& b) {
        check(a.headless->geometry().columns == b.headless->geometry().columns, "columns diverge", a.headless->geometry().columns, b.headless->geometry().columns);
        check(a.headless->geometry().rows == b.headless->geometry().rows, "rows diverge", a.headless->geometry().rows, b.headless->geometry().rows);
        const VtermTestState sa = a.api->inspect();
        const VtermTestState sb = b.api->inspect();
        check(sa.kittyKeyboardFlags == sb.kittyKeyboardFlags, "kitty keyboard flags diverge", sa.kittyKeyboardFlags, sb.kittyKeyboardFlags);
        check(sa.screenReverseVideo == sb.screenReverseVideo, "reverse video diverges", sa.screenReverseVideo, sb.screenReverseVideo);
        check(sa.ledState == sb.ledState, "led state diverges", sa.ledState, sb.ledState);
        check(sa.reverseWrapMode == sb.reverseWrapMode, "reverse wrap diverges", sa.reverseWrapMode, sb.reverseWrapMode);
        check(sa.nationalReplacementMode == sb.nationalReplacementMode, "national replacement diverges", sa.nationalReplacementMode, sb.nationalReplacementMode);
        check(sa.cursorStyle == sb.cursorStyle, "cursor styles diverge", (u8)(sa.cursorStyle), (u8)(sb.cursorStyle));
        check(equalCell(sa.pen.cell, sb.pen.cell), "pens diverge", 0, 0);
        check(equalColor(sa.pen.fg, sb.pen.fg) && equalColor(sa.pen.bg, sb.pen.bg), "pen colors diverge", 0, 0);
        check(sa.rectangleOrigin.rowBase == sb.rectangleOrigin.rowBase && sa.rectangleOrigin.rowLimit == sb.rectangleOrigin.rowLimit, "vertical margins diverge", sa.rectangleOrigin.rowBase, sb.rectangleOrigin.rowBase);
        check(sa.rectangleOrigin.columnBase == sb.rectangleOrigin.columnBase && sa.rectangleOrigin.columnLimit == sb.rectangleOrigin.columnLimit, "horizontal margins diverge", sa.rectangleOrigin.columnBase, sb.rectangleOrigin.columnBase);
        check((u8)(sa.mouse.mode) == (u8)(sb.mouse.mode) && (u8)(sa.mouse.enc) == (u8)(sb.mouse.enc), "mouse state diverges", (u8)(sa.mouse.mode), (u8)(sb.mouse.mode));
        check(sa.mouse.focusEventMode == sb.mouse.focusEventMode, "mouse focus mode diverges", 0, 0);
    }

    static void comparePty(Rig& a, Rig& b) {
        const size_t size = a.pty->bytes.length();
        check(size == b.pty->bytes.length(), "pty response sizes diverge", size, b.pty->bytes.length());
        check(size == 0 || memcmp(a.pty->bytes.data(), b.pty->bytes.data(), size) == 0, "pty responses diverge", size, 0);
        a.pty->bytes.reset();
        b.pty->bytes.reset();
    }

    static void compareUpdates(const Rig& a, const TerminalUpdate* ua, const Rig& b, const TerminalUpdate* ub) {
        // Frame presence and damage history are legitimately delivery-dependent:
        // a pending frame produced by an early chunk of a split feed survives,
        // and the damage accumulator carries it. Only content fields captured
        // at output() time must converge.
        if (ua == nullptr || ub == nullptr) {
            return;
        }
        check(ua->cursor.posX == ub->cursor.posX && ua->cursor.posY == ub->cursor.posY, "cursors diverge", ua->cursor.posX, ub->cursor.posX);
        check(ua->cursor.style == ub->cursor.style, "cursor styles diverge", (u8)(ua->cursor.style), (u8)(ub->cursor.style));
        check(ua->viewOffset == ub->viewOffset && ua->historyRows == ub->historyRows, "scrollback diverges", ua->viewOffset, ub->viewOffset);
    }

    struct ActionResult {
        bool present = false;
        u64 value = 0;
        Buffer text;
    };

    struct CellSnap {
        TerminalCell cell;
        CellColor underlineColor;
        u8 lineAttribute;
        u32 graphemeOffset;
        u32 graphemeSize;
    };

    // The grid snapshot is two flat trivially-destructible pieces: cell
    // records and one shared grapheme codepoint arena.
    struct GridSnapshot {
        Vector<CellSnap> cells;
        Vector<u32> graphemes;
    };

    static void snapshotGrid(Rig& rig, GridSnapshot& snapshot) {
        const u16 columns = rig.headless->geometry().columns;
        const u16 rows = rig.headless->geometry().rows;
        snapshot.cells.clear();
        snapshot.graphemes.clear();
        for (u16 row = 0; row < rows; ++row) {
            for (u16 column = 0; column < columns; ++column) {
                const VtermTestCell cell = rig.api->cell(row, column);
                CellSnap snap = {};
                snap.cell = cell.cell;
                snap.underlineColor = cell.underlineColor;
                snap.lineAttribute = cell.lineAttribute;
                snap.graphemeOffset = (u32)(snapshot.graphemes.length());
                snap.graphemeSize = (u32)(cell.graphemeSize);
                snapshot.graphemes.append(cell.grapheme, cell.graphemeSize);
                snapshot.cells.pushBack(snap);
            }
        }
    }

    static void compareSnapshot(const Rig& rig, const GridSnapshot& snapshot) {
        const u16 columns = rig.headless->geometry().columns;
        const u16 rows = rig.headless->geometry().rows;
        check(snapshot.cells.length() == (size_t)(columns)*rows, "geometry changed during probe", snapshot.cells.length(), 0);
        for (u16 row = 0; row < rows; ++row) {
            for (u16 column = 0; column < columns; ++column) {
                const CellSnap& snap = snapshot.cells[(size_t)(row)*columns + column];
                const VtermTestCell cell = rig.api->cell(row, column);
                check(equalCell(snap.cell, cell.cell), "scroll round trip changed cells", row, column);
                check(snap.lineAttribute == cell.lineAttribute, "scroll round trip changed attributes", row, column);
                check(snap.underlineColor == cell.underlineColor, "scroll round trip changed underline", row, column);
                check(snap.graphemeSize == cell.graphemeSize, "scroll round trip changed graphemes", row, column);
                if (snap.graphemeSize != 0) {
                    check(memcmp(snapshot.graphemes.data() + snap.graphemeOffset, cell.grapheme, (size_t)(snap.graphemeSize) * sizeof(u32)) == 0, "scroll round trip changed grapheme bytes", row, column);
                }
            }
        }
    }

    // Scrolling the view to the oldest scrollback row and back must restore
    // the view offset and leave the visible grid untouched. The detour also
    // re-validates the cell invariants against history rows.
    static void probeScrollRoundTrip(Rig& rig, u32 viewOffset, u32 historyRows) {
        if (historyRows == 0 || viewOffset > 60000) {
            return;
        }
        // In alt-scroll mode the wheel emits arrow keys instead of scrolling.
        if (rig.api->privateMode(1007) && (rig.api->privateMode(47) || rig.api->privateMode(1047))) {
            return;
        }
        GridSnapshot snapshot;
        snapshotGrid(rig, snapshot);
        rig.api->scrollUp(0xffff);
        checkCells(rig);
        rig.api->scrollDown(0xffff);
        rig.api->scrollUp((u16)(viewOffset));
        rig.term->expose();
        if (const TerminalUpdate* update = rig.term->output()) {
            check(update->viewOffset == viewOffset, "scroll round trip lost the view offset", update->viewOffset, viewOffset);
            rig.term->consume();
        }
        compareSnapshot(rig, snapshot);
    }

    static u16 u16at(const u8* data) {
        return (u16)(data[0] | ((u16)(data[1]) << 8));
    }

    static i32 coordinate(const u8* data, u16 limit) {
        return (i32)(u16at(data) % (u16)(limit + 32)) - 16;
    }

    static ActionResult apply(Rig& rig, u8 op, const u8* payload, size_t len) {
        ActionResult result;
        Vterm* const term = rig.term;
        TestApi* const api = rig.api;
        switch (op) {
            case 200:
                if (len >= 2) {
                    api->key((InputKey)(payload[0] % (u8)(InputKey::Count)), (VtModifier)(payload[1] & 7));
                }
                break;
            case 201:
                if (len >= 2) {
                    api->character(payload[0], (VtModifier)(payload[1] & 7));
                }
                break;
            case 202:
                if (len >= 6) {
                    api->kittyKey(u16at(payload), u16at(payload + 2), payload[4], payload[5] & 15, (VtermKeyEventType)(payload[5] % 3 + 1));
                }
                break;
            case 203:
                if (len >= 9) {
                    api->locatorPosition((u16)(u16at(payload) % (rig.headless->geometry().columns + 4)), (u16)(u16at(payload + 2) % (rig.headless->geometry().rows + 4)), u16at(payload + 4), u16at(payload + 6), payload[8]);
                }
                break;
            case 204:
                if (len >= 2) {
                    api->locatorButton(payload[0], (payload[1] & 1) != 0);
                }
                break;
            case 205:
                if (len >= 8) {
                    result.present = true;
                    result.value = api->mouseHighlightRelease(u16at(payload), u16at(payload + 2), u16at(payload + 4), u16at(payload + 6));
                }
                break;
            case 206:
                if (len >= 2) {
                    api->scrollUp(u16at(payload) % 1024);
                }
                break;
            case 207:
                if (len >= 2) {
                    api->scrollDown(u16at(payload) % 1024);
                }
                break;
            case 208:
                api->pageUp();
                break;
            case 209:
                api->pageDown();
                break;
            case 210:
                if (len >= 5) {
                    api->selectionStart(coordinate(payload, rig.headless->geometry().pixelWidth), coordinate(payload + 2, rig.headless->geometry().pixelHeight), (payload[4] & 1) != 0);
                }
                break;
            case 211:
                if (len >= 5) {
                    api->selectionExtend(coordinate(payload, rig.headless->geometry().pixelWidth), coordinate(payload + 2, rig.headless->geometry().pixelHeight), (payload[4] & 1) != 0);
                }
                break;
            case 212:
                if (len >= 4) {
                    api->selectionUpdate(coordinate(payload, rig.headless->geometry().pixelWidth), coordinate(payload + 2, rig.headless->geometry().pixelHeight));
                }
                break;
            case 213: {
                const VtermTextResult selection = api->selectionFinish();
                result.present = true;
                result.value = selection.status;
                if (selection.text.length() != 0) {
                    result.text.append(selection.text.data(), selection.text.length());
                }
                break;
            }
            case 214:
                api->selectionRectangular();
                break;
            case 215:
                result.present = true;
                result.value = api->advanceSelectionAutoscroll();
                break;
            case 216:
                api->paste(StringView(payload, len));
                break;
            case 217:
                if (len >= 2) {
                    const u16 columns = (u16)(1 + payload[0] % 200);
                    const u16 rows = (u16)(1 + payload[1] % 60);
                    const Insets insets = rig.composer->contentInsets();
                    rig.composer->resize((u16)(gridPixelWidth(columns, insets, rig.composer->geometry.cellPixelWidth)), (u16)(gridPixelHeight(rows, insets, rig.composer->geometry.cellPixelHeight)));
                }
                break;
            case 218:
                if (len >= 1) {
                    // The clock is fake and shared, so deadline-based expiry
                    // is deterministic between the rigs now.
                    result.present = true;
                    result.value = term->expireSynchronizedOutput((payload[0] & 1) != 0);
                }
                break;
            case 219:
                if (len >= 1) {
                    result.present = true;
                    result.value = term->advanceAnimation((payload[0] & 1) != 0);
                }
                break;
            case 220:
                if (len >= 1) {
                    term->sendBytes(StringView(payload + 1, len - 1), (payload[0] & 1) != 0);
                }
                break;
            case 221:
                if (len >= 2) {
                    // Grow, then shrink straight back with no writes in
                    // between: reflow must restore the grid exactly.
                    const u16 backWidth = rig.composer->geometry.pixelWidth;
                    const u16 backHeight = rig.composer->geometry.pixelHeight;
                    const u16 columns = (u16)(rig.composer->geometry.columns + 1 + payload[0] % 80);
                    const u16 rows = (u16)(rig.composer->geometry.rows + payload[1] % 20);
                    const Insets insets = rig.composer->contentInsets();
                    rig.composer->resize((u16)(gridPixelWidth(columns, insets, rig.composer->geometry.cellPixelWidth)), (u16)(gridPixelHeight(rows, insets, rig.composer->geometry.cellPixelHeight)));
                    rig.composer->resize(backWidth, backHeight);
                }
                break;
            case 222:
                if (len >= 2) {
                    // Mimic a font zoom: glyph metrics change while the window
                    // adopts the same cell geometry, as fontChanged() does.
                    const u16 glyphWidth = (u16)(1 + payload[0] % 4);
                    const u16 glyphHeight = (u16)(1 + payload[1] % 4);
                    rig.composer->geometry.setCellPixelSize(glyphWidth, glyphHeight);
                    const Insets insets = rig.composer->contentInsets();
                    rig.composer->resize((u16)(gridPixelWidth(rig.composer->geometry.columns, insets, glyphWidth)), (u16)(gridPixelHeight(rig.composer->geometry.rows, insets, glyphHeight)));
                }
                break;
            case 225: {
                // Run the headless platform's timer loop once: deadline
                // timers (synchronized output, blink, animation) fire here.
                PollerLoop* const poller = static_cast<PollerLoop*>(rig.composer->platform->poller());
                if (poller != nullptr) {
                    poller->dispatchTimers();
                    if (poller->nextDeadline() == 0) {
                        poller->wait(0);
                    }
                }
                break;
            }
            default:
                if (rig.splitFeeds && len > 2) {
                    // Deliver the same bytes in chunks: escape sequences split
                    // across pty reads must not change parser semantics.
                    const size_t first = payload[0] % len;
                    const size_t second = first + payload[1] % (len - first);
                    term->feedPty(StringView(payload, first));
                    term->feedPty(StringView(payload + first, second - first));
                    term->feedPty(StringView(payload + second, len - second));
                } else {
                    term->feedPty(StringView(payload, len));
                }
                break;
        }
        return result;
    }

    static void compareResult(const ActionResult& a, const ActionResult& b) {
        check(a.present == b.present, "action result presence diverges", 0, 0);
        if (!a.present) {
            return;
        }
        check(a.value == b.value, "action results diverge", a.value, b.value);
        check(StringView(a.text) == StringView(b.text), "action texts diverge", a.text.used(), b.text.used());
    }

    static void validateRig(Rig& rig) {
        rig.term->expose();
        if (const TerminalUpdate* update = rig.term->output()) {
            checkUpdate(rig, *update);
            rig.term->consume();
        }
        checkState(rig);
        checkCells(rig);
    }

    static void runRecord(Rig& a, Rig& b, WildSet& wild, u8 op, const u8* payload, size_t len) {
        // Deterministic clock: 10 ms per record. Synchronized output's 150 ms
        // deadline expires a handful of records after the mode is enabled.
        fuzzClockUs += 10'000;

        if (op == 223) {
            wild.create();
        } else if (op == 224 && len >= 1) {
            wild.destroy(payload[0]);
        }

        const ActionResult ra = apply(a, op, payload, len);
        const ActionResult rb = apply(b, op, payload, len);
        compareResult(ra, rb);
        comparePty(a, b);

        // expose() forces a presentation on both rigs so that the cursor and
        // damage state can be compared on every record.
        a.term->expose();
        b.term->expose();
        const TerminalUpdate* ua = a.term->output();
        const TerminalUpdate* ub = b.term->output();
        if (ua != nullptr) {
            checkUpdate(a, *ua);
        }
        if (ub != nullptr) {
            checkUpdate(b, *ub);
        }
        compareUpdates(a, ua, b, ub);
        if (ua != nullptr) {
            a.term->consume();
        }
        if (ub != nullptr) {
            b.term->consume();
        }

        checkState(a);
        checkState(b);
        checkCells(a);
        checkCells(b);
        compareState(a, b);
        compareCells(a, b);

        if (ua != nullptr && ub != nullptr && (ua->viewOffset != 0 || (sequence & 3) == 0)) {
            const u32 viewOffset = ua->viewOffset;
            const u32 historyRows = ua->historyRows;
            probeScrollRoundTrip(a, viewOffset, historyRows);
            probeScrollRoundTrip(b, viewOffset, historyRows);
            comparePty(a, b);
        }

        // Wild terminals get the same record and the full invariant suite;
        // only the mirror pair is differentially compared.
        for (size_t i = 0; i < wild.count; ++i) {
            apply(*wild.rigs[i], op, payload, len);
            validateRig(*wild.rigs[i]);
        }
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t len) {
    // Fresh rigs per input: deep states come from long record streams, and
    // every crash artifact is standalone-reproducible and minimizable.
    fuzzClockUs = FuzzClockInitialUs;
    Rig a;
    Rig b;
    WildSet wild;
    b.splitFeeds = true;
    ++sequence;
    size_t offset = 0;
    while (offset + 2 <= len) {
        u8 op = data[offset];
        const size_t size = data[offset + 1];
        offset += 2;
        if (offset + size > len) {
            break;
        }
        // A quarter of the opcode space drives actions; the rest feeds bytes.
        if (op >= 192) {
            op = (u8)(200 + (op - 192) % 26);
        }
        runRecord(a, b, wild, op, (const u8*)(data + offset), size);
        offset += size;
    }
    return 0;
}
