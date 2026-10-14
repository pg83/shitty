/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pty.h"
#include "render.h"
#include "options.h"
#include "composer.h"
#include "font_pack.h"
#include "pane_layout.h"
#include "span_shaper.h"
#include "vt_headless.h"
#include "font_embedded.h"
#include "font_resolver.h"
#include "grid_geometry.h"
#include "render_reference.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/vt_test.h>
#include <lib/vterm/vt_trace.h>
#include <lib/vterm/cell_extra_store.h>

#if defined(HAVE_METAL_RENDERER)
    #include "render_metal.h"
#endif

#include <plt/fiber.h>
#include <plt/platform.h>
#include <plt/platform_headless.h>
#include <plt/window.h>

#include <std/tst/ut.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>

#include <unistd.h>

using namespace stl;

namespace {
    struct CaptureOutput final: public Output {
        size_t writeImpl(const void* data, size_t size) override;

        Buffer bytes;
    };

    static void discardOutput(Vterm& terminal) {
        if (terminal.output() != nullptr) {
            terminal.consume();
        }
    }

    static void insistMatchingCursor(Vterm& whole, Vterm& split) {
        whole.expose();
        split.expose();
        const TerminalUpdate* const wholeUpdate = whole.output();
        const TerminalUpdate* const splitUpdate = split.output();
        STD_INSIST(wholeUpdate != nullptr);
        STD_INSIST(splitUpdate != nullptr);
        STD_INSIST(wholeUpdate->cursor.posX == splitUpdate->cursor.posX);
        STD_INSIST(wholeUpdate->cursor.posY == splitUpdate->cursor.posY);
        whole.consume();
        split.consume();
    }

    // How many times `needle` appears in what the child was sent. Counted
    // rather than merely found: the whole point of the resize tests is
    // that one event produces one report, and a "contains" check passes
    // just as happily on two.
    static size_t countOccurrences(const Buffer& haystack, StringView needle) {
        const StringView bytes((const u8*)(haystack.data()), haystack.used());
        size_t found = 0;
        for (size_t at = 0; at + needle.length() <= bytes.length(); ++at) {
            if (StringView(bytes.data() + at, needle.length()) == needle) {
                ++found;
            }
        }
        return found;
    }

    static void feedInFuzzChunks(Vterm& terminal, const u8* bytes, size_t size) {
        const size_t first = bytes[0] % size;
        const size_t second = first + bytes[1] % (size - first);
        terminal.feedPty(StringView(bytes, first));
        terminal.feedPty(StringView(bytes + first, second - first));
        terminal.feedPty(StringView(bytes + second, size - second));
    }
}

size_t CaptureOutput::writeImpl(const void* data, size_t size) {
    bytes.append(data, size);
    return size;
}

namespace {
    // The second terminal of the coexistence test needs a pty face of
    // its own; test scaffolding stays in the test.
    // Vterm::create hands the trace factory the terminal's TestApi, and
    // that is the only door to advanceSelectionAutoscroll() - the forced
    // step that makes the autoscroll observable without waiting out its
    // real interval on a real loop.
    struct CaptureTestApi final: public VtermTraceFactory {
        VtermTrace* construct(TestApi* api) override {
            testApi = api;
            return nullptr;
        }

        TestApi* testApi = nullptr;
    };

    struct SecondPtyStub final: public PtyHandle {
        explicit SecondPtyStub(Composer& composer_)
            : composer(composer_)
        {
        }

        void resize(const PtySize&) override {
        }

        void engage() override {
        }

        Chunk* allocate(size_t len) override {
            payload_.reset();
            payload_.grow(len);
            payload_.seekAbsolute(len);
            used_ = len;
            return &chunk_;
        }

        void send(Chunk*, size_t len) override {
            sent.append(payload_.data(), len);
        }

        Chunk* acquire() override {
            composer.scheduler->current()->park();
            return nullptr;
        }

        void release(Chunk*) override {
        }

        pid_t foregroundProcessGroup() override {
            return group;
        }

        struct StubChunk final: public Chunk {
            explicit StubChunk(SecondPtyStub* owner_)
                : owner(owner_)
            {
            }

            void* data() override {
                return owner->payload_.mutData();
            }

            size_t length() override {
                return owner->used_;
            }

            Chunk* next() override {
                return nullptr;
            }

            SecondPtyStub* owner;
        };

        Composer& composer;
        // Everything the terminal wrote to its child, for the tests that
        // count reports rather than merely notice them.
        stl::Buffer sent;
        // What foregroundProcessGroup() answers; 0 is "no foreground".
        pid_t group = 0;
        stl::Buffer payload_;
        size_t used_ = 0;
        StubChunk chunk_{this};
    };

    // Forwards to the real headless host, recording the titles the
    // terminal publishes on the way through.
    struct TitleCaptureHost final: public VtHost {
        explicit TitleCaptureHost(VtHost* inner_)
            : inner(inner_)
        {
        }

        plt::Clipboard* primary() override {
            return inner->primary();
        }

        plt::Clipboard* secondary() override {
            return inner->secondary();
        }

        plt::WindowInfo info() override {
            return inner->info();
        }

        void requestFrame() override {
            inner->requestFrame();
        }

        void requestResize(u32 width, u32 height) override {
            inner->requestResize(width, height);
        }

        void requestMaximized(bool maximized) override {
            inner->requestMaximized(maximized);
        }

        void requestFullscreen(bool fullscreen) override {
            inner->requestFullscreen(fullscreen);
        }

        void requestIconify() override {
            inner->requestIconify();
        }

        void requestRestore() override {
            inner->requestRestore();
        }

        void requestMove(i32 x, i32 y) override {
            inner->requestMove(x, y);
        }

        void requestFocus() override {
            inner->requestFocus();
        }

        void requestAttention() override {
            inner->requestAttention();
        }

        void requestPointerIcon(plt::PointerIcon icon) override {
            inner->requestPointerIcon(icon);
        }

        void requestOpenUri(StringView uri) override {
            inner->requestOpenUri(uri);
        }

        bool uriSchemeAllowed(StringView scheme) override {
            return inner->uriSchemeAllowed(scheme);
        }

        void titleChanged(const VtermTitleChanged& event) override {
            lastTitle.reset();
            lastTitle.append(event.title.data(), event.title.length());
            inner->titleChanged(event);
        }

        void resized() override {
            inner->resized();
        }

        // Three our VtHost carries and upstream's does not (A1, A11):
        // forwarded like the rest, so wrapping the harness's host
        // changes nothing but what the titles are seen through.
        VtInsets contentInsets() override {
            return inner->contentInsets();
        }

        void surfaceResized(u32 width, u32 height) override {
            inner->surfaceResized(width, height);
        }

        size_t cellCapacityExcept(const Vterm* except) override {
            return inner->cellCapacityExcept(except);
        }

        VtHost* inner;
        Buffer lastTitle;
    };
}

