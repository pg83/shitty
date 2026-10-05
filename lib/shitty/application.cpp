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
 */

#include "application.h"

#include "pty.h"
#include "brand.h"
#include "render.h"
#include "options.h"
#include "session.h"
#include "shell_integration.h"
#include "startup.h"
#include "composer.h"
#include "font_pack.h"
#include "test_mode.h"
#include "test_input.h"
#include "debug_trace.h"
#include "drop_target.h"
#include "input_remap.h"
#include "span_shaper.h"
#include "ui_csd_tabs.h"
#include "configuration.h"
#include "grid_geometry.h"
#include "bookmarks.h"
#include "bookmark_probe.h"
#include "input_bindings.h"
#include "quick_companion.h"
#include "ui_quick_hotkey.h"
#include "ui_palette.h"
#include "ui_sidebar_tabs.h"
#include "quick_frame_store.h"

#include <lib/vterm/num.h>
#include <lib/vterm/fatal.h>
#include <lib/vterm/vterm.h>
#include <lib/vterm/listener.h>

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/alg/defer.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>
#include <std/sys/event_fd.h>

#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <langinfo.h>
#if defined(HAVE_VULKAN_WAYLAND)
    #include "ui_wayland_chrome.h"
#endif

#include <plt/drop.h>
#include <sys/wait.h>
#include <plt/fiber.h>
#include <plt/input.h>
#include <plt/mutex.h>
#include <sys/types.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/poller.h>
#include <plt/platform.h>

using namespace stl;
using namespace plt;

namespace {
    struct ApplicationImpl;

    struct CallFontInc final: public Listener {
        explicit CallFontInc(ApplicationImpl* application);

        void onListen(void*) override;

        ApplicationImpl* application;
    };

    struct CallFontDec final: public Listener {
        explicit CallFontDec(ApplicationImpl* application);

        void onListen(void*) override;

        ApplicationImpl* application;
    };

    struct CallFontReset final: public Listener {
        explicit CallFontReset(ApplicationImpl* application);

        void onListen(void*) override;

        ApplicationImpl* application;
    };

    struct CallContentScaleChanged final: public Listener {
        explicit CallContentScaleChanged(ApplicationImpl* application);

        void onListen(void*) override;

        ApplicationImpl* application;
    };

    struct CallFontChanged final: public Listener {
        explicit CallFontChanged(ApplicationImpl* application);

        void onListen(void*) override;

        ApplicationImpl* application;
    };

    // How often the active terminal refreshes its title from its pty's
    // foreground process.
    constexpr u64 foregroundPollUs = 500'000;

    // The one foreground-name poll for the whole program.
    struct ForegroundTitlePoll final: public plt::TimerCallback {
        explicit ForegroundTitlePoll(ApplicationImpl* application);

        void ready() override;

        ApplicationImpl* application;
    };

    struct CallConfigChanged final: public Listener {
        explicit CallConfigChanged(ApplicationImpl* application);

        void onListen(void*) override;

        ApplicationImpl* application;
    };

    struct ApplicationImpl final: public Application, public plt::WindowEvents, public plt::FrameCallback {
        explicit ApplicationImpl(Composer& composer);
        ~ApplicationImpl();

        int run(int argc, char* argv[]) override;
        void close() override;
        bool frame(const plt::WindowInfo& info) override;

        Composer& composer;
        ForegroundTitlePoll foregroundTitlePoll_{this};
        ObjPool* fontpackPool = nullptr;
        // True until the first frame supplies real metrics; -geometry is
        // applied against them exactly once.
        bool initialGeometryPending = true;
        // Set once in run(), before showWindow() reads it: whether the
        // quick-terminal window actually has a working way to be shown
        // again after it starts hidden. False on every platform without
        // the macOS hotkey module, and on macOS whenever quickHotkey
        // failed to parse or register.
        bool quickHotkeyActive = false;
        // The last window state -verbose reported. updateWindowInfo() runs
        // on every frame, so the trace is a transition and not a state:
        // without these two it would print the same line sixty times a
        // second. The grid half of the old trace moved to Composer::resize()
        // (F4, Q2), which is the only place that knows a grid changed.
        bool tracedFullscreen = false;
        bool tracedMaximized = false;
        // The frame's working storage, kept across frames rather than
        // built on each one: a window drawing sixty times a second must
        // not allocate three times a second per frame to say what it is
        // drawing. clear() keeps the capacity, so after the first frame
        // of a given pane count these never grow again.
        stl::Vector<SessionPane> framePanes;
        // F9: the seams of this frame, in surface pixels. Cleared and
        // refilled per frame like framePanes above, and for the same
        // reason - a field rather than a local so the allocation is made
        // once and not on every frame.
        stl::Vector<PixelRect> frameSeams;
        stl::Vector<const TerminalUpdate*> paneOutputs;
        stl::Vector<PaneUpdate> frameUpdates;
        // R3: frames the backend has refused since the last one it took.
        // A refused frame is asked for again, so a refusal that repeats
        // is a window that has stopped presenting - and the only thing
        // that ever said so was the CPU it burned asking.
        //
        // R3a-4: diagnostics only. Nothing may branch on this counter
        // but reportRefusedFrame() - not the retry, not the expose, not
        // what is handed to the backend. A window that drew one frame
        // differently because of how many were refused before it would
        // be a window whose picture depends on its log.
        u32 refusedFrames = 0;

        int takeTestFd(int& argc, char* argv[]);
        void createRenderer();
        static void childSignalHandler(int signal, siginfo_t* info, void*);
        void setupSignals();
        bool presentTerminal();
        bool collectPaneOutputs();
        const TerminalUpdate* buildFrameUpdates(const Insets& chrome, i32& anchorX, i32& anchorY);
        void reportRefusedFrame() const;
        bool repaintTerminal();
        bool eventLoop();
        void updateWindowInfo(const plt::WindowInfo& info);
        void showWindow();
        void checkLocale();
        void fontInc();
        void fontDec();
        void fontReset();
        void fontChanged();
        void setFontSize(u16 size);
        void contentScaleChanged();
        void configChanged();
        void replaceFontpack(u16 size);
        void publishFontChanged();
        void wire();
    };
}

// Forward declared: ~ApplicationImpl() below calls it, but its
// definition sits with the rest of the quick-companion lifecycle code,
// next to childSignalHandler() and quickCompanionPid further down.
static void stopQuickCompanion();

CallFontInc::CallFontInc(ApplicationImpl* application_)
    : application(application_)
{
}

void CallFontInc::onListen(void*) {
    application->fontInc();
}

CallFontDec::CallFontDec(ApplicationImpl* application_)
    : application(application_)
{
}

void CallFontDec::onListen(void*) {
    application->fontDec();
}

