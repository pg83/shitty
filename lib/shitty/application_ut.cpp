/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render.h"
#include "options.h"
#include "session.h"
#include "composer.h"
#include "application.h"
#include "pane_layout.h"
#include "grid_geometry.h"
#include "ui_quick_hotkey.h"
#include "quick_frame_store.h"

#include <lib/vterm/listener.h>

#include <std/tst/ut.h>
#include <std/str/view.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <plt/input.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/poller_loop.h>
#include <plt/platform_headless.h>

using namespace stl;

namespace {
    // Mirrors quick_frame_store_ut.cpp's makeTempDir(): a mkdtemp()
    // directory this process actually owns, so defaultQuickFramePath()
    // has a real, writable directory to place the state file next to.
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/application_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }
}

namespace {
    // R8-test. A forked /bin/sh inherits MallocScribble from this process,
    // and libmalloc then prints its "enabling scribbling to detect mods to
    // free blocks" banner to the child's stderr - which is the slave side
    // of this pane's pty. The line arrives as CR/LF terminated text, lands
    // in the pane's grid, and moves the cursor off the home cell; the
    // anchor assertions below then read a row that the shell never wrote.
    //
    // Measured with a probe that fed one banner-shaped line to the focused
    // pane before the first frame: count and x hold, y comes back 56 where
    // 18 is expected - two whole glyph heights - which is exactly the
    // shape of the flake seen once in a full run under MallocScribble
    // (drive.anchor.y, application_ut.cpp:332 at e74011b8) and could not
    // be reproduced by running the test on its own.
    //
    // Unset rather than worked around: libmalloc reads the variable when
    // this process starts, so clearing it now leaves this process's own
    // scribbling on and only stops children inheriting it - and no child
    // of these tests is under test. Only sound while tests run serially,
    // which is what build.py asks for (--threads=1).
    void keepMallocDebugOutOfTheChildren() {
        unsetenv("MallocScribble");
        unsetenv("MallocPreScribble");
        unsetenv("MallocGuardEdges");
    }

    struct SavedSignals {
        SavedSignals() {
            STD_INSIST(sigaction(SIGCHLD, nullptr, &child) == 0);
            STD_INSIST(sigaction(SIGINT, nullptr, &interrupt) == 0);
            STD_INSIST(sigaction(SIGQUIT, nullptr, &quit) == 0);
        }

        ~SavedSignals() noexcept {
            sigaction(SIGCHLD, &child, nullptr);
            sigaction(SIGINT, &interrupt, nullptr);
            sigaction(SIGQUIT, &quit, nullptr);
        }

        struct sigaction child{};
        struct sigaction interrupt{};
        struct sigaction quit{};
    };

    // R3-test. Counts what ApplicationImpl asks a backend for inside one
    // frame callback, and can refuse on demand. Both numbers are things
    // no assertion about pixels can reach: "the frame settles no later
    // than the second present" (plan, T3) is a statement about calls,
    // and so is "exposeAll() is not on the successful path" - a window
    // that exposed every pane on every frame would draw the same
    // pixels and only cost more.
    //
    // Installed over the renderer ApplicationImpl built, not instead of
    // it: what is under test is the retry, and a retry answered by a
    // stub that takes anything proves nothing.
    struct CountingRenderer final: Renderer {
        explicit CountingRenderer(Renderer* inner_)
            : inner(inner_)
        {
        }

        bool update(const PaneUpdate* panes, size_t count) override {
            ++paneUpdates;
            // F6: where each pane was placed on the surface. The only
            // reader of buildFrameUpdates()' arithmetic outside
            // ApplicationImpl - a backend gets these rectangles and
            // nothing else says where a pane went.
            lastAreas.clear();
            for (size_t at = 0; at < count; ++at) {
                lastAreas.pushBack(panes[at].area);
            }
            if (refuseEverything) {
                ++refusals;
                return false;
            }
            const bool presented = inner->update(panes, count);
            refusals += presented ? 0 : 1;
            return presented;
        }

        bool update(const TerminalUpdate& update) override {
            return inner->update(update);
        }

        void setSeams(const PixelRect* seams, size_t count, Color ink) override {
            inner->setSeams(seams, count, ink);
        }

        bool repaint() override {
            ++repaints;
            return inner->repaint();
        }

        void reset() {
            paneUpdates = 0;
            refusals = 0;
            repaints = 0;
        }

        Renderer* inner;
        stl::Vector<PixelRect> lastAreas;
        u32 paneUpdates = 0;
        u32 refusals = 0;
        u32 repaints = 0;
        bool refuseEverything = false;
    };

    // stderr into a file for as long as this lives. The diagnostic R3
    // asks for is a line on a stream and not a return value - that is
    // the whole point of it, a stall that used to announce itself with
    // nothing but a percent of a CPU - so reading it back is the only
    // way to assert it was printed.
    struct CapturedStderr {
        explicit CapturedStderr(const char* path) {
            file = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
            STD_INSIST(file >= 0);
            saved = dup(STDERR_FILENO);
            STD_INSIST(saved >= 0);
            fflush(stderr);
            STD_INSIST(dup2(file, STDERR_FILENO) >= 0);
        }

        // The bytes written since the capture began, NUL terminated so
        // strstr() can be asked about them.
        void collect(Buffer& out) {
            fflush(stderr);
            const off_t length = lseek(file, 0, SEEK_END);
            STD_INSIST(length >= 0);
            out.reset();
            out.grow((size_t)(length) + 1);
            STD_INSIST(lseek(file, 0, SEEK_SET) == 0);
            size_t taken = 0;
            while (taken < (size_t)(length)) {
                const ssize_t got = read(file, (u8*)(out.mutData()) + taken, (size_t)(length) - taken);
                if (got <= 0) {
                    break;
                }
                taken += (size_t)(got);
            }
            const u8 terminator = 0;
            out.seekAbsolute(taken);
            out.append(&terminator, 1);
        }

        ~CapturedStderr() noexcept {
            if (saved >= 0) {
                fflush(stderr);
                dup2(saved, STDERR_FILENO);
                close(saved);
            }
            if (file >= 0) {
                close(file);
            }
        }

        int saved = -1;
        int file = -1;
    };

    bool mentions(const Buffer& text, const char* needle) {
        return strstr((const char*)(text.data()), needle) != nullptr;
    }

    size_t mentionCount(const Buffer& text, const char* needle) {
        size_t found = 0;
        const char* at = (const char*)(text.data());
        for (;;) {
            at = strstr(at, needle);
            if (at == nullptr) {
                return found;
            }
            ++found;
            at += strlen(needle);
        }
    }

    struct DriveApplication final: plt::TimerCallback {
        explicit DriveApplication(Composer& composer_)
            : composer(composer_)
        {
        }

        void ready() override {
            fired = true;
            auto& window = static_cast<plt::WindowHeadless&>(*composer.window);
            // T5.2: the grid ApplicationImpl::showWindow() asked the
            // window for, read before anything here moves it. Nothing
            // else ever sees that number: showWindow() runs once, from
            // run(), between the last font change and the event loop,
            // and the first frame's updateWindowInfo() re-counts the
            // grid over whatever the platform then reports. The reserves
            // below are set after this line on purpose - they change the
            // count, and what is under test is the count showWindow()
            // made.
            gridColumnsAtShow = composer.geometry.columns;
            gridRowsAtShow = composer.geometry.rows;
            // F4, I3: two reserves that are not each other, set before
            // the frame that anchors the input method. Without them the
            // horizontal inset and the vertical one are both the border
            // and the anchor can take either for either - which is how
            // R3-test's R16 (the IME rect reading its x out of
            // insets.top) stayed alive through two waves. The sides are
            // the ones real chrome uses, and the numbers are small
            // enough to leave the 20x4 grid a grid.
            composer.setChromeReserve(ChromeSide::Left, 3);
            composer.setChromeReserve(ChromeSide::Top, 16);
            if (splitPanes) {
                // T10/T13: two live panes before the first frame, so what
                // is dispatched below is the multi-pane frame. The focus
                // then goes back to the *first* pane, which is what lets
                // the anchor tell "the focused pane" from "the last pane
                // in the list" - left where the split put it, the two are
                // one pane and the assertion asserts nothing.
                split = composer.sessions->splitFocused(SplitDirection::Vertical);
                Vector<SessionPane> panes;
                composer.sessions->visiblePanes(panes);
                if (panes.length() == 2) {
                    composer.sessions->focusPane(panes[0].id);
                    secondPane = panes[1].id;
                    firstTerminal = panes[0].terminal;
                }
            }
            framePresented = window.dispatchFrame();
            // One present for the whole tab, not one per pane.
            generationAfterFirstFrame = window.presentedFrame().generation;
            // Read here and not after run(): later frames anchor on
            // whichever pane has the focus then, and what is under test is
            // the frame just dispatched.
            anchor = window.requestedTextInputRect();

            // T5.2: the same anchor, off the home cell. Before the line
            // is entered below, so no shell output can be in flight and
            // the cursor is where this probe put it and nowhere else.
            if (cursorAnchorProbe) {
                driveCursorAnchorFrame(window);
            }

            // F6: the same frame read as rectangles rather than as an
            // anchor. Needs the split above, so it runs after it and
            // before the line entered below moves anything.
            if (frameBridgeProbe) {
                driveFrameBridgeFrame(window);
            }

            // Enter one line through the same platform-facing sink used by
            // Wayland/Cocoa. This crosses FiberInputSink, InputRouter,
            // SessionSet and Vterm before reaching the real PTY handle.
            composer.input->text({.codepoint = 'g'});
            composer.input->text({.codepoint = 'o'});
            composer.input->key({
                .key = plt::InputKey::Enter,
                .action = plt::InputAction::Press,
            });
            composer.input->flush();
            if (secondPane != 0) {
                // A second multi-pane frame, so the pane walk is not only
                // ever seen on the first one. The frame has to be
                // *requested* or dispatchFrame() has nothing to dispatch -
                // it was not, at first, and this whole half of the stand
                // ran as a no-op while looking like a test.
                //
                // What it does NOT reach is the retained-output branch:
                // both panes arrive at the frame with output pending every
                // time, and draining the second one by hand immediately
                // before the request does not change that - it is silent
                // when drained and speaking again inside the frame, for a
                // reason left unfound. So the two mutations that would
                // prove that branch - consume() on every pane, and
                // dropping the retained pane - pass in silence, and no
                // coverage of it should be read into this test.
                firstTerminal->feedPty(StringView(u8"x"));
                composer.window->requestFrame();
                secondFramePresented = window.dispatchFrame();
                // And the other shell gets its line too, or run() never
                // returns: the loop ends when the last session does.
                composer.sessions->focusPane(secondPane);
                if (quietPaneProbe) {
                    driveQuietPaneFrame(window);
                }
                if (screenChangeProbe) {
                    driveScreenChangeFrames(window);
                }
                if (synchronizedNeighbourProbe) {
                    driveSynchronizedNeighbourFrame(window);
                }
                if (rendererRebuildProbe) {
                    driveRebuiltRendererFrame(window);
                }
                if (refusalDiagnosticsProbe) {
                    driveRefusalDiagnostics(window);
                }
                composer.input->text({.codepoint = 'g'});
                composer.input->text({.codepoint = 'o'});
                composer.input->key({
                    .key = plt::InputKey::Enter,
                    .action = plt::InputAction::Press,
                });
                composer.input->flush();
            }
        }