STD_TEST_SUITE(VtermHeadless) {
    STD_TEST(BuildsItsOwnEmbeddingPieces) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());

        VtermHeadless* const headless = VtermHeadless::create(composer, nullptr);

        STD_INSIST(headless->platform() != nullptr);
        STD_INSIST(headless->window() != nullptr);
        STD_INSIST(headless->host() != nullptr);
        STD_INSIST(headless->host()->primary() != nullptr);
        STD_INSIST(headless->host()->secondary() != nullptr);
        STD_INSIST(headless->geometry().columns != 0);
        STD_INSIST(headless->terminal() != nullptr);

        // The surface it sizes is 80 columns by 24 rows at one pixel per
        // cell. Nothing downstream pins those two: every consumer of this
        // harness feeds bytes and reads output, and a grid transposed to
        // 24x80 answers all of them without complaining. Swap the two in
        // VtermHeadless::create and this is the only line that notices.
        STD_INSIST(composer.geometry.columns == 80);
        STD_INSIST(composer.geometry.rows == 24);
        STD_INSIST(composer.geometry.pixelWidth == gridPixelWidth(80, composer.contentInsets(), composer.geometry.cellPixelWidth));
        STD_INSIST(composer.geometry.pixelHeight == gridPixelHeight(24, composer.contentInsets(), composer.geometry.cellPixelHeight));
    }

    // A tab is a second terminal behind the same window. Two of them must
    // be able to exist at once against one set of embedding pieces: the
    // geometry, the extras and the host are per window, and each terminal
    // only adds itself on top.
    STD_TEST(SecondVtermCoexistsOnOneEmbedding) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Vterm* const first = VtermHeadless::create(composer, nullptr)->terminal();

        Vterm* const second = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, windowPane(composer), *composer.pool->make<SecondPtyStub>(composer), nullptr);

        STD_INSIST(first != nullptr);
        STD_INSIST(second != nullptr);
        STD_INSIST(first != second);
    }

    // A8: the terminal's grid is the one it was handed, not the one the
    // window has. Both terminals here share a Composer whose window is 80
    // by 24; the second was created as a 10 by 4 pane and has to describe
    // itself that way to its child.
    //
    // DEC mode 2048 is the probe because it needs no option to be turned
    // on and because it names both axes in one report: 48;rows;columns;
    // height;width. The pane's two numbers are neither equal nor the
    // window's, so an implementation that read the window, or that paired
    // columns with rows, answers something else rather than accidentally
    // right.
    STD_TEST(TakesItsGridFromThePaneItWasGiven) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput windowPty;
        Vterm& whole = *VtermHeadless::create(composer, nullptr, &windowPty)->terminal();
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4}, panePty, nullptr);

        whole.feedPty(StringView(u8"\x1b[?2048h"));
        pane->feedPty(StringView(u8"\x1b[?2048h"));

        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"\x1b[48;24;80;24;80t")) == 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"\x1b[48;4;10;4;10t")) == 1);
    }

    // A5-3 (panes-R5-arch): DECRQSS for DECSLPP asks "how many lines is
    // the page", and the page is this terminal's, not the window's. The
    // two were the same number until A8 gave the terminal its own grid,
    // and this path reaches the window through windowInfo() rather than
    // composer.geometry.rows - which is why the grep A8's acceptance criterion
    // rests on never saw it.
    //
    // esctest covers DECSLPP too, but it cannot catch this: it sets the
    // window to 27 rows and reads 27 back, and a whole-window terminal
    // answers the same either way. Only a pane shorter than its window
    // tells the two apart.
    STD_TEST(TheDecrqssPageLengthIsThePanesAndNotTheWindows) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput windowPty;
        Vterm& whole = *VtermHeadless::create(composer, nullptr, &windowPty)->terminal();
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4}, panePty, nullptr);

        whole.feedPty(StringView(u8"\x1bP$qt\x1b\\"));
        pane->feedPty(StringView(u8"\x1bP$qt\x1b\\"));

        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"1$r24t")) == 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"1$r4t")) == 1);
        // The window's answer given to the pane - what stood here before.
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"1$r24t")) == 0);
    }

    // T5.1 SS2.7, T5.5: XTWINOPS splits in two and the split is silent
    // when it is wrong. The *text area* reports - CSI 18t in characters
    // and CSI 14t in pixels - are this pane's grid. The *window* reports
    // - CSI 19t (the screen in characters), CSI 14;2t (the drawing
    // surface in pixels), CSI 15t (the screen in pixels) and CSI 16t
    // (the cell) - are the window's, shared by every pane on it, and a
    // pane must answer them with exactly what the whole window answers.
    //
    // Both directions are pinned because both compile and neither
    // crashes. A window report narrowed to the pane tells an application
    // the screen shrank when the window was split; a text-area report
    // widened to the window tells it to draw 80 columns into a 10-column
    // pane. The window here is 80 x 24 and the pane 10 x 4, so every one
    // of these six answers would change if its side of the split moved.
    STD_TEST(AnswersTheWindowReportsAboutTheWindowAndTheTextAreaAboutThePane) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        // XTWINOPS is off by default, so the reports under test would
        // otherwise be answered with silence and every count below would
        // read zero for the right reason and the wrong one. The slot is
        // the supported way to swap the snapshot the core reads.
        VtConfig windowOpsConfig = *composer.vtConfig.config;
        windowOpsConfig.allowWindowOps = true;
        composer.vtConfig.config = &windowOpsConfig;
        CaptureOutput windowPty;
        Vterm& whole = *VtermHeadless::create(composer, nullptr, &windowPty)->terminal();
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        // The pane carries a border of its own, and a wide one, because
        // that is what makes the window reports falsifiable: the two
        // grid* divisions below take the *window's* content insets out
        // of 1920 screen pixels, and swapping this pane's insets in
        // moves the answer by exactly their sum. With both at zero the
        // wrong insets would answer correctly and the test would pass
        // for no reason (T5.1 SS2.7 is precisely that substitution).
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4, .insets = {.top = 7, .right = 7, .bottom = 7, .left = 7}}, panePty, nullptr);

        // The text area, in characters and then in pixels. The headless
        // cell is 1 x 1 px, so the pixel form is the grid again - which
        // is what makes a pane answering the window's 80 x 24 visible.
        whole.feedPty(StringView(u8"\x1b[18t"));
        pane->feedPty(StringView(u8"\x1b[18t"));
        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"8;24;80t")) == 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"8;4;10t")) == 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"8;24;80t")) == 0);

        whole.feedPty(StringView(u8"\x1b[14t"));
        pane->feedPty(StringView(u8"\x1b[14t"));
        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"4;24;80t")) == 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"4;4;10t")) == 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"4;24;80t")) == 0);

        // The window's four. Compared as bytes rather than against a
        // literal: what the headless screen measures is the platform's
        // business, and the contract under test is that the pane repeats
        // the window verbatim - not what either of them says.
        const auto sameAnswer = [&](StringView request) {
            const size_t windowMark = windowPty.bytes.used();
            const size_t paneMark = panePty.sent.used();
            whole.feedPty(request);
            pane->feedPty(request);
            const StringView windowAnswer((const u8*)(windowPty.bytes.data()) + windowMark, windowPty.bytes.used() - windowMark);
            const StringView paneAnswer((const u8*)(panePty.sent.data()) + paneMark, panePty.sent.used() - paneMark);
            // An empty answer would make every comparison below pass by
            // saying nothing at all.
            STD_INSIST(!windowAnswer.empty());
            STD_INSIST(windowAnswer == paneAnswer);
        };
        sameAnswer(StringView(u8"\x1b[19t"));
        sameAnswer(StringView(u8"\x1b[14;2t"));
        sameAnswer(StringView(u8"\x1b[15t"));
        sameAnswer(StringView(u8"\x1b[16t"));

        // And the screen in characters is not this pane's grid, which is
        // what it would collapse to if columnsForPixelWidth() ever read
        // the pane it belongs to instead of the window it divides.
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"9;4;10t")) == 0);
    }

    // A1/A10: the window reports are counted out of the *window's*
    // content insets - the user's border plus what the chrome reserved -
    // and the terminal gets them by asking the embedder for them
    // (VtHost::contentInsets()).
    //
    // This test exists because the fixture above cannot say any of that.
    // Options::border defaults to 0 and nothing there sets a chrome
    // reserve, so composer.contentInsets() is {0,0,0,0} in every other
    // test in this file - and against zero insets a host that answered
    // zeros, a host that had lost the border half of the sum, and a
    // correct one all produce the same bytes. It is the seventh fixture
    // of this merge whose default silently switched the subject off, and
    // the shape is always the same: the parameter that zeroes what is
    // under test is the default, and everybody uses the default.
    //
    // So the reserve here is deliberately non-zero AND asymmetric: a
    // border of 3 on four sides plus 11 points of chrome on the left.
    // Asymmetric because the substitutions this guards against are all
    // sums of a different subset of the same numbers, and a symmetric
    // reserve makes two of them agree by accident.
    STD_TEST(TheWindowReportsCountTheWindowsOwnReserveAndNotThePanesBorder) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        options.border = 3;
        // As above: the reports under test answer with silence otherwise.
        options.vt.allowWindowOps = true;
        composer.setOptions(&options);
        composer.setChromeReserve(ChromeSide::Left, 11);
        CaptureOutput windowPty;
        Vterm& whole = *VtermHeadless::create(composer, nullptr, &windowPty)->terminal();

        // The fixture proves itself before it proves anything else: with
        // any of these four equal to zero, or the left equal to the
        // right, the assertions below would hold for a host that had
        // dropped a term.
        const Insets windowInsets = composer.contentInsets();
        STD_INSIST(windowInsets.left == 14);
        STD_INSIST(windowInsets.right == 3);
        STD_INSIST(windowInsets.top == 3);
        STD_INSIST(windowInsets.bottom == 3);
        // And the pane's own insets are a different number, which is
        // what makes the T5.1 SS2.7 substitution visible rather than
        // harmless: the border alone, with no chrome in it (A10).
        STD_INSIST(windowPane(composer).insets.left == 3);

        // CSI 19t, the screen in characters. The headless screen is
        // 1920 x 1080 at one pixel per cell, so the window's reserve is
        // the whole of the difference: 1920 - (14 + 3) columns and
        // 1080 - (3 + 3) rows.
        whole.feedPty(StringView(u8"\x1b[19t"));
        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"9;1074;1903t")) == 1);
        // What each way of getting it wrong would have answered instead.
        // Zero insets - a contentInsets() that was never implemented:
        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"9;1080;1920t")) == 0);
        // The chrome reserve without the border:
        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"9;1080;1909t")) == 0);
        // This pane's insets in place of the window's:
        STD_INSIST(countOccurrences(windowPty.bytes, StringView(u8"9;1074;1914t")) == 0);

        // CSI 8t, the other direction of the same arithmetic and the
        // other two methods with it: the core turns a grid back into
        // pixels with the window's insets, asks the window for that size
        // and then commits it on the embedder's surface through
        // VtHost::surfaceResized(). Ask for 10 rows of 20 columns and
        // the surface owes 14 + 3 + 20 pixels across and 3 + 3 + 10
        // down - and the grid counted back out of it is the 20 x 10 that
        // was asked for.
        whole.feedPty(StringView(u8"\x1b[8;10;20t"));
        STD_INSIST(composer.geometry.pixelWidth == 37);
        STD_INSIST(composer.geometry.pixelHeight == 16);
        STD_INSIST(composer.geometry.columns == 20);
        STD_INSIST(composer.geometry.rows == 10);
    }

    // A9: the frame carries the grid its rows were built by, so whoever
    // walks row.cells is told how long that array is instead of assuming
    // the window's length. The window here is 80 by 24 and the pane is
    // 10 by 4: filling these in from the composer answers 80 for the
    // pane, which is 70 cells past the end of every row it hands over.
    STD_TEST(CarriesThePaneGridInTheFrameAndNotTheWindows) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Vterm& whole = *VtermHeadless::create(composer, nullptr)->terminal();
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        Vterm& pane = *Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4}, panePty, nullptr);

        whole.expose();
        pane.expose();
        const TerminalUpdate* const wholeUpdate = whole.output();
        const TerminalUpdate* const paneUpdate = pane.output();
        STD_INSIST(wholeUpdate != nullptr);
        STD_INSIST(paneUpdate != nullptr);

        // Both, because a pane that answered 10 by 4 while the whole
        // window also answered 10 by 4 would be a constant, not a grid.
        STD_INSIST(wholeUpdate->gridColumns == composer.geometry.columns);
        STD_INSIST(wholeUpdate->gridRows == composer.geometry.rows);
        STD_INSIST(paneUpdate->gridColumns == 10);
        STD_INSIST(paneUpdate->gridRows == 4);

        // And it is the grid of the rows in this very frame: an exposed
        // terminal damages its whole view, so the count is the height.
        STD_INSIST(paneUpdate->rowCount == paneUpdate->gridRows);
    }

    // A11: the cell-extra store is one per window and it is sized by the
    // sum over the live panes. The list of live panes is the pane tree,
    // which lives in SessionSet - so a Composer with no SessionSet, which
    // is what a headless adapter is, has no panes to sum: a terminal
    // there is the only one there is, and it sizes the store for itself.
    //
    // The window here holds 80 x 24 and the store publishes ten slots per
    // cell through slotBudget(), the only number it exposes. That a
    // second terminal on the same Composer does *not* add to this is the
    // documented limit of a set-less Composer and not the contract:
    // SessionSet::cellCapacityExcept() is what makes the sum exact, and
    // SumsTheExtraStoreBudgetOverEveryLivePane in session_ut.cpp is where
    // that is proved.
    STD_TEST(SizesTheSharedExtraStoreByThePaneWhenThereIsNoPaneList) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless::create(composer, nullptr);

        STD_INSIST(composer.sessions == nullptr);
        const size_t windowCells = (size_t)(composer.geometry.columns) * (composer.geometry.rows + composer.vtConfig.config->saveLines);
        STD_INSIST(windowCells >= (size_t)(80) * 24);
        STD_INSIST(composer.extras.store->slotBudget() >= windowCells * 10);

        // A second terminal sizes the store to its own pane, because with
        // no pane list there is nothing to add it to.
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4}, panePty, nullptr);
        STD_INSIST(pane != nullptr);
        const size_t paneCells = (size_t)(10) * (4 + composer.vtConfig.config->saveLines);
        STD_INSIST(paneCells < windowCells);
        STD_INSIST(composer.extras.store->slotBudget() >= paneCells * 10);
        // And it is that pane's own count and not a leftover of the
        // window's: a budget that had simply stopped being updated would
        // still read the larger number.
        STD_INSIST(composer.extras.store->slotBudget() < windowCells * 10);

        // paneResized is the other door into the same number.
        pane->paneResized({.columns = 8, .rows = 3});
        const size_t shrunkCells = (size_t)(8) * (3 + composer.vtConfig.config->saveLines);
        STD_INSIST(composer.extras.store->slotBudget() >= shrunkCells * 10);
    }

    // A11, the half taken over the screens inside one pane. cellCapacity()
    // is the sum over the screens a terminal actually holds, and the
    // alternate one counts from the moment it is entered - a terminal
    // that has been on the alt screen owns two screens' worth of cells
    // and the shared store has to be sized for both. Dropping that term
    // is silent: nothing fails, the store is merely smaller than the
    // window it serves and collects more often than it should. Measured
    // by mutation in T5.3 - `if (false && altScreenInitialized)` passed
    // the whole suite without a single red.
    //
    // slotBudget() is the only number the store publishes, and asserting
    // it equals cellCapacity() * 10 rather than merely tracking it is
    // deliberate twice over: it pins the whole chain from screen to
    // budget, and it is what says the ceiling has not clamped - a
    // clamped budget would stop being ten per cell and this equality
    // would fail rather than quietly agree.
    STD_TEST(CountsBothScreensOnceThePaneHasEnteredTheAlternate) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        // R6-test: the scrollback is the fixture and not decoration. The
        // headless default is saveLines = 0 (vt_config.h), and with no
        // scrollback the two screens hold the same number of cells - so
        // the sum below reads the same whichever screen the second term
        // came from, and adding the primary's capacity twice instead of
        // the alternate's passed all 963 tests. The product default is
        // 500 (options.cpp), where the two terms differ by a whole
        // scrollback, so the fixture is moved onto the shape the defect
        // would actually be seen in. The config snapshot is swapped
        // before the terminal is made because Screen::createPrimary()
        // reads saveLines once, at construction.
        VtConfig scrollbackConfig = *composer.vtConfig.config;
        scrollbackConfig.saveLines = 500;
        composer.vtConfig.config = &scrollbackConfig;
        CaptureOutput pty;
        Vterm& terminal = *VtermHeadless::create(composer, nullptr, &pty)->terminal();

        const size_t saveLines = composer.vtConfig.config->saveLines;
        const size_t primaryCells = (size_t)(composer.geometry.columns) * (composer.geometry.rows + saveLines);
        // The alternate screen has no scrollback, so it is the grid and
        // not the grid plus saveLines.
        const size_t alternateCells = (size_t)(composer.geometry.columns) * composer.geometry.rows;
        STD_INSIST(alternateCells != 0);
        // ...and the two terms of the sum are different numbers, which is
        // what makes the sum say which screen each one came from.
        STD_INSIST(primaryCells != alternateCells);
        STD_INSIST(terminal.cellCapacity() == primaryCells);
        STD_INSIST(composer.extras.store->slotBudget() == primaryCells * 10);

        terminal.feedPty(StringView(u8"\x1b[?1049h"));

        STD_INSIST(terminal.cellCapacity() == primaryCells + alternateCells);
        STD_INSIST(composer.extras.store->slotBudget() == (primaryCells + alternateCells) * 10);

        // And it is a sum and not a ratchet: the share goes when the
        // screen goes. A budget that only ever grew would keep the
        // alternate's cells for the rest of the terminal's life, which is
        // the last-writer defect of A11 wearing the other sign.
        terminal.feedPty(StringView(u8"\x1b[?1049l"));

        STD_INSIST(terminal.cellCapacity() == primaryCells);
        STD_INSIST(composer.extras.store->slotBudget() == primaryCells * 10);
    }

    // The negative control for the test above, and the reason it is a
    // separate test rather than a preamble: it has to stay green under
    // the mutation that reddens the other one. What it says is that the
    // number answers "is the alternate screen counted" and not "was
    // anything fed at all" - autowrap, plain text, SGR and an OSC 8
    // hyperlink all reach the terminal here, and none of them is a
    // screen, so neither the capacity nor the budget may move.
    STD_TEST(TheExtraStoreBudgetIgnoresInputThatEntersNoScreen) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        Vterm& terminal = *VtermHeadless::create(composer, nullptr, &pty)->terminal();

        const size_t capacity = terminal.cellCapacity();
        const size_t budget = composer.extras.store->slotBudget();
        STD_INSIST(capacity != 0);
        STD_INSIST(budget == capacity * 10);

        terminal.feedPty(StringView(u8"\x1b[?7h"));
        terminal.feedPty(StringView(u8"hello world, a line of ordinary text\r\n"));
        terminal.feedPty(StringView(u8"\x1b[1;32mcoloured\x1b[0m and \x1b]8;;https://x.test\x1b\\linked\x1b]8;;\x1b\\\r\n"));

        STD_INSIST(terminal.cellCapacity() == capacity);
        STD_INSIST(composer.extras.store->slotBudget() == budget);
    }

    // The risk A8 names: resizeGrid reflows the scrollback, rebuilds the
    // screen and reports to the child, so a second idle pass over it
    // sends a resize the shell never asked for. One window resize, one
    // report - counted, because a test that only checked the numbers were
    // right would pass on two identical reports.
    STD_TEST(ReportsOneInBandResizePerWindowResize) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        Vterm& terminal = *VtermHeadless::create(composer, nullptr, &pty)->terminal();
        terminal.feedPty(StringView(u8"\x1b[?2048h"));
        pty.bytes.reset();

        // One pixel size change, one grid change: 100 by 30 at one pixel
        // per cell.
        composer.resize(100, 30);

        STD_INSIST(countOccurrences(pty.bytes, StringView(u8"\x1b[48;")) == 1);
        STD_INSIST(countOccurrences(pty.bytes, StringView(u8"\x1b[48;30;100;30;100t")) == 1);

        // Moving the pane without resizing it: the grid rebuild is
        // skipped, the report to the child is not - that is upstream's
        // contract for an unchanged grid, and it is also where a
        // duplicated pass would show up most quietly. Still exactly one.
        pty.bytes.reset();
        terminal.paneResized({.columns = 100, .rows = 30, .originX = 7, .originY = 3});
        STD_INSIST(countOccurrences(pty.bytes, StringView(u8"\x1b[48;")) == 1);
        STD_INSIST(countOccurrences(pty.bytes, StringView(u8"\x1b[48;30;100;30;100t")) == 1);

        // And a window resize to the size it already has never reaches
        // the terminal at all: Composer filters it, so the child hears
        // nothing rather than hearing the same thing twice.
        pty.bytes.reset();
        composer.resize(100, 30);
        STD_INSIST(pty.bytes.used() == 0);
    }

    // A8, the half no test reached: originX_/originY_ are carried by a
    // real Vterm, not by a MouseGeometry built in a test. Every existing
    // origin test lives in mouse_frontend_ut, where the origin is written
    // straight into the struct - so the four call sites in vterm.cpp that
    // hand originX_/originY_ to mouseGeometry(), and the two lines of
    // paneResized() that store them, were exercised only with zero. A
    // pane that never starts anywhere makes an exchanged pair of axes,
    // or a dropped origin, answer exactly what the correct code answers.
    //
    // The probes below are offset from the pane's own corner by a
    // different number of cells on each axis, and the origin itself is
    // three cells across by two rows down, so neither exchanging the two
    // origins nor dropping them lands on the right answer.
    // PA (R7-test). The store collects over its clients, and a terminal
    // hands over both of its screens - the one on show and the alternate
    // one. Nothing checked the second: dropping frame_alt from
    // VtermImpl::collectExtras() passed all 876 tests.
    //
    // The case that matters is an alternate screen that is no longer on
    // show but still holds its cells: DECSET 47 keeps the alternate
    // buffer when it is left, so a terminal that ran vim and came back
    // still owns those refs. An implementation that asked only "which
    // screen is this terminal showing" would free them under it - the
    // same dangling read the shared store was repaired for, one level
    // down.
    STD_TEST(AnInactiveAlternateScreensCellsSurviveACollection) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Vterm& terminal = *VtermHeadless::create(composer, nullptr)->terminal();

        // Into the alternate screen, and a cluster too big to sit inline
        // in a cell, so the cell holds a ref into the shared store.
        terminal.feedPty(StringView(u8"\x1b[?47h"));
        terminal.feedPty(StringView(u8"b\xcc\x82\xcc\x83"));
        terminal.expose();
        const TerminalUpdate* const inAlt = terminal.output();
        STD_INSIST(inAlt != nullptr);
        STD_INSIST(inAlt->rowCount != 0);
        const TerminalCell* const altCell = &inAlt->rows[0].cells[0];
        // The premise: without an extra there is nothing to lose and this
        // test would pass on any code at all.
        STD_INSIST(altCell->hasExtra());
        const size_t clusterSize = composer.extras.store->grapheme(*altCell).size();
        STD_INSIST(clusterSize == 3);
        terminal.consume();

        // Back to the primary screen. Mode 47 leaves the alternate
        // buffer alone, so those cells are still out there, held by a
        // screen nobody is looking at.
        terminal.feedPty(StringView(u8"\x1b[?47l"));
        terminal.expose();
        const TerminalUpdate* const inPrimary = terminal.output();
        STD_INSIST(inPrimary != nullptr);
        STD_INSIST(&inPrimary->rows[0].cells[0] != altCell);
        terminal.consume();

        CellExtraStore* const before = composer.extras.store;
        Vector<TerminalCell*> none;
        before->collect(none, nullptr, 0);
        // A collection really happened: collect() publishes a
        // replacement store. Without this the test passes on a build
        // that never collects, which is the shape every "the data
        // survived" check has.
        STD_INSIST(composer.extras.store != before);

        CellExtraStore* const store = composer.extras.store;
        STD_INSIST(altCell->hasExtra());
        STD_INSIST(store->grapheme(*altCell).size() == clusterSize);
        STD_INSIST(store->grapheme(*altCell)[0] == 'b');
        STD_INSIST(store->grapheme(*altCell)[2] == 0x0303);
    }

    // PB (R7-test). A terminal hands the collection two things: the
    // cells of its screens, and its roots - refs it holds outside any
    // cell. The open hyperlink is one of those. Dropping it from
    // VtermImpl::collectExtras() passed all 876 tests.
    //
    // CollectionRewritesNonCellRoots next door checks that the store
    // *can* rewrite a root it is handed. That is a different sentence
    // from "the terminal hands it over", and the name covers both.
    //
    // The consequence is not a cell that reads wrong now, but every cell
    // written after the collection: they are stamped with the root, and
    // a root left pointing into the dead store stamps them with whatever
    // moved into that slot.
    STD_TEST(TheOpenHyperlinkSurvivesACollectionAndKeepsStampingCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Vterm& terminal = *VtermHeadless::create(composer, nullptr)->terminal();

        // A second, unreferenced link first: it dies in the collection,
        // which is what moves the surviving refs and makes a stale root
        // point at something rather than at nothing.
        terminal.feedPty(StringView(u8"\x1b]8;;https://dead.test\x1b\\"));
        terminal.feedPty(StringView(u8"\x1b]8;;\x1b\\"));
        // Now the link that stays open across the collection.
        terminal.feedPty(StringView(u8"\x1b]8;;https://live.test\x1b\\"));
        terminal.feedPty(StringView(u8"A"));
        terminal.expose();
        const TerminalUpdate* const before = terminal.output();
        STD_INSIST(before != nullptr);
        STD_INSIST(before->rowCount != 0);
        const TerminalCell* const stamped = &before->rows[0].cells[0];
        STD_INSIST(stamped->hasExtra());
        STD_INSIST(composer.extras.store->hyperlink(*stamped) == StringView(u8"https://live.test"));
        terminal.consume();

        CellExtraStore* const previous = composer.extras.store;
        Vector<TerminalCell*> none;
        previous->collect(none, nullptr, 0);
        STD_INSIST(composer.extras.store != previous);
        // The premise of the whole test: the collection really moved
        // things, so a root that was not carried across is now pointing
        // at a live slot belonging to somebody else.
        STD_INSIST(composer.extras.store->findHyperlink(StringView(u8"uri=https://dead.test")) == 0);

        // The link is still open, so the next cell the shell writes is
        // stamped with it - through the root, which is the thing under
        // test.
        terminal.feedPty(StringView(u8"B"));
        terminal.expose();
        const TerminalUpdate* const after = terminal.output();
        STD_INSIST(after != nullptr);
        CellExtraStore* const store = composer.extras.store;
        const TerminalCell* const stampedAfter = &after->rows[0].cells[1];
        STD_INSIST(stampedAfter->hasExtra());
        STD_INSIST(store->hyperlink(*stampedAfter) == StringView(u8"https://live.test"));
        // And the cell written before it still says the same thing, so
        // the two ends of the collection agree.
        STD_INSIST(store->hyperlink(*stamped) == StringView(u8"https://live.test"));
    }

    // PC (R7-test). The terminal's second root: the hyperlink captured
    // when a grapheme cluster opened, kept so that the marks arriving
    // after it are stamped with the link the base character had. It sits
    // outside every cell for as long as the cluster is being assembled,
    // and dropping it from VtermImpl::collectExtras() passed all 876
    // tests.
    //
    // So the collection has to land *inside* a cluster - after the base
    // character and before its combining mark. That is not a contrived
    // moment: a collection runs when the store fills, which is driven by
    // the shell's output, not by cluster boundaries.
    STD_TEST(TheHyperlinkOfAnOpenClusterSurvivesACollection) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Vterm& terminal = *VtermHeadless::create(composer, nullptr)->terminal();

        // A link that nothing will reference, so the collection has a
        // slot to free and the surviving refs actually move.
        terminal.feedPty(StringView(u8"\x1b]8;;https://dead.test\x1b\\"));
        terminal.feedPty(StringView(u8"\x1b]8;;\x1b\\"));
        terminal.feedPty(StringView(u8"\x1b]8;;https://live.test\x1b\\"));
        // The base character opens a cluster and takes a copy of the
        // link with it. Nothing closes the cluster yet - and nothing
        // may: an OSC between the base character and its marks ends the
        // cluster, which is how the first version of this test came to
        // assert on a cluster that had never grown.
        terminal.feedPty(StringView(u8"b"));

        CellExtraStore* const previous = composer.extras.store;
        Vector<TerminalCell*> none;
        previous->collect(none, nullptr, 0);
        STD_INSIST(composer.extras.store != previous);
        // The premise: the collection moved things, so a root not
        // carried across now names a slot that belongs to somebody else.
        STD_INSIST(composer.extras.store->findHyperlink(StringView(u8"uri=https://dead.test")) == 0);

        // The mark joins the cluster opened before the collection, and
        // the cell is rewritten with the cluster's saved link.
        terminal.feedPty(StringView(u8"\xcc\x82\xcc\x83"));
        terminal.expose();
        const TerminalUpdate* const after = terminal.output();
        STD_INSIST(after != nullptr);
        STD_INSIST(after->rowCount != 0);
        const TerminalCell* const cell = &after->rows[0].cells[0];
        CellExtraStore* const store = composer.extras.store;
        STD_INSIST(cell->hasExtra());
        // Both halves: the cluster grew, and it kept its link.
        STD_INSIST(store->grapheme(*cell).size() == 3);
        STD_INSIST(store->hyperlink(*cell) == StringView(u8"https://live.test"));
    }

    // Audit finding 6, R7-test. Two lines in vterm.cpp pass the pane's
    // origin to the mouse frontend - selectionPoint() and
    // currentSelectionAutoscrollDirection() - and neither was executed by
    // anything: T12 put __builtin_trap() in both and the suite stayed
    // green. So this is not a weak oracle, it is no execution at all, and
    // no mutation there could ever be caught.
    //
    // The pointer tests below this one drive the *reporting* path, which
    // a shell turns on with DECSET 1000. Selection is the other path -
    // the one taken when the application is not reading the mouse - and
    // it has its own translation from pixels to a cell. Today every pane
    // starts at zero, so mixing the two coordinate systems changes
    // nothing; wave 8 is where it starts costing, and it is Q1 of wave 5
    // over again, in the mouse this time.
    STD_TEST(SelectionStartsInTheCellThePaneOwnsAndNotTheWindows) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless::create(composer, nullptr);
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        const int glyphWidth = composer.geometry.cellPixelWidth;
        const int glyphHeight = composer.geometry.cellPixelHeight;
        const int originX = 3 * glyphWidth;
        const int originY = 2 * glyphHeight;
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4, .originX = originX, .originY = originY, .width = 10 * glyphWidth, .height = 4 * glyphHeight}, panePty, nullptr);
        STD_INSIST(pane != nullptr);
        // No DECSET 1000 here on purpose: with reporting off the press
        // starts a selection instead of being sent to the child, and the
        // selection is what carries the second translation.
        pane->focus(true);
        pane->pointerPresence(true);

        const Insets insets = composer.contentInsets();
        const auto at = [&](int cellsAcross, int cellsDown, int dx, int dy) {
            return plt::PointerButtonInput{
                plt::PointerButton::Primary,
                true,
                insets.left + originX + cellsAcross * glyphWidth + dx,
                insets.top + originY + cellsDown * glyphHeight + dy,
                0,
                0.0,
            };
        };

        // Press in the middle of the pane's cell (2, 1) and drag to (5, 1).
        pane->pointerButton(at(2, 1, glyphWidth / 2, glyphHeight / 2));
        pane->pointerMotion({insets.left + originX + 5 * glyphWidth + glyphWidth / 2, insets.top + originY + 1 * glyphHeight + glyphHeight / 2, 0});

        pane->expose();
        const TerminalUpdate* const update = pane->output();
        STD_INSIST(update != nullptr);
        // The pane's own cell, counted from the pane's own corner. An
        // origin dropped on the way answers (5, 3) here - three columns
        // and two rows further in, which is exactly the origin expressed
        // in cells, and still inside this 10 x 4 grid, so the wrong answer
        // looks every bit as valid as the right one.
        STD_INSIST(update->selection.tl.x == 2);
        STD_INSIST(update->selection.tl.y == 1);
        STD_INSIST(update->selection.tl.x != 5);
        STD_INSIST(update->selection.tl.y != 3);
        // And the far end travelled the three cells the pointer did,
        // which says the same translation was applied twice and not once.
        STD_INSIST(update->selection.br.x == 5);
        STD_INSIST(update->selection.br.y == 1);
    }

    // The other of the two lines. Autoscroll asks "is the pointer past
    // the edge of my grid", and the edge is the pane's top, not the
    // window's content top. The y used here sits between the two: inside
    // the window's content box, above the pane. Honouring the origin
    // makes that "above the pane" and scrolls the view back into history;
    // dropping it makes the same pixel an ordinary row of the pane and
    // scrolls nothing.
    STD_TEST(AutoscrollMeasuresFromThePanesTopEdgeAndNotTheWindows) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        // A scrollback, or there is nowhere for the view to scroll to and
        // scrollView() refuses whichever direction it is handed.
        options.vt.saveLines = 200;
        composer.setOptions(&options);
        VtermHeadless::create(composer, nullptr);
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        const int glyphWidth = composer.geometry.cellPixelWidth;
        const int glyphHeight = composer.geometry.cellPixelHeight;
        const int originX = 3 * glyphWidth;
        const int originY = 2 * glyphHeight;
        CaptureTestApi trace;
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4, .originX = originX, .originY = originY, .width = 10 * glyphWidth, .height = 4 * glyphHeight}, panePty, &trace);
        STD_INSIST(pane != nullptr);
        STD_INSIST(trace.testApi != nullptr);
        pane->focus(true);
        pane->pointerPresence(true);

        // Enough output to put rows into the scrollback, so scrolling the
        // view back is a thing that can happen at all.
        for (unsigned line = 0; line < 40; ++line) {
            pane->feedPty(StringView(u8"line\r\n"));
        }
        pane->expose();
        const TerminalUpdate* const settled = pane->output();
        STD_INSIST(settled != nullptr);
        STD_INSIST(settled->historyRows != 0);
        STD_INSIST(settled->viewOffset == 0);
        pane->consume();

        const Insets insets = composer.contentInsets();
        // A selection has to be running, with a button down, or the
        // direction is refused before the geometry is ever consulted.
        pane->pointerButton({plt::PointerButton::Primary, true, insets.left + originX + glyphWidth / 2, insets.top + originY + 2 * glyphHeight, 0, 0.0});
        pane->pointerMotion({insets.left + originX + 4 * glyphWidth, insets.top + originY + glyphHeight / 2, 0});
        STD_INSIST(trace.testApi->hasSelection());

        // The pixel between the two edges: below the window's content
        // top, above the pane's.
        const int between = insets.top + originY - glyphHeight / 2;
        STD_INSIST(between > insets.top);
        STD_INSIST(between <= insets.top + originY);
        pane->pointerMotion({insets.left + originX + 4 * glyphWidth, between, 0});

        // Forced rather than waited for: the production path parks a
        // fiber on a deadline, and a test that slept for it would be
        // slow and flaky at once.
        STD_INSIST(trace.testApi->advanceSelectionAutoscroll());

        pane->expose();
        const TerminalUpdate* const scrolled = pane->output();
        STD_INSIST(scrolled != nullptr);
        // Scrolled back into history. Counted from the window's top the
        // same pixel is an ordinary row of this pane, the direction is
        // zero, and the view stays where it was.
        STD_INSIST(scrolled->viewOffset != 0);
    }

    STD_TEST(PointerReportsCountFromTheOriginTheVtermWasGiven) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless::create(composer, nullptr);
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        const int glyphWidth = composer.geometry.cellPixelWidth;
        const int glyphHeight = composer.geometry.cellPixelHeight;
        const int originX = 3 * glyphWidth;
        const int originY = 2 * glyphHeight;
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4, .originX = originX, .originY = originY, .width = 10 * glyphWidth, .height = 4 * glyphHeight}, panePty, nullptr);

        // VT200 button reporting with SGR coordinates: one report per
        // press, naming the cell in one line.
        pane->feedPty(StringView(u8"\x1b[?1000h\x1b[?1006h"));
        panePty.sent.reset();

        const Insets insets = composer.contentInsets();
        const auto press = [&](int cellsAcross, int cellsDown) {
            pane->pointerButton({
                plt::PointerButton::Primary,
                true,
                insets.left + originX + cellsAcross * glyphWidth,
                insets.top + originY + cellsDown * glyphHeight,
                0,
                0.0,
            });
        };

        // The pane's own first cell is 1;1 to its child, however far into
        // the window the pane begins.
        press(0, 0);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"\x1b[<0;1;1M")) == 1);

        // Four cells across and one down: exchanging the two origins
        // answers column 4 here, dropping them answers column 8.
        panePty.sent.reset();
        press(4, 1);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"\x1b[<0;5;2M")) == 1);

        // And down the other axis, where dropping the origin answers row
        // 5 instead of 3.
        panePty.sent.reset();
        press(0, 2);
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"\x1b[<0;1;3M")) == 1);
    }

    // The origin does not only arrive at birth: paneResized carries it
    // too, and its two assignments were the other half with no coverage.
    // Moving the pane without changing its grid has to move every pointer
    // report with it - the same press names a different cell afterwards.
    STD_TEST(MovingThePaneMovesWhereItsPointerReportsCountFrom) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless::create(composer, nullptr);
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        const int glyphWidth = composer.geometry.cellPixelWidth;
        const int glyphHeight = composer.geometry.cellPixelHeight;
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4, .width = 10 * glyphWidth, .height = 4 * glyphHeight}, panePty, nullptr);
        pane->feedPty(StringView(u8"\x1b[?1000h\x1b[?1006h"));

        const Insets insets = composer.contentInsets();
        const int pixelX = insets.left + 5 * glyphWidth;
        const int pixelY = insets.top + 3 * glyphHeight;
        const auto press = [&]() {
            pane->pointerButton({plt::PointerButton::Primary, true, pixelX, pixelY, 0, 0.0});
        };

        panePty.sent.reset();
        press();
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"\x1b[<0;6;4M")) == 1);

        // Same grid, new origin: three cells across and one row down.
        // The two offsets differ, so an implementation that stored one of
        // them into both fields answers something else.
        pane->paneResized({.columns = 10, .rows = 4, .originX = 3 * glyphWidth, .originY = 1 * glyphHeight, .width = 10 * glyphWidth, .height = 4 * glyphHeight});
        panePty.sent.reset();
        press();
        STD_INSIST(countOccurrences(panePty.sent, StringView(u8"\x1b[<0;3;3M")) == 1);
    }

    // T5.4 section 5.4 left this half of A5 with no observer at all:
    // pointerPresence() writes no byte of its own - it resets the mouse
    // frontend and asks for a redraw - so a mutation that handed it to
    // the wrong terminal kept all 953 tests green. The pane's own
    // reporting path is where it does become visible to the child.
    //
    // Any-event tracking reports a move only when it changes cell, so a
    // second move to the same cell is silent. resetMotion(), inside
    // pointerPresence(), drops that memory - and the very same move is
    // reported again. One sequence in the child's stream, present
    // exactly when the pointer's arrival reached this terminal.
    STD_TEST(PointerPresenceIsVisibleToTheChildThroughTheMotionFilter) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless::create(composer, nullptr);
        auto& panePty = *composer.pool->make<SecondPtyStub>(composer);
        const int glyphWidth = composer.geometry.cellPixelWidth;
        const int glyphHeight = composer.geometry.cellPixelHeight;
        Vterm* const pane = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, {.columns = 10, .rows = 4, .width = 10 * glyphWidth, .height = 4 * glyphHeight}, panePty, nullptr);
        STD_INSIST(pane != nullptr);
        pane->focus(true);
        pane->pointerPresence(true);

        // Any-event motion tracking with SGR coordinates: a move is
        // reported with no button held, which is what the pointer merely
        // crossing a pane does.
        pane->feedPty(StringView(u8"\x1b[?1003h\x1b[?1006h"));

        const Insets insets = composer.contentInsets();
        const StringView report(u8"\x1b[<35;3;2M");
        const auto moveToTheSameCell = [&]() {
            pane->pointerMotion({insets.left + 2 * glyphWidth, insets.top + 1 * glyphHeight, 0});
        };

        panePty.sent.reset();
        moveToTheSameCell();
        STD_INSIST(countOccurrences(panePty.sent, report) == 1);

        // The filter itself, asserted before it is used as an oracle: a
        // fixture where the second move reported anyway would make the
        // check below pass with pointerPresence() gutted.
        panePty.sent.reset();
        moveToTheSameCell();
        STD_INSIST(countOccurrences(panePty.sent, report) == 0);

        // The pointer leaves the window and comes back. The grid did not
        // move, the mode did not change, and neither call writes a byte -
        // but the filter no longer claims to know where the pointer was.
        pane->pointerPresence(false);
        pane->pointerPresence(true);
        panePty.sent.reset();
        moveToTheSameCell();
        STD_INSIST(countOccurrences(panePty.sent, report) == 1);
    }

    // Upstream's test, on our create(): VtermHeadless::create() takes a
    // Composer here rather than a pool and a config (task A), so the
    // scrollback cap is set on the config snapshot the composer carries
    // - before the terminal is made, because Screen::createPrimary()
    // reads saveLines once, at construction.
    // Upstream's ForegroundProcessFillsTheTitleFallback on our create():
    // VtermHeadless::create() takes a Composer rather than a pool and a
    // config (task A), Vterm::create() takes the pane, and SecondPtyStub
    // reaches its scheduler through the composer. The host is upstream's
    // TitleCaptureHost, wrapping the one the headless harness built.
    STD_TEST(ForegroundProcessFillsTheTitleFallback) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless* const headless = VtermHeadless::create(composer, nullptr);
        TitleCaptureHost& host = *composer.pool->make<TitleCaptureHost>(headless->host());
        SecondPtyStub& pty = *composer.pool->make<SecondPtyStub>(composer);
        Vterm* const terminal = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, host, windowPane(composer), pty, nullptr);

        // Premise, before any behavior is asked of it: the two groups
        // the test switches between are real, distinct processes, so a
        // name change is a change and not the same name twice. Without
        // this the last block would pass on a stub that never looked.
        STD_INSIST(getpid() != getppid());
        STD_INSIST(getpid() > 0 && getppid() > 0);

        // Without a foreground the refresh is a no-op.
        terminal->refreshForegroundName();
        STD_INSIST(host.lastTitle.length() == 0);

        // The observation names the empty title after the foreground.
        pty.group = getpid();
        terminal->refreshForegroundName();
        STD_INSIST(host.lastTitle.length() != 0);
        const Buffer own{StringView(host.lastTitle)};

        // An application's title beats the fallback and stands while
        // the foreground name is stable.
        const u8 osc[] = {0x1b, ']', '2', ';', 'm', 'c', ' ', 't', 'i', 't', 'l', 'e', 0x07};
        terminal->feedPty(StringView(osc, sizeof(osc)));
        STD_INSIST(StringView(host.lastTitle) == StringView(u8"mc title"));
        terminal->refreshForegroundName();
        STD_INSIST(StringView(host.lastTitle) == StringView(u8"mc title"));

        // A change of the foreground name retires whatever stood.
        pty.group = getppid();
        terminal->refreshForegroundName();
        STD_INSIST(host.lastTitle.length() != 0);
        STD_INSIST(StringView(host.lastTitle) != StringView(u8"mc title"));
        STD_INSIST(StringView(host.lastTitle) != StringView(own));
    }

    STD_TEST(ScrollViewMovesClampsAndReturnsTheOffset) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtConfig scrollbackConfig = *composer.vtConfig.config;
        scrollbackConfig.saveLines = 10;
        composer.vtConfig.config = &scrollbackConfig;
        Vterm* const terminal = VtermHeadless::create(composer, nullptr)->terminal();
        const int fedLines = 40;
        for (int line = 0; line < fedLines; ++line) {
            const u8 text[] = {'x', '\r', '\n'};
            terminal->feedPty(StringView(text, sizeof(text)));
        }

        // The premise, before the behaviour: the cap has to be smaller
        // than what was fed, or "clamped to ten" and "kept everything" are the same number and
        // the clamp below would hold with no clamp in the code at all.
        // The headless default is saveLines = 0, where every assertion
        // here reads back 0 and the test is vacuous.
        STD_INSIST(scrollbackConfig.saveLines == 10);
        STD_INSIST((int)(scrollbackConfig.saveLines) < fedLines);

        STD_INSIST(terminal->scrollView(0) == 0);
        STD_INSIST(terminal->scrollView(3) == 3);
        STD_INSIST(terminal->scrollViewTo(3) == 3);
        STD_INSIST(terminal->scrollView(100) == 10);
        STD_INSIST(terminal->scrollView(-2) == 8);
        STD_INSIST(terminal->scrollViewTo(0) == 0);
        STD_INSIST(terminal->scrollViewTo(9999) == 10);

        // The alternate screen keeps no history; the view cannot move.
        const u8 alt[] = {'\x1b', '[', '?', '1', '0', '4', '9', 'h'};
        terminal->feedPty(StringView(alt, sizeof(alt)));
        STD_INSIST(terminal->scrollView(5) == 0);
        STD_INSIST(terminal->scrollViewTo(5) == 0);
    }

    STD_TEST(KeepsFallbackTitleForTerminalReset) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Vterm* const terminal = VtermHeadless::create(composer, nullptr)->terminal();
        const u8 reset[] = {'\x1b', 'c'};

        terminal->feedPty(StringView(reset, sizeof(reset)));

        STD_INSIST(terminal->output() != nullptr);
    }

    STD_TEST(KeepsDoubleWidthOutputIndependentOfPtyChunking) {
        // Record format is the fuzz target's [op, size, pty bytes] stream.
        // All three records are pty input; the split form mirrors main_fuzz.
        const u8 corpus[] = {
            0x00,
            0x41,
            0x1b,
            0x23,
            0x36,
            0xd7,
            0x31,
            0x67,
            0x1b,
            0x5b,
            0x31,
            0x30,
            0x30,
            0x49,
            0x1b,
            0x5b,
            0x34,
            0x37,
            0x5a,
            0x1b,
            0x5b,
            0x35,
            0x38,
            0x3b,
            0x35,
            0x3b,
            0x32,
            0x33,
            0x33,
            0x3b,
            0x32,
            0x35,
            0x3b,
            0x36,
            0x38,
            0x3b,
            0x34,
            0x3a,
            0x35,
            0x3b,
            0x34,
            0x38,
            0x3b,
            0xa4,
            0x35,
            0x3b,
            0x34,
            0x38,
            0x6d,
            0x1b,
            0x5b,
            0x31,
            0x32,
            0x3b,
            0x33,
            0x36,
            0x48,
            0x1b,
            0x5b,
            0x33,
            0x37,
            0x42,
            0x00,
            0x3d,
            0x1b,
            0x5b,
            0x34,
            0x3b,
            0x32,
            0x24,
            0x70,
            0x1b,
            0x48,
            0x1b,
            0x5b,
            0x31,
            0x67,
            0x1b,
            0x5b,
            0x32,
            0x37,
            0x49,
            0x1b,
            0x5b,
            0x39,
            0x30,
            0x5a,
            0x1b,
            0x5b,
            0x3f,
            0x32,
            0x4a,
            0x1b,
            0x5b,
            0x33,
            0x31,
            0x4c,
            0x1b,
            0x5b,
            0x3f,
            0x36,
            0x39,
            0x68,
            0x1b,
            0x5b,
            0x31,
            0x38,
            0x3b,
            0x32,
            0x38,
            0x72,
            0x1b,
            0x5b,
            0x33,
            0x33,
            0x3b,
            0x36,
            0x38,
            0x73,
            0x1b,
            0x3b,
            0x5b,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0xe1,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x61,
            0x0a,
            0x1b,
            0x5d,
            0x31,
            0x31,
            0x30,
            0x3b,
            0x72,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x30,
            0x4a,
            0x1b,
            0x5b,
            0x33,
            0x31,
            0x54,
        };
        auto wholePool = ObjPool::fromMemory();
        auto splitPool = ObjPool::fromMemory();
        Composer& wholeComposer = *wholePool->make<Composer>(wholePool.mutPtr());
        Composer& splitComposer = *splitPool->make<Composer>(splitPool.mutPtr());
        Vterm& whole = *VtermHeadless::create(wholeComposer, nullptr)->terminal();
        Vterm& split = *VtermHeadless::create(splitComposer, nullptr)->terminal();
        discardOutput(whole);
        discardOutput(split);

        size_t offset = 0;
        while (offset + 2 <= sizeof(corpus)) {
            const u8 op = corpus[offset++];
            const size_t size = corpus[offset++];
            STD_INSIST(op < 192);
            STD_INSIST(offset + size <= sizeof(corpus));
            whole.feedPty(StringView(corpus + offset, size));
            feedInFuzzChunks(split, corpus + offset, size);
            insistMatchingCursor(whole, split);
            offset += size;
        }
        STD_INSIST(offset == sizeof(corpus));
    }

    STD_TEST(KeepsUtf8GraphemeInputIndependentOfPtyChunking) {
        // Saved fuzz state: the final record splits a ZWJ sequence after a
        // wide glyph wraps into, then is discarded by, a double-width row.
        const u8 corpus[] = {
            0x68,
            0x65,
            0x1b,
            0x5b,
            0x64,
            0x1b,
            0x08,
            0x0b,
            0x1b,
            0x23,
            0x33,
            0x31,
            0x34,
            0x31,
            0x3b,
            0x2b,
            0x58,
            0x5b,
            0x35,
            0x38,
            0x3b,
            0x35,
            0x3b,
            0x31,
            0x31,
            0xc6,
            0xc4,
            0xce,
            0xc7,
            0xc4,
            0xcc,
            0xc7,
            0xc4,
            0x32,
            0x3b,
            0x31,
            0x34,
            0x00,
            0x06,
            0x1b,
            0x5b,
            0x3f,
            0x36,
            0x39,
            0x68,
            0x68,
            0x0e,
            0x1b,
            0x5b,
            0x33,
            0x34,
            0x3b,
            0x33,
            0x36,
            0x73,
            0x00,
            0x35,
            0x1b,
            0x5b,
            0x3f,
            0x36,
            0x39,
            0x68,
            0x1b,
            0x5b,
            0x35,
            0x3b,
            0x33,
            0x30,
            0x72,
            0x1b,
            0x5b,
            0x35,
            0x31,
            0x3b,
            0x36,
            0x32,
            0x73,
            0x00,
            0x04,
            0x1b,
            0x5b,
            0x35,
            0x6e,
            0xb1,
            0x02,
            0x8d,
            0x23,
            0x00,
            0x55,
            0x1b,
            0x5b,
            0x31,
            0x35,
            0x3b,
            0x31,
            0x39,
            0x48,
            0x1b,
            0x5b,
            0x33,
            0x32,
            0x44,

            0x33,
            0x48,
            0x1b,
            0x5b,
            0x35,
            0x31,
            0x47,
            0x1b,
            0x5b,
            0x3f,
            0x36,
            0x68,
            0x1b,
            0x5b,
            0x5b,
            0x31,
            0x23,
            0x0f,
            0x9f,
            0x91,
            0x80,
            0x8d,
            0xd0,
            0x6c,
            0x68,
            0x1b,
            0x5b,
            0x34,
            0x68,
            0x65,
            0x1b,
            0x5b,
            0x64,
            0x1b,
            0x08,
            0x0b,
            0x1b,
            0x23,
            0x33,
            0x31,
            0x34,
            0x31,
            0x3b,
            0x2b,
            0x35,
            0x6c,
            0x1b,
            0x5b,
            0x32,
            0x30,
            0x68,
            0x1b,
            0x23,
            0x34,
            0x80,
            0xfe,
            0x09,
            0x1b,
            0x5b,
            0x37,
            0x3b,
            0x31,
            0x38,
            0x33,
            0x48,
            0x31,
            0x47,
            0x1b,
            0x5b,
            0x3f,
            0x36,
            0x68,
            0xf0,
            0x5b,

            0x32,
            0x30,
            0x68,
            0xd7,
            0x90,
            0x0d,
            0x0a,
            0x00,
            0x3e,
            0x1b,
            0x5b,
            0x3f,
            0x32,
            0x4b,
            0x1b,
            0x5b,
            0x31,
            0x36,
            0x49,
            0xf0,
            0x9f,
            0x91,
            0xa9,
            0xe2,
            0x80,
            0x8d,
            0xf0,
            0x9f,
            0x95,
            0xa9,
            0xe2,
            0x80,
            0x8d,
            0xf0,
            0x9f,
            0x91,
            0x1b,
            0x5b,
            0x5b,
            0x3f,
            0x31,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x51,
            0x30,
            0x68,
        };
        auto wholePool = ObjPool::fromMemory();
        auto splitPool = ObjPool::fromMemory();
        Composer& wholeComposer = *wholePool->make<Composer>(wholePool.mutPtr());
        Composer& splitComposer = *splitPool->make<Composer>(splitPool.mutPtr());
        Vterm& whole = *VtermHeadless::create(wholeComposer, nullptr)->terminal();
        Vterm& split = *VtermHeadless::create(splitComposer, nullptr)->terminal();
        discardOutput(whole);
        discardOutput(split);

        size_t offset = 0;
        while (offset + 2 <= sizeof(corpus)) {
            const u8 op = corpus[offset++];
            const size_t size = corpus[offset++];
            STD_INSIST(op < 192);
            STD_INSIST(offset + size <= sizeof(corpus));
            whole.feedPty(StringView(corpus + offset, size));
            feedInFuzzChunks(split, corpus + offset, size);
            insistMatchingCursor(whole, split);
            offset += size;
        }
        STD_INSIST(offset == sizeof(corpus));
    }

    STD_TEST(PtyAndTerminalOutputsAreConsumedIndependently) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        Vterm& terminal = *VtermHeadless::create(composer, nullptr, &pty)->terminal();
        if (terminal.output() != nullptr) {
            terminal.consume();
        }
        const u8 input[] = {'a', 0x1b, '[', 'c'};

        terminal.feedPty(StringView(input, sizeof(input)));

        STD_INSIST(!pty.bytes.empty());
        STD_INSIST(terminal.output() != nullptr);
        pty.bytes.reset();
        STD_INSIST(pty.bytes.empty());
        STD_INSIST(terminal.output() != nullptr);
        terminal.consume();
        STD_INSIST(terminal.output() == nullptr);
    }

    STD_TEST(FeedConsumesTerminalAndPtyOutput) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        VtermHeadless* const headless = VtermHeadless::create(composer, nullptr, &pty);
        const u8 input[] = {'a', 0x1b, '[', 'c'};

        headless->feed(input, sizeof(input));

        STD_INSIST(!pty.bytes.empty());
        STD_INSIST(headless->terminal()->output() == nullptr);

        pty.bytes.reset();
        headless->feed(input, sizeof(input));

        STD_INSIST(!pty.bytes.empty());
        STD_INSIST(headless->terminal()->output() == nullptr);
    }

    STD_TEST(RawDeviceAttributesDoesNotProducePtyOutputInUtf8Mode) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        Vterm* const terminal = VtermHeadless::create(composer, nullptr, &pty)->terminal();
        const u8 rawDeviceAttributes = 0x9a;

        terminal->feedPty(StringView(&rawDeviceAttributes, 1));

        STD_INSIST(pty.bytes.empty());
        terminal->feedPty(StringView(u8"\x1bZ"));
        STD_INSIST(!pty.bytes.empty());
    }

    STD_TEST(RawDeviceAttributesWorksInSingleByteMode) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        Vterm* const terminal = VtermHeadless::create(composer, nullptr, &pty)->terminal();
        const u8 input[] = {'\x1b', '%', '@', 0x9a};

        terminal->feedPty(StringView(input, sizeof(input)));

        STD_INSIST(!pty.bytes.empty());
    }

    STD_TEST(BulkUtf8DecoderMatchesByteWiseDecoder) {
        // The whole-buffer feed decodes through placeUtf8Run, tiny feeds
        // through Utf8Decoder::pushByte.  Screens must match cell for cell
        // for every replacement-character rule and chunk-boundary split.
        const u8 directed[] =
            // Valid 2-, 3- and 4-byte sequences with edge codepoints.
            u8"A\xc3\xa9 \xe2\x82\xac \xf0\x9f\x92\xbb "
            u8"\xe0\xa0\x80 \xed\x9f\xbf \xf4\x8f\xbf\xbf Z\r\n"
            // Stray continuations: the C1 range resets grapheme input.
            u8"\x80\x9f\xa0\xbf Z\r\n"
            // Bytes that can never begin a sequence.
            u8"\xc0\xc1\xf5\xff Z\r\n"
            // Overlong, surrogate and beyond-U+10FFFF first continuations.
            u8"\xe0\x80 \xe0\x9f \xed\xa0 \xf0\x80 \xf4\x90 Z\r\n"
            // Leads truncated at every position.
            u8"\xc2Z \xe2Z \xe2\x82Z \xf0Z \xf0\x90Z \xf0\x90\x8fZ\r\n"
            // Combining, wide and joined clusters against garbage.
            u8"e\xcc\x81 \xe4\xbd\xa0 \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb \x80\xcc\x81 Z\r\n"
            // Controls inside a pending sequence are transparent to the
            // streaming decoder: the sequence completes around them.
            u8"\xe2\x07\x82\xac \xc3\x07\xa9 \xe2\x82\x07\xac \xf0\x9f\x00\x92\xbb \xe2\x7f\x82\xac Z\r\n";

        // Deterministic garbage over the full byte range except ESC: mode
        // and charset changes are covered by directed tests elsewhere.
        u8 garbage[4096];
        u32 state = 0x2545f491;
        for (size_t index = 0; index < sizeof(garbage); ++index) {
            state = state * 747796405u + 2891336453u;
            const u8 byte = (u8)(state >> 24);
            garbage[index] = byte == 0x1b ? 0x20 : byte;
        }

        const auto compareScreens = [](Vterm& whole, Vterm& split, u16 columns) {
            whole.expose();
            split.expose();
            const TerminalUpdate* const wholeUpdate = whole.output();
            const TerminalUpdate* const splitUpdate = split.output();
            STD_INSIST(wholeUpdate != nullptr);
            STD_INSIST(splitUpdate != nullptr);
            STD_INSIST(wholeUpdate->cursor.posX == splitUpdate->cursor.posX);
            STD_INSIST(wholeUpdate->cursor.posY == splitUpdate->cursor.posY);
            STD_INSIST(wholeUpdate->rowCount == splitUpdate->rowCount);
            STD_INSIST(wholeUpdate->rowCount > 0);
            for (size_t index = 0; index < wholeUpdate->rowCount; ++index) {
                const TerminalRow& wholeRow = wholeUpdate->rows[index];
                const TerminalRow& splitRow = splitUpdate->rows[index];
                STD_INSIST(wholeRow.row == splitRow.row);
                STD_INSIST(wholeRow.lineAttribute == splitRow.lineAttribute);
                for (u16 cell = 0; cell < columns; ++cell) {
                    STD_INSIST(wholeRow.cells[cell].style == splitRow.cells[cell].style);
                    STD_INSIST(wholeRow.cells[cell].content == splitRow.cells[cell].content);
                }
            }
            whole.consume();
            split.consume();
        };

        const size_t chunkSizes[] = {1, 2, 3, 7};
        for (const size_t chunk : chunkSizes) {
            auto wholePool = ObjPool::fromMemory();
            auto splitPool = ObjPool::fromMemory();
            Composer& wholeComposer = *wholePool->make<Composer>(wholePool.mutPtr());
            Composer& splitComposer = *splitPool->make<Composer>(splitPool.mutPtr());
            Vterm& whole = *VtermHeadless::create(wholeComposer, nullptr)->terminal();
            Vterm& split = *VtermHeadless::create(splitComposer, nullptr)->terminal();
            discardOutput(whole);
            discardOutput(split);

            whole.feedPty(StringView(directed, sizeof(directed) - 1));
            for (size_t offset = 0; offset < sizeof(directed) - 1; offset += chunk) {
                const size_t length = sizeof(directed) - 1 - offset < chunk ? sizeof(directed) - 1 - offset : chunk;
                split.feedPty(StringView(directed + offset, length));
            }
            compareScreens(whole, split, wholeComposer.geometry.columns);

            whole.feedPty(StringView(garbage, sizeof(garbage)));
            for (size_t offset = 0; offset < sizeof(garbage); offset += chunk) {
                const size_t length = sizeof(garbage) - offset < chunk ? sizeof(garbage) - offset : chunk;
                split.feedPty(StringView(garbage + offset, length));
            }
            compareScreens(whole, split, wholeComposer.geometry.columns);
        }
    }
}

