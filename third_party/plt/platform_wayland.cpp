#include "platform_wayland.h"

#include "drop.h"
#include "fiber.h"
#include "input.h"
#include "poller.h"
#include "window.h"
#include "platform.h"
#include "loop_wake.h"
#include "poller_loop.h"
#include "pointer_grab.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol-code.h"
#include "cursor-shape-v1-client-protocol.h"
#include "viewporter-client-protocol-code.h"
#include "xdg-activation-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "tablet-unstable-v2-client-protocol-code.h"
#include "cursor-shape-v1-client-protocol-code.h"
#include "xdg-activation-v1-client-protocol-code.h"
#include "fractional-scale-v1-client-protocol-code.h"
#include "text-input-unstable-v3-client-protocol-code.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol-code.h"
#include "primary-selection-unstable-v1-client-protocol-code.h"

#include <std/sys/crt.h>
#include <std/ios/input.h>
#include <std/sym/i_map.h>
#include <std/ios/output.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/thr/poll_fd.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <new>
#include <cstring>
#include <cerrno>
#include <poll.h>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

using namespace stl;
using namespace plt;

extern char** environ;

namespace {
    struct PlatformImpl;
    struct WindowImpl;

    // One selection or drop payload as a pulling stream. The reader owns
    // it: readImpl serves the local snapshot or parks on the transfer pipe,
    // and deleting the object before end of stream cancels the transfer by
    // closing the pipe under the source.
    struct StreamInput final: public Input {
        StreamInput(PlatformImpl& platform, int fd, Buffer&& local, bool* drained);
        ~StreamInput() noexcept override;

        void operator delete(StreamInput* input, std::destroying_delete_t) noexcept;

        size_t readImpl(void* data, size_t len) override;

        PlatformImpl& platform;
        Buffer local;
        size_t offset = 0;
        int fd;
        bool* drained;
        bool eof = false;
    };

    // A replacement selection accumulating until finish() publishes it;
    // deleting the object without finish() abandons the write.
    struct StreamOutput final: public Output {
        StreamOutput(PlatformImpl& platform, bool primary);

        void operator delete(StreamOutput* output, std::destroying_delete_t) noexcept;

        size_t writeImpl(const void* data, size_t size) override;
        void finishImpl() override;

        PlatformImpl& platform;
        Buffer accumulated;
        bool primary;
        bool finished = false;
    };

    // One spawnable unit: the stack for a platform task fiber, recycled
    // through the platform free list because tasks come and go with every
    // transfer. 64K covers the deepest task, the drag session delivering a
    // drop into the client.
    struct TaskBlock {
        TaskBlock* next = nullptr;
        alignas(16) u8 stack[64 * 1024];
    };

    // A fiber body carved out of the platform allocator; releases itself
    // and recycles its stack when the fiber finishes.
    template <typename F>
    struct FiberTask;

    // The permanent key-repeat fiber: the initial delay and the cadence are
    // parkFor() waits, and any state change wakes it to re-evaluate.
    struct RepeatBody final: public Runable {
        explicit RepeatBody(PlatformImpl* platform);

        void run() override;

        PlatformImpl* platform;
    };

    constexpr u32 scaleDenominator = 120;
    // Abort a selection transfer when the peer makes no progress for this
    // long; otherwise a stalled clipboard source or consumer pins the pipe
    // descriptors and the transfer object forever.
    constexpr u64 selectionTransferTimeoutUs = 30'000'000;
    const StringView utf8Mime(u8"text/plain;charset=utf-8");
    const StringView plainMime(u8"text/plain");
    const StringView utf8StringMime(u8"UTF8_STRING");

    struct Offer {
        struct wl_data_offer* data = nullptr;
        struct zwp_primary_selection_offer_v1* primary = nullptr;
        // Every offered mime, NUL-terminated and concatenated, in offer
        // order. accept and receive must name an exact offered spelling.
        Buffer mimeData;
        u32 mimeCount = 0;
        bool utf8 = false;
        bool utf8String = false;
        bool plain = false;

        void reset();
        void addMime(const char* mime);
        const char* mimeAt(size_t index) const;
        const char* offered(StringView mime) const;
        const char* mime() const;
    };

    // The DropOffer view over the drag session's Offer, valid for the
    // duration of one DropTarget callback.
    struct DndOfferView final: public DropOffer {
        size_t formats() const override;
        StringView format(size_t index) const override;

        const Offer* offer = nullptr;
    };

    struct DndDrop final: public Drop {
        DropOffer* what() override;
        Input* read(StringView mime) override;

        PlatformImpl* platform = nullptr;
        DndOfferView* view = nullptr;
        struct wl_data_offer* offer = nullptr;
        bool started = false;
        bool drained = false;
    };

    // One drag session, owned by the stack of its fiber. The platform points
    // at it only during the hover phase and feeds it events; the fiber
    // consumes them between parks and runs the drop transfer inline.
    struct DndSession {
        WindowImpl* window = nullptr;
        Offer offer;
        Fiber* fiber = nullptr;
        u32 serial = 0;
        wl_fixed_t motionX = 0;
        wl_fixed_t motionY = 0;
        bool motionPending = false;
        bool dropPending = false;
        bool leavePending = false;
    };

    struct ClipboardImpl final: public Clipboard {
        Input* read() override;
        Output* write() override;

        WindowImpl* window = nullptr;
        bool primary = false;
    };

    struct WindowImpl final: public Window, public TimerCallback {
        WindowImpl(PlatformImpl& platform, const WindowOptions& options);
        ~WindowImpl();

        void requestShow() override;
        void requestClose() override;
        void requestFrame() override;
        void ready() override;
        void requestTitle(StringView title) override;
        void requestAttention() override;
        void requestRestore() override;
        void requestIconify() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestResize(u32 width, u32 height) override;
        void requestMinimumSize(u32 width, u32 height) override;
        void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        WindowInfo info() const override;
        bool inLiveResize() const override;
        Clipboard* primary() override;
        Clipboard* secondary() override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestOpenUri(StringView uri) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        RenderContext renderContext() const override;

        void configure();
        void contentScale(u32 numerator);
        void pointerEntered(u32 serial, wl_fixed_t x, wl_fixed_t y);
        void pointerLeft();
        void pointerMoved(wl_fixed_t x, wl_fixed_t y);
        void pointerButton(u32 time, u32 button, u32 state);
        void pointerAxis(u32 axis, wl_fixed_t value);
        void pointerAxisSteps(u32 axis, i32 value120);
        void pointerFrame();
        void frameReady(struct wl_callback* callback);
        void cancelFrame();
        void updateCursor();
        u32 pixelWidth() const;
        u32 pixelHeight() const;
        u32 logicalForPixel(u32 pixels) const;
        i32 logicalCoordinate(i32 pixels) const;
        u32 snappedLogical(u32 suggested, u32 unit, u32 base) const;
        void setLogicalSize(u32 width, u32 height);

        PlatformImpl& platform;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        DropTarget* dropTarget = nullptr;
        struct wl_surface* surface = nullptr;
        struct xdg_surface* xdgSurface = nullptr;
        struct xdg_toplevel* toplevel = nullptr;
        struct zxdg_toplevel_decoration_v1* decoration = nullptr;
        struct wp_viewport* viewport = nullptr;
        struct wp_fractional_scale_v1* fractionalScale = nullptr;
        struct wl_callback* frameCallback = nullptr;
        struct xdg_activation_token_v1* activationToken = nullptr;
        ClipboardImpl primarySelection;
        ClipboardImpl clipboardSelection;
        Buffer title;
        u32 logicalWidth = 1;
        u32 logicalHeight = 1;
        u32 pendingWidth = 0;
        u32 pendingHeight = 0;
        u32 scaleNumerator = scaleDenominator;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        u32 resizeUnitWidth = 1;
        u32 resizeUnitHeight = 1;
        u32 resizeBaseWidth = 0;
        u32 resizeBaseHeight = 0;
        i32 pointerX = 0;
        i32 pointerY = 0;
        double scrollX = 0;
        double scrollY = 0;
        i32 scrollStepsX = 0;
        i32 scrollStepsY = 0;
        i32 textInputX = 0;
        i32 textInputY = 0;
        u32 textInputWidth = 0;
        u32 textInputHeight = 0;
        PointerIcon cursor = PointerIcon::Text;
        bool shown = false;
        bool configured = false;
        bool closeRequested = false;
        bool focused = false;
        bool maximized = false;
        bool fullscreen = false;
        bool tiled = false;
        bool pendingFocused = false;
        bool pendingMaximized = false;
        bool pendingFullscreen = false;
        bool pendingTiled = false;
        bool frameRequested = false;
        bool frameScheduled = false;
        u32 frameRetries = 0;
    };

    struct PlatformImpl final: public Platform, public PollCallback {
        explicit PlatformImpl(ObjPool& owner);
        ~PlatformImpl();

        Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
        LoopWake* createLoopWake(ObjPool& owner, TimerCallback& callback) override;
        Poller* poller() override;
        Scheduler* scheduler() override;
        void run() override;
        void stop() override;
        void ready(PollFD event) override;

        void bindRegistry(u32 name, const char* interface, u32 version);
        void globalRemoved(u32 name);
        void seatCapabilities(u32 capabilities);
        void createSelectionDevices();
        void armDisplay(bool write);
        bool flushDisplay();
        void dispatch();
        void serial(u32 value);
        void keyboardKey(u32 serial, u32 time, u32 key, u32 state, bool repeated = false);
        bool consumeEnterPressedKey(u32 key, u32 state);
        void repeat();
        void stopRepeat();
        u16 modifiers() const;
        InputKey inputKey(xkb_keysym_t symbol) const;
        u32 keymapCodepoint(xkb_keycode_t key, xkb_layout_index_t layout) const;
        u32 layoutCodepoint(xkb_keycode_t key) const;
        u32 baseCodepoint(xkb_keycode_t key) const;
        bool composing() const;
        size_t composeFeed(xkb_keysym_t symbol, u32 codepoint, u32* codepoints, size_t capacity);
        void applyClipboardSelection();
        void applyPrimarySelection();
        void dragEntered(u32 serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* offer);
        void dragMoved(wl_fixed_t x, wl_fixed_t y);
        void dragLeft();
        void dragDropped();
        void runDragSession(DndSession& session);
        void runDropTransfer(DndSession& session);
        void setClipboard(StringView content);
        void setPrimary(StringView content);
        void setCursor(WindowImpl& window);
        void activate(WindowImpl& window);
        void writeSelection(int fd, StringView content);

        template <typename F>
        void spawnTask(F body) {
            TaskBlock* const block = takeTaskBlock();
            scheduler_->spawn(*allocator_->make<FiberTask<F>>(*this, block, body), block->stack, sizeof(block->stack));
        }
        TaskBlock* takeTaskBlock();
        void recycleTaskBlock(TaskBlock* block);
        void enableTextInput(WindowImpl& window);
        void disableTextInput();
        void textInputEntered(struct wl_surface* surface);
        void textInputLeft(struct wl_surface* surface);
        void textInputDone();
        void textInputRectChanged(WindowImpl& window, bool commit);