        // R8-test, T10's open hole: a frame in which one pane speaks and
        // the other hands over its retained form.
        //
        // Why this cannot be done on the frames above: while the window's
        // geometry is still settling, ApplicationImpl::frame() itself arms
        // every pane before presentTerminal() gets to ask any of them -
        // updateWindowInfo() -> Composer::resize() -> CallSessionsResize
        // -> SessionSet::applyLayout() -> Vterm::paneResized(), whose
        // cf->expose() plus redraw() reaches every pane of the tab and not
        // only the ones whose grid moved (98a08f42, and vterm.cpp's
        // paneResized). The first frame runs fontChanged() for the initial
        // geometry, so the second frame arrives with genuinely new numbers
        // and resize() fans out again. From the third frame on the numbers
        // repeat, resize() returns early, and a pane that was not written
        // to stays quiet.
        void driveQuietPaneFrame(plt::WindowHeadless& window) {
            // Two frames to let the geometry stop moving. The drain below
            // is what makes the result not depend on this count being
            // exactly right - but the count has to be enough for resize()
            // to have gone quiet, or the drain is undone inside the frame.
            composer.window->requestFrame();
            window.dispatchFrame();
            composer.window->requestFrame();
            window.dispatchFrame();

            Vector<SessionPane> panes;
            composer.sessions->visiblePanes(panes);
            if (panes.length() != 2) {
                return;
            }
            // The focused pane is the one that will say nothing: whatever
            // it still held is taken here, so it reaches presentTerminal()
            // with no output() at all.
            quietPaneOriginX = panes[1].area.x;
            quietPaneOriginY = panes[1].area.y;
            Vterm* const quiet = panes[1].terminal;
            if (quiet->output() != nullptr) {
                quiet->consume();
            }
            quietPaneDrained = quiet->output() == nullptr;

            // ...and the other one does speak, so what follows is a frame
            // rather than the repaint a window where nobody spoke gets.
            anchorsBeforeQuietFrame = window.requestedTextInputRect().count;
            firstTerminal->feedPty(StringView(u8"y"));
            composer.window->requestFrame();
            quietFramePresented = window.dispatchFrame();
            quietAnchor = window.requestedTextInputRect();
        }

        // R3-test. Every requested frame the window still has, up to a
        // bound. Unbounded is not an option: a window whose backend
        // refuses asks for another frame each time it is refused, so a
        // drain that waited for framePending() to clear would be the
        // stall itself rather than a test of it.
        void drainFrames(plt::WindowHeadless& window) {
            for (int at = 0; at < 8 && window.framePending(); ++at) {
                window.dispatchFrame();
            }
        }

        // R3-test. Frames until the window's geometry has stopped
        // moving, for the same reason driveQuietPaneFrame() needs them:
        // while it moves, Composer::resize() fans out to every pane and
        // a pane drained by hand speaks again inside the next frame.
        void settleGeometry(plt::WindowHeadless& window) {
            for (int at = 0; at < 3; ++at) {
                composer.window->requestFrame();
                window.dispatchFrame();
            }
        }

        // T5.2: a frame whose cursor is not on the home cell.
        //
        // Every anchor above is read at (0, 0), and there both products
        // of cellOrigin() are zero: `column * glyphWidth` and
        // `row * glyphHeight` vanish, the corner is the content box's
        // own corner, and the call site can hand the row over as the
        // column and the cell height as the cell width without moving
        // the anchor by a pixel. The reserves make the two *insets*
        // different numbers, which is why a transposed inset is caught -
        // but the insets are all that survives at the home cell, so
        // nothing else at that call site is.
        //
        // The cursor goes to column 3 of row 1, which is a cell whose
        // column and row differ, on a cell box whose width and height
        // differ. Those two facts are what make the column/row hand-off
        // and the width/height hand-off two separate observable things:
        // swapping either one alone moves the anchor, and each moves it
        // to a different wrong place.
        void driveCursorAnchorFrame(plt::WindowHeadless& window) {
            // The grid has to have stopped moving before the cursor is
            // placed: while it moves, Composer::resize() re-counts it and
            // a grid that shrinks past the cursor takes the cursor with
            // it.
            settleGeometry(window);
            Vector<SessionPane> panes;
            composer.sessions->visiblePanes(panes);
            if (panes.length() != 1) {
                return;
            }
            // One pane, so the pane origin buildFrameUpdates() adds to
            // the cell origin is (0, 0) and what is left in the anchor is
            // cellOrigin() alone. Recorded rather than assumed - a
            // non-zero origin here would hide a transposition of exactly
            // its own size.
            cursorPaneOriginX = panes[0].area.x;
            cursorPaneOriginY = panes[0].area.y;
            cursorGridColumns = composer.geometry.columns;
            cursorGridRows = composer.geometry.rows;
            // CUP, one-based: row 2, column 4 - the cursor's own (3, 1).
            // Fed as terminal output rather than typed, so the position
            // does not depend on a shell, a prompt, or an echo.
            panes[0].terminal->feedPty(StringView(u8"\x1b[2;4H"));
            anchorsBeforeCursorFrame = window.requestedTextInputRect().count;
            composer.window->requestFrame();
            cursorFramePresented = window.dispatchFrame();
            cursorAnchor = window.requestedTextInputRect();
        }

        // F6, R6-arch finding 2. The A10 bridge for a frame:
        // SessionSet answers in content-box coordinates and a backend is
        // owed coordinates on the surface, and buildFrameUpdates() is
        // the one place the two are joined (application.cpp).
        //
        // Nothing read those rectangles back. The anchor assertions
        // above go through the *other* branch of the same function - the
        // focused pane's cell origin, which is composed against
        // contentInsets() further down presentTerminal() and never
        // against the chrome this bridge adds - so both a substitution
        // and the outright deletion of the addition survived every test
        // in the tree.
        //
        // Recorded through the renderer because that is the only reader
        // there is: the headless window keeps pixels and a generation,
        // not a pane list.
        void driveFrameBridgeFrame(plt::WindowHeadless& window) {
            settleGeometry(window);
            Vector<SessionPane> panes;
            bridgePanesDrained = drainPanes(panes) && panes.length() == 2;
            if (!bridgePanesDrained) {
                return;
            }
            // What SessionSet says, before the bridge: content-box
            // coordinates, one rectangle per visible pane.
            bridgeBoxAreas.clear();
            for (const SessionPane& pane : panes) {
                bridgeBoxAreas.pushBack(pane.area);
            }
            bridgeChrome = composer.chromeInsets();
            bridgeContent = composer.contentInsets();

            CountingRenderer* const spy = composer.pool->make<CountingRenderer>(composer.renderer);
            composer.renderer = spy;
            // A pane has to speak or presentTerminal() answers with a
            // repaint, which builds no frame at all and would leave the
            // spy holding the empty list it started with.
            panes[0].terminal->feedPty(StringView(u8"z"));
            composer.window->requestFrame();
            bridgeFramePresented = window.dispatchFrame();
            bridgeSurfaceAreas.clear();
            for (size_t at = 0; at < spy->lastAreas.length(); ++at) {
                bridgeSurfaceAreas.pushBack(spy->lastAreas[at]);
            }

            composer.renderer = spy->inner;
            drainFrames(window);
        }

        // Every visible pane with nothing left to say. Answers whether
        // they all fell silent, which is the state the frames below are
        // about: a pane still holding output would damage rows of its
        // own and hide whichever exposure is under test.
        bool drainPanes(Vector<SessionPane>& panes) {
            panes.clear();
            composer.sessions->visiblePanes(panes);
            bool drained = !panes.empty();
            for (const SessionPane& pane : panes) {
                if (pane.terminal->output() != nullptr) {
                    pane.terminal->consume();
                }
                drained = drained && pane.terminal->output() == nullptr;
            }
            return drained;
        }