namespace {
    // A2/R7-2: a window with a split, where only one of its panes has
    // anything new. Both terminals are panes of their own - the harness
    // terminal is here for the platform and the window it builds, not
    // for the frame - and each paints its cell 0,0 a colour of its own,
    // so what a backend kept and what it redrew can be told apart in the
    // pixels.
    struct QuietPaneFixture {
        static constexpr u16 columns = 8;
        static constexpr u16 rows = 3;

        QuietPaneFixture() {
            composer = pool->make<Composer>(pool.mutPtr());
            VtermHeadless::create(*composer, nullptr);
            // The harness draws nothing and sizes its glyph 1x1; a
            // backend needs a real one. Embedded resolver only, so the
            // test does not depend on system fonts.
            while (!composer->fontResolvers.empty()) {
                composer->fontResolvers.popFront();
            }
            composer->fontResolvers.pushBack(createEmbeddedFontResolver(*composer));
            composer->fonts = Fontpack::create(*composer, *pool, nullptr, 0, nullptr, 0, 16);
            composer->geometry.setCellPixelSize(composer->fonts->getPx(), composer->fonts->getPy());
            const Insets insets = composer->contentInsets();
            composer->resize((u16)(gridPixelWidth(columns, insets, composer->geometry.cellPixelWidth)), (u16)(gridPixelHeight((u16)(2 * rows), insets, composer->geometry.cellPixelHeight)));
            // The render side of the grid moved out of Screen: without a
            // shaper the backends take every frame with no strips at all,
            // which is not what these two panes are here to measure.
            composer->shaper = SpanShaper::create(*composer, *pool);
            busy = Vterm::create(*composer->pool, composer->geometry, composer->vtConfig, composer->extras, *composer->smallObjects, *composer->scheduler, *composer->host, {.columns = columns, .rows = rows}, *composer->pool->make<SecondPtyStub>(*composer), nullptr);
            quiet = Vterm::create(*composer->pool, composer->geometry, composer->vtConfig, composer->extras, *composer->smallObjects, *composer->scheduler, *composer->host, {.columns = columns, .rows = rows}, *composer->pool->make<SecondPtyStub>(*composer), nullptr);
        }