CallFontReset::CallFontReset(ApplicationImpl* application_)
    : application(application_)
{
}

void CallFontReset::onListen(void*) {
    application->fontReset();
}

CallContentScaleChanged::CallContentScaleChanged(ApplicationImpl* application_)
    : application(application_)
{
}

void CallContentScaleChanged::onListen(void*) {
    application->contentScaleChanged();
}

CallFontChanged::CallFontChanged(ApplicationImpl* application_)
    : application(application_)
{
}

void CallFontChanged::onListen(void*) {
    application->fontChanged();
}

ForegroundTitlePoll::ForegroundTitlePoll(ApplicationImpl* application_)
    : application(application_)
{
}

void ForegroundTitlePoll::ready() {
    Composer& composer = application->composer;
    composer.platform->poller()->timeout(foregroundPollUs, *this);
    if (!composer.opts->titleFallbackProcess) {
        return;
    }
    if (composer.sessions == nullptr || composer.sessions->count() == 0) {
        return;
    }
    composer.sessions->activeTerminal()->refreshForegroundName();
}

CallConfigChanged::CallConfigChanged(ApplicationImpl* application_)
    : application(application_)
{
}

void CallConfigChanged::onListen(void*) {
    application->configChanged();
}

ApplicationImpl::ApplicationImpl(Composer& composer_)
    : composer(composer_)
{
}

void ApplicationImpl::wire() {
    composer.fontIncListeners.pushBack(composer.pool->make<CallFontInc>(this));
    composer.fontDecListeners.pushBack(composer.pool->make<CallFontDec>(this));
    composer.fontResetListeners.pushBack(composer.pool->make<CallFontReset>(this));
    composer.contentScaleChangedListeners.pushBack(composer.pool->make<CallContentScaleChanged>(this));
    composer.fontChangedListeners.pushBack(composer.pool->make<CallFontChanged>(this));
    composer.configChangedListeners.pushBack(composer.pool->make<CallConfigChanged>(this));
    composer.inputBindings->add(InputActions::IncFontSize, &composer.fontIncListeners);
    composer.inputBindings->add(InputActions::DecFontSize, &composer.fontDecListeners);
    composer.inputBindings->add(InputActions::ResetFontSize, &composer.fontResetListeners);
}

ApplicationImpl::~ApplicationImpl() {
    // Covers the paths where close() (and its own _exit()) never runs -
    // an exception unwinding out of run() after the companion was
    // spawned, or the SHITTY_FOR_TESTS build, where close() stops the
    // event loop instead of exiting and run() returns normally instead.
    // A no-op when close() already ran: stopQuickCompanion() clears the
    // pid on its way out.
    stopQuickCompanion();
    delete fontpackPool;
}

