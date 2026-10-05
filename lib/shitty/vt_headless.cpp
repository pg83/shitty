/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "vt_headless.h"

#include "composer.h"
#include "pane_layout.h"
#include "grid_geometry.h"

#include <lib/vterm/pty.h>
#include <lib/vterm/fatal.h>
#include <lib/vterm/vterm.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/vt_config.h>
#include <lib/vterm/vt_geometry.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/ios/out.h>
#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <plt/fiber.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/platform_headless.h>

using namespace stl;

namespace {
    // The headless pty face: one reusable full-length chunk, and a send
    // forwards to whatever Output the host installed. No child, no
    // drain thread; the read side never delivers.
    struct OutputPtyHandle final: public PtyHandle {
        OutputPtyHandle(plt::Scheduler& scheduler, Output& sink);

        void resize(const PtySize& size) override;
        void engage() override;
        Chunk* allocate(size_t len) override;
        void send(Chunk* chunk, size_t len) override;
        Chunk* acquire() override;
        void release(Chunk* chunks) override;
        pid_t foregroundProcessGroup() override;

        struct HeadlessChunk final: public Chunk {
            void* data() override;
            size_t length() override;
            Chunk* next() override;

            Buffer payload_;
            size_t used_ = 0;
            bool loaned_ = false;
        };

        plt::Scheduler& scheduler;
        Output& sink;
        HeadlessChunk chunk_;
    };

    struct VtermHeadlessImpl final: public VtermHeadless {
        void feed(const u8* data, size_t len) override;
        Vterm* terminal() override;
        plt::Platform* platform() override;
        plt::Window* window() override;
        VtHost* host() override;
        VtGeometry& geometry() override;
        VtCellExtras& extras() override;

        // The embedding pieces are the composer's, not this adapter's
        // own - upstream's headless builds them itself because it has no
        // Composer to build them for it.
        Composer* composer_ = nullptr;
        Vterm* terminal_ = nullptr;
        plt::Platform* platform_ = nullptr;
        plt::Window* window_ = nullptr;
        VtHost* host_ = nullptr;
    };

    // The headless host owns its terminal for the process lifetime, so it
    // also owns the resize and font deliveries a session set would make.
    //
    // Upstream's HeadlessVtHost is not here: it exists because upstream's
    // headless builds its own window with no Composer around it, while
    // ours is a Composer embedder and borrows the adapter Composer
    // already installs. A second adapter beside it would be a second
    // place the window is reached from.
    struct CallHeadlessResize final: public Listener {
        CallHeadlessResize(Composer& composer, Vterm* terminal);

        void onListen(void*) override;

        // A8: the headless host shows one terminal filling its surface,
        // so the pane it hands over is the window's - but it has to hand
        // one over, because the terminal no longer reads the composer for
        // its grid.
        Composer& composer;
        Vterm* terminal;
    };

    struct CallHeadlessFontChanged final: public Listener {
        explicit CallHeadlessFontChanged(Vterm* terminal);

        void onListen(void*) override;

        Vterm* terminal;
    };
}

void* OutputPtyHandle::HeadlessChunk::data() {
    return payload_.mutData();
}

size_t OutputPtyHandle::HeadlessChunk::length() {
    return used_;
}

PtyHandle::Chunk* OutputPtyHandle::HeadlessChunk::next() {
    return nullptr;
}

OutputPtyHandle::OutputPtyHandle(plt::Scheduler& scheduler_, Output& sink_)
    : scheduler(scheduler_)
    , sink(sink_)
{
}

void OutputPtyHandle::resize(const PtySize&) {
}

void OutputPtyHandle::engage() {
}

PtyHandle::Chunk* OutputPtyHandle::allocate(size_t len) {
    STD_INSIST(!chunk_.loaned_);
    chunk_.payload_.reset();
    chunk_.payload_.grow(len);
    chunk_.payload_.seekAbsolute(len);
    chunk_.used_ = len;
    chunk_.loaned_ = true;
    return &chunk_;
}