        PixelRect topArea() const {
            return {0, 0, composer->geometry.pixelWidth, (u16)(composer->geometry.pixelHeight / 2)};
        }

        PixelRect bottomArea() const {
            const u16 half = (u16)(composer->geometry.pixelHeight / 2);
            return {0, half, composer->geometry.pixelWidth, (u16)(composer->geometry.pixelHeight - half)};
        }

        // The first frame is a reshape whatever it carries - the backend
        // retains nothing yet - so both panes damage themselves whole.
        void presentWholeFrame(Renderer& renderer) {
            busy->expose();
            quiet->expose();
            const TerminalUpdate* const busyUpdate = busy->output();
            const TerminalUpdate* const quietUpdate = quiet->output();
            STD_INSIST(busyUpdate != nullptr);
            STD_INSIST(quietUpdate != nullptr);
            STD_INSIST(busyUpdate->rowCount == rows);
            STD_INSIST(quietUpdate->rowCount == rows);
            const PaneUpdate panes[2] = {
                {topArea(), *busyUpdate},
                {bottomArea(), *quietUpdate},
            };
            STD_INSIST(renderer.update(panes, 2));
            busy->consume();
            quiet->consume();
        }

        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer* composer = nullptr;
        Vterm* busy = nullptr;
        Vterm* quiet = nullptr;
    };