        PollerLoop* poller_ = nullptr;
        PollWaiter displayWaiter_;
        ObjPool* owner_ = nullptr;
        SmallObjAllocator* allocator_ = nullptr;
        Scheduler* scheduler_ = nullptr;
        TaskBlock* taskBlocks_ = nullptr;
        struct wl_display* display = nullptr;
        struct wl_registry* registry = nullptr;
        struct wl_compositor* compositor = nullptr;
        struct xdg_wm_base* wmBase = nullptr;
        struct wl_seat* seat = nullptr;
        struct wl_keyboard* keyboard = nullptr;
        struct wl_pointer* pointer = nullptr;
        struct wl_data_device_manager* dataDeviceManager = nullptr;
        struct wl_data_device* dataDevice = nullptr;
        struct wl_data_source* clipboardSource = nullptr;
        struct zwp_primary_selection_device_manager_v1* primaryManager = nullptr;
        struct zwp_primary_selection_device_v1* primaryDevice = nullptr;
        struct zwp_primary_selection_source_v1* primarySource = nullptr;
        struct wp_viewporter* viewporter = nullptr;
        struct wp_fractional_scale_manager_v1* fractionalScaleManager = nullptr;
        struct zxdg_decoration_manager_v1* decorationManager = nullptr;
        struct xdg_activation_v1* activation = nullptr;
        struct wp_cursor_shape_manager_v1* cursorShapeManager = nullptr;
        struct wp_cursor_shape_device_v1* cursorShapeDevice = nullptr;
        u32 cursorShapeVersion = 0;
        struct wl_output* output = nullptr;
        struct zwp_text_input_manager_v3* textInputManager = nullptr;
        struct zwp_text_input_v3* textInput = nullptr;
        WindowImpl* textInputWindow = nullptr;
        struct xkb_context* xkbContext = nullptr;
        struct xkb_keymap* keymap = nullptr;
        struct xkb_state* xkbState = nullptr;
        struct xkb_compose_table* composeTable = nullptr;
        struct xkb_compose_state* composeState = nullptr;
        WindowImpl* keyboardFocus = nullptr;
        PointerGrab pointerGrab;
        Vector<u32> enterPressedKeys;
        Buffer pendingPreeditText;
        Buffer pendingCommitText;
        i32 pendingPreeditCursorBegin = -1;
        i32 pendingPreeditCursorEnd = -1;
        bool pendingPreedit = false;
        bool pendingCommit = false;
        bool preeditVisible = false;
        WindowImpl* repeatWindow = nullptr;
        u32 repeatKeycode = 0;
        u32 repeatSerial = 0;
        u32 repeatTime = 0;
        u32 repeatRate = 0;
        u32 repeatDelay = 0;
        RepeatBody repeatBody_{this};
        Fiber* repeatFiber_ = nullptr;
        alignas(16) u8 repeatStack_[lightFiberStack];
        u32 latestSerial = 0;
        u32 pointerEnterSerial = 0;
        u32 seatName = 0;
        u32 outputName = 0;
        u32 outputWidth = 0;
        u32 outputHeight = 0;
        i32 outputScale = 1;
        Offer pendingClipboardOffer;
        Offer clipboardOffer;
        Offer pendingPrimaryOffer;
        Offer primaryOffer;
        DndSession* dndSession = nullptr;
        Buffer clipboardContent;
        Buffer primaryContent;
        bool clipboardPending = false;
        bool primaryPending = false;
        bool stopped = false;
    };

    template <typename F>
    struct FiberTask final: public Runable {
        FiberTask(PlatformImpl& platform_, TaskBlock* block_, F body_)
            : platform(platform_)
            , block(block_)
            , body(body_)
        {
        }

        void run() override {
            body();
            PlatformImpl& owner = platform;
            TaskBlock* const spent = block;
            owner.allocator_->release(this);
            // Still running on spent's stack: safe, nothing can reuse it
            // before the final cooperative switch out.
            owner.recycleTaskBlock(spent);
        }

        PlatformImpl& platform;
        TaskBlock* block;
        F body;
    };

    TaskBlock* PlatformImpl::takeTaskBlock() {
        TaskBlock* block = taskBlocks_;
        if (block != nullptr) {
            taskBlocks_ = block->next;
            block->next = nullptr;
            return block;
        }
        return owner_->make<TaskBlock>();
    }

    void PlatformImpl::recycleTaskBlock(TaskBlock* block) {
        block->next = taskBlocks_;
        taskBlocks_ = block;
    }

    bool textMime(const char* mime) {
        const StringView value(mime);
        return value == utf8Mime || value == plainMime || value == utf8StringMime;
    }

    ssize_t writeNoSignal(int fd, const void* data, size_t size) {
        sigset_t blocked;
        sigset_t previous;
        sigset_t pending;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        const int maskError = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
        if (maskError != 0) {
            errno = maskError;
            return -1;
        }
        bool wasPending = false;
        if (sigpending(&pending) == 0) {
            wasPending = sigismember(&pending, SIGPIPE) == 1;
        }
        const ssize_t result = write(fd, data, size);
        const int writeError = errno;
        if (result < 0 && writeError == EPIPE && !wasPending) {
            const struct timespec timeout{};
            while (sigtimedwait(&blocked, nullptr, &timeout) < 0 && errno == EINTR) {
            }
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (result < 0) {
            errno = writeError;
        }
        return result;
    }

    // Decodes one UTF-8 sequence; returns the bytes consumed, or 0 when the
    // input starts with an invalid byte which the caller should skip.
    size_t decodeUtf8One(const u8* bytes, size_t length, u32* value) {
        const u8 lead = bytes[0];
        u32 decoded;
        size_t continuations;
        if (lead < 0x80) {
            decoded = lead;
            continuations = 0;
        } else if ((lead & 0xe0) == 0xc0) {
            decoded = lead & 0x1f;
            continuations = 1;
        } else if ((lead & 0xf0) == 0xe0) {
            decoded = lead & 0x0f;
            continuations = 2;
        } else if ((lead & 0xf8) == 0xf0) {
            decoded = lead & 0x07;
            continuations = 3;
        } else {
            return 0;
        }
        if (length <= continuations) {
            return 0;
        }
        for (size_t offset = 1; offset <= continuations; ++offset) {
            const u8 continuation = bytes[offset];
            if ((continuation & 0xc0) != 0x80) {
                return 0;
            }
            decoded = (decoded << 6) | (continuation & 0x3f);
        }
        // Reject overlong forms, UTF-16 surrogates, and values past
        // U+10FFFF: these bytes come from the compositor and would
        // otherwise reach the pty as invalid scalars.
        static constexpr u32 minimums[4] = {0, 0x80, 0x800, 0x10000};
        if (decoded < minimums[continuations] || decoded > 0x10ffff || (decoded >= 0xd800 && decoded <= 0xdfff)) {
            return 0;
        }
        *value = decoded;
        return continuations + 1;
    }

    size_t decodeUtf8(const u8* bytes, size_t length, u32* codepoints, size_t capacity) {
        size_t count = 0;
        for (size_t index = 0; index != length && count != capacity;) {
            u32 value;
            const size_t consumed = decodeUtf8One(bytes + index, length - index, &value);
            if (consumed == 0) {
                ++index;
                continue;
            }
            index += consumed;
            codepoints[count++] = value;
        }
        return count;
    }

    void releaseKeyboard(struct wl_keyboard* keyboard) {
        if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
            wl_keyboard_release(keyboard);
        } else {
            wl_keyboard_destroy(keyboard);
        }
    }