        // R3-test: how many times one frame callback asks the backend.
        // Once for an ordinary frame; twice for the frame a pane changed
        // screens in, the second time with every pane handed over whole.
        // That second number is T3's third acceptance criterion read
        // literally - "no later than the second presentTerminal()" - and
        // the first is its fourth, which no pixel can answer.
        void driveScreenChangeFrames(plt::WindowHeadless& window) {
            settleGeometry(window);
            Vector<SessionPane> panes;
            probePanesDrained = drainPanes(panes) && panes.length() == 2;
            if (!probePanesDrained) {
                return;
            }
            CountingRenderer* const spy = composer.pool->make<CountingRenderer>(composer.renderer);
            composer.renderer = spy;

            // An ordinary frame: one pane prints a character, the other
            // says nothing and hands over its retained form.
            panes[0].terminal->feedPty(StringView(u8"z"));
            composer.window->requestFrame();
            ordinaryFramePresented = window.dispatchFrame();
            ordinaryPaneUpdates = spy->paneUpdates;
            ordinaryRefusals = spy->refusals;

            // And the frame the defect lives in. The pane that changes
            // screens is the one that speaks; its neighbour is silent
            // and owes the reshaped frame every row it has not got.
            spy->reset();
            drainPanes(panes);
            panes[0].terminal->feedPty(StringView(u8"\x1b[?1049h\x1b[H\x1b[2JALTERNATE"));
            composer.window->requestFrame();
            // The plan's bound, dispatched rather than assumed: "the
            // frame settles no later than the second presentTerminal()".
            // T3 answers the refusal inside the callback it happened in,
            // so one dispatch is enough for it - but a window that
            // exposed the panes and asked for another frame would be
            // just as correct by that criterion, and counting the
            // dispatches leaves room for it. Four is a cap on a loop
            // that would otherwise be the stall.
            for (int at = 0; at < 4 && !screenChangeFramePresented && window.framePending(); ++at) {
                screenChangeFramePresented = window.dispatchFrame();
                ++screenChangeDispatches;
            }
            screenChangePaneUpdates = spy->paneUpdates;
            screenChangeRefusals = spy->refusals;

            composer.renderer = spy->inner;
            drainFrames(window);
        }

        // R3a-1 (F3). The same defect entered from the other side: the
        // pane that owes the reshaped frame every row is also the one
        // inside a synchronized update (\e[?2026h), and inside one
        // redraw() is a no-op. exposeAll() would then damage the rows
        // and leave the pending flag down, the pane would hand over its
        // retained form again - which carries no damage at all - and the
        // reshaped frame would be refused again, for as long as the mode
        // held.
        //
        // In the product that is bounded by the synchronized watchdog,
        // 150ms of a window that has stopped. Here it is not bounded by
        // anything: the test build starts no timers at all (vterm.cpp,
        // startTimers() under SHITTY_FOR_TESTS), so the stall is forever
        // and the loop below is a cap on it rather than a wait for it.
        void driveSynchronizedNeighbourFrame(plt::WindowHeadless& window) {
            settleGeometry(window);
            Vector<SessionPane> panes;
            probePanesDrained = drainPanes(panes) && panes.length() == 2;
            if (!probePanesDrained) {
                return;
            }
            CountingRenderer* const spy = composer.pool->make<CountingRenderer>(composer.renderer);
            composer.renderer = spy;

            // The neighbour opens a synchronized update and then has
            // nothing more to say - which is the ordinary shape of a TUI
            // between two redraws, not a contrivance.
            panes[1].terminal->feedPty(StringView(u8"\x1b[?2026h"));
            synchronizedNeighbourArmed = panes[1].terminal->state().synchronizedOutput;
            synchronizedNeighbourSilent = panes[1].terminal->output() == nullptr;

            // And the other pane changes screens, which is what makes
            // the frame a reshaped one and the neighbour's silence a
            // refusal.
            panes[0].terminal->feedPty(StringView(u8"\x1b[?1049h\x1b[H\x1b[2JALTERNATE"));
            composer.window->requestFrame();
            for (int at = 0; at < 4 && !synchronizedFramePresented && window.framePending(); ++at) {
                synchronizedFramePresented = window.dispatchFrame();
                ++synchronizedDispatches;
            }
            synchronizedPaneUpdates = spy->paneUpdates;
            synchronizedNeighbourStillArmed = panes[1].terminal->state().synchronizedOutput;

            composer.renderer = spy->inner;
            drainFrames(window);
        }

        // R3-test, the plan's second path: the renderer died with its
        // surface (render_vk.cpp does exactly this from inside update())
        // and frame() builds a fresh one. A fresh renderer retains no
        // panes, so its first frame is a reshaped one and every pane of
        // the window owes it every row - not only the active terminal,
        // which is all the code exposed before T3.
        void driveRebuiltRendererFrame(plt::WindowHeadless& window) {
            settleGeometry(window);
            Vector<SessionPane> panes;
            probePanesDrained = drainPanes(panes) && panes.length() == 2;
            if (!probePanesDrained) {
                return;
            }
            composer.renderer = nullptr;
            composer.rendererPool = ObjPool::fromMemory();
            generationBeforeRebuild = window.presentedFrame().generation;
            composer.window->requestFrame();
            rebuiltFramePresented = window.dispatchFrame();
            generationAfterRebuild = window.presentedFrame().generation;
            rebuiltRendererExists = composer.renderer != nullptr;

            // And the frames after it are ordinary again: the rebuild is
            // answered once, not turned into a mode.
            CountingRenderer* const spy = composer.pool->make<CountingRenderer>(composer.renderer);
            composer.renderer = spy;
            drainPanes(panes);
            panes[0].terminal->feedPty(StringView(u8"z"));
            composer.window->requestFrame();
            frameAfterRebuildPresented = window.dispatchFrame();
            paneUpdatesAfterRebuild = spy->paneUpdates;
            composer.renderer = spy->inner;
            drainFrames(window);
        }

        // R3-test: a refusal a full expose cannot cure. R3 asks for it to
        // reach stderr instead of only the CPU, and for it to say what
        // the frame looked like without saying what was in it.
        void driveRefusalDiagnostics(plt::WindowHeadless& window) {
            settleGeometry(window);
            Vector<SessionPane> panes;
            probePanesDrained = drainPanes(panes) && panes.length() == 2;
            if (!probePanesDrained) {
                return;
            }
            CountingRenderer* const spy = composer.pool->make<CountingRenderer>(composer.renderer);
            composer.renderer = spy;

            // Refusing everything stands in for the refusals a window
            // cannot mend - a grid of zero, a surface that went away
            // under the backend. Whatever the reason, the answer the
            // window has is the one it already gave, so the frames repeat
            // and nothing but this line says so.
            spy->refuseEverything = true;
            {
                CapturedStderr captured(diagnosticsPath);
                for (int at = 0; at < 4; ++at) {
                    panes[0].terminal->feedPty(StringView(u8"z"));
                    composer.window->requestFrame();
                    refusedFramesPresented += window.dispatchFrame() ? 1 : 0;
                }
                captured.collect(diagnostics);
            }
            refusedFrameUpdates = spy->paneUpdates;

            // The other direction, on the same window and the same panes:
            // a backend that takes its frames says nothing at all.
            spy->refuseEverything = false;
            spy->reset();
            {
                CapturedStderr captured(diagnosticsPath);
                for (int at = 0; at < 4; ++at) {
                    panes[0].terminal->feedPty(StringView(u8"z"));
                    composer.window->requestFrame();
                    healthyFramesPresented += window.dispatchFrame() ? 1 : 0;
                }
                captured.collect(healthyDiagnostics);
            }

            // M11 (F3): and a second stall on the same window, to pin
            // the one line nothing guarded - the counter going back to
            // zero on the frame the backend takes. Without it these
            // three refusals are the fifth, sixth and seventh in a row
            // rather than the first three, the threshold is long past
            // and the stride does not land, and a window that stopped
            // presenting a second time says nothing at all about it.
            spy->refuseEverything = true;
            {
                CapturedStderr captured(diagnosticsPath);
                for (int at = 0; at < 3; ++at) {
                    panes[0].terminal->feedPty(StringView(u8"z"));
                    composer.window->requestFrame();
                    secondStallFramesPresented += window.dispatchFrame() ? 1 : 0;
                }
                captured.collect(secondStallDiagnostics);
            }

            // Z1 (F3): and the end of that second stall's reports. The
            // repeat used to have none - a window stalled on something
            // it cannot mend wrote the same line about the same frame
            // for as long as it lived, into a journal nobody trimmed.
            // Six reports and then quiet, counted here from the second
            // one on: the first was captured above.
            {
                CapturedStderr captured(diagnosticsPath);
                for (int at = 0; at < 3700; ++at) {
                    panes[0].terminal->feedPty(StringView(u8"z"));
                    composer.window->requestFrame();
                    cappedFramesPresented += window.dispatchFrame() ? 1 : 0;
                }
                captured.collect(cappedDiagnostics);
            }
            spy->refuseEverything = false;

            composer.renderer = spy->inner;
            drainFrames(window);
        }

        Composer& composer;
        bool fired = false;
        bool framePresented = false;
        bool splitPanes = false;
        bool split = false;
        u64 secondPane = 0;
        Vterm* firstTerminal = nullptr;
        bool secondFramePresented = false;
        u64 generationAfterFirstFrame = 0;
        plt::WindowTextInputRect anchor;

