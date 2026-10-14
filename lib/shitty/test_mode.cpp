/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "test_mode.h"

#include "pty.h"
#include "render.h"
#include "options.h"
#include "session.h"
#include "startup.h"
#include "composer.h"
#include "font_pack.h"
#include "test_input.h"
#include "application.h"
#include "debug_trace.h"
#include "drop_target.h"
#include "span_shaper.h"
#include "configuration.h"
#include "grid_geometry.h"
#include "render_reference.h"

#include <lib/vterm/hex.h>
#include <lib/vterm/num.h>
#include <lib/vterm/utf8.h>
#include <lib/vterm/fatal.h>
#include <lib/vterm/screen.h>
#include <lib/vterm/grapheme.h>
#include <lib/vterm/keyboard.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/input_handler.h>
#include <lib/vterm/mouse_frontend.h>
#include <lib/vterm/mouse_protocol.h>
#include <lib/vterm/cell_extra_store.h>

#if defined(HAVE_VULKAN_WAYLAND)
    #include "render_vk.h"
#endif
#if defined(HAVE_METAL_RENDERER)
    #include "render_metal.h"
#endif
#include <lib/vterm/vterm.h>
#include <lib/vterm/vt_test.h>
#include <lib/vterm/vt_trace.h>

#include <plt/clipboard.h>
#include <plt/drop.h>
#include <plt/fiber.h>
#include <plt/mutex.h>
#include <plt/platform_headless.h>

#include <std/alg/defer.h>
#include <std/alg/minmax.h>
#include <std/dbg/assert.h>
#include <std/ios/output.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/alg/xchg.h>
#include <std/lib/buffer.h>
#include <std/thr/runable.h>
#include <std/ios/in_mem.h>
#include <std/ios/input.h>
#include <std/ios/sys.h>
#include <std/mem/obj_list.h>
#include <std/dbg/insist.h>
#include <std/mem/small_obj_allocator.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/sym/i_map.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <fcntl.h>
#include <limits.h>
#include <functional>
#include <map>
#include <new>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

using namespace stl;
using namespace plt;

namespace {
    extern "C" int openpty(int*, int*, char*, const termios*, const winsize*);

    struct TestFontpack final: Fontpack, Listener {
        explicit TestFontpack(Composer& composer_)
            : composer(composer_)
        {
            if (const char* value = getenv("SHITTY_TEST_BOX_STROKE")) {
                char* end = nullptr;
                const float parsed = strtof(value, &end);
                if (end != value && *end == '\0' && parsed > 0.0f) {
                    boxStroke = parsed;
                }
            }
        }

        u16 getPx() const override {
            return composer.geometry.cellPixelWidth;
        }

        u16 getPy() const override {
            return composer.geometry.cellPixelHeight;
        }

        float boxDrawingStroke() const override {
            return boxStroke;
        }

        bool hasBold() const override {
            return false;
        }

        bool hasItalic() const override {
            return false;
        }

        bool hasBoldItalic() const override {
            return false;
        }

        void onListen(void*) override {
            composer.fontSize = composer.opts->fontsize;
            for (IntrusiveNode* node = composer.fontChangedListeners.mutFront(); node != composer.fontChangedListeners.mutEnd();) {
                Listener* const listener = static_cast<Listener*>(node);
                node = node->next;
                listener->onListen();
            }
        }

        Font* styledFace(Font* face, FontStyle) const override {
            return face;
        }

        Font* resolveFace(const u32* codepoints, size_t count) override {
            // With the Vulkan shadow armed, behave like the real pack: a
            // cluster without a verdict unwinds the frame once, so the
            // shadow's acquire/submit cycle sees the miss-retry storm the
            // fake coverage otherwise hides.
            if (throwMisses && missed != nullptr && count != 0) {
                const u64 key = StringView((const u8*)(codepoints), count * sizeof(u32)).hash64();
                if (missed->find(key) == nullptr) {
                    FontFaceMiss miss;
                    miss.count = count < FontFaceMiss::limit ? count : FontFaceMiss::limit;
                    memcpy(miss.codepoints, codepoints, miss.count * sizeof(u32));
                    throw miss;
                }
            }
            return nullptr;
        }

        void adoptFaceFor(const FontFaceMiss& miss) override {
            if (missed == nullptr || miss.count == 0) {
                return;
            }
            const u64 key = StringView((const u8*)(miss.codepoints), miss.count * sizeof(u32)).hash64();
            *missed->insert(key) = key;
        }

        void armMissThrows(ObjPool& pool) {
            missed = pool.make<IntMap<u64>>(&pool);
            throwMisses = true;
        }

        float boxStroke = 0.0f;

        Composer& composer;
        Buffer bitmap;
        IntMap<u64>* missed = nullptr;
        bool throwMisses = false;
    };

    // Forwards every frame to the reference renderer and, when armed, to
    // the Vulkan shadow: the same updates run the real swapchain cycle
    // over a headless surface, so the acquire/submit/present machinery is
    // exercised by ordinary harness traffic.
    struct MirrorRenderer final: public Renderer {
        MirrorRenderer(Renderer* primary_, Renderer* shadow_)
            : primary(primary_)
            , shadow(shadow_)
        {
        }

        bool update(const TerminalUpdate& update) override {
            // The shadow goes first so a cluster without a face verdict
            // unwinds ITS frame: the reference pass would otherwise adopt
            // every face and starve the Vulkan path of the miss-retry
            // cycle this shadow exists to exercise.
            shadow->update(update);
            return primary->update(update);
        }

        // R3-qa. Without this the multi-pane form fell through to the
        // Renderer default, which refuses any frame wider than one pane
        // by construction (render.h) - so with the shadow armed no test
        // ever built a split frame on the second backend, and the two
        // could not be compared on the only kind of frame this plan is
        // about. Same order as above, and for the same reason.
        bool update(const PaneUpdate* panes, size_t count) override {
            shadow->update(panes, count);
            return primary->update(panes, count);
        }

        // Seams belong to the frame, so both backends have to be told
        // about them before the update they apply to; a shadow drawing
        // a split window without dividers is not the same frame.
        void setSeams(const PixelRect* seams, size_t count, Color ink) override {
            shadow->setSeams(seams, count, ink);
            primary->setSeams(seams, count, ink);
        }

        bool repaint() override {
            shadow->repaint();
            return primary->repaint();
        }

        Renderer* primary;
        Renderer* shadow;
    };

    struct TestPty;

    // The harness chunk: one reusable buffer granted at full length -
    // the harness does not cap - so a send carries a whole write exactly
    // like the stream face did, and the scripted 64K flush contract
    // keeps meaning what it meant.
    struct TestChunk final: public PtyHandle::Chunk {
        void* data() override;
        size_t length() override;
        PtyHandle::Chunk* next() override;

        Buffer payload_;
        size_t used_ = 0;
        bool loaned_ = false;
    };

    struct TestPtyStager final: public Runable {
        explicit TestPtyStager(TestPty* pty);

        void run() override;

        TestPty* pty;
    };

    // Scripted PTY endpoints; the tests install these instead of lambdas.
    struct PtyReadHandler {
        virtual ssize_t read(u8* buffer, size_t size) = 0;
    };

    struct PtyWriteHandler {
        virtual ssize_t write(const u8* buffer, size_t size) = 0;
    };

    // Scripted PTY reads: byte payloads live in one arena, records index
    // into it, and head walks forward instead of popping the front.
    struct ScriptedPtyRead {
        u32 offset = 0;
        u32 length = 0;
        int error = 0;
        bool eof = false;
    };

    struct ScriptedReadQueue final: public PtyReadHandler {
        Vector<ScriptedPtyRead> items;
        Buffer arena;
        size_t head = 0;

        bool empty() const {
            return head == items.length();
        }

        void compact() {
            if (empty()) {
                items.clear();
                arena.reset();
                head = 0;
            }
        }

        void push(StringView data, int error, bool eof) {
            ScriptedPtyRead item;
            item.offset = (u32)(arena.used());
            item.length = (u32)(data.length());
            item.error = error;
            item.eof = eof;
            arena.append(data.data(), data.length());
            items.pushBack(item);
        }

        size_t pendingBytes() const {
            size_t total = 0;
            for (size_t index = head; index < items.length(); ++index) {
                total += items[index].length;
            }
            return total;
        }

        ssize_t read(u8* buffer, size_t size) override {
            if (empty()) {
                errno = EAGAIN;
                return (ssize_t)(-1);
            }
            ScriptedPtyRead& item = items.mut(head);
            if (item.eof) {
                ++head;
                compact();
                return (ssize_t)(0);
            }
            if (item.error) {
                errno = item.error;
                ++head;
                compact();
                return (ssize_t)(-1);
            }
            const size_t count = min(size, (size_t)(item.length));
            memcpy(buffer, (const u8*)(arena.data()) + item.offset, count);
            item.offset += (u32)(count);
            item.length -= (u32)(count);
            if (item.length == 0) {
                ++head;
                compact();
            }
            return (ssize_t)(count);
        }
    };

    struct ScriptedPtyWrite {
        size_t count = 0;
        int error = 0;
    };

    struct ScriptedWriteQueue final: public PtyWriteHandler {
        Vector<ScriptedPtyWrite> items;
        size_t head = 0;
        Buffer written;

        bool empty() const {
            return head == items.length();
        }

        ssize_t write(const u8* buffer, size_t size) override {
            if (empty()) {
                errno = EAGAIN;
                return (ssize_t)(-1);
            }
            const ScriptedPtyWrite item = items[head++];
            if (empty()) {
                items.clear();
                head = 0;
            }
            if (item.error) {
                errno = item.error;
                return (ssize_t)(-1);
            }
            const size_t count = min(size, item.count);
            written.append(buffer, count);
            return (ssize_t)(count);
        }
    };

    struct TestPty final: public PtyHandle {
        TestPty(Composer& composer, ObjPool& owner, int fd);
        ~TestPty() noexcept;

        void resize(const PtySize& size) override;

        void engage() override;

        Chunk* allocate(size_t len) override;

        void send(Chunk* chunk, size_t len) override;

        Chunk* acquire() override;

        void release(Chunk* chunks) override;
        pid_t foregroundProcessGroup() override;

        plt::FiberMutex* mutex_ = nullptr;
        ssize_t read(u8* buffer, size_t size);
        ssize_t write(const u8* buffer, size_t size);
        size_t rawWrite(const void* data, size_t size);
        void start();
        bool outputDrained() const;
        bool scriptStalled() const;
        void kickOutput();
        void setReadHandler(PtyReadHandler* handler);
        void setWriteHandler(PtyWriteHandler* handler);
        void takeReadData(Buffer& out);
        void takeWriteData(Buffer& out);

        Composer& composer_;
        ObjPool& owner_;
        int fd_;
        PtyReadHandler* onRead = nullptr;
        PtyWriteHandler* onWrite = nullptr;
        Buffer readData;
        Buffer writeData;
        TestChunk outChunk_;
        TestPtyStager stager_;
        Buffer staged_;
        plt::Fiber* stagerFiber_ = nullptr;
        plt::Fiber* blockedWriter_ = nullptr;
        bool scriptedWrites_ = false;
    };

    struct TestPtyFactory final: public Pty {
        TestPtyFactory(Composer& composer, int firstFd);

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command, const PtySize& size, StringView directory) override;

        Composer& composer;
        int firstFd;
        bool first = true;
        Vector<TestPty*> handles;
        Vector<int> peers;
    };

    struct TestUtf8Decoder {
        TestUtf8Decoder();

        void push(StringView input, Vector<u32>& result);
        void flush(Vector<u32>& result);
        void reset();

        Vector<u32> output;
        Utf8Decoder decoder;
    };

    struct FailFontChange final: public Listener {
        void arm();
        void onListen(void*) override;

        bool armed = false;
    };

    struct MemDropOffer final: public plt::DropOffer {
        size_t formats() const override;
        StringView format(size_t index) const override;

        StringView mime;
    };

    struct MemDrop final: public plt::Drop {
        plt::DropOffer* what() override;
        Input* read(StringView mime) override;

        MemDropOffer offer;
        StringView payload;
    };

    // One drop delivered through the real drop target on a dedicated
    // fiber, mirroring the platform's DnD transfer fiber.
    struct DropDelivery final: public Runable {
        DropDelivery(plt::DropTarget* target, StringView payload, StringView mime);

        void run() override;

        plt::DropTarget* target;
        Buffer payload;
        StringView mime;
        alignas(16) u8 stack[plt::lightFiberStack];
    };
}

void* TestChunk::data() {
    return payload_.mutData();
}

size_t TestChunk::length() {
    return used_;
}

PtyHandle::Chunk* TestChunk::next() {
    return nullptr;
}

TestPtyStager::TestPtyStager(TestPty* pty_)
    : pty(pty_)
{
}

void TestPtyStager::run() {
    TestPty& impl = *pty;
    plt::Fiber* const self = impl.composer_.platform->scheduler()->current();
    Buffer local;
    for (;;) {
        while (impl.staged_.empty()) {
            self->park();
        }
        const plt::LockGuard guard(*impl.mutex_);
        while (!impl.staged_.empty()) {
            xchg(local, impl.staged_);
            impl.rawWrite(local.data(), local.used());
            local.reset();
        }
    }
}

TestPty::TestPty(Composer& composer, ObjPool& owner, int fd)
    : composer_(composer)
    , owner_(owner)
    , fd_(fd)
    , stager_(this)
{
    mutex_ = composer.platform->scheduler()->createMutex(owner);
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        raiseError(StringView(u8"test PTY nonblocking setup failed"));
    }
}

TestPty::~TestPty() noexcept {
    close(fd_);
}

PtyHandle::Chunk* TestPty::allocate(size_t len) {
    STD_INSIST(!outChunk_.loaned_);
    outChunk_.payload_.reset();
    outChunk_.payload_.grow(len);
    outChunk_.payload_.seekAbsolute(len);
    outChunk_.used_ = len;
    outChunk_.loaned_ = true;
    return &outChunk_;
}

void TestPty::send(Chunk* chunk, size_t len) {
    // The stream face's write and flush, fused: loop-context bytes stage
    // and the stager is kicked at once, a fiber writes directly under
    // the stream mutex.
    STD_INSIST(chunk == &outChunk_ && outChunk_.loaned_);
    outChunk_.loaned_ = false;
    const void* const bytes = outChunk_.payload_.data();
    plt::Scheduler* const scheduler = composer_.platform->scheduler();
    if (scheduler->current() == nullptr) {
        staged_.append(bytes, len);
        if (!staged_.empty() && stagerFiber_ != nullptr) {
            stagerFiber_->wake();
        }
        return;
    }
    if (mutex_->heldByCurrent()) {
        rawWrite(bytes, len);
        return;
    }
    const plt::LockGuard guard(*mutex_);
    rawWrite(bytes, len);
}

void TestPty::start() {
    stagerFiber_ = composer_.platform->scheduler()->create(owner_, stager_);
}

ssize_t TestPty::read(u8* buffer, size_t size) {
    const ssize_t count = onRead != nullptr ? onRead->read(buffer, size) : ::read(fd_, buffer, size);
    if (count > 0) {
        readData.append(buffer, (size_t)(count));
    }
    return count;
}

ssize_t TestPty::write(const u8* buffer, size_t size) {
    const ssize_t count = onWrite != nullptr ? onWrite->write(buffer, size) : ::write(fd_, buffer, size);
    if (count > 0) {
        writeData.append(buffer, (size_t)(count));
    }
    return count;
}