    void releasePointer(struct wl_pointer* pointer) {
        if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION) {
            wl_pointer_release(pointer);
        } else {
            wl_pointer_destroy(pointer);
        }
    }

    void releaseSeat(struct wl_seat* seat) {
        if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(seat);
        } else {
            wl_seat_destroy(seat);
        }
    }

    void releaseDataDevice(struct wl_data_device* device) {
        if (wl_data_device_get_version(device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
            wl_data_device_release(device);
        } else {
            wl_data_device_destroy(device);
        }
    }

    void releaseOutput(struct wl_output* output) {
        if (wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
            wl_output_release(output);
        } else {
            wl_output_destroy(output);
        }
    }

    [[noreturn]]
    void fail(StringView message) {
        Errno(errno == 0 ? EINVAL : errno).raise(message);
    }

    void offerMime(Offer& offer, const char* mime) {
        offer.addMime(mime);
        if (!textMime(mime)) {
            return;
        }
        if (StringView(mime) == utf8Mime) {
            offer.utf8 = true;
        } else if (StringView(mime) == utf8StringMime) {
            offer.utf8String = true;
        } else {
            offer.plain = true;
        }
    }

    void dataOfferOffer(void* data, struct wl_data_offer*, const char* mime) {
        offerMime(*(Offer*)(data), mime);
    }

    const struct wl_data_offer_listener dataOfferListener{
        .offer = dataOfferOffer,
        .source_actions = [](void*, struct wl_data_offer*, u32) {},
        .action = [](void*, struct wl_data_offer*, u32) {},
    };

    void primaryOfferOffer(void* data, struct zwp_primary_selection_offer_v1*, const char* mime) {
        offerMime(*(Offer*)(data), mime);
    }

    const struct zwp_primary_selection_offer_v1_listener primaryOfferListener{
        .offer = primaryOfferOffer,
    };

    void dataSourceTarget(void*, struct wl_data_source*, const char*) {
    }

    void dataSourceSend(void* data, struct wl_data_source*, const char*, int fd) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.writeSelection(fd, StringView(platform.clipboardContent));
    }

    void dataSourceCancelled(void* data, struct wl_data_source* source) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        if (platform.clipboardSource == source) {
            platform.clipboardSource = nullptr;
        }
        wl_data_source_destroy(source);
    }

    const struct wl_data_source_listener dataSourceListener{
        .target = dataSourceTarget,
        .send = dataSourceSend,
        .cancelled = dataSourceCancelled,
        .dnd_drop_performed = [](void*, struct wl_data_source*) {},
        .dnd_finished = [](void*, struct wl_data_source*) {},
        .action = [](void*, struct wl_data_source*, u32) {},
    };

    void primarySourceSend(void* data, struct zwp_primary_selection_source_v1*, const char*, int fd) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.writeSelection(fd, StringView(platform.primaryContent));
    }

    void primarySourceCancelled(void* data, struct zwp_primary_selection_source_v1* source) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        if (platform.primarySource == source) {
            platform.primarySource = nullptr;
        }
        zwp_primary_selection_source_v1_destroy(source);
    }

    const struct zwp_primary_selection_source_v1_listener primarySourceListener{
        .send = primarySourceSend,
        .cancelled = primarySourceCancelled,
    };

    void dataDeviceDataOffer(void* data, struct wl_data_device*, struct wl_data_offer* proxy) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.pendingClipboardOffer.reset();
        platform.pendingClipboardOffer.data = proxy;
        wl_data_offer_add_listener(proxy, &dataOfferListener, &platform.pendingClipboardOffer);
    }

    void dataDeviceSelection(void* data, struct wl_data_device*, struct wl_data_offer* proxy) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.clipboardOffer.reset();
        if (proxy == nullptr) {
            return;
        }
        if (platform.pendingClipboardOffer.data == proxy) {
            platform.clipboardOffer = platform.pendingClipboardOffer;
            platform.pendingClipboardOffer = {};
        } else {
            platform.clipboardOffer.data = proxy;
            wl_data_offer_add_listener(proxy, &dataOfferListener, &platform.clipboardOffer);
        }
    }

    const struct wl_data_device_listener dataDeviceListener{
        .data_offer = dataDeviceDataOffer,
        .enter =
            [](void* data, struct wl_data_device*, u32 serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* offer) {
        ((PlatformImpl*)(data))->dragEntered(serial, surface, x, y, offer);
    },
        .leave =
            [](void* data, struct wl_data_device*) {
        ((PlatformImpl*)(data))->dragLeft();
    },
        .motion =
            [](void* data, struct wl_data_device*, u32, wl_fixed_t x, wl_fixed_t y) {
        ((PlatformImpl*)(data))->dragMoved(x, y);
    },
        .drop =
            [](void* data, struct wl_data_device*) {
        ((PlatformImpl*)(data))->dragDropped();
    },
        .selection = dataDeviceSelection,
    };

    void primaryDeviceDataOffer(void* data, struct zwp_primary_selection_device_v1*, struct zwp_primary_selection_offer_v1* proxy) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.pendingPrimaryOffer.reset();
        platform.pendingPrimaryOffer.primary = proxy;
        zwp_primary_selection_offer_v1_add_listener(proxy, &primaryOfferListener, &platform.pendingPrimaryOffer);
    }

    void primaryDeviceSelection(void* data, struct zwp_primary_selection_device_v1*, struct zwp_primary_selection_offer_v1* proxy) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.primaryOffer.reset();
        if (proxy == nullptr) {
            return;
        }
        if (platform.pendingPrimaryOffer.primary == proxy) {
            platform.primaryOffer = platform.pendingPrimaryOffer;
            platform.pendingPrimaryOffer = {};
        } else {
            platform.primaryOffer.primary = proxy;
            zwp_primary_selection_offer_v1_add_listener(proxy, &primaryOfferListener, &platform.primaryOffer);
        }
    }

    const struct zwp_primary_selection_device_v1_listener primaryDeviceListener{
        .data_offer = primaryDeviceDataOffer,
        .selection = primaryDeviceSelection,
    };

    void keyboardKeymap(void* data, struct wl_keyboard*, u32 format, int fd, u32 size) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
            close(fd);
            return;
        }
        void* const mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapping == MAP_FAILED) {
            return;
        }
        struct xkb_keymap* const keymap = xkb_keymap_new_from_string(platform.xkbContext, (const char*)(mapping), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(mapping, size);
        if (keymap == nullptr) {
            return;
        }
        struct xkb_state* const state = xkb_state_new(keymap);
        if (state == nullptr) {
            xkb_keymap_unref(keymap);
            return;
        }
        if (platform.xkbState != nullptr) {
            xkb_state_unref(platform.xkbState);
        }
        if (platform.keymap != nullptr) {
            xkb_keymap_unref(platform.keymap);
        }
        platform.keymap = keymap;
        platform.xkbState = state;
    }

    void keyboardEnter(void* data, struct wl_keyboard*, u32 serial, struct wl_surface* surface, struct wl_array* keys) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.serial(serial);
        // A destroyed surface arrives as a null proxy; the window teardown
        // already dropped the focus state it would have cleared here.
        if (surface == nullptr) {
            return;
        }
        platform.keyboardFocus = (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
        // Keys held while focus arrives update state only; delivering them as
        // presses would type the key that switched focus into the terminal.
        // Their eventual releases are suppressed to match.
        platform.enterPressedKeys.clear();
        if (keys != nullptr) {
            const u32* key = (const u32*)(keys->data);
            const u32* const keysEnd = key + keys->size / sizeof(*key);
            for (; key != keysEnd; ++key) {
                platform.enterPressedKeys.pushBack(*key);
            }
        }
        if (platform.keyboardFocus != nullptr) {
            platform.keyboardFocus->focused = true;
            platform.keyboardFocus->requestFrame();
            if (platform.keyboardFocus->input != nullptr) {
                platform.keyboardFocus->input->focus(true);
                platform.keyboardFocus->input->flush();
            }
        }
    }

    void keyboardLeave(void* data, struct wl_keyboard*, u32 serial, struct wl_surface* surface) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.serial(serial);
        platform.enterPressedKeys.clear();
        if (platform.composeState != nullptr) {
            xkb_compose_state_reset(platform.composeState);
        }
        WindowImpl* const window = surface == nullptr ? nullptr : (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
        if (window != nullptr) {
            window->focused = false;
            window->requestFrame();
            if (window->input != nullptr) {
                window->input->focus(false);
                window->input->flush();
            }
        }
        if (surface == nullptr || platform.keyboardFocus == window) {
            platform.keyboardFocus = nullptr;
        }
        platform.stopRepeat();
    }

    void keyboardKey(void* data, struct wl_keyboard*, u32 serial, u32 time, u32 key, u32 state) {
        ((PlatformImpl*)(data))->keyboardKey(serial, time, key, state);
    }

    void keyboardModifiers(void* data, struct wl_keyboard*, u32 serial, u32 depressed, u32 latched, u32 locked, u32 group) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.serial(serial);
        if (platform.xkbState != nullptr) {
            xkb_state_update_mask(platform.xkbState, depressed, latched, locked, 0, 0, group);
        }
    }

    void keyboardRepeatInfo(void* data, struct wl_keyboard*, i32 rate, i32 delay) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.repeatRate = rate > 0 ? (u32)(rate) : 0;
        platform.repeatDelay = delay > 0 ? (u32)(delay) : 0;
    }

    const struct wl_keyboard_listener keyboardListener{
        .keymap = keyboardKeymap,
        .enter = keyboardEnter,
        .leave = keyboardLeave,
        .key = keyboardKey,
        .modifiers = keyboardModifiers,
        .repeat_info = keyboardRepeatInfo,
    };

    void pointerFrame(void* data, struct wl_pointer*) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
        if (window != nullptr) {
            window->pointerFrame();
        }
    }

    // Seats below version 5 never send wl_pointer.frame, so flush after every
    // event which would otherwise wait for the end of the frame.
    void pointerFrameFallback(PlatformImpl& platform) {
        if (platform.pointer != nullptr && wl_pointer_get_version(platform.pointer) < WL_POINTER_FRAME_SINCE_VERSION) {
            pointerFrame(&platform, nullptr);
        }
    }

    void pointerEnter(void* data, struct wl_pointer*, u32 serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.serial(serial);
        platform.pointerEnterSerial = serial;
        WindowImpl* const window = surface == nullptr ? nullptr : (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
        platform.pointerGrab.enter(window);
        if (window != nullptr) {
            window->pointerEntered(serial, x, y);
        }
        pointerFrameFallback(platform);
    }

    void pointerLeave(void* data, struct wl_pointer*, u32 serial, struct wl_surface* surface) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.serial(serial);
        platform.pointerEnterSerial = 0;
        WindowImpl* const window = surface == nullptr ? nullptr : (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
        if (window != nullptr) {
            window->pointerLeft();
        }
        platform.pointerGrab.leave(window);
        pointerFrameFallback(platform);
    }

    void pointerMotion(void* data, struct wl_pointer*, u32, wl_fixed_t x, wl_fixed_t y) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
        if (window != nullptr) {
            window->pointerMoved(x, y);
        }
        pointerFrameFallback(platform);
    }

    void pointerButton(void* data, struct wl_pointer*, u32 serial, u32 time, u32 button, u32 state) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.serial(serial);
        WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.buttonTarget(state == WL_POINTER_BUTTON_STATE_PRESSED));
        if (window != nullptr) {
            window->pointerButton(time, button, state);
        }
        pointerFrameFallback(platform);
    }

    void pointerAxis(void* data, struct wl_pointer*, u32, u32 axis, wl_fixed_t value) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
        if (window != nullptr) {
            window->pointerAxis(axis, value);
        }
        pointerFrameFallback(platform);
    }

    void pointerAxisSteps(void* data, struct wl_pointer*, u32 axis, i32 value120) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
        if (window != nullptr) {
            window->pointerAxisSteps(axis, value120);
        }
    }

    const struct wl_pointer_listener pointerListener{
        .enter = pointerEnter,
        .leave = pointerLeave,
        .motion = pointerMotion,
        .button = pointerButton,
        .axis = pointerAxis,
        .frame = pointerFrame,
        .axis_source = [](void*, struct wl_pointer*, u32) {},
        .axis_stop = [](void*, struct wl_pointer*, u32, u32) {},
        .axis_discrete =
            [](void* data, struct wl_pointer* pointer, u32 axis, i32 discrete) {
        pointerAxisSteps(data, pointer, axis, discrete * 120);
    },
        .axis_value120 = pointerAxisSteps,
        .axis_relative_direction = [](void*, struct wl_pointer*, u32, u32) {},
        #if defined(WL_POINTER_WARP_SINCE_VERSION)
            .warp = [](void* data, struct wl_pointer*, wl_fixed_t x, wl_fixed_t y) {
            pointerMotion(data, nullptr, 0, x, y);
        },
        #endif
    };

    void seatCapabilities(void* data, struct wl_seat*, u32 capabilities) {
        ((PlatformImpl*)(data))->seatCapabilities(capabilities);
    }

    const struct wl_seat_listener seatListener{
        .capabilities = seatCapabilities,
        .name = [](void*, struct wl_seat*, const char*) {},
    };

    void outputMode(void* data, struct wl_output*, u32 flags, i32 width, i32 height, i32) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            platform.outputWidth = width > 0 ? (u32)(width) : 0;
            platform.outputHeight = height > 0 ? (u32)(height) : 0;
        }
    }

    void outputScale(void* data, struct wl_output*, i32 scale) {
        ((PlatformImpl*)(data))->outputScale = max(1, scale);
    }

    const struct wl_output_listener outputListener{
        .geometry = [](void*, struct wl_output*, i32, i32, i32, i32, i32, const char*, const char*, i32) {},
        .mode = outputMode,
        .done = [](void*, struct wl_output*) {},
        .scale = outputScale,
        .name = [](void*, struct wl_output*, const char*) {},
        .description = [](void*, struct wl_output*, const char*) {},
    };

    void registryGlobal(void* data, struct wl_registry*, u32 name, const char* interface, u32 version) {
        ((PlatformImpl*)(data))->bindRegistry(name, interface, version);
    }

    const struct wl_registry_listener registryListener{
        .global = registryGlobal,
        .global_remove = [](void* data, struct wl_registry*, u32 name) {
        ((PlatformImpl*)(data))->globalRemoved(name);
    },
    };

    const struct xdg_wm_base_listener wmBaseListener{
        .ping = [](void*, struct xdg_wm_base* wmBase, u32 serial) {
        xdg_wm_base_pong(wmBase, serial);
    },
    };

    void surfaceEnter(void* data, struct wl_surface*, struct wl_output*) {
        WindowImpl& window = *(WindowImpl*)(data);
        if (window.fractionalScale == nullptr) {
            window.contentScale((u32)(window.platform.outputScale) * scaleDenominator);
        }
    }

    const struct wl_surface_listener surfaceListener{
        .enter = surfaceEnter,
        .leave = [](void*, struct wl_surface*, struct wl_output*) {},
        .preferred_buffer_scale =
            [](void* data, struct wl_surface*, i32 scale) {
        WindowImpl& window = *(WindowImpl*)(data);
        if (window.fractionalScale == nullptr) {
            window.contentScale((u32)(max(1, scale)) * scaleDenominator);
        }
    },
        .preferred_buffer_transform = [](void*, struct wl_surface*, u32) {},
    };

    void toplevelConfigure(void* data, struct xdg_toplevel*, i32 width, i32 height, struct wl_array* states) {
        WindowImpl& window = *(WindowImpl*)(data);
        window.pendingFocused = false;
        window.pendingMaximized = false;
        window.pendingFullscreen = false;
        window.pendingTiled = false;
        const u32* state = (const u32*)(states->data);
        const u32* const stateEnd = state + states->size / sizeof(*state);
        for (; state != stateEnd; ++state) {
            switch (*state) {
                case XDG_TOPLEVEL_STATE_ACTIVATED:
                    window.pendingFocused = true;
                    break;
                case XDG_TOPLEVEL_STATE_MAXIMIZED:
                    window.pendingMaximized = true;
                    break;
                case XDG_TOPLEVEL_STATE_FULLSCREEN:
                    window.pendingFullscreen = true;
                    break;
                case XDG_TOPLEVEL_STATE_TILED_LEFT:
                case XDG_TOPLEVEL_STATE_TILED_RIGHT:
                case XDG_TOPLEVEL_STATE_TILED_TOP:
                case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
                    window.pendingTiled = true;
                    break;
                default:
                    break;
            }
        }
        window.pendingWidth = width > 0 ? (u32)(width) : window.logicalWidth;
        window.pendingHeight = height > 0 ? (u32)(height) : window.logicalHeight;
    }

    const struct xdg_toplevel_listener toplevelListener{
        .configure = toplevelConfigure,
        .close =
            [](void* data, struct xdg_toplevel*) {
        ((WindowImpl*)(data))->requestClose();
    },
        .configure_bounds = [](void*, struct xdg_toplevel*, i32, i32) {},
        .wm_capabilities = [](void*, struct xdg_toplevel*, struct wl_array*) {},
    };

    const struct xdg_surface_listener xdgSurfaceListener{
        .configure = [](void* data, struct xdg_surface* surface, u32 serial) {
        xdg_surface_ack_configure(surface, serial);
        ((WindowImpl*)(data))->configure();
    },
    };

    const struct wp_fractional_scale_v1_listener fractionalScaleListener{
        .preferred_scale = [](void* data, struct wp_fractional_scale_v1*, u32 numerator) {
        ((WindowImpl*)(data))->contentScale(numerator);
    },
    };

    const struct wl_callback_listener frameListener{
        .done = [](void* data, struct wl_callback* callback, u32) {
        ((WindowImpl*)(data))->frameReady(callback);
    },
    };

    const struct xdg_activation_token_v1_listener activationTokenListener{
        .done = [](void* data, struct xdg_activation_token_v1* token, const char* value) {
        WindowImpl& window = *(WindowImpl*)(data);
        if (window.activationToken != token) {
            xdg_activation_token_v1_destroy(token);
            return;
        }
        if (window.platform.activation != nullptr) {
            xdg_activation_v1_activate(window.platform.activation, value, window.surface);
        }
        xdg_activation_token_v1_destroy(token);
        window.activationToken = nullptr;
    },
    };

    void textInputPreedit(void* data, struct zwp_text_input_v3*, const char* text, i32 cursorBegin, i32 cursorEnd) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.pendingPreeditText.reset();
        if (text != nullptr) {
            const StringView value(text);
            platform.pendingPreeditText.append(value.data(), value.length());
        }
        platform.pendingPreeditCursorBegin = cursorBegin;
        platform.pendingPreeditCursorEnd = cursorEnd;
        platform.pendingPreedit = true;
    }

    void textInputCommit(void* data, struct zwp_text_input_v3*, const char* text) {
        PlatformImpl& platform = *(PlatformImpl*)(data);
        platform.pendingCommitText.reset();
        if (text != nullptr) {
            const StringView value(text);
            platform.pendingCommitText.append(value.data(), value.length());
        }
        platform.pendingCommit = true;
    }

    const struct zwp_text_input_v3_listener textInputListener{
        .enter =
            [](void* data, struct zwp_text_input_v3*, struct wl_surface* surface) {
        ((PlatformImpl*)(data))->textInputEntered(surface);
    },
        .leave =
            [](void* data, struct zwp_text_input_v3*, struct wl_surface* surface) {
        ((PlatformImpl*)(data))->textInputLeft(surface);
    },
        .preedit_string = textInputPreedit,
        .commit_string = textInputCommit,
        // A terminal has no surrounding text for the input method to delete.
        .delete_surrounding_text = [](void*, struct zwp_text_input_v3*, u32, u32) {},
        .done =
            [](void* data, struct zwp_text_input_v3*, u32) {
        ((PlatformImpl*)(data))->textInputDone();
    },
        #if defined(ZWP_TEXT_INPUT_V3_ACTION_SINCE_VERSION)
            // Version 2 events; never delivered because the manager is bound
            // at version 1.
            .action = [](void*, struct zwp_text_input_v3*, u32, u32) {},
            .language = [](void*, struct zwp_text_input_v3*, const char*) {},
            .preedit_hint = [](void*, struct zwp_text_input_v3*, u32, u32, u32) {},
        #endif
    };

    u32 cursorShape(PointerIcon icon, u32 version) {
        switch (icon) {
            case PointerIcon::Default:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
            case PointerIcon::ContextMenu:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU;
            case PointerIcon::Help:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP;
            case PointerIcon::Pointer:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
            case PointerIcon::Progress:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
            case PointerIcon::Wait:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
            case PointerIcon::Cell:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL;
            case PointerIcon::Crosshair:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
            case PointerIcon::Text:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
            case PointerIcon::VerticalText:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT;
            case PointerIcon::Alias:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS;
            case PointerIcon::Copy:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY;
            case PointerIcon::Move:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
            case PointerIcon::NoDrop:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
            case PointerIcon::NotAllowed:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
            case PointerIcon::Grab:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
            case PointerIcon::Grabbing:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
            case PointerIcon::ResizeEast:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
            case PointerIcon::ResizeNorth:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
            case PointerIcon::ResizeNorthEast:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
            case PointerIcon::ResizeNorthWest:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
            case PointerIcon::ResizeSouth:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
            case PointerIcon::ResizeSouthEast:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
            case PointerIcon::ResizeSouthWest:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
            case PointerIcon::ResizeWest:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
            case PointerIcon::ResizeEastWest:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
            case PointerIcon::ResizeNorthSouth:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
            case PointerIcon::ResizeNorthEastSouthWest:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
            case PointerIcon::ResizeNorthWestSouthEast:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
            case PointerIcon::ResizeColumn:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
            case PointerIcon::ResizeRow:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE;
            case PointerIcon::AllScroll:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
            case PointerIcon::ZoomIn:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN;
            case PointerIcon::ZoomOut:
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT;
            case PointerIcon::DndAsk:
                // dnd_ask exists since cursor-shape v2; a v1 compositor gets
                // the copy shape, the usual visual for an undecided drag.
                if (version >= 2) {
                    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK;
                }
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY;
            case PointerIcon::ResizeAll:
                // all_resize exists since cursor-shape v2; move is the closest
                // omnidirectional shape a v1 compositor offers.
                if (version >= 2) {
                    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE;
                }
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
            case PointerIcon::DisappearingItem:
                // Cocoa-only poof cursor: the item vanishes when dropped, so
                // no-drop carries the closest meaning.
                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
        }
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

void Offer::reset() {
    if (data != nullptr) {
        wl_data_offer_destroy(data);
    }
    if (primary != nullptr) {
        zwp_primary_selection_offer_v1_destroy(primary);
    }
    *this = {};
}

const char* Offer::mime() const {
    // receive and accept must name a mime the source actually offered, so
    // the flags track the exact offered spellings.
    if (utf8) {
        return "text/plain;charset=utf-8";
    }
    if (utf8String) {
        return "UTF8_STRING";
    }
    return plain ? "text/plain" : nullptr;
}

void Offer::addMime(const char* mime) {
    mimeData.append(mime, StringView(mime).length() + 1);
    ++mimeCount;
}

const char* Offer::mimeAt(size_t index) const {
    const char* current = (const char*)(mimeData.data());
    while (index != 0) {
        current += StringView(current).length() + 1;
        --index;
    }
    return current;
}

const char* Offer::offered(StringView mime) const {
    const char* current = (const char*)(mimeData.data());
    for (u32 index = 0; index != mimeCount; ++index) {
        const StringView candidate(current);
        if (candidate == mime) {
            return current;
        }
        current += candidate.length() + 1;
    }
    return nullptr;
}

size_t DndOfferView::formats() const {
    return offer->mimeCount;
}

StringView DndOfferView::format(size_t index) const {
    return StringView(offer->mimeAt(index));
}

DropOffer* DndDrop::what() {
    return view;
}

Input* DndDrop::read(StringView mime) {
    const char* const chosen = started ? nullptr : view->offer->offered(mime);
    started = true;
    int fd = -1;
    bool* flag = nullptr;
    if (chosen != nullptr) {
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC) == 0) {
            wl_data_offer_receive(offer, chosen, pipes[1]);
            close(pipes[1]);
            if (platform->flushDisplay()) {
                fd = pipes[0];
                flag = &drained;
            } else {
                close(pipes[0]);
            }
        }
    }
    // A refused or unstartable read is an immediately empty stream that
    // never marks the transfer drained.
    return platform->allocator_->make<StreamInput>(*platform, fd, Buffer(), flag);
}

