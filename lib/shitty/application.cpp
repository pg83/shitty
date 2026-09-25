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
#include "input_bindings.h"

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

#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <langinfo.h>
#include <plt/drop.h>
#include <sys/wait.h>
#include <plt/fiber.h>
#include <plt/input.h>
#include <plt/mutex.h>
#include <sys/types.h>
#include <plt/poller.h>
#include <plt/window.h>
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

        int takeTestFd(int& argc, char* argv[]);
        void createRenderer();
        static void childSignalHandler(int signal, siginfo_t* info, void*);
        void setupSignals();
        bool presentTerminal();
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
    const u32 border = 2u * composer.layout.borderPixels;
    composer.window->requestMinimumSize(border + composer.geometry.cellPixelWidth, border + composer.geometry.cellPixelHeight);
    composer.window->requestResizeUnit(composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight, border, border);
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
    composer.window->requestResize(border + (u32)(columns)*composer.geometry.cellPixelWidth, border + (u32)(rows)*composer.geometry.cellPixelHeight);
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

void ApplicationImpl::childSignalHandler(int signal, siginfo_t*, void*) {
    // SIGCHLD does not queue: one delivery may stand for several exited
    // children (the shell plus xdg-open helpers), so reap until drained.
    if (signal == SIGCHLD) {
        int status = 0;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            // Reap only. Which shell dying ends the process is not
            // decidable here: the answer depends on how many sessions are
            // left, and that races with the close this same death is
            // about to trigger through the pty's EOF path. That path owns
            // the decision; the status is recorded for it to exit with.
            lastChildStatus = (sig_atomic_t)(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        }
    }
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

bool ApplicationImpl::presentTerminal() {
    // composer.sessions, not the member: under test the session set is
    // the harness's, published there before the first frame can land.
    Vterm* const vterm = composer.sessions->activeTerminal();
    if (composer.renderer == nullptr) {
        return false;
    }
    const TerminalUpdate* const output = vterm->output();
    if (output == nullptr) {
        const bool repainted = composer.renderer->repaint();
        if (!repainted) {
            composer.window->requestFrame();
        }
        return repainted;
    }
    const bool presented = composer.renderer->update(*output);
    if (!presented) {
        composer.window->requestFrame();
        return false;
    }
    // Keep the input-method candidate window anchored to the cursor cell.
    composer.window->requestTextInputRect((i32)(composer.layout.originX + (u32)(output->cursor.posX) * composer.geometry.cellPixelWidth), (i32)(composer.layout.originY + (u32)(output->cursor.posY) * composer.geometry.cellPixelHeight), composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
    vterm->consume();
    return true;
}

void ApplicationImpl::close() {
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
    const u16 previousColumns = composer.geometry.columns;
    const u16 previousRows = composer.geometry.rows;
    composer.resizeWindow((u16)(min(info.width, (u32)(UINT16_MAX))), (u16)(min(info.height, (u32)(UINT16_MAX))));
    if (composer.debugFd >= 0 && (composer.geometry.columns != previousColumns || composer.geometry.rows != previousRows)) {
        StringBuilder line;
        line << StringView(u8"window ") << (i64)(info.width) << StringView(u8"x") << (i64)(info.height);
        line << StringView(u8" scale=") << (i64)((int)(info.contentScale * 100));
        line << StringView(u8" fullscreen=") << (i64)(info.fullscreen) << StringView(u8" maximized=") << (i64)(info.maximized) << StringView(u8" tiled=") << (i64)(info.tiled);
        line << StringView(u8" grid ") << (i64)(previousColumns) << StringView(u8"x") << (i64)(previousRows);
        line << StringView(u8" -> ") << (i64)(composer.geometry.columns) << StringView(u8"x") << (i64)(composer.geometry.rows);
        debugTraceLine(composer, StringView(line));
    }
    if (composer.opts->vt.verbose && (composer.geometry.columns != previousColumns || composer.geometry.rows != previousRows)) {
        // The full-screen transition bugs live in the resize sequence a
        // platform delivers; the trace is how a report shows it to us.
        fprintf(stderr, "%s: window: %ux%u px, grid %ux%u -> %ux%u, scale %.2f%s%s\n", composer.brand->identifierCString(), info.width, info.height, previousColumns, previousRows, composer.geometry.columns, composer.geometry.rows, (double)(info.contentScale), info.fullscreen ? ", fullscreen" : "", info.maximized ? ", maximized" : "");
    }
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
        createRenderer();
        composer.sessions->activeTerminal()->expose();
    }
    return presentTerminal();
}

bool ApplicationImpl::eventLoop() {
    composer.platform->run();
    return true;
}

void ApplicationImpl::showWindow() {
    const u32 border = 2u * composer.layout.borderPixels;
    const u32 width = border + (u32)(composer.opts->nCols) * composer.geometry.cellPixelWidth;
    const u32 height = border + (u32)(composer.opts->nRows) * composer.geometry.cellPixelHeight;
    composer.window->requestShow();
    composer.resizeWindow((u16)(min(width, (u32)(UINT16_MAX))), (u16)(min(height, (u32)(UINT16_MAX))));
}

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

int ApplicationImpl::run(int argc, char* argv[]) {
    int testFd = -1;
#ifdef SHITTY_FOR_TESTS
    testFd = takeTestFd(argc, argv);
#endif
    checkLocale();
    // After the locale: option parsing resolves the auto width level by
    // probing the libc's wcwidth.
    composer.config = Config::create(composer);
    composer.config->initialize(&argc, argv);
    composer.installFontRenderers();
    // In the parent, before any thread exists. TERM and the version are
    // process-wide constants identical for every terminal behind the
    // window, and setenv() must never run in a forked child of a
    // multithreaded process: glibc's environ lock is not reset at fork.
    configureTerminalChildEnvironment(*composer.brand, composer.opts->vt.widths);
    enterHomeWhenLaunchedFromDesktop();
    composer.fontSize = composer.opts->fontsize;
    composer.inputRemap = InputRemap::create(composer);
    if (testFd >= 0) {
        return runTestMode(composer, *TestInput::create(composer), *this, *this, testFd, argc, argv);
    }

    composer.launch = composer.pool->make<LaunchCommand>(buildLaunchCommand(argc, argv, composer.opts->shell, composer.opts->login));
    if (composer.platform == nullptr) {
        composer.platform = plt::Platform::create(*composer.pool);
    }
    // Input deliveries run on one fiber, so stream-backed handlers may
    // suspend without stopping the event loop; later input waits in the
    // sink's queue.
    composer.input = plt::createFiberInputSink(*composer.pool, *composer.platform->scheduler(), *composer.input);
    composer.window = composer.platform->createWindow(
        *composer.pool,
        {
            .appId = composer.brand->identifier(),
            .title = composer.opts->vt.title,
            .width = (u32)(max(320, (int)(composer.opts->nCols) * composer.opts->fontsize / 2)),
            .height = (u32)(max(200, (int)(composer.opts->nRows) * composer.opts->fontsize)),
            .decorations = !composer.opts->noDecorations,
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
#endif
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
    composer.pty = createPty(*composer.pool, *composer.platform->scheduler(), composer.platform);

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