void ApplicationImpl::publishFontChanged() {
    for (IntrusiveNode* node = composer.fontChangedListeners.mutFront(); node != composer.fontChangedListeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

void ApplicationImpl::replaceFontpack(u16 size) {
    ObjPool* const previousPool = fontpackPool;
    Fontpack* const previousFonts = composer.fonts;
    const u16 previousFontSize = composer.fontSize;
    const u16 previousGlyphWidth = composer.geometry.cellPixelWidth;
    const u16 previousGlyphHeight = composer.geometry.cellPixelHeight;
    ObjPool* const nextPool = ObjPool::fromMemoryRaw();
    Fontpack* next;
    try {
        int scaled = (int)(size * composer.contentScale + 0.5f);
        scaled = scaled < 1 ? 1 : scaled > 255 ? 255 : scaled;
        const u16 pixels = (u16)(scaled);
        next = Fontpack::create(composer, *nextPool, composer.opts->fontnames.data(), composer.opts->fontnames.length(), composer.opts->symbolFonts.data(), composer.opts->symbolFonts.length(), pixels);
    } catch (...) {
        delete nextPool;
        throw;
    }

    fontpackPool = nextPool;
    composer.fontSize = size;
    composer.fonts = next;
    composer.geometry.setCellPixelSize(next->getPx(), next->getPy());
    if (composer.debugFd >= 0) {
        StringBuilder line;
        line << StringView(u8"font size=") << (i64)(size);
        line << StringView(u8" cell ") << (i64)(previousGlyphWidth) << StringView(u8"x") << (i64)(previousGlyphHeight);
        line << StringView(u8" -> ") << (i64)(next->getPx()) << StringView(u8"x") << (i64)(next->getPy());
        debugTraceLine(composer, StringView(line));
    }
    try {
        publishFontChanged();
    } catch (...) {
        fontpackPool = previousPool;
        composer.fontSize = previousFontSize;
        composer.fonts = previousFonts;
        composer.geometry.cellPixelWidth = previousGlyphWidth;
        composer.geometry.cellPixelHeight = previousGlyphHeight;
        delete nextPool;
        throw;
    }
    delete previousPool;
}

void ApplicationImpl::fontChanged() {
    // Until the first frame reports real window metrics the requested
    // geometry wins: the pre-show window is a guess, and the grid derived
    // from it must not displace -geometry. Afterwards font changes keep
    // the grid the user has.
    const bool sized = !initialGeometryPending;
    const u16 columns = sized && composer.geometry.columns != 0 ? composer.geometry.columns : composer.opts->nCols;
    const u16 rows = sized && composer.geometry.rows != 0 ? composer.geometry.rows : composer.opts->nRows;
    const Insets insets = composer.contentInsets();
    // One cell plus the reserve is the smallest window that still shows a
    // terminal; the bare reserve is the base a resize increment counts
    // cells from.
    const GridPixelSize smallest = gridPixelSize(1, 1, insets, composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
    const GridPixelSize reserve = gridPixelSize(0, 0, insets, composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
    composer.window->requestMinimumSize(smallest.width, smallest.height);
    composer.window->requestResizeUnit(composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight, reserve.width, reserve.height);
    const plt::WindowInfo info = composer.window->info();
    if (info.fullscreen || info.maximized || info.tiled) {
        // The window is the screen's, the compositor's tile, or the
        // maximized frame - not ours to resize (issue 38, issue 46: a
        // self-resize under a tiler bounces against the compositor's
        // configure and every font step reflows twice). Let the next
        // frame reflow the grid over the same pixels.
        composer.window->requestFrame();
        return;
    }
    const GridPixelSize target = gridPixelSize(columns, rows, insets, composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
    composer.window->requestResize(target.width, target.height);
}

void ApplicationImpl::setFontSize(u16 size) {
    if (composer.fontSize == size) {
        return;
    }
    try {
        replaceFontpack(size);
    } catch (...) {
    }
}

void ApplicationImpl::fontInc() {
    if (composer.fontSize < 255) {
        setFontSize(composer.fontSize + 1);
    }
}

void ApplicationImpl::fontDec() {
    if (composer.fontSize > 1) {
        setFontSize(composer.fontSize - 1);
    }
}

void ApplicationImpl::fontReset() {
    setFontSize(composer.opts->fontsize);
}

void ApplicationImpl::contentScaleChanged() {
    if (fontpackPool != nullptr) {
        try {
            replaceFontpack(composer.fontSize);
        } catch (...) {
            // The physical border is derived from contentScale independently
            // of the font resource, so its geometry still has to be applied.
            fontChanged();
        }
    }
}

void ApplicationImpl::configChanged() {
    if (fontpackPool == nullptr) {
        return;
    }
    try {
        composer.installFontRenderers();
        replaceFontpack(composer.opts->fontsize);
    } catch (...) {
        // Border and geometry derive directly from the new snapshot even
        // when an external font resource cannot be reopened.
        fontChanged();
    }
}

void ApplicationImpl::createRenderer() {
    // Assigning the fresh pool destroys the previous one — and with it
    // the dead renderer and its listeners.
    composer.rendererPool = ObjPool::fromMemory();
    composer.renderer = Renderer::create(composer, *composer.rendererPool, composer.window->renderContext());
}

int ApplicationImpl::takeTestFd(int& argc, char* argv[]) {
    for (int k = 1; k < argc; ++k) {
        if (StringView(argv[k]) != StringView(u8"--test-fd")) {
            continue;
        }
        if (k + 1 >= argc) {
            raiseError(StringView(u8"--test-fd requires a descriptor"));
        }
        i64 fd = -1;
        if (!parseI64(StringView(argv[k + 1]), fd) || fd < 0 || fd > INT_MAX) {
            raiseError(StringView(u8"invalid --test-fd descriptor"));
        }
        for (int j = k; j + 2 < argc; ++j) {
            argv[j] = argv[j + 2];
        }
        argc -= 2;
        argv[argc] = nullptr;
        return (int)(fd);
    }
    return -1;
}

// The status of the most recently reaped child. The signal handler
// records it; ApplicationImpl::close exits with it once the last session
// is gone, which is the only place that knows the process is ending.
static volatile sig_atomic_t lastChildStatus = 0;

// The quick-terminal companion's pid, once spawned - not a session
// child at all, just a sibling GUI process this one starts and later
// signals. sig_atomic_t rather than pid_t: it has to be the type the
// signal handler below can read and write safely, and every pid_t this
// platform hands out fits in it. -1 means "no companion running";
// childSignalHandler reaps it like any other child (it is still this
// process's child and must not be left a zombie) but must not let its
// exit status overwrite lastChildStatus, which close() below reads to
// propagate the real shell's exit code.
static volatile sig_atomic_t quickCompanionPid = -1;

void ApplicationImpl::childSignalHandler(int signal, siginfo_t*, void*) {
    // SIGCHLD does not queue: one delivery may stand for several exited
    // children (the shell plus xdg-open helpers), so reap until drained.
    if (signal == SIGCHLD) {
        int status = 0;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if ((sig_atomic_t)(pid) == quickCompanionPid) {
                quickCompanionPid = -1;
                continue;
            }
            // Reap only. Which shell dying ends the process is not
            // decidable here: the answer depends on how many sessions are
            // left, and that races with the close this same death is
            // about to trigger through the pty's EOF path. That path owns
            // the decision; the status is recorded for it to exit with.
            lastChildStatus = (sig_atomic_t)(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        }
    }
}

// Kills the companion, if one is running, and forgets its pid so a
// second call (close() then the destructor, on the paths where both
// run) is a harmless no-op. SIGTERM, not SIGHUP: the companion is not a
// pty session's shell, it is a whole second copy of this program, and
// its own default disposition for SIGTERM is termination. There is no
// attempt to wait for it to actually exit - the ordinary SIGCHLD path
// above reaps it - and no respawn if it already died on its own: this
// runs once, at this process's own exit, and does not try to keep the
// companion alive.
//
// Known gap, not fixed here: a SIGKILL against this process skips this
// entirely, same as it skips every other destructor and cleanup path in
// the program. The companion is then left running until it, or its own
// shell, exits on its own.
static void stopQuickCompanion() {
    if (quickCompanionPid <= 0) {
        return;
    }
    kill((pid_t)(quickCompanionPid), SIGTERM);
    quickCompanionPid = -1;
}

void ApplicationImpl::setupSignals() {
    struct sigaction childAction{};
    childAction.sa_sigaction = childSignalHandler;
    childAction.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&childAction.sa_mask);
    if (sigaction(SIGCHLD, &childAction, nullptr) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"can't install SIGCHLD handler: sigaction()"));
    }

    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    if (sigaction(SIGINT, &defaultAction, nullptr) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"can't reset SIGINT handler: sigaction()"));
    }
    if (sigaction(SIGQUIT, &defaultAction, nullptr) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"can't reset SIGQUIT handler: sigaction()"));
    }
}

bool ApplicationImpl::repaintTerminal() {
    const bool repainted = composer.renderer->repaint();
    if (!repainted) {
        composer.window->requestFrame();
    }
    return repainted;
}

// R7-2: a pane with a frame is asked through output() and owes
// consume(); a pane with nothing to say hands over its retained form,
// which neither captures damage nor arms consume(). Within one walk a
// pane is asked exactly one of the two, never both: the two forms share
// the terminal's row buffer and its preedit window.
//
// R3a-5: one walk, not one frame. A refused frame is collected a second
// time, and a pane that handed over its retained form the first time
// speaks the second - so across the two walks of a refused frame the
// same pane can be asked both ways, into the same shared buffer. That
// is safe because the first assembly is thrown away whole before the
// second touches anything: buildFrameUpdates() clears frameUpdates,
// the anchor is reassigned from the second walk's return, and nothing
// reads the stale retained form in between. What is per-walk is the
// consume() bookkeeping, and that is why the walk is a step of its own.
//
// Answers whether any pane had a frame of its own.
bool ApplicationImpl::collectPaneOutputs() {
    paneOutputs.clear();
    bool spoke = false;
    for (const SessionPane& pane : framePanes) {
        const TerminalUpdate* const output = pane.terminal->output();
        paneOutputs.pushBack(output);
        spoke = spoke || output != nullptr;
    }
    return spoke;
}