PlatformImpl::PlatformImpl(ObjPool& owner)
    : poller_(PollerLoop::create(owner))
{
    owner_ = &owner;
    allocator_ = SmallObjAllocator::create(&owner);
    scheduler_ = Scheduler::create(owner, *poller_);
    scheduler_->spawn(repeatBody_, repeatStack_, sizeof(repeatStack_));
    display = wl_display_connect(nullptr);
    if (display == nullptr) {
        fail(u8"wl_display_connect failed");
    }
    xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (xkbContext == nullptr) {
        fail(u8"xkb_context_new failed");
    }
    const char* locale = getenv("LC_ALL");
    if (locale == nullptr || *locale == 0) {
        locale = getenv("LC_CTYPE");
    }
    if (locale == nullptr || *locale == 0) {
        locale = getenv("LANG");
    }
    if (locale == nullptr || *locale == 0) {
        locale = "C";
    }
    // Dead-key compose sequences. A missing table (e.g. a plain "C" locale
    // without compose data) simply disables composition.
    composeTable = xkb_compose_table_new_from_locale(xkbContext, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (composeTable != nullptr) {
        composeState = xkb_compose_state_new(composeTable, XKB_COMPOSE_STATE_NO_FLAGS);
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, this);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        fail(u8"Wayland registry roundtrip failed");
    }
    if (compositor == nullptr || wmBase == nullptr || seat == nullptr) {
        fail(u8"Wayland compositor lacks required globals");
    }
    createSelectionDevices();
    flushDisplay();
}

PlatformImpl::~PlatformImpl() {
    poller_->cancel(displayWaiter_);
    stopRepeat();
    pendingClipboardOffer.reset();
    clipboardOffer.reset();
    pendingPrimaryOffer.reset();
    primaryOffer.reset();
    if (dndSession != nullptr) {
        // The session fiber owns the offer; ending the session synchronously
        // makes it release the proxy before the display goes away.
        dndSession->window = nullptr;
        dndSession->fiber->wake();
    }
    if (clipboardSource != nullptr) {
        wl_data_source_destroy(clipboardSource);
    }
    if (primarySource != nullptr) {
        zwp_primary_selection_source_v1_destroy(primarySource);
    }
    if (textInput != nullptr) {
        zwp_text_input_v3_destroy(textInput);
    }
    if (textInputManager != nullptr) {
        zwp_text_input_manager_v3_destroy(textInputManager);
    }
    if (cursorShapeDevice != nullptr) {
        wp_cursor_shape_device_v1_destroy(cursorShapeDevice);
    }
    if (pointer != nullptr) {
        releasePointer(pointer);
    }
    if (keyboard != nullptr) {
        releaseKeyboard(keyboard);
    }
    if (dataDevice != nullptr) {
        releaseDataDevice(dataDevice);
    }
    if (primaryDevice != nullptr) {
        zwp_primary_selection_device_v1_destroy(primaryDevice);
    }
    if (output != nullptr) {
        releaseOutput(output);
    }
    if (seat != nullptr) {
        releaseSeat(seat);
    }
    if (cursorShapeManager != nullptr) {
        wp_cursor_shape_manager_v1_destroy(cursorShapeManager);
    }
    if (activation != nullptr) {
        xdg_activation_v1_destroy(activation);
    }
    if (decorationManager != nullptr) {
        zxdg_decoration_manager_v1_destroy(decorationManager);
    }
    if (fractionalScaleManager != nullptr) {
        wp_fractional_scale_manager_v1_destroy(fractionalScaleManager);
    }
    if (viewporter != nullptr) {
        wp_viewporter_destroy(viewporter);
    }
    if (primaryManager != nullptr) {
        zwp_primary_selection_device_manager_v1_destroy(primaryManager);
    }
    if (dataDeviceManager != nullptr) {
        wl_data_device_manager_destroy(dataDeviceManager);
    }
    if (wmBase != nullptr) {
        xdg_wm_base_destroy(wmBase);
    }
    if (compositor != nullptr) {
        wl_compositor_destroy(compositor);
    }
    if (registry != nullptr) {
        wl_registry_destroy(registry);
    }
    if (composeState != nullptr) {
        xkb_compose_state_unref(composeState);
    }
    if (composeTable != nullptr) {
        xkb_compose_table_unref(composeTable);
    }
    if (xkbState != nullptr) {
        xkb_state_unref(xkbState);
    }
    if (keymap != nullptr) {
        xkb_keymap_unref(keymap);
    }
    if (xkbContext != nullptr) {
        xkb_context_unref(xkbContext);
    }
    if (display != nullptr) {
        wl_display_disconnect(display);
    }
}

Window* PlatformImpl::createWindow(ObjPool& windowOwner, const WindowOptions& options) {
    return windowOwner.make<WindowImpl>(*this, options);
}

LoopWake* PlatformImpl::createLoopWake(ObjPool& owner, TimerCallback& callback) {
    return LoopWake::create(owner, *poller_, callback);
}

Poller* PlatformImpl::poller() {
    return poller_;
}

Scheduler* PlatformImpl::scheduler() {
    return scheduler_;
}

void PlatformImpl::armDisplay(bool write) {
    displayWaiter_.fd = {
        .fd = wl_display_get_fd(display),
        .flags = PollFlag::In | (write ? PollFlag::Out : 0),
    };
    displayWaiter_.callback = this;
    poller_->arm(displayWaiter_);
}

bool PlatformImpl::flushDisplay() {
    int result;
    do {
        result = wl_display_flush(display);
    } while (result < 0 && errno == EINTR);
    if (result >= 0) {
        armDisplay(false);
        return true;
    }
    if (errno == EAGAIN) {
        armDisplay(true);
        return true;
    }
    poller_->cancel(displayWaiter_);
    stop();
    return false;
}

void PlatformImpl::ready(PollFD event) {
    if (event.flags & (PollFlag::Err | PollFlag::Hup) || !(event.flags & (PollFlag::In | PollFlag::Out))) {
        stop();
        return;
    }
    if (event.flags & PollFlag::In) {
        if (wl_display_dispatch(display) < 0) {
            stop();
            return;
        }
    }
    flushDisplay();
}