    static const StringView paintRed(u8"\x1b[48;2;255;0;0m ");
    static const StringView paintGreen(u8"\x1b[48;2;0;255;0m ");
    // Row 2, column 1: one row of damage in a three-row grid, which is
    // what makes the frame below a partial one.
    static const StringView paintBlueOnSecondRow(u8"\x1b[2;1H\x1b[48;2;0;0;255m ");
}

// R7-2. The frame is a list of panes and every live pane owes it an
// entry; output() has none to give for a pane with nothing to say. Until
// retainedOutput() that pane simply dropped out of the list, and a frame
// with one pane where the last had two is a reshape: both backends then
// demand every row of every pane, do not get them, and refuse - which
// only asks for the same frame again. A window with a split and one idle
// shell stopped drawing.
STD_TEST_SUITE(QuietPaneFrame) {
    STD_TEST(TheReferenceBackendTakesAFrameWhoseQuietPaneDamagedNothing) {
        QuietPaneFixture fx;
        fx.busy->feedPty(paintRed);
        fx.quiet->feedPty(paintGreen);

        Vector<u8> pixels;
        pixels.zero((size_t)(fx.composer->geometry.pixelWidth) * fx.composer->geometry.pixelHeight * 3);
        plt::HeadlessRenderTarget target;
        target.pixels = pixels.mutData();
        target.length = pixels.length();
        target.width = fx.composer->geometry.pixelWidth;
        target.height = fx.composer->geometry.pixelHeight;
        target.stride = fx.composer->geometry.pixelWidth * 3;
        ObjPool::Ref rendererPool = ObjPool::fromMemory();
        ReferenceRenderer* const renderer = ReferenceRenderer::create(*fx.composer, *rendererPool, {plt::RenderBackend::Headless, nullptr, &target});
        STD_INSIST(renderer != nullptr);

        fx.presentWholeFrame(*renderer);

        // One row of one pane changes; the other pane has nothing at all
        // to say, which is exactly what output() cannot express.
        fx.busy->feedPty(paintBlueOnSecondRow);
        const TerminalUpdate* const busyUpdate = fx.busy->output();
        STD_INSIST(busyUpdate != nullptr);
        STD_INSIST(busyUpdate->rowCount == 1);
        STD_INSIST(fx.quiet->output() == nullptr);

        const TerminalUpdate& quietUpdate = fx.quiet->retainedOutput();
        // The acceptance criterion, in its two halves: the quiet pane
        // owes the frame no rows at all, and it still names the grid its
        // retained cells were built by (A9 - zero would be a refusal).
        STD_INSIST(quietUpdate.rowCount == 0);
        STD_INSIST(quietUpdate.gridColumns == QuietPaneFixture::columns);
        STD_INSIST(quietUpdate.gridRows == QuietPaneFixture::rows);

        const PaneUpdate panes[2] = {
            {fx.topArea(), *busyUpdate},
            {fx.bottomArea(), quietUpdate},
        };
        STD_INSIST(renderer->update(panes, 2));

        const ReferenceImage image = renderer->image();
        STD_INSIST(image.pixels != nullptr);
        const Insets insets = fx.composer->paneInsets();
        const auto pixelAt = [&image](u16 x, u16 y) {
            const size_t index = 3 * ((size_t)(y)*image.width + x);
            return Color{image.pixels[index], image.pixels[index + 1], image.pixels[index + 2]};
        };
        // Kept, not repainted: the quiet pane's cell 0,0 is the green it
        // was given a frame ago, and this frame carried no cell of it.
        const PixelRect bottom = fx.bottomArea();
        STD_INSIST((pixelAt((u16)(bottom.x + insets.left), (u16)(bottom.y + insets.top)) == Color{0, 255, 0}));
        // And the busy pane's one damaged row did land.
        const PixelRect top = fx.topArea();
        STD_INSIST((pixelAt((u16)(top.x + insets.left), (u16)(top.y + insets.top + fx.composer->geometry.cellPixelHeight)) == Color{0, 0, 255}));

        // The regression this exists for. Drop the quiet pane from the
        // frame - which is all the layout could do before this method -
        // and the frame is refused, because a pane count that changed is
        // a reshape and the busy pane damaged one row of three. The
        // refusal asks for the frame again, and the next one is the same
        // one: the window is stuck here.
        const PaneUpdate alone[1] = {{{0, 0, fx.composer->geometry.pixelWidth, fx.composer->geometry.pixelHeight}, *busyUpdate}};
        STD_INSIST(!renderer->update(alone, 1));
    }

    // Vterm's own half of the contract, without a backend in the way:
    // the retained form is the update output() would have given, minus
    // the damage - and it takes none of the damage with it, so the
    // output() that follows still reports it whole.
    STD_TEST(TheRetainedFormCarriesThePresentationAndLeavesTheDamageAlone) {
        QuietPaneFixture fx;
        fx.busy->feedPty(paintRed);
        discardOutput(*fx.busy);
        fx.busy->feedPty(paintBlueOnSecondRow);

        const TerminalUpdate& retained = fx.busy->retainedOutput();
        STD_INSIST(retained.rowCount == 0);
        STD_INSIST(retained.colors != nullptr);
        STD_INSIST(retained.shapes != nullptr);
        STD_INSIST(retained.cursor.posY == 1);

        // Asking for it neither consumed the pending row nor armed
        // consume(): the frame that follows is the one that was owed.
        const TerminalUpdate* const update = fx.busy->output();
        STD_INSIST(update != nullptr);
        STD_INSIST(update->rowCount == 1);
        STD_INSIST(update->rows[0].row == 1);
        STD_INSIST(update->shapes == retained.shapes);
        STD_INSIST(update->gridColumns == retained.gridColumns);
        STD_INSIST(update->gridRows == retained.gridRows);
        fx.busy->consume();

        // Consumed, so there is nothing left to say - and the retained
        // form is still there to say it.
        STD_INSIST(fx.busy->output() == nullptr);
        STD_INSIST(fx.busy->retainedOutput().rowCount == 0);
    }
}

