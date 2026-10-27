/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pty.h"
#include "composer.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/vt_headless.h>

#include <std/tst/ut.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>

#include <unistd.h>
#include <plt/fiber.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/platform_headless.h>

using namespace stl;

namespace {
    struct CaptureOutput final: public Output {
        size_t writeImpl(const void* data, size_t size) override;

        Buffer bytes;
        size_t writes = 0;
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

    static void feedInFuzzChunks(Vterm& terminal, const u8* bytes, size_t size) {
        const size_t first = bytes[0] % size;
        const size_t second = first + bytes[1] % (size - first);
        terminal.feedPty(StringView(bytes, first));
        terminal.feedPty(StringView(bytes + first, second - first));
        terminal.feedPty(StringView(bytes + second, size - second));
    }
}

size_t CaptureOutput::writeImpl(const void* data, size_t size) {
    ++writes;
    bytes.append(data, size);
    return size;
}

namespace {
    // The second terminal of the coexistence test needs a pty face of
    // its own; test scaffolding stays in the test.
    struct SecondPtyStub final: public PtyHandle {
        explicit SecondPtyStub(plt::Scheduler& scheduler_)
            : scheduler(scheduler_)
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

        void send(Chunk*, size_t) override {
        }

        Chunk* acquire() override {
            scheduler.current()->park();
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

        plt::Scheduler& scheduler;
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

        void requestResizeCells(u32 columns, u32 rows) override {
            inner->requestResizeCells(columns, rows);
        }

        VtGridSize gridSize(u32 width, u32 height) override {
            return inner->gridSize(width, height);
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

        VtHost* inner;
        Buffer lastTitle;
    };
}

STD_TEST_SUITE(VtermHeadless) {
    STD_TEST(BuildsItsOwnEmbeddingPieces) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());

        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);

        STD_INSIST(headless->platform() != nullptr);
        STD_INSIST(headless->window() != nullptr);
        STD_INSIST(headless->host() != nullptr);
        STD_INSIST(headless->host()->primary() != nullptr);
        STD_INSIST(headless->host()->secondary() != nullptr);
        STD_INSIST(headless->geometry().columns != 0);
        STD_INSIST(headless->terminal() != nullptr);
    }

    // A tab is a second terminal behind the same window. Two of them must
    // be able to exist at once against one set of embedding pieces: the
    // geometry, the extras and the host are per window, and each terminal
    // only adds itself on top.
    STD_TEST(SecondVtermCoexistsOnOneEmbedding) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
        Vterm* const first = headless->terminal();

        plt::Scheduler& scheduler = *headless->platform()->scheduler();
        Vterm* const second = Vterm::create(*composer.pool, headless->geometry(), composer.vtConfig, headless->extras(), *composer.smallObjects, scheduler, *headless->host(), *composer.pool->make<SecondPtyStub>(scheduler), nullptr);

        STD_INSIST(first != nullptr);
        STD_INSIST(second != nullptr);
        STD_INSIST(first != second);
    }

    STD_TEST(ForegroundProcessFillsTheTitleFallback) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
        plt::Scheduler& scheduler = *headless->platform()->scheduler();
        TitleCaptureHost& host = *composer.pool->make<TitleCaptureHost>(headless->host());
        SecondPtyStub& pty = *composer.pool->make<SecondPtyStub>(scheduler);
        Vterm* const terminal = Vterm::create(*composer.pool, headless->geometry(), composer.vtConfig, headless->extras(), *composer.smallObjects, scheduler, host, pty, nullptr);

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
        VtConfig config = *composer.vtConfig.config;
        config.saveLines = 10;
        Vterm* const terminal = VtermHeadless::create(*composer.pool, config, nullptr)->terminal();
        for (int line = 0; line < 40; ++line) {
            const u8 text[] = {'x', '\r', '\n'};
            terminal->feedPty(StringView(text, sizeof(text)));
        }

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
        Vterm* const terminal = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr)->terminal();
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
        Vterm& whole = *VtermHeadless::create(*wholeComposer.pool, *wholeComposer.vtConfig.config, nullptr)->terminal();
        Vterm& split = *VtermHeadless::create(*splitComposer.pool, *splitComposer.vtConfig.config, nullptr)->terminal();
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
        Vterm& whole = *VtermHeadless::create(*wholeComposer.pool, *wholeComposer.vtConfig.config, nullptr)->terminal();
        Vterm& split = *VtermHeadless::create(*splitComposer.pool, *splitComposer.vtConfig.config, nullptr)->terminal();
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
        Vterm& terminal = *VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr, &pty)->terminal();
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
        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr, &pty);
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
        Vterm* const terminal = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr, &pty)->terminal();
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
        Vterm* const terminal = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr, &pty)->terminal();
        const u8 input[] = {'\x1b', '%', '@', 0x9a};

        terminal->feedPty(StringView(input, sizeof(input)));

        STD_INSIST(!pty.bytes.empty());
    }

    STD_TEST(FlushSkipsUnchangedFramesButRequestsPendingOutput) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr);
        Vterm* const terminal = headless->terminal();
        auto* const window = static_cast<plt::WindowHeadless*>(headless->window());
        while (window->framePending()) {
            window->dispatchFrame();
        }

        terminal->flush();
        STD_INSIST(!window->framePending());

        terminal->expose();
        terminal->flush();
        STD_INSIST(window->framePending());
    }

    STD_TEST(WheelReportsShareOnePtyWrite) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr, &pty);
        Vterm* const terminal = headless->terminal();
        terminal->feedPty(StringView(u8"\x1b[?1049h\x1b[?1000h\x1b[?1006h"));
        pty.bytes.reset();
        pty.writes = 0;

        terminal->scroll({.y = 3.0});

        STD_INSIST(pty.writes == 1);
        STD_INSIST(StringView(pty.bytes) == StringView(u8"\x1b[<64;1;1M\x1b[<64;1;1M\x1b[<64;1;1M"));
    }

    STD_TEST(PreciseWheelUsesCellMetricsAndContentScale) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CaptureOutput pty;
        VtermHeadless* const headless = VtermHeadless::create(*composer.pool, *composer.vtConfig.config, nullptr, &pty);
        Vterm* const terminal = headless->terminal();
        headless->geometry().setCellPixelSize(10, 20);
        terminal->feedPty(StringView(u8"\x1b[?1049h\x1b[?1000h\x1b[?1006h"));
        pty.bytes.reset();
        pty.writes = 0;

        // Two native units are 20 logical pixels, or 40 backing pixels at
        // scale two: three rows with the 1.5x sensitivity multiplier.
        terminal->scroll({.y = 2.0, .precise = true, .contentScale = 2.0});

        STD_INSIST(pty.writes == 1);
        STD_INSIST(StringView(pty.bytes) == StringView(u8"\x1b[<64;1;1M\x1b[<64;1;1M\x1b[<64;1;1M"));
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
            Vterm& whole = *VtermHeadless::create(*wholeComposer.pool, *wholeComposer.vtConfig.config, nullptr)->terminal();
            Vterm& split = *VtermHeadless::create(*splitComposer.pool, *splitComposer.vtConfig.config, nullptr)->terminal();
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