void PlatformImpl::bindRegistry(u32 name, const char* interface, u32 version) {
    if (StringView(interface) == StringView(wl_compositor_interface.name)) {
        compositor = (struct wl_compositor*)(wl_registry_bind(registry, name, &wl_compositor_interface, min(version, 6u)));
    } else if (StringView(interface) == StringView(xdg_wm_base_interface.name)) {
        wmBase = (struct xdg_wm_base*)(wl_registry_bind(registry, name, &xdg_wm_base_interface, min(version, 6u)));
        xdg_wm_base_add_listener(wmBase, &wmBaseListener, this);
    } else if (StringView(interface) == StringView(wl_seat_interface.name) && seat == nullptr) {
        seat = (struct wl_seat*)(wl_registry_bind(registry, name, &wl_seat_interface, min(version, 8u)));
        seatName = name;
        wl_seat_add_listener(seat, &seatListener, this);
    } else if (StringView(interface) == StringView(wl_data_device_manager_interface.name)) {
        dataDeviceManager = (struct wl_data_device_manager*)(wl_registry_bind(registry, name, &wl_data_device_manager_interface, min(version, 3u)));
    } else if (StringView(interface) == StringView(zwp_primary_selection_device_manager_v1_interface.name)) {
        primaryManager = (struct zwp_primary_selection_device_manager_v1*)(wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1));
    } else if (StringView(interface) == StringView(wp_viewporter_interface.name)) {
        viewporter = (struct wp_viewporter*)(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (StringView(interface) == StringView(wp_fractional_scale_manager_v1_interface.name)) {
        fractionalScaleManager = (struct wp_fractional_scale_manager_v1*)(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
    } else if (StringView(interface) == StringView(zxdg_decoration_manager_v1_interface.name)) {
        decorationManager = (struct zxdg_decoration_manager_v1*)(wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
    } else if (StringView(interface) == StringView(xdg_activation_v1_interface.name)) {
        activation = (struct xdg_activation_v1*)(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    } else if (StringView(interface) == StringView(wp_cursor_shape_manager_v1_interface.name)) {
        cursorShapeVersion = min(version, 2u);
        cursorShapeManager = (struct wp_cursor_shape_manager_v1*)(wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, cursorShapeVersion));
    } else if (StringView(interface) == StringView(zwp_text_input_manager_v3_interface.name)) {
        textInputManager = (struct zwp_text_input_manager_v3*)(wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1));
        createSelectionDevices();
    } else if (StringView(interface) == StringView(wl_output_interface.name) && output == nullptr) {
        output = (struct wl_output*)(wl_registry_bind(registry, name, &wl_output_interface, min(version, 4u)));
        outputName = name;
        wl_output_add_listener(output, &outputListener, this);
    }
}

void PlatformImpl::globalRemoved(u32 name) {
    if (name == outputName && output != nullptr) {
        releaseOutput(output);
        output = nullptr;
        outputName = 0;
        outputWidth = 0;
        outputHeight = 0;
        outputScale = 1;
        return;
    }
    if (name == seatName && seat != nullptr) {
        // Drop every seat-derived object; a replacement seat rebinds through
        // the registry and re-creates them from its capabilities.
        seatCapabilities(0);
        if (textInput != nullptr) {
            // Full leave semantics: clear the pending and visible preedit
            // state, not just the proxy.
            textInputLeft(nullptr);
            zwp_text_input_v3_destroy(textInput);
            textInput = nullptr;
        }
        if (dataDevice != nullptr) {
            releaseDataDevice(dataDevice);
            dataDevice = nullptr;
        }
        if (primaryDevice != nullptr) {
            zwp_primary_selection_device_v1_destroy(primaryDevice);
            primaryDevice = nullptr;
        }
        releaseSeat(seat);
        seat = nullptr;
        seatName = 0;
    }
}

void PlatformImpl::seatCapabilities(u32 capabilities) {
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == nullptr) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboardListener, this);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard != nullptr) {
        releaseKeyboard(keyboard);
        keyboard = nullptr;
        keyboardFocus = nullptr;
        enterPressedKeys.clear();
        stopRepeat();
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer == nullptr) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointerListener, this);
        if (cursorShapeManager != nullptr) {
            cursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(cursorShapeManager, pointer);
        }
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer != nullptr) {
        if (cursorShapeDevice != nullptr) {
            wp_cursor_shape_device_v1_destroy(cursorShapeDevice);
            cursorShapeDevice = nullptr;
        }
        releasePointer(pointer);
        pointer = nullptr;
        pointerEnterSerial = 0;
        pointerGrab.reset();
    }
    createSelectionDevices();
}

void PlatformImpl::createSelectionDevices() {
    if (seat == nullptr) {
        return;
    }
    if (dataDeviceManager != nullptr && dataDevice == nullptr) {
        dataDevice = wl_data_device_manager_get_data_device(dataDeviceManager, seat);
        wl_data_device_add_listener(dataDevice, &dataDeviceListener, this);
    }
    if (primaryManager != nullptr && primaryDevice == nullptr) {
        primaryDevice = zwp_primary_selection_device_manager_v1_get_device(primaryManager, seat);
        zwp_primary_selection_device_v1_add_listener(primaryDevice, &primaryDeviceListener, this);
    }
    if (textInputManager != nullptr && textInput == nullptr) {
        textInput = zwp_text_input_manager_v3_get_text_input(textInputManager, seat);
        zwp_text_input_v3_add_listener(textInput, &textInputListener, this);
    }
}

void PlatformImpl::dispatch() {
    if (wl_display_dispatch_pending(display) < 0) {
        stop();
        return;
    }
    poller_->dispatchTimers();
}

void PlatformImpl::run() {
    // stopped is consumed on exit, not reset on entry: a fiber spawned
    // before run() executes its prefix inline and may call stop() before
    // the loop starts, and that stop must not be erased.
    while (!stopped) {
        dispatch();
        if (stopped) {
            break;
        }
        if (!flushDisplay()) {
            break;
        }
        poller_->wait(poller_->nextDeadline());
    }
    stopped = false;
}

void PlatformImpl::stop() {
    stopped = true;
}