#if defined(HAVE_METAL_RENDERER)

// The same frame at the other backend. Both of them retain cells across
// frames and both reshape on a changed pane count, so a quiet pane that
// only one of them accepted would be a pane that cannot be drawn on the
// hardware path.
STD_TEST_SUITE(QuietPaneFrameOnMetal) {
    STD_TEST(TheMetalBackendTakesAFrameWhoseQuietPaneDamagedNothing) {
        QuietPaneFixture fx;
        fx.busy->feedPty(paintRed);
        fx.quiet->feedPty(paintGreen);

        ObjPool::Ref rendererPool = ObjPool::fromMemory();
        Renderer* const renderer = createMetalRenderer(*fx.composer, *rendererPool, {plt::RenderBackend::Headless, nullptr, nullptr});
        STD_INSIST(renderer != nullptr);

        fx.presentWholeFrame(*renderer);

        fx.busy->feedPty(paintBlueOnSecondRow);
        const TerminalUpdate* const busyUpdate = fx.busy->output();
        STD_INSIST(busyUpdate != nullptr);
        STD_INSIST(busyUpdate->rowCount == 1);
        STD_INSIST(fx.quiet->output() == nullptr);

        const TerminalUpdate& quietUpdate = fx.quiet->retainedOutput();
        STD_INSIST(quietUpdate.rowCount == 0);
        STD_INSIST(quietUpdate.gridColumns == QuietPaneFixture::columns);
        STD_INSIST(quietUpdate.gridRows == QuietPaneFixture::rows);

        const PaneUpdate panes[2] = {
            {fx.topArea(), *busyUpdate},
            {fx.bottomArea(), quietUpdate},
        };
        STD_INSIST(renderer->update(panes, 2));

        Buffer rgb;
        u32 width = 0;
        u32 height = 0;
        STD_INSIST(renderer->captureOutput(rgb, width, height));
        STD_INSIST(width == fx.composer->geometry.pixelWidth);
        const Insets insets = fx.composer->paneInsets();
        const auto pixelAt = [&rgb, width](u16 x, u16 y) {
            const auto* const bytes = (const u8*)(rgb.data());
            const size_t index = 3 * ((size_t)(y)*width + x);
            return Color{bytes[index], bytes[index + 1], bytes[index + 2]};
        };
        const PixelRect bottom = fx.bottomArea();
        STD_INSIST((pixelAt((u16)(bottom.x + insets.left), (u16)(bottom.y + insets.top)) == Color{0, 255, 0}));
        const PixelRect top = fx.topArea();
        STD_INSIST((pixelAt((u16)(top.x + insets.left), (u16)(top.y + insets.top + fx.composer->geometry.cellPixelHeight)) == Color{0, 0, 255}));

        const PaneUpdate alone[1] = {{{0, 0, fx.composer->geometry.pixelWidth, fx.composer->geometry.pixelHeight}, *busyUpdate}};
        STD_INSIST(!renderer->update(alone, 1));
    }
}

