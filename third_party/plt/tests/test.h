#pragma once

#include "drop.h"
#include "input.h"
#include "platform.h"
#include "poller.h"
#include "window.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>

namespace plt::test {
    enum class Command : u32 {
        DeferInitialConfigure,
        ReleaseInitialConfigure,
        QueryInitialConfigure,
        PointerEnter,
        PreferredScale,
        QuerySelection,
        QueryMinimum,
        OfferSelection,
        OfferPlainSelection,
        OfferUnsupportedSelection,
        ReleaseRead,
        RequestSourceData,
        RequestBrokenSourceData,
        CancelSources,
        ReleaseWrite,
        QueryWrite,
        AwaitTitles,
        ConfigureWindowState,
        ConfigureWindowResize,
        CloseWindow,
        QueryWindowRequests,
        QueryWindowGeometry,
        QueryFrames,
        CompleteFrames,
        PointerSequence,
        QueryCursor,
        KeyboardEnter,
        KeyboardPress,
        KeyboardRelease,
        KeyboardControl,
        KeyboardControlShift,
        KeyboardControlCapsLock,
        KeyboardRussianControl,
        KeyboardLeave,
        InvalidKeymap,
        QueryActivation,
        QueryPrimarySelection,
        OfferPrimarySelection,
        RequestPrimarySourceData,
        PointerValue120,
        KeyboardEnterWithKeys,
        RemoveOutput,
        RestoreOutput,
        TextInputEnter,
        TextInputPreedit,
        TextInputCommitString,
        TextInputCommitInvalid,
        RemoveSeat,
        DragEnter,
        DragEnterUtf8String,
        DragEnterUriList,
        DragMotion,
        DragDrop,
        DragLeave,
        DragData,
        DragUriData,
        QueryDragAccept,
        QueryDragFinish,
        CursorShapeV1,
        QuerySelectionSerial,
        QueryTextInput,
        QueryTextInputRect,
        Quit,
    };

    struct Reply {
        u32 count = 0;
        i32 first = 0;
        i32 second = 0;
    };

    enum WindowRequest : u32 {
        UpdatedTitle = 1 << 0,
        InitialAppId = 1 << 1,
        Move = 1 << 2,
        Maximize = 1 << 3,
        Unmaximize = 1 << 4,
        Fullscreen = 1 << 5,
        Unfullscreen = 1 << 6,
        Minimize = 1 << 7,
    };

    Reply command(int fd, Command value);
    void pump(Platform& platform);
    stl::Buffer repeated(size_t size, u8 value);

    struct Client {
        explicit Client(int controlFd, u32 width = 800, u32 minimum = 1, WindowEvents* events = nullptr, InputSink* input = nullptr, bool waitForConfigure = true, FrameCallback* frame = nullptr, DropTarget* drop = nullptr);

        int controlFd;
        stl::ObjPool::Ref owner;
        Platform* platform = nullptr;
        Window* window = nullptr;
    };

    struct StreamRead {
        stl::Buffer content;
        u32 chunks = 0;
        bool complete = false;
    };

    // Reads clipboard.read() to end of payload on a fresh fiber; the fiber
    // owns the stream, so complete flips only after the delete.
    void readOnFiber(Platform& platform, Clipboard& clipboard, StreamRead& read);
    // Reads at most one chunk on a fiber, then deletes the stream: the
    // consumer-side abort of a transfer.
    void abortOnFiber(Platform& platform, Clipboard& clipboard, StreamRead& read);
    void writeClipboard(Clipboard& clipboard, stl::StringView content);

    struct EventSink final: WindowEvents, FrameCallback {
        void close() override {
            ++closeCount;
        }

        bool frame(const WindowInfo& info) override {
            ++frameCount;
            lastInfo = info;
            return submitFrames;
        }

        WindowInfo lastInfo;
        u32 closeCount = 0;
        u32 frameCount = 0;
        bool submitFrames = false;
    };