size_t TestPty::rawWrite(const void* data, size_t size) {
    plt::Scheduler* const scheduler = composer_.platform->scheduler();
    const u8* current = (const u8*)(data);
    size_t remaining = size;
    while (remaining != 0) {
        constexpr size_t maximumWrite = 64 * 1024;
        const size_t chunk = remaining < maximumWrite ? remaining : maximumWrite;
        const ssize_t count = write(current, chunk);
        if (count > 0) {
            current += count;
            remaining -= (size_t)(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (scriptedWrites_ && count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Scripted backpressure keeps the unsent bytes and waits for the
            // next FLUSH_OUTPUT kick; the test controls every retry. A fatal
            // scripted error follows the production PTY contract below and
            // drops bytes that can never be delivered.
            blockedWriter_ = scheduler->current();
            blockedWriter_->park();
            blockedWriter_ = nullptr;
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Real backpressure drains as the harness reads the slave side.
            // Never wait on the event alone: macOS kqueue does not deliver
            // write readiness for pty masters, so an open-ended wait would
            // park this fiber forever. The timeout turns a missed event
            // into a retry.
            scheduler->awaitWritable(fd_, 20'000);
            continue;
        }
        break;
    }
    return size;
}

bool TestPty::outputDrained() const {
    // A held stream mutex means a transaction is still replaying, even when
    // it waits on real backpressure rather than a scripted kick.
    return staged_.empty() && blockedWriter_ == nullptr && !mutex_->locked();
}

bool TestPty::scriptStalled() const {
    // Only a scripted stall parks the writer here; real backpressure waits
    // on the descriptor instead.
    return blockedWriter_ != nullptr;
}

void TestPty::kickOutput() {
    if (blockedWriter_ != nullptr) {
        blockedWriter_->wake();
    }
    if (!staged_.empty() && stagerFiber_ != nullptr) {
        stagerFiber_->wake();
    }
}

void TestPty::resize(const PtySize& requested) {
    winsize size{};
    size.ws_col = (unsigned short)(requested.columns);
    size.ws_row = (unsigned short)(requested.rows);
    size.ws_xpixel = (unsigned short)(requested.pixelWidth);
    size.ws_ypixel = (unsigned short)(requested.pixelHeight);
    if (ioctl(fd_, TIOCSWINSZ, &size) < 0) {
        // Deliberately harder than production, which only sysWarns here
        // (pty.cpp, resizePty). A user's terminal is worth more running
        // mis-sized than dead; a harness whose whole job is to answer
        // WINSIZE_PANE is not, and a size that silently stopped tracking
        // the grid would surface as a wrong number in some later test
        // rather than as this. Since F1a this also runs on the session
        // open path, so the failure is a refused start, which is the
        // shape it should have.
        raiseError(StringView(u8"test PTY resize failed"));
    }
}

void TestPty::engage() {
}

PtyHandle::Chunk* TestPty::acquire() {
    // The harness scripts transport reads explicitly; the production
    // session reader parks here for the arena's lifetime, exactly like
    // the stream reader did.
    for (;;) {
        composer_.platform->scheduler()->current()->park();
    }
}

void TestPty::release(Chunk*) {
}

pid_t TestPty::foregroundProcessGroup() {
    return 0;
}

void TestPty::setReadHandler(PtyReadHandler* handler) {
    onRead = handler;
}

void TestPty::setWriteHandler(PtyWriteHandler* handler) {
    onWrite = handler;
    scriptedWrites_ = true;
    if (blockedWriter_ != nullptr) {
        blockedWriter_->wake();
    }
}

void TestPty::takeReadData(Buffer& out) {
    out.reset();
    out.xchg(readData);
}

void TestPty::takeWriteData(Buffer& out) {
    out.reset();
    out.xchg(writeData);
}

TestPtyFactory::TestPtyFactory(Composer& composer_, int firstFd_)
    : composer(composer_)
    , firstFd(firstFd_)
{
}

PtyHandle* TestPtyFactory::spawn(ObjPool& owner, const LaunchCommand&, const PtySize& size, StringView) {
    int fd = firstFd;
    // The first fd arrives from runTestMode() already open, so it never
    // passes through the openpty() below and has to be sized on its own.
    // Sizing it is not optional: openSession() no longer resizes a handle
    // after spawning it (K3), so this is the only place the first pane's
    // pty learns its grid at all.
    const bool fromOutside = first;
    if (first) {
        first = false;
    } else {
        // Every session after the first gets its geometry the same way the
        // real factory does it - on the slave, before anyone can read
        // TIOCGWINSZ. A pane born from a split would survive without it,
        // because applyLayout() sizes the whole new layout straight after;
        // a session opened as a new tab gets no such second chance and was
        // born 0x0 (R1a-test, finding 3).
        struct winsize born{};
        born.ws_col = (unsigned short)(size.columns);
        born.ws_row = (unsigned short)(size.rows);
        born.ws_xpixel = (unsigned short)(size.pixelWidth);
        born.ws_ypixel = (unsigned short)(size.pixelHeight);
        int pair[2] = {-1, -1};
        if (openpty(&pair[0], &pair[1], nullptr, nullptr, &born) != 0) {
            raiseError(StringView(u8"openpty for a new session"));
        }
        fd = pair[0];
        peers.pushBack(pair[1]);
    }
    TestPty* const handle = owner.make<TestPty>(composer, owner, fd);
    if (fromOutside) {
        // After the handle rather than before the fork, and no race in
        // it unlike production: nothing reads this end of the pair until
        // the CHILD command, which forks a child much later.
        handle->resize(size);
    }
    handle->start();
    handles.pushBack(handle);
    return handle;
}

TestUtf8Decoder::TestUtf8Decoder() {
}

void TestUtf8Decoder::push(StringView input, Vector<u32>& result) {
    for (const u8 ch : input) {
        if (ch < 0x80) {
            if (decoder.checkPrematureEOS()) {
                output.pushBack(decoder.getUnicode());
            }
            output.pushBack(ch);
        } else {
            for (int completed = decoder.pushByte(ch); completed > 0; --completed) {
                output.pushBack(decoder.getUnicode());
            }
        }
    }
    result.clear();
    result.xchg(output);
}

void TestUtf8Decoder::flush(Vector<u32>& result) {
    if (decoder.checkPrematureEOS()) {
        output.pushBack(decoder.getUnicode());
    }
    result.clear();
    result.xchg(output);
}

void TestUtf8Decoder::reset() {
    output.clear();
    decoder.reset();
}

void FailFontChange::arm() {
    armed = true;
}

void FailFontChange::onListen(void*) {
    if (!armed) {
        return;
    }
    armed = false;
    Errno(EIO).raise(StringView(u8"injected font replacement failure"));
}

size_t MemDropOffer::formats() const {
    return 1;
}

StringView MemDropOffer::format(size_t) const {
    return mime;
}

plt::DropOffer* MemDrop::what() {
    return &offer;
}

Input* MemDrop::read(StringView) {
    return new MemoryInput(payload.data(), payload.length());
}

DropDelivery::DropDelivery(plt::DropTarget* target_, StringView payload_, StringView mime_)
    : target(target_)
    , payload((const char*)(payload_.data()), payload_.length())
    , mime(mime_)
{
}

void DropDelivery::run() {
    MemDrop drop;
    drop.offer.mime = mime;
    drop.payload = StringView(payload);
    target->dropped(drop);
}

namespace {

    // The scheduler serving the control fiber; writeAll parks on it when
    // the nonblocking control socket fills.
    static plt::Scheduler* controlScheduler = nullptr;

    static void writeAll(int fd, StringView data) {
        size_t offset = 0;
        while (offset < data.length()) {
            const ssize_t count = write(fd, data.data() + offset, data.length() - offset);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && controlScheduler != nullptr && controlScheduler->current() != nullptr) {
                    if (!controlScheduler->awaitWritable(fd, 0)) {
                        raiseError(StringView(u8"test control write failed"));
                    }
                    continue;
                }
                raiseError(StringView(u8"test control write failed"));
            }
            offset += (size_t)(count);
        }
    }

    static void writeAll(int fd, const char* data) {
        writeAll(fd, StringView(data));
    }

    // Streams every part into one builder and writes the result; numbers
    // print decimally, HexOut prints its bytes hex-encoded.
    struct HexOut {
        StringView bytes;
    };

    static StringView hexview(StringView view) {
        return view;
    }

    static StringView hexview(const Buffer& buffer) {
        return StringView(buffer);
    }

    template <typename... Part>
    static void writeParts(int fd, const Part&... part) {
        StringBuilder text;
        const auto push = [&text](const auto& value) {
            if constexpr (requires { value.bytes; }) {
                static constexpr char digits[] = "0123456789abcdef";
                for (const u8 byte : value.bytes) {
                    const char pair[2] = {digits[byte >> 4], digits[byte & 15]};
                    text.append(pair, 2);
                }
            } else {
                text << value;
            }
        };
        (push(part), ...);
        writeAll(fd, StringView(text));
    }

    // Whitespace-separated argument scanning over one command line.
    struct ArgReader {
        const u8* at;
        const u8* end;

        explicit ArgReader(StringView text)
            : at(text.begin())
            , end(text.end())
        {
        }

        void skipSpace() {
            while (at != end && (*at == ' ' || *at == '\t')) {
                ++at;
            }
        }

        bool token(char* out, size_t capacity) {
            skipSpace();
            size_t length = 0;
            while (at != end && *at != ' ' && *at != '\t') {
                if (length + 1 >= capacity) {
                    return false;
                }
                out[length++] = (char)(*at++);
            }
            out[length] = '\0';
            return length != 0;
        }

        template <typename T>
        bool read(T& out) {
            char word[64];
            i64 value = 0;
            if (!token(word, sizeof(word)) || !parseI64(StringView(word), value)) {
                return false;
            }
            out = (T)(value);
            return true;
        }

        bool read(double& out) {
            char word[64];
            return token(word, sizeof(word)) && parseF64(StringView(word), out);
        }
    };

    static bool readLine(plt::Scheduler* scheduler, int fd, Buffer& buffered, Buffer& line) {
        while (true) {
            const u8* const base = (const u8*)(buffered.data());
            const void* const found = memchr(base, '\n', buffered.used());
            if (found != nullptr) {
                const size_t newline = (const u8*)(found)-base;
                line.reset();
                line.append(base, newline);
                const size_t remaining = buffered.used() - newline - 1;
                memmove(buffered.mutData(), base + newline + 1, remaining);
                buffered.seekAbsolute(remaining);
                return true;
            }

            char chunk[4096];
            const ssize_t count = read(fd, chunk, sizeof(chunk));
            if (count == 0) {
                return false;
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && scheduler != nullptr && scheduler->current() != nullptr) {
                    if (!scheduler->awaitReadable(fd, 0)) {
                        return false;
                    }
                    continue;
                }
                raiseError(StringView(u8"test control read failed"));
            }
            buffered.append(chunk, (size_t)(count));
        }
    }

    static bool startsWith(StringView text, StringView prefix) {
        return text.length() >= prefix.length() && StringView(text.data(), prefix.length()) == prefix;
    }

    static StringView tail(StringView text, size_t offset) {
        return StringView(text.data() + offset, text.length() - offset);
    }

    static u8 hexDigit(char ch) {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        raiseError(StringView(u8"invalid hex input"));
    }

    static void decodeHex(StringView input, Buffer& output) {
        if (input.length() % 2) {
            raiseError(StringView(u8"odd-length hex input"));
        }
        output.reset();
        for (size_t k = 0; k < input.length(); k += 2) {
            const u8 byte = (u8)((hexDigit(input[k]) << 4) | hexDigit(input[k + 1]));
            output.append(&byte, 1);
        }
    }

    static u8 hexDigit(u8 ch) {
        if (ch >= u8'0' && ch <= u8'9') {
            return ch - u8'0';
        }
        if (ch >= u8'a' && ch <= u8'f') {
            return ch - u8'a' + 10;
        }
        if (ch >= u8'A' && ch <= u8'F') {
            return ch - u8'A' + 10;
        }
        Errno(EINVAL).raise(StringView(u8"invalid hex input"));
    }

    static void appendHex(StringBuilder& output, StringView input) {
        for (const u8 byte : input) {
            output << Hex{byte, 2};
        }
    }

    // FONT_LOAD/RENDER_IMAGE requests carry a NUL-separated font list; the
    // views alias the request string.
    static void splitFontNames(StringView request, Vector<StringView>& names) {
        names.clear();
        size_t begin = 0;
        while (begin <= request.length()) {
            const void* found = begin < request.length() ? memchr(request.data() + begin, '\0', request.length() - begin) : nullptr;
            const size_t end = found != nullptr ? (size_t)((const u8*)(found)-request.data()) : request.length();
            if (end != begin) {
                names.pushBack(StringView(request.data() + begin, end - begin));
            }
            begin = end + 1;
        }
        if (names.empty()) {
            raiseError(StringView(u8"empty font list"));
        }
    }

    struct TraceEvent {
        explicit TraceEvent(const char* type_)
            : type(type_)
        {
        }

        const char* type;
        Buffer data;
    };

    // The trace both records parser events and, as the VtermTraceFactory
    // handed to Vterm::create, receives the terminal's TestApi.
    struct VtermTraceImpl final: public VtermTrace, public VtermTraceFactory {
        explicit VtermTraceImpl(ObjPool& owner)
            : eventStore(&owner)
        {
        }

        ~VtermTraceImpl();

        VtermTrace* construct(TestApi* testApi) override;

        void text(const u8* data, size_t size) override;
        void control(u8 ch) override;
        void escapeBegin() override;
        void escapeByte(u8 ch) override;
        void escapeEnd() override;
        void escapeCancel() override;
        void csi(u8 finalByte, StringView privatePrefix, StringView intermediates, const u32* parameters, const unsigned char* separators, size_t parameterCount, bool hadParameters) override;
        void stringBegin(VtermTraceString type) override;
        void stringData(const u8* data, size_t size) override;
        void stringEnd() override;
        void stringCancel() override;
        void drain(Buffer& out) override;
        void clear() override;
        void osc(u32 command, StringView payload) override;
        void bell() override;
        void leds(u8 state) override;
        void cwd(StringView path) override;
        void notify(StringView id, StringView title, StringView body, bool close) override;
        void progress(u32 state, u32 percent) override;
        void windowOperation(u32 operation, u32 first, u32 second) override;
        void drainActions(Buffer& out) override;
        StringView currentCwd() const override;

        static VtermTraceImpl* create(Composer& composer);

        constexpr static size_t noEvent = SIZE_MAX;

        size_t add(const char* type);
        void erase(size_t& index);
        void appendHex(StringView input);
        static const char* stringName(VtermTraceString type);

        TestApi* testApi = nullptr;
        ObjList<TraceEvent> eventStore;
        Vector<TraceEvent*> events;
        size_t escapeEvent = noEvent;
        size_t stringEvent = noEvent;
        StringBuilder actions;
        Buffer cwdPath;
    };

    struct SessionTraceFactory final: public VtermTraceFactory {
        explicit SessionTraceFactory(Composer& composer_)
            : composer(composer_)
        {
        }

        VtermTrace* construct(TestApi* testApi) override {
            VtermTraceImpl* const trace = VtermTraceImpl::create(composer);
            traces.pushBack(trace);
            return trace->construct(testApi);
        }

        Composer& composer;
        Vector<VtermTraceImpl*> traces;
    };

    struct TestClipboard;

    struct TestClipboardFacet final: public plt::Clipboard {
        Input* read() override;
        Output* write() override;

        TestClipboard* owner = nullptr;
        bool primary = false;
    };

    // Snapshot stream over one selection buffer; plain delete releases it.
    // readChunk lets protocol tests force boundaries inside UTF-8 and
    // bracketed-paste sequences; zero leaves reads limited only by the caller.
    struct TestClipboardInput final: public Input {
        TestClipboardInput(SmallObjAllocator* allocator, const Buffer& content, size_t readChunk);

        void operator delete(TestClipboardInput* input, std::destroying_delete_t) noexcept;

        size_t readImpl(void* data, size_t len) override;

        SmallObjAllocator* allocator;
        Buffer content;
        size_t offset = 0;
        size_t readChunk;
    };

    struct TestClipboardOutput final: public Output {
        TestClipboardOutput(SmallObjAllocator* allocator, TestClipboard* owner, bool primary);

        void operator delete(TestClipboardOutput* output, std::destroying_delete_t) noexcept;

        size_t writeImpl(const void* data, size_t size) override;
        void finishImpl() override;

        SmallObjAllocator* allocator;
        TestClipboard* owner;
        Buffer accumulated;
        bool primary;
        bool finished = false;
    };

    struct TestClipboard {
        explicit TestClipboard(Composer& composer);

        void writePrimary(StringView content);
        void writeClipboard(StringView content);

        Composer& composer;
        TestClipboardFacet primaryFacet;
        TestClipboardFacet systemFacet;
        Buffer primary;
        Buffer system;
        u64 generation = 0;
        size_t readChunk = 0;
    };

    struct TestTerminal {
        TestTerminal(Composer& composer, Vterm& terminal, TestApi& testApi, TestPty& pty, ReferenceRenderer& renderer, plt::WindowHeadless& window);

        void feedPtyOutput(const u8* data, size_t size);
        void feedPtyOutput(const Buffer& chunkArena, const Vector<u32>& chunkEnds);
        void update();
        void redraw();
        bool repaint();
        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd);
        void resize(u16 width, u16 height);
        void sendKey(InputKey key, VtModifier modifiers = VtModifier::none);
        void sendCharacter(u8 byte, VtModifier modifiers = VtModifier::none);
        void sendBytes(StringView bytes, bool userInput = false);
        void writeKittyKey(InputKey key, u16 modifiers, VtermKeyEventType event);
        void writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event);
        bool readPty();
        void drainPty();
        bool servicePty(bool readable, bool writable);
        bool outputDrained();
        void kickOutput();
        MouseTrackingState getMouseTrackingState();
        u8 getKittyKeyboardFlags();
        bool getScreenReverseVideo();
        u8 getLedState();
        bool getReverseWrapMode();
        bool getNationalReplacementMode();
        bool getAlternateScroll();
        bool getAnsiMode(u32 mode);
        bool getPrivateMode(u32 mode);
        bool getTabStop(u16 column);
        void setWrapped(u16 row);
        u8 getRowSemantic(i32 row);
        u8 getSemanticClick();
        bool cursorIsAtPrompt();
        bool getPendingWrap();
        TerminalCursor::Style getCursorStyle();
        TerminalPen getPenState();
        RectangleOrigin getRectangleOrigin();
        size_t getHyperlinkCount();
        void getHyperlink(int x, int y, Buffer& out);
        bool mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY);
        void setLocatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons = 0);
        void reportLocatorButton(u8 button, bool pressed);
        void mouseWheelUp(u16 count = 1);
        void mouseWheelDown(u16 count = 1);
        void pageUp();
        void pageDown();
        void selectStart(int x, int y, bool cycle);
        void selectExtend(int x, int y, bool cycle);
        void selectUpdate(int x, int y);
        bool selectFinish(Buffer& selection);
        void selectRectangularModeToggle();
        void pasteSelection(StringView selection);
        void setHasFocus(bool focused);
        bool expireSynchronizedOutput(bool force = false);
        bool advanceAnimation(bool force = false);
        bool advanceSelectionAutoscroll();
        void allText(Buffer& out) const;

        Composer& composer;
        Vterm& terminal;
        TestApi& testApi;
        TestPty& pty;
        ReferenceRenderer& renderer;
        plt::WindowHeadless& window;
        u8 ptyInputBuffer[64 * 1024];
        bool consumePty(bool drain);
        bool present();
    };

    // One harness kit per opened session. The terminal-bound control
    // commands resolve theirs through the pty of the session the window
    // shows, so a test switches by chord (or NEW_SESSION) and then pokes
    // whichever session is active; the indexed commands reach background
    // sessions the way their own shells would.
    struct SessionKit {
        TestTerminal* terminal = nullptr;
        TestPty* pty = nullptr;
        VtermTraceImpl* trace = nullptr;
    };

    // Test instrumentation follows the same public action lists as the
    // production owner. Registered after SessionSet, these observers update
    // only the harness's indexed view once the real operation has completed.
    struct TestSessionAction final: public Listener {
        explicit TestSessionAction(std::function<void()> callback_)
            : callback(callback_)
        {
        }

        void onListen(void*) override {
            callback();
        }

        std::function<void()> callback;
    };

    void publishSessionAction(IntrusiveList& listeners) {
        for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }
    }

    template <typename Cell>
    static unsigned cellUnderline(const Cell& cell) {
        return cell.underline;
    }

    template <>
    unsigned cellUnderline(const TerminalCell& cell) {
        return cell.underlined();
    }

    template <typename Cell>
    static unsigned cellFlags(const Cell& cell, u8 lineAttribute) {
        return (cell.dwidth << 0) | (cell.dwidth_cont << 1) | (cell.bold << 2) | (cell.italic << 3) | (cellUnderline(cell) << 4) | (cell.inverse << 5) | (cell.wrap << 6) | (cell.faint << 7) | (cell.blink << 8) | (cell.conceal << 9) | (cell.strike << 10) | (cell.overline << 11) | (cell.underline_style << 12) | ((cell.protected_char != 0) << 15) | (lineAttribute << 16) | (cell.drawn << 18);
    }

    static unsigned cellFlags(const TerminalCell& cell) {
        return cellFlags(cell, 0);
    }

}