        // T5.2: the grid showWindow() asked for, and the anchor of a
        // cell that is not the home cell.
        u16 gridColumnsAtShow = 0;
        u16 gridRowsAtShow = 0;
        bool cursorAnchorProbe = false;
        bool cursorFramePresented = false;
        u64 anchorsBeforeCursorFrame = 0;
        i32 cursorPaneOriginX = -1;
        i32 cursorPaneOriginY = -1;
        u16 cursorGridColumns = 0;
        u16 cursorGridRows = 0;
        plt::WindowTextInputRect cursorAnchor;

        // F6: the frame's rectangles, on both sides of the bridge.
        bool frameBridgeProbe = false;
        bool bridgePanesDrained = false;
        bool bridgeFramePresented = false;
        Vector<PixelRect> bridgeBoxAreas;
        Vector<PixelRect> bridgeSurfaceAreas;
        Insets bridgeChrome;
        Insets bridgeContent;

        // R8-test: the retained-output half of the frame. Off for T10's
        // own stand, which asserts about the frames before this one.
        bool quietPaneProbe = false;
        bool quietPaneDrained = false;
        bool quietFramePresented = false;
        u64 anchorsBeforeQuietFrame = 0;
        i32 quietPaneOriginX = 0;
        i32 quietPaneOriginY = 0;
        plt::WindowTextInputRect quietAnchor;

        // R3-test.
        bool probePanesDrained = false;

        bool screenChangeProbe = false;
        bool ordinaryFramePresented = false;
        u32 ordinaryPaneUpdates = 0;
        u32 ordinaryRefusals = 0;
        bool screenChangeFramePresented = false;
        int screenChangeDispatches = 0;
        u32 screenChangePaneUpdates = 0;
        u32 screenChangeRefusals = 0;

        bool synchronizedNeighbourProbe = false;
        bool synchronizedNeighbourArmed = false;
        bool synchronizedNeighbourSilent = false;
        bool synchronizedNeighbourStillArmed = false;
        bool synchronizedFramePresented = false;
        int synchronizedDispatches = 0;
        u32 synchronizedPaneUpdates = 0;

        bool rendererRebuildProbe = false;
        u64 generationBeforeRebuild = 0;
        u64 generationAfterRebuild = 0;
        bool rebuiltFramePresented = false;
        bool rebuiltRendererExists = false;
        bool frameAfterRebuildPresented = false;
        u32 paneUpdatesAfterRebuild = 0;

        bool refusalDiagnosticsProbe = false;
        const char* diagnosticsPath = nullptr;
        int refusedFramesPresented = 0;
        int healthyFramesPresented = 0;
        u32 refusedFrameUpdates = 0;
        int secondStallFramesPresented = 0;
        int cappedFramesPresented = 0;
        Buffer diagnostics;
        Buffer healthyDiagnostics;
        Buffer secondStallDiagnostics;
        Buffer cappedDiagnostics;
    };

    struct StopOnTimeout final: plt::TimerCallback {
        explicit StopOnTimeout(plt::Platform& platform_)
            : platform(platform_)
        {
        }

        void ready() override {
            fired = true;
            platform.stop();
        }

        plt::Platform& platform;
        bool fired = false;
    };
}