    struct InputRecorder final: InputSink {
        void key(const KeyInput& input) override {
            lastKey = input;
            if (input.action == InputAction::Press) {
                pressedKey = input;
                ++pressCount;
            } else if (input.action == InputAction::Repeat) {
                ++repeatCount;
            } else {
                ++releaseCount;
            }
        }

        void text(const TextInput& input) override {
            lastText = input;
            ++textCount;
        }

        void preedit(stl::StringView text, i32 cursorBegin, i32 cursorEnd) override {
            lastPreedit.reset();
            lastPreedit.append(text.data(), text.length());
            lastPreeditCursorBegin = cursorBegin;
            lastPreeditCursorEnd = cursorEnd;
            ++preeditCount;
        }

        void pointerMotion(const PointerMotionInput& input) override {
            lastMotion = input;
            ++motionCount;
        }

        void pointerButton(const PointerButtonInput& input) override {
            lastButton = input;
            if (input.pressed) {
                ++buttonPressCount;
            } else {
                ++buttonReleaseCount;
            }
        }

        void scroll(const ScrollInput& input) override {
            lastScroll = input;
            ++scrollCount;
        }

        void focus(bool focused) override {
            if (focused) {
                ++focusCount;
            } else {
                ++blurCount;
            }
        }

        void pointerPresence(bool present) override {
            if (present) {
                ++pointerEnterCount;
            } else {
                ++pointerLeaveCount;
            }
        }

        void flush() override {
            ++flushCount;
        }

        KeyInput pressedKey;
        KeyInput lastKey;
        TextInput lastText;
        stl::Buffer lastPreedit;
        i32 lastPreeditCursorBegin = -1;
        i32 lastPreeditCursorEnd = -1;
        u32 preeditCount = 0;
        PointerMotionInput lastMotion;
        PointerButtonInput lastButton;
        ScrollInput lastScroll;
        u32 pressCount = 0;
        u32 repeatCount = 0;
        u32 releaseCount = 0;
        u32 textCount = 0;
        u32 motionCount = 0;
        u32 buttonPressCount = 0;
        u32 buttonReleaseCount = 0;
        u32 scrollCount = 0;
        u32 focusCount = 0;
        u32 blurCount = 0;
        u32 pointerEnterCount = 0;
        u32 pointerLeaveCount = 0;
        u32 flushCount = 0;
    };

    struct StopOnClose final: WindowEvents {
        explicit StopOnClose(Platform*& platform_)
            : platform(platform_)
        {
        }

        void close() override {
            closed = true;
            platform->stop();
        }

        Platform*& platform;
        bool closed = false;
    };

    bool nonblockingShow(int fd);
    bool windowApi(int fd);
    bool frameApi(int fd);
    bool frameRetry(int fd);
    bool pointerInput(int fd);
    bool keyboardInput(int fd);
    bool localSelections(int fd);
    bool missingSelections(int fd);
    bool rejectedSelection(int fd);
    bool pollerApi(int fd);
    bool deferredClipboard(int fd);
    bool fractionalRounding(int fd);
    bool minimumAfterScale(int fd);
    bool asynchronousRead(int fd);
    bool asynchronousPrimary(int fd);
    bool cancelAsynchronousRead(int fd);
    bool cancelReadyClipboardRead(int fd);
    bool asynchronousWrite(int fd);
    bool brokenClipboardConsumer(int fd);
    bool flushBackpressure(int fd);
    bool queuedWaylandEvent(int fd);
    bool plainMimeSelection(int fd);
    bool unsupportedMimeSelection(int fd);
    bool sourceCancellation(int fd);
    bool invalidKeymap(int fd);
    bool multipleWindows(int fd);
    bool scrollValue120(int fd);
    bool keyboardEnterKeys(int fd);
    bool outputRemoval(int fd);
    bool textInput(int fd);
    bool cursorShapes(int fd);
    bool cursorShapesV1(int fd);
    bool fiberClipboard(int fd);
    bool textDrop(int fd);
    bool utf8StringDrop(int fd);
    bool uriListDrop(int fd);
    bool rawDropApi(int fd);
    bool rejectedDrag(int fd);
    bool cancelledDrag(int fd);
}