// Where each pane says what it has to say, on the surface. Answers the
// focused pane's update - the one the input method anchors to - or null
// when no visible pane holds the focus.
const TerminalUpdate* ApplicationImpl::buildFrameUpdates(const Insets& chrome, i32& anchorX, i32& anchorY) {
    frameUpdates.clear();
    const TerminalUpdate* anchored = nullptr;
    anchorX = 0;
    anchorY = 0;
    for (size_t at = 0; at < framePanes.length(); ++at) {
        const SessionPane& pane = framePanes[at];
        const TerminalUpdate& update = paneOutputs[at] != nullptr ? *paneOutputs[at] : pane.terminal->retainedOutput();
        const PixelRect area{
            (u16)(chrome.left + pane.area.x),
            (u16)(chrome.top + pane.area.y),
            pane.area.width,
            pane.area.height,
        };
        frameUpdates.pushBack(PaneUpdate{area, update});
        if (pane.focused) {
            anchored = &update;
            anchorX = pane.area.x;
            anchorY = pane.area.y;
        }
    }
    return anchored;
}

// R3: a refusal a full expose did not cure. Handing over every pane
// whole is the strongest answer this side of the backend, so a frame
// refused after it is refused for a reason the window cannot mend - a
// grid of zero, say - and the window will ask for that same frame for
// as long as it lives. That was the entire symptom the first time:
// percent of a CPU and a picture that had stopped, with nothing
// anywhere to say which.
//
// Grids and counts only. The frame carries what the user is reading,
// and none of it belongs in a log.
void ApplicationImpl::reportRefusedFrame() const {
    // Three, because one is not yet news: the first refusal is answered
    // inside the frame it happened in, and a surface that is not ready
    // yet - a resize, a rebuilt renderer - legitimately refuses a frame
    // or two while it settles. Past that the window is stalled, and at
    // sixty frames a second three of them is fifty milliseconds. Said
    // again every ten seconds or so, because a stall that outlives the
    // scrollback is still worth one line.
    static constexpr u32 stalledFrames = 3;
    static constexpr u32 repeatEvery = 600;
    // Z1: and a last one, because the repeat had no end. A window
    // stalled on something it cannot mend lives on, asking, and every
    // report after the first few says the same thing about the same
    // frame - so six of them, about a minute of stall, and then quiet.
    // Counted off refusedFrames rather than kept in a second field, so
    // that the count and the silence end together: the frame the
    // backend finally takes clears the counter, and a window that
    // stalls again is news again.
    //
    // Frames rather than a clock, deliberately. The stride is only ever
    // approximate - the platform decides how often a refused frame is
    // asked for again - and what has to be bounded is the number of
    // lines, which a clock does not bound on its own.
    static constexpr u32 reportLimit = 6;
    if (refusedFrames < stalledFrames) {
        return;
    }
    const u32 sinceStall = refusedFrames - stalledFrames;
    if (sinceStall % repeatEvery != 0) {
        return;
    }
    const u32 report = sinceStall / repeatEvery;
    if (report >= reportLimit) {
        return;
    }
    const char* const brand = composer.brand->identifierCString();
    fprintf(stderr, "%s: renderer refused %u frames in a row, a full expose included; %zu pane(s) offered:\n", brand, refusedFrames, frameUpdates.length());
    for (size_t at = 0; at < frameUpdates.length(); ++at) {
        const TerminalUpdate& update = frameUpdates[at].update;
        // The refusals a frame can be read for from here, in the order
        // the backends test them. Anything else is the backend's own
        // (a lost surface, a row whose cells went missing), and saying
        // so is more use than guessing.
        const char* reason = "no reason this side of the backend";
        if (update.colors == nullptr) {
            reason = "no colors";
        } else if (update.gridColumns == 0 || update.gridRows == 0) {
            reason = "empty grid";
        } else if (update.rowCount != update.gridRows) {
            reason = "owes the reshaped frame every row and has not got them";
        }
        fprintf(stderr, "%s:   pane %zu: grid %ux%u, %zu row(s) given: %s\n", brand, at, update.gridColumns, update.gridRows, update.rowCount, reason);
    }
    // Z1: said, so that a reader who finds the last of these does not
    // take the silence after it for a window that recovered.
    if (report + 1 == reportLimit) {
        fprintf(stderr, "%s: no more will be said about this stall unless a frame is taken\n", brand);
    }
}