VtermTraceImpl::~VtermTraceImpl() {
    clear();
}

VtermTrace* VtermTraceImpl::construct(TestApi* testApi_) {
    testApi = testApi_;
    return this;
}

VtermTraceImpl* VtermTraceImpl::create(Composer& composer) {
    return composer.pool->make<VtermTraceImpl>(*composer.pool);
}

size_t VtermTraceImpl::add(const char* type) {
    events.pushBack(eventStore.make(type));
    return events.length() - 1;
}

void VtermTraceImpl::erase(size_t& index) {
    if (index == noEvent) {
        return;
    }
    eventStore.release(events[index]);
    memmove(events.mutData() + index, events.data() + index + 1, (events.length() - index - 1) * sizeof(TraceEvent*));
    events.popBack();
    if (escapeEvent != noEvent && escapeEvent > index) {
        --escapeEvent;
    }
    if (stringEvent != noEvent && stringEvent > index) {
        --stringEvent;
    }
    index = noEvent;
}

const char* VtermTraceImpl::stringName(VtermTraceString type) {
    switch (type) {
        case VtermTraceString::Osc:
            return "osc";
        case VtermTraceString::Dcs:
            return "dcs";
        case VtermTraceString::Apc:
            return "apc";
        case VtermTraceString::Pm:
            return "pm";
        case VtermTraceString::Sos:
            return "sos";
    }
    return "string";
}

void VtermTraceImpl::text(const u8* data, size_t size) {
    if (!size) {
        return;
    }
    if (events.empty() || StringView(events.back()->type) != StringView(u8"text")) {
        add("text");
    }
    events.back()->data.append(data, size);
}

void VtermTraceImpl::control(u8 ch) {
    const size_t index = add("control");
    events[index]->data.append(&ch, 1);
}

void VtermTraceImpl::escapeBegin() {
    escapeCancel();
    escapeEvent = add("escape");
}

void VtermTraceImpl::escapeByte(u8 ch) {
    if (escapeEvent != noEvent) {
        events[escapeEvent]->data.append(&ch, 1);
    }
}

void VtermTraceImpl::escapeEnd() {
    escapeEvent = noEvent;
}

void VtermTraceImpl::escapeCancel() {
    erase(escapeEvent);
}

void VtermTraceImpl::csi(u8 finalByte, StringView privatePrefix, StringView intermediates, const u32* parameters, const unsigned char* separators, size_t parameterCount, bool hadParameters) {
    escapeCancel();
    const size_t index = add("csi");
    StringBuilder sequence;
    sequence << privatePrefix;
    if (hadParameters) {
        for (size_t k = 0; k < parameterCount; ++k) {
            if (k) {
                const char separator = (char)(separators[k]);
                sequence.append(&separator, 1);
            }
            sequence << parameters[k];
        }
    }
    sequence << intermediates;
    const char final = (char)(finalByte);
    sequence.append(&final, 1);
    events[index]->data.xchg(sequence);
}

void VtermTraceImpl::stringBegin(VtermTraceString type) {
    escapeCancel();
    stringCancel();
    stringEvent = add(stringName(type));
}

void VtermTraceImpl::stringData(const u8* data, size_t size) {
    if (stringEvent != noEvent) {
        events[stringEvent]->data.append(data, size);
    }
}

void VtermTraceImpl::stringEnd() {
    stringEvent = noEvent;
}

void VtermTraceImpl::stringCancel() {
    erase(stringEvent);
}

void VtermTraceImpl::drain(Buffer& out) {
    StringBuilder result;
    size_t count = events.length();
    if (escapeEvent != noEvent) {
        count = min(count, escapeEvent);
    }
    if (stringEvent != noEvent) {
        count = min(count, stringEvent);
    }
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t k = 0; k < count; ++k) {
        result << StringView(events[k]->type) << StringView(u8" ");
        for (const u8 byte : StringView(events[k]->data)) {
            const char pair[2] = {digits[byte >> 4], digits[byte & 15]};
            result.append(pair, 2);
        }
        result.append("\n", 1);
        eventStore.release(events[k]);
    }
    memmove(events.mutData(), events.data() + count, (events.length() - count) * sizeof(TraceEvent*));
    for (size_t drained = count; drained > 0; --drained) {
        events.popBack();
    }
    if (escapeEvent != noEvent) {
        escapeEvent -= count;
    }
    if (stringEvent != noEvent) {
        stringEvent -= count;
    }
    out.xchg(result);
}

void VtermTraceImpl::clear() {
    for (TraceEvent* event : events) {
        eventStore.release(event);
    }
    events.clear();
    escapeEvent = noEvent;
    stringEvent = noEvent;
}

void VtermTraceImpl::appendHex(StringView input) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const u8 ch : input) {
        const char pair[2] = {digits[ch >> 4], digits[ch & 15]};
        actions.append(pair, 2);
    }
}

void VtermTraceImpl::osc(u32 command, StringView payload) {
    actions << StringView(u8"OSC ") << command << StringView(u8" ");
    appendHex(payload);
    actions << StringView(u8"\n");
}

void VtermTraceImpl::bell() {
    actions << StringView(u8"BELL\n");
}

void VtermTraceImpl::leds(u8 state) {
    actions << StringView(u8"LEDS ") << (u64)(state) << StringView(u8"\n");
}

void VtermTraceImpl::cwd(StringView path) {
    cwdPath.reset();
    cwdPath.append(path.data(), path.length());
}

void VtermTraceImpl::notify(StringView id, StringView title, StringView body, bool close) {
    actions << (close ? StringView(u8"NOTIFY_CLOSE ") : StringView(u8"NOTIFY "));
    appendHex(id);
    if (!close) {
        actions << StringView(u8" ");
        appendHex(title);
        actions << StringView(u8" ");
        appendHex(body);
    }
    actions << StringView(u8"\n");
}

void VtermTraceImpl::progress(u32 state, u32 percent) {
    actions << StringView(u8"PROGRESS ") << state << StringView(u8" ") << percent << StringView(u8"\n");
}

void VtermTraceImpl::windowOperation(u32 operation, u32 first, u32 second) {
    actions << StringView(u8"WINDOW ") << operation << StringView(u8" ") << first << StringView(u8" ") << second << StringView(u8"\n");
}

void VtermTraceImpl::drainActions(Buffer& out) {
    out.reset();
    out.xchg(actions);
}

StringView VtermTraceImpl::currentCwd() const {
    return StringView(cwdPath);
}

TestClipboardInput::TestClipboardInput(SmallObjAllocator* allocator_, const Buffer& content_, size_t readChunk_)
    : allocator(allocator_)
    , readChunk(readChunk_)
{
    content.append(content_.data(), content_.length());
}

void TestClipboardInput::operator delete(TestClipboardInput* input, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = input->allocator;
    owner->release(input);
}

size_t TestClipboardInput::readImpl(void* data, size_t len) {
    if (readChunk != 0 && len > readChunk) {
        len = readChunk;
    }
    const size_t count = len < content.length() - offset ? len : content.length() - offset;
    memcpy(data, (const u8*)(content.data()) + offset, count);
    offset += count;
    return count;
}

TestClipboardOutput::TestClipboardOutput(SmallObjAllocator* allocator_, TestClipboard* owner_, bool primary_)
    : allocator(allocator_)
    , owner(owner_)
    , primary(primary_)
{
}

void TestClipboardOutput::operator delete(TestClipboardOutput* output, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = output->allocator;
    owner->release(output);
}

size_t TestClipboardOutput::writeImpl(const void* data, size_t size) {
    accumulated.append(data, size);
    return size;
}

void TestClipboardOutput::finishImpl() {
    if (finished) {
        return;
    }
    finished = true;
    if (primary) {
        owner->writePrimary(StringView(accumulated));
    } else {
        owner->writeClipboard(StringView(accumulated));
    }
}

Input* TestClipboardFacet::read() {
    return owner->composer.smallObjects->make<TestClipboardInput>(owner->composer.smallObjects, primary ? owner->primary : owner->system, owner->readChunk);
}

Output* TestClipboardFacet::write() {
    return owner->composer.smallObjects->make<TestClipboardOutput>(owner->composer.smallObjects, owner, primary);
}

TestClipboard::TestClipboard(Composer& composer_)
    : composer(composer_)
{
    primaryFacet.owner = this;
    primaryFacet.primary = true;
    systemFacet.owner = this;
}

void TestClipboard::writePrimary(StringView content) {
    primary.reset();
    primary.append(content.data(), content.length());
    ++generation;
}

void TestClipboard::writeClipboard(StringView content) {
    system.reset();
    system.append(content.data(), content.length());
}

TestTerminal::TestTerminal(Composer& composer_, Vterm& terminal, TestApi& testApi, TestPty& pty, ReferenceRenderer& renderer_, plt::WindowHeadless& window_)
    : composer(composer_)
    , terminal(terminal)
    , testApi(testApi)
    , pty(pty)
    , renderer(renderer_)
    , window(window_)
{
}

bool TestTerminal::present() {
    while (window.framePending()) {
        if (!window.dispatchFrame()) {
            return false;
        }
    }
    return true;
}

void TestTerminal::feedPtyOutput(const u8* data, size_t size) {
    renderer.resetUpdateStats();
    terminal.feedPty(StringView(data, size));
    update();
}

void TestTerminal::feedPtyOutput(const Buffer& chunkArena, const Vector<u32>& chunkEnds) {
    renderer.resetUpdateStats();
    u32 begin = 0;
    for (const u32 end : chunkEnds) {
        terminal.feedPty(StringView((const u8*)(chunkArena.data()) + begin, end - begin));
        begin = end;
    }
    update();
}

void TestTerminal::update() {
    window.requestFrame();
    present();
}

void TestTerminal::redraw() {
    terminal.expose();
    update();
}

bool TestTerminal::repaint() {
    window.requestFrame();
    return present();
}

void TestTerminal::preedit(StringView text, i32 cursorBegin, i32 cursorEnd) {
    terminal.preedit(text, cursorBegin, cursorEnd);
    update();
}

void TestTerminal::resize(u16 width, u16 height) {
    window.requestResize(width, height);
    update();
}

void TestTerminal::allText(Buffer& out) const {
    Buffer& output = out;
    output.reset();
    output.grow((renderer.historyRows() + renderer.rows()) * ((size_t)(renderer.columns()) * 4 + 1));
    const auto appendCodepoint = [&](u32 codepoint) {
        Utf8Encoder::pushUnicode(codepoint, [&](u8 byte) {
            output.append(&byte, 1);
        });
    };
    for (i32 row = -(i32)(renderer.historyRows()); row < renderer.rows(); ++row) {
        size_t contentEnd = output.used();
        for (u16 column = 0; column < renderer.columns(); ++column) {
            const VtermTestCell value = testApi.logicalCell(row, column);
            const TerminalCell& cell = value.cell;
            if (cell.dwidth_cont) {
                continue;
            }
            if (value.graphemeSize != 0) {
                for (size_t index = 0; index < value.graphemeSize; ++index) {
                    appendCodepoint(value.grapheme[index]);
                }
            } else {
                appendCodepoint(cell.uc_pt == 0 ? ' ' : cell.uc_pt);
            }
            if (cell.drawn || (cell.uc_pt != 0 && cell.uc_pt != ' ') || value.graphemeSize != 0) {
                contentEnd = output.used();
            }
        }
        output.seekAbsolute(contentEnd);
        const u8 separator = 0;
        output.append(&separator, 1);
    }
}

void TestTerminal::sendKey(InputKey key, VtModifier modifiers) {
    testApi.key(key, modifiers);
    update();
}

void TestTerminal::sendCharacter(u8 byte, VtModifier modifiers) {
    testApi.character(byte, modifiers);
    update();
}

void TestTerminal::sendBytes(StringView bytes, bool userInput) {
    terminal.sendBytes(bytes, userInput);
    update();
}

void TestTerminal::writeKittyKey(InputKey key, u16 modifiers, VtermKeyEventType keyEvent) {
    testApi.kittyKey(key, modifiers, keyEvent);
    update();
}

void TestTerminal::writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType keyEvent) {
    testApi.kittyKey(key, shiftedKey, baseLayoutKey, modifiers, keyEvent);
    update();
}

bool TestTerminal::outputDrained() {
    return pty.outputDrained();
}

void TestTerminal::kickOutput() {
    pty.kickOutput();
}

bool TestTerminal::readPty() {
    return consumePty(false);
}

void TestTerminal::drainPty() {
    consumePty(true);
}

bool TestTerminal::consumePty(bool drain) {
    bool finished = false;
    while (true) {
        ssize_t count;
        do {
            count = pty.read(ptyInputBuffer, sizeof(ptyInputBuffer));
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            terminal.feedPty(StringView(ptyInputBuffer, count));
            if (drain) {
                continue;
            }
        } else if (count == 0 || (count < 0 && errno == EIO)) {
            finished = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            finished = true;
        }
        break;
    }
    window.requestFrame();
    present();
    return finished;
}

bool TestTerminal::servicePty(bool readable, bool writable) {
    if (writable) {
        kickOutput();
    }
    return readable && readPty();
}

MouseTrackingState TestTerminal::getMouseTrackingState() {
    return testApi.inspect().mouse;
}

u8 TestTerminal::getKittyKeyboardFlags() {
    return testApi.inspect().kittyKeyboardFlags;
}

bool TestTerminal::getAlternateScroll() {
    // Read through the terminal's own state accessor rather than the
    // test api, so this exercises what a client outside the terminal
    // actually sees.
    return terminal.state().alternateScroll;
}

bool TestTerminal::getScreenReverseVideo() {
    return testApi.inspect().screenReverseVideo;
}

u8 TestTerminal::getLedState() {
    return testApi.inspect().ledState;
}

bool TestTerminal::getReverseWrapMode() {
    return testApi.inspect().reverseWrapMode;
}

bool TestTerminal::getNationalReplacementMode() {
    return testApi.inspect().nationalReplacementMode;
}

bool TestTerminal::getAnsiMode(u32 mode) {
    return testApi.ansiMode(mode);
}

bool TestTerminal::getPrivateMode(u32 mode) {
    return testApi.privateMode(mode);
}

bool TestTerminal::getTabStop(u16 column) {
    return testApi.tabStop(column);
}

void TestTerminal::setWrapped(u16 row) {
    testApi.setWrapped(row);
    update();
}

u8 TestTerminal::getRowSemantic(i32 row) {
    return testApi.rowSemantic(row);
}

u8 TestTerminal::getSemanticClick() {
    return testApi.semanticClick();
}

bool TestTerminal::cursorIsAtPrompt() {
    return testApi.cursorIsAtPrompt();
}

bool TestTerminal::getPendingWrap() {
    return testApi.inspect().pendingWrap;
}

TerminalCursor::Style TestTerminal::getCursorStyle() {
    return testApi.inspect().cursorStyle;
}

TerminalPen TestTerminal::getPenState() {
    return testApi.inspect().pen;
}

RectangleOrigin TestTerminal::getRectangleOrigin() {
    return testApi.inspect().rectangleOrigin;
}

size_t TestTerminal::getHyperlinkCount() {
    return testApi.inspect().hyperlinkCount;
}

void TestTerminal::getHyperlink(int x, int y, Buffer& out) {
    const StringView result = testApi.hyperlinkAt(x, y);
    out.reset();
    out.append(result.data(), result.length());
}

bool TestTerminal::mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY) {
    const bool result = testApi.mouseHighlightRelease(endX, endY, mouseX, mouseY);
    update();
    return result;
}

void TestTerminal::setLocatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons) {
    testApi.locatorPosition(column, row, pixelX, pixelY, buttons);
    update();
}

void TestTerminal::reportLocatorButton(u8 button, bool pressed) {
    testApi.locatorButton(button, pressed);
    update();
}

void TestTerminal::mouseWheelUp(u16 count) {
    testApi.scrollUp(count);
    update();
}

void TestTerminal::mouseWheelDown(u16 count) {
    testApi.scrollDown(count);
    update();
}

void TestTerminal::pageUp() {
    testApi.pageUp();
    update();
}

void TestTerminal::pageDown() {
    testApi.pageDown();
    update();
}

void TestTerminal::selectStart(int x, int y, bool cycle) {
    testApi.selectionStart(x, y, cycle);
    update();
}