#endif

#if defined(HAVE_METAL_RENDERER)

namespace {
    // The sequence a full-screen program opens with, and the one it
    // leaves by. What matters to a backend is neither: entering the
    // alternate screen swaps the pane's Screen (vterm.cpp), and the
    // Screen pointer is what update.shapes carries, so both backends read
    // the frame that follows as a reshaped one and demand every row of
    // every pane in it.
    static const StringView enterAlternate(u8"\x1b[?1049h\x1b[H\x1b[48;2;0;0;255m ");
    static const StringView leaveAlternate(u8"\x1b[?1049l");

    // Integer blending on the CPU against float blending on the GPU
    // differs by at most a rounding step per channel - the same number
    // tst/test_gpu_parity.py allows itself, for the same reason.
    constexpr u8 parityTolerance = 3;

    // The frame a window hands a backend when one pane changed screens
    // and the other has nothing to say: the one that speaks through
    // output(), the one that does not through its retained form. This is
    // the frame both backends refuse, and refusing it is correct - the
    // quiet pane's retained form carries no damage at all.
    struct ScreenChangeFrame {
        const TerminalUpdate* busy = nullptr;
        const TerminalUpdate* quiet = nullptr;
    };

    void compareBackendPixels(const ReferenceImage& reference, const Buffer& gpu, u32 gpuWidth, u32 gpuHeight) {
        STD_INSIST(reference.pixels != nullptr);
        STD_INSIST(gpuWidth == reference.width);
        STD_INSIST(gpuHeight == reference.height);
        STD_INSIST(gpu.used() == reference.length);
        const auto* const bytes = (const u8*)(gpu.data());
        size_t offenders = 0;
        u16 worst = 0;
        for (size_t index = 0; index < reference.length; ++index) {
            const u16 delta = (u16)(reference.pixels[index] > bytes[index] ? reference.pixels[index] - bytes[index] : bytes[index] - reference.pixels[index]);
            worst = delta > worst ? delta : worst;
            offenders += delta > parityTolerance ? 1 : 0;
        }
        if (offenders != 0) {
            sysE << StringView(u8"parity: ") << offenders << StringView(u8" channel(s) past the tolerance, worst ") << worst << endL;
        }
        STD_INSIST(offenders == 0);
    }