STD_TEST_SUITE(ApplicationProduction) {
    STD_TEST(HeadlessRunWiresPresentsAndTearsDownProductionComponents) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        // Application and its threaded Pty have the same process lifetime
        // here as in runMain. Sessions still tear down through their arenas.
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::InputSink* const router = composer.input;
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf '\\033]2;orchestrated\\007seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(9, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.fired);
        STD_INSIST(drive.framePresented);
        STD_INSIST(!timeout.fired);
        STD_INSIST(composer.platform == platform);
        STD_INSIST(composer.input != router);
        STD_INSIST(composer.window != nullptr);
        STD_INSIST(composer.pty != nullptr);
        STD_INSIST(composer.sessions != nullptr);
        STD_INSIST(composer.renderer == nullptr);
        STD_INSIST(SessionSet::liveSessions == 0);

        auto& window = static_cast<plt::WindowHeadless&>(*composer.window);
        STD_INSIST(window.presentedFrame().generation == 1);
        STD_INSIST(window.title() == StringView(u8"orchestrated"));

        // The input method's candidate window is anchored to the cursor
        // cell, and the cell's origin takes its x from the horizontal
        // inset and its y from the vertical one. The frame above is
        // presented before a single keystroke, so the cursor is the home
        // cell and the anchor is the content box's own corner - the two
        // reserves DriveApplication set make that corner two different
        // numbers, which is the whole point of asserting it.
        const plt::WindowTextInputRect anchor = window.requestedTextInputRect();
        const Insets insets = composer.contentInsets();

        STD_INSIST(anchor.count == 1);
        STD_INSIST(insets.left != insets.top);
        STD_INSIST(anchor.x == (i32)(insets.left));
        STD_INSIST(anchor.y == (i32)(insets.top));
        STD_INSIST(anchor.width == composer.geometry.cellPixelWidth);
        STD_INSIST(anchor.height == composer.geometry.cellPixelHeight);

        // R8-test: no reaping here, and deliberately none.
        //
        // This used to be `while (waitpid(-1, nullptr, 0) > 0) {}`, written
        // when the Pty suite waited on -1 as well and could pick up this
        // suite's zombies instead of its own. That end has since been
        // fixed - pty_ut.cpp reaps by pid and says why, and
        // PtyHandle::childPid() exists for exactly that - so the reason for
        // draining here is gone, while the drain itself became the same
        // bug pointing the other way: a blind wait takes whichever corpse
        // is ready, including a child another suite is about to wait for.
        //
        // Measured, at e74011b8 plus this branch, with
        // `--threads=8 Pty Fiber ApplicationProduction ToggleQuickWindow`:
        //
        //   with the drains        5 of 6 runs red, always
        //                          `waitpid(child, &status, 0) == child`
        //                          at pty_ut.cpp:226
        //   without the drains     0 of 13 runs with that failure
        //   without this suite     0 of 6 runs red, drains or no drains
        //
        // The drains were the thief, and the second row is what says so:
        // the only thing that changed between the first two is these three
        // loops.
        //
        // What --threads=8 does NOT become is green: it has other,
        // structural cross-suite couplings that are not reaping and are not
        // a test file's to fix. SessionSet::liveSessions is one static for
        // the whole process, so Pty::EofClosesOneSessionBeforeItsFollowupWake
        // counts this suite's sessions too (one run in the thirteen), and
        // three Application::run() at once deadlock on the same static and
        // on the process-wide SIGCHLD disposition (one run in the thirteen).
        // The build runs this binary at --threads=1 (build.py), which is
        // where none of that arises.
        //
        // What is left behind is a handful of zombies for the lifetime of
        // the test binary. Nothing waits on -1 any more, so nothing can
        // trip over them. Reaping them properly would mean waiting on the
        // pids of *these* shells, and no seam hands them over: run() makes
        // the Pty itself (application.cpp) and neither SessionSet nor Vterm
        // exposes the handle whose childPid() would answer.
    }

    // T5.2, the first of two holes wave 6 measured at the call sites of
    // grid_geometry.h. The arithmetic of cellOrigin() is covered cell by
    // cell in grid_geometry_ut.cpp, including cellOrigin(3, 1, ...); the
    // hand-off in ApplicationImpl::presentTerminal() is not, and the
    // three anchor tests in this file could not cover it because all
    // three read the anchor on the home cell.
    //
    // Measured on this tree, against 959 green tests:
    //
    //   cellOrigin(posY, posX, ...)                      0 red
    //   cellOrigin(..., cellPixelHeight, cellPixelWidth) 0 red
    //   contentInsets() -> Insets{}                      3 red
    //
    // The third is the control: the insets *are* watched, because
    // DriveApplication sets two reserves that are not each other. The
    // column and the row are not, and neither is the cell box, because
    // at (0, 0) both terms they multiply are zero. This test is the same
    // anchor read one cell in from the corner and one row down, which is
    // the smallest fixture that separates all three.
    STD_TEST(TheAnchorOffTheHomeCellTakesItsColumnFromTheWidthAndItsRowFromTheHeight) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.cursorAnchorProbe = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(9, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.fired);
        STD_INSIST(!timeout.fired);
        STD_INSIST(drive.cursorFramePresented);

        auto& window = static_cast<plt::WindowHeadless&>(*composer.window);
        const Insets insets = composer.contentInsets();
        const u32 glyphWidth = composer.geometry.cellPixelWidth;
        const u32 glyphHeight = composer.geometry.cellPixelHeight;

        // The fixture, asserted rather than assumed. Each of these four
        // is one of the ways this test can quietly stop testing what it
        // was written for, and every one of them has a precedent in this
        // file: a square cell box makes the width and the height
        // interchangeable, equal insets make the two sides
        // interchangeable, a pane origin that is not zero adds a number
        // to both axes, and a grid too small for the cell puts the
        // cursor somewhere other than where the CUP asked.
        STD_INSIST(glyphWidth != glyphHeight);
        STD_INSIST(insets.left != insets.top);
        STD_INSIST(drive.cursorPaneOriginX == 0);
        STD_INSIST(drive.cursorPaneOriginY == 0);
        STD_INSIST(drive.cursorGridColumns > 3);
        STD_INSIST(drive.cursorGridRows > 1);

        // This frame anchored, and it is this frame's anchor being read
        // and not the previous one's - the position alone would hold
        // just as well if the frame had never anchored at all.
        STD_INSIST(drive.cursorAnchor.count == drive.anchorsBeforeCursorFrame + 1);

        // The anchor itself: column 3 times the cell *width* off the
        // *left* inset, row 1 times the cell *height* off the *top* one.
        STD_INSIST(drive.cursorAnchor.x == (i32)(insets.left + 3 * glyphWidth));
        STD_INSIST(drive.cursorAnchor.y == (i32)(insets.top + 1 * glyphHeight));

        // And spelled out again as four numbers that differ from each
        // other, so that the assertions above cannot all be satisfied by
        // one wrong value standing in for another. Written as
        // inequalities against what the two mutations produce, because
        // that is the sentence this test exists to say: the transposed
        // call gives insets.left + 1 * glyphWidth for x, and the swapped
        // cell box gives insets.left + 3 * glyphHeight.
        STD_INSIST(drive.cursorAnchor.x != (i32)(insets.left + 1 * glyphWidth));
        STD_INSIST(drive.cursorAnchor.x != (i32)(insets.left + 3 * glyphHeight));
        STD_INSIST(drive.cursorAnchor.y != (i32)(insets.top + 3 * glyphHeight));
        STD_INSIST(drive.cursorAnchor.y != (i32)(insets.top + 1 * glyphWidth));

        // The cell box is handed over whole, and this is the only test
        // that reads it off a frame where the anchor moved - so a width
        // and a height swapped *here* would not be the same swap as the
        // one above.
        STD_INSIST(drive.cursorAnchor.width == glyphWidth);
        STD_INSIST(drive.cursorAnchor.height == glyphHeight);

        // The window is the one the run built, and no other.
        STD_INSIST(window.requestedTextInputRect().count >= drive.cursorAnchor.count);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // T5.2, the second hole. ApplicationImpl::showWindow() is the one
    // caller that turns -geometry into pixels, and its two calls -
    // gridPixelWidth(nCols, ...) and gridPixelHeight(nRows, ...) - take
    // the column count and the row count as bare u32. Swapping them
    // compiles, runs, and asks the window for a square-ish grid instead
    // of the one the user asked for; measured on this tree, both
    // swaps were green across all 959 tests.
    //
    // WindowSizingRequests covers the other end of the same arithmetic -
    // what fontChanged() asks for after the window exists - and cannot
    // see this one: showWindow() runs once, before the event loop, and
    // re-counts the grid on the Composer rather than requesting a size
    // from the window. The grid it leaves behind is read at the top of
    // DriveApplication::ready(), which is the first thing that runs
    // after the loop starts and before any frame has re-counted it.
    //
    // -geometry 24x7 rather than the 20x4 the other runs use: the two
    // numbers have to differ, and differing from the other tests' pair
    // as well means a stale reading cannot pass for a fresh one.
    STD_TEST(TheWindowShownAtStartupAsksForTheRequestedColumnsAndRowsOnTheirOwnAxes) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char geometry[] = "-geometry";
        char geometryValue[] = "24x7";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(9, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.fired);
        STD_INSIST(!timeout.fired);

        // The requested grid, on the requested axes. gridPixelWidth()
        // and gridColumns() are exact inverses, so this is the column
        // count showWindow() spelled and not an approximation of it.
        STD_INSIST(composer.opts->nCols == 24);
        STD_INSIST(composer.opts->nRows == 7);
        STD_INSIST(drive.gridColumnsAtShow == 24);
        STD_INSIST(drive.gridRowsAtShow == 7);

        // And the pair is not a square, so neither number can stand in
        // for the other. Without this the two assertions above hold
        // under both swaps and say nothing.
        STD_INSIST(drive.gridColumnsAtShow != drive.gridRowsAtShow);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // T10/T13 acceptance: the frame is every visible pane, and the whole
    // production path is walked - Application::run, the renderer, the
    // window - rather than a harness standing in for it. Until this, the
    // multi-pane presentTerminal() had no stand at all: every renderer
    // test hands the backend a frame it built itself, and every session
    // test stops at the pane list.
    STD_TEST(ATabOfTwoPanesPresentsOneFrameAndAnchorsOnTheFocusedOne) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(10, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.split);
        STD_INSIST(drive.secondPane != 0);
        STD_INSIST(drive.framePresented);
        STD_INSIST(drive.secondFramePresented);
        STD_INSIST(!timeout.fired);

        // One present for the whole tab, not one per pane.
        STD_INSIST(drive.generationAfterFirstFrame == 1);

        // The candidate window follows the pane being typed into. The
        // focus was put back on the first pane, so the anchor is the
        // window's own content corner; taking the *last* pane in the list
        // instead - which is what a loop that overwrites the anchor every
        // iteration does - would put it half a window to the right.
        //
        // A1: the corner is read off contentInsets() rather than rebuilt
        // from the reserves DriveApplication set. Those are set in
        // *points* and contentInsets() scales them, so a corner spelled
        // as the raw reserve plus the scalar border option is the same
        // number only while contentScale is 1 - and it charges one
        // symmetric border to both axes, which is the one thing A1 says
        // the layout never is.
        STD_INSIST(drive.anchor.count == 1);
        const Insets insets = composer.contentInsets();
        STD_INSIST(drive.anchor.x == (i32)(insets.left));
        STD_INSIST(drive.anchor.y == (i32)(insets.top));

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // F6, R6-arch finding 2. A10's other road. SessionSet hands out pane
    // rectangles counted inside the content box; a frame's are counted
    // on the surface; buildFrameUpdates() is the one place the two are
    // bridged, and application.cpp says so in words. Nothing held it:
    // asking contentInsets() there, and deleting the addition outright,
    // both survived all 963 tests.
    //
    // The test above walks the same function and cannot see either one.
    // It reads the anchor, and the anchor's x is composed further down
    // presentTerminal() against contentInsets() - a different reading of
    // a different inset - so the rectangles handed to the backend are
    // untouched by any assertion in the tree.
    //
    // Two things the fixture has to keep, or this asserts nothing:
    //
    //  - a border that is not zero, or chromeInsets() and
    //    contentInsets() are the same number and the substitution is
    //    invisible. -border is passed explicitly rather than left to the
    //    default, and its value is asserted below.
    //  - a reserve that is not zero on either axis, or the addition can
    //    be deleted without moving a pixel. DriveApplication sets 3 and
    //    16, which are also not each other, so an axis taking the other
    //    axis's reserve is caught too.
    STD_TEST(EveryPanesRectangleCrossesToTheSurfaceByTheChromeReserveAlone) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        drive.frameBridgeProbe = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        // Seven, and said out loud: the default is 2, which would do,
        // but a default is not a fixture and the next person to change
        // it would blind this test without touching it.
        char border[] = "-border";
        char borderValue[] = "7";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            border,
            borderValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(12, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(!timeout.fired);
        STD_INSIST(drive.split);
        STD_INSIST(drive.bridgePanesDrained);
        STD_INSIST(drive.bridgeFramePresented);

        // The fixture, before anything is read off it. Each of these is
        // a way the assertions below could hold while saying nothing.
        const u16 reserveLeft = drive.bridgeChrome.left;
        const u16 reserveTop = drive.bridgeChrome.top;
        STD_INSIST(reserveLeft == 3);
        STD_INSIST(reserveTop == 16);
        STD_INSIST(reserveLeft != reserveTop);
        // The border is in there, so the two insets differ: without this
        // the substitution under test is not a different number.
        STD_INSIST(drive.bridgeContent.left == reserveLeft + 7);
        STD_INSIST(drive.bridgeContent.top == reserveTop + 7);
        STD_INSIST(drive.bridgeContent.left != reserveLeft);
        STD_INSIST(drive.bridgeContent.top != reserveTop);

        // Two panes on both sides of the bridge, and the same two.
        STD_INSIST(drive.bridgeBoxAreas.length() == 2);
        STD_INSIST(drive.bridgeSurfaceAreas.length() == 2);

        // The split is vertical, so the far pane starts somewhere the
        // near one does not. Without this the pane offset is zero on
        // both panes and the reserve is the whole answer for each -
        // which a bridge that ignored the pane's own x would also give.
        STD_INSIST(drive.bridgeBoxAreas[0].x == 0);
        STD_INSIST(drive.bridgeBoxAreas[1].x != 0);

        for (size_t at = 0; at < 2; ++at) {
            const PixelRect box = drive.bridgeBoxAreas[at];
            const PixelRect surface = drive.bridgeSurfaceAreas[at];
            // The reserve, once, and nothing else: not the border, which
            // is air the pane keeps inside its own rectangle, and not
            // nothing.
            STD_INSIST(surface.x == reserveLeft + box.x);
            STD_INSIST(surface.y == reserveTop + box.y);
            // And the bridge moves a pane, it does not resize one.
            STD_INSIST(surface.width == box.width);
            STD_INSIST(surface.height == box.height);
        }

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // R8-test, the hole T10 handed over: presentTerminal()'s
    // retained-output branch. Two lines of the T13 contract live there
    // and had nothing behind them - "a pane with nothing to say hands
    // over its retained form" and "consume() only for the panes that
    // were asked through output()".
    //
    // The frame under test has one pane speaking and one silent, and the
    // silent one is the *focused* one. That is what makes the branch
    // readable from outside ApplicationImpl: the input method's anchor is
    // taken from the focused pane's update, whichever of the two forms
    // that update came from, so a frame that skipped the silent pane
    // would reach requestTextInputRect() with no anchor at all and record
    // nothing. Hence the count is asserted as well as the position -
    // position alone repeats the previous frame's number and would hold
    // just as well if this frame had never anchored anything.
    //
    // consume() on the silent pane is guarded by the product itself:
    // Vterm::consume() asserts on updateScreen, which a retained form
    // never sets.
    STD_TEST(TheQuietPaneOfAFrameHandsOverItsRetainedFormAndKeepsTheAnchor) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        drive.quietPaneProbe = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(10, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.split);
        STD_INSIST(!timeout.fired);

        // The pane really was silent going into the frame, and the frame
        // really was presented - without both of these the assertions
        // below are about some other frame than the one under test.
        STD_INSIST(drive.quietPaneDrained);
        STD_INSIST(drive.quietFramePresented);

        // The two panes are side by side, so the second one's anchor is a
        // different number from the first one's - which is what makes the
        // position worth asserting at all.
        STD_INSIST(drive.quietPaneOriginX > 0);
        STD_INSIST(drive.quietPaneOriginY == 0);

        // This frame anchored, and it anchored on the silent focused pane.
        STD_INSIST(drive.quietAnchor.count == drive.anchorsBeforeQuietFrame + 1);
        const Insets insets = composer.contentInsets();
        STD_INSIST(drive.quietAnchor.x == (i32)(insets.left) + drive.quietPaneOriginX);
        STD_INSIST(drive.quietAnchor.y == (i32)(insets.top) + drive.quietPaneOriginY);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // R3-test, T3's third and fourth acceptance criteria, counted.
    //
    // tst/test_pane_alt_screen.py already says the window keeps
    // presenting, and tst/test_pane_frame_recovery.py says what the
    // frames cost. Neither can see how many times one frame callback
    // asked the backend, and that is the sentence R1 is written in: a
    // refusal is answered inside the frame it happened in, by handing
    // over every pane whole, and an ordinary frame does not pay for it.
    STD_TEST(AScreenChangeIsAnsweredInsideItsOwnFrameAndOrdinaryFramesAreNot) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        drive.screenChangeProbe = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(10, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.split);
        STD_INSIST(!timeout.fired);
        // Both panes really were silent going in, or the counts below
        // are counts of some other pair of frames.
        STD_INSIST(drive.probePanesDrained);

        // The successful path, measured rather than read: one ask, no
        // refusal. A window that exposed every pane every frame would
        // land here with the same pixels and the same one ask - so the
        // refusal count is what says the exposure did not happen, and
        // tst/test_pane_frame_recovery.py reads the rows that prove it.
        STD_INSIST(drive.ordinaryFramePresented);
        STD_INSIST(drive.ordinaryPaneUpdates == 1);
        STD_INSIST(drive.ordinaryRefusals == 0);

        // And the frame one pane changed screens in: refused once,
        // answered with every pane handed over whole, taken the second
        // time it was asked - which is the plan's "no later than the
        // second presentTerminal()" counted rather than assumed.
        //
        // Exactly one refusal is the load-bearing number, and it is
        // two-sided: a window that answered with anything less than the
        // panes whole would refuse again and again (expose() alone does
        // exactly that, R2), and one that handed the panes over on every
        // frame would refuse none - and would draw the whole window on
        // every keystroke, which is what the ordinary frame above says
        // it does not.
        STD_INSIST(drive.screenChangeFramePresented);
        STD_INSIST(drive.screenChangeDispatches <= 2);
        STD_INSIST(drive.screenChangeRefusals == 1);
        STD_INSIST(drive.screenChangePaneUpdates == 2);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // R3a-1 (F3): the same frame, with the silent neighbour inside a
    // synchronized update. Handing every pane over whole is only an
    // answer if the pane can hear it, and a pane inside \e[?2026h could
    // not: redraw() returns without arming the pending flag, so the
    // pane offered its retained form again, the reshaped frame was
    // refused again, and the window stopped presenting. Twelve of
    // twelve frames refused when R3-arch probed it.
    //
    // Nothing exotic about the state. \e[?2026h is what a modern TUI
    // wraps every one of its redraws in, and the neighbour changing
    // screens is one of them starting or leaving vim - the two only
    // have to fall inside the same 150ms.
    STD_TEST(AScreenChangeSettlesEvenWhenTheSilentNeighbourIsInsideASynchronizedUpdate) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        drive.synchronizedNeighbourProbe = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(10, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.split);
        STD_INSIST(!timeout.fired);
        STD_INSIST(drive.probePanesDrained);

        // The scenario really was set up, or what follows is the plain
        // screen change above under another name: the neighbour is
        // inside a synchronized update, and it is silent.
        STD_INSIST(drive.synchronizedNeighbourArmed);
        STD_INSIST(drive.synchronizedNeighbourSilent);

        // And the frame lands anyway, on the callback it was given.
        // Without exposeAll() lifting the mode this is false on every
        // dispatch, forever: the test build starts no timers, so the
        // watchdog that saves the product is not here.
        STD_INSIST(drive.synchronizedFramePresented);
        STD_INSIST(drive.synchronizedDispatches == 1);
        STD_INSIST(drive.synchronizedPaneUpdates == 2);

        // The mode is gone rather than merely worked around, which is
        // the whole of the fix and the reason resizeGrid() does the same
        // thing: a pane still holding it would refuse the next reshaped
        // frame the same way.
        STD_INSIST(!drive.synchronizedNeighbourStillArmed);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // R3-test, the plan's second path, and the one the diagnosis reached
    // by reading the code rather than by measuring: a renderer that died
    // with its surface is rebuilt inside frame(), and the fresh one
    // retains no panes at all. Before T3 the window exposed the active
    // terminal alone here, leaving every other pane of the window owing
    // rows it had not got - the same stall, reached by losing a surface
    // instead of by changing a screen.
    //
    // What this pins is that the path is reachable and that a window of
    // two panes comes back from it with a frame on the callback it was
    // given. It cannot tell the exposure in frame() from the one
    // presentTerminal() would do on the refusal that followed - the two
    // leave the same pixels and the same counts.
    STD_TEST(ARebuiltRendererPresentsAWindowOfTwoPanesOnTheFrameItWasGiven) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        drive.rendererRebuildProbe = true;
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(10, argv);
        poller->cancel(timeout);

        STD_INSIST(result == 0);
        STD_INSIST(drive.split);
        STD_INSIST(!timeout.fired);
        STD_INSIST(drive.probePanesDrained);

        // A renderer was built to replace the one that was dropped...
        STD_INSIST(drive.rebuiltRendererExists);
        // ...and the frame it was built for landed, on that callback and
        // not on a later one: exactly one more present than before.
        STD_INSIST(drive.rebuiltFramePresented);
        STD_INSIST(drive.generationAfterRebuild == drive.generationBeforeRebuild + 1);

        // And the window is an ordinary window again afterwards - one
        // ask per frame, the rebuild answered once rather than latched.
        STD_INSIST(drive.frameAfterRebuildPresented);
        STD_INSIST(drive.paneUpdatesAfterRebuild == 1);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }

    // R3 and T3's third acceptance criterion: the refusal a full expose
    // does not cure. Handing over every pane whole is the strongest
    // answer a window has, so a frame refused after it is refused for
    // something the window cannot mend, and it will ask for that same
    // frame for as long as it lives. That was the entire symptom the
    // first time - a percent of a CPU and a picture that had stopped,
    // with nothing anywhere to say which - and this is the line that
    // now says it.
    //
    // Both directions. A diagnostic that fires on healthy frames is a
    // diagnostic nobody reads, so the second capture is as much of the
    // test as the first.
    STD_TEST(AnIncurableRefusalReachesStderrAndAHealthyWindowSaysNothing) {
        SavedSignals savedSignals;
        keepMallocDebugOutOfTheChildren();
        StringBuilder directory;
        makeTempDir(directory);
        StringBuilder path;
        path << StringView((const u8*)(directory.cStr()), strlen(directory.cStr())) << StringView(u8"/stderr.txt");
        ObjPool* const pool = ObjPool::fromMemoryRaw();
        Composer& composer = *pool->make<Composer>(pool);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.platform = platform;
        auto* const poller = static_cast<plt::PollerLoop*>(platform->poller());
        DriveApplication drive(composer);
        drive.splitPanes = true;
        drive.refusalDiagnosticsProbe = true;
        drive.diagnosticsPath = path.cStr();
        StopOnTimeout timeout(*platform);
        poller->timeout(1, drive);
        poller->timeout(5'000'000, timeout);

        Application* const application = Application::create(composer);
        char program[] = "application_ut";
        char config[] = "-config";
        char configPath[] = "/dev/null";
        char panes[] = "-panes";
        char geometry[] = "-geometry";
        char geometryValue[] = "20x4";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char commandFlag[] = "-c";
        char script[] = "IFS= read -r line; printf 'seen:%s\\n' \"$line\"";
        char* argv[] = {
            program,
            config,
            configPath,
            panes,
            geometry,
            geometryValue,
            execute,
            shell,
            commandFlag,
            script,
            nullptr,
        };

        const int result = application->run(10, argv);
        poller->cancel(timeout);
        unlink(path.cStr());
        rmdir(directory.cStr());

        STD_INSIST(result == 0);
        STD_INSIST(drive.split);
        STD_INSIST(!timeout.fired);
        STD_INSIST(drive.probePanesDrained);

        // Four frames offered and none of them taken, each asked for
        // twice - once as it came and once with every pane handed over
        // whole, which is the answer that did not help.
        STD_INSIST(drive.refusedFramesPresented == 0);
        STD_INSIST(drive.refusedFrameUpdates == 8);

        // Said once and not four times: the threshold is three frames in
        // a row, and past it the line repeats on a stride rather than on
        // every frame. A window that stalls at sixty frames a second
        // would otherwise write the stall into the scrollback it stalled
        // over.
        STD_INSIST(mentionCount(drive.diagnostics, "refused") == 1);
        STD_INSIST(mentions(drive.diagnostics, "renderer refused 3 frames in a row"));
        // What the line is for: the grids and the row counts, which are
        // what tells a refusal the window can mend from one it cannot.
        STD_INSIST(mentionCount(drive.diagnostics, "pane ") == 2);
        STD_INSIST(mentions(drive.diagnostics, "pane 0: grid 9x4"));
        STD_INSIST(mentions(drive.diagnostics, "pane 1: grid 9x4"));
        STD_INSIST(mentions(drive.diagnostics, "4 row(s) given"));
        // Grids and counts only. The frame carries what the user is
        // reading, and the pane was fed a character to make it speak -
        // so if any cell of it could reach a log, that character would.
        STD_INSIST(!mentions(drive.diagnostics, "z"));

        // And with the same four frames taken, nothing at all.
        STD_INSIST(drive.healthyFramesPresented == 4);
        STD_INSIST(drive.healthyDiagnostics.used() == 1);

        // M11 (F3): the counter starts over on the frame that lands, so
        // a window that stalls a second time is news a second time. This
        // is the only thing that reads the reset - remove it and the
        // three refusals below are the fifth, sixth and seventh, past
        // the threshold and off the stride, and this capture is empty.
        STD_INSIST(drive.secondStallFramesPresented == 0);
        STD_INSIST(mentionCount(drive.secondStallDiagnostics, "refused") == 1);
        STD_INSIST(mentions(drive.secondStallDiagnostics, "renderer refused 3 frames in a row"));

        // Z1 (F3): the repeat ends. Six reports about one stall and no
        // more - one at three frames and five on the stride, the first
        // of the six captured above and the other five here - and the
        // last of them says so, so that the silence after it is not
        // read as a window that recovered. Three thousand seven hundred
        // frames were offered past that first report and refused; a
        // repeat without a bound would have written six of these.
        STD_INSIST(drive.cappedFramesPresented == 0);
        STD_INSIST(mentionCount(drive.cappedDiagnostics, "renderer refused") == 5);
        STD_INSIST(mentions(drive.cappedDiagnostics, "refused 3003 frames in a row"));
        STD_INSIST(!mentions(drive.cappedDiagnostics, "refused 3603 frames in a row"));
        STD_INSIST(mentionCount(drive.cappedDiagnostics, "no more will be said") == 1);

        // No reaping - see the note in
        // HeadlessRunWiresPresentsAndTearsDownProductionComponents.
    }
}