bool ApplicationImpl::presentTerminal() {
    if (composer.renderer == nullptr) {
        return false;
    }
    // composer.sessions, not the member: under test the session set is
    // the harness's, published there before the first frame can land.
    //
    // A2/A6-4: the frame is every visible pane, in the order the layout
    // hands them out. That order is part of the key both backends retain
    // a pane's cells under, so it has to be the same order every frame -
    // which is exactly what this list is, since it comes from one walk
    // of one tree.
    framePanes.clear();
    composer.sessions->visiblePanes(framePanes);
    if (framePanes.empty()) {
        // A frame between the last pane's death and the window's own.
        // There is nothing to lay out, and what the backend still holds
        // is the right picture until the window goes.
        return repaintTerminal();
    }

    // The outputs are collected before any retained form is taken,
    // because a window where nobody spoke is a window that wants a
    // repaint rather than a frame of nothing but retained forms - and
    // asking a terminal for its retained form is not free.
    if (!collectPaneOutputs()) {
        return repaintTerminal();
    }

    // A10, the window -> content box step: the rectangles SessionSet
    // hands out are counted inside the content box, and a frame's are
    // counted on the surface. This is the one place the two are bridged,
    // so no backend and no pane has to know that chrome reserved a side.
    const Insets chrome = composer.chromeInsets();
    i32 anchorX = 0;
    i32 anchorY = 0;
    const TerminalUpdate* anchored = buildFrameUpdates(chrome, anchorX, anchorY);

    // F9: the seams, bridged onto the surface by the same insets the
    // panes just were. SessionSet answers in content-box coordinates and
    // has already clamped each band to the air between two panes, so
    // there is nothing left to decide here beyond where the content box
    // sits on the surface.
    frameSeams.clear();
    composer.sessions->visibleSeams(frameSeams);
    for (size_t at = 0; at < frameSeams.length(); ++at) {
        PixelRect& seam = frameSeams.mut(at);
        seam.x = (u16)(chrome.left + seam.x);
        seam.y = (u16)(chrome.top + seam.y);
    }
    composer.renderer->setSeams(frameSeams.data(), frameSeams.length(), composer.opts->paneDividerColor);

    bool presented = composer.renderer->update(frameUpdates.data(), frameUpdates.length());
    if (!presented) {
        // R1: a refusal says "your frame is incomplete", and the frame
        // that comes back unchanged is incomplete in the same way. The
        // only answer that can make the next one differ is to hand over
        // every pane whole - every pane, because the one the backend is
        // owed rows by is the one that had nothing to say, and that is
        // never the pane that just changed.
        //
        // A frame is reshaped whenever any pane's Screen changes
        // identity - which is what \e[?1049h does - and a reshaped frame
        // owes all rows of all panes (render_reference.cpp, and
        // render_metal.mm word for word). A pane that was not written to
        // hands over its retained form, and that carries no damage by
        // construction (vterm.cpp, retainedOutput), so it cannot pay.
        //
        // Retried here rather than left to the next frame, so the window
        // presents on the callback it was given instead of skipping one.
        //
        // R3a-2: not because a refusal leaves the retain untouched - the
        // reference backend refuses before it touches anything, but
        // Metal also refuses later, inside the loop that materialises
        // the rows, by which point it has already cleared its pane list
        // and rewritten part of its cells. The second assembly is safe
        // because it repairs that: a truncated pane list makes the next
        // frame a reshaped one, which demands every row of every pane -
        // exactly what exposeAll() has just paid for - and rewrites the
        // cells in full. The retry does not avoid the damage, it is the
        // thing that undoes it.
        for (const SessionPane& pane : framePanes) {
            pane.terminal->exposeAll();
        }
        collectPaneOutputs();
        anchored = buildFrameUpdates(chrome, anchorX, anchorY);
        presented = composer.renderer->update(frameUpdates.data(), frameUpdates.length());
    }
    if (!presented) {
        ++refusedFrames;
        reportRefusedFrame();
        composer.window->requestFrame();
        return false;
    }
    refusedFrames = 0;
    // Keep the input-method candidate window anchored to the cursor cell
    // of the pane being typed into - the focused one, which is not the
    // last one in the list. Its origin is added to the window's insets
    // the same way a pointer mapping adds it (mouse_frontend.h): the
    // insets are the window's, the origin is one pane's inside them.
    if (anchored != nullptr) {
        const CellOrigin anchor = cellOrigin(anchored->cursor.posX, anchored->cursor.posY, composer.contentInsets(), composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
        composer.window->requestTextInputRect(anchor.x + anchorX, anchor.y + anchorY, composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
    }
    // Only the panes that were asked through output(): consume() asserts
    // on a pane that handed over its retained form.
    for (size_t at = 0; at < framePanes.length(); ++at) {
        if (paneOutputs[at] != nullptr) {
            framePanes[at].terminal->consume();
        }
    }
    return true;
}

void ApplicationImpl::close() {
    stopQuickCompanion();
#if defined(SHITTY_FOR_TESTS)
    composer.platform->stop();
#else
    // Exit with the shell's status only when a dying shell is what ended
    // the window: liveSessions reaches zero only through close(). Closing
    // the window yourself, with sessions still live, is not a shell's
    // failure and must not borrow the status of one that ended earlier.
    _exit(SessionSet::liveSessions == 0 ? (int)(lastChildStatus) : 0);
#endif
}

void ApplicationImpl::updateWindowInfo(const plt::WindowInfo& info) {
    if (isfinite(info.contentScale) && info.contentScale > 0.0f) {
        composer.setContentScale(info.contentScale);
    }
    // The debug trace wants the grid on both sides of the resize, and
    // the reserve Composer::resize() applies is part of what it counts.
    const u16 previousColumns = composer.geometry.columns;
    const u16 previousRows = composer.geometry.rows;
    composer.resize((u16)(min(info.width, (u32)(UINT16_MAX))), (u16)(min(info.height, (u32)(UINT16_MAX))));
    if (composer.debugFd >= 0 && (composer.geometry.columns != previousColumns || composer.geometry.rows != previousRows)) {
        StringBuilder line;
        line << StringView(u8"window ") << (i64)(info.width) << StringView(u8"x") << (i64)(info.height);
        line << StringView(u8" scale=") << (i64)((int)(info.contentScale * 100));
        line << StringView(u8" fullscreen=") << (i64)(info.fullscreen) << StringView(u8" maximized=") << (i64)(info.maximized) << StringView(u8" tiled=") << (i64)(info.tiled);
        line << StringView(u8" grid ") << (i64)(previousColumns) << StringView(u8"x") << (i64)(previousRows);
        line << StringView(u8" -> ") << (i64)(composer.geometry.columns) << StringView(u8"x") << (i64)(composer.geometry.rows);
        debugTraceLine(composer, StringView(line));
    }
    // The grid line is Composer::resize()'s to print: a reserve set by
    // cmd+b or by a reload re-counts the grid without any platform
    // callback, and the trace that lived here missed every one of them
    // (F4, Q2). What only this callback knows is the window state the
    // full-screen transition bugs live in, so that is what stays.
    if (composer.vtConfig.config->verbose && (info.fullscreen != tracedFullscreen || info.maximized != tracedMaximized)) {
        fprintf(stderr, "%s: window: %ux%u px, %s%s\n", composer.brand->identifierCString(), info.width, info.height, info.fullscreen ? "fullscreen" : "windowed", info.maximized ? ", maximized" : "");
    }
    tracedFullscreen = info.fullscreen;
    tracedMaximized = info.maximized;
    if (initialGeometryPending) {
        // The first real metrics (glyphs at the live content scale) size
        // the window to the requested geometry exactly once.
        fontChanged();
        initialGeometryPending = false;
    }
}

bool ApplicationImpl::frame(const plt::WindowInfo& info) {
    updateWindowInfo(info);
    if (composer.sessions == nullptr) {
        // Still starting up: entering fullscreen on macOS displays the layer,
        // and so lands here, before the renderer and sessions exist (issue
        // 116). The first frame after startup paints.
        return false;
    }
    if (composer.renderer == nullptr) {
        // The previous renderer died with its surface and dropped its own
        // pool; build a fresh one and repaint everything.
        //
        // Everything is every visible pane, whole: a renderer with no
        // panes retained yet reads its first frame as a reshaped one and
        // owes all rows of all of them. The active terminal alone left
        // the window's other panes owing rows they had not got, which is
        // the stall presentTerminal() answers one path over - reached
        // here by losing a surface rather than by changing a screen.
        createRenderer();
        framePanes.clear();
        composer.sessions->visiblePanes(framePanes);
        for (const SessionPane& pane : framePanes) {
            pane.terminal->exposeAll();
        }
    }
    return presentTerminal();
}

bool ApplicationImpl::eventLoop() {
    composer.platform->run();
    return true;
}

void ApplicationImpl::showWindow() {
    const Insets insets = composer.contentInsets();
    const u32 width = gridPixelWidth(composer.opts->nCols, insets, composer.geometry.cellPixelWidth);
    const u32 height = gridPixelHeight(composer.opts->nRows, insets, composer.geometry.cellPixelHeight);
    if (!composer.opts->quick || !quickHotkeyActive) {
        // A quick-terminal window with a working hotkey starts hidden;
        // nothing else shows it until the hotkey fires and toggles it via
        // requestShowAt(). Without one - unparsable or unregistered
        // quickHotkey, or simply no hotkey module outside macOS -
        // quickHotkeyActive stays false and the window shows normally
        // instead, so the shell behind it is never permanently
        // unreachable; run() already sent a diagnostic to stderr for that
        // case. The grid still needs its initial size below either way.
        composer.window->requestShow();
    }
    composer.resize((u16)(min(width, (u32)(UINT16_MAX))), (u16)(min(height, (u32)(UINT16_MAX))));
}

namespace {
    // A6: a saved frame wins over quickGeometry once one exists;
    // deleting the state file (T2's documented reset) falls back to
    // quickGeometry, unchanged from before this option existed -
    // loadQuickFrame() already treats a missing or corrupt file as
    // "nothing saved", so this only ever does anything when there
    // really is something to apply.
    void applySavedQuickFrame(Composer& composer) {
        if (!composer.opts->quickRememberFrame) {
            return;
        }
        StringBuilder path;
        if (!defaultQuickFramePath(composer.opts->configPath, path)) {
            return;
        }
        QuickFrame frame;
        if (!loadQuickFrame(StringView(path), frame)) {
            return;
        }
#if defined(__APPLE__)
        // Guarded the same way createQuickHotkey()/createCsdTabsUi() are
        // below: applyQuickFrameToWindow() is defined in the darwin-only
        // ui_quick_hotkey.mm (build.py:660), and calling it from here
        // unconditionally left every non-Apple build with an unresolved
        // symbol, reached through toggleQuickWindow() (R2-test, L1). The
        // portable fallback underneath is what those platforms run, and
        // it was always meant to be the only thing they run.
        if (applyQuickFrameToWindow(composer, frame)) {
            // The Cocoa path (ui_quick_hotkey.mm): one atomic setFrame:,
            // sidesteps the requestMove()/requestResize() ordering
            // hazard below entirely (F2's report, B2) by never using
            // those two separate calls in the first place.
            //
            // requestShowAt() has already sized the grid to the screen it
            // put the window on - the one under the pointer - and the
            // frame just applied may have moved the window back to a
            // different one. When the two screens' content scales differ,
            // the scale change that follows regrids the font and resizes
            // the window to the grid of the screen it no longer is on,
            // pulling it off the frame just restored: measured on a 1x
            // monitor plus a 2x panel, a restored 1000x500 came back
            // 980x490 without this. Re-deriving the grid from the window
            // as it now is - still expressed in the scale the composer
            // currently carries, since the scale change has not been
            // delivered yet - makes that resize reproduce the restored
            // frame instead of replacing it.
            const plt::WindowInfo restored = composer.window->info();
            composer.resize((u16)(quickFrameRegridExtent(restored.width, restored.contentScale, composer.contentScale)), (u16)(quickFrameRegridExtent(restored.height, restored.contentScale, composer.contentScale)));
            return;
        }
#endif
        // Fallback for a backend with no concrete NSWindow to reach
        // through Window::renderContext() (headless today, and any
        // future non-Cocoa backend): the portable Window interface only
        // offers requestMove()/requestResize() separately. It runs the
        // same quickFrameTarget() clamp the Cocoa path does - one
        // implementation, so the tests below stand on the real thing
        // (R2-qa round 2, I8) - but not the atomicity fix (B2), which is
        // acceptable because requestResize() is only asynchronous by
        // construction on Cocoa (see its own comment in
        // platform_cocoa.mm, guarding against re-entering frame());
        // headless applies both synchronously and has no such hazard.
        //
        // WindowInfo describes the screen by its pixel size alone, with
        // no origin and no chrome to ask about: the visible area is the
        // whole screen in points, starting at zero, and the titlebar
        // height is zero, which makes the target frame the content frame.
        const plt::WindowInfo info = composer.window->info();
        const double scale = info.contentScale > 0.0f ? (double)(info.contentScale) : 1.0;
        const QuickFrameRect visible{
            .x = 0,
            .y = 0,
            .width = (double)(max(1u, info.screenPixelWidth)) / scale,
            .height = (double)(max(1u, info.screenPixelHeight)) / scale,
        };
        const QuickFrameRect target = quickFrameTarget(frame, visible, 0);
        composer.window->requestMove((i32)(target.x), (i32)(target.y));
        composer.window->requestResize((u32)(target.width * scale), (u32)(target.height * scale));
    }
}

// The one entry point ui_quick_hotkey's Carbon handler calls on every
// press; a free function rather than a method because it is declared in
// ui_quick_hotkey.h, which the hotkey module includes without pulling in
// ApplicationImpl. Asks composer.window for its actual state instead of
// keeping a flag here: hide-on-resign-key (platform_cocoa.mm) can hide
// the window from underneath this without going through this function
// at all, and a flag of our own would then disagree with reality on the
// very next press.
//
// visible() alone is not enough: on Cocoa, a miniaturized window still
// answers isVisible with true, so a naive toggle would try to hide an
// already-Dock-hidden window instead of bringing it back. info().iconified
// catches that case and routes it through the show branch instead.
void toggleQuickWindow(Composer& composer) {
    if (composer.window == nullptr) {
        return;
    }
    const bool showing = composer.window->visible() && !composer.window->info().iconified;
    if (showing) {
        composer.window->requestHide();
    } else {
        composer.window->requestShowAt(plt::ShowPlacement::TopOfActiveScreen);
        applySavedQuickFrame(composer);
    }
}

#if defined(SHITTY_FOR_TESTS)
namespace {
    // R2-qa round 2, I10: a way to drive the quick window in a live
    // process without a global hotkey. Two rounds of acceptance testing
    // failed to deliver a single synthetic Carbon chord into a test
    // binary (0 of 22 presses with every permission granted), and the
    // one thing that did work - a marker file plus SIGUSR1 - meant
    // patching configuration.cpp by hand for every measurement. This is
    // that patch, made permanent and kept out of the shipped terminal:
    // SHITTY_FOR_TESTS is the same build flag takeTestFd() lives under,
    // so only st_test carries it.
    //
    // SIGUSR2 because SIGUSR1 is already the config reload
    // (configuration.cpp). The signal handler only bumps an eventfd the
    // poller watches, the same shape ConfigImpl uses, so the toggle
    // itself runs on the main loop - AppKit is not async-signal-safe and
    // toggleQuickWindow() reaches straight into it.
    //
    //   .build/st_test -config /tmp/q.toml &
    //   kill -USR2 $!            # show; again to hide
    EventFD* quickToggleEvent = nullptr;

    struct QuickToggleSignal final: public PollCallback {
        explicit QuickToggleSignal(Composer& composer_)
            : composer(composer_)
        {
        }

        ~QuickToggleSignal() {
            // The SIGUSR2 handler writes through quickToggleEvent, which
            // points at this object's own EventFD. Leaving it set once the
            // pool that owns this is gone left one signal between the
            // process and a write to freed memory (R2-test, Z4). Only
            // reachable in st_test, and only after run() has returned -
            // but the handler stays installed until the process exits, so
            // "unreachable in practice" is not the same as safe.
            quickToggleEvent = nullptr;
        }

        void ready(PollFD) override {
            event.drain();
            composer.platform->poller()->arm(waiter);
            toggleQuickWindow(composer);
        }

        Composer& composer;
        EventFD event;
        PollWaiter waiter;
    };

    void quickToggleSignalHandler(int) {
        if (quickToggleEvent != nullptr) {
            quickToggleEvent->signal();
        }
    }

    // Best-effort and never fatal: a test-only convenience must not be
    // able to stop the terminal from starting.
    void installQuickToggleSignal(Composer& composer) {
        QuickToggleSignal* const toggle = composer.pool->make<QuickToggleSignal>(composer);
        toggle->waiter.fd = {
            .fd = toggle->event.fd(),
            .flags = PollFlag::In,
        };
        toggle->waiter.callback = toggle;
        composer.platform->poller()->arm(toggle->waiter);
        quickToggleEvent = &toggle->event;
        struct sigaction action{};
        action.sa_handler = quickToggleSignalHandler;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR2, &action, nullptr) < 0) {
            quickToggleEvent = nullptr;
            composer.platform->poller()->cancel(toggle->waiter);
        }
    }
}
#endif