    // The first frame, given to both backends out of one collection of
    // the panes. QuietPaneFixture::presentWholeFrame() cannot serve two
    // renderers: it consumes what it presented, and the expose() it
    // starts from only marks the output pending - so the second backend
    // would be handed a frame of no rows at all. Both are primed from
    // one pair of updates here instead, and the consume() comes after
    // the second one has drawn.
    void primeBothBackends(QuietPaneFixture& fx, Renderer& reference, Renderer& metal) {
        fx.busy->exposeAll();
        fx.quiet->exposeAll();
        const TerminalUpdate* const busyUpdate = fx.busy->output();
        const TerminalUpdate* const quietUpdate = fx.quiet->output();
        STD_INSIST(busyUpdate != nullptr);
        STD_INSIST(quietUpdate != nullptr);
        STD_INSIST(busyUpdate->rowCount == QuietPaneFixture::rows);
        STD_INSIST(quietUpdate->rowCount == QuietPaneFixture::rows);
        const PaneUpdate panes[2] = {
            {fx.topArea(), *busyUpdate},
            {fx.bottomArea(), *quietUpdate},
        };
        STD_INSIST(reference.update(panes, 2));
        STD_INSIST(metal.update(panes, 2));
        fx.busy->consume();
        fx.quiet->consume();
    }
}

// R3-test, the plan's fifth check: "parity Metal <-> reference on a
// multi-pane frame with a screen change". tst/test_gpu_parity.py cannot
// carry it and could not be extended to - MirrorRenderer (test_mode.cpp)
// implements only the pane-less update(), so Renderer's default takes
// every frame of more than one pane and refuses it before either backend
// sees it (render.h). Under SHITTY_TEST_VULKAN a split window presents
// nothing at all; the whole of the harness's parity apparatus is
// single-pane by construction.
//
// So the two backends are driven side by side here instead, over the
// frames T3 is about: the one they both refuse, and the one exposeAll()
// makes of it. Both halves matter. Agreeing on the refusal is what makes
// the answer to it a window's business rather than one backend's, and
// agreeing on the pixels afterwards is what says the answer left the
// same picture on both.
STD_TEST_SUITE(MultiPaneScreenChangeParity) {
    STD_TEST(BothBackendsRefuseTheSameFrameAndTakeTheSameOneAfterExposeAll) {
        QuietPaneFixture fx;
        fx.busy->feedPty(paintRed);
        fx.quiet->feedPty(paintGreen);

        Vector<u8> pixels;
        pixels.zero((size_t)(fx.composer->geometry.pixelWidth) * fx.composer->geometry.pixelHeight * 3);
        plt::HeadlessRenderTarget target;
        target.pixels = pixels.mutData();
        target.length = pixels.length();
        target.width = fx.composer->geometry.pixelWidth;
        target.height = fx.composer->geometry.pixelHeight;
        target.stride = fx.composer->geometry.pixelWidth * 3;
        ObjPool::Ref referencePool = ObjPool::fromMemory();
        ReferenceRenderer* const reference = ReferenceRenderer::create(*fx.composer, *referencePool, {plt::RenderBackend::Headless, nullptr, &target});
        STD_INSIST(reference != nullptr);
        ObjPool::Ref metalPool = ObjPool::fromMemory();
        Renderer* const metal = createMetalRenderer(*fx.composer, *metalPool, {plt::RenderBackend::Headless, nullptr, nullptr});
        STD_INSIST(metal != nullptr);

        // Both start from the same frame, so both retain the same cells
        // and the reshape below is the same reshape for each of them.
        primeBothBackends(fx, *reference, *metal);

        const auto compareNow = [&]() {
            Buffer rgb;
            u32 width = 0;
            u32 height = 0;
            STD_INSIST(metal->captureOutput(rgb, width, height));
            compareBackendPixels(reference->image(), rgb, width, height);
        };

        compareNow();

        // The defect's own frame. One pane changes the identity of its
        // Screen; the other was not written to, so it has no output() at
        // all and hands over the retained form that owes rows it cannot
        // pay.
        fx.busy->feedPty(enterAlternate);
        const TerminalUpdate* const changed = fx.busy->output();
        STD_INSIST(changed != nullptr);
        STD_INSIST(fx.quiet->output() == nullptr);
        {
            const TerminalUpdate& retained = fx.quiet->retainedOutput();
            STD_INSIST(retained.rowCount == 0);
            const PaneUpdate refused[2] = {
                {fx.topArea(), *changed},
                {fx.bottomArea(), retained},
            };
            // Both, and for the same reason. A backend that took this
            // frame would be reading the quiet pane's cells out of a
            // store that has just been resized under it.
            STD_INSIST(!reference->update(refused, 2));
            STD_INSIST(!metal->update(refused, 2));
        }

        // T3's answer, in the shape ApplicationImpl gives it: every
        // visible pane exposed whole, the frame collected again, and the
        // same two backends asked again.
        fx.busy->exposeAll();
        fx.quiet->exposeAll();
        const TerminalUpdate* const busyAgain = fx.busy->output();
        const TerminalUpdate* const quietAgain = fx.quiet->output();
        STD_INSIST(busyAgain != nullptr);
        STD_INSIST(quietAgain != nullptr);
        // The whole point of exposeAll() over expose(): rows, not just a
        // pending flag. Without the damage these two counts are zero and
        // the frame below is refused exactly as the one above was.
        STD_INSIST(busyAgain->rowCount == QuietPaneFixture::rows);
        STD_INSIST(quietAgain->rowCount == QuietPaneFixture::rows);
        const PaneUpdate whole[2] = {
            {fx.topArea(), *busyAgain},
            {fx.bottomArea(), *quietAgain},
        };
        STD_INSIST(reference->update(whole, 2));
        STD_INSIST(metal->update(whole, 2));
        fx.busy->consume();
        fx.quiet->consume();

        compareNow();

        // The pane that changed screens is on its alternate screen, and
        // the one that did not is where it was - blue over green, and
        // both backends agree on which pixel is which.
        const ReferenceImage image = reference->image();
        const Insets insets = fx.composer->paneInsets();
        const auto pixelAt = [&image](u16 x, u16 y) {
            const size_t index = 3 * ((size_t)(y)*image.width + x);
            return Color{image.pixels[index], image.pixels[index + 1], image.pixels[index + 2]};
        };
        const PixelRect top = fx.topArea();
        const PixelRect bottom = fx.bottomArea();
        STD_INSIST((pixelAt((u16)(top.x + insets.left), (u16)(top.y + insets.top)) == Color{0, 0, 255}));
        STD_INSIST((pixelAt((u16)(bottom.x + insets.left), (u16)(bottom.y + insets.top)) == Color{0, 255, 0}));

        // And the way out, which is the same swap in the other
        // direction and the same refusal with it.
        fx.busy->feedPty(leaveAlternate);
        const TerminalUpdate* const back = fx.busy->output();
        STD_INSIST(back != nullptr);
        STD_INSIST(fx.quiet->output() == nullptr);
        {
            const PaneUpdate refused[2] = {
                {fx.topArea(), *back},
                {fx.bottomArea(), fx.quiet->retainedOutput()},
            };
            STD_INSIST(!reference->update(refused, 2));
            STD_INSIST(!metal->update(refused, 2));
        }
        fx.busy->exposeAll();
        fx.quiet->exposeAll();
        const TerminalUpdate* const busyBack = fx.busy->output();
        const TerminalUpdate* const quietBack = fx.quiet->output();
        STD_INSIST(busyBack != nullptr);
        STD_INSIST(quietBack != nullptr);
        const PaneUpdate wholeBack[2] = {
            {fx.topArea(), *busyBack},
            {fx.bottomArea(), *quietBack},
        };
        STD_INSIST(reference->update(wholeBack, 2));
        STD_INSIST(metal->update(wholeBack, 2));
        fx.busy->consume();
        fx.quiet->consume();

        compareNow();

        // Back on the primary screen: the red it was painted before the
        // program took over, and the neighbour still green.
        const ReferenceImage after = reference->image();
        const auto pixelAfter = [&after](u16 x, u16 y) {
            const size_t index = 3 * ((size_t)(y)*after.width + x);
            return Color{after.pixels[index], after.pixels[index + 1], after.pixels[index + 2]};
        };
        STD_INSIST((pixelAfter((u16)(top.x + insets.left), (u16)(top.y + insets.top)) == Color{255, 0, 0}));
        STD_INSIST((pixelAfter((u16)(bottom.x + insets.left), (u16)(bottom.y + insets.top)) == Color{0, 255, 0}));
    }

    // The mutation this suite exists to kill, stated as a test of its
    // own: expose() marks the output pending and damages nothing
    // (vterm.cpp), so a window that answered the refusal with it hands
    // the backend the same zero rows it just refused. Both backends
    // refuse again, and the window is where it was.
    STD_TEST(ExposeAloneLeavesTheFrameRefusedByBothBackends) {
        QuietPaneFixture fx;
        fx.busy->feedPty(paintRed);
        fx.quiet->feedPty(paintGreen);

        Vector<u8> pixels;
        pixels.zero((size_t)(fx.composer->geometry.pixelWidth) * fx.composer->geometry.pixelHeight * 3);
        plt::HeadlessRenderTarget target;
        target.pixels = pixels.mutData();
        target.length = pixels.length();
        target.width = fx.composer->geometry.pixelWidth;
        target.height = fx.composer->geometry.pixelHeight;
        target.stride = fx.composer->geometry.pixelWidth * 3;
        ObjPool::Ref referencePool = ObjPool::fromMemory();
        ReferenceRenderer* const reference = ReferenceRenderer::create(*fx.composer, *referencePool, {plt::RenderBackend::Headless, nullptr, &target});
        ObjPool::Ref metalPool = ObjPool::fromMemory();
        Renderer* const metal = createMetalRenderer(*fx.composer, *metalPool, {plt::RenderBackend::Headless, nullptr, nullptr});
        STD_INSIST(reference != nullptr && metal != nullptr);
        primeBothBackends(fx, *reference, *metal);

        // The frame the window was refused: one pane changed screens and
        // has its damage to show for it, the other was not written to.
        fx.busy->feedPty(enterAlternate);
        STD_INSIST(fx.quiet->output() == nullptr);

        // ...answered with expose() instead of exposeAll(). The pending
        // flag arrives at the quiet pane and the rows do not, which is
        // the whole of the difference between the two methods.
        fx.busy->expose();
        fx.quiet->expose();
        const TerminalUpdate* const busyUpdate = fx.busy->output();
        const TerminalUpdate* const quietUpdate = fx.quiet->output();
        STD_INSIST(busyUpdate != nullptr);
        STD_INSIST(quietUpdate != nullptr);
        STD_INSIST(quietUpdate->rowCount == 0);

        const PaneUpdate stillRefused[2] = {
            {fx.topArea(), *busyUpdate},
            {fx.bottomArea(), *quietUpdate},
        };
        STD_INSIST(!reference->update(stillRefused, 2));
        STD_INSIST(!metal->update(stillRefused, 2));
    }
}

#endif