// The arithmetic behind the window's minimum size and resize increment
// is covered cell by cell in grid_geometry_ut.cpp; the hand-off is not. plt::Window takes each
// pair as two u32 scalars, so swapping them at the call site in
// ApplicationImpl::fontChanged() compiles and runs, and every backend
// but this one drops the request into a real window manager that obeys
// it silently - the mutation F3 named R14' and measured as green across
// the whole graph. The headless window now records both pairs, which is
// what makes the hand-off itself readable.
STD_TEST_SUITE(WindowSizingRequests) {
    STD_TEST(TheMinimumSizeAndTheResizeUnitPairEachAxisWithItsOwnInsets) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        options.border = 0;
        options.nCols = 80;
        options.nRows = 24;
        composer.setOptions(&options);
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        composer.geometry.setCellPixelSize(8, 16);
        // Chrome on two edges, so the horizontal reserve and the
        // vertical one are different numbers and neither can stand in
        // for the other. Without a reserve on any side the two axes of
        // the reserve are both `2 * border` and a swap is invisible
        // again - which is exactly why this test could not be written
        // before wave 4.
        composer.setChromeReserve(ChromeSide::Right, 220);
        composer.setChromeReserve(ChromeSide::Top, 32);
        Application::create(composer);

        // What Composer does after a font is replaced or its size
        // changes: fontChanged() is reached through this list, never
        // called directly from outside.
        for (IntrusiveNode* node = composer.fontChangedListeners.mutFront(); node != composer.fontChangedListeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }

        auto& window = static_cast<plt::WindowHeadless&>(*composer.window);
        const Insets insets = composer.contentInsets();

        STD_INSIST(insets.left == 0);
        STD_INSIST(insets.right == 220);
        STD_INSIST(insets.top == 32);
        STD_INSIST(insets.bottom == 0);

        const plt::WindowSizeRequest minimum = window.requestedMinimumSize();

        STD_INSIST(minimum.count == 1);
        // One cell plus the reserve, each axis out of its own two sides.
        STD_INSIST(minimum.width == gridPixelWidth(1, insets, composer.geometry.cellPixelWidth));
        STD_INSIST(minimum.height == gridPixelHeight(1, insets, composer.geometry.cellPixelHeight));
        STD_INSIST(minimum.width == 228);
        STD_INSIST(minimum.height == 48);

        const plt::WindowResizeUnitRequest unit = window.requestedResizeUnit();

        STD_INSIST(unit.count == 1);
        STD_INSIST(unit.width == composer.geometry.cellPixelWidth);
        STD_INSIST(unit.height == composer.geometry.cellPixelHeight);
        STD_INSIST(unit.baseWidth == gridPixelWidth(0, insets, composer.geometry.cellPixelWidth));
        STD_INSIST(unit.baseHeight == gridPixelHeight(0, insets, composer.geometry.cellPixelHeight));
        STD_INSIST(unit.baseWidth == 220);
        STD_INSIST(unit.baseHeight == 32);

        // And the window it asks for is the requested geometry with the
        // same two reserves put back, width from the horizontal pair.
        const plt::WindowInfo info = composer.window->info();

        STD_INSIST(info.width == gridPixelWidth(80, insets, composer.geometry.cellPixelWidth));
        STD_INSIST(info.height == gridPixelHeight(24, insets, composer.geometry.cellPixelHeight));
        STD_INSIST(info.width == 860);
        STD_INSIST(info.height == 416);
    }
}