u16 PlatformImpl::modifiers() const {
    if (xkbState == nullptr) {
        return 0;
    }
    auto active = [this](const char* name) {
        return xkb_state_mod_name_is_active(xkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    u16 result = 0;
    if (active(XKB_MOD_NAME_SHIFT)) {
        result |= InputShift;
    }
    if (active(XKB_MOD_NAME_CTRL)) {
        result |= InputControl;
    }
    if (active(XKB_MOD_NAME_ALT)) {
        result |= InputAlt;
    }
    if (active(XKB_MOD_NAME_LOGO)) {
        result |= InputSuper;
    }
    if (active(XKB_MOD_NAME_CAPS)) {
        result |= InputCapsLock;
    }
    if (active(XKB_MOD_NAME_NUM)) {
        result |= InputNumLock;
    }
    if (active("Mod5")) {
        result |= InputAltGraph;
    }
    return result;
}

InputKey PlatformImpl::inputKey(xkb_keysym_t symbol) const {
    if (symbol >= XKB_KEY_F1 && symbol <= XKB_KEY_F35) {
        return (InputKey)((u8)(InputKey::F1) + symbol - XKB_KEY_F1);
    }
    if (symbol >= XKB_KEY_a && symbol <= XKB_KEY_z) {
        return InputKey::Printable;
    }
    if (symbol >= XKB_KEY_A && symbol <= XKB_KEY_Z) {
        return InputKey::Printable;
    }
    if ((symbol >= XKB_KEY_0 && symbol <= XKB_KEY_9) || (symbol >= XKB_KEY_space && symbol <= XKB_KEY_asciitilde)) {
        return InputKey::Printable;
    }
    switch (symbol) {
        case XKB_KEY_Escape:
            return InputKey::Escape;
        case XKB_KEY_Return:
            return InputKey::Enter;
        case XKB_KEY_BackSpace:
            return InputKey::Backspace;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            return InputKey::Tab;
        case XKB_KEY_Insert:
            return InputKey::Insert;
        case XKB_KEY_Delete:
            return InputKey::Delete;
        case XKB_KEY_Home:
            return InputKey::Home;
        case XKB_KEY_End:
            return InputKey::End;
        case XKB_KEY_Up:
            return InputKey::Up;
        case XKB_KEY_Down:
            return InputKey::Down;
        case XKB_KEY_Left:
            return InputKey::Left;
        case XKB_KEY_Right:
            return InputKey::Right;
        case XKB_KEY_Page_Up:
            return InputKey::PageUp;
        case XKB_KEY_Page_Down:
            return InputKey::PageDown;
        case XKB_KEY_Clear:
            return InputKey::Clear;
        case XKB_KEY_KP_0:
        case XKB_KEY_KP_1:
        case XKB_KEY_KP_2:
        case XKB_KEY_KP_3:
        case XKB_KEY_KP_4:
        case XKB_KEY_KP_5:
        case XKB_KEY_KP_6:
        case XKB_KEY_KP_7:
        case XKB_KEY_KP_8:
        case XKB_KEY_KP_9:
            return (InputKey)((u8)(InputKey::Keypad0) + symbol - XKB_KEY_KP_0);
        case XKB_KEY_KP_Decimal:
            return InputKey::KeypadDecimal;
        case XKB_KEY_KP_Divide:
            return InputKey::KeypadDivide;
        case XKB_KEY_KP_Multiply:
            return InputKey::KeypadMultiply;
        case XKB_KEY_KP_Subtract:
            return InputKey::KeypadSubtract;
        case XKB_KEY_KP_Add:
            return InputKey::KeypadAdd;
        case XKB_KEY_KP_Enter:
            return InputKey::KeypadEnter;
        case XKB_KEY_KP_Equal:
            return InputKey::KeypadEqual;
        case XKB_KEY_KP_Separator:
            return InputKey::KeypadSeparator;
        case XKB_KEY_KP_F1:
            return InputKey::KeypadF1;
        case XKB_KEY_KP_F2:
            return InputKey::KeypadF2;
        case XKB_KEY_KP_F3:
            return InputKey::KeypadF3;
        case XKB_KEY_KP_F4:
            return InputKey::KeypadF4;
        case XKB_KEY_KP_Insert:
            return InputKey::KeypadInsert;
        case XKB_KEY_KP_Delete:
            return InputKey::KeypadDelete;
        case XKB_KEY_KP_Up:
            return InputKey::KeypadUp;
        case XKB_KEY_KP_Down:
            return InputKey::KeypadDown;
        case XKB_KEY_KP_Left:
            return InputKey::KeypadLeft;
        case XKB_KEY_KP_Right:
            return InputKey::KeypadRight;
        case XKB_KEY_KP_Home:
            return InputKey::KeypadHome;
        case XKB_KEY_KP_End:
            return InputKey::KeypadEnd;
        case XKB_KEY_KP_Page_Up:
            return InputKey::KeypadPageUp;
        case XKB_KEY_KP_Page_Down:
            return InputKey::KeypadPageDown;
        case XKB_KEY_KP_Begin:
            return InputKey::KeypadBegin;
        case XKB_KEY_KP_Space:
            return InputKey::KeypadSpace;
        case XKB_KEY_KP_Tab:
            return InputKey::KeypadTab;
        case XKB_KEY_Caps_Lock:
            return InputKey::CapsLock;
        case XKB_KEY_Scroll_Lock:
            return InputKey::ScrollLock;
        case XKB_KEY_Num_Lock:
            return InputKey::NumLock;
        case XKB_KEY_Print:
            return InputKey::PrintScreen;
        case XKB_KEY_Pause:
            return InputKey::Pause;
        case XKB_KEY_Menu:
            return InputKey::Menu;
        case XKB_KEY_Shift_L:
            return InputKey::LeftShift;
        case XKB_KEY_Control_L:
            return InputKey::LeftControl;
        case XKB_KEY_Alt_L:
            return InputKey::LeftAlt;
        case XKB_KEY_Super_L:
            return InputKey::LeftSuper;
        case XKB_KEY_Shift_R:
            return InputKey::RightShift;
        case XKB_KEY_Control_R:
            return InputKey::RightControl;
        case XKB_KEY_Alt_R:
        case XKB_KEY_ISO_Level3_Shift:
            return InputKey::RightAlt;
        case XKB_KEY_Super_R:
            return InputKey::RightSuper;
        case XKB_KEY_XF86AudioPlay:
            return InputKey::MediaPlay;
        case XKB_KEY_XF86AudioPause:
            return InputKey::MediaPause;
        case XKB_KEY_XF86AudioStop:
            return InputKey::MediaStop;
        case XKB_KEY_XF86AudioForward:
            return InputKey::MediaFastForward;
        case XKB_KEY_XF86AudioRewind:
            return InputKey::MediaRewind;
        case XKB_KEY_XF86AudioNext:
            return InputKey::MediaTrackNext;
        case XKB_KEY_XF86AudioPrev:
            return InputKey::MediaTrackPrevious;
        case XKB_KEY_XF86AudioRecord:
            return InputKey::MediaRecord;
        case XKB_KEY_XF86AudioLowerVolume:
            return InputKey::VolumeDown;
        case XKB_KEY_XF86AudioRaiseVolume:
            return InputKey::VolumeUp;
        case XKB_KEY_XF86AudioMute:
            return InputKey::VolumeMute;
        default:
            return xkb_keysym_to_utf32(symbol) != 0 ? InputKey::Printable : InputKey::Unknown;
    }
}

u32 PlatformImpl::keymapCodepoint(xkb_keycode_t key, xkb_layout_index_t layout) const {
    if (keymap == nullptr) {
        return 0;
    }
    const xkb_keysym_t* symbols = nullptr;
    if (xkb_keymap_key_get_syms_by_level(keymap, key, layout, 0, &symbols) <= 0) {
        return 0;
    }
    return xkb_keysym_to_utf32(symbols[0]);
}

u32 PlatformImpl::layoutCodepoint(xkb_keycode_t key) const {
    const xkb_layout_index_t layout = xkb_state_key_get_layout(xkbState, key);
    return layout == XKB_LAYOUT_INVALID ? 0 : keymapCodepoint(key, layout);
}

u32 PlatformImpl::baseCodepoint(xkb_keycode_t key) const {
    return keymapCodepoint(key, 0);
}

void PlatformImpl::serial(u32 value) {
    latestSerial = value;
    applyClipboardSelection();
    applyPrimarySelection();
}

bool PlatformImpl::consumeEnterPressedKey(u32 key, u32 state) {
    for (size_t index = 0; index != enterPressedKeys.length(); ++index) {
        if (enterPressedKeys[index] != key) {
            continue;
        }
        enterPressedKeys.mut(index) = enterPressedKeys.back();
        enterPressedKeys.popBack();
        // The press was never delivered, so swallow its release too. A fresh
        // press of the same keycode flows through normally from now on.
        return state == WL_KEYBOARD_KEY_STATE_RELEASED;
    }
    return false;
}

bool PlatformImpl::composing() const {
    return composeState != nullptr && xkb_compose_state_get_status(composeState) == XKB_COMPOSE_COMPOSING;
}

size_t PlatformImpl::composeFeed(xkb_keysym_t symbol, u32 codepoint, u32* codepoints, size_t capacity) {
    if (composeState != nullptr && xkb_compose_state_feed(composeState, symbol) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(composeState)) {
            case XKB_COMPOSE_COMPOSING:
                return 0;
            case XKB_COMPOSE_COMPOSED: {
                char buffer[64];
                const int length = xkb_compose_state_get_utf8(composeState, buffer, sizeof(buffer));
                xkb_compose_state_reset(composeState);
                if (length <= 0) {
                    return 0;
                }
                return decodeUtf8((const u8*)(buffer), min((size_t)(length), sizeof(buffer) - 1), codepoints, capacity);
            }
            case XKB_COMPOSE_CANCELLED:
                xkb_compose_state_reset(composeState);
                return 0;
            case XKB_COMPOSE_NOTHING:
                break;
        }
    }
    if (codepoint != 0) {
        codepoints[0] = codepoint;
        return 1;
    }
    return 0;
}

void PlatformImpl::keyboardKey(u32 serial, u32 time, u32 key, u32 state, bool repeated) {
    // Repeats replay the original press serial; a replay must not roll
    // latestSerial back past newer events.
    if (!repeated) {
        this->serial(serial);
    }
    if (!repeated && consumeEnterPressedKey(key, state)) {
        return;
    }
    if (keyboardFocus == nullptr || keyboardFocus->input == nullptr || xkbState == nullptr) {
        return;
    }
    const xkb_keycode_t keycode = key + 8;
    const xkb_keysym_t symbol = xkb_state_key_get_one_sym(xkbState, keycode);
    const InputAction action = repeated ? InputAction::Repeat : (state == WL_KEYBOARD_KEY_STATE_PRESSED ? InputAction::Press : InputAction::Release);
    const u32 codepoint = xkb_keysym_to_utf32(symbol);
    u32 composed[8];
    size_t composedCount = 0;
    if (action == InputAction::Press) {
        composedCount = composeFeed(symbol, codepoint, composed, sizeof(composed) / sizeof(composed[0]));
    } else if (action == InputAction::Repeat && !composing() && codepoint != 0) {
        composed[0] = codepoint;
        composedCount = 1;
    }
    const u16 activeModifiers = modifiers();
    keyboardFocus->input->key({
        .key = inputKey(symbol),
        .action = action,
        .modifiers = activeModifiers,
        .layoutCodepoint = layoutCodepoint(keycode),
        .baseCodepoint = baseCodepoint(keycode),
    });
    if (action != InputAction::Release && !(activeModifiers & (InputControl | InputSuper))) {
        for (size_t index = 0; index != composedCount; ++index) {
            if (composed[index] >= 0x20 && composed[index] != 0x7f) {
                keyboardFocus->input->text({
                    .codepoint = composed[index],
                    .modifiers = activeModifiers,
                });
            }
        }
    }
    keyboardFocus->input->flush();

    if (!repeated && state == WL_KEYBOARD_KEY_STATE_PRESSED && repeatRate != 0 && keymap != nullptr && xkb_keymap_key_repeats(keymap, keycode)) {
        repeatWindow = keyboardFocus;
        repeatKeycode = key;
        repeatSerial = serial;
        repeatTime = time;
        repeatFiber_->wake();
    } else if (!repeated && state == WL_KEYBOARD_KEY_STATE_RELEASED && repeatWindow == keyboardFocus && repeatKeycode == key) {
        stopRepeat();
    }
}

void PlatformImpl::repeat() {
    if (repeatWindow == nullptr || repeatRate == 0 || repeatWindow != keyboardFocus) {
        stopRepeat();
        return;
    }
    keyboardKey(repeatSerial, repeatTime, repeatKeycode, WL_KEYBOARD_KEY_STATE_PRESSED, true);
}

void PlatformImpl::stopRepeat() {
    repeatWindow = nullptr;
    repeatKeycode = 0;
    if (repeatFiber_ != nullptr) {
        repeatFiber_->wake();
    }
}

RepeatBody::RepeatBody(PlatformImpl* platform_)
    : platform(platform_)
{
}

void RepeatBody::run() {
    PlatformImpl& impl = *platform;
    Fiber* const self = impl.scheduler_->current();
    impl.repeatFiber_ = self;
    for (;;) {
        while (impl.repeatKeycode == 0) {
            self->park();
        }
        // The initial delay; a wake means the state changed and the outer
        // loop re-evaluates from scratch.
        if (self->parkFor((u64)(impl.repeatDelay) * 1000)) {
            continue;
        }
        while (impl.repeatKeycode != 0 && impl.repeatRate != 0) {
            impl.repeat();
            if (impl.repeatKeycode == 0 || impl.repeatRate == 0) {
                break;
            }
            if (self->parkFor(1'000'000 / impl.repeatRate)) {
                break;
            }
        }
    }
}

void PlatformImpl::writeSelection(int fd, StringView content) {
    spawnTask([this, fd, owned = Buffer(content)] {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            return;
        }
        size_t offset = 0;
        while (offset != owned.length()) {
            if (!scheduler_->awaitWritable(fd, selectionTransferTimeoutUs)) {
                break;
            }
            const size_t chunk = min<size_t>(owned.length() - offset, 64 * 1024);
            const ssize_t count = writeNoSignal(fd, (const u8*)(owned.data()) + offset, chunk);
            if (count > 0) {
                offset += (size_t)(count);
            } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
        close(fd);
    });
}

StreamInput::StreamInput(PlatformImpl& platform_, int fd_, Buffer&& local_, bool* drained_)
    : platform(platform_)
    , local(static_cast<Buffer&&>(local_))
    , fd(fd_)
    , drained(drained_) {
    if (fd >= 0) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
        }
    } else {
        eof = local.empty();
    }
}

StreamInput::~StreamInput() noexcept {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    if (drained != nullptr) {
        *drained = eof;
    }
}

void StreamInput::operator delete(StreamInput* input, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const allocator = input->platform.allocator_;
    allocator->release(input);
}

size_t StreamInput::readImpl(void* data, size_t len) {
    if (offset != local.length()) {
        const size_t count = min(len, local.length() - offset);
        memcpy(data, (const u8*)(local.data()) + offset, count);
        offset += count;
        if (offset == local.length()) {
            eof = true;
        }
        return count;
    }
    if (fd < 0) {
        eof = eof || local.empty();
        return 0;
    }
    for (;;) {
        const ssize_t count = ::read(fd, data, len);
        if (count > 0) {
            return (size_t)(count);
        }
        if (count == 0) {
            eof = true;
            close(fd);
            fd = -1;
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            fd = -1;
            return 0;
        }
        // The watchdog: a peer that stops making progress for this long
        // aborts the transfer instead of pinning the pipe.
        if (platform.scheduler_->current() == nullptr || !platform.scheduler_->awaitReadable(fd, selectionTransferTimeoutUs)) {
            close(fd);
            fd = -1;
            return 0;
        }
    }
}

StreamOutput::StreamOutput(PlatformImpl& platform_, bool primary_)
    : platform(platform_)
    , primary(primary_)
{
}

void StreamOutput::operator delete(StreamOutput* output, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const allocator = output->platform.allocator_;
    allocator->release(output);
}

size_t StreamOutput::writeImpl(const void* data, size_t size) {
    accumulated.append(data, size);
    return size;
}

void StreamOutput::finishImpl() {
    if (finished) {
        return;
    }
    finished = true;
    if (primary) {
        platform.setPrimary(StringView(accumulated));
    } else {
        platform.setClipboard(StringView(accumulated));
    }
}