void TestTerminal::selectExtend(int x, int y, bool cycle) {
    testApi.selectionExtend(x, y, cycle);
    update();
}

void TestTerminal::selectUpdate(int x, int y) {
    testApi.selectionUpdate(x, y);
    update();
}

bool TestTerminal::selectFinish(Buffer& selection) {
    const VtermTextResult result = testApi.selectionFinish();
    selection.reset();
    selection.append(result.text.data(), result.text.length());
    update();
    return result.status;
}

void TestTerminal::selectRectangularModeToggle() {
    testApi.selectionRectangular();
    update();
}

void TestTerminal::pasteSelection(StringView selection) {
    testApi.paste(selection);
    update();
}

void TestTerminal::setHasFocus(bool focused) {
    composer.input->focus(focused);
    update();
}

bool TestTerminal::expireSynchronizedOutput(bool force) {
    const bool result = terminal.expireSynchronizedOutput(force);
    update();
    return result;
}

bool TestTerminal::advanceAnimation(bool force) {
    const bool result = terminal.advanceAnimation(force);
    update();
    return result;
}

bool TestTerminal::advanceSelectionAutoscroll() {
    const bool result = testApi.advanceSelectionAutoscroll();
    update();
    return result;
}

namespace {

    static void drainInput(int fd, Buffer& output) {
        output.reset();
        char chunk[4096];
        while (true) {
            const ssize_t count = read(fd, chunk, sizeof(chunk));
            if (count > 0) {
                output.append(chunk, (size_t)(count));
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (count < 0 && errno == EIO) {
                // macOS revokes the pty slave once the child session
                // leader exits; the undelivered queue died with it and
                // there is nothing left to drain.
                break;
            }
            if (count == 0) {
                break;
            }
            raiseError(StringView(u8"test PTY read failed"));
        }
    }

    static InputKey parseKey(const char* name) {
        struct KeyName {
            const char* name;
            InputKey key;
        };

        static const KeyName keys[] = {
            {"SPACE", InputKey::Space},
            {"RETURN", InputKey::Enter},
            {"BACKSPACE", InputKey::Backspace},
            {"TAB", InputKey::Tab},
            {"UP", InputKey::Up},
            {"DOWN", InputKey::Down},
            {"LEFT", InputKey::Left},
            {"RIGHT", InputKey::Right},
            {"INSERT", InputKey::Insert},
            {"DELETE", InputKey::Delete},
            {"HOME", InputKey::Home},
            {"END", InputKey::End},
            {"PAGE_UP", InputKey::PageUp},
            {"PAGE_DOWN", InputKey::PageDown},
            {"CLEAR", InputKey::Clear},
            {"F1", InputKey::F1},
            {"F2", InputKey::F2},
            {"F3", InputKey::F3},
            {"F4", InputKey::F4},
            {"F5", InputKey::F5},
            {"F6", InputKey::F6},
            {"F7", InputKey::F7},
            {"F8", InputKey::F8},
            {"F9", InputKey::F9},
            {"F10", InputKey::F10},
            {"F11", InputKey::F11},
            {"F12", InputKey::F12},
            {"F13", InputKey::F13},
            {"F14", InputKey::F14},
            {"F15", InputKey::F15},
            {"F16", InputKey::F16},
            {"F17", InputKey::F17},
            {"F18", InputKey::F18},
            {"F19", InputKey::F19},
            {"F20", InputKey::F20},
            {"F21", InputKey::F21},
            {"F22", InputKey::F22},
            {"F23", InputKey::F23},
            {"F24", InputKey::F24},
            {"F25", InputKey::F25},
            {"F26", InputKey::F26},
            {"F27", InputKey::F27},
            {"F28", InputKey::F28},
            {"F29", InputKey::F29},
            {"F30", InputKey::F30},
            {"F31", InputKey::F31},
            {"F32", InputKey::F32},
            {"F33", InputKey::F33},
            {"F34", InputKey::F34},
            {"F35", InputKey::F35},
            {"KP_F1", InputKey::KeypadF1},
            {"KP_F2", InputKey::KeypadF2},
            {"KP_F3", InputKey::KeypadF3},
            {"KP_F4", InputKey::KeypadF4},
            {"KP_PLUS", InputKey::KeypadAdd},
            {"KP_MINUS", InputKey::KeypadSubtract},
            {"KP_STAR", InputKey::KeypadMultiply},
            {"KP_SLASH", InputKey::KeypadDivide},
            {"KP_COMMA", InputKey::KeypadSeparator},
            {"KP_DOT", InputKey::KeypadDecimal},
            {"KP_EQUAL", InputKey::KeypadEqual},
            {"KP_TAB", InputKey::KeypadTab},
            {"KP_SPACE", InputKey::KeypadSpace},
            {"KP_ENTER", InputKey::KeypadEnter},
            {"KP_LEFT", InputKey::KeypadLeft},
            {"KP_RIGHT", InputKey::KeypadRight},
            {"KP_UP", InputKey::KeypadUp},
            {"KP_DOWN", InputKey::KeypadDown},
            {"KP_HOME", InputKey::KeypadHome},
            {"KP_END", InputKey::KeypadEnd},
            {"KP_PAGE_UP", InputKey::KeypadPageUp},
            {"KP_PAGE_DOWN", InputKey::KeypadPageDown},
            {"KP_INSERT", InputKey::KeypadInsert},
            {"KP_DELETE", InputKey::KeypadDelete},
            {"KP_BEGIN", InputKey::KeypadBegin},
            {"KP_0", InputKey::Keypad0},
            {"KP_1", InputKey::Keypad1},
            {"KP_2", InputKey::Keypad2},
            {"KP_3", InputKey::Keypad3},
            {"KP_4", InputKey::Keypad4},
            {"KP_5", InputKey::Keypad5},
            {"KP_6", InputKey::Keypad6},
            {"KP_7", InputKey::Keypad7},
            {"KP_8", InputKey::Keypad8},
            {"KP_9", InputKey::Keypad9},
            {"CAPS_LOCK", InputKey::CapsLock},
            {"SCROLL_LOCK", InputKey::ScrollLock},
            {"NUM_LOCK", InputKey::NumLock},
            {"PRINT", InputKey::PrintScreen},
            {"PAUSE", InputKey::Pause},
            {"MENU", InputKey::Menu},
            {"LEFT_SHIFT", InputKey::LeftShift},
            {"LEFT_CONTROL", InputKey::LeftControl},
            {"LEFT_ALT", InputKey::LeftAlt},
            {"LEFT_SUPER", InputKey::LeftSuper},
            {"RIGHT_SHIFT", InputKey::RightShift},
            {"RIGHT_CONTROL", InputKey::RightControl},
            {"RIGHT_ALT", InputKey::RightAlt},
            {"RIGHT_SUPER", InputKey::RightSuper},
            {"MEDIA_PLAY", InputKey::MediaPlay},
            {"MEDIA_PAUSE", InputKey::MediaPause},
            {"MEDIA_PLAY_PAUSE", InputKey::MediaPlayPause},
            {"MEDIA_REVERSE", InputKey::MediaReverse},
            {"MEDIA_STOP", InputKey::MediaStop},
            {"MEDIA_FAST_FORWARD", InputKey::MediaFastForward},
            {"MEDIA_REWIND", InputKey::MediaRewind},
            {"MEDIA_TRACK_NEXT", InputKey::MediaTrackNext},
            {"MEDIA_TRACK_PREVIOUS", InputKey::MediaTrackPrevious},
            {"MEDIA_RECORD", InputKey::MediaRecord},
            {"VOLUME_DOWN", InputKey::VolumeDown},
            {"VOLUME_UP", InputKey::VolumeUp},
            {"VOLUME_MUTE", InputKey::VolumeMute},
        };
        for (const KeyName& entry : keys) {
            if (StringView(entry.name) == StringView(name)) {
                return entry.key;
            }
        }
        raiseError(StringView(u8"unknown key"));
    }
}

int runTestMode(Composer& composer, TestInput& input, plt::WindowEvents& events, plt::FrameCallback& frame, int controlFd, int argc, char* argv[]) {
    int io[2];
    if (openpty(&io[0], &io[1], nullptr, nullptr, nullptr) < 0) {
        raiseError(StringView(u8"test openpty failed"));
    }
    // Terminal-side descriptors must not leak into spawned children: a child
    // holding the control socket or the pty pair alive wedges the harness
    // when st_test itself dies.
    if (fcntl(controlFd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(io[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(io[1], F_SETFD, FD_CLOEXEC) < 0) {
        raiseError(StringView(u8"test FD_CLOEXEC setup failed"));
    }
    termios childTtyAttrs;
    if (tcgetattr(io[1], &childTtyAttrs) < 0) {
        close(io[0]);
        close(io[1]);
        raiseError(StringView(u8"test tcgetattr failed"));
    }
    termios ttyAttrs = childTtyAttrs;
    cfmakeraw(&ttyAttrs);
    if (tcsetattr(io[1], TCSANOW, &ttyAttrs) < 0) {
        close(io[0]);
        close(io[1]);
        raiseError(StringView(u8"test tcsetattr failed"));
    }
    const int flags = fcntl(io[1], F_GETFL, 0);
    if (flags < 0 || fcntl(io[1], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(io[0]);
        close(io[1]);
        raiseError(StringView(u8"test socket setup failed"));
    }

    {
        unsigned glyphWidth = 1;
        unsigned glyphHeight = 1;
        if (const char* geometry = getenv("SHITTY_TEST_GLYPH")) {
            StringView widthText;
            StringView heightText;
            u64 width = 0;
            u64 height = 0;
            const bool valid = StringView(geometry).split('x', widthText, heightText) && parseU64(widthText, width) && parseU64(heightText, height);
            if (!valid || width == 0 || height == 0) {
                raiseError(StringView(u8"invalid test glyph geometry"));
            }
            glyphWidth = (unsigned)(width);
            glyphHeight = (unsigned)(height);
        }
        composer.geometry.setCellPixelSize(glyphWidth, glyphHeight);
    }
    auto* const testFonts = composer.pool->make<TestFontpack>(composer);
    composer.fonts = testFonts;
    composer.configChangedListeners.pushBack(testFonts);
    const Insets insets = composer.contentInsets();
    const u16 width = (u16)(gridPixelWidth(composer.opts->nCols, insets, composer.geometry.cellPixelWidth));
    const u16 height = (u16)(gridPixelHeight(composer.opts->nRows, insets, composer.geometry.cellPixelHeight));
    composer.platform = plt::createHeadlessPlatform(*composer.pool);
    composer.config->start();
    STD_DEFER {
        composer.config->stop();
    };
    composer.window = composer.platform->createWindow(
        *composer.pool,
        {
            .title = StringView(composer.vtConfig.config->title),
            .width = width,
            .height = height,
            .decorations = !composer.opts->noDecorations,
            .input = composer.input,
            .events = &events,
            .frame = &frame,
        }
    );
    composer.installVtHost();
    auto& window = static_cast<plt::WindowHeadless&>(*composer.window);
    openDebugTrace(composer);
    // The same startup request the interactive run makes; the first
    // dispatched frame then carries the grown window into the grid.
    applyStartupWindowState(composer);
    composer.resize(width, height);
    LaunchCommand testLaunch;
    composer.launch = &testLaunch;
    TestPtyFactory ptyFactory(composer, io[0]);
    composer.pty = &ptyFactory;
    TestClipboard clipboard(composer);
    window.setClipboards(clipboard.primaryFacet, clipboard.systemFacet);
    composer.shaper = SpanShaper::create(composer, *composer.pool);
    composer.rendererPool = ObjPool::fromMemory();
    composer.renderer = Renderer::create(composer, *composer.rendererPool, window.renderContext());
    auto& renderer = static_cast<ReferenceRenderer&>(*composer.renderer);
    // The GPU renderer this platform builds, drawing the harness's own
    // frames beside the reference renderer so their pixels can be
    // compared. Which backend it is follows the build, not the test:
    // Vulkan over a headless surface on Linux, Metal into a texture on
    // macOS. The control commands keep their VULKAN_ names because
    // tst/harness.py speaks them and that file belongs to no task here.
    Renderer* gpuShadow = nullptr;
#if defined(HAVE_VULKAN_WAYLAND) || defined(HAVE_METAL_RENDERER)
    if (getenv("SHITTY_TEST_VULKAN") != nullptr) {
        const plt::RenderContext headlessGpu{plt::RenderBackend::Headless, nullptr, nullptr};
        try {
    #if defined(HAVE_VULKAN_WAYLAND)
            gpuShadow = createVulkanRenderer(composer, *composer.rendererPool, headlessGpu);
    #else
            gpuShadow = createMetalRenderer(composer, *composer.rendererPool, headlessGpu);
    #endif
        } catch (Exception& error) {
            const StringView description = error.description();
            sysE << StringView(u8"shitty: gpu shadow unavailable: ") << description << endL;
        }
        if (gpuShadow != nullptr) {
            composer.renderer = composer.rendererPool->make<MirrorRenderer>(&renderer, gpuShadow);
            testFonts->armMissThrows(*composer.pool);
        }
    }
#endif
    SessionTraceFactory traceFactory(composer);
    composer.vtermTraceFactory = &traceFactory;
    SessionSet* const sessions = SessionSet::create(composer);
    TestPty& terminalPty = *ptyFactory.handles.back();
    VtermTraceImpl& vtermTrace = *traceFactory.traces.back();
    Vterm& vterm = *sessions->activeTerminal();
    {
        Buffer discardedActions;
        vtermTrace.drainActions(discardedActions);
    }
    TestApi& testApi = *vtermTrace.testApi;
    renderer.attach(testApi);
    window.requestFrame();
    window.dispatchFrame();
    TestTerminal terminal(composer, vterm, testApi, terminalPty, renderer, window);
    Vector<SessionKit> sessionKits;
    sessionKits.pushBack({&terminal, &terminalPty, &vtermTrace});
    const auto kitFor = [&](Vterm* terminal_) -> SessionKit& {
        for (size_t at = 0; at < sessionKits.length(); ++at) {
            if (&sessionKits[at].terminal->terminal == terminal_) {
                return sessionKits.mut(at);
            }
        }
        raiseError(StringView(u8"no harness kit for this session"));
    };
    // A pane index is a position in SessionSetImpl::visiblePanes(), from
    // zero: that call walks the active tab's layout, so index 0 is the
    // first pane in visual order and a vertical split puts the new pane
    // at index 1. Every pane test leans on that order, so it is read
    // from the layout on each command rather than kept as a list of our
    // own that a split or a close could leave stale.
    const auto visiblePane = [&](u32 index) -> SessionPane {
        Vector<SessionPane> panes;
        sessions->visiblePanes(panes);
        if (index >= panes.length()) {
            raiseError(StringView(u8"no such pane"));
        }
        return panes[index];
    };
    const auto activeKitIndex = [&]() -> size_t {
        Vterm* const activeTerminal = sessions->activeTerminal();
        for (size_t at = 0; at < sessionKits.length(); ++at) {
            if (&sessionKits[at].terminal->terminal == activeTerminal) {
                return at;
            }
        }
        raiseError(StringView(u8"active session has no harness kit"));
    };
    // R8-sec 2.2: which kit the window is showing, kept as an index and
    // never as a Vterm*. This listener chain runs *after* SessionSet's own
    // close listener - closeTabAction is registered in SessionSet::create()
    // and publish() walks from the head - so by the time trackClosedSession
    // looks, the terminal it wants has been retired and the reaper that
    // wakeReaper() ran inside that close may already have freed it.
    // Comparing a freed pointer is undefined behaviour even where it does
    // not fault; an index is not a pointer and has nothing to compare.
    //
    // Unlike activeKitIndex() above, this does not raise when nothing
    // matches: closing the last tab leaves the window naming a terminal
    // whose kit has just gone, and that is the window on its way out
    // rather than an error.
    const auto trackedKitOfActive = [&]() -> size_t {
        Vterm* const active = sessions->activeTerminal();
        for (size_t at = 0; at < sessionKits.length(); ++at) {
            if (&sessionKits[at].terminal->terminal == active) {
                return at;
            }
        }
        return sessionKits.length();
    };
    size_t trackedActiveKit = trackedKitOfActive();
    // A session is born in two ways and only one of them publishes
    // newTabListeners: SessionSet::splitFocused() opens a session too,
    // and announces it through composer.sessionsChangedListeners at the
    // end. So adoption listens there as well, and because that list also
    // fires for a focus move, a title and a close, it adopts by absence
    // rather than by count: the session the window now shows either has
    // a kit already or gets one built from the pty and the trace the
    // factories have just appended.
    //
    // trackedActiveKit moves only when a kit is actually built. A close
    // publishes here too, and trackClosedSession below needs that index
    // to still name the kit that was in front when the close began.
    const auto adoptActiveSession = [&]() {
        Vterm* const active = sessions->activeTerminal();
        for (size_t at = 0; at < sessionKits.length(); ++at) {
            if (&sessionKits[at].terminal->terminal == active) {
                return;
            }
        }
        TestPty* const extraPty = ptyFactory.handles.back();
        VtermTraceImpl* const extraTrace = traceFactory.traces.back();
        {
            Buffer discardedActions;
            extraTrace->drainActions(discardedActions);
        }
        sessionKits.pushBack({composer.pool->make<TestTerminal>(composer, *active, *extraTrace->testApi, *extraPty, renderer, window), extraPty, extraTrace});
        trackedActiveKit = sessionKits.length() - 1;
    };
    // Still registered for the tab action it was written for: a new tab
    // owes its kit to opening the tab, not to whatever activate()
    // happens to publish on the way. Idempotent, so both routes may run.
    TestSessionAction trackNewSession(adoptActiveSession);
    TestSessionAction trackSplitSession(adoptActiveSession);
    TestSessionAction trackClosedSession([&]() {
        // The kit that was in front when the close began, named by where
        // it sits rather than by what it points at.
        const size_t closed = trackedActiveKit;
        if (closed >= sessionKits.length()) {
            raiseError(StringView(u8"closed session has no harness kit"));
        }
        for (size_t at = closed; at + 1 < sessionKits.length(); ++at) {
            sessionKits.mut(at) = sessionKits[at + 1];
        }
        sessionKits.popBack();
        trackedActiveKit = trackedKitOfActive();
    });
    const auto paneKit = [&](u32 index) -> SessionKit& {
        return kitFor(visiblePane(index).terminal);
    };
    // A pane's grid, read off its own pty: Vterm keeps TIOCSWINSZ in step
    // with the grid it reflows to, so the pane's child and the harness
    // get their columns and rows from one number. WINSIZE_PANE reports
    // it; SCREEN_TEXT_PANE walks the screen by it.
    const auto paneWinsize = [&](const SessionKit& kit) -> winsize {
        winsize size{};
        if (ioctl(kit.pty->fd_, TIOCGWINSZ, &size) < 0) {
            raiseError(StringView(u8"test TIOCGWINSZ failed"));
        }
        return size;
    };
    // SCREEN_TEXT answers from the renderer's retained cells, which
    // belong to whichever pane it drew last (render_reference.cpp) - one
    // window, one answer. A per-pane answer cannot come from there, so
    // this reads the pane's own screen through its TestApi in the same
    // shape: printable ASCII, blanks kept, one newline per row.
    const auto paneScreenText = [&](const SessionKit& kit, Buffer& out) {
        const winsize size = paneWinsize(kit);
        out.reset();
        for (u16 row = 0; row < size.ws_row; ++row) {
            for (u16 column = 0; column < size.ws_col; ++column) {
                const u32 codepoint = kit.terminal->testApi.cell(row, column).cell.uc_pt;
                const char printable = codepoint >= 0x20 && codepoint <= 0x7e ? (char)(codepoint) : ' ';
                out.append(&printable, 1);
            }
            out.append("\n", 1);
        }
    };
    const auto trackSwitch = [&]() {
        trackedActiveKit = trackedKitOfActive();
    };
    // Put the window's focus on the kit at this index, if that kit is a
    // pane of the tab in front. The tab walk CLOSE_SESSION otherwise
    // uses cannot move inside a tab at all - activateNext() refuses
    // outright while there is only one - so without this there is no way
    // to name an unfocused pane for closing, and asking for one spun
    // forever (R1a-qa, B1).
    const auto focusKitInActiveTab = [&](size_t index) -> bool {
        Vector<SessionPane> panes;
        sessions->visiblePanes(panes);
        for (const SessionPane& pane : panes) {
            if (&sessionKits[index].terminal->terminal == pane.terminal) {
                sessions->focusPane(pane.id);
                trackSwitch();
                return true;
            }
        }
        return false;
    };
    TestSessionAction trackPreviousSession(trackSwitch);
    TestSessionAction trackNextSession(trackSwitch);
    composer.newTabListeners.pushBack(&trackNewSession);
    composer.sessionsChangedListeners.pushBack(&trackSplitSession);
    composer.closeTabListeners.pushBack(&trackClosedSession);
    composer.prevTabListeners.pushBack(&trackPreviousSession);
    composer.nextTabListeners.pushBack(&trackNextSession);
    FailFontChange failFontChange;
    composer.fontChangedListeners.pushFront(&failFontChange);
    pid_t childPid = -1;
    int childExitStatus = -1;

    ScriptedReadQueue scriptedPtyReads;
    ScriptedWriteQueue scriptedPtyWrites;
    Buffer& writtenPtyData = scriptedPtyWrites.written;
    TestUtf8Decoder testUtf8Decoder;
    const auto installScriptedPtyReader = [&]() {
        terminalPty.setReadHandler(&scriptedPtyReads);
    };
    terminal.redraw();
    writeAll(controlFd, "READY\n");

    const auto pumpChild = [&]() {
        terminal.kickOutput();
        pollfd source{io[0], POLLIN, 0};
        if (poll(&source, 1, 0) > 0 && (source.revents & POLLIN)) {
            terminal.readPty();
        }
        int status = 0;
        if (childPid > 0 && waitpid(childPid, &status, WNOHANG) == childPid) {
            childPid = -1;
            if (WIFEXITED(status)) {
                childExitStatus = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                childExitStatus = 128 + WTERMSIG(status);
            } else {
                childExitStatus = 255;
            }
            // By reap time every child write has completed, but a PTY read is
            // not required to consume every buffered write.  Drain through
            // EAGAIN before publishing the exit status and final screen.
            terminal.drainPty();
        }
    };

    // Blocks until every byte handed to the PTY stream has reached the
    // kernel. The writer replays from a fiber: after each drained slave
    // chunk it needs a poll round to see the descriptor writable again,
    // so the wait yields through the poller instead of counting attempts.
    // A scripted stall is left alone — the test controls every retry — and
    // a running child owns the slave side.
    const auto waitOutputDrained = [&]() {
        while (!terminal.outputDrained()) {
            if (childPid > 0 || terminalPty.scriptStalled()) {
                break;
            }
            {
                Buffer drained;
                drainInput(io[1], drained);
            }
            controlScheduler->yield();
        }
    };

    plt::DropTarget* const dropTarget = createDropTarget(*composer.pool, composer);
    const auto spawnDrop = [&](const Buffer& payload, StringView mime) {
        DropDelivery* const delivery = composer.pool->make<DropDelivery>(dropTarget, StringView(payload), mime);
        composer.platform->scheduler()->spawn(*delivery, delivery->stack, sizeof(delivery->stack));
    };
    // The platform delivers file drops as text/uri-list; the test path
    // takes the same route through a file URI.
    const auto pathToUriList = [](StringView path, Buffer& uri) {
        static const char hexDigits[] = "0123456789abcdef";
        uri.reset();
        uri.append("file://", 7);
        for (const u8 byte : path) {
            const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '/' || byte == '.' || byte == '_' || byte == '-' || byte == '~';
            if (plain) {
                uri.append(&byte, 1);
            } else {
                const char escaped[3] = {'%', hexDigits[byte >> 4], hexDigits[byte & 15]};
                uri.append(escaped, 3);
            }
        }
        uri.append("\r\n", 2);
    };

    Buffer buffered;
    Buffer lineBytes;
    // The whole control protocol runs on one fiber inside the platform
    // loop, so a handler may block on the PTY stream or a timer while the
    // loop keeps serving fibers and frames.
    const int controlFlags = fcntl(controlFd, F_GETFL, 0);
    if (controlFlags >= 0) {
        fcntl(controlFd, F_SETFL, controlFlags | O_NONBLOCK);
    }
    controlScheduler = composer.platform->scheduler();
    auto controlLoop = [&] {
        try {
            while (readLine(controlScheduler, controlFd, buffered, lineBytes)) {
                const StringView line(lineBytes);
                try {
                    // Shadow the outer first-session locals: every
                    // terminal-bound command addresses the session the
                    // window shows, so tests switch first and poke
                    // second. The child and script helpers above stay
                    // bound to the first session, whose pty owns the
                    // spawned shell.
                    //
                    // Inside the try on purpose: a session the harness
                    // has no kit for is a failed command, answered with
                    // ERR, and not a reason to stop the whole terminal
                    // and leave the test reading a closed socket.
                    SessionKit& activeKit = kitFor(sessions->activeTerminal());
                    TestTerminal& terminal = *activeKit.terminal;
                    TestPty& terminalPty = *activeKit.pty;
                    if (startsWith(line, StringView(u8"WRITE "))) {
                        Buffer input;
                        decodeHex(tail(line, 6), input);
                        terminal.feedPtyOutput((const u8*)(input.data()), input.used());
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"WRITE_SESSION "))) {
                        // Session <index>'s shell produced bytes: they
                        // parse into that terminal, active or not, and
                        // its responses go to its own pty.
                        ArgReader args(tail(line, 14));
                        u32 index = 0;
                        char encoded[64 * 1024];
                        if (!args.read(index) || index >= sessionKits.length() || !args.token(encoded, sizeof(encoded))) {
                            raiseError(StringView(u8"invalid session write"));
                        }
                        Buffer input;
                        decodeHex(StringView(encoded), input);
                        sessionKits[index].terminal->feedPtyOutput((const u8*)(input.data()), input.used());
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"READ_INPUT_SESSION "))) {
                        ArgReader args(tail(line, 19));
                        u32 index = 0;
                        if (!args.read(index) || index >= sessionKits.length()) {
                            raiseError(StringView(u8"invalid session read"));
                        }
                        Buffer taken;
                        sessionKits[index].pty->takeWriteData(taken);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(taken)}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"WRITE_CHUNKS "))) {
                        ArgReader args(tail(line, 13));
                        Buffer chunkArena;
                        Vector<u32> chunkEnds;
                        char encoded[64 * 1024];
                        Buffer decoded;
                        while (args.token(encoded, sizeof(encoded))) {
                            decodeHex(StringView(encoded), decoded);
                            chunkArena.append(decoded.data(), decoded.used());
                            chunkEnds.pushBack((u32)(chunkArena.used()));
                        }
                        if (chunkEnds.empty()) {
                            raiseError(StringView(u8"empty PTY chunk list"));
                        }
                        terminal.feedPtyOutput(chunkArena, chunkEnds);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"MEASURE_WIDTHS "))) {
                        ArgReader args(tail(line, 15));
                        char encoded[64 * 1024];
                        Buffer decoded;
                        Buffer input;
                        size_t count = 0;
                        while (args.token(encoded, sizeof(encoded))) {
                            input.append(
                                "\x1b"
                                "c",
                                2
                            );
                            decodeHex(StringView(encoded), decoded);
                            input.append(decoded.data(), decoded.used());
                            input.append("\x1b[6n", 4);
                            ++count;
                        }
                        if (!count) {
                            raiseError(StringView(u8"empty width measurement"));
                        }
                        // Reports are taken from the in-process write capture (see
                        // READ_INPUT): the kernel pty path is asynchronous and the
                        // reports would double-report through a later READ_INPUT.
                        {
                            Buffer drained;
                            drainInput(io[1], drained);
                        }
                        {
                            Buffer discarded;
                            terminalPty.takeWriteData(discarded);
                        }
                        terminal.feedPtyOutput((const u8*)(input.data()), input.used());
                        waitOutputDrained();
                        {
                            Buffer drained;
                            drainInput(io[1], drained);
                        }
                        Buffer taken;
                        terminalPty.takeWriteData(taken);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(taken)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"OPTIONS")) {
                        const auto packedColor = [](Color color) {
                            return ((u32)(color.red) << 16) | ((u32)(color.green) << 8) | color.blue;
                        };
                        // T8. uriScheme is the one option here whose value
                        // is a list. Joined with commas and never spaces:
                        // the harness splits this line on whitespace, so a
                        // space inside a value would be read as the start
                        // of the next field.
                        StringBuilder uriSchemeList;
                        for (const StringView scheme : composer.opts->uriSchemes) {
                            if (!StringView(uriSchemeList).empty()) {
                                uriSchemeList << StringView(u8",");
                            }
                            uriSchemeList << scheme;
                        }
                        writeParts(controlFd, StringView(u8"OK fontsize="), (i64)(composer.opts->fontsize), StringView(u8" border="), (i64)(composer.opts->border), StringView(u8" columns="), (i64)(composer.opts->nCols), StringView(u8" rows="), (i64)(composer.opts->nRows), StringView(u8" save_lines="), (i64)(composer.vtConfig.config->saveLines), StringView(u8" fg="), (i64)(packedColor(composer.vtConfig.config->fg)), StringView(u8" bg="), (i64)(packedColor(composer.vtConfig.config->bg)), StringView(u8" cr="), (i64)(packedColor(composer.vtConfig.config->cr)), StringView(u8" alt_scroll="), (i64)(composer.vtConfig.config->altScrollMode), StringView(u8" bold_colors="), (i64)(composer.vtConfig.config->boldColors), StringView(u8" auto_copy="), (i64)(composer.vtConfig.config->autoCopyMode), StringView(u8" allow_osc52_read="), (i64)(composer.vtConfig.config->allowOsc52Read), StringView(u8" allow_window_ops="), (i64)(composer.vtConfig.config->allowWindowOps), StringView(u8" maximized="), (i64)(composer.opts->maximized), StringView(u8" fullscreen="), (i64)(composer.opts->fullscreen), StringView(u8" no_decorations="), (i64)(composer.opts->noDecorations), StringView(u8" transparent_titlebar="), (i64)(composer.opts->transparentTitlebar), StringView(u8" background_opacity="), (i64)(composer.opts->backgroundOpacity), StringView(u8" background_blur="), backdropModeName(composer.opts->backgroundBlur), StringView(u8" quick="), (i64)(composer.opts->quick), StringView(u8" quick_geometry_w_percent="), (i64)(composer.opts->quickGeometry.width.percent), StringView(u8" quick_geometry_w="), (i64)(composer.opts->quickGeometry.width.value), StringView(u8" quick_geometry_h_percent="), (i64)(composer.opts->quickGeometry.height.percent), StringView(u8" quick_geometry_h="), (i64)(composer.opts->quickGeometry.height.value), StringView(u8" quick_geometry_x_percent="), (i64)(composer.opts->quickGeometry.x.percent), StringView(u8" quick_geometry_x="), (i64)(composer.opts->quickGeometry.x.value), StringView(u8" quick_geometry_y_percent="), (i64)(composer.opts->quickGeometry.y.percent), StringView(u8" quick_geometry_y="), (i64)(composer.opts->quickGeometry.y.value), StringView(u8" quick_corner_radius="), (i64)(composer.opts->quickCornerRadius), StringView(u8" quick_remember_frame="), (i64)(composer.opts->quickRememberFrame), StringView(u8" sidebar_tabs="), (i64)(composer.opts->sidebarTabs), StringView(u8" sidebar_width="), (i64)(composer.opts->sidebarWidth), StringView(u8" sidebar_tab_tint="), (i64)(composer.opts->sidebarTabTint), StringView(u8" auto_hide_chrome="), (i64)(composer.opts->autoHideChrome), StringView(u8" panes="), (i64)(composer.opts->panes), StringView(u8" natural_editing="), (i64)(composer.opts->naturalEditing), StringView(u8" pane_divider_color="), (i64)(packedColor(composer.opts->paneDividerColor)), StringView(u8" uri_schemes="), StringView(uriSchemeList), StringView(u8"\n"));
                    } else if (line == StringView(u8"ARGV")) {
                        Buffer arguments;
                        for (int index = 0; index < argc; ++index) {
                            if (index) {
                                arguments.append("", 1);
                            }
                            const StringView argument(argv[index]);
                            arguments.append(argument.data(), argument.length());
                        }
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(arguments)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"LAUNCH_COMMAND")) {
                        const LaunchCommand command = buildLaunchCommand(argc, argv, composer.opts->shell, composer.opts->login);
                        Buffer encoded;
                        const StringView executable(command.executable());
                        encoded.append(executable.data(), executable.length());
                        for (size_t index = 0; index < command.offsets.length(); ++index) {
                            encoded.append("", 1);
                            const StringView argument(command.argument(index));
                            encoded.append(argument.data(), argument.length());
                        }
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(encoded)}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"FONT_LOAD "))) {
                        Buffer request;
                        decodeHex(tail(line, 10), request);
                        Vector<StringView> names;
                        splitFontNames(StringView(request), names);
                        ObjPool::Ref fontPool = ObjPool::fromMemory();
                        Fontpack* fonts = Fontpack::create(composer, *fontPool, names.data(), names.length(), composer.opts->symbolFonts.data(), composer.opts->symbolFonts.length(), composer.opts->fontsize);
                        writeParts(controlFd, StringView(u8"OK "), (i64)(fonts->getPx()), StringView(u8" "), (i64)(fonts->getPy()), StringView(u8" "), (i64)(fonts->hasBold()), StringView(u8" "), (i64)(fonts->hasItalic()), StringView(u8" "), (i64)(fonts->hasBoldItalic()), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"RENDER_IMAGE "))) {
                        Buffer request;
                        decodeHex(tail(line, 13), request);
                        Vector<StringView> names;
                        splitFontNames(StringView(request), names);
                        ObjPool::Ref renderPool = ObjPool::fromMemory();
                        Composer& renderComposer = *renderPool->make<Composer>(renderPool.mutPtr());
                        renderComposer.setOptions(composer.opts);
                        renderComposer.installFontRenderers();
                        renderComposer.contentScale = composer.contentScale;
                        Fontpack* fonts = Fontpack::create(renderComposer, *renderPool, names.data(), names.length(), composer.opts->symbolFonts.data(), composer.opts->symbolFonts.length(), composer.opts->fontsize);
                        renderComposer.fonts = fonts;
                        renderComposer.extras.replace(composer.extras.store);
                        renderComposer.geometry.setCellPixelSize(fonts->getPx(), fonts->getPy());
                        const Insets imageInsets = composer.contentInsets();
                        const u16 imageWidth = (u16)(gridPixelWidth(renderer.columns(), imageInsets, fonts->getPx()));
                        const u16 imageHeight = (u16)(gridPixelHeight(renderer.rows(), imageInsets, fonts->getPy()));
                        renderComposer.resize(imageWidth, imageHeight);
                        renderComposer.platform = plt::createHeadlessPlatform(*renderPool);
                        renderComposer.shaper = SpanShaper::create(renderComposer, *renderPool);
                        TerminalUpdate imageUpdate = renderer.renderUpdate();
                        // The retained cells shape through a throwaway screen
                        // carrying the requested fontpack; its own rows stay
                        // blank.
                        imageUpdate.shapes = Screen::createPrimary(renderComposer.extras, *renderPool, renderer.columns(), renderer.rows(), imageUpdate.colors, 0);
                        imageUpdate.shapeFromCells = true;

                        struct ImageFrame final: plt::FrameCallback {
                            bool frame(const plt::WindowInfo&) override {
                                return renderer->update(*update);
                            }

                            Renderer* renderer = nullptr;
                            const TerminalUpdate* update = nullptr;
                        } imageFrame;

                        renderComposer.window = renderComposer.platform->createWindow(
                            *renderPool,
                            {
                                .width = imageWidth,
                                .height = imageHeight,
                                .frame = &imageFrame,
                            }
                        );
                        renderComposer.installVtHost();
                        auto& imageWindow = static_cast<plt::WindowHeadless&>(*renderComposer.window);
                        imageFrame.renderer = Renderer::create(renderComposer, *renderPool, imageWindow.renderContext());
                        imageFrame.update = &imageUpdate;
                        imageWindow.requestFrame();
                        if (!imageWindow.dispatchFrame()) {
                            raiseError(StringView(u8"reference image presentation failed"));
                        }
                        const plt::HeadlessFrame image = imageWindow.presentedFrame();
                        writeParts(controlFd, StringView(u8"OK "), (i64)(image.width), StringView(u8" "), (i64)(image.height), StringView(u8" "), HexOut{StringView(image.pixels, image.length)}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"GRAPHEME_BREAKS "))) {
                        ArgReader args(tail(line, 16));
                        char token[32];
                        Buffer boundaries;
                        GraphemeBreaker breaker;
                        while (args.token(token, sizeof(token))) {
                            u64 value = 0;
                            if (!parseU64(StringView(token), value, 16) || value > 0x10ffff) {
                                raiseError(StringView(u8"invalid codepoint"));
                            }
                            const char mark = breaker.breakBefore(value) ? '1' : '0';
                            boundaries.append(&mark, 1);
                        }
                        if (boundaries.empty()) {
                            raiseError(StringView(u8"empty grapheme sequence"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), StringView(boundaries), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"PREEDIT "))) {
                        ArgReader args(tail(line, 8));
                        char encoded[4096];
                        i32 begin = -1;
                        i32 end = -1;
                        if (args.token(encoded, sizeof(encoded))) {
                            args.read(begin);
                            args.read(end);
                        }
                        Buffer decoded;
                        if (StringView(encoded) != StringView(u8"-")) {
                            decodeHex(StringView(encoded), decoded);
                        }
                        terminal.preedit(StringView(decoded), begin, end);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"INPUT "))) {
                        Buffer input;
                        decodeHex(tail(line, 6), input);
                        terminal.sendBytes(StringView((const u8*)(input.data()), input.used()));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"USER_INPUT "))) {
                        Buffer input;
                        decodeHex(tail(line, 11), input);
                        terminal.sendBytes(StringView((const u8*)(input.data()), input.used()), true);
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"HARD_RESET")) {
                        terminal.testApi.hardReset();
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"SPAWN "))) {
                        if (childPid > 0) {
                            raiseError(StringView(u8"child already running"));
                        }
                        if (tcsetattr(io[1], TCSANOW, &childTtyAttrs) < 0) {
                            raiseError(StringView(u8"test child tcsetattr failed"));
                        }
                        Buffer encoded;
                        decodeHex(tail(line, 6), encoded);
                        encoded.append("", 1);
                        Vector<char*> argumentPointers;
                        {
                            char* cursor = (char*)(encoded.mutData());
                            char* const endOfAll = cursor + encoded.used() - 1;
                            while (cursor < endOfAll) {
                                argumentPointers.pushBack(cursor);
                                cursor += StringView(cursor).length() + 1;
                            }
                        }
                        if (argumentPointers.empty() || argumentPointers[0][0] == '\0') {
                            raiseError(StringView(u8"empty child command"));
                        }
                        const char* ttyPath = ttyname(io[1]);
                        if (!ttyPath) {
                            raiseError(StringView(u8"test child tty has no path"));
                        }
                        const StringView ttyView(ttyPath);
                        char childTtyPath[PATH_MAX];
                        const size_t ttyLength = ttyView.length() < sizeof(childTtyPath) - 1 ? ttyView.length() : sizeof(childTtyPath) - 1;
                        memcpy(childTtyPath, ttyView.data(), ttyLength);
                        childTtyPath[ttyLength] = '\0';
                        childExitStatus = -1;
                        childPid = fork();
                        if (childPid < 0) {
                            raiseError(StringView(u8"test fork failed"));
                        }
                        if (childPid == 0) {
                            setsid();
                            close(io[1]);
                            const int childTty = open(childTtyPath, O_RDWR);
                            if (childTty < 0) {
                                _exit(126);
                            }
                            ioctl(childTty, TIOCSCTTY, 0);
                            dup2(childTty, STDIN_FILENO);
                            dup2(childTty, STDOUT_FILENO);
                            dup2(childTty, STDERR_FILENO);
                            close(io[0]);
                            if (childTty > STDERR_FILENO) {
                                close(childTty);
                            }
                            configureTerminalChildEnvironment(*composer.brand, composer.vtConfig.config->widths);
                            argumentPointers.pushBack(nullptr);
                            execvp(argumentPointers[0], argumentPointers.mutData());
                            _exit(127);
                        }
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"PUMP")) {
                        pumpChild();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"READ_PTY")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.readPty()), StringView(u8"\n"));
                    } else if (line == StringView(u8"READ_CHILD_OUTPUT")) {
                        Buffer taken;
                        terminalPty.takeReadData(taken);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(taken)}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"PTY_READ_SCRIPT "))) {
                        scriptedPtyReads.items.clear();
                        scriptedPtyReads.arena.reset();
                        scriptedPtyReads.head = 0;
                        ArgReader args(tail(line, 16));
                        char token[64 * 1024];
                        bool any = false;
                        Buffer decoded;
                        while (args.token(token, sizeof(token))) {
                            any = true;
                            if (StringView(token) == StringView(u8"z")) {
                                scriptedPtyReads.push(StringView(), 0, true);
                            } else if (token[0] == 'd' && token[1] != '\0') {
                                decodeHex(StringView(token + 1), decoded);
                                scriptedPtyReads.push(StringView(decoded), 0, false);
                            } else if (token[0] == 'e' && token[1] != '\0') {
                                i64 error = 0;
                                if (!parseI64(StringView(token + 1), error) || error <= 0) {
                                    raiseError(StringView(u8"invalid PTY errno"));
                                }
                                scriptedPtyReads.push(StringView(), (int)(error), false);
                            } else {
                                raiseError(StringView(u8"invalid PTY read script"));
                            }
                        }
                        if (!any) {
                            raiseError(StringView(u8"empty PTY read script"));
                        }
                        installScriptedPtyReader();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"PTY_READ_REPEAT "))) {
                        ArgReader args(tail(line, 16));
                        unsigned byte;
                        size_t count;
                        int eof;
                        if (!(args.read(byte) && args.read(count) && args.read(eof)) || byte > 255 || count == 0 || count > 64 * 1024 * 1024 || eof < 0 || eof > 1) {
                            raiseError(StringView(u8"invalid repeated PTY input"));
                        }
                        scriptedPtyReads.items.clear();
                        scriptedPtyReads.arena.reset();
                        scriptedPtyReads.head = 0;
                        {
                            Buffer repeated;
                            repeated.grow(count);
                            memset(repeated.mutData(), (int)(byte), count);
                            repeated.seekAbsolute(count);
                            scriptedPtyReads.push(StringView(repeated), 0, false);
                        }
                        if (eof) {
                            scriptedPtyReads.push(StringView(), 0, true);
                        }
                        installScriptedPtyReader();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"PTY_WRITE_SCRIPT "))) {
                        scriptedPtyWrites.items.clear();
                        scriptedPtyWrites.head = 0;
                        writtenPtyData.reset();
                        ArgReader args(tail(line, 17));
                        char token[64];
                        bool any = false;
                        while (args.token(token, sizeof(token))) {
                            any = true;
                            if (token[0] == 'n' && token[1] != '\0') {
                                u64 count = 0;
                                if (!parseU64(StringView(token + 1), count) || count == 0) {
                                    raiseError(StringView(u8"invalid PTY write count"));
                                }
                                scriptedPtyWrites.items.pushBack({count, 0});
                            } else if (token[0] == 'e' && token[1] != '\0') {
                                i64 error = 0;
                                if (!parseI64(StringView(token + 1), error) || error <= 0) {
                                    raiseError(StringView(u8"invalid PTY write errno"));
                                }
                                scriptedPtyWrites.items.pushBack({0, (int)(error)});
                            } else {
                                raiseError(StringView(u8"invalid PTY write script"));
                            }
                        }
                        if (!any) {
                            raiseError(StringView(u8"empty PTY write script"));
                        }
                        scriptedPtyWrites.written.reset();
                        terminalPty.setWriteHandler(&scriptedPtyWrites);
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"NEW_SESSION")) {
                        // The same action and the same Pty::spawn path as
                        // Cmd+T in production.
                        publishSessionAction(composer.newTabListeners);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"SPLIT "))) {
                        // The same action and the same split path as
                        // Cmd+D / Cmd+Shift+D in production; needs
                        // -panes, without which SessionSet declines and
                        // the pane count stays where it was.
                        ArgReader args(tail(line, 6));
                        char direction[8];
                        if (!args.token(direction, sizeof(direction))) {
                            raiseError(StringView(u8"SPLIT needs a direction"));
                        }
                        const StringView chosen(direction);
                        // splitFocused() answers with a bool, but the
                        // listener chain it is published through has no
                        // return value, so a refusal is only visible as a
                        // pane count that did not move. Counting is what
                        // keeps "panes are off" from reading as "splitting
                        // is broken" in the test that forgot the flag.
                        Vector<SessionPane> before;
                        sessions->visiblePanes(before);
                        if (chosen == StringView(u8"V")) {
                            publishSessionAction(composer.splitVerticalListeners);
                        } else if (chosen == StringView(u8"H")) {
                            publishSessionAction(composer.splitHorizontalListeners);
                        } else {
                            raiseError(StringView(u8"SPLIT direction is V or H"));
                        }
                        Vector<SessionPane> after;
                        sessions->visiblePanes(after);
                        if (after.length() == before.length()) {
                            raiseError(StringView(u8"split declined (needs -panes)"));
                        }
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"PANE_COUNT")) {
                        Vector<SessionPane> panes;
                        sessions->visiblePanes(panes);
                        writeParts(controlFd, StringView(u8"OK "), (i64)(panes.length()), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"FOCUS_PANE "))) {
                        ArgReader args(tail(line, 11));
                        u32 index = 0;
                        if (!args.read(index)) {
                            raiseError(StringView(u8"FOCUS_PANE needs an index"));
                        }
                        sessions->focusPane(visiblePane(index).id);
                        // Same bookkeeping the tab moves do. focusPane()
                        // publishes sessionsChangedListeners, but adoption
                        // there deliberately leaves trackedActiveKit alone,
                        // so without this the close path would go on naming
                        // the pane that was in front before (R1a-qa, B2).
                        //
                        // Keep it even though no test can currently see it
                        // go: CLOSE_SESSION now always starts by focusing
                        // its target, and focusKitInActiveTab() repairs the
                        // same record on the way, so the only reader of
                        // trackedActiveKit is already correct by the time it
                        // reads (R1a-test round 2, finding 2). This is the
                        // bookkeeping invariant for whoever comes to read
                        // trackedActiveKit next, not dead code.
                        trackSwitch();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"WINSIZE_PANE "))) {
                        ArgReader args(tail(line, 13));
                        u32 index = 0;
                        if (!args.read(index)) {
                            raiseError(StringView(u8"WINSIZE_PANE needs an index"));
                        }
                        const winsize size = paneWinsize(paneKit(index));
                        writeParts(controlFd, StringView(u8"OK "), (i64)(size.ws_col), StringView(u8" "), (i64)(size.ws_row), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"SCREEN_TEXT_PANE "))) {
                        ArgReader args(tail(line, 17));
                        u32 index = 0;
                        if (!args.read(index)) {
                            raiseError(StringView(u8"SCREEN_TEXT_PANE needs an index"));
                        }
                        Buffer text;
                        paneScreenText(paneKit(index), text);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{hexview(StringView(text))}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"PTY_WRITE_PANE "))) {
                        // Pane <index>'s shell produced bytes, the way
                        // WRITE_SESSION does it for a session index: they
                        // parse into that pane's terminal alone, which is
                        // how one pane goes to the alternate screen while
                        // its neighbour stays quiet.
                        ArgReader args(tail(line, 15));
                        u32 index = 0;
                        char encoded[64 * 1024];
                        if (!args.read(index) || !args.token(encoded, sizeof(encoded))) {
                            raiseError(StringView(u8"invalid pane write"));
                        }
                        SessionKit& target = paneKit(index);
                        Buffer input;
                        decodeHex(StringView(encoded), input);
                        target.terminal->feedPtyOutput((const u8*)(input.data()), input.used());
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"CLOSE_SESSION "))) {
                        ArgReader args(tail(line, 14));
                        u32 index = 0;
                        if (!args.read(index) || index >= sessionKits.length()) {
                            raiseError(StringView(u8"CLOSE_SESSION needs an index"));
                        }
                        // Reaching the session about to close takes both
                        // moves, because a session is a pane of a tab and
                        // not a tab of its own: the walk crosses tabs and
                        // focusing crosses panes. So the search does both
                        // at every stop - focus it here, else step to the
                        // next tab and look again - rather than looking
                        // once and then only walking, which left a pane
                        // that is neither in front nor focused in its own
                        // tab unreachable however far the walk went
                        // (R1a-qa, V5). Bounded by the number of kits:
                        // there is a tab for each at most, so a lap that
                        // has looked in every tab will not find it on the
                        // next one either.
                        //
                        // The walk moves the window for real, so a search
                        // that comes up empty puts the tab it started on
                        // back in front before refusing: a command that
                        // answers ERR must leave the window where it
                        // found it, or the test that caught the error and
                        // carried on is addressing a tab it never chose
                        // (R1a-qa, V4).
                        const size_t startedOn = activeKitIndex();
                        for (size_t steps = 0; !focusKitInActiveTab(index); ++steps) {
                            if (steps == sessionKits.length()) {
                                for (size_t back = 0; activeKitIndex() != startedOn && back < sessionKits.length(); ++back) {
                                    publishSessionAction(composer.nextTabListeners);
                                }
                                raiseError(StringView(u8"CLOSE_SESSION cannot reach that session"));
                            }
                            publishSessionAction(composer.nextTabListeners);
                        }
                        publishSessionAction(composer.closeTabListeners);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"PRIVATE_MODE "))) {
                        ArgReader args(tail(line, 13));
                        size_t mode = 0;
                        if (!args.read(mode)) {
                            raiseError(StringView(u8"PRIVATE_MODE needs a mode"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getPrivateMode((u32)(mode))), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"ANSI_MODE "))) {
                        ArgReader args(tail(line, 10));
                        size_t mode = 0;
                        if (!args.read(mode)) {
                            raiseError(StringView(u8"ANSI_MODE needs a mode"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getAnsiMode((u32)(mode))), StringView(u8"\n"));
                    } else if (line == StringView(u8"SESSION_STATE")) {
                        char reply[64];
                        const int length = snprintf(reply, sizeof(reply), "%zu %zu\n", sessionKits.length(), activeKitIndex());
                        writeAll(controlFd, StringView((const u8*)(reply), (size_t)(length)));
                    } else if (line == StringView(u8"WAIT_READ_PTY")) {
                        const bool ready = composer.platform->scheduler()->awaitReadable(io[0], 1'000'000);
                        if (!ready) {
                            raiseError(StringView(u8"PTY input timeout"));
                        }
                        terminal.readPty();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"FAIL_NEXT_PRESENT")) {
                        window.failNextPresentation();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"REFERENCE_IMAGE")) {
                        const ReferenceImage image = renderer.image();
                        writeParts(controlFd, StringView(u8"OK "), (i64)(image.width), StringView(u8" "), (i64)(image.height), StringView(u8" "), HexOut{StringView(image.pixels, image.length)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"VULKAN_IMAGE")) {
                        Buffer rgb;
                        u32 imageWidth = 0;
                        u32 imageHeight = 0;
                        if (gpuShadow == nullptr || !gpuShadow->captureOutput(rgb, imageWidth, imageHeight)) {
                            raiseError(StringView(u8"gpu output capture unavailable"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(imageWidth), StringView(u8" "), (i64)(imageHeight), StringView(u8" "), HexOut{StringView(rgb)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"VULKAN_SHADOW")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(gpuShadow != nullptr), StringView(u8"\n"));
                    } else if (line == StringView(u8"SHAPE_GENERATION")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(composer.shaper->spanGeneration()), StringView(u8"\n"));
                    } else if (line == StringView(u8"FAIL_NEXT_FONT_CHANGE")) {
                        failFontChange.arm();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"PRESENT")) {
                        terminal.redraw();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"REPAINT")) {
                        if (!terminal.repaint()) {
                            raiseError(StringView(u8"repaint failed"));
                        }
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"GPU_ATTRIBUTE_MASKS")) {
                        TerminalCell cell{};
                        cell.dwidth = true;
                        const u32 doubleWidth = Renderer::cellAttributes(cell);
                        cell.dwidth = false;
                        cell.dwidth_cont = true;
                        const u32 continuation = Renderer::cellAttributes(cell);
                        writeParts(controlFd, StringView(u8"OK "), (i64)(doubleWidth), StringView(u8" "), (i64)(continuation), StringView(u8"\n"));
                    } else if (line == StringView(u8"POLL_CHILD")) {
                        pumpChild();
                        Buffer text;
                        renderer.screenText(text);
                        writeParts(controlFd, StringView(u8"OK "), (i64)(childPid > 0), StringView(u8" "), (i64)(childExitStatus), StringView(u8" "), HexOut{hexview(StringView(text))}, StringView(u8"\n"));
                    } else if (line == StringView(u8"CHILD_STATUS")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(childPid > 0), StringView(u8" "), (i64)(childExitStatus), StringView(u8"\n"));
                    } else if (line == StringView(u8"PAGE_UP")) {
                        terminal.pageUp();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"PAGE_DOWN")) {
                        terminal.pageDown();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"WHEEL_UP")) {
                        terminal.mouseWheelUp();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"WHEEL_DOWN")) {
                        terminal.mouseWheelDown();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"SCROLL "))) {
                        ArgReader args(tail(line, 7));
                        double x;
                        double y;
                        unsigned modifiers;
                        unsigned phase;
                        unsigned precise;
                        unsigned momentum;
                        int pixelX;
                        int pixelY;
                        double time;
                        if (!(args.read(x) && args.read(y) && args.read(modifiers) && args.read(pixelX) && args.read(pixelY) && args.read(phase) && args.read(precise) && args.read(momentum) && args.read(time)) || modifiers > 15 || phase > 4 || precise > 1 || momentum > 1) {
                            raiseError(StringView(u8"invalid scroll event"));
                        }
                        composer.input->scroll({
                            .x = x,
                            .y = y,
                            .pixelX = pixelX,
                            .pixelY = pixelY,
                            .modifiers = (u16)(modifiers),
                            .phase = (plt::ScrollPhase)(phase),
                            .precise = precise != 0,
                            .momentum = momentum != 0,
                            .time = time,
                        });
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"POINTER "))) {
                        ArgReader args(tail(line, 8));
                        double x, y, scaleX, scaleY;
                        unsigned modifiers;
                        if (!(args.read(x) && args.read(y) && args.read(modifiers) && args.read(scaleX) && args.read(scaleY)) || modifiers > 15) {
                            raiseError(StringView(u8"invalid pointer event"));
                        }
                        const int pixelX = mouseFramebufferCoordinate(x, scaleX);
                        const int pixelY = mouseFramebufferCoordinate(y, scaleY);
                        composer.input->pointerMotion({pixelX, pixelY, (u16)(modifiers)});
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"BUTTON "))) {
                        ArgReader args(tail(line, 7));
                        int button;
                        unsigned pressed, modifiers;
                        double x, y, time, scaleX, scaleY;
                        if (!(args.read(button) && args.read(pressed) && args.read(x) && args.read(y) && args.read(modifiers) && args.read(time) && args.read(scaleX) && args.read(scaleY)) || button < 0 || button > 7 || pressed > 1 || modifiers > 15) {
                            raiseError(StringView(u8"invalid button event"));
                        }
                        const int pixelX = mouseFramebufferCoordinate(x, scaleX);
                        const int pixelY = mouseFramebufferCoordinate(y, scaleY);
                        const u64 clipboardGeneration = clipboard.generation;
                        composer.input->pointerButton({(PointerButton)(button), pressed != 0, pixelX, pixelY, (u16)(modifiers), time});
                        terminal.update();
                        Buffer selection;
                        if (clipboard.generation != clipboardGeneration) {
                            const StringView content(clipboard.primary);
                            selection.append(content.data(), content.length());
                        }
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(selection)}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"RESIZE "))) {
                        ArgReader args(tail(line, 7));
                        unsigned columns;
                        unsigned rows;
                        if (!(args.read(columns) && args.read(rows)) || !columns || !rows) {
                            raiseError(StringView(u8"invalid resize"));
                        }
                        const Insets resizeInsets = composer.contentInsets();
                        terminal.resize(gridPixelWidth(columns, resizeInsets, composer.geometry.cellPixelWidth), gridPixelHeight(rows, resizeInsets, composer.geometry.cellPixelHeight));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"RESIZE_PIXELS "))) {
                        ArgReader args(tail(line, 14));
                        unsigned pixelWidth;
                        unsigned pixelHeight;
                        const Insets pixelInsets = composer.contentInsets();
                        if (!(args.read(pixelWidth) && args.read(pixelHeight)) || pixelWidth <= gridPixelWidth(0, pixelInsets, composer.geometry.cellPixelWidth) || pixelHeight <= gridPixelHeight(0, pixelInsets, composer.geometry.cellPixelHeight)) {
                            raiseError(StringView(u8"invalid pixel resize"));
                        }
                        terminal.resize(pixelWidth, pixelHeight);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"WINDOW_INFO "))) {
                        ArgReader args(tail(line, 12));
                        i64 x;
                        i64 y;
                        u64 pixelWidth;
                        u64 pixelHeight;
                        u64 screenWidth;
                        u64 screenHeight;
                        unsigned iconified;
                        unsigned maximized;
                        unsigned fullscreen;
                        unsigned tiled = 0;
                        if (!(args.read(x) && args.read(y) && args.read(pixelWidth) && args.read(pixelHeight) && args.read(screenWidth) && args.read(screenHeight) && args.read(iconified) && args.read(maximized) && args.read(fullscreen)) || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || pixelWidth > UINT16_MAX || pixelHeight > UINT16_MAX || screenWidth > UINT32_MAX || screenHeight > UINT32_MAX || iconified > 1 || maximized > 1 || fullscreen > 1) {
                            raiseError(StringView(u8"invalid window info"));
                        }
                        if ((args.read(tiled)) && tiled > 1) {
                            raiseError(StringView(u8"invalid window info"));
                        }
                        plt::WindowInfo info = window.info();
                        info.x = x;
                        info.y = y;
                        info.width = pixelWidth;
                        info.height = pixelHeight;
                        info.screenPixelWidth = screenWidth;
                        info.screenPixelHeight = screenHeight;
                        info.iconified = iconified;
                        info.maximized = maximized;
                        info.fullscreen = fullscreen;
                        info.tiled = tiled;
                        window.configure(info);
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"WINSIZE")) {
                        // The active session's pty, like every other
                        // terminal-bound command; background ptys hear
                        // resizes too and report once activated.
                        winsize size{};
                        if (ioctl(terminalPty.fd_, TIOCGWINSZ, &size) < 0) {
                            raiseError(StringView(u8"test TIOCGWINSZ failed"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(size.ws_col), StringView(u8" "), (i64)(size.ws_row), StringView(u8"\n"));
                    } else if (line == StringView(u8"WINSIZE_FULL")) {
                        winsize size{};
                        if (ioctl(terminalPty.fd_, TIOCGWINSZ, &size) < 0) {
                            raiseError(StringView(u8"test TIOCGWINSZ failed"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(size.ws_col), StringView(u8" "), (i64)(size.ws_row), StringView(u8" "), (i64)(size.ws_xpixel), StringView(u8" "), (i64)(size.ws_ypixel), StringView(u8"\n"));
                    } else if (line == StringView(u8"FONT_STATE")) {
                        StringBuilder output;
                        output << StringView(u8"OK ") << composer.fontSize << StringView(u8" ") << composer.geometry.cellPixelWidth << StringView(u8" ") << composer.geometry.cellPixelHeight << StringView(u8" ") << composer.geometry.pixelWidth << StringView(u8" ") << composer.geometry.pixelHeight << StringView(u8" ") << composer.geometry.columns << StringView(u8" ") << composer.geometry.rows << StringView(u8" ") << (unsigned)(composer.contentScale * 1000.0f + 0.5f) << StringView(u8" ") << composer.borderPixels() << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"LAST_UPDATE")) {
                        Buffer response;
                        renderer.lastUpdate(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"LAST_UPDATE_ROWS")) {
                        Buffer response;
                        renderer.lastUpdateRows(response);
                        writeAll(controlFd, StringView(response));
                    } else if (startsWith(line, StringView(u8"FRONTEND_SCALE "))) {
                        unsigned xNumerator = 0;
                        unsigned xDenominator = 0;
                        unsigned yNumerator = 0;
                        unsigned yDenominator = 0;
                        char trailing = 0;
                        if (sscanf((const char*)(lineBytes.cStr()) + 15, "%u %u %u %u %c", &xNumerator, &xDenominator, &yNumerator, &yDenominator, &trailing) != 4 || xNumerator == 0 || xDenominator == 0 || yNumerator == 0 || yDenominator == 0 || xNumerator > 10000 || xDenominator > 10000 || yNumerator > 10000 || yDenominator > 10000) {
                            Errno(EINVAL).raise(StringView(u8"invalid frontend scale"));
                        }
                        plt::WindowInfo info = window.info();
                        info.contentScale = max((float)(xNumerator) / xDenominator, (float)(yNumerator) / yDenominator);
                        window.configure(info);
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"KEY "))) {
                        ArgReader args(tail(line, 4));
                        char name[64];
                        unsigned modifiers;
                        if (!args.token(name, sizeof(name)) || !args.read(modifiers) || modifiers > 7) {
                            raiseError(StringView(u8"invalid key"));
                        }
                        terminal.sendKey(parseKey(name), (VtModifier)(modifiers));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"CHAR "))) {
                        ArgReader args(tail(line, 5));
                        unsigned character;
                        unsigned modifiers;
                        if (!(args.read(character) && args.read(modifiers)) || character > 255 || modifiers > 7) {
                            raiseError(StringView(u8"invalid char"));
                        }
                        terminal.sendCharacter((u8)(character), (VtModifier)(modifiers));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"CONTROL_CHARACTER "))) {
                        ArgReader args(tail(line, 18));
                        int key;
                        unsigned shifted;
                        u8 character = 0;
                        if (!(args.read(key) && args.read(shifted)) || shifted > 1 || !controlCharacter(key, shifted, character)) {
                            raiseError(StringView(u8"invalid control character"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(character), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"FRONTEND_CONTROL "))) {
                        ArgReader args(tail(line, 17));
                        int key;
                        unsigned shifted;
                        unsigned alt;
                        u8 character = 0;
                        if (!(args.read(key) && args.read(shifted) && args.read(alt)) || shifted > 1 || alt > 1 || !controlCharacter(key, shifted, character)) {
                            raiseError(StringView(u8"invalid frontend control"));
                        }
                        VtModifier modifiers = VtModifier::control;
                        if (shifted) {
                            modifiers = modifiers | VtModifier::shift;
                        }
                        if (alt) {
                            modifiers = modifiers | VtModifier::alt;
                        }
                        terminal.sendCharacter(character, modifiers);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"FRONTEND_KEY_EVENT "))) {
                        ArgReader args(tail(line, 19));
                        int key;
                        int scancode;
                        int action;
                        int modifiers;
                        if (!(args.read(key) && args.read(scancode) && args.read(action) && args.read(modifiers)) || action < 0 || action > 2 || modifiers < 0) {
                            raiseError(StringView(u8"invalid frontend key event"));
                        }
                        input.key(key, scancode, action, modifiers);
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"FRONTEND_LAYOUT_KEY "))) {
                        ArgReader args(tail(line, 20));
                        int key;
                        int action;
                        int modifiers;
                        unsigned layout;
                        unsigned shifted;
                        unsigned base;
                        if (!(args.read(key) && args.read(action) && args.read(modifiers) && args.read(layout) && args.read(shifted) && args.read(base)) || action < 0 || action > 2 || modifiers < 0 || layout > 0x10ffff || shifted > 0x10ffff || base > 0x10ffff) {
                            raiseError(StringView(u8"invalid frontend layout key"));
                        }
                        input.layoutKey(key, action, modifiers, layout, shifted, base);
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"FRONTEND_TEXT_EVENT "))) {
                        ArgReader args(tail(line, 20));
                        unsigned codepoint;
                        int modifiers;
                        if (!(args.read(codepoint) && args.read(modifiers)) || codepoint > 0x10ffff || modifiers < 0) {
                            raiseError(StringView(u8"invalid frontend text event"));
                        }
                        input.text(codepoint, modifiers);
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"KITTY_KEY "))) {
                        ArgReader args(tail(line, 10));
                        u32 key;
                        u32 shifted;
                        u32 base;
                        unsigned modifiers;
                        unsigned event;
                        if (!(args.read(key) && args.read(shifted) && args.read(base) && args.read(modifiers) && args.read(event)) || event < 1 || event > 3) {
                            raiseError(StringView(u8"invalid kitty key"));
                        }
                        terminal.writeKittyKey(key, shifted, base, modifiers, (VtermKeyEventType)(event));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"KITTY_SPECIAL "))) {
                        ArgReader args(tail(line, 14));
                        char name[64];
                        unsigned modifiers;
                        unsigned event;
                        if (!args.token(name, sizeof(name)) || !args.read(modifiers) || !args.read(event) || event < 1 || event > 3) {
                            raiseError(StringView(u8"invalid kitty special key"));
                        }
                        terminal.writeKittyKey(parseKey(name), modifiers, (VtermKeyEventType)(event));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"PASTE "))) {
                        Buffer pasted;
                        decodeHex(tail(line, 6), pasted);
                        terminal.pasteSelection(StringView(pasted));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"DROP "))) {
                        Buffer dropped;
                        decodeHex(tail(line, 5), dropped);
                        spawnDrop(dropped, StringView(u8"text/plain;charset=utf-8"));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"DROP_PATH "))) {
                        Buffer path;
                        decodeHex(tail(line, 10), path);
                        Buffer uriList;
                        pathToUriList(StringView(path), uriList);
                        spawnDrop(uriList, StringView(u8"text/uri-list"));
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"PASTE_CLIPBOARD 0") || line == StringView(u8"PASTE_CLIPBOARD 1")) {
                        writeAll(controlFd, testApi.pasteClipboard(line.back() == '1') ? "OK 1\n" : "OK 0\n");
                    } else if (startsWith(line, StringView(u8"FOCUS "))) {
                        terminal.setHasFocus(tail(line, 6) == "1");
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"POINTER_PRESENCE 0") || line == StringView(u8"POINTER_PRESENCE 1")) {
                        composer.input->pointerPresence(line.back() == '1');
                        terminal.update();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"HIGHLIGHT_RELEASE "))) {
                        ArgReader args(tail(line, 18));
                        unsigned endX, endY, mouseX, mouseY;
                        if (!(args.read(endX) && args.read(endY) && args.read(mouseX) && args.read(mouseY))) {
                            raiseError(StringView(u8"invalid highlight release"));
                        }
                        terminal.mouseHighlightRelease(endX, endY, mouseX, mouseY);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"LOCATOR_POSITION "))) {
                        ArgReader args(tail(line, 17));
                        unsigned column, row, pixelX, pixelY, buttons;
                        if (!(args.read(column) && args.read(row) && args.read(pixelX) && args.read(pixelY) && args.read(buttons))) {
                            raiseError(StringView(u8"invalid locator position"));
                        }
                        terminal.setLocatorPosition(column, row, pixelX, pixelY, buttons);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"LOCATOR_BUTTON "))) {
                        ArgReader args(tail(line, 15));
                        unsigned button, pressed;
                        if (!(args.read(button) && args.read(pressed))) {
                            raiseError(StringView(u8"invalid locator button"));
                        }
                        terminal.reportLocatorButton(button, pressed != 0);
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"SYNC_TIMEOUT")) {
                        terminal.expireSynchronizedOutput(true);
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"BLINK_TICK")) {
                        if (terminal.advanceAnimation(true)) {
                            terminal.redraw();
                        }
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"SELECTION_AUTOSCROLL_TICK")) {
                        terminal.advanceSelectionAutoscroll();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"SELECT_START ")) || startsWith(line, StringView(u8"SELECT_EXTEND ")) || startsWith(line, StringView(u8"SELECT_UPDATE "))) {
                        const bool start = startsWith(line, StringView(u8"SELECT_START "));
                        const bool extend = startsWith(line, StringView(u8"SELECT_EXTEND "));
                        ArgReader args(tail(line, start ? 13 : 14));
                        int column;
                        int row;
                        if (!(args.read(column) && args.read(row))) {
                            raiseError(StringView(u8"invalid selection point"));
                        }
                        unsigned cycle = 0;
                        if ((start || extend) && args.read(cycle) && cycle > 1) {
                            raiseError(StringView(u8"invalid selection cycle"));
                        }
                        const Insets selectInsets = composer.contentInsets();
                        const int pointX = selectInsets.left + column * composer.geometry.cellPixelWidth;
                        const int pointY = selectInsets.top + row * composer.geometry.cellPixelHeight;
                        if (start) {
                            terminal.selectStart(pointX, pointY, cycle != 0);
                        } else if (extend) {
                            terminal.selectExtend(pointX, pointY, cycle != 0);
                        } else {
                            terminal.selectUpdate(pointX, pointY);
                        }
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"SELECT_RECTANGULAR")) {
                        terminal.selectRectangularModeToggle();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"SELECT_FINISH")) {
                        Buffer selection;
                        terminal.selectFinish(selection);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(selection)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"SELECT_CLEAR")) {
                        testApi.selectionClear();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"HAS_SELECTION")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)testApi.hasSelection(), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"HYPERLINK "))) {
                        ArgReader args(tail(line, 10));
                        int column;
                        int row;
                        if (!(args.read(column) && args.read(row))) {
                            raiseError(StringView(u8"invalid hyperlink point"));
                        }
                        Buffer link;
                        const Insets linkInsets = composer.contentInsets();
                        terminal.getHyperlink(linkInsets.left + column, linkInsets.top + row, link);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(link)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"HYPERLINK_COUNT")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getHyperlinkCount()), StringView(u8"\n"));
                    } else if (line == StringView(u8"DESKTOP_STATE")) {
                        const unsigned linkIcon = window.pointerIcon() == plt::PointerIcon::Pointer ? 1 : 0;
                        StringBuilder output;
                        output << StringView(u8"OK ") << linkIcon << StringView(u8" ") << window.openUriCount() << StringView(u8" ") << renderer.hoveredHyperlink() << StringView(u8" ") << renderer.hoveredLinkBegin() << StringView(u8" ") << renderer.hoveredLinkEnd() << StringView(u8" ");
                        if (window.openedUri().empty()) {
                            output << StringView(u8"-");
                        } else {
                            appendHex(output, window.openedUri());
                        }
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"WINDOW_TITLE")) {
                        writeParts(controlFd, StringView(u8"OK "), HexOut{window.title()}, StringView(u8"\n"));
                    } else if (line == StringView(u8"READ_ACTIONS")) {
                        Buffer actions;
                        vtermTrace.drainActions(actions);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(actions)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"STATE")) {
                        const auto& mouse = terminal.getMouseTrackingState();
                        writeParts(controlFd, StringView(u8"OK "), (i64)((unsigned)(mouse.mode)), StringView(u8" "), (i64)((unsigned)(mouse.enc)), StringView(u8" "), (i64)(mouse.focusEventMode), StringView(u8" "), (i64)(terminal.getKittyKeyboardFlags()), StringView(u8"\n"));
                    } else if (line == StringView(u8"PROTOCOL_STATE")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getScreenReverseVideo()), StringView(u8" "), (i64)(terminal.getLedState()), StringView(u8" "), (i64)(terminal.getReverseWrapMode()), StringView(u8" "), (i64)(terminal.getNationalReplacementMode()), StringView(u8" "), (i64)(terminal.getAlternateScroll()), StringView(u8"\n"));
                    } else if (line == StringView(u8"CURSOR_STATE")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getPrivateMode(25)), StringView(u8" "), (i64)(terminal.getPrivateMode(12)), StringView(u8" "), (i64)((unsigned)(terminal.getCursorStyle())), StringView(u8"\n"));
                    } else if (line == StringView(u8"CURSOR_PENDING_WRAP")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getPendingWrap()), StringView(u8"\n"));
                    } else if (line == StringView(u8"CURSOR_AT_PROMPT")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.cursorIsAtPrompt()), StringView(u8"\n"));
                    } else if (line == StringView(u8"SEMANTIC_CLICK")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getSemanticClick()), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"ROW_SEMANTIC "))) {
                        ArgReader args(tail(line, 13));
                        int row;
                        if (!(args.read(row))) {
                            raiseError(StringView(u8"invalid semantic row"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getRowSemantic(row)), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"TAB_STOP "))) {
                        ArgReader args(tail(line, 9));
                        unsigned column;
                        if (!(args.read(column)) || column > 65535) {
                            raiseError(StringView(u8"invalid tab stop column"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.getTabStop(column)), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"TAB_STOPS "))) {
                        ArgReader args(tail(line, 10));
                        unsigned parsedColumns;
                        if (!(args.read(parsedColumns)) || parsedColumns > 65535) {
                            raiseError(StringView(u8"invalid tab stop columns"));
                        }
                        const u16 columns = parsedColumns;
                        StringBuilder output;
                        output << StringView(u8"OK ");
                        for (u16 column = 0; column < columns; ++column) {
                            output << (terminal.getTabStop(column) ? StringView(u8"1") : StringView(u8"0"));
                        }
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (startsWith(line, StringView(u8"SET_WRAPPED "))) {
                        ArgReader args(tail(line, 12));
                        unsigned row;
                        if (!(args.read(row)) || row > 65535) {
                            raiseError(StringView(u8"invalid wrapped row"));
                        }
                        terminal.setWrapped((u16)(row));
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"CONFORMANCE_STATE")) {
                        StringBuilder output;
                        output << StringView(u8"OK screen=") << (terminal.getPrivateMode(47) ? StringView(u8"Alternate") : StringView(u8"Primary")) << StringView(u8" IRM=") << (unsigned)(terminal.getAnsiMode(4)) << StringView(u8" SRM=") << (unsigned)(terminal.getAnsiMode(12)) << StringView(u8" LNM=") << (unsigned)(terminal.getAnsiMode(20)) << StringView(u8" DECCKM=") << (unsigned)(terminal.getPrivateMode(1)) << StringView(u8" DECCOLM=") << (unsigned)(terminal.getPrivateMode(3)) << StringView(u8" DECSCLM=") << (unsigned)(terminal.getPrivateMode(4)) << StringView(u8" DECSCNM=") << (unsigned)(terminal.getPrivateMode(5)) << StringView(u8" DECOM=") << (unsigned)(terminal.getPrivateMode(6)) << StringView(u8" DECAWM=") << (unsigned)(terminal.getPrivateMode(7)) << StringView(u8" DECARM=") << (unsigned)(terminal.getPrivateMode(8)) << StringView(u8" DECTCEM=") << (unsigned)(terminal.getPrivateMode(25)) << StringView(u8" DECNKM=") << (unsigned)(terminal.getPrivateMode(66)) << StringView(u8" DECBKM=") << (unsigned)(terminal.getPrivateMode(67)) << StringView(u8" DECLRMM=") << (unsigned)(terminal.getPrivateMode(69)) << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"RECTANGLE_ORIGIN")) {
                        const RectangleOrigin origin = terminal.getRectangleOrigin();
                        writeParts(controlFd, StringView(u8"OK "), (i64)(origin.rowBase), StringView(u8" "), (i64)(origin.columnBase), StringView(u8" "), (i64)(origin.rowLimit), StringView(u8" "), (i64)(origin.columnLimit), StringView(u8"\n"));
                    } else if (line == StringView(u8"PEN_STATE")) {
                        const TerminalPen pen = terminal.getPenState();
                        StringBuilder output;
                        output << StringView(u8"OK ") << cellFlags(pen.cell) << StringView(u8" ") << (unsigned)(pen.fg.red) << StringView(u8" ") << (unsigned)(pen.fg.green) << StringView(u8" ") << (unsigned)(pen.fg.blue) << StringView(u8" ") << (unsigned)(pen.bg.red) << StringView(u8" ") << (unsigned)(pen.bg.green) << StringView(u8" ") << (unsigned)(pen.bg.blue) << StringView(u8" ") << pen.cell.foreground().legacyIndex() << StringView(u8" ") << pen.cell.background().legacyIndex() << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"PARSER_TRACE_ON")) {
                        vtermTrace.clear();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"PARSER_TRACE_CLEAR")) {
                        vtermTrace.clear();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"READ_PARSER_TRACE")) {
                        Buffer events;
                        vtermTrace.drain(events);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(events)}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"UTF8_PUSH "))) {
                        Buffer pushed;
                        decodeHex(tail(line, 10), pushed);
                        Vector<u32> codepoints;
                        testUtf8Decoder.push(StringView(pushed), codepoints);
                        StringBuilder output;
                        output << StringView(u8"OK");
                        for (const u32 codepoint : codepoints) {
                            output << StringView(u8" ") << Hex{codepoint};
                        }
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"UTF8_FLUSH")) {
                        Vector<u32> codepoints;
                        testUtf8Decoder.flush(codepoints);
                        StringBuilder output;
                        output << StringView(u8"OK");
                        for (const u32 codepoint : codepoints) {
                            output << StringView(u8" ") << Hex{codepoint};
                        }
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"UTF8_RESET")) {
                        testUtf8Decoder.reset();
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"CODEPOINT_WIDTHS "))) {
                        ArgReader args(tail(line, 17));
                        char token[32];
                        StringBuilder output;
                        output << StringView(u8"OK");
                        size_t count = 0;
                        while (args.token(token, sizeof(token))) {
                            u64 codepoint = 0;
                            if (!parseU64(StringView(token), codepoint, 16) || codepoint > 0x10ffff) {
                                raiseError(StringView(u8"invalid codepoint"));
                            }
                            output << StringView(u8" ") << composer.vtConfig.config->widths.codepointWidth((u32)(codepoint));
                            ++count;
                        }
                        if (!count) {
                            raiseError(StringView(u8"empty codepoint width request"));
                        }
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"RENDER_STATE")) {
                        Buffer response;
                        renderer.renderState(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"SELECTION_STATE")) {
                        Buffer response;
                        renderer.selectionState(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"CHARSET_STATE")) {
                        const VtermTestState state = testApi.inspect();
                        writeParts(controlFd, StringView(u8"OK "), (i64)(state.charsets[0]), StringView(u8" "), (i64)(state.charsets[1]), StringView(u8" "), (i64)(state.charsets[2]), StringView(u8" "), (i64)(state.charsets[3]), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"MOUSE_ENCODE "))) {
                        ArgReader args(tail(line, 13));
                        unsigned encoding;
                        unsigned type;
                        unsigned modifiers;
                        int motionButton;
                        int button;
                        int column;
                        int row;
                        if (!(args.read(encoding) && args.read(type) && args.read(modifiers) && args.read(motionButton) && args.read(button) && args.read(column) && args.read(row)) || encoding > 4 || type > 2) {
                            raiseError(StringView(u8"invalid mouse event"));
                        }
                        StringBuilder report;
                        encodeMouseProtocol(report, (MouseTrackingEnc)(encoding), (MouseEventType)(type), modifiers, motionButton, button, column, row);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{hexview(StringView(report))}, StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"SET_PRIMARY "))) {
                        const void* found = memchr(line.data() + 12, ' ', line.length() - 12);
                        if (found == nullptr) {
                            raiseError(StringView(u8"invalid primary selection"));
                        }
                        const size_t separator = (const u8*)(found)-line.data();
                        char flag[8];
                        const size_t flagLength = min(separator - 12, sizeof(flag) - 1);
                        memcpy(flag, line.data() + 12, flagLength);
                        flag[flagLength] = '\0';
                        i64 autoCopy = 0;
                        if (!parseI64(StringView(flag), autoCopy) || autoCopy < 0 || autoCopy > 1) {
                            raiseError(StringView(u8"invalid auto-copy state"));
                        }
                        Buffer content;
                        decodeHex(tail(line, separator + 1), content);
                        const StringView selection(content);
                        clipboard.writePrimary(selection);
                        if (autoCopy) {
                            clipboard.writeClipboard(selection);
                        }
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"SET_SYSTEM "))) {
                        Buffer content;
                        decodeHex(tail(line, 11), content);
                        clipboard.writeClipboard(StringView(content));
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"SET_CLIPBOARD_CHUNK "))) {
                        ArgReader args(tail(line, 20));
                        unsigned long long size = 0;
                        if (!args.read(size)) {
                            raiseError(StringView(u8"invalid clipboard chunk size"));
                        }
                        clipboard.readChunk = (size_t)(size);
                        writeAll(controlFd, "OK\n");
                    } else if (startsWith(line, StringView(u8"GET_SELECTION "))) {
                        ArgReader args(tail(line, 14));
                        int primary = -1;
                        if (!args.read(primary) || primary < 0 || primary > 1) {
                            raiseError(StringView(u8"invalid selection kind"));
                        }
                        const StringView content = primary ? StringView(clipboard.primary) : StringView(clipboard.system);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{content}, StringView(u8"\n"));
                    } else if (line == StringView(u8"GET_CWD")) {
                        StringBuilder output;
                        output << StringView(u8"OK ");
                        appendHex(output, vtermTrace.currentCwd());
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (startsWith(line, StringView(u8"OSC7_CWD "))) {
                        Buffer input;
                        input.append("\x1b]7;", 4);
                        Buffer cwdBytes;
                        decodeHex(tail(line, 9), cwdBytes);
                        input.append(cwdBytes.data(), cwdBytes.used());
                        input.append("\x1b\\", 2);
                        terminal.feedPtyOutput((const u8*)(input.data()), input.used());
                        StringBuilder output;
                        output << StringView(u8"OK ");
                        appendHex(output, vtermTrace.currentCwd());
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (startsWith(line, StringView(u8"PRESENTED_PIXEL "))) {
                        ArgReader args(tail(line, 16));
                        u32 x = 0;
                        u32 y = 0;
                        if (!(args.read(x) && args.read(y))) {
                            raiseError(StringView(u8"invalid presented pixel request"));
                        }
                        const plt::HeadlessFrame image = window.presentedFrame();
                        if (image.pixels == nullptr || image.format != plt::HeadlessPixelFormat::RGB8 || x >= image.width || y >= image.height) {
                            raiseError(StringView(u8"presented pixel unavailable"));
                        }
                        const u8* const pixel = image.pixels + (size_t)(y)*image.stride + 3u * x;
                        writeParts(controlFd, StringView(u8"OK "), (i64)(pixel[0]), StringView(u8" "), (i64)(pixel[1]), StringView(u8" "), (i64)(pixel[2]), StringView(u8"\n"));
                    } else if (line == StringView(u8"SNAPSHOT")) {
                        Buffer response;
                        renderer.snapshot(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"MODEL_SNAPSHOT")) {
                        Buffer response;
                        renderer.modelSnapshot(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"MODEL_DIGEST")) {
                        Buffer response;
                        renderer.modelDigest(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"SCROLLBACK_STATE")) {
                        Buffer response;
                        renderer.scrollbackState(response);
                        writeAll(controlFd, StringView(response));
                    } else if (line == StringView(u8"SCREEN_TEXT")) {
                        Buffer text;
                        renderer.screenText(text);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{hexview(StringView(text))}, StringView(u8"\n"));
                    } else if (line == StringView(u8"ALL_TEXT")) {
                        Buffer contents;
                        terminal.allText(contents);
                        StringBuilder output;
                        output << StringView(u8"OK ");
                        appendHex(output, StringView(contents));
                        output << StringView(u8"\n");
                        writeAll(controlFd, StringView(output));
                    } else if (line == StringView(u8"READ_INPUT")) {
                        // Responses are written from this very process, so the
                        // authoritative "what was sent to the application" stream
                        // is the capture taken at the write call itself.  Reading
                        // it back through the kernel pty would race the
                        // asynchronous master→slave delivery, and responses larger
                        // than the pty buffer would never be visible to a single
                        // opportunistic drain.  The slave queue is drained only to
                        // unstick a stalled flush and to keep already-reported
                        // bytes from reaching a later spawned child; a running
                        // child owns the slave side.
                        waitOutputDrained();
                        if (childPid <= 0) {
                            {
                                Buffer drained;
                                drainInput(io[1], drained);
                            }
                        }
                        Buffer taken;
                        terminalPty.takeWriteData(taken);
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(taken)}, StringView(u8"\n"));
                    } else if (line == StringView(u8"FLUSH_OUTPUT")) {
                        terminal.kickOutput();
                        writeAll(controlFd, "OK\n");
                    } else if (line == StringView(u8"FLUSH_OUTPUT_RESULT")) {
                        terminal.kickOutput();
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.outputDrained()), StringView(u8"\n"));
                    } else if (line == StringView(u8"READ_WRITTEN_PTY")) {
                        writeParts(controlFd, StringView(u8"OK "), HexOut{StringView(writtenPtyData)}, StringView(u8"\n"));
                        writtenPtyData.reset();
                    } else if (line == StringView(u8"PENDING_SCRIPTED_PTY_READ_BYTES")) {
                        writeParts(controlFd, StringView(u8"OK "), (i64)(scriptedPtyReads.pendingBytes()), StringView(u8"\n"));
                    } else if (startsWith(line, StringView(u8"SERVICE_PTY "))) {
                        ArgReader args(tail(line, 12));
                        int readable;
                        int writable;
                        if (!(args.read(readable) && args.read(writable)) || readable < 0 || readable > 1 || writable < 0 || writable > 1) {
                            raiseError(StringView(u8"invalid PTY service event"));
                        }
                        writeParts(controlFd, StringView(u8"OK "), (i64)(terminal.servicePty(readable, writable)), StringView(u8"\n"));
                    } else if (line == StringView(u8"QUIT")) {
                        if (childPid > 0) {
                            kill(childPid, SIGKILL);
                            // A child blocked writing into the full slave
                            // output queue only dies once that write can
                            // finish: macOS parks it in the pty driver where
                            // even SIGKILL waits. Keep draining the master so
                            // the kill can land, and give up on a wedged
                            // driver rather than hang the whole harness.
                            const u64 reapDeadline = monotonicNowUs() + 5'000'000;
                            while (waitpid(childPid, nullptr, WNOHANG) != childPid) {
                                terminal.readPty();
                                if (monotonicNowUs() >= reapDeadline) {
                                    break;
                                }
                                pollfd corpse{io[0], POLLIN, 0};
                                poll(&corpse, 1, 10);
                            }
                            childPid = -1;
                        }
                        writeAll(controlFd, "OK\n");
                        break;
                    } else {
                        writeAll(controlFd, "ERR unknown command\n");
                    }
                } catch (Exception& error) {
                    writeParts(controlFd, StringView(u8"ERR "), error.description(), StringView(u8"\n"));
                }
                // A harness that keeps the next command buffered would otherwise
                // hold this fiber runnable forever and starve the poll loop the
                // stream transactions wait on.
                controlScheduler->yield();
            }

        } catch (Exception& error) {
            writeParts(controlFd, StringView(u8"ERR "), error.description(), StringView(u8"\n"));
        }
        composer.platform->stop();
    };
    auto controlBody = makeRunable(controlLoop);
    // Control commands run the full terminal call graph here, including
    // system font discovery and rendering, not just the protocol parser.
    constexpr size_t controlStackSize = 1024 * 1024;
    Buffer controlStack(controlStackSize);
    composer.platform->scheduler()->spawn(controlBody, controlStack.mutData(), controlStackSize);
    composer.platform->run();

    composer.pty = nullptr;
    composer.launch = nullptr;
    composer.vtermTraceFactory = nullptr;
    for (const int peer : ptyFactory.peers) {
        close(peer);
    }
    close(io[1]);
    return 0;
}