// toggleQuickWindow() is the one entry point ui_quick_hotkey's Carbon
// handler calls; it is plain portable logic over plt::Window (no
// #if defined(__APPLE__) guard on its own definition in application.cpp),
// so the headless backend exercises the real thing here, not a stand-in.
// What it must not do - register a real global hotkey, touch a live
// NSWindow/focus - is out of scope by construction: this function never
// touches Carbon at all, that lives in ui_quick_hotkey.mm instead.
STD_TEST_SUITE(ToggleQuickWindow) {
    STD_TEST(NullWindowIsANoOp) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        STD_INSIST(composer.window == nullptr);

        toggleQuickWindow(composer);

        STD_INSIST(composer.window == nullptr);
    }

    STD_TEST(HiddenWindowIsShownByToggle) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        STD_INSIST(!composer.window->visible());

        toggleQuickWindow(composer);

        STD_INSIST(composer.window->visible());
    }

    STD_TEST(VisibleWindowIsHiddenByToggle) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        composer.window->requestShow();
        STD_INSIST(composer.window->visible());

        toggleQuickWindow(composer);

        STD_INSIST(!composer.window->visible());
    }

    // Regression test: a toggle that only
    // asks visible() would hide an already-Dock-minimized window instead
    // of restoring it, because a minimized NSWindow still answers
    // isVisible with true. toggleQuickWindow() must also check
    // info().iconified and take the show branch instead.
    STD_TEST(IconifiedVisibleWindowIsShownRatherThanHiddenByToggle) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        composer.window->requestShow();
        composer.window->requestIconify();
        STD_INSIST(composer.window->visible());
        STD_INSIST(composer.window->info().iconified);

        toggleQuickWindow(composer);

        STD_INSIST(composer.window->visible());
    }

    // A6: a saved frame wins over the default placement once
    // quickRememberFrame is on and a state file exists.
    // WindowHeadlessImpl starts every window at x=10, y=20, 800x600
    // (its constructor) and requestShowAt() on this backend never
    // changes geometry (it falls through to requestShow(), see
    // platform_headless.cpp) - so unaffected geometry after a show is
    // exactly this backend's real default, not a stand-in value.
    STD_TEST(ShowAppliesTheSavedFrameOverTheDefaultPlacement) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();

        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder configPath;
        configPath << StringView(dir) << StringView(u8"/config.toml");
        StringBuilder framePath;
        STD_INSIST(defaultQuickFramePath(StringView(configPath), framePath));
        const QuickFrame saved{.x = 100, .y = 50, .width = 640, .height = 480};
        STD_INSIST(saveQuickFrame(StringView(framePath), saved));

        Options options;
        options.quickRememberFrame = true;
        options.configPath = StringView(configPath);
        composer.setOptions(&options);

        toggleQuickWindow(composer);

        const plt::WindowInfo info = composer.window->info();
        STD_INSIST(info.x == 100);
        STD_INSIST(info.y == 50);
        STD_INSIST(info.width == 640);
        STD_INSIST(info.height == 480);

        unlink(framePath.cStr());
        rmdir(dir.cStr());
    }

    STD_TEST(ShowIgnoresASavedFrameWhenQuickRememberFrameIsOff) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();

        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder configPath;
        configPath << StringView(dir) << StringView(u8"/config.toml");
        StringBuilder framePath;
        STD_INSIST(defaultQuickFramePath(StringView(configPath), framePath));
        const QuickFrame saved{.x = 100, .y = 50, .width = 640, .height = 480};
        STD_INSIST(saveQuickFrame(StringView(framePath), saved));

        Options options;
        options.quickRememberFrame = false;
        options.configPath = StringView(configPath);
        composer.setOptions(&options);

        toggleQuickWindow(composer);

        const plt::WindowInfo info = composer.window->info();
        STD_INSIST(info.x == 10);
        STD_INSIST(info.y == 20);
        STD_INSIST(info.width == 800);
        STD_INSIST(info.height == 600);

        unlink(framePath.cStr());
        rmdir(dir.cStr());
    }

    // A6: no saved file falls back to the default placement, exactly the
    // same as before quickRememberFrame existed - deleting the state
    // file is the documented reset.
    STD_TEST(ShowWithNoSavedFileKeepsTheDefaultPlacement) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();

        Options options;
        options.quickRememberFrame = true;
        options.configPath = StringView(u8"/nonexistent/config.toml");
        composer.setOptions(&options);

        toggleQuickWindow(composer);

        const plt::WindowInfo info = composer.window->info();
        STD_INSIST(info.x == 10);
        STD_INSIST(info.y == 20);
        STD_INSIST(info.width == 800);
        STD_INSIST(info.height == 600);
    }

    // A6's clamp-into-the-current-screen: WindowHeadlessImpl's
    // constructor sets a simulated 1920x1080 screen, unrelated to the
    // window's own placement above.
    STD_TEST(ShowClampsASavedFrameLargerThanTheScreen) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();

        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder configPath;
        configPath << StringView(dir) << StringView(u8"/config.toml");
        StringBuilder framePath;
        STD_INSIST(defaultQuickFramePath(StringView(configPath), framePath));
        const QuickFrame saved{.x = 5000, .y = -500, .width = 5000, .height = 5000};
        STD_INSIST(saveQuickFrame(StringView(framePath), saved));

        Options options;
        options.quickRememberFrame = true;
        options.configPath = StringView(configPath);
        composer.setOptions(&options);

        toggleQuickWindow(composer);

        const plt::WindowInfo info = composer.window->info();
        STD_INSIST(info.width == 1920);
        STD_INSIST(info.height == 1080);
        STD_INSIST(info.x == 0);
        STD_INSIST(info.y == 0);

        unlink(framePath.cStr());
        rmdir(dir.cStr());
    }

    // F2's report, I1: the four tests above cannot tell a points clamp
    // from a pixels clamp, because at contentScale = 1 they are
    // numerically the same bound - ShowClampsASavedFrameLargerThanTheScreen
    // stayed green through the exact B1 mutation that broke this on any
    // Retina display. WindowHeadlessImpl::configure() lets a headless
    // window carry a real contentScale, so this covers B1 without a live
    // NSWindow at all.
    STD_TEST(ShowDoesNotHalveAFullScreenSavedFrameAtDoubleScale) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        plt::WindowHeadless& headlessWindow = static_cast<plt::WindowHeadless&>(*composer.window);
        headlessWindow.configure({
            .width = 800,
            .height = 600,
            .screenPixelWidth = 3840,
            .screenPixelHeight = 2160,
            .contentScale = 2.0f,
        });

        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder configPath;
        configPath << StringView(dir) << StringView(u8"/config.toml");
        StringBuilder framePath;
        STD_INSIST(defaultQuickFramePath(StringView(configPath), framePath));
        // The screen's own size in points - 3840x2160 backing pixels at
        // scale 2. Reading this saved size as backing pixels (B1's bug,
        // and what the pre-B4 file format actually stored) would divide
        // it by the scale and restore half a screen.
        const QuickFrame saved{.x = 0, .y = 0, .width = 1920, .height = 1080};
        STD_INSIST(saveQuickFrame(StringView(framePath), saved));

        Options options;
        options.quickRememberFrame = true;
        options.configPath = StringView(configPath);
        composer.setOptions(&options);

        toggleQuickWindow(composer);

        const plt::WindowInfo info = composer.window->info();
        STD_INSIST(info.width == 3840);
        STD_INSIST(info.height == 2160);
        STD_INSIST(info.x == 0);
        STD_INSIST(info.y == 0);

        unlink(framePath.cStr());
        rmdir(dir.cStr());
    }

    STD_TEST(ShowClampsASavedFramePositionInPointsNotPixelsAtDoubleScale) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        plt::WindowHeadless& headlessWindow = static_cast<plt::WindowHeadless&>(*composer.window);
        headlessWindow.configure({
            .width = 800,
            .height = 600,
            .screenPixelWidth = 3840,
            .screenPixelHeight = 2160,
            .contentScale = 2.0f,
        });

        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder configPath;
        configPath << StringView(dir) << StringView(u8"/config.toml");
        StringBuilder framePath;
        STD_INSIST(defaultQuickFramePath(StringView(configPath), framePath));
        // The screen is 3840x2160 backing pixels, 1920x1080 points at
        // scale 2. A saved 960x540-point window at x=1000 has its right
        // edge at 1960, past the 1920-point-wide screen; the correct
        // bound in points is 1920-960=960. A clamp computed in pixels
        // (B1's bug) would compare x=1000 against a 3840-wide bound and
        // never clamp it at all.
        const QuickFrame saved{.x = 1000, .y = 50, .width = 960, .height = 540};
        STD_INSIST(saveQuickFrame(StringView(framePath), saved));

        Options options;
        options.quickRememberFrame = true;
        options.configPath = StringView(configPath);
        composer.setOptions(&options);

        toggleQuickWindow(composer);

        const plt::WindowInfo info = composer.window->info();
        STD_INSIST(info.width == 1920);
        STD_INSIST(info.height == 1080);
        STD_INSIST(info.x == 960);
        STD_INSIST(info.y == 50);

        unlink(framePath.cStr());
        rmdir(dir.cStr());
    }

    // The other half of what applySavedQuickFrame() decides: which of
    // its two paths runs at all. applyQuickFrameToWindow() is what
    // answers that, and its answer is the backend tag - not the .window
    // pointer, which every backend fills in (the headless one with its
    // own render target). Bridging that pointer and messaging it took a
    // whole test binary down twice on this wave (F2's own report, and
    // R2-qa round 2's B5 in the sibling file). The tests around this one
    // would notice such a regression only as a SIGSEGV somewhere in the
    // middle of the suite; this one names it, and pins that a refused
    // window is left exactly as it was rather than half-moved.
    //
    // ui_quick_hotkey.mm is macOS-only in build.py, so off darwin there
    // is nothing to link against - the same guard applySavedQuickFrame()
    // itself now carries (R2-test, L1).