void ApplicationImpl::checkLocale() {
    const char* locale = setlocale(LC_ALL, "");
    if (locale != nullptr && StringView(nl_langinfo(CODESET)) == StringView(u8"UTF-8")) {
        return;
    }
    // A terminal launched outside a login context - Automator, launchd,
    // a .app bundle - inherits no locale at all and lands in plain "C",
    // which turns every non-ASCII listing in the child shell into
    // question marks (issue 63). Unless the user pinned LC_ALL
    // explicitly, force a UTF-8 character type and export it so the
    // shell inherits it; the other locale categories stay untouched.
    const char* pinned = getenv("LC_ALL");
    if (pinned == nullptr || pinned[0] == '\0') {
        static const char* const candidates[] = {"C.UTF-8", "UTF-8", "en_US.UTF-8"};
        for (const char* candidate : candidates) {
            if (setlocale(LC_CTYPE, candidate) == nullptr) {
                continue;
            }
            if (StringView(nl_langinfo(CODESET)) == StringView(u8"UTF-8")) {
                setenv("LC_CTYPE", candidate, 1);
                return;
            }
        }
    }
    if (locale == nullptr) {
        sysO << StringView(u8"Warning: could not set locale; international input may be broken.") << endL;
        return;
    }
    sysO << StringView(u8"Warning: non-UTF-8 locale ") << StringView(locale) << StringView(u8"; international input may be broken.") << endL;
}