void PlatformImpl::dragEntered(u32 serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* offer) {
    if (dndSession != nullptr) {
        // A new session begins before the old one saw leave; end it. The
        // fiber tears down synchronously and detaches itself.
        dndSession->leavePending = true;
        dndSession->fiber->wake();
    }
    WindowImpl* const window = surface == nullptr ? nullptr : (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
    Offer adopted;
    if (offer != nullptr) {
        if (pendingClipboardOffer.data != offer) {
            pendingClipboardOffer.reset();
            pendingClipboardOffer.data = offer;
            wl_data_offer_add_listener(offer, &dataOfferListener, &pendingClipboardOffer);
        }
        adopted = pendingClipboardOffer;
        pendingClipboardOffer = {};
    }
    spawnTask([this, window, serial, x, y, adopted] {
        DndSession session;
        session.window = window;
        session.offer = adopted;
        session.fiber = scheduler_->current();
        session.serial = serial;
        session.motionX = x;
        session.motionY = y;
        session.motionPending = true;
        dndSession = &session;
        runDragSession(session);
        if (dndSession == &session) {
            dndSession = nullptr;
        }
        session.offer.reset();
        flushDisplay();
    });
}

void PlatformImpl::runDragSession(DndSession& session) {
    Buffer acceptedMime;
    DropAction lastAction = DropAction::None;
    bool replySent = false;
    while (true) {
        if (session.leavePending || session.window == nullptr) {
            if (session.window != nullptr && session.window->dropTarget != nullptr) {
                session.window->dropTarget->dragLeft();
            }
            return;
        }
        if (session.dropPending) {
            runDropTransfer(session);
            return;
        }
        if (session.motionPending && session.offer.data != nullptr) {
            session.motionPending = false;
            DropReply reply;
            if (session.window->dropTarget != nullptr) {
                DndOfferView view;
                view.offer = &session.offer;
                const i32 pixelX = (i32)(((i64)(wl_fixed_to_double(session.motionX) * session.window->scaleNumerator)) / scaleDenominator);
                const i32 pixelY = (i32)(((i64)(wl_fixed_to_double(session.motionY) * session.window->scaleNumerator)) / scaleDenominator);
                reply = session.window->dropTarget->dragOver(view, pixelX, pixelY);
            }
            const char* const accepted = reply.mime.empty() ? nullptr : session.offer.offered(reply.mime);
            const DropAction action = accepted == nullptr ? DropAction::None : reply.action;
            const StringView acceptedView = accepted == nullptr ? StringView() : StringView(accepted);
            if (!replySent || action != lastAction || StringView(acceptedMime) != acceptedView) {
                replySent = true;
                lastAction = action;
                acceptedMime = Buffer(acceptedView);
                // A null accept mime tells the source nothing here can
                // consume the drag.
                wl_data_offer_accept(session.offer.data, session.serial, accepted);
                if (wl_data_offer_get_version(session.offer.data) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION) {
                    u32 mask = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
                    if (action == DropAction::Copy) {
                        mask = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
                    } else if (action == DropAction::Move) {
                        mask = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
                    }
                    wl_data_offer_set_actions(session.offer.data, mask, mask);
                }
                flushDisplay();
            }
            continue;
        }
        session.fiber->park();
    }
}

void PlatformImpl::runDropTransfer(DndSession& session) {
    // The hover phase is over; a new session may begin while this transfer
    // drains.
    if (dndSession == &session) {
        dndSession = nullptr;
    }
    WindowImpl* const window = session.window;
    if (window == nullptr || window->dropTarget == nullptr || session.offer.data == nullptr) {
        return;
    }
    DndOfferView view;
    view.offer = &session.offer;
    struct wl_data_offer* const taken = session.offer.data;
    session.offer.data = nullptr;
    DndDrop drop;
    drop.platform = this;
    drop.view = &view;
    drop.offer = taken;
    // The target pulls the payload inline on this fiber; drained flips only
    // when its stream reached end of payload before being deleted.
    window->dropTarget->dropped(drop);
    if (drop.drained && wl_data_offer_get_version(taken) >= WL_DATA_OFFER_FINISH_SINCE_VERSION) {
        wl_data_offer_finish(taken);
    }
    wl_data_offer_destroy(taken);
    flushDisplay();
}

void PlatformImpl::dragMoved(wl_fixed_t x, wl_fixed_t y) {
    if (dndSession == nullptr) {
        return;
    }
    dndSession->motionX = x;
    dndSession->motionY = y;
    dndSession->motionPending = true;
    dndSession->fiber->wake();
}

void PlatformImpl::dragLeft() {
    if (dndSession == nullptr) {
        return;
    }
    dndSession->leavePending = true;
    dndSession->fiber->wake();
}

void PlatformImpl::dragDropped() {
    if (dndSession == nullptr) {
        return;
    }
    dndSession->dropPending = true;
    dndSession->fiber->wake();
}

void PlatformImpl::applyClipboardSelection() {
    if (!clipboardPending || dataDeviceManager == nullptr || dataDevice == nullptr || latestSerial == 0) {
        return;
    }
    if (clipboardSource != nullptr) {
        wl_data_source_destroy(clipboardSource);
    }
    clipboardSource = wl_data_device_manager_create_data_source(dataDeviceManager);
    wl_data_source_add_listener(clipboardSource, &dataSourceListener, this);
    wl_data_source_offer(clipboardSource, "text/plain;charset=utf-8");
    wl_data_source_offer(clipboardSource, "text/plain");
    wl_data_device_set_selection(dataDevice, clipboardSource, latestSerial);
    clipboardPending = false;
    flushDisplay();
}

void PlatformImpl::applyPrimarySelection() {
    if (!primaryPending || primaryManager == nullptr || primaryDevice == nullptr || latestSerial == 0) {
        return;
    }
    if (primarySource != nullptr) {
        zwp_primary_selection_source_v1_destroy(primarySource);
    }
    primarySource = zwp_primary_selection_device_manager_v1_create_source(primaryManager);
    zwp_primary_selection_source_v1_add_listener(primarySource, &primarySourceListener, this);
    zwp_primary_selection_source_v1_offer(primarySource, "text/plain;charset=utf-8");
    zwp_primary_selection_source_v1_offer(primarySource, "text/plain");
    zwp_primary_selection_device_v1_set_selection(primaryDevice, primarySource, latestSerial);
    primaryPending = false;
    flushDisplay();
}

void PlatformImpl::setClipboard(StringView content) {
    clipboardContent.reset();
    clipboardContent.append(content.data(), content.length());
    clipboardPending = true;
    applyClipboardSelection();
}

void PlatformImpl::setPrimary(StringView content) {
    primaryContent.reset();
    primaryContent.append(content.data(), content.length());
    primaryPending = true;
    applyPrimarySelection();
}

void PlatformImpl::setCursor(WindowImpl& window) {
    // set_shape validates against the wl_pointer.enter serial; any newer
    // serial (e.g. from a keyboard event) makes compositors ignore the
    // request.
    if (cursorShapeDevice == nullptr || pointerGrab.focusTarget() != &window || pointerEnterSerial == 0) {
        return;
    }
    wp_cursor_shape_device_v1_set_shape(cursorShapeDevice, pointerEnterSerial, cursorShape(window.cursor, cursorShapeVersion));
}

void PlatformImpl::activate(WindowImpl& window) {
    if (activation == nullptr || window.activationToken != nullptr) {
        return;
    }
    window.activationToken = xdg_activation_v1_get_activation_token(activation);
    xdg_activation_token_v1_add_listener(window.activationToken, &activationTokenListener, &window);
    if (latestSerial != 0 && seat != nullptr) {
        xdg_activation_token_v1_set_serial(window.activationToken, latestSerial, seat);
    }
    xdg_activation_token_v1_set_surface(window.activationToken, window.surface);
    xdg_activation_token_v1_commit(window.activationToken);
}

void PlatformImpl::enableTextInput(WindowImpl& window) {
    if (textInput == nullptr) {
        return;
    }
    zwp_text_input_v3_enable(textInput);
    zwp_text_input_v3_set_content_type(textInput, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
    if (window.textInputWidth != 0 && window.textInputHeight != 0) {
        textInputRectChanged(window, false);
    }
    zwp_text_input_v3_commit(textInput);
    flushDisplay();
}

void PlatformImpl::disableTextInput() {
    if (textInput == nullptr) {
        return;
    }
    zwp_text_input_v3_disable(textInput);
    zwp_text_input_v3_commit(textInput);
    flushDisplay();
}

void PlatformImpl::textInputEntered(struct wl_surface* surface) {
    if (surface == nullptr) {
        return;
    }
    textInputWindow = (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
    if (textInputWindow != nullptr) {
        enableTextInput(*textInputWindow);
    }
}

void PlatformImpl::textInputLeft(struct wl_surface* surface) {
    WindowImpl* const window = surface == nullptr ? nullptr : (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
    if (surface != nullptr && window != textInputWindow) {
        return;
    }
    WindowImpl* const focused = textInputWindow;
    textInputWindow = nullptr;
    disableTextInput();
    pendingPreedit = false;
    pendingCommit = false;
    pendingPreeditText.reset();
    pendingCommitText.reset();
    if (preeditVisible && focused != nullptr && focused->input != nullptr) {
        focused->input->preedit({}, -1, -1);
        focused->input->flush();
    }
    preeditVisible = false;
}

void PlatformImpl::textInputDone() {
    // Pending values are double-buffered: done applies them and resets the
    // pending state, so a batch without a preedit string clears the preview.
    const bool commitPending = pendingCommit;
    const bool preeditPending = pendingPreedit;
    pendingCommit = false;
    pendingPreedit = false;
    WindowImpl* const window = textInputWindow;
    if (window == nullptr || window->input == nullptr) {
        pendingCommitText.reset();
        pendingPreeditText.reset();
        preeditVisible = false;
        return;
    }
    bool delivered = false;
    if (commitPending && !pendingCommitText.empty()) {
        const u16 activeModifiers = modifiers();
        const u8* const bytes = (const u8*)(pendingCommitText.data());
        const size_t length = pendingCommitText.length();
        for (size_t index = 0; index != length;) {
            u32 value;
            const size_t consumed = decodeUtf8One(bytes + index, length - index, &value);
            if (consumed == 0) {
                ++index;
                continue;
            }
            index += consumed;
            if (value >= 0x20 && value != 0x7f) {
                window->input->text({
                    .codepoint = value,
                    .modifiers = activeModifiers,
                });
                delivered = true;
                if (textInputWindow != window) {
                    // A sink callback tore the focus down mid-delivery;
                    // the leave path already reset the preedit state.
                    pendingCommitText.reset();
                    pendingPreeditText.reset();
                    return;
                }
            }
        }
    }
    const StringView preeditText = preeditPending ? StringView(pendingPreeditText) : StringView();
    const bool preeditShown = !preeditText.empty();
    if (preeditShown || preeditVisible) {
        window->input->preedit(preeditText, preeditPending ? pendingPreeditCursorBegin : -1, preeditPending ? pendingPreeditCursorEnd : -1);
        delivered = true;
    }
    preeditVisible = preeditShown;
    if (delivered) {
        window->input->flush();
    }
    pendingCommitText.reset();
    pendingPreeditText.reset();
}

void PlatformImpl::textInputRectChanged(WindowImpl& window, bool commit) {
    if (textInput == nullptr || textInputWindow != &window) {
        return;
    }
    zwp_text_input_v3_set_cursor_rectangle(textInput, window.logicalCoordinate(window.textInputX), window.logicalCoordinate(window.textInputY), (i32)(window.logicalForPixel(window.textInputWidth)), (i32)(window.logicalForPixel(window.textInputHeight)));
    if (commit) {
        zwp_text_input_v3_commit(textInput);
        flushDisplay();
    }
}

WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
    : platform(platform_)
    , input(options.input)
    , events(options.events)
    , frame(options.frame)
    , dropTarget(options.drop)
    , logicalWidth(max(1u, options.width))
    , logicalHeight(max(1u, options.height))
    , minimumWidth(max(1u, options.minimumWidth))
    , minimumHeight(max(1u, options.minimumHeight))
{
    primarySelection.window = this;
    primarySelection.primary = true;
    clipboardSelection.window = this;
    surface = wl_compositor_create_surface(platform.compositor);
    if (surface == nullptr) {
        fail(u8"wl_compositor_create_surface failed");
    }
    wl_proxy_set_user_data((struct wl_proxy*)(surface), this);
    wl_surface_add_listener(surface, &surfaceListener, this);
    xdgSurface = xdg_wm_base_get_xdg_surface(platform.wmBase, surface);
    xdg_surface_add_listener(xdgSurface, &xdgSurfaceListener, this);
    toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, this);

    if (platform.decorationManager != nullptr) {
        decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(platform.decorationManager, toplevel);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (platform.viewporter != nullptr) {
        viewport = wp_viewporter_get_viewport(platform.viewporter, surface);
    }
    if (platform.fractionalScaleManager != nullptr && viewport != nullptr) {
        fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(platform.fractionalScaleManager, surface);
        wp_fractional_scale_v1_add_listener(fractionalScale, &fractionalScaleListener, this);
    }
    if (fractionalScale == nullptr) {
        contentScale((u32)(platform.outputScale) * scaleDenominator);
    }

    Buffer appId(options.appId);
    Buffer initialTitle(options.title);
    xdg_toplevel_set_app_id(toplevel, appId.cStr());
    xdg_toplevel_set_title(toplevel, initialTitle.cStr());
    title = initialTitle;
    requestMinimumSize(minimumWidth, minimumHeight);
}

WindowImpl::~WindowImpl() {
    platform.poller_->cancel(*this);
    if (platform.dndSession != nullptr && platform.dndSession->window == this) {
        platform.dndSession->window = nullptr;
        platform.dndSession->fiber->wake();
    }
    if (platform.keyboardFocus == this) {
        platform.keyboardFocus = nullptr;
        platform.stopRepeat();
    }
    if (platform.textInputWindow == this) {
        platform.textInputWindow = nullptr;
    }
    platform.pointerGrab.remove(this);
    cancelFrame();
    if (activationToken != nullptr) {
        xdg_activation_token_v1_destroy(activationToken);
    }
    if (fractionalScale != nullptr) {
        wp_fractional_scale_v1_destroy(fractionalScale);
    }
    if (viewport != nullptr) {
        wp_viewport_destroy(viewport);
    }
    if (decoration != nullptr) {
        zxdg_toplevel_decoration_v1_destroy(decoration);
    }
    if (toplevel != nullptr) {
        xdg_toplevel_destroy(toplevel);
    }
    if (xdgSurface != nullptr) {
        xdg_surface_destroy(xdgSurface);
    }
    if (surface != nullptr) {
        wl_surface_destroy(surface);
    }
}

u32 WindowImpl::pixelWidth() const {
    return max(1u, (u32)(((u64)(logicalWidth)*scaleNumerator + scaleDenominator / 2) / scaleDenominator));
}

u32 WindowImpl::pixelHeight() const {
    return max(1u, (u32)(((u64)(logicalHeight)*scaleNumerator + scaleDenominator / 2) / scaleDenominator));
}

u32 WindowImpl::logicalForPixel(u32 pixels) const {
    return max(1u, (u32)(((u64)(pixels)*scaleDenominator + scaleNumerator - 1) / scaleNumerator));
}

i32 WindowImpl::logicalCoordinate(i32 pixels) const {
    return (i32)(((i64)(pixels)*scaleDenominator) / (i64)(scaleNumerator));
}

u32 WindowImpl::snappedLogical(u32 suggested, u32 unit, u32 base) const {
    if (unit <= 1 || suggested == 0) {
        return suggested;
    }
    const u32 pixels = max(1u, (u32)(((u64)(suggested)*scaleNumerator) / scaleDenominator));
    if (pixels <= base) {
        return logicalForPixel(base + unit);
    }
    const u32 target = base + ((pixels - base) / unit) * unit;
    for (u32 logical = logicalForPixel(target); logical != 0; --logical) {
        if (((u64)(logical)*scaleNumerator) / scaleDenominator == target) {
            return logical;
        }
        if (logical + 2 < logicalForPixel(target)) {
            break;
        }
    }
    return suggested;
}

void WindowImpl::setLogicalSize(u32 width, u32 height) {
    width = max(1u, width);
    height = max(1u, height);
    logicalWidth = width;
    logicalHeight = height;
    xdg_surface_set_window_geometry(xdgSurface, 0, 0, logicalWidth, logicalHeight);
    if (viewport != nullptr) {
        wp_viewport_set_destination(viewport, logicalWidth, logicalHeight);
    } else {
        wl_surface_set_buffer_scale(surface, max(1, (i32)(scaleNumerator / scaleDenominator)));
    }
}

void WindowImpl::configure() {
    focused = pendingFocused;
    maximized = pendingMaximized;
    fullscreen = pendingFullscreen;
    tiled = pendingTiled;
    u32 width = pendingWidth == 0 ? logicalWidth : pendingWidth;
    u32 height = pendingHeight == 0 ? logicalHeight : pendingHeight;
    if (!maximized && !fullscreen && !tiled) {
        width = snappedLogical(width, resizeUnitWidth, resizeBaseWidth);
        height = snappedLogical(height, resizeUnitHeight, resizeBaseHeight);
    }
    const bool first = !configured;
    setLogicalSize(width, height);
    configured = true;
    if (first || shown) {
        requestFrame();
    }
}

void WindowImpl::contentScale(u32 numerator) {
    if (numerator == 0 || numerator == scaleNumerator) {
        return;
    }
    scaleNumerator = numerator;
    if (fractionalScale != nullptr) {
        wl_surface_set_buffer_scale(surface, 1);
    }
    xdg_toplevel_set_min_size(toplevel, logicalForPixel(minimumWidth), logicalForPixel(minimumHeight));
    setLogicalSize(logicalWidth, logicalHeight);
    requestFrame();
}

void WindowImpl::requestShow() {
    if (shown) {
        return;
    }
    shown = true;
    wl_surface_commit(surface);
}

void WindowImpl::requestClose() {
    if (!closeRequested) {
        closeRequested = true;
        if (events != nullptr) {
            events->close();
        }
    }
}

void WindowImpl::requestFrame() {
    frameRequested = true;
    if (!configured || frameCallback != nullptr || frameScheduled || frame == nullptr) {
        return;
    }
    frameScheduled = true;
    platform.poller_->timeout(0, *this);
}

void WindowImpl::ready() {
    frameScheduled = false;
    if (!configured || !frameRequested || frameCallback != nullptr || frame == nullptr) {
        return;
    }
    frameRequested = false;
    if (!frame->frame(info())) {
        if (frameRequested) {
            // The callback re-requested while failing.  Retry once
            // immediately (transient failures during resize), then back
            // off: a persistently failing renderer (hidden window, lost
            // swapchain) must not spin the poller at timeout(0).
            frameScheduled = true;
            ++frameRetries;
            platform.poller_->timeout(frameRetries > 1 ? 10'000 : 0, *this);
        }
        return;
    }
    frameRetries = 0;
    frameCallback = wl_surface_frame(surface);
    if (frameCallback != nullptr) {
        wl_callback_add_listener(frameCallback, &frameListener, this);
    }
    // The renderer's Vulkan WSI owns buffer attachment for this surface; this
    // is a state-only commit which latches the frame callback. Both run on
    // this thread, so the commit cannot interleave with a WSI present.
    wl_surface_commit(surface);
}

void WindowImpl::cancelFrame() {
    if (frameCallback != nullptr) {
        wl_callback_destroy(frameCallback);
        frameCallback = nullptr;
    }
}

void WindowImpl::frameReady(struct wl_callback* callback) {
    if (callback != frameCallback) {
        wl_callback_destroy(callback);
        return;
    }
    wl_callback_destroy(frameCallback);
    frameCallback = nullptr;
    if (frameRequested) {
        requestFrame();
    }
}

void WindowImpl::requestTitle(StringView value) {
    title.reset();
    title.append(value.data(), value.length());
    xdg_toplevel_set_title(toplevel, title.cStr());
}

void WindowImpl::requestAttention() {
    platform.activate(*this);
}

void WindowImpl::requestRestore() {
    xdg_toplevel_unset_maximized(toplevel);
    xdg_toplevel_unset_fullscreen(toplevel);
}

void WindowImpl::requestIconify() {
    xdg_toplevel_set_minimized(toplevel);
}

void WindowImpl::requestMove(i32, i32) {
    if (platform.seat != nullptr && platform.latestSerial != 0) {
        xdg_toplevel_move(toplevel, platform.seat, platform.latestSerial);
    }
}

void WindowImpl::requestFocus() {
    platform.activate(*this);
}

void WindowImpl::requestMaximized(bool value) {
    if (value) {
        xdg_toplevel_set_maximized(toplevel);
    } else {
        xdg_toplevel_unset_maximized(toplevel);
    }
}

void WindowImpl::requestFullscreen(bool value) {
    if (value) {
        xdg_toplevel_set_fullscreen(toplevel, nullptr);
    } else {
        xdg_toplevel_unset_fullscreen(toplevel);
    }
}

void WindowImpl::requestResize(u32 width, u32 height) {
    setLogicalSize(logicalForPixel(width), logicalForPixel(height));
    requestFrame();
}

void WindowImpl::requestMinimumSize(u32 width, u32 height) {
    minimumWidth = max(1u, width);
    minimumHeight = max(1u, height);
    xdg_toplevel_set_min_size(toplevel, logicalForPixel(minimumWidth), logicalForPixel(minimumHeight));
}

void WindowImpl::requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeUnitWidth = max(1u, width);
    resizeUnitHeight = max(1u, height);
    resizeBaseWidth = baseWidth;
    resizeBaseHeight = baseHeight;
}

bool WindowImpl::inLiveResize() const {
    return false;
}

WindowInfo WindowImpl::info() const {
    return {
        .width = pixelWidth(),
        .height = pixelHeight(),
        .screenPixelWidth = platform.outputWidth,
        .screenPixelHeight = platform.outputHeight,
        .contentScale = (float)(scaleNumerator) / scaleDenominator,
        .focused = focused,
        .maximized = maximized,
        .fullscreen = fullscreen,
        .tiled = tiled,
    };
}

Clipboard* WindowImpl::primary() {
    return &primarySelection;
}

Clipboard* WindowImpl::secondary() {
    return &clipboardSelection;
}

Input* ClipboardImpl::read() {
    PlatformImpl& platform = window->platform;
    Buffer local;
    int fd = -1;
    if (primary && platform.primarySource != nullptr) {
        // We own the selection: serve a snapshot, so a replacement made
        // while the consumer reads does not tear the payload.
        local.append(platform.primaryContent.data(), platform.primaryContent.length());
    } else if (!primary && platform.clipboardSource != nullptr) {
        local.append(platform.clipboardContent.data(), platform.clipboardContent.length());
    } else {
        Offer& offer = primary ? platform.primaryOffer : platform.clipboardOffer;
        const char* const mime = offer.mime();
        if (mime != nullptr) {
            int pipes[2];
            if (pipe2(pipes, O_CLOEXEC) == 0) {
                if (primary) {
                    zwp_primary_selection_offer_v1_receive(offer.primary, mime, pipes[1]);
                } else {
                    wl_data_offer_receive(offer.data, mime, pipes[1]);
                }
                close(pipes[1]);
                if (platform.flushDisplay()) {
                    fd = pipes[0];
                } else {
                    close(pipes[0]);
                }
            }
        }
    }
    return platform.allocator_->make<StreamInput>(platform, fd, static_cast<Buffer&&>(local), nullptr);
}

Output* ClipboardImpl::write() {
    PlatformImpl& platform = window->platform;
    return platform.allocator_->make<StreamOutput>(platform, primary);
}

void WindowImpl::requestPointerIcon(PointerIcon icon) {
    cursor = icon;
    updateCursor();
}

void WindowImpl::requestOpenUri(StringView uri) {
    Buffer path;
    path.append(uri.data(), uri.length());
    char* const arguments[] = {
        (char*)("xdg-open"),
        path.cStr(),
        nullptr,
    };
    pid_t pid = -1;
    posix_spawnp(&pid, arguments[0], nullptr, nullptr, arguments, environ);
}

void WindowImpl::requestTextInputRect(i32 x, i32 y, u32 width, u32 height) {
    if (x == textInputX && y == textInputY && width == textInputWidth && height == textInputHeight) {
        return;
    }
    textInputX = x;
    textInputY = y;
    textInputWidth = width;
    textInputHeight = height;
    platform.textInputRectChanged(*this, true);
}

void WindowImpl::updateCursor() {
    platform.setCursor(*this);
}

void WindowImpl::pointerEntered(u32, wl_fixed_t x, wl_fixed_t y) {
    pointerMoved(x, y);
    updateCursor();
    if (input != nullptr) {
        input->pointerPresence(true);
    }
}

void WindowImpl::pointerLeft() {
    if (input != nullptr) {
        input->pointerPresence(false);
    }
}

void WindowImpl::pointerMoved(wl_fixed_t x, wl_fixed_t y) {
    pointerX = (i32)(((i64)(wl_fixed_to_double(x) * scaleNumerator)) / scaleDenominator);
    pointerY = (i32)(((i64)(wl_fixed_to_double(y) * scaleNumerator)) / scaleDenominator);
    if (input != nullptr) {
        input->pointerMotion({
            .pixelX = pointerX,
            .pixelY = pointerY,
            .modifiers = platform.modifiers(),
        });
    }
}

void WindowImpl::pointerButton(u32 time, u32 button, u32 state) {
    PointerButton mapped;
    switch (button) {
        case BTN_LEFT:
            mapped = PointerButton::Primary;
            break;
        case BTN_RIGHT:
            mapped = PointerButton::Secondary;
            break;
        case BTN_MIDDLE:
            mapped = PointerButton::Middle;
            break;
        case BTN_SIDE:
            mapped = PointerButton::Auxiliary1;
            break;
        case BTN_EXTRA:
            mapped = PointerButton::Auxiliary2;
            break;
        case BTN_FORWARD:
            mapped = PointerButton::Auxiliary3;
            break;
        case BTN_BACK:
            mapped = PointerButton::Auxiliary4;
            break;
        case BTN_TASK:
            mapped = PointerButton::Auxiliary5;
            break;
        default:
            return;
    }
    if (input != nullptr) {
        input->pointerButton({
            .button = mapped,
            .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED,
            .pixelX = pointerX,
            .pixelY = pointerY,
            .modifiers = platform.modifiers(),
            .time = time / 1000.0,
        });
    }
}

void WindowImpl::pointerAxis(u32 axis, wl_fixed_t value) {
    const double converted = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        scrollX += converted;
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        scrollY += converted;
    }
}

void WindowImpl::pointerAxisSteps(u32 axis, i32 value120) {
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        scrollStepsX += value120;
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        scrollStepsY += value120;
    }
}

void WindowImpl::pointerFrame() {
    if (input != nullptr) {
        // Wheel frames carry the intended step count in value120/discrete
        // units; prefer them over the continuous-axis heuristic, which only
        // approximates lines from smooth-scroll distance.
        double lineX = -scrollX / 10.0;
        double lineY = -scrollY / 10.0;
        if (scrollStepsX != 0 || scrollStepsY != 0) {
            lineX = -scrollStepsX / 120.0;
            lineY = -scrollStepsY / 120.0;
        }
        if (lineX != 0 || lineY != 0) {
            input->scroll({
                .x = lineX,
                .y = lineY,
                .pixelX = pointerX,
                .pixelY = pointerY,
                .modifiers = platform.modifiers(),
            });
        }
        input->flush();
    }
    scrollX = 0;
    scrollY = 0;
    scrollStepsX = 0;
    scrollStepsY = 0;
}

RenderContext WindowImpl::renderContext() const {
    return {
        .backend = RenderBackend::Wayland,
        .connection = platform.display,
        .window = surface,
    };
}

Platform* plt::createWaylandPlatform(ObjPool& owner) {
    return owner.make<PlatformImpl>(owner);
}