void OutputPtyHandle::send(Chunk* chunk, size_t len) {
    STD_INSIST(chunk == &chunk_ && chunk_.loaned_);
    chunk_.loaned_ = false;
    sink.write(chunk_.payload_.data(), len);
    sink.flush();
}

PtyHandle::Chunk* OutputPtyHandle::acquire() {
    scheduler.current()->park();
    return nullptr;
}

void OutputPtyHandle::release(Chunk*) {
}

pid_t OutputPtyHandle::foregroundProcessGroup() {
    return 0;
}

CallHeadlessResize::CallHeadlessResize(Composer& composer_, Vterm* terminal_)
    : composer(composer_)
    , terminal(terminal_)
{
}

void CallHeadlessResize::onListen(void*) {
    terminal->paneResized(windowPane(composer));
}

CallHeadlessFontChanged::CallHeadlessFontChanged(Vterm* terminal_)
    : terminal(terminal_)
{
}

void CallHeadlessFontChanged::onListen(void*) {
    terminal->presentationInvalidated();
}

void VtermHeadlessImpl::feed(const u8* data, size_t len) {
    if (data == nullptr && len != 0) {
        raiseError(StringView(u8"headless Vterm input is null"));
    }
    if (len == 0) {
        return;
    }
    terminal_->feedPty(StringView(data, len));
    if (terminal_->output() != nullptr) {
        terminal_->consume();
    }
}

Vterm* VtermHeadlessImpl::terminal() {
    return terminal_;
}

plt::Platform* VtermHeadlessImpl::platform() {
    return platform_;
}

plt::Window* VtermHeadlessImpl::window() {
    return window_;
}

VtHost* VtermHeadlessImpl::host() {
    return host_;
}

VtGeometry& VtermHeadlessImpl::geometry() {
    return composer_->geometry;
}

VtCellExtras& VtermHeadlessImpl::extras() {
    return composer_->extras;
}

VtermHeadless* VtermHeadless::create(Composer& composer, VtermTraceFactory* traceFactory, Output* ptyCapture) {
    constexpr u16 columns = 80;
    constexpr u16 rows = 24;
    constexpr u16 cellPixelWidth = 1;
    constexpr u16 cellPixelHeight = 1;
    // A1: the headless surface is counted out of contentInsets() like
    // every other one, not out of a bare cell count - the border option
    // is an option here too.
    const Insets insets = composer.contentInsets();
    const u16 pixelWidth = (u16)(gridPixelWidth(columns, insets, cellPixelWidth));
    const u16 pixelHeight = (u16)(gridPixelHeight(rows, insets, cellPixelHeight));

    plt::Platform* const platform = plt::createHeadlessPlatform(*composer.pool);
    plt::Window* const window = platform->createWindow(
        *composer.pool,
        {
            .width = pixelWidth,
            .height = pixelHeight,
        }
    );
    composer.platform = platform;
    composer.window = window;
    composer.installVtHost();
    composer.geometry.setCellPixelSize(cellPixelWidth, cellPixelHeight);
    composer.resize(pixelWidth, pixelHeight);

    VtermHeadlessImpl* const result = composer.pool->make<VtermHeadlessImpl>();
    result->composer_ = &composer;
    result->platform_ = platform;
    result->window_ = window;
    result->host_ = composer.host;
    plt::Scheduler* const scheduler = platform->scheduler();
    Output* const sink = ptyCapture != nullptr ? ptyCapture : createNullOutput(composer.pool);
    Vterm* const vterm = Vterm::create(*composer.pool, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *scheduler, *composer.host, windowPane(composer), *composer.pool->make<OutputPtyHandle>(*scheduler, *sink), traceFactory);
    result->terminal_ = vterm;
    composer.resizedListeners.pushBack(composer.pool->make<CallHeadlessResize>(composer, vterm));
    composer.fontChangedListeners.pushBack(composer.pool->make<CallHeadlessFontChanged>(vterm));
    return result;
}
