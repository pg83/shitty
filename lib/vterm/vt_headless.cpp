/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "vt_headless.h"

#include "pty.h"
#include "vterm.h"

#include <lib/vterm/fatal.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/vt_config.h>
#include <lib/vterm/vt_geometry.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/ios/out.h>
#include <std/ios/input.h>
#include <std/alg/minmax.h>
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

        VtGeometry geometry_;
        VtConfigSlot configSlot_;
        VtCellExtras extras_;
        Vterm* terminal_ = nullptr;
        plt::Platform* platform_ = nullptr;
        plt::Window* window_ = nullptr;
        VtHost* host_ = nullptr;
    };

    // The headless host owns its terminal for the process lifetime, so
    // it is also the terminal's embedder: window requests forward to the
    // headless window, and the events a session set would fan out go
    // straight to the one terminal.
    struct HeadlessVtHost final: public VtHost {
        explicit HeadlessVtHost(plt::Window* window);

        plt::Clipboard* primary() override;
        plt::Clipboard* secondary() override;
        plt::WindowInfo info() override;
        void requestFrame() override;
        void requestResize(u32 width, u32 height) override;
        void requestResizeCells(u32 columns, u32 rows) override;
        VtGridSize gridSize(u32 pixelWidth, u32 pixelHeight) override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestIconify() override;
        void requestRestore() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestAttention() override;
        void requestPointerIcon(plt::PointerIcon icon) override;
        void requestOpenUri(stl::StringView uri) override;
        bool uriSchemeAllowed(stl::StringView scheme) override;
        void titleChanged(const VtermTitleChanged& event) override;
        void resized() override;

        plt::Window* window;
        Vterm* terminal = nullptr;
        VtGeometry* geometry = nullptr;
    };
}

HeadlessVtHost::HeadlessVtHost(plt::Window* window_)
    : window(window_)
{
}

plt::Clipboard* HeadlessVtHost::primary() {
    return window->primary();
}

plt::Clipboard* HeadlessVtHost::secondary() {
    return window->secondary();
}

plt::WindowInfo HeadlessVtHost::info() {
    return window->info();
}

void HeadlessVtHost::requestFrame() {
    window->requestFrame();
}

void HeadlessVtHost::requestResize(u32 width, u32 height) {
    window->requestResize(width, height);
    const auto grid = gridSize(width, height);
    geometry->resizeCells((u16)(min(grid.columns, (u32)(UINT16_MAX))), (u16)(min(grid.rows, (u32)(UINT16_MAX))), this);
}

VtGridSize HeadlessVtHost::gridSize(u32 width, u32 height) {
    return {max(1u, width / geometry->cellPixelWidth), max(1u, height / geometry->cellPixelHeight)};
}

void HeadlessVtHost::requestResizeCells(u32 columns, u32 rows) {
    requestResize(columns * geometry->cellPixelWidth, rows * geometry->cellPixelHeight);
}

void HeadlessVtHost::requestMaximized(bool maximized) {
    window->requestMaximized(maximized);
}

void HeadlessVtHost::requestFullscreen(bool fullscreen) {
    window->requestFullscreen(fullscreen);
}

void HeadlessVtHost::requestIconify() {
    window->requestIconify();
}

void HeadlessVtHost::requestRestore() {
    window->requestRestore();
}

void HeadlessVtHost::requestMove(i32 x, i32 y) {
    window->requestMove(x, y);
}

void HeadlessVtHost::requestFocus() {
    window->requestFocus();
}

void HeadlessVtHost::requestAttention() {
    window->requestAttention();
}

void HeadlessVtHost::requestPointerIcon(plt::PointerIcon icon) {
    window->requestPointerIcon(icon);
}

void HeadlessVtHost::requestOpenUri(StringView uri) {
    window->requestOpenUri(uri);
}

bool HeadlessVtHost::uriSchemeAllowed(StringView scheme) {
    // The parsed-option default, so a headless terminal detects the
    // same links a freshly launched GUI one would.
    static const StringView allowed[] = {
        StringView(u8"http"),
        StringView(u8"https"),
        StringView(u8"file"),
    };
    for (const StringView& candidate : allowed) {
        if (scheme.length() != candidate.length()) {
            continue;
        }
        bool match = true;
        for (size_t index = 0; index < scheme.length(); ++index) {
            const u8 byte = scheme[index];
            const u8 folded = byte >= 'A' && byte <= 'Z' ? (u8)(byte + ('a' - 'A')) : byte;
            if (folded != candidate[index]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

void HeadlessVtHost::titleChanged(const VtermTitleChanged&) {
}

void HeadlessVtHost::resized() {
    if (terminal != nullptr) {
        terminal->windowResized();
    }
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
    return geometry_;
}

VtCellExtras& VtermHeadlessImpl::extras() {
    return extras_;
}

VtermHeadless* VtermHeadless::create(ObjPool& pool, const VtConfig& config, VtermTraceFactory* traceFactory, Output* ptyCapture) {
    constexpr u16 columns = 80;
    constexpr u16 rows = 24;
    constexpr u16 cellPixelWidth = 1;
    constexpr u16 cellPixelHeight = 1;
    constexpr u16 pixelWidth = columns * cellPixelWidth;
    constexpr u16 pixelHeight = rows * cellPixelHeight;

    plt::Platform* const platform = plt::createHeadlessPlatform(pool);
    plt::Window* const window = platform->createWindow(
        pool,
        {
            .width = pixelWidth,
            .height = pixelHeight,
        }
    );
    HeadlessVtHost* const host = pool.make<HeadlessVtHost>(window);
    VtermHeadlessImpl* const result = pool.make<VtermHeadlessImpl>();
    result->platform_ = platform;
    result->window_ = window;
    result->host_ = host;
    result->configSlot_.config = &config;
    host->geometry = &result->geometry_;
    result->geometry_.setCellPixelSize(cellPixelWidth, cellPixelHeight);
    result->geometry_.resizeCells(columns, rows, host);
    result->extras_.store = CellExtraStore::create(result->extras_, pool);
    SmallObjAllocator* const smallObjects = SmallObjAllocator::create(&pool);
    plt::Scheduler* const scheduler = platform->scheduler();
    Output* const sink = ptyCapture != nullptr ? ptyCapture : createNullOutput(&pool);
    Vterm* const vterm = Vterm::create(pool, result->geometry_, result->configSlot_, result->extras_, *smallObjects, *scheduler, *host, *pool.make<OutputPtyHandle>(*scheduler, *sink), traceFactory);
    result->terminal_ = vterm;
    host->terminal = vterm;
    return result;
}