// The one place -backgroundBlur's three modes cross from this library
// into the platform layer. Two enums rather than one shared type on
// purpose: Options owns the user's vocabulary (it is what the config
// spells, what the help text describes and what the test-mode dump
// prints back), plt owns the backend's, and the pair meets here - the
// same shape every other WindowOptions field already has, and the
// reason this bridge, rather than platform_cocoa.mm, is where the
// option's type change stopped in wave 1.
static plt::Backdrop platformBackdrop(BackdropMode mode) {
    switch (mode) {
        case BackdropMode::Off:
            return plt::Backdrop::None;
        case BackdropMode::Blur:
            return plt::Backdrop::Blur;
        case BackdropMode::Glass:
            return plt::Backdrop::Glass;
    }
    return plt::Backdrop::None;
}

int ApplicationImpl::run(int argc, char* argv[]) {
    // Captured before anything below can touch argv: takeTestFd() and
    // Config::initialize() may both shift later entries out from under
    // consumed flags, but neither ever moves argv[0] itself. This is
    // what a spawned quick-terminal companion re-execs.
    const char* const argv0 = argv[0];
    int testFd = -1;
#ifdef SHITTY_FOR_TESTS
    testFd = takeTestFd(argc, argv);
#endif
    checkLocale();
    // After the locale: option parsing resolves the auto width level by
    // probing the libc's wcwidth.
    // Before the first option parse, while the launching parent is still
    // alive; every later consumer reads the committed field.
    composer.desktopLaunch = launchedFromDesktop(getppid());
    composer.config = Config::create(composer);
    composer.config->initialize(&argc, argv);
    composer.installFontRenderers();
    // In the parent, before any thread exists. TERM and the version are
    // process-wide constants identical for every terminal behind the
    // window, and setenv() must never run in a forked child of a
    // multithreaded process: glibc's environ lock is not reset at fork.
    configureTerminalChildEnvironment(*composer.brand, composer.vtConfig.config->widths);
    enterHomeWhenLaunchedFromDesktop(composer.desktopLaunch);
    composer.fontSize = composer.opts->fontsize;
    composer.inputRemap = InputRemap::create(composer);
    if (testFd >= 0) {
        return runTestMode(composer, *TestInput::create(composer), *this, *this, testFd, argc, argv);
    }

    LaunchCommand launch = buildLaunchCommand(argc, argv, composer.opts->shell, composer.opts->login);
    {
        // Read once, here: the rule is about the directory this process
        // was launched in, and nothing in it ever changes that.
        char inherited[PATH_MAX];
        Buffer home;
        homeDirectory(home);
        launchDirectory(composer.opts->directory, StringView(getcwd(inherited, sizeof(inherited)) != nullptr ? inherited : ""), StringView(home), launch.directory);
    }
    composer.launch = composer.pool->make<LaunchCommand>(static_cast<LaunchCommand&&>(launch));
    if (argc > 2 && StringView(argv[1]) == StringView(u8"-e")) {
        // -e ran a command in place of the shell; bookmarks still want
        // the shell. Resolved here, with the launch command, while the
        // process has no threads for its setenv(SHELL) to race.
        char* none[] = {nullptr};
        composer.shellLaunch = composer.pool->make<LaunchCommand>(buildLaunchCommand(1, none, composer.opts->shell, composer.opts->login));
    } else {
        composer.shellLaunch = composer.launch;
    }
    if (composer.opts->shellIntegration && shellIsZsh(StringView(composer.shellLaunch->executable()))) {
        // Still before any thread: this sets ZDOTDIR for every shell the
        // window starts, bookmarks' included.
        StringBuilder directory;
        shellIntegrationDirectory(composer.brand->identifier(), directory);
        const bool installed = installShellIntegration(StringView(directory));
        if (composer.vtConfig.config->verbose) {
            fprintf(stderr, "%s: shell integration: %s %s\n", composer.brand->identifierCString(), installed ? "zsh reads" : "cannot write", Buffer(StringView(directory)).cStr());
        }
    }
    {
        // Bookmarks come from -bookmarksFile, ~ expanded like -directory,
        // else bookmarks.toml beside the config. A missing file is no
        // bookmarks, silently: most users never write one.
        BookmarkShelf* const shelf = composer.pool->make<BookmarkShelf>();
        StringBuilder path;
        if (!composer.opts->bookmarksFile.empty()) {
            Buffer home;
            homeDirectory(home);
            Buffer expanded;
            launchDirectory(composer.opts->bookmarksFile, StringView(), StringView(home), expanded);
            path << StringView(expanded);
        } else {
            defaultBookmarksPath(composer.opts->configPath, path);
        }
        shelf->path = composer.pool->intern(StringView(path));
        // The same read as every later one, folder tables included: a
        // folder saved with no bookmark in it is there from the start.
        reloadBookmarks(*shelf, *composer.pool, composer.brand->identifier());
        composer.bookmarks = shelf;
    }
    if (composer.platform == nullptr) {
        composer.platform = plt::Platform::create(*composer.pool);
    }
    // After the platform: the probe's thread wakes this loop through it.
    // Only where a sidebar lists the bookmarks - nothing else says what
    // it finds.
    composer.bookmarkProbe = BookmarkProbe::create(*composer.pool, composer);
    if (composer.opts->sidebarTabs) {
        composer.bookmarkProbe->watch(*composer.bookmarks);
    }
    // Input deliveries run on one fiber, so stream-backed handlers may
    // suspend without stopping the event loop; later input waits in the
    // sink's queue.
    composer.input = plt::createFiberInputSink(*composer.pool, *composer.platform->scheduler(), *composer.input);
    composer.window = composer.platform->createWindow(
        *composer.pool,
        {
            .appId = composer.brand->identifier(),
            .title = composer.vtConfig.config->title,
            .width = (u32)(max(320, (int)(composer.opts->nCols) * composer.opts->fontsize / 2)),
            .height = (u32)(max(200, (int)(composer.opts->nRows) * composer.opts->fontsize)),
            .decorations = !composer.opts->noDecorations,
            // The window drawn by this program on Wayland: the tab list is
            // its sidebar, and it is layered like the Mac's.
            .clientChrome = composer.opts->sidebarTabs && composer.opts->layeredWindow,
            .transparentTitlebar = composer.opts->transparentTitlebar,
            .quick = composer.opts->quick,
            .quickGeometry = composer.opts->quickGeometry,
            .quickCornerRadius = composer.opts->quickCornerRadius,
            .backgroundOpacity = composer.opts->backgroundOpacity,
            .backdrop = platformBackdrop(composer.opts->backgroundBlur),
            .input = composer.input,
            .events = this,
            .frame = this,
            .drop = createDropTarget(*composer.pool, composer),
            .icon = composer.brand->iconData(),
            .appName = composer.brand->displayName(),
        }
    );
    composer.installVtHost();
    openDebugTrace(composer);
#if defined(__APPLE__)
    // The title-bar tab strip: a fire-and-forget listener over the
    // NSWindow the render context carries.
    createCsdTabsUi(*composer.pool, composer);
    // The sidebar tab list, the same way, and inside the same guard for
    // the same reason: ui_sidebar_tabs.mm is in the darwin sources only
    // (build.py), and calling into it unconditionally is exactly what
    // left every non-Apple build with an unresolved symbol last time
    // (R2-test, L1). It is created whether or not -sidebarTabs is on -
    // the object reserves nothing and draws nothing while the option is
    // off, and a config reload can turn it on without a restart.
    createSidebarTabsUi(*composer.pool, composer);
    // The command palette on Cmd+K, the same way (ui_palette.mm).
    createPaletteUi(*composer.pool, composer);
    if (composer.opts->quick) {
        // The global hotkey that shows and hides the quick-terminal
        // window; wired up only when the window is actually behaving as
        // one, so a plain terminal never claims the chord from the rest
        // of the system.
        quickHotkeyActive = createQuickHotkey(*composer.pool, composer);
    }
#endif
#if defined(HAVE_VULKAN_WAYLAND)
    createWaylandChrome(*composer.pool, composer);
#endif
    if (composer.opts->quick && !quickHotkeyActive) {
        // No working hotkey - either this build has no hotkey module at
        // all (every non-Apple platform today), or createQuickHotkey()
        // already printed why the chord itself didn't take. Either way
        // the window cannot stay hidden forever with nothing able to
        // bring it back: showWindow() reads quickHotkeyActive and shows
        // it normally below.
        sysE << composer.brand->identifier() << StringView(u8": quick: no working hotkey to bring the window back with; showing it normally instead") << endL;
    }
    composer.config->start();
    STD_DEFER {
        composer.config->stop();
    };
    contentScaleChanged();

    replaceFontpack(composer.opts->fontsize);
    composer.shaper = SpanShaper::create(composer, *composer.pool);
    applyStartupWindowState(composer);
    showWindow();

    setupSignals();
#if defined(SHITTY_FOR_TESTS)
    // After setupSignals() for the same reason the companion spawn below
    // is: this installs a disposition of its own and must not be undone
    // by the handler setup that follows it.
    installQuickToggleSignal(composer);
#endif
    // After setupSignals(): the custom SIGCHLD handler has to be in
    // place before the companion can die, or its exit would be reaped
    // silently under the still-default disposition and never clear
    // quickCompanionPid above. Every guard - the option unset, this
    // being a quick window itself, a self-referential path, a fork/exec
    // failure - lives in spawnQuickCompanion() and leaves this process
    // running either way; only a real pid is ever stored.
    quickCompanionPid = (sig_atomic_t)(spawnQuickCompanion(composer.opts->quickCompanion, composer.opts->configPath, composer.opts->quick, argv0, composer.brand->identifier()));
    composer.pty = createPty(*composer.pool, *composer.platform->scheduler(), composer.platform, composer.brand->identifierCString());

    createRenderer();
    SessionSet::create(composer);
    composer.platform->poller()->timeout(foregroundPollUs, foregroundTitlePoll_);

    eventLoop();
    composer.platform->poller()->cancel(foregroundTitlePoll_);
    // The swapchain holds proxies of the platform display. The composer and
    // its rendererPool outlive the platform in the main pool, so destroying
    // the renderer there would touch Wayland objects after the display is
    // disconnected; drop it while the connection is still alive.
    composer.renderer = nullptr;
    composer.rendererPool = ObjPool::fromMemory();
    return 0;
}

Application* Application::create(Composer& composer) {
    ApplicationImpl* const application = composer.pool->make<ApplicationImpl>(composer);
    application->wire();
    return application;
}

void applyStartupWindowState(Composer& composer) {
    if (composer.opts->fullscreen) {
        composer.window->requestFullscreen(true);
    } else if (composer.opts->maximized) {
        composer.window->requestMaximized(true);
    }
}