#if defined(__APPLE__)
    STD_TEST(ApplyingASavedFrameRefusesANonCocoaBackend) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        STD_INSIST(composer.window->renderContext().backend != plt::RenderBackend::Cocoa);
        STD_INSIST(composer.window->renderContext().window != nullptr);
        const plt::WindowInfo before = composer.window->info();

        STD_INSIST(!applyQuickFrameToWindow(composer, {.x = 100, .y = 50, .width = 640, .height = 480}));

        const plt::WindowInfo after = composer.window->info();
        STD_INSIST(after.x == before.x);
        STD_INSIST(after.y == before.y);
        STD_INSIST(after.width == before.width);
        STD_INSIST(after.height == before.height);
    }

    STD_TEST(ApplyingASavedFrameRefusesAComposerWithNoWindow) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        STD_INSIST(composer.window == nullptr);

        STD_INSIST(!applyQuickFrameToWindow(composer, {.x = 100, .y = 50, .width = 640, .height = 480}));
    }
#endif

    // R2-qa round 2, B4: one state file, two displays of different
    // scale. A frame saved in backing pixels meant a different window on
    // each of them - measured as a 1000x500-point window coming back
    // 2000x968 on the 1x monitor and 522x262 back on the 2x panel, with
    // the corrupted value persisted in between. Stored in points, the
    // very same file has to describe the very same window on both: the
    // position identical, the size identical once each screen's own
    // scale is undone.
    STD_TEST(ShowRestoresTheSameFrameOnScreensOfDifferentScale) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);

        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder configPath;
        configPath << StringView(dir) << StringView(u8"/config.toml");
        StringBuilder framePath;
        STD_INSIST(defaultQuickFramePath(StringView(configPath), framePath));
        const QuickFrame saved{.x = 300, .y = 200, .width = 640, .height = 480};
        STD_INSIST(saveQuickFrame(StringView(framePath), saved));

        Options options;
        options.quickRememberFrame = true;
        options.configPath = StringView(configPath);
        composer.setOptions(&options);

        // The 1x monitor: 1920x1080 backing pixels is also 1920x1080
        // points, WindowHeadlessImpl's own default.
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        toggleQuickWindow(composer);
        const plt::WindowInfo single = composer.window->info();

        // The 2x panel: 3840x2160 backing pixels is the same 1920x1080
        // points of usable screen.
        composer.window = platform->createWindow(*pool, {});
        composer.installVtHost();
        static_cast<plt::WindowHeadless&>(*composer.window)
            .configure({
                .width = 800,
                .height = 600,
                .screenPixelWidth = 3840,
                .screenPixelHeight = 2160,
                .contentScale = 2.0f,
            });
        toggleQuickWindow(composer);
        const plt::WindowInfo doubled = composer.window->info();

        STD_INSIST(single.x == 300);
        STD_INSIST(single.y == 200);
        STD_INSIST(single.width == 640);
        STD_INSIST(single.height == 480);
        // Same position in points, same size in points - which is twice
        // as many backing pixels on the 2x screen, not half as many.
        STD_INSIST(doubled.x == single.x);
        STD_INSIST(doubled.y == single.y);
        STD_INSIST(doubled.width == single.width * 2);
        STD_INSIST(doubled.height == single.height * 2);

        unlink(framePath.cStr());
        rmdir(dir.cStr());
    }
}
