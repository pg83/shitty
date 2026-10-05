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
 *
 * See the file LICENSE.GPL3 for the full license.
 */

#include "vterm.h"

#include "pty.h"
#include "parser.h"
#include "screen.h"
#include "vt_test.h"
#include "vt_trace.h"
#include "term_features.h"
#include "mouse_frontend.h"
#include "mouse_protocol.h"
#include "cell_extra_store.h"
#include "prompt_editor.h"

#include <lib/vterm/hex.h>
#include <lib/vterm/utf8.h>
#include <lib/vterm/base64.h>
#include <lib/vterm/unicode.h>
#include <lib/vterm/vt_grid.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/grapheme.h>
#include <lib/vterm/keyboard.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/vt_config.h>
#include <lib/vterm/color_spec.h>
#include <lib/vterm/unicode_map.h>
#include <lib/vterm/vt_geometry.h>
#include <lib/vterm/input_handler.h>

#include <std/sys/fd.h>
#include <std/ios/out.h>
#include <std/sys/crt.h>
#include <std/alg/xchg.h>
#include <std/str/view.h>
#include <std/alg/bound.h>
#include <std/ios/input.h>
#include <std/sym/i_map.h>
#include <std/sym/s_map.h>
#include <std/sys/throw.h>
#include <std/sys/types.h>
#include <std/alg/minmax.h>
#include <std/dbg/assert.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/ptr/scoped.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/ios/fs_utils.h>
#include <std/mem/obj_pool.h>
#include <std/rng/split_mix_64.h>
#include <std/mem/small_obj_allocator.h>

#include <map>
#include <set>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef __APPLE__
    #include <libproc.h>
#endif
#include <functional>
#include <plt/drop.h>
#include <plt/fiber.h>
#include <plt/mutex.h>
#include <sys/types.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/clipboard.h>

#if defined(__SSE2__)
    #include <emmintrin.h>
#endif

using namespace stl;
using namespace plt;

void MouseTrackingState::setMode(MouseTrackingMode value) {
    if (mode != value) {
        mode = value;
        ++generation;
    }
}

void MouseTrackingState::setEncoding(MouseTrackingEnc value) {
    if (enc != value) {
        enc = value;
        ++generation;
    }
}

namespace {
    static constexpr u64 selectionAutoscrollInterval = 50'000;
    // The pointer modifiers arming the hyperlink gesture: Control is the
    // Linux convention, Super carries the macOS Command+click habit.
    static constexpr u16 hyperlinkModifiers = InputControl | InputSuper;

    static StringView stringView(const Buffer& value) {
        return StringView(value);
    }

    // Sorted tab-stop bookkeeping over a flat vector.
    static size_t tabLowerBound(const Vector<u16>& tabs, u16 value) {
        return lowerBound(tabs.begin(), tabs.end(), value) - tabs.begin();
    }

    static size_t tabUpperBound(const Vector<u16>& tabs, u16 value) {
        return upperBound(tabs.begin(), tabs.end(), value) - tabs.begin();
    }

    static bool tabContains(const Vector<u16>& tabs, u16 value) {
        return binaryContains(tabs.begin(), tabs.end(), value);
    }

    static void tabInsertAt(Vector<u16>& tabs, size_t at, u16 value) {
        tabs.pushBack(value);
        memmove(tabs.mutData() + at + 1, tabs.data() + at, (tabs.length() - at - 1) * sizeof(u16));
        tabs.mut(at) = value;
    }

    static void tabEraseAt(Vector<u16>& tabs, size_t at) {
        memmove(tabs.mutData() + at, tabs.data() + at + 1, (tabs.length() - at - 1) * sizeof(u16));
        tabs.popBack();
    }

    // The hash maps have no wholesale clear; values carry their key so a
    // sweep can collect and erase them.
    template <typename T>
    static void clearIntMap(IntMap<T>& map) {
        Vector<u64> keys;
        map.visit([&keys](T& value) {
            keys.pushBack(value.key);
        });
        for (const u64 key : keys) {
            map.erase(key);
        }
    }

    static StringView semanticOption(StringView payload, StringView name) {
        StringView command;
        StringView options;
        if (!payload.split(';', command, options)) {
            return {};
        }
        while (!options.empty()) {
            StringView option = options;
            StringView rest;
            if (options.split(';', option, rest)) {
                options = rest;
            } else {
                options = {};
            }
            StringView key;
            StringView value;
            if (option.split('=', key, value) && key == name) {
                return value;
            }
        }
        return {};
    }

    static bool kittyClipboardMimeSupported(StringView mimeType) {
        return mimeType.empty() || mimeType == StringView(u8"text/plain") || mimeType == StringView(u8"text/plain;charset=utf-8") || mimeType == StringView(u8"UTF8_STRING") || mimeType == StringView(u8"STRING") || mimeType == StringView(u8"TEXT");
    }

    static StringView selectKittyClipboardMime(StringView mimeTypes) {
        if (mimeTypes.empty()) {
            return StringView(u8"text/plain");
        }
        while (!mimeTypes.empty()) {
            StringView mimeType = mimeTypes;
            StringView rest;
            if (mimeTypes.split(' ', mimeType, rest)) {
                mimeTypes = rest;
            } else {
                mimeTypes = {};
            }
            if (kittyClipboardMimeSupported(mimeType)) {
                return mimeType.empty() ? StringView(u8"text/plain") : mimeType;
            }
        }
        return {};
    }

    static void copyKittyClipboardId(Buffer& output, StringView input) {
        output.reset();
        for (const u8 byte : input) {
            if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '+' || byte == '.') {
                output.append(&byte, 1);
            }
        }
    }

    static void writeKittyClipboardPacket(Output& output, bool send8BitControls, StringView type, StringView status, StringView id = {}, StringView mimeType = {}, StringView payload = {}, bool primary = false) {
        StringBuilder header;
        header << (send8BitControls ? StringView(u8"\x9d") : StringView(u8"\x1b]")) << StringView(u8"5522;type=") << type << StringView(u8":status=") << status;
        if (primary && status == StringView(u8"OK")) {
            header << StringView(u8":loc=primary");
        }
        if (!id.empty()) {
            header << StringView(u8":id=") << id;
        }
        if (!mimeType.empty()) {
            header << StringView(u8":mime=");
        }
        StringView bytes(header);
        output.write(bytes.data(), bytes.length());
        if (!mimeType.empty()) {
            Base64Encoder encoder;
            encoder.write(output, mimeType);
            encoder.finish(output);
        }
        if (!payload.empty()) {
            const u8 separator = ';';
            output.write(&separator, 1);
            Base64Encoder encoder;
            encoder.write(output, payload);
            encoder.finish(output);
        }
        const StringView suffix = send8BitControls ? StringView(u8"\x9c") : StringView(u8"\x1b\\");
        output.write(suffix.data(), suffix.length());
        output.flush();
    }

    // The pty's stream face over the block transport: bytes land straight
    // in the current block's payload. The first write of a batch sizes
    // its block exactly - a keystroke stays in the smallest allocator
    // class - and every follow-up before the flush takes whole blocks,
    // so a paste's line-split runs coalesce instead of cutting a block
    // per fragment. flush() sends the partial tail and ends the batch.
    struct PtyBlockOutput final: public Output {
        explicit PtyBlockOutput(PtyHandle& pty);

        size_t writeImpl(const void* data, size_t len) override;
        void flushImpl() override;

        PtyHandle* pty;
        PtyHandle::Chunk* chunk = nullptr;
        size_t filled = 0;
        bool batch = false;
    };

    struct PasteOutput final: public Output {
        PasteOutput(Output* output, bool bracketed);
        ~PasteOutput() noexcept override;

        size_t writeImpl(const void* data, size_t size) override;
        void begin();

        Output* output;
        bool bracketed;
        bool started = false;
        bool previousCarriageReturn = false;
        bool pendingC1Lead = false;
    };

    struct FiberTaskBase: public Runable {
        virtual void cancel() = 0;
    };

    // One spawnable unit for a terminal transaction; recycled through the
    // terminal free list because transactions come and go with every key
    // and paste. Its non-trivial destructor is registered in the terminal
    // arena, making a parked transaction disappear when that arena dies.
    struct FiberBlock {
        ~FiberBlock() noexcept;

        FiberBlock* next = nullptr;
        FiberTaskBase* task = nullptr;
        plt::Fiber* fiber = nullptr;
        alignas(16) u8 stack[plt::lightFiberStack];
    };

    struct VtermImpl;

    // R7: what makes this terminal's refs visible to a collection some
    // other terminal started. The store is the window's, so a pane that
    // never noticed the budget is exactly the pane whose cells used to be
    // left pointing into the freed pool.
    struct CallVtermCollectExtras final: public CellExtraClient {
        explicit CallVtermCollectExtras(VtermImpl* terminal);

        void collectExtras(Vector<TerminalCell*>& cells, Vector<u32*>& roots) override;

        VtermImpl* terminal;
    };

    // A one-shot fiber body carved out of the small-object allocator;
    // releases itself and recycles its stack when the fiber finishes.
    template <typename F>
    struct FiberTask final: public FiberTaskBase {
        FiberTask(VtermImpl* terminal_, FiberBlock* block_, F&& body_)
            : terminal(terminal_)
            , block(block_)
            , body(static_cast<F&&>(body_))
        {
        }

        void run() override;
        void cancel() override;

        VtermImpl* terminal;
        FiberBlock* block;
        F body;
    };

    static plt::Clipboard* selectionTarget(VtHost& host, bool primary) {
        return primary ? host.primary() : host.secondary();
    }

    static void writeSelection(plt::Clipboard& clipboard, StringView content) {
        const ScopedPtr<Output> output{clipboard.write()};
        output->write(content.data(), content.length());
        output->finish();
    }

    struct GraphemeBuffer {
        void clear();
        bool pushBack(u32 codepoint);
        bool empty() const;
        size_t size() const;
        const u32* data() const;

        // Keep pathological combining sequences from turning every appended
        // codepoint into a larger CellExtra allocation and copy.  This is the
        // same complete-cluster limit used by Kitty.
        constexpr static size_t capacity = 24;
        u32 values[capacity] = {};
        size_t size_ = 0;
    };

    struct VtermInputSpec {
        InputKey key;
        const char* input;
        size_t length = 0;

        size_t getLength() const;
    };

    struct VtermImpl;

    struct VtermInput {
        explicit VtermInput(VtermImpl* terminal);

        bool key(const KeyInput& input);
        bool text(const TextInput& input);
        bool pointerMotion(const PointerMotionInput& input);
        bool pointerButton(const PointerButtonInput& input);
        bool scroll(const ScrollInput& input);
        void focus(bool focused);
        void pointerPresence(bool present);
        void flush();

        VtModifier legacyModifiers(u16 modifiers) const;
        u16 kittyModifiers(u16 modifiers) const;
        void mouseProtocolCoordinates(MouseTrackingEnc encoding, int pixelX, int pixelY, u16& column, u16& row) const;
        void sendMouseProtocol(MouseTrackingEnc encoding, MouseEventType type, u16 modifiers, int button, int column, int row);
        void sendMouseButtonProtocol(MouseEventType type, int button, int pixelX, int pixelY, u16 modifiers, const MouseTrackingState& tracking);
        bool refreshHyperlink();
        void refreshHyperlinkAndRedraw();
        void updatePointer(int pixelX, int pixelY, u16 modifiers);
        void updatePointerModifiers(const KeyInput& input);
        int currentSelectionAutoscrollDirection() const;
        void updateSelectionAutoscroll();
        void stopSelectionAutoscroll();
        void armTimeout();
        bool advanceSelectionAutoscroll(bool force);
        ScreenHyperlink resolveLink(int pixelX, int pixelY);
        bool copy();
        bool paste(bool primary);

        struct PendingTextKey {
            bool active = false;
            u32 primary = 0;
            u32 shifted = 0;
            u32 base = 0;
            u16 modifiers = 0;
            VtermKeyEventType event = VtermKeyEventType::Press;
        };

        VtermImpl* terminal;
        MouseFrontendState mouse;
        PendingTextKey pendingTextKey;
        unsigned suppressedTextInputs = 0;
        bool suppressRepeatedTextInput = false;
        bool hyperlinkClick = false;
        // A single click that may yet place the command line's cursor: where
        // it was pressed, kept until the release shows it was not a drag.
        bool promptClick = false;
        int promptClickX = 0;
        int promptClickY = 0;
        int pointerX = 0;
        int pointerY = 0;
        u16 pointerModifiers = 0;
        u32 hoveredHyperlink = 0;
        u32 hoveredLinkBegin = 0;
        u32 hoveredLinkEnd = 0;
        bool pointerPresent = false;
        bool pointerPositionKnown = false;
        bool pointerFocused = true;
        int selectionAutoscrollDirection = 0;
        u64 selectionAutoscrollDeadline = 0;
        bool rectangularSelectionKeyConsumed = false;
    };

    // A permanent timer fiber body dispatching to one VtermImpl method.
    struct VtermTimerBody final: public Runable {
        VtermTimerBody(VtermImpl* parent, void (VtermImpl::*method)());

        void run() override;

        VtermImpl* parent;
        void (VtermImpl::*method)();
    };

    struct VtermImpl final: public Vterm, public ParserIface {
        VtermImpl(ObjPool& owner, VtGeometry& windowGeometry, const VtConfigSlot& configSlot, VtCellExtras& extras, SmallObjAllocator& smallObjects, plt::Scheduler& scheduler, VtHost& host, const VtGeometry& geometry, PtyHandle& pty, VtermTraceFactory* traceFactory, Output* dump);

        const VtConfig& config() const;

        ~VtermImpl();

        void feedPty(StringView bytes) override;
        void feedPty(const StringView* slices, size_t count) override;
        void show() override;
        void hide() override;
        void expose() override;
        void exposeAll() override;
        void focus(bool focused) override;
        bool key(const KeyInput& input) override;
        bool text(const TextInput& input) override;
        bool pointerMotion(const PointerMotionInput& input) override;
        bool pointerButton(const PointerButtonInput& input) override;
        bool scroll(const ScrollInput& input) override;
        void pointerPresence(bool present) override;
        void flush() override;
        void copy() override;
        void paste(bool primary) override;
        void clear() override;
        void key(InputKey key, VtModifier modifiers);
        void character(u8 byte, VtModifier modifiers);
        void sendBytes(StringView bytes, bool userInput) override;
        bool selectCommandLine() override;
        bool commandLineEditable() const override;
        bool commandLineUndo(bool redo) override;
        // The editor's own: whether there is a line to edit now, and where
        // it starts; a click placing its cursor; the selection replaced.
        bool promptAtInput() const;
        bool promptOrigin(PromptOrigin& origin) const;
        // Under -verbose: one line on stderr saying why the editor did not
        // act, with everything the decision read.
        void promptTrace(const char* action) const;
        bool promptPlaceCursor(int pixelX, int pixelY);
        bool promptReplaceSelection(const u32* insert, size_t count);
        void kittyKey(InputKey key, u16 modifiers, VtermKeyEventType event);
        void kittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event);
        bool mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY);
        void locatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons);
        void locatorButton(u8 button, bool pressed);
        void scrollUp(u16 count);
        void scrollDown(u16 count);
        void pageUp() override;
        void pageDown() override;
        u32 scrollView(i32 rows) override;
        u32 scrollViewTo(u32 offset) override;
        void selectionStart(int pixelX, int pixelY, bool cycleSnapTo);
        void selectionExtend(int pixelX, int pixelY, bool cycleSnapTo);
        void selectionUpdate(int pixelX, int pixelY);
        VtermTextResult selectionFinish();
        bool hasSelection() const;
        void selectionClear();
        void selectionRectangular();
        void paste(StringView text);
        bool pasteMimeNotification(bool primary);
        ScreenHyperlink resolveHyperlink(int pixelX, int pixelY) const;
        StringView hyperlinkAt(int pixelX, int pixelY);
        void refreshForegroundName() override;
        bool expireSynchronizedOutput(bool force) override;
        bool advanceAnimation(bool force) override;
        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd) override;
        void dropText(Input& source) override;
        void dropUriList(Input& source) override;
        void dropBuffered(StringView text);
        bool bufferDropPayload(Input& source, Buffer& content);
        void buildQuotedEntry(StringView entry, StringBuilder& quoted);
        const TerminalUpdate* output() override;
        const TerminalUpdate& retainedOutput() override;
        void consume() override;
        VtermState state() const override;
        TestApi* createTestApi();

        void parserResetGraphemeInput() override;
        void parserBell() override;
        bool parserAutoNewlineMode() const override;
        CompatibilityLevel parserCompatibilityLevel() const override;
        void parserSetCompatibilityLevel(CompatibilityLevel level) override;
        void parserSet8BitControls(bool enabled) override;
        void parserSetApplicationKeypad(bool enabled) override;
        void parserMoveCursorBackward(u32 count) override;
        bool parserHexTitleInput() const override;
        void parserSingleShift(u8 index) override;
        void parserLockingShiftGl(u8 index) override;
        void parserLockingShiftGr(u8 index) override;
        void parserResetCharsets(bool isoLatin1) override;
        void parserDesignateCharset(u8 index, Charset charset, u16 id, bool is96) override;
        bool parserHighlightMouseTracking() const override;
        bool windowOperationsAllowed() const override;
        void parserWritePty(StringView bytes) override;
        bool parserGroundUtf8Enabled() const override;
        void parserGroundHigh(u8 byte) override;
        void parserGroundAscii(u8 byte) override;
        bool parserUtf8BulkEligible() const override;
        size_t parserPlaceAscii(StringView bytes) override;
        size_t parserPlaceUtf8Run(StringView bytes, u8& pendingTrace) override;

        void paneResized(const VtGeometry& geometry) override;
        void presentationInvalidated() override;
        void configChanged() override;
        void resizeGrid();
        void createPrimaryScreen();
        void createAlternateScreen();
        void createInactiveAlternateScreen();
        struct SavedCursor;
        void resizeScreen(Screen*& frame, ObjPool*& pool, Screen::Cursor& cursor, SavedCursor* trackedCursor);

        void redraw();
        bool animationActive() const;
        void wakeTimers();
        void startTimers();
        void runSyncWatchdog();
        void runBlink();
        void runAutoscroll();
        void enableBlinkingText();
        void refreshBlinkingText();
        void exposeFrames();

        using InputSpec = VtermInputSpec;

        void sendKey(InputKey key, VtModifier modifiers = VtModifier::none);
        void sendCharacter(u8 ch, VtModifier modifiers = VtModifier::none);
        void sendUserInput(StringView bytes, bool scrollToBottom = true);
        void writePty(StringView bytes);
        void writePtyLocked(StringView bytes);
        bool modifyOtherKeyEncoded(u8 ch, VtModifier modifiers) const;
        void writeProtocolResponse(StringView prefix, StringView payload, StringView suffix = {});
        void writeKittyKey(InputKey key, u16 modifiers, VtermKeyEventType event);
        void writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event);
        u8 getKittyKeyboardFlags() const;

        void setLocatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons = 0);
        void reportLocatorButton(u8 button, bool pressed);

        void setHasFocus(bool);
        void getHyperlink(int pX, int pY, Buffer& out) const;
        void mouseWheelUp(u16 count = 1);
        void mouseWheelDown(u16 count = 1);
        void mouseWheelRight(u16 count = 1);
        void mouseWheelLeft(u16 count = 1);
        void selectStart(int pX, int pY, bool cycleSnapTo);
        void selectExtend(int pX, int pY, bool cycleSnapTo);
        void selectUpdate(int pX, int pY);
        bool selectFinish(Buffer& utf8_selection);
        void selectClear();
        void selectRectangularModeToggle();

        void pasteSelection(StringView utf8_selection);

        Point selectionPoint(int pX, int pY) const;
        void getLocalEcho(const u8* const begin, const u8* const end, Buffer& out);
        template <typename F>
        void spawnTransaction(F&& body);
        void spawnPtyWrite(StringView bytes);

        bool processInput(const u8* input, int size, bool refresh = true);
        bool processInput(const StringView* slices, size_t count, bool refresh = true);
        [[gnu::noinline]] bool processInputImpl(const StringView* slices, size_t count, bool refresh);

        bool presentationChanged() const override;
        void syncPresentationCursor(const TerminalCursor& before);
        void changePresentation();
        u64 presentationRevision() const;
        TerminalCursor presentationCursor(u32 viewOffset) const;
        void overlayPreedit(TerminalUpdate& update);
        void fillTerminalUpdate(TerminalUpdate& update, const ScreenFrame& frame, const TerminalRow* rows);

        void writeCsiResponse(StringView payload);
        void writeDcsResponse(StringView payload);
        void writeOscResponse(StringView payload);
        void recordOsc(u32 command, StringView payload);
        void recordBell();
        void recordLeds(u8 state);
        void publishTitle(u32 command, StringView title);
        void notifyTitleChanged(StringView title);
        void publishCwd(StringView path);
        void publishNotify(StringView id, StringView title, StringView body, bool close);
        void publishProgress(u32 state, u32 percent);
        void windowOperation(u32 operation, u32 first, u32 second);
        plt::WindowInfo windowInfo() const;
        u32 columnsForPixelWidth(u32 width) const;
        u32 rowsForPixelHeight(u32 height) const;
        u32 windowColumns() const;
        u32 windowRows() const;
        u32 screenColumns() const;
        u32 screenRows() const;

        struct InputSpecTable {
            bool (*predicate)(const VtermImpl&) = nullptr;
            const InputSpec* specs = nullptr;
        };

        const InputSpecTable* getInputSpecTable();
        const InputSpec* selectInputSpecs(size_t& cursor);
        const InputSpec& getInputSpec(InputKey key);

        void unhandledInput(unsigned char ch) override;
        void resetTerminal();
        void resetAttrs();
        void resetScreen(bool resetTabStops = true);
        void clearScreen();
        void fillScreen(u16 ch);
        void collectCellExtrasIfNeeded(bool force = false);
        void collectCellExtras();
        void collectExtras(Vector<TerminalCell*>& cells, Vector<u32*>& roots);
        void updateExtraCellCount();
        size_t cellCapacity() const override;

        void normalizeCursorPos();
        bool isCursorInsideMargins() const;
        void activeColumns(u16& begin, u16& end) const;
        void activeLine(u16& begin, u16& end);
        u16 doubleWidthEnd(u16 normalEnd) const;
        void eraseRow(u16 pY);
        void eraseRows(u16 startY, u16 count);
        void copyRow(u16 dstY, u16 srcY);
        void insertRows(u16 startY, u16 count);
        void deleteRows(u16 startY, u16 count);
        void insertCols(u16 startX, u16 count);
        void deleteCols(u16 startX, u16 count);
        void eraseRangeInRow(u16 row, u16 start, u16 count);
        void selectiveEraseRangeInRow(u16 row, u16 start, u16 count);
        void eraseEcmaRangeInRow(u16 row, u16 start, u16 count);
        void eraseEcmaRow(u16 row);

        struct Rectangle {
            u16 top;
            u16 left;
            u16 bottom;
            u16 right;
        };

        bool rectangleFromParams(CsiRectangle parameters, Rectangle& rectangle) const;
        void rectangleOrigin(u16& rowBase, u16& columnBase, u16& rowLimit, u16& columnLimit) const;

        void showCursor();
        void hideCursor();
        void inputGraphicChar(unsigned char ch);
        void placeGraphicChar();
        void placeGraphicChar(bool graphemeBoundary);
        void placeGraphicChar(bool graphemeBoundary, u8 width);
        void placeRepeatedCodepoint(u32 codepoint, u32 count);
        template <bool insert>
        void placeAsciiRun(const u8* input, size_t size);
        size_t placeAsciiLines(const u8* input, size_t size);
        int placeUtf8Run(const u8* input, int size, u8& pendingTrace);
        size_t placeGraphemeRun(const u32* codepoints, size_t count, u64 resetBefore = 0);
        template <bool hasWide>
        void placePreparedRun(const u32* input, const u8* widths, size_t size);
        u8 codepointData(u32 codepoint);
        void resetGraphemeInput();
        void jumpToNextTabStop();
        void setFgFromPalIx();
        void setBgFromPalIx();

        CellColor attrForeground() const noexcept {
            return attrs.foreground();
        }

        CellColor attrBackground() const noexcept {
            return attrs.background();
        }

        CellColor attrUnderlineColor() const noexcept {
            return attrs.inlineUnderlineColor();
        }

        void setAttrForeground(CellColor color) noexcept {
            attrs.setForeground(color);
            eraseAttrs.setForeground(color);
            eraseAttrs.setInlineUnderlineColor(color);
        }

        void setAttrBackground(CellColor color) noexcept {
            attrs.setBackground(color);
            eraseAttrs.setBackground(color);
        }

        void setAttrUnderlineColor(CellColor color) noexcept {
            attrs.setInlineUnderlineColor(color);
        }

        void inp_LF();
        void inp_CR() override;
        void inp_HT() override;
        bool performIndex();
        void moveCursorBackward(u32 count);
        void scrollRegionUp(u16 count);
        void scrollRegionDown(u16 count);

        bool esc_IND() override;
        void esc_RI() override;
        void esc_NEL() override;
        void esc_BI() override;
        void esc_FI() override;
        void esc_HTS() override;
        void esc_SPA() override;
        void esc_EPA() override;
        bool horizontalMarginMode() const override;
        void csi_SLRM(u32 left, u32 right, bool valid) override;
        void csi_SCOSC();
        void csi_SCORC() override;
        void esc_DECSC() override;
        void esc_DECRC() override;
        void esc_RIS() override;
        void csi_DECSTR() override;

        void csi_CUU(u32 count) override;
        void csi_CUD(u32 count) override;
        void csi_CUF(u32 count) override;
        void csi_CUB(u32 count) override;
        void csi_CNL(u32 count) override;
        void csi_CPL(u32 count) override;
        void csi_CHA(u32 column) override;
        void csi_HPA(u32 column) override;
        void csi_HPR(u32 count) override;
        void csi_VPA(u32 row) override;
        void csi_VPR(u32 count) override;
        void csi_CUP(u32 row, u32 column) override;
        void csi_SU(u32 count) override;
        void csi_SD(u32 count) override;
        void csi_CHT(u32 count) override;
        void csi_CBT(u32 count) override;
        void csi_REP(u32 count) override;

        void eraseDisplayAfter() override;
        void eraseDisplayBefore() override;
        void eraseDisplayAll() override;
        void eraseScrollback() override;
        void eraseLineAfter() override;
        void eraseLineBefore() override;
        void eraseLineAll() override;
        void selectiveEraseDisplayAfter() override;
        void selectiveEraseDisplayBefore() override;
        void selectiveEraseDisplayAll() override;
        void selectiveEraseLineAfter() override;
        void selectiveEraseLineBefore() override;
        void selectiveEraseLineAll() override;
        void setDecProtection(bool enabled) override;
        void csi_DECFRA(u32 codepoint, CsiRectangle rectangle) override;
        void csi_DECCRA(CsiRectangle source, u32 targetRow, u32 targetColumn) override;
        void csi_DECERA(CsiRectangle rectangle, bool selective) override;
        void setAttributeChangeExtent(bool rectangular) override;
        void changeRectangleAttributes(CsiRectangle rectangle, CellAttributeChange change) override;
        void csi_XTCHECKSUM(u32 flags) override;
        void csi_DECRQCRA(u32 requestId, CsiRectangle rectangle) override;
        void csi_IL(u32 count) override;
        void csi_DL(u32 count) override;
        void csi_ICH(u32 count) override;
        void csi_DCH(u32 count) override;
        void csi_ECH(u32 count) override;
        void csi_DECIC(u32 count) override;
        void csi_DECDC(u32 count) override;

        void csi_STBM(u32 top, u32 bottom, bool valid) override;
        void clearTabStop() override;
        void clearAllTabStops() override;
        void resetTabStops() override;
        ParserModeState parserModeState() const override;
        void setKeyboardLocked(bool enabled) override;
        void setInsertMode(bool enabled) override;
        void setEraseModeAll(bool enabled) override;
        void setLocalEcho(bool enabled) override;
        void setAutoNewline(bool enabled) override;
        void setAnsiMode(bool enabled) override;
        void setApplicationCursorKeys(bool enabled) override;
        void setColumn132(bool enabled) override;
        void setSmoothScroll(bool enabled) override;
        void setScreenReverseVideo(bool enabled) override;
        void setOriginMode(bool enabled) override;
        void setAutoWrap(bool enabled) override;
        void setAutoRepeat(bool enabled) override;
        void setAllowColumnMode(bool enabled) override;
        void setMoreFix(bool enabled) override;
        void setNationalReplacement(bool enabled) override;
        void setReverseWrap(bool enabled) override;
        void setMouseTracking(MouseTrackingMode mode) override;
        void setCursorBlink(bool enabled) override;
        void setCursorVisible(bool enabled) override;
        void setAlternateScreen(bool enabled, bool clear) override;
        void setBackspaceSendsBackspace(bool enabled) override;
        void setHorizontalMargins(bool enabled) override;
        void setNoClearColumn(bool enabled) override;
        void setFocusEvents(bool enabled) override;
        void setMouseEncoding(MouseTrackingEnc encoding, bool enabled) override;
        void setAlternateScroll(bool enabled) override;
        void setEightBitInput(bool enabled) override;
        void setAltSendsEscape(bool enabled) override;
        void setSavedAlternateScreen(bool enabled) override;
        void setExtendedReverseWrap(bool enabled) override;
        void setBracketedPaste(bool enabled) override;
        void setSynchronizedOutput(bool enabled) override;
        void setGraphemeCluster(bool enabled) override;
        void setColorSchemeUpdates(bool enabled) override;
        void setInBandResize(bool enabled) override;
        void setPasteMimeNotifications(bool enabled) override;
        void savePrivateMode(u32 mode, bool enabled) override;
        bool restorePrivateMode(u32 mode, bool& enabled) const override;
        void reportMode(u32 mode, bool privateMode, u8 state) override;

        void csi_ecma48_SL(u32 count) override;
        void csi_ecma48_SR(u32 count) override;
        void setCursorStyle(u8 reportStyle, TerminalCursor::Style shape, bool blink) override;
        void refreshCursorStyle() override;

        void csi_priDA() override;
        void csi_secDA() override;
        void csi_terDA() override;
        void csi_DECRQDE() override;
        void csi_DECREQTPARM(u32 permission) override;
        void csi_DECRQTSR_COLOR(u32 model) override;
        void csi_DECRQPSR_TABS() override;
        void csi_DECRQPSR_CURSOR() override;
        void csi_DECRQUPSS() override;
        void dsrOperatingStatus() override;
        void dsrCursorPosition(bool privateMode) override;
        void dsrPrinter() override;
        void dsrUserDefinedKeys() override;
        void dsrKeyboard() override;
        void dsrLocator() override;
        void dsrLocatorType() override;
        void dsrMacroSpace() override;
        void dsrMemoryChecksum(u32 requestId) override;
        void dsrDataIntegrity() override;
        void dsrMultipleSession() override;
        void dsrColorScheme() override;
        void sgrReset() override;
        void sgrBold(bool enabled) override;
        void sgrFaint(bool enabled) override;
        void sgrItalic(bool enabled) override;
        void sgrUnderline(u8 style) override;
        void sgrBlink(bool enabled) override;
        void sgrInverse(bool enabled) override;
        void sgrConceal(bool enabled) override;
        void sgrStrike(bool enabled) override;
        void sgrOverline(bool enabled) override;
        void sgrForeground(CellColor color, int paletteIndex, bool brightenBold) override;
        void sgrDefaultForeground() override;
        void sgrBackground(CellColor color, int paletteIndex) override;
        void sgrDefaultBackground() override;
        void sgrUnderlineColor(CellColor color, int paletteIndex) override;
        void sgrDefaultUnderlineColor() override;
        void sgrFinish() override;
        void csi_XTPUSHSGR(const u32* attributes, size_t count) override;
        void csi_XTPOPSGR() override;
        void esch_DECALN() override;
        void setLineAttribute(u8 attribute) override;
        void osc_TITLE_0(StringView) override;
        void osc_TITLE_1(StringView) override;
        void osc_TITLE_2(StringView) override;
        void osc_PALETTE(u32, Color, bool) override;
        void osc_SPECIAL_COLOR(u32, Color, bool) override;
        void osc_SPECIAL_COLOR_MODE(u32, u32) override;
        void osc_RAW(u32, StringView) override;
        void osc_CWD(StringView, bool) override;
        void osc_HYPERLINK(StringView, bool, StringView) override;
        void osc_NOTIFY(StringView) override;
        void osc_PROGRESS(u32, u32, bool) override;
        void osc_DEFAULT_FOREGROUND(Color, bool) override;
        void osc_DEFAULT_BACKGROUND(Color, bool) override;
        void osc_CURSOR_COLOR(Color, bool) override;
        void osc_SELECTION_BACKGROUND(Color, bool) override;
        void osc_SELECTION_FOREGROUND(Color, bool) override;
        void writeDynamicColorResponse(u32, Color);
        void writeKittyClipboardStatus(StringView, StringView, StringView);
        void osc_CLIPBOARD_QUERY(bool, bool, u8, bool) override;
        void osc_CLIPBOARD_WRITE(StringView, bool, bool, bool) override;
        void osc_KITTY_CLIPBOARD_READ(StringView, StringView, bool, bool) override;
        void osc_KITTY_CLIPBOARD_WRITE(StringView, bool) override;
        void abortKittyClipboardWrite(StringView status);
        void osc_KITTY_CLIPBOARD_WRITE_DATA(StringView, StringView, StringView, bool) override;
        void osc_KITTY_CLIPBOARD_WRITE_ALIAS(StringView, StringView, StringView, bool) override;
        void osc_KITTY_CLIPBOARD_INVALID(StringView, bool) override;
        void osc_NOTIFICATION_CAPABILITIES(StringView) override;
        void osc_NOTIFICATION_CLOSE(StringView) override;
        void osc_NOTIFICATION_TITLE(StringView, StringView, bool, bool) override;
        void osc_NOTIFICATION_BODY(StringView, StringView, bool, bool) override;
        void applyNotificationPart(StringView, StringView, bool, bool, bool);
        void osc_RESET_PALETTE() override;
        void osc_RESET_PALETTE(u32) override;
        void osc_RESET_SPECIAL_COLOR() override;
        void osc_RESET_SPECIAL_COLOR(u32) override;
        void osc_RESET_DEFAULT_FOREGROUND() override;
        void osc_RESET_DEFAULT_BACKGROUND() override;
        void osc_RESET_CURSOR_COLOR() override;
        void osc_RESET_SELECTION_BACKGROUND() override;
        void osc_RESET_SELECTION_FOREGROUND() override;
        void osc_SHELL_A(StringView) override;
        void osc_SHELL_B(StringView) override;
        void osc_SHELL_C(StringView) override;
        void osc_SHELL_D(StringView) override;
        void osc_SHELL_I(StringView) override;
        void osc_SHELL_L(StringView) override;
        void osc_SHELL_N(StringView) override;
        void osc_SHELL_P(StringView) override;
        void osc_SHELL_UNKNOWN(StringView) override;
        void osc_UNKNOWN(u32, StringView) override;
        void csi_DECSCL(CompatibilityLevel level, bool send8BitControls) override;
        void xtResizePixels(u32 height, bool heightPresent, u32 width, bool widthPresent) override;
        void xtResizeCells(u32 height, bool heightPresent, u32 width, bool widthPresent) override;
        void xtWindowOperation(u32 operation, u32 first, u32 second) override;
        void xtReportWindowState() override;
        void xtReportWindowPosition() override;
        void xtReportWindowPixelSize(bool compositorSize) override;
        void xtReportScreenPixelSize() override;
        void xtReportCellSize() override;
        void xtReportGridSize() override;
        void xtReportScreenGridSize() override;
        void xtReportIconTitle() override;
        void xtReportWindowTitle() override;
        void xtPushTitle(bool icon, bool window) override;
        void xtPopTitle(bool icon, bool window) override;
        void xtResizeRows(u32 rows) override;
        void resetTitleModes() override;
        void setTitleMode(u8 bit, bool enabled) override;
        void csi_XTHIMOUSE(u32 start, u32 startX, u32 startY, u32 firstRow, u32 lastRow) override;
        void setLocatorReporting(bool enabled, bool oneShot, bool pixels) override;
        void resetLocatorEvents() override;
        void setLocatorButtonDown(bool enabled) override;
        void setLocatorButtonUp(bool enabled) override;
        void csi_DECRQLP() override;
        void csi_DECEFR(u32 top, u32 left, u32 bottom, u32 right) override;
        void csi_DECAC_TEXT(u8 foreground, u8 background) override;
        void csi_DECAC_TEXT_RESET() override;
        void csi_DECAC_FRAME(u8 foreground, u8 background) override;
        void csi_DECAC_FRAME_RESET() override;
        void resetModifyKeyResources() override;
        void setModifyKeyResource(u8 resource, u8 value, bool useDefault) override;
        void reportModifyKeyResource(u8 resource) override;
        void csi_kittyKeyboardPush(u32 flags) override;
        void csi_kittyKeyboardPop(u32 count) override;
        void setKittyKeyboardFlags(u8 flags) override;
        void addKittyKeyboardFlags(u8 flags) override;
        void removeKittyKeyboardFlags(u8 flags) override;
        void csi_kittyKeyboardQuery() override;
        void csi_XTVERSION() override;
        void csi_XTSMGRAPHICS(u32 item, u32 action, u32 value) override;
        void csi_SETMARK() override;
        void resetLeds() override;
        void setLed(u8 index, bool enabled) override;
        void commitLeds() override;
        void dcs_DECRQSS_DECSCL() override;
        void dcs_DECRQSS_SGR() override;
        void dcs_DECRQSS_DECSTBM() override;
        void dcs_DECRQSS_DECSLRM() override;
        void dcs_DECRQSS_DECSLPP() override;
        void dcs_DECRQSS_DECSCUSR() override;
        void dcs_DECRQSS_DECSCA() override;
        void dcs_DECRQSS_DECSACE() override;
        void dcs_DECRQSS_UNKNOWN() override;
        void writeDecrqssResponse(StringView);
        void dcs_XTGETTCAP(StringView encoded, StringView value) override;
        void dcs_DECUDK(bool clearDefinitions, bool lockDefinitions, const ParserUdkDefinition* definitions, size_t definitionCount, StringView values) override;
        void dcs_DECRSTS_HLS(u32 index, u32 hue, u32 luminosity, u32 saturation) override;
        void dcs_DECRSTS_RGB(u32 index, u32 red, u32 green, u32 blue) override;
        void dcs_DECRSTS_TABS_BEGIN() override;
        void dcs_DECRSTS_TAB(u32 column) override;
        void dcs_DECRSTS_CURSOR(u32 row, u32 column, u8 rendition, u8 protection, u8 flags, u8 gl, u8 gr, u8 sizeFlags, const Charset* charsets, const u16* charsetIds) override;
        void dcs_DECAUPSS(Charset charset, u16 id, bool is96) override;
        void dcs_SIXEL(const ParserSixelImage& image) override;

        void reportInBandResize();
        void reportColorScheme();
        void writeTitleResponse(char, StringView);
        void applyPaletteColor(u16 index, Color color);

        VtermInput input;
        VtermTimerBody syncBody_{this, &VtermImpl::runSyncWatchdog};
        VtermTimerBody blinkBody_{this, &VtermImpl::runBlink};
        VtermTimerBody autoscrollBody_{this, &VtermImpl::runAutoscroll};
        plt::Fiber* syncFiber_ = nullptr;
        plt::Fiber* blinkFiber_ = nullptr;
        plt::Fiber* autoscrollFiber_ = nullptr;
        ObjPool& owner_;
        // Upstream's explicit embedding pieces, in upstream's order.
        // windowGeometry_ is the window's: its surface size and the cell
        // size the font gives it, one per window and shared by every
        // pane on it. This pane's own rectangle is pane_ below - the
        // same type, a different instance, and never the same numbers
        // once a window holds two panes.
        VtGeometry& windowGeometry_;
        const VtConfigSlot& configSlot_;
        VtCellExtras& extras_;
        SmallObjAllocator& smallObjects_;
        plt::Scheduler& scheduler_;
        VtHost& host;
        // A8: this terminal's own geometry, handed to it by whatever lays
        // panes out, rather than read back off the window - its grid,
        // its rectangle on the surface, how far its text reaches and the
        // border it keeps around that text. Every "columns" and "rows"
        // below is this pane's, not the window's, and while a window
        // shows one terminal the two are the same numbers, which is what
        // makes the whole migration invisible.
        //
        // T5.5: the grid is read straight out of here. The pair of u16
        // that used to shadow pane_.columns/pane_.rows is gone; there is
        // one instance of this pane's size and nothing to keep in step
        // with it.
        //
        // The window's half of the geometry is not copied in here: the
        // surface and the cell size stay in windowGeometry_ and are read
        // from there at the call sites that ask about the window. One
        // pane holding its own copy of a number the whole window shares
        // is a copy that goes stale between a font change and the resize
        // that follows it.
        //
        // Declared here, above nColsEff, because the constructor's
        // initializer list reads pane_.columns to seed it and members are
        // initialized in declaration order.
        VtGeometry pane_;
        // Captured per terminal: a deferred transaction that resumes after
        // a tab switch must still write to the shell it began talking to.
        PtyBlockOutput ptyStream_;
        Output* const ptyOutput_;
        plt::FiberMutex* const ptyMutex_;
        VtermTrace* const trace;
        Output* dump;
        UnicodeMap<u8>* const unicodeProperties;
        Buffer protocolResponseScratch;
        Vector<TerminalRow> outputRows;
        // IME composition preview: overlay cells composed into the frame
        // at output() time, foot-style. preeditWindow holds the visible
        // slice the synthetic span points at.
        Vector<TerminalCell> preeditCells;
        Vector<TerminalCell> preeditWindow;
        i32 preeditCursorBeginCell = -1;
        i32 preeditCursorEndCell = -1;
        TerminalUpdate terminalUpdate;
        // Storage of its own rather than terminalUpdate's, so that asking
        // one terminal for both forms cannot turn a frame already handed
        // out by output() into a zero-damage one behind the caller's back.
        TerminalUpdate retainedUpdate;

        Buffer inputResult;
        bool outputPending = false;
        Screen* updateScreen = nullptr;
        u64 presentedRevision = 0;
        u64 updatingRevision = 0;
        u32 revision = 1;

        Vector<TerminalCell*> extraCells;
        u32 processInputDepth = 0;
        FiberBlock* fiberBlocks_ = nullptr;
        // Transactions in flight; a dying session's arena waits for zero.
        bool presentedSinceGcSafePoint = false;

        TerminalColors colors;
        Color originalPalette256[256];
        Screen* frame_pri = nullptr;
        ObjPool* framePriPool = nullptr;
        Screen* frame_alt = nullptr;
        ObjPool* frameAltPool = nullptr;
        Screen* cf = nullptr;
        u16 posX = 0;
        u16 posY = 0;
        u16 marginTop = 0;
        u16 marginBottom = 0;
        bool lastCol = false;

        TerminalCell attrs{};
        TerminalCell eraseAttrs{};

        struct SavedSgr {
            TerminalCell attrs;
            int fgPalIx;
            int bgPalIx;
            int underlinePalIx;
            u32 valid;
            bool underlineColorDefault;
        };

        SavedSgr sgrStack[10]{};
        u8 sgrStackNext = 0;
        u8 sgrStackCount = 0;
        Color cursorColor;
        Color selectionFgColor;
        Color selectionBgColor;
        u8 selectionColorMask = 0;
        u32 activeHyperlink = 0;
        u32 nextHyperlink = 1;
        u32 currentSemantic = 0;
        bool semanticUntilEndOfLine = false;
        // The command line zsh last reported (OSC 7701) since the input began
        // (OSC 133;B), and the column it began at.
        PromptLine promptLine;
        bool promptReported = false;
        u16 promptColumn = 0;
        u32 inactiveSemantic = 0;
        bool inactiveSemanticUntilEndOfLine = false;
        enum class SemanticClick : u8 {
            None,
            Absolute,
            Relative,
            Line,
            Multiple,
            ConservativeVertical,
            SmartVertical,
        };
        SemanticClick semanticClick = SemanticClick::None;
        SemanticClick inactiveSemanticClick = SemanticClick::None;
        bool assignedDefaultColors = false;
        Buffer windowTitle;
        Buffer iconTitle;
        // The title last offered to the host. It may be the current working
        // directory while the explicit window title is still the default.
        Buffer presentedTitle;
        bool titleSet = false;
        // The foreground process name last seen on the pty; a change of
        // it is what retires the previous foreground's title.
        Buffer foregroundName_;
        u8 titleModes = 0;

        struct SavedTitles {
            bool hasIcon = false;
            bool hasWindow = false;
            Buffer icon;
            Buffer window;
        };

        // The xterm title stack is capped at ten entries; a push beyond that
        // drops the oldest.
        SavedTitles titleStack[10];
        size_t titleDepth = 0;

        struct NotificationPart {
            Buffer text;
            size_t encodedOffset = (size_t)-1;
        };

        struct Notification {
            u64 key = 0;
            NotificationPart title;
            NotificationPart body;
        };

        IntMap<Notification> notifications;
        int defaultFgPalIx;
        int defaultBgPalIx;
        int fgPalIx;
        int bgPalIx;
        int underlinePalIx = -2;
        bool reverseVideo = false;
        bool screenReverseVideo = false;
        bool underlineColorDefault = true;
        bool hasFocus = false;

        Parser* parser;
        Utf8Decoder utf8dec;
        GraphemeBuffer inputGrapheme;
        u32 inputGraphemeBase = 0;
        GraphemeBreaker inputGraphemeBreaker;
        Screen* inputGraphemeScreen = nullptr;
        u16 inputGraphemeX = 0;
        u16 inputGraphemeY = 0;
        bool inputGraphemeWide = false;
        TerminalCell inputGraphemeAttrs{};
        u32 inputGraphemeHyperlink = 0;
        u32 inputGraphemeSemantic = 0;

        VtModifier modifiers = VtModifier::none;

        bool showCursorMode = true;
        bool noClearColumnMode = false;
        TerminalCursor::Style cursorShape = TerminalCursor::Style::filled_block;
        u8 cursorStyleParam = 2;
        bool cursorBlinkMode = false;
        bool haveBlinkingText = false;
        bool blinkVisible = true;
        bool cursorTemporarilyHidden = false;
        u64 nextBlink = 0;
        bool altScreenBufferMode = false;
        bool altScreenInitialized = false;
        bool autoWrapMode = true;
        bool autoRepeatMode = true;
        bool smoothScrollMode = false;
        bool allowColumnMode = false;
        bool moreFixMode = false;
        bool autoNewlineMode = false;
        bool keyboardLocked = false;
        bool insertMode = false;
        bool eraseModeAll = false;
        bool isoProtectionActive = false;
        bool rectangularAttributeExtent = false;
        u8 checksumFlags = 0;
        bool bkspSendsDel = true;
        bool localEcho = false;
        bool bracketedPasteMode = false;
        bool synchronizedOutputMode = false;
        bool graphemeClusterMode = true;
        bool colorSchemeUpdateMode = false;
        bool inBandResizeMode = false;
        bool pasteMimeNotificationsMode = false;
        u32 progressPercent = 0;
        u64 synchronizedOutputDeadline = 0;
        bool send8BitControls = false;
        bool altScrollMode = false;
        bool altSendsEscape = true;
        bool eightBitInput = false;
        bool reverseWrapMode = false;
        bool extendedReverseWrapMode = false;
        bool nationalReplacementMode = false;
        u8 ledState = 0;
        u8 modifyOtherKeys = 1;
        u8 modifyKeyResources[8] = {};
        u8 initialModifyKeyResources[8] = {};

        struct SavedMode {
            u64 key = 0;
            bool enabled = false;
        };

        struct UserKey {
            u64 key = 0;
            Buffer text;
        };

        IntMap<SavedMode> savedPrivModes;
        IntMap<UserKey> userDefinedKeys;
        bool userDefinedKeysLocked = false;
        Buffer kittyClipboardWriteId;
        // The open write transaction; deleting it without finish() aborts.
        stl::Output* kittyClipboardWriteStream = nullptr;
        size_t kittyClipboardWriteLength = 0;

        struct KittyKeyboardState {
            u8 flags = 0;
            Vector<u8> stack;
        };

        KittyKeyboardState kittyKeyboardPri;
        KittyKeyboardState kittyKeyboardAlt;

        KittyKeyboardState& kittyKeyboardState();
        const KittyKeyboardState& kittyKeyboardState() const;

        bool horizMarginMode = false;
        u16 nColsEff = 0;
        u16 hMargin = 0;

        Vector<u16> tabStops;
        bool tabStopsCustomized = false;
        bool tabStopsRestored = false;

        CompatibilityLevel compatLevel = CompatibilityLevel::VT400;

        enum class CursorKeyMode : u8 {
            ANSI,
            Application
        };
        CursorKeyMode cursorKeyMode = CursorKeyMode::ANSI;

        enum class KeypadMode : u8 {
            Normal,
            Application
        };
        KeypadMode keypadMode = KeypadMode::Normal;

        enum class OriginMode : u8 {
            Absolute,
            ScrollingRegion
        };
        OriginMode originMode = OriginMode::Absolute;

        enum class ColMode : u8 {
            C80,
            C132
        };
        ColMode colMode = ColMode::C80;

        void switchColMode(ColMode colMode, bool force = false);
        void switchScreenBufferMode(bool altScreenBufferMode, bool clearAlternate = false);
        bool cursorIsAtPrompt() const;
        void startSemanticPrompt(StringView payload);

        struct CharsetState {
            Charset g[4] = {Charset::UTF8, Charset::UTF8, Charset::UTF8, Charset::UTF8};
            u16 ids[4] = {'B', 'B', 'B', 'B'};

            u8 gl = 0;
            u8 gr = 2;

            u8 ss = 0;
            u8 size96 = 0;
        };

        CharsetState charsetState;
        Charset userPreferenceCharset = Charset::DecSuppl;
        u16 userPreferenceCharsetId = ((u16)('%') << 8) | '5';
        bool userPreferenceCharset96 = false;

        static const u16* charCodes[];
        u32 translateCharset(Charset charset, unsigned char ch) const;

        struct SavedCursor {
            bool isSet = false;
            u16 posX = 0;
            u16 posY = 0;
            bool lastCol = false;
            TerminalCell attrs{};
            TerminalCell eraseAttrs{};
            OriginMode originMode = OriginMode::Absolute;
            CharsetState charsetState = CharsetState{};
            int fgPalIx = -2;
            int bgPalIx = -2;
            int underlinePalIx = -2;
            bool underlineColorDefault = true;
        };

        SavedCursor savedCursorPri;
        SavedCursor savedCursorAlt;
        SavedCursor* savedCursor = &savedCursorPri;

        bool selectUpdatesTop = false;
        bool selectUpdatesLeft = false;
        bool selectPivotFixed = false;

        MouseTrackingState mouseTrk;

        struct MouseHighlightState {
            bool active = false;
            u16 startX = 1;
            u16 startY = 1;
            u16 firstRow = 1;
            u16 lastRow = 1;
        } mouseHighlight;

        struct LocatorState {
            u8 enabled = 0;
            bool pixels = false;
            bool reportDown = false;
            bool reportUp = false;
            u8 buttons = 0;
            u16 column = 1;
            u16 row = 1;
            u16 pixelX = 1;
            u16 pixelY = 1;
            bool filter = false;
            u16 filterTop = 1;
            u16 filterLeft = 1;
            u16 filterBottom = 1;
            u16 filterRight = 1;
        } locator;
    };

    struct TestApiImpl final: public TestApi {
        explicit TestApiImpl(VtermImpl* vterm);

        VtermTestState inspect() const override;
        bool ansiMode(u32 mode) const override;
        bool privateMode(u32 mode) const override;
        void hardReset() override;
        bool tabStop(u16 column) const override;
        void setWrapped(u16 row) override;
        VtermTestCell cell(u16 row, u16 column) const override;
        VtermTestCell logicalCell(i32 row, u16 column) const override;
        u8 rowSemantic(i32 row) const override;
        u8 semanticClick() const override;
        bool cursorIsAtPrompt() const override;
        void key(InputKey key, VtModifier modifiers) override;
        void character(u8 byte, VtModifier modifiers) override;
        void kittyKey(InputKey key, u16 modifiers, VtermKeyEventType event) override;
        void kittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event) override;
        bool mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY) override;
        void locatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons) override;
        void locatorButton(u8 button, bool pressed) override;
        void scrollUp(u16 count) override;
        void scrollDown(u16 count) override;
        void pageUp() override;
        void pageDown() override;
        void selectionStart(int pixelX, int pixelY, bool cycleSnapTo) override;
        void selectionExtend(int pixelX, int pixelY, bool cycleSnapTo) override;
        void selectionUpdate(int pixelX, int pixelY) override;
        VtermTextResult selectionFinish() override;
        bool hasSelection() const override;
        void selectionClear() override;
        void selectionRectangular() override;
        bool advanceSelectionAutoscroll() override;
        void paste(StringView text) override;
        bool pasteClipboard(bool primary) override;
        StringView hyperlinkAt(int pixelX, int pixelY) override;

        VtermImpl* vterm;
    };

    void GraphemeBuffer::clear() {
        size_ = 0;
    }

    bool GraphemeBuffer::pushBack(u32 codepoint) {
        if (size_ == capacity) {
            return false;
        }
        values[size_++] = codepoint;
        return true;
    }

    bool GraphemeBuffer::empty() const {
        return size_ == 0;
    }

    size_t GraphemeBuffer::size() const {
        return size_;
    }

    const u32* GraphemeBuffer::data() const {
        return values;
    }
}

template <typename F>
void FiberTask<F>::run() {
    block->task = this;
    block->fiber = terminal->scheduler_.current();
    body();
    VtermImpl* const owner = terminal;
    FiberBlock* const spent = block;
    spent->task = nullptr;
    spent->fiber = nullptr;
    owner->smallObjects_.release(this);
    // Still running on spent's stack: safe, nothing can reuse it before
    // the final cooperative switch out.
    spent->next = owner->fiberBlocks_;
    owner->fiberBlocks_ = spent;
}

template <typename F>
void FiberTask<F>::cancel() {
    VtermImpl* const owner = terminal;
    owner->smallObjects_.release(this);
}

FiberBlock::~FiberBlock() noexcept {
    if (fiber == nullptr) {
        return;
    }
    // Arena destruction is deferred away from a session fiber, so this is
    // necessarily a blocked, never the current, transaction.
    fiber->release();
    fiber = nullptr;
    FiberTaskBase* const cancelled = task;
    task = nullptr;
    cancelled->cancel();
}

VtermTimerBody::VtermTimerBody(VtermImpl* parent_, void (VtermImpl::*method_)())
    : parent(parent_)
    , method(method_)
{
}

void VtermTimerBody::run() {
    (parent->*method)();
}

template <typename F>
void VtermImpl::spawnTransaction(F&& body) {
    FiberBlock* block = fiberBlocks_;
    if (block != nullptr) {
        fiberBlocks_ = block->next;
        block->next = nullptr;
    } else {
        block = owner_.make<FiberBlock>();
    }
    FiberTask<F>* const task = smallObjects_.make<FiberTask<F>>(this, block, static_cast<F&&>(body));
    scheduler_.spawn(*task, block->stack, sizeof(block->stack));
}

void VtermImpl::spawnPtyWrite(StringView bytes) {
    spawnTransaction([this, view = bytes] {
        // The spawn runs this prefix synchronously, so the caller's bytes
        // are alive exactly until the first suspension point: copy them
        // onto this fiber's stack (or the heap for a large payload) before
        // the lock can park us.
        u8 local[1024];
        Buffer owned;
        const u8* data;
        if (view.length() <= sizeof(local)) {
            memcpy(local, view.data(), view.length());
            data = local;
        } else {
            owned.append(view.data(), view.length());
            data = (const u8*)(owned.data());
        }
        const plt::LockGuard guard(*ptyMutex_);
        writePtyLocked(StringView(data, view.length()));
    });
}

VtermInput::VtermInput(VtermImpl* terminal_)
    : terminal(terminal_)
{
}

VtModifier VtermInput::legacyModifiers(u16 modifiers) const {
    VtModifier result = VtModifier::none;
    if (modifiers & InputShift) {
        result = result | VtModifier::shift;
    }
    if (modifiers & InputControl) {
        result = result | VtModifier::control;
    }
    if (modifiers & InputAlt) {
        result = result | VtModifier::alt;
    }
    if (modifiers & InputSuper) {
        result = result | VtModifier::super;
    }
    return result;
}

u16 VtermInput::kittyModifiers(u16 modifiers) const {
    u16 result = 0;
    if (modifiers & InputShift) {
        result |= 1;
    }
    if (modifiers & InputAlt) {
        result |= 2;
    }
    if (modifiers & InputControl) {
        result |= 4;
    }
    if (modifiers & InputSuper) {
        result |= 8;
    }
    if (modifiers & InputCapsLock) {
        result |= 64;
    }
    if (modifiers & InputNumLock) {
        result |= 128;
    }
    return result;
}

bool VtermInput::paste(bool primary) {
    if (terminal->pasteMimeNotificationsMode) {
        return terminal->pasteMimeNotification(primary);
    }
    VtHost& host = terminal->host;
    const bool bracketed = terminal->bracketedPasteMode;
    // Captured by value: the clipboard read parks this fiber for up to the
    // selection transfer timeout, and the terminal that started the paste
    // may not be the active one by the time it resumes.
    Output* const output = terminal->ptyOutput_;
    plt::FiberMutex* const mutex = terminal->ptyMutex_;
    terminal->spawnTransaction([&host, output, mutex, primary, bracketed] {
        const plt::LockGuard guard(*mutex);
        const ScopedPtr<Input> source{selectionTarget(host, primary)->read()};
        PasteOutput paste(output, bracketed);
        for (;;) {
            u8 chunk[8 * 1024];
            const size_t count = source->read(chunk, sizeof(chunk));
            if (count == 0) {
                break;
            }
            paste.write(chunk, count);
        }
    });
    return true;
}

bool VtermInput::copy() {
    // The chord copies this window's own selection, and with none it must
    // leave the clipboard alone: an application may have just filled it
    // through OSC 52 (a TUI selecting for itself is exactly that case),
    // and the old primary-to-clipboard pump would clobber that with
    // whatever stale selection was staged before - on macOS the Find
    // pasteboard is shared by every application's search field.
    if (!terminal->hasSelection()) {
        return true;
    }
    const VtermTextResult selected = terminal->selectionFinish();
    if (!selected.status) {
        return true;
    }
    writeSelection(*terminal->host.primary(), selected.text);
    writeSelection(*terminal->host.secondary(), selected.text);
    return true;
}

ScreenHyperlink VtermInput::resolveLink(int pixelX, int pixelY) {
    return terminal->resolveHyperlink(pixelX, pixelY);
}

bool VtermInput::refreshHyperlink() {
    ScreenHyperlink next;
    if (pointerFocused && pointerPresent && pointerPositionKnown && (pointerModifiers & hyperlinkModifiers)) {
        next = resolveLink(pointerX, pointerY);
    }
    if (next.displayId == hoveredHyperlink && next.begin == hoveredLinkBegin && next.end == hoveredLinkEnd) {
        return false;
    }
    const bool wasActive = hoveredHyperlink != 0 || hoveredLinkBegin < hoveredLinkEnd;
    hoveredHyperlink = next.displayId;
    hoveredLinkBegin = next.begin;
    hoveredLinkEnd = next.end;
    const bool active = hoveredHyperlink != 0 || hoveredLinkBegin < hoveredLinkEnd;
    if (active != wasActive) {
        terminal->host.requestPointerIcon(active ? plt::PointerIcon::Pointer : plt::PointerIcon::Text);
    }
    return true;
}

void VtermInput::refreshHyperlinkAndRedraw() {
    if (refreshHyperlink()) {
        terminal->redraw();
    }
}

void VtermInput::updatePointer(int pixelX, int pixelY, u16 modifiers) {
    pointerX = pixelX;
    pointerY = pixelY;
    pointerModifiers = modifiers;
    pointerPresent = true;
    pointerPositionKnown = true;
    refreshHyperlinkAndRedraw();
}

void VtermInput::updatePointerModifiers(const KeyInput& input) {
    pointerModifiers = input.modifiers;
    if (input.key == InputKey::LeftControl || input.key == InputKey::RightControl) {
        if (input.action == InputAction::Release) {
            pointerModifiers &= ~InputControl;
        } else {
            pointerModifiers |= InputControl;
        }
    }
    if (input.key == InputKey::LeftSuper || input.key == InputKey::RightSuper) {
        if (input.action == InputAction::Release) {
            pointerModifiers &= ~InputSuper;
        } else {
            pointerModifiers |= InputSuper;
        }
    }
    refreshHyperlinkAndRedraw();
}

int VtermInput::currentSelectionAutoscrollDirection() const {
    const unsigned selectionButtons = (1u << (unsigned)(PointerButton::Primary)) | (1u << (unsigned)(PointerButton::Secondary));
    if (!mouse.selectionOngoing() || !(mouse.buttons() & selectionButtons) || terminal->cf->currentSelection().null() || !pointerFocused || !pointerPresent || !pointerPositionKnown) {
        return 0;
    }
    return mouseAutoscrollDirection(pointerY, mouseGeometry(terminal->pane_, terminal->windowGeometry_));
}

void VtermInput::updateSelectionAutoscroll() {
    const int direction = currentSelectionAutoscrollDirection();
    if (direction == 0) {
        stopSelectionAutoscroll();
        return;
    }
    if (direction == selectionAutoscrollDirection && selectionAutoscrollDeadline != 0) {
        return;
    }
    selectionAutoscrollDirection = direction;
    selectionAutoscrollDeadline = monotonicNowUs() + selectionAutoscrollInterval;
    armTimeout();
}

void VtermInput::stopSelectionAutoscroll() {
    selectionAutoscrollDirection = 0;
    selectionAutoscrollDeadline = 0;
}

void VtermInput::armTimeout() {
    terminal->wakeTimers();
}

bool VtermInput::advanceSelectionAutoscroll(bool force) {
    if (selectionAutoscrollDeadline == 0) {
        return false;
    }
    const u64 now = monotonicNowUs();
    if (!force && now < selectionAutoscrollDeadline) {
        return false;
    }
    const int direction = currentSelectionAutoscrollDirection();
    if (direction == 0 || direction != selectionAutoscrollDirection) {
        stopSelectionAutoscroll();
        return false;
    }
    if (!terminal->cf->scrollView(direction < 0 ? 1 : -1)) {
        stopSelectionAutoscroll();
        return false;
    }
    terminal->refreshBlinkingText();
    terminal->selectUpdate(pointerX, pointerY);
    selectionAutoscrollDeadline = now + selectionAutoscrollInterval;
    armTimeout();
    return true;
}

void VtermInput::flush() {
    if (!pendingTextKey.active) {
        return;
    }
    const PendingTextKey pending = pendingTextKey;
    pendingTextKey.active = false;
    terminal->writeKittyKey(pending.primary, pending.shifted, pending.base, pending.modifiers, pending.event);
}

PtyBlockOutput::PtyBlockOutput(PtyHandle& pty_)
    : pty(&pty_)
{
}

size_t PtyBlockOutput::writeImpl(const void* data, size_t len) {
    constexpr size_t batchRequest = 64 * 1024;
    const u8* bytes = (const u8*)(data);
    size_t remaining = len;
    while (remaining != 0) {
        if (chunk == nullptr) {
            chunk = pty->allocate(batch ? batchRequest : remaining);
            filled = 0;
            batch = true;
        }
        const size_t space = chunk->length() - filled;
        const size_t count = remaining < space ? remaining : space;
        __builtin_memcpy((u8*)(chunk->data()) + filled, bytes, count);
        filled += count;
        bytes += count;
        remaining -= count;
        if (filled == chunk->length()) {
            PtyHandle::Chunk* const full = chunk;
            chunk = nullptr;
            pty->send(full, filled);
        }
    }
    return len;
}

void PtyBlockOutput::flushImpl() {
    if (chunk != nullptr) {
        PtyHandle::Chunk* const tail = chunk;
        chunk = nullptr;
        pty->send(tail, filled);
    }
    batch = false;
}

PasteOutput::PasteOutput(Output* output_, bool bracketed_)
    : output(output_)
    , bracketed(bracketed_)
{
}

PasteOutput::~PasteOutput() noexcept {
    if (pendingC1Lead) {
        const u8 lead = 0xc2;
        output->write(&lead, 1);
    }
    if (started && bracketed) {
        output->write(StringView(u8"\x1b[201~").data(), 6);
    }
    output->flush();
}

size_t PasteOutput::writeImpl(const void* data, size_t size) {
    if (size == 0) {
        return 0;
    }
    begin();
    const u8 carriageReturn = '\r';
    const u8* current = static_cast<const u8*>(data);
    const u8* const end = current + size;

    if (pendingC1Lead) {
        pendingC1Lead = false;
        if (*current >= 0x80 && *current <= 0x9f) {
            output->write(StringView(u8"\xef\xbf\xbd").data(), 3);
            ++current;
        } else {
            const u8 lead = 0xc2;
            output->write(&lead, 1);
        }
    }

    while (current != end) {
        if (previousCarriageReturn) {
            previousCarriageReturn = false;
            if (*current == '\n') {
                ++current;
                continue;
            }
        }

        const u8* span = current;
        while (current != end) {
            const u8 byte = *current;
            if ((byte >= 0x01 && byte <= 0x08) || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f || byte == 0xc2) {
                break;
            }
            ++current;
        }
        if (span != current) {
            output->write(span, current - span);
        }
        if (current == end) {
            break;
        }

        const u8 byte = *current++;
        if (byte == '\n') {
            output->write(&carriageReturn, 1);
        } else if (byte == '\r') {
            output->write(&carriageReturn, 1);
            previousCarriageReturn = true;
        } else if (byte == 0xc2) {
            if (current == end) {
                pendingC1Lead = true;
            } else if (*current >= 0x80 && *current <= 0x9f) {
                output->write(StringView(u8"\xef\xbf\xbd").data(), 3);
                ++current;
            } else {
                output->write(&byte, 1);
            }
        } else {
            u8 picture[] = {0xe2, 0x90, (u8)(byte + 0x80)};
            if (byte == 0x7f) {
                picture[2] = 0xa1;
            }
            output->write(picture, sizeof(picture));
        }
    }
    output->flush();
    return size;
}

void PasteOutput::begin() {
    if (started) {
        return;
    }
    started = true;
    if (bracketed) {
        output->write(StringView(u8"\x1b[200~").data(), 6);
    }
}

static u8 numericKeypadCharacter(InputKey key) {
    if (key >= InputKey::Keypad0 && key <= InputKey::Keypad9) {
        return '0' + (u8)(key) - (u8)(InputKey::Keypad0);
    }
    switch (key) {
        case InputKey::KeypadDecimal:
        case InputKey::KeypadDelete:
            return '.';
        case InputKey::KeypadDivide:
            return '/';
        case InputKey::KeypadMultiply:
            return '*';
        case InputKey::KeypadSubtract:
            return '-';
        case InputKey::KeypadAdd:
            return '+';
        case InputKey::KeypadEnter:
            return '\r';
        case InputKey::KeypadEqual:
            return '=';
        case InputKey::KeypadSeparator:
            return ',';
        case InputKey::KeypadInsert:
            return '0';
        case InputKey::KeypadEnd:
            return '1';
        case InputKey::KeypadDown:
            return '2';
        case InputKey::KeypadPageDown:
            return '3';
        case InputKey::KeypadLeft:
            return '4';
        case InputKey::KeypadBegin:
            return '5';
        case InputKey::KeypadRight:
            return '6';
        case InputKey::KeypadHome:
            return '7';
        case InputKey::KeypadUp:
            return '8';
        case InputKey::KeypadPageUp:
            return '9';
        case InputKey::KeypadSpace:
            return ' ';
        case InputKey::KeypadTab:
            return '\t';
        default:
            return 0;
    }
}

bool VtermInput::key(const KeyInput& input) {
    flush();
    updatePointerModifiers(input);
    suppressRepeatedTextInput = input.action == InputAction::Repeat && !terminal->autoRepeatMode;
    const VtModifier modifiers = legacyModifiers(input.modifiers);
    const bool pressed = input.action != InputAction::Release;
    if (!pressed && input.baseCodepoint == ' ' && rectangularSelectionKeyConsumed) {
        rectangularSelectionKeyConsumed = false;
        return true;
    }
    if (input.baseCodepoint == ' ' && mouse.selectionOngoing()) {
        if (pressed) {
            rectangularSelectionKeyConsumed = true;
            terminal->selectionRectangular();
        }
        return true;
    }
    if (suppressRepeatedTextInput) {
        return true;
    }
    if (pressed && (input.key == InputKey::Backspace || input.key == InputKey::Delete) && terminal->hasSelection() && terminal->promptReplaceSelection(nullptr, 0)) {
        return true;
    }

    const u8 kittyFlags = terminal->getKittyKeyboardFlags();
    const u16 kittyMods = kittyModifiers(input.modifiers);
    const VtermKeyEventType event = input.action == InputAction::Release ? VtermKeyEventType::Release : input.action == InputAction::Repeat ? VtermKeyEventType::Repeat : VtermKeyEventType::Press;
    if (kittyFlags) {
        if (input.key == InputKey::Escape) {
            terminal->writeKittyKey(27, 0, 0, kittyMods, event);
            return true;
        }
        if (input.key != InputKey::Unknown && input.key != InputKey::Printable && input.key != InputKey::Space) {
            terminal->writeKittyKey(input.key, kittyMods, event);
            return true;
        }
        const u16 textMods = kittyMods & ~(64 | 128);
        const u32 layoutKey = input.layoutCodepoint != 0 ? input.layoutCodepoint : input.baseCodepoint;
        const u32 shiftedKey = textMods & 1 ? input.shiftedCodepoint : 0;
        const bool baseLayoutShortcut = terminal->config().kittyCtrlBaseLayout && (kittyFlags & 0x04) && (textMods & 4) && !(input.modifiers & InputAltGraph) && layoutKey >= 0x80 && input.baseCodepoint >= 0x20 && input.baseCodepoint < 0x7f;
        // Compatibility for consumers that ignore Kitty's base-layout field.
        const u32 primaryKey = baseLayoutShortcut ? input.baseCodepoint : layoutKey;
        // A text-producing repeat stays plain text unless report-all is set;
        // only its release has no text event and therefore needs a CSI report.
        const bool reportRelease = (kittyFlags & 0x02) && event == VtermKeyEventType::Release;
        if (primaryKey && ((textMods & (2 | 4 | 8)) || (kittyFlags & 0x08) || reportRelease)) {
            if (pressed && !(textMods & (2 | 4 | 8))) {
                pendingTextKey = {
                    true,
                    primaryKey,
                    shiftedKey,
                    input.baseCodepoint,
                    kittyMods,
                    event,
                };
                return true;
            }
            terminal->writeKittyKey(primaryKey, shiftedKey, input.baseCodepoint, kittyMods, event);
            // The packet swallows the text event this press is about to
            // deliver - but the frontends deliver text only without
            // Control and without Super (cocoa's interpretKeyEvents and
            // the wayland key handler gate it identically), so only an
            // Alt-modified press has one coming. Counting a suppression
            // for a press that never produces text eats the next typed
            // character instead.
            if (pressed && (textMods & 2) && !(textMods & (4 | 8))) {
                ++suppressedTextInputs;
            }
            return true;
        }
    }
    if (!pressed) {
        return true;
    }
    const u8 keypadCharacter = numericKeypadCharacter(input.key);
    if (keypadCharacter != 0 && ((input.modifiers & InputNumLock) || terminal->keypadMode == VtermImpl::KeypadMode::Normal)) {
        terminal->sendCharacter(keypadCharacter, modifiers);
        return true;
    }
    if (input.key == InputKey::Escape) {
        terminal->sendCharacter((u8)('\x1b'), modifiers);
        return true;
    }
    if (input.key != InputKey::Unknown && input.key != InputKey::Printable && input.key != InputKey::Space) {
        if (input.key == InputKey::Tab && (modifiers & VtModifier::shift) != VtModifier::none) {
            if ((modifiers & VtModifier::alt) != VtModifier::none) {
                terminal->sendCharacter((u8)('\x1b'), VtModifier::none);
            }
            terminal->sendKey(InputKey::Tab, VtModifier::shift);
            return true;
        }
        if (input.key == InputKey::Backspace && (modifiers & VtModifier::control) != VtModifier::none) {
            terminal->sendCharacter((u8)(terminal->bkspSendsDel ? '\b' : '\x7f'), modifiers & VtModifier::alt);
            return true;
        }
        terminal->sendKey(input.key, modifiers);
        return true;
    }
    if (input.modifiers & InputControl) {
        // The layout's own key wins while it prints ASCII - on QWERTZ the
        // key labeled Z must give Ctrl+Z, not the positional Ctrl+Y; the
        // base layout is the fallback for non-Latin layouts, which have
        // no control byte of their own (kitty's legacy rule). A key with
        // no ASCII on either layer has nothing to encode in any mode.
        const u32 layoutKey = input.layoutCodepoint;
        const u32 ruleKey = layoutKey >= 0x20 && layoutKey < 0x7f ? layoutKey : input.baseCodepoint;
        if (ruleKey < 0x20 || ruleKey >= 0x7f) {
            return true;
        }
        if (terminal->modifyOtherKeys == 2 && terminal->modifyOtherKeyEncoded((u8)(ruleKey), modifiers)) {
            terminal->sendCharacter((u8)(ruleKey), modifiers);
            return true;
        }
        int controlKey = (int)(ruleKey);
        if (controlKey >= 'a' && controlKey <= 'z') {
            controlKey -= 'a' - 'A';
        }
        u8 character = 0;
        if (controlCharacter(controlKey, input.modifiers & InputShift, character)) {
            terminal->sendCharacter(character, modifiers);
        }
    }
    return true;
}

bool VtermInput::text(const TextInput& input) {
    if (suppressRepeatedTextInput) {
        return true;
    }
    if (pendingTextKey.active) {
        const PendingTextKey pending = pendingTextKey;
        pendingTextKey.active = false;
        const u32 alternate = input.codepoint != pending.primary ? input.codepoint : 0;
        terminal->writeKittyKey(pending.primary, alternate, pending.base, pending.modifiers, pending.event);
        return true;
    }
    if (suppressedTextInputs) {
        --suppressedTextInputs;
        return true;
    }
    if (input.codepoint == 0) {
        return false;
    }
    // Typed over a selection in the command line: it replaces what is
    // selected, as in a text field.
    if (input.codepoint >= 0x20 && input.codepoint != 0x7f && terminal->hasSelection()) {
        const u32 typed = input.codepoint;
        if (terminal->promptReplaceSelection(&typed, 1)) {
            return true;
        }
    }
    const VtModifier modifiers = legacyModifiers(input.modifiers);
    if (input.codepoint < 0x80) {
        terminal->sendCharacter((u8)(input.codepoint), modifiers);
        return true;
    }
    u8 encoded[4];
    size_t size = 0;
    Utf8Encoder::pushUnicode(input.codepoint, [&](u8 byte) {
        encoded[size++] = byte;
    });
    if ((input.modifiers & InputAlt) && terminal->altSendsEscape) {
        terminal->sendUserInput(StringView(u8"\x1b"));
    }
    terminal->sendUserInput(StringView(encoded, size));
    return true;
}

void VtermInput::mouseProtocolCoordinates(MouseTrackingEnc encoding, int pixelX, int pixelY, u16& column, u16& row) const {
    const MouseProtocolPoint point = mouseProtocolPoint(encoding, pixelX, pixelY, mouseGeometry(terminal->pane_, terminal->windowGeometry_));
    column = point.column;
    row = point.row;
}

void VtermInput::sendMouseProtocol(MouseTrackingEnc encoding, MouseEventType type, u16 modifiers, int button, int column, int row) {
    const unsigned protocolModifiers = mouseProtocolModifiers(modifiers);
    StringBuilder report;
    if (encodeMouseProtocol(report, encoding, type, protocolModifiers, mouse.motionButton(), button, column, row)) {
        terminal->writePty(StringView((const u8*)(report.data()), report.used()));
    }
}

void VtermInput::sendMouseButtonProtocol(MouseEventType type, int button, int pixelX, int pixelY, u16 modifiers, const MouseTrackingState& tracking) {
    if (!mouseButtonReportAllowed(tracking.mode, type, button)) {
        return;
    }
    u16 column = 0;
    u16 row = 0;
    mouseProtocolCoordinates(tracking.enc, pixelX, pixelY, column, row);
    if (tracking.mode == MouseTrackingMode::VT200_Highlight && type == MouseEventType::Release) {
        terminal->mouseHighlightRelease(column, row, column, row);
        return;
    }
    sendMouseProtocol(tracking.enc, type, tracking.mode == MouseTrackingMode::X10_Compat ? 0 : modifiers, button, column, row);
}

bool VtermInput::pointerButton(const PointerButtonInput& input) {
    updatePointer(input.pixelX, input.pixelY, input.modifiers);
    const int button = (int)(input.button);
    mouse.updateButton(button, input.pressed);
    if (!input.pressed && (input.button == PointerButton::Primary || input.button == PointerButton::Secondary)) {
        stopSelectionAutoscroll();
    }
    const MouseTrackingState tracking = terminal->mouseTrk;
    const int protocolButton = mouseTerminalButton(button);
    u16 locatorColumn = 1;
    u16 locatorRow = 1;
    mouseProtocolCoordinates(MouseTrackingEnc::Default, input.pixelX, input.pixelY, locatorColumn, locatorRow);
    terminal->setLocatorPosition(locatorColumn, locatorRow, max(1, input.pixelX + 1), max(1, input.pixelY + 1), 0);
    if (protocolButton >= 1 && protocolButton <= 4) {
        terminal->reportLocatorButton(protocolButton, input.pressed);
    }
    if (!input.pressed && input.button == PointerButton::Primary && hyperlinkClick) {
        hyperlinkClick = false;
        return true;
    }
    if (input.pressed && input.button == PointerButton::Primary) {
        hyperlinkClick = false;
        if (input.modifiers & hyperlinkModifiers) {
            const ScreenHyperlink link = resolveLink(input.pixelX, input.pixelY);
            if (!link.payload.empty()) {
                hyperlinkClick = true;
                terminal->host.requestOpenUri(link.payload);
                return true;
            }
        }
    }
    if (mouse.protocolActive(input.modifiers, tracking.mode)) {
        sendMouseButtonProtocol(input.pressed ? MouseEventType::Press : MouseEventType::Release, protocolButton, input.pixelX, input.pixelY, input.modifiers, tracking);
        return true;
    }
    if (input.pressed) {
        const bool cycleSnapTo = mouse.registerClick(button, input.pixelX, input.pixelY, input.time) > 1;
        promptClick = input.button == PointerButton::Primary && !cycleSnapTo && !(input.modifiers & InputShift);
        promptClickX = input.pixelX;
        promptClickY = input.pixelY;
        if (input.button == PointerButton::Primary) {
            if ((input.modifiers & InputShift) && terminal->hasSelection()) {
                terminal->selectionExtend(input.pixelX, input.pixelY, cycleSnapTo);
            } else {
                terminal->selectionStart(input.pixelX, input.pixelY, cycleSnapTo);
            }
            mouse.beginSelection();
        } else if (input.button == PointerButton::Secondary) {
            terminal->selectionExtend(input.pixelX, input.pixelY, cycleSnapTo);
            mouse.beginSelection();
        }
        return true;
    }
    if (input.button == PointerButton::Primary || input.button == PointerButton::Secondary) {
        mouse.endSelection();
        const VtermTextResult selected = terminal->selectionFinish();
        // A click that selected nothing, in the command line: its cursor goes
        // where the click was, as in a text field.
        if (input.button == PointerButton::Primary && promptClick && !selected.status && !terminal->hasSelection()) {
            terminal->promptPlaceCursor(promptClickX, promptClickY);
        } else if (input.button == PointerButton::Primary && terminal->config().verbose) {
            fprintf(stderr, "%.*s: prompt editor: release not a click: single=%d selected=%d selection=%d\n", (int)(terminal->config().brandName.length()), (const char*)(terminal->config().brandName.data()), (int)(promptClick), (int)(selected.status), (int)(terminal->hasSelection()));
        }
        promptClick = false;
        if (selected.status) {
            writeSelection(*terminal->host.primary(), selected.text);
            if (terminal->config().autoCopyMode) {
                writeSelection(*terminal->host.secondary(), selected.text);
            }
        }
    } else if (input.button == PointerButton::Middle) {
        paste(true);
    }
    return true;
}

bool VtermInput::pointerMotion(const PointerMotionInput& input) {
    updatePointer(input.pixelX, input.pixelY, input.modifiers);
    u16 locatorColumn = 1;
    u16 locatorRow = 1;
    mouseProtocolCoordinates(MouseTrackingEnc::Default, input.pixelX, input.pixelY, locatorColumn, locatorRow);
    terminal->setLocatorPosition(locatorColumn, locatorRow, max(1, input.pixelX + 1), max(1, input.pixelY + 1), 0);
    const MouseTrackingState tracking = terminal->mouseTrk;
    if (mouse.protocolActive(input.modifiers, tracking.mode)) {
        stopSelectionAutoscroll();
        if (tracking.mode == MouseTrackingMode::VT200_ButtonEvent && !mouse.primaryButtonPressed()) {
            return true;
        }
        if (tracking.mode != MouseTrackingMode::VT200_ButtonEvent && tracking.mode != MouseTrackingMode::VT200_AnyEvent) {
            return true;
        }
        u16 column = 0;
        u16 row = 0;
        mouseProtocolCoordinates(tracking.enc, input.pixelX, input.pixelY, column, row);
        if (mouse.reportMotion(column, row, tracking.mode, tracking.enc, tracking.generation)) {
            sendMouseProtocol(tracking.enc, MouseEventType::Motion, input.modifiers, 0, column, row);
        }
    } else if (mouse.buttons() & ((1u << (unsigned)(PointerButton::Primary)) | (1u << (unsigned)(PointerButton::Secondary)))) {
        terminal->selectionUpdate(input.pixelX, input.pixelY);
        updateSelectionAutoscroll();
    } else {
        stopSelectionAutoscroll();
    }
    return true;
}

bool VtermInput::scroll(const ScrollInput& input) {
    updatePointer(input.pixelX, input.pixelY, input.modifiers);
    const MouseTrackingState tracking = terminal->mouseTrk;
    const bool reporting = mouse.protocolActive(input.modifiers, tracking.mode);
    const bool alternate = terminal->altScrollMode && terminal->altScreenBufferMode;
    const MouseWheelSteps steps = mouse.consumeWheel(input.x, input.y, reporting || alternate);
    if (reporting) {
        for (int k = 0; k < steps.y; ++k) {
            sendMouseButtonProtocol(MouseEventType::Press, 4, input.pixelX, input.pixelY, input.modifiers, tracking);
        }
        for (int k = 0; k < -steps.y; ++k) {
            sendMouseButtonProtocol(MouseEventType::Press, 5, input.pixelX, input.pixelY, input.modifiers, tracking);
        }
        for (int k = 0; k < -steps.x; ++k) {
            sendMouseButtonProtocol(MouseEventType::Press, 6, input.pixelX, input.pixelY, input.modifiers, tracking);
        }
        for (int k = 0; k < steps.x; ++k) {
            sendMouseButtonProtocol(MouseEventType::Press, 7, input.pixelX, input.pixelY, input.modifiers, tracking);
        }
    } else {
        if (steps.y > 0) {
            terminal->mouseWheelUp(steps.y);
        } else if (steps.y < 0) {
            terminal->mouseWheelDown(-steps.y);
        }
        if (steps.x > 0) {
            terminal->mouseWheelRight(steps.x);
        } else if (steps.x < 0) {
            terminal->mouseWheelLeft(-steps.x);
        }
    }
    return true;
}

void VtermInput::focus(bool focused) {
    pointerFocused = focused;
    if (!focused) {
        pointerModifiers = 0;
        hyperlinkClick = false;
        mouse.clearButtons();
        mouse.endSelection();
        stopSelectionAutoscroll();
        suppressedTextInputs = 0;
        suppressRepeatedTextInput = false;
        pendingTextKey.active = false;
        rectangularSelectionKeyConsumed = false;
    }
    refreshHyperlinkAndRedraw();
    terminal->setHasFocus(focused);
}

void VtermInput::pointerPresence(bool present) {
    mouse.resetMotion();
    pointerPresent = present;
    if (!present) {
        pointerPositionKnown = false;
        stopSelectionAutoscroll();
    }
    refreshHyperlinkAndRedraw();
}

VtermImpl::~VtermImpl() {
    delete framePriPool;
    delete frameAltPool;
}

void VtermImpl::createPrimaryScreen() {
    ObjPool* const next = ObjPool::fromMemoryRaw();
    Screen* screen;
    try {
        screen = Screen::createPrimary(extras_, *next, pane_.columns, pane_.rows, &colors, config().saveLines);
    } catch (...) {
        delete next;
        throw;
    }
    delete framePriPool;
    framePriPool = next;
    frame_pri = screen;
}

void VtermImpl::createAlternateScreen() {
    ObjPool* const next = ObjPool::fromMemoryRaw();
    Screen* screen;
    try {
        screen = Screen::createAlternate(extras_, *next, pane_.columns, pane_.rows, &colors);
    } catch (...) {
        delete next;
        throw;
    }
    delete frameAltPool;
    frameAltPool = next;
    frame_alt = screen;
}

void VtermImpl::createInactiveAlternateScreen() {
    ObjPool* const next = ObjPool::fromMemoryRaw();
    Screen* screen;
    try {
        screen = Screen::createInactiveAlternate(extras_, *next);
    } catch (...) {
        delete next;
        throw;
    }
    delete frameAltPool;
    frameAltPool = next;
    frame_alt = screen;
}

void VtermImpl::resizeScreen(Screen*& frame, ObjPool*& pool, Screen::Cursor& cursor, SavedCursor* trackedCursor) {
    Screen::Cursor trackedState;
    Screen::Cursor* trackedStatePtr = nullptr;
    if (trackedCursor != nullptr && trackedCursor->isSet) {
        trackedState = {Point(trackedCursor->posX, trackedCursor->posY), trackedCursor->lastCol};
        trackedStatePtr = &trackedState;
    }
    ObjPool* const next = ObjPool::fromMemoryRaw();
    Screen* screen;
    try {
        screen = frame->resizedWithHistory(*next, pane_.columns, pane_.rows, config().saveLines, cursor, trackedStatePtr);
    } catch (...) {
        delete next;
        throw;
    }
    delete pool;
    pool = next;
    frame = screen;
    if (trackedStatePtr != nullptr) {
        trackedCursor->posX = trackedState.position.x;
        trackedCursor->posY = trackedState.position.y;
        trackedCursor->lastCol = trackedState.pendingWrap;
    }
}

void VtermImpl::feedPty(StringView bytes) {
    if (bytes.empty()) {
        return;
    }
    if (dump != nullptr) {
        dump->write(bytes.data(), bytes.length());
    }
    // processInput reports whether the presentation revision moved; only a
    // real change schedules a frame. The transport layer stays out of it.
    if (processInput(bytes.data(), (int)(bytes.length()))) {
        host.requestFrame();
    }
}

void VtermImpl::feedPty(const StringView* slices, size_t count) {
    if (count == 0) {
        return;
    }
    if (dump != nullptr) {
        for (size_t index = 0; index < count; ++index) {
            dump->write(slices[index].data(), slices[index].length());
        }
    }
    if (processInput(slices, count)) {
        host.requestFrame();
    }
}

void VtermImpl::expose() {
    redraw();
}

void VtermImpl::exposeAll() {
    // R3a-1. A synchronized update (\e[?2026h) makes redraw() a no-op,
    // so a pane that opened one and then fell silent would take the
    // damage and still hand over its retained form - the same zero rows
    // the caller asked to be rid of, refused again, for as long as the
    // mode holds. resizeGrid() drops the mode on both of its branches
    // for exactly this reason; this is the same movement for the same
    // reason. A torn frame in one pane is cheaper than a window that
    // has stopped presenting, and the pane is handing over everything
    // it has anyway.
    if (synchronizedOutputMode) {
        setSynchronizedOutput(false);
    }
    // The damage first, the pending flag second, and both of them: a
    // pane whose rows are damaged but whose output is not pending is
    // asked for its retained form instead, and a retained form carries
    // no damage at all (retainedOutput() zeroes it). Either half alone
    // leaves the caller with the same zero rows it asked to be rid of.
    exposeFrames();
    redraw();
}

void VtermImpl::copy() {
    input.copy();
}

void VtermImpl::paste(bool primary) {
    input.paste(primary);
}

void VtermImpl::promptTrace(const char* action) const {
    if (!config().verbose) {
        return;
    }
    i64 row = 0;
    u16 expected = 0;
    const PromptOrigin probe{0, promptColumn, pane_.columns};
    promptCursorCell(probe, promptLine.text.data(), promptLine.text.length(), config().widths, promptLine.cursor, row, expected);
    fprintf(stderr, "%.*s: prompt editor: %s declined: option=%d reported=%d semantic=%u alternate=%d inputColumn=%u cursor=%u,%u expectedColumn=%u lineRow=%lld length=%zu lineCursor=%zu columns=%u\n",
        (int)(config().brandName.length()), (const char*)(config().brandName.data()), action,
        (int)(config().promptEditor), (int)(promptReported), (unsigned)(currentSemantic), (int)(altScreenBufferMode),
        (unsigned)(promptColumn), (unsigned)(posX), (unsigned)(posY), (unsigned)(expected), (long long)(row),
        (size_t)(promptLine.text.length()), (size_t)(promptLine.cursor), (unsigned)(pane_.columns));
}

bool VtermImpl::promptAtInput() const {
    // After the prompt's start as well as after the input's: a theme that
    // marks its prompt again while redrawing it (reset-prompt) sends a
    // second 133;A mid-line, and the input's column stays the one its 133;B
    // gave - the cursor check in promptOriginFromCursor() says whether that
    // still fits the line.
    return currentSemantic == 1 || currentSemantic == 2;
}

bool VtermImpl::promptOrigin(PromptOrigin& origin) const {
    if (!config().promptEditor || !promptReported || !promptAtInput() || altScreenBufferMode) {
        return false;
    }
    return promptOriginFromCursor(promptColumn, pane_.columns, promptLine, config().widths, (i64)(posY), posX, origin);
}

bool VtermImpl::promptPlaceCursor(int pixelX, int pixelY) {
    PromptOrigin origin;
    if (!promptOrigin(origin)) {
        promptTrace("click");
        return false;
    }
    const Point cell = selectionPoint(pixelX, pixelY);
    size_t index = 0;
    if (!promptIndexAt(origin, promptLine.text.data(), promptLine.text.length(), config().widths, cell.y, (u16)(cell.x < 0 ? 0 : cell.x), index)) {
        if (config().verbose) {
            fprintf(stderr, "%.*s: prompt editor: click at %d,%d outside the line starting %lld,%u\n", (int)(config().brandName.length()), (const char*)(config().brandName.data()), cell.x, cell.y, (long long)(origin.row), (unsigned)(origin.column));
        }
        return false;
    }
    if (index != promptLine.cursor) {
        StringBuilder sequence;
        promptSetSequence(promptLine.text.data(), promptLine.text.length(), index, sequence);
        sendBytes(StringView(sequence), true);
    }
    return true;
}

bool VtermImpl::promptReplaceSelection(const u32* insert, size_t count) {
    PromptOrigin origin;
    if (!promptOrigin(origin)) {
        return false;
    }
    const Rect selected = cf->logicalSelection();
    if (selected.empty() || selected.rectangular) {
        return false;
    }
    // The selection's end is the first cell it leaves out, so both ends are
    // positions in the line the same way.
    size_t from = 0;
    size_t to = 0;
    const u32* const text = promptLine.text.data();
    const size_t length = promptLine.text.length();
    if (!promptIndexAt(origin, text, length, config().widths, selected.tl.y, (u16)(selected.tl.x), from) || !promptIndexAt(origin, text, length, config().widths, selected.br.y, (u16)(selected.br.x), to) || to <= from) {
        return false;
    }
    StringBuilder sequence;
    promptReplace(promptLine, from, to, insert, count, sequence);
    selectClear();
    sendBytes(StringView(sequence), true);
    return true;
}

bool VtermImpl::commandLineEditable() const {
    PromptOrigin origin;
    if (!promptOrigin(origin)) {
        promptTrace("key");
        return false;
    }
    return true;
}

bool VtermImpl::selectCommandLine() {
    PromptOrigin origin;
    if (!promptOrigin(origin) || promptLine.text.empty()) {
        return false;
    }
    i64 row = 0;
    u16 column = 0;
    promptCursorCell(origin, promptLine.text.data(), promptLine.text.length(), config().widths, promptLine.text.length(), row, column);
    const Point first((int)(origin.column), (int)(origin.row));
    const Point past((int)(column), (int)(row));
    cf->beginSelection(first);
    cf->updateSelection(Rect(first, past));
    changePresentation();
    redraw();
    return true;
}

bool VtermImpl::commandLineUndo(bool redo) {
    PromptOrigin origin;
    if (!promptOrigin(origin)) {
        promptTrace(redo ? "redo" : "undo");
        return false;
    }
    sendBytes(redo ? promptRedoSequence() : promptUndoSequence(), true);
    if (config().verbose) {
        fprintf(stderr, "%.*s: prompt editor: %s sent\n", (int)(config().brandName.length()), (const char*)(config().brandName.data()), redo ? "redo" : "undo");
    }
    return true;
}

void VtermImpl::clear() {
    const u8 formFeed = 0x0c;
    sendBytes(StringView(&formFeed, 1), true);
}

void VtermImpl::focus(bool focused) {
    input.focus(focused);
}

bool VtermImpl::key(const KeyInput& value) {
    return input.key(value);
}

bool VtermImpl::text(const TextInput& value) {
    return input.text(value);
}

bool VtermImpl::pointerMotion(const PointerMotionInput& value) {
    return input.pointerMotion(value);
}

bool VtermImpl::pointerButton(const PointerButtonInput& value) {
    return input.pointerButton(value);
}

bool VtermImpl::scroll(const ScrollInput& value) {
    return input.scroll(value);
}

void VtermImpl::pointerPresence(bool present) {
    input.pointerPresence(present);
}

void VtermImpl::flush() {
    input.flush();
    host.requestFrame();
}

void VtermImpl::key(InputKey key_, VtModifier modifiers_) {
    sendKey(key_, modifiers_);
}

void VtermImpl::character(u8 byte, VtModifier modifiers_) {
    sendCharacter(byte, modifiers_);
}

void VtermImpl::sendBytes(StringView bytes, bool userInput) {
    if (userInput) {
        sendUserInput(bytes);
    } else {
        writePty(bytes);
    }
}

void VtermImpl::kittyKey(InputKey key_, u16 modifiers_, VtermKeyEventType event) {
    writeKittyKey(key_, modifiers_, event);
}

void VtermImpl::kittyKey(u32 key_, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers_, VtermKeyEventType event) {
    writeKittyKey(key_, shiftedKey, baseLayoutKey, modifiers_, event);
}

void VtermImpl::locatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons) {
    setLocatorPosition(column, row, pixelX, pixelY, buttons);
}

void VtermImpl::locatorButton(u8 button, bool pressed) {
    reportLocatorButton(button, pressed);
}

void VtermImpl::scrollUp(u16 count) {
    mouseWheelUp(count);
}

void VtermImpl::scrollDown(u16 count) {
    mouseWheelDown(count);
}

void VtermImpl::selectionStart(int pixelX, int pixelY, bool cycleSnapTo) {
    selectStart(pixelX, pixelY, cycleSnapTo);
}

void VtermImpl::selectionExtend(int pixelX, int pixelY, bool cycleSnapTo) {
    selectExtend(pixelX, pixelY, cycleSnapTo);
}

void VtermImpl::selectionUpdate(int pixelX, int pixelY) {
    selectUpdate(pixelX, pixelY);
}

VtermTextResult VtermImpl::selectionFinish() {
    inputResult.reset();
    const bool selected = selectFinish(inputResult);
    return {stringView(inputResult), selected};
}

bool VtermImpl::hasSelection() const {
    return cf->hasSelection();
}

void VtermImpl::selectionClear() {
    selectClear();
}

void VtermImpl::selectionRectangular() {
    selectRectangularModeToggle();
}

void VtermImpl::paste(StringView text) {
    pasteSelection(text);
}

ScreenHyperlink VtermImpl::resolveHyperlink(int pixelX, int pixelY) const {
    u16 column = 0;
    u16 row = 0;
    if (!mouseCell(pixelX, pixelY, mouseGeometry(pane_, windowGeometry_), column, row)) {
        return {};
    }
    const ScreenInfo info = cf->info();
    if (column >= info.columns || row >= info.rows) {
        return {};
    }
    const ScreenHyperlink link = cf->hyperlinkAt(row, column);
    // An explicit OSC 8 hyperlink is authoritative; a detected plain URI
    // is only actionable when its scheme is on the configured list.
    if (link.displayId == 0 && !link.payload.empty() && !host.uriSchemeAllowed(link.scheme)) {
        return {};
    }
    return link;
}

StringView VtermImpl::hyperlinkAt(int pixelX, int pixelY) {
    const StringView payload = resolveHyperlink(pixelX, pixelY).payload;
    inputResult.reset();
    inputResult.append(payload.data(), payload.length());
    return stringView(inputResult);
}

TerminalCursor VtermImpl::presentationCursor(u32 viewOffset) const {
    TerminalCursor result;
    result.posX = posX;
    result.posY = posY + viewOffset;
    result.style = cursorTemporarilyHidden || !showCursorMode ? TerminalCursor::Style::hidden : hasFocus ? cursorShape : TerminalCursor::Style::hollow_block;
    result.color = cursorColor;
    return result;
}

void VtermImpl::fillTerminalUpdate(TerminalUpdate& update, const ScreenFrame& frame, const TerminalRow* rows) {
    update = {};
    update.rows = rows;
    update.rowCount = frame.damagedRows;
    // A9: this pane's grid - the one the rows above were built by.
    update.gridColumns = pane_.columns;
    update.gridRows = pane_.rows;
    update.colors = &colors;
    update.viewOffset = frame.viewOffset;
    update.historyRows = frame.historyRows;
    update.cursor = presentationCursor(frame.viewOffset);
    update.selection = frame.selection;
    update.snappedSelection = frame.snappedSelection;
    update.selectionForeground = selectionFgColor;
    update.selectionBackground = selectionBgColor;
    update.selectionColorMask = selectionColorMask;
    update.hoveredHyperlink = input.hoveredHyperlink;
    update.hoveredLinkBegin = input.hoveredLinkBegin;
    update.hoveredLinkEnd = input.hoveredLinkEnd;
    update.screenReverse = screenReverseVideo;
    update.blinkVisible = blinkVisible;
    update.cursorBlink = cursorBlinkMode;
}

void VtermImpl::dropBuffered(StringView text) {
    if (text.empty()) {
        return;
    }
    const bool bracketed = bracketedPasteMode;
    spawnTransaction([this, bracketed, view = text] {
        // Copied before the first suspension point, like spawnPtyWrite.
        u8 local[1024];
        Buffer owned;
        const u8* data;
        if (view.length() <= sizeof(local)) {
            memcpy(local, view.data(), view.length());
            data = local;
        } else {
            owned.append(view.data(), view.length());
            data = (const u8*)(owned.data());
        }
        const plt::LockGuard guard(*ptyMutex_);
        PasteOutput paste(ptyOutput_, bracketed);
        paste.write(data, view.length());
    });
}

bool VtermImpl::bufferDropPayload(Input& source, Buffer& content) {
    // The buffered fallback must not balloon on a hostile drag source; the
    // fiber path streams and needs no cap.
    constexpr size_t payloadLimit = 16u << 20;
    for (;;) {
        u8 chunk[16 * 1024];
        const size_t count = source.read(chunk, sizeof(chunk));
        if (count == 0) {
            return !content.empty();
        }
        if (count > payloadLimit - content.length()) {
            return false;
        }
        content.append(chunk, count);
    }
}

void VtermImpl::dropText(Input& source) {
    plt::Scheduler* const scheduler = &scheduler_;
    if (scheduler->current() == nullptr) {
        Buffer content;
        if (bufferDropPayload(source, content)) {
            dropBuffered(StringView(content));
        }
        return;
    }
    // The stream is pulled on this fiber under the mutex: backpressure
    // propagates to the drag source instead of ballooning a buffer.
    const plt::LockGuard guard(*ptyMutex_);
    PasteOutput paste(ptyOutput_, bracketedPasteMode);
    for (;;) {
        u8 chunk[4096];
        const size_t count = source.read(chunk, sizeof(chunk));
        if (count == 0) {
            break;
        }
        paste.write(chunk, count);
    }
}

// Local files insert as percent-decoded shell-quoted paths with a
// trailing separator, other schemes as the quoted verbatim URI.
void VtermImpl::buildQuotedEntry(StringView entry, StringBuilder& quoted) {
    Buffer decoded;
    StringView value = entry;
    if (plt::fileUriToPath(entry, decoded)) {
        value = StringView(decoded);
    }
    bool plain = !value.empty();
    for (size_t index = 0; index < value.length(); ++index) {
        const u8 byte = value.data()[index];
        const bool alnum = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
        if (!alnum && byte != '/' && byte != '.' && byte != '_' && byte != '-' && byte != '+' && byte != ':' && byte != '@' && byte != '%' && byte != ',' && byte != '=') {
            plain = false;
            break;
        }
    }
    if (plain) {
        quoted << value;
    } else {
        quoted << StringView(u8"'");
        for (size_t index = 0; index < value.length(); ++index) {
            const u8 byte = value.data()[index];
            if (byte == '\'') {
                quoted << StringView(u8"'\\''");
            } else {
                quoted.append(&byte, 1);
            }
        }
        quoted << StringView(u8"'");
    }
    quoted << StringView(u8" ");
}

void VtermImpl::dropUriList(Input& source) {
    plt::Scheduler* const scheduler = &scheduler_;
    if (scheduler->current() == nullptr) {
        Buffer content;
        if (!bufferDropPayload(source, content)) {
            return;
        }
        StringView payload(content);
        StringView entry;
        while (plt::nextUriListEntry(payload, entry)) {
            StringBuilder quoted(entry.length() + 4);
            buildQuotedEntry(entry, quoted);
            dropBuffered(StringView(quoted));
        }
        return;
    }
    // One line at most this long is metadata, not a payload; a source that
    // never ends a line is abandoned mid-stream.
    constexpr size_t entryLimit = 64 * 1024;
    const plt::LockGuard guard(*ptyMutex_);
    Buffer pending;
    for (bool complete = false; !complete;) {
        u8 chunk[4096];
        const size_t count = source.read(chunk, sizeof(chunk));
        complete = count == 0;
        pending.append(chunk, count);
        if (pending.length() > entryLimit) {
            return;
        }
        // Only complete lines are parseable mid-stream; the last one joins
        // at end of stream.
        size_t boundary = pending.length();
        if (!complete) {
            const u8* const data = (const u8*)(pending.data());
            while (boundary > 0 && data[boundary - 1] != '\n') {
                --boundary;
            }
        }
        StringView ready((const u8*)(pending.data()), boundary);
        StringView entry;
        while (plt::nextUriListEntry(ready, entry)) {
            StringBuilder quoted(entry.length() + 4);
            buildQuotedEntry(entry, quoted);
            PasteOutput paste(ptyOutput_, bracketedPasteMode);
            paste.write(quoted.data(), quoted.used());
        }
        Buffer tail(StringView((const u8*)(pending.data()) + boundary, pending.length() - boundary));
        stl::xchg(pending, tail);
    }
}

// A control or a joiner opening a cluster has no cell of its own and
// nothing to extend, so printed text drops it - and so does the preview.
// C1 codepoints are the exception, as they are in the grid: decoded out
// of UTF-8 text they are placed like ordinary characters.
static bool droppedAtClusterStart(u32 codepoint) {
    if (codepoint >= 0x80 && codepoint <= 0x9f) {
        return false;
    }
    const GraphemeClass graphemeClass = unicodeCodepointProperties(codepoint).graphemeClass;
    return graphemeClass == GraphemeClass::Control || graphemeClass == GraphemeClass::Zwj;
}

void VtermImpl::preedit(StringView text, i32 cursorBegin, i32 cursorEnd) {
    preeditCells.clear();
    preeditCursorBeginCell = -1;
    preeditCursorEndCell = -1;

    // The preview clusters like printed text: a combining mark, a joiner
    // or a variation selector extends the cell before it instead of
    // losing its codepoint, and a cluster that changes width takes its
    // second cell with it. What the input method shows is then what the
    // grid will hold once the composition commits.
    CellExtraStore& extras = *extras_.store;
    GraphemeBreaker breaker;
    GraphemeBuffer cluster;
    // The lead cell of the cluster still open; -1 when there is none.
    i32 leadCell = -1;

    const u8* bytes = text.data();
    size_t remaining = text.length();
    i32 offset = 0;
    while (remaining > 0) {
        u32 codepoint = 0;
        const size_t consumed = Utf8Decoder::decodeOne(bytes, remaining, codepoint);
        if (consumed == 0) {
            break;
        }
        if (cursorBegin >= 0 && preeditCursorBeginCell < 0 && offset >= cursorBegin) {
            preeditCursorBeginCell = (i32)(preeditCells.length());
        }
        const u8 data = codepointData(codepoint);
        const u8 width = data & 0x03;
        const bool boundary = breaker.breakBefore(codepoint, (data & 0x04) != 0);
        if (!boundary && leadCell >= 0) {
            const u32 previous = cluster.data()[cluster.size() - 1];
            const GraphemeWidthEffect widthEffect = graphemeClusterMode ? config().widths.graphemeWidthEffect(previous, codepoint) : GraphemeWidthEffect::Unchanged;
            // Past the cluster limit the preview keeps what it has, as
            // the grid does.
            if (cluster.pushBack(codepoint)) {
                if (widthEffect == GraphemeWidthEffect::Wide && !preeditCells[leadCell].dwidth) {
                    preeditCells.mut(leadCell).dwidth = 1;
                    TerminalCell continuation{};
                    continuation.dwidth_cont = 1;
                    continuation.drawn = 1;
                    preeditCells.pushBack(continuation);
                } else if (widthEffect == GraphemeWidthEffect::Narrow && preeditCells[leadCell].dwidth) {
                    preeditCells.mut(leadCell).dwidth = 0;
                    preeditCells.popBack();
                }
                extras.setGrapheme(preeditCells.mut(leadCell), cluster.data(), cluster.size());
            }
        } else if (width == 0 && droppedAtClusterStart(codepoint)) {
            breaker.reset();
            leadCell = -1;
        } else {
            leadCell = (i32)(preeditCells.length());
            cluster.clear();
            cluster.pushBack(codepoint);
            TerminalCell cell{};
            cell.uc_pt = codepoint;
            cell.drawn = 1;
            if (width == 2) {
                cell.dwidth = 1;
                preeditCells.pushBack(cell);
                TerminalCell continuation{};
                continuation.dwidth_cont = 1;
                continuation.drawn = 1;
                preeditCells.pushBack(continuation);
            } else {
                preeditCells.pushBack(cell);
            }
        }
        if (offset < cursorEnd) {
            preeditCursorEndCell = (i32)(preeditCells.length());
        }
        bytes += consumed;
        remaining -= consumed;
        offset += (i32)(consumed);
    }
    if (cursorBegin >= 0 && preeditCursorBeginCell < 0) {
        preeditCursorBeginCell = (i32)(preeditCells.length());
    }
    if (preeditCursorEndCell >= 0 && preeditCursorEndCell > (i32)(preeditCells.length())) {
        preeditCursorEndCell = (i32)(preeditCells.length());
    }
    // Underline the preview; show the input method's cursor range in
    // reverse video (a collapsed range marks the cell after it).
    for (size_t index = 0; index != preeditCells.length(); ++index) {
        TerminalCell& cell = preeditCells.mut(index);
        const bool inCursor = preeditCursorBeginCell >= 0 && preeditCursorEndCell > preeditCursorBeginCell && (i32)(index) >= preeditCursorBeginCell && (i32)(index) < preeditCursorEndCell;
        if (inCursor) {
            cell.inverse = 1;
        } else {
            cell.underline_style = 1;
        }
    }

    cf->expose();
    changePresentation();
    redraw();
}

const TerminalUpdate* VtermImpl::output() {
    if (!outputPending) {
        return nullptr;
    }

    Screen* const frame = cf;
    const ScreenFrame output = frame->captureFrame(outputRows.mutData());

    updateScreen = frame;
    updatingRevision = presentationRevision();
    fillTerminalUpdate(terminalUpdate, output, outputRows.data());
    terminalUpdate.shapes = frame;
    overlayPreedit(terminalUpdate);
    return &terminalUpdate;
}

const TerminalUpdate& VtermImpl::retainedOutput() {
    Screen* const frame = cf;
    // Captured rather than assembled field by field: the view offset,
    // the history depth and both selection rectangles are what the
    // renderer draws this pane's retained cells *through*, and this is
    // the one place that knows how to read them. The damage travels
    // with it and is then dropped - the screen still holds it, so the
    // output() that follows reports it in full.
    ScreenFrame quiet = frame->captureFrame(outputRows.mutData());
    quiet.damagedRows = 0;
    fillTerminalUpdate(retainedUpdate, quiet, outputRows.data());
    retainedUpdate.shapes = frame;
    overlayPreedit(retainedUpdate);
    return retainedUpdate;
}

void VtermImpl::overlayPreedit(TerminalUpdate& update) {
    if (preeditCells.empty()) {
        return;
    }
    const i32 row = (i32)(posY) + (i32)(update.viewOffset);
    if (row < 0 || row >= (i32)(pane_.rows) || cf->lineAttribute(posY) != 0) {
        return;
    }
    const i32 columns = pane_.columns;
    i32 count = (i32)(preeditCells.length());
    // foot's clipping policy: shift left when the preview does not fit
    // to the end of the line; when it exceeds the whole row keep the
    // tail (fresh input) visible.
    i32 sliceBegin = 0;
    if (count > columns) {
        sliceBegin = count - columns;
        count = columns;
    }
    i32 startColumn = posX;
    if (startColumn + count > columns) {
        startColumn = columns - count;
    }
    if (sliceBegin > 0 && preeditCells[sliceBegin].dwidth_cont) {
        ++sliceBegin;
        --count;
    }
    if (count <= 0) {
        return;
    }

    preeditWindow.clear();
    preeditWindow.append(preeditCells.data() + sliceBegin, (size_t)(count));

    update.overlayCells = preeditWindow.data();
    update.overlayRow = (u16)(row);
    update.overlayColumn = (u16)(startColumn);
    update.overlayCount = (u16)(count);

    // The regular cursor hides while composing; the anchor for the input
    // method's candidate window tracks the preview cursor cell.
    update.cursor.style = TerminalCursor::Style::hidden;
    if (preeditCursorBeginCell >= 0) {
        const i32 cursorCell = preeditCursorBeginCell - sliceBegin;
        const i32 column = startColumn + (cursorCell < 0 ? 0 : cursorCell > count ? count : cursorCell);
        update.cursor.posX = (u16)(column >= columns ? columns - 1 : column);
        update.cursor.posY = (u16)(row);
    }
}

void VtermImpl::consume() {
    STD_ASSERT(updateScreen != nullptr);
    updateScreen->resetDamage();
    presentedRevision = updatingRevision;
    updatingRevision = 0;
    updateScreen = nullptr;
    outputPending = false;
    presentedSinceGcSafePoint = true;
    collectCellExtrasIfNeeded();
}

VtermState VtermImpl::state() const {
    VtermState result;
    result.mouseTracking = mouseTrk.mode;
    result.mouseEncoding = mouseTrk.enc;
    result.synchronizedOutput = synchronizedOutputMode;
    result.alternateScreen = altScreenBufferMode;
    result.bracketedPaste = bracketedPasteMode;
    result.applicationCursorKeys = cursorKeyMode == CursorKeyMode::Application;
    result.applicationKeypad = keypadMode == KeypadMode::Application;
    result.focusEvents = mouseTrk.focusEventMode;
    result.autoWrap = autoWrapMode;
    result.originMode = originMode == OriginMode::ScrollingRegion;
    result.insertMode = insertMode;
    result.showCursor = showCursorMode;
    result.screenReverse = screenReverseVideo;
    result.alternateScroll = altScrollMode;
    return result;
}

TestApi* VtermImpl::createTestApi() {
#ifdef SHITTY_FOR_TESTS
    return owner_.make<TestApiImpl>(this);
#else
    return nullptr;
#endif
}

TestApiImpl::TestApiImpl(VtermImpl* vterm_)
    : vterm(vterm_)
{
}

VtermTestState TestApiImpl::inspect() const {
    VtermTestState result;
    result.mouse = vterm->mouseTrk;
    result.kittyKeyboardFlags = vterm->getKittyKeyboardFlags();
    result.screenReverseVideo = vterm->screenReverseVideo;
    result.ledState = vterm->ledState;
    result.reverseWrapMode = vterm->reverseWrapMode;
    result.nationalReplacementMode = vterm->nationalReplacementMode;
    result.pendingWrap = vterm->lastCol;
    result.cursorStyle = vterm->cursorShape;
    result.pen.cell = vterm->attrs;
    result.pen.fg = vterm->colors.resolve(vterm->attrs.foreground());
    result.pen.bg = vterm->colors.resolve(vterm->attrs.background());
    if (vterm->originMode == VtermImpl::OriginMode::ScrollingRegion) {
        result.rectangleOrigin = {vterm->marginTop, vterm->hMargin, vterm->marginBottom, vterm->nColsEff};
    } else {
        result.rectangleOrigin = {0, 0, vterm->pane_.rows, vterm->pane_.columns};
    }
    result.hyperlinkCount = vterm->extras_.store->hyperlinkCount();
    for (size_t index = 0; index < 4; ++index) {
        result.charsets[index] = (u8)(vterm->charsetState.g[index]);
    }
    return result;
}

bool TestApiImpl::ansiMode(u32 mode) const {
    switch (mode) {
        case 4:
            return vterm->insertMode;
        case 6:
            return vterm->eraseModeAll;
        case 12:
            return !vterm->localEcho;
        case 20:
            return vterm->autoNewlineMode;
        default:
            return false;
    }
}

bool TestApiImpl::privateMode(u32 mode) const {
    using CursorKeyMode = typename VtermImpl::CursorKeyMode;
    using ColMode = typename VtermImpl::ColMode;
    using KeypadMode = typename VtermImpl::KeypadMode;
    using OriginMode = typename VtermImpl::OriginMode;
    switch (mode) {
        case 1:
            return vterm->cursorKeyMode == CursorKeyMode::Application;
        case 3:
            return vterm->colMode == ColMode::C132;
        case 4:
            return vterm->smoothScrollMode;
        case 5:
            return vterm->screenReverseVideo;
        case 6:
            return vterm->originMode == OriginMode::ScrollingRegion;
        case 7:
            return vterm->autoWrapMode;
        case 8:
            return vterm->autoRepeatMode;
        case 12:
            return vterm->cursorBlinkMode;
        case 40:
            return vterm->allowColumnMode;
        case 41:
            return vterm->moreFixMode;
        case 42:
            return vterm->nationalReplacementMode;
        case 45:
            return vterm->reverseWrapMode;
        case 9:
            return vterm->mouseTrk.mode == MouseTrackingMode::X10_Compat;
        case 25:
            return vterm->showCursorMode;
        case 47:
        case 1047:
            return vterm->altScreenBufferMode;
        case 66:
            return vterm->keypadMode == KeypadMode::Application;
        case 67:
            return !vterm->bkspSendsDel;
        case 69:
            return vterm->horizMarginMode;
        case 95:
            return vterm->noClearColumnMode;
        case 1000:
            return vterm->mouseTrk.mode == MouseTrackingMode::VT200;
        case 1001:
            return vterm->mouseTrk.mode == MouseTrackingMode::VT200_Highlight;
        case 1002:
            return vterm->mouseTrk.mode == MouseTrackingMode::VT200_ButtonEvent;
        case 1003:
            return vterm->mouseTrk.mode == MouseTrackingMode::VT200_AnyEvent;
        case 1004:
            return vterm->mouseTrk.focusEventMode;
        case 1005:
            return vterm->mouseTrk.enc == MouseTrackingEnc::UTF8;
        case 1006:
            return vterm->mouseTrk.enc == MouseTrackingEnc::SGR;
        case 1007:
            return vterm->altScrollMode;
        case 1015:
            return vterm->mouseTrk.enc == MouseTrackingEnc::URXVT;
        case 1016:
            return vterm->mouseTrk.enc == MouseTrackingEnc::SGRPixels;
        case 1034:
            return vterm->eightBitInput;
        case 1036:
        case 1039:
            return vterm->altSendsEscape;
        case 1045:
            return vterm->extendedReverseWrapMode;
        case 2004:
            return vterm->bracketedPasteMode;
        case 2026:
            return vterm->synchronizedOutputMode;
        case 2027:
            return vterm->graphemeClusterMode;
        case 2031:
            return vterm->colorSchemeUpdateMode;
        case 2048:
            return vterm->inBandResizeMode;
        case 5522:
            return vterm->pasteMimeNotificationsMode;
        default:
            return false;
    }
}

void TestApiImpl::hardReset() {
    vterm->resetTerminal();
}

bool TestApiImpl::tabStop(u16 column) const {
    if (column >= vterm->pane_.columns) {
        return false;
    }
    if (!vterm->tabStopsCustomized) {
        return column % 8 == 0;
    }
    return tabContains(vterm->tabStops, (u16)(column));
}

void TestApiImpl::setWrapped(u16 row) {
    const ScreenInfo info = vterm->cf->info();
    if (row < info.rows && info.columns != 0) {
        vterm->cf->setWrapped(row, info.columns - 1);
    }
}

VtermTestCell TestApiImpl::cell(u16 row, u16 column) const {
    const ScreenInfo info = vterm->cf->info();
    if (row >= info.rows || column >= info.columns) {
        return {};
    }
    VtermTestCell result;
    result.cell = vterm->cf->testCell(row, column);
    CellExtraStore& extras = *vterm->extras_.store;
    const GraphemeView grapheme = extras.grapheme(result.cell.extraRef());
    result.grapheme = grapheme.data();
    result.graphemeSize = grapheme.size();
    result.underlineColor = extras.underlineColor(result.cell);
    result.lineAttribute = vterm->cf->lineAttribute(row);
    return result;
}

VtermTestCell TestApiImpl::logicalCell(i32 row, u16 column) const {
    const ScreenInfo info = vterm->cf->info();
    if (row < -(i32)(info.historyRows) || row >= info.rows || column >= info.columns) {
        return {};
    }
    VtermTestCell result;
    result.cell = vterm->cf->testLogicalCell(row, column);
    CellExtraStore& extras = *vterm->extras_.store;
    const GraphemeView grapheme = extras.grapheme(result.cell.extraRef());
    result.grapheme = grapheme.data();
    result.graphemeSize = grapheme.size();
    result.underlineColor = extras.underlineColor(result.cell);
    return result;
}

u8 TestApiImpl::rowSemantic(i32 row) const {
    const ScreenInfo info = vterm->cf->info();
    if (row < -(i32)(info.historyRows) || row >= info.rows) {
        return 0;
    }
    return (u8)(vterm->cf->semanticPrompt(row));
}

u8 TestApiImpl::semanticClick() const {
    return (u8)(vterm->semanticClick);
}

bool TestApiImpl::cursorIsAtPrompt() const {
    return vterm->cursorIsAtPrompt();
}

void TestApiImpl::key(InputKey key_, VtModifier modifiers) {
    vterm->key(key_, modifiers);
}

void TestApiImpl::character(u8 byte, VtModifier modifiers) {
    vterm->character(byte, modifiers);
}

void TestApiImpl::kittyKey(InputKey key_, u16 modifiers, VtermKeyEventType event) {
    vterm->kittyKey(key_, modifiers, event);
}

void TestApiImpl::kittyKey(u32 key_, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event) {
    vterm->kittyKey(key_, shiftedKey, baseLayoutKey, modifiers, event);
}

bool TestApiImpl::mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY) {
    return vterm->mouseHighlightRelease(endX, endY, mouseX, mouseY);
}

void TestApiImpl::locatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons) {
    vterm->locatorPosition(column, row, pixelX, pixelY, buttons);
}

void TestApiImpl::locatorButton(u8 button, bool pressed) {
    vterm->locatorButton(button, pressed);
}

void TestApiImpl::scrollUp(u16 count) {
    vterm->scrollUp(count);
}

void TestApiImpl::scrollDown(u16 count) {
    vterm->scrollDown(count);
}

void TestApiImpl::pageUp() {
    vterm->pageUp();
}

void TestApiImpl::pageDown() {
    vterm->pageDown();
}

void TestApiImpl::selectionStart(int pixelX, int pixelY, bool cycleSnapTo) {
    vterm->selectionStart(pixelX, pixelY, cycleSnapTo);
}

void TestApiImpl::selectionExtend(int pixelX, int pixelY, bool cycleSnapTo) {
    vterm->selectionExtend(pixelX, pixelY, cycleSnapTo);
}

void TestApiImpl::selectionUpdate(int pixelX, int pixelY) {
    vterm->selectionUpdate(pixelX, pixelY);
}

VtermTextResult TestApiImpl::selectionFinish() {
    return vterm->selectionFinish();
}

bool TestApiImpl::hasSelection() const {
    return vterm->hasSelection();
}

void TestApiImpl::selectionClear() {
    vterm->selectionClear();
}

void TestApiImpl::selectionRectangular() {
    vterm->selectionRectangular();
}

bool TestApiImpl::advanceSelectionAutoscroll() {
    return vterm->input.advanceSelectionAutoscroll(true);
}

void TestApiImpl::paste(StringView text) {
    vterm->paste(text);
}

bool TestApiImpl::pasteClipboard(bool primary) {
    return vterm->input.paste(primary);
}

StringView TestApiImpl::hyperlinkAt(int pixelX, int pixelY) {
    return vterm->hyperlinkAt(pixelX, pixelY);
}

bool VtermImpl::animationActive() const {
    return haveBlinkingText || cursorBlinkMode;
}

void VtermImpl::enableBlinkingText() {
    if (!animationActive()) {
        nextBlink = monotonicNowUs() + 500'000;
    }
    haveBlinkingText = true;
    wakeTimers();
}

void VtermImpl::exposeFrames() {
    frame_pri->expose();
    frame_alt->expose();
}

void VtermImpl::refreshBlinkingText() {
    const bool blinking = cf->hasBlinkingText();
    if (blinking && !animationActive()) {
        nextBlink = monotonicNowUs() + 500'000;
    }
    haveBlinkingText = blinking;
    wakeTimers();
}

void VtermImpl::wakeTimers() {
    if (syncFiber_ != nullptr) {
        syncFiber_->wake();
    }
    if (blinkFiber_ != nullptr) {
        blinkFiber_->wake();
    }
    if (autoscrollFiber_ != nullptr) {
        autoscrollFiber_->wake();
    }
}

void VtermImpl::startTimers() {
#ifdef SHITTY_FOR_TESTS
    // The harness drives blink, synchronized-output expiry and autoscroll
    // through the forced test entry points; live timers would race the
    // deterministic snapshots.
    return;
#endif
    plt::Scheduler* const scheduler = &scheduler_;
    syncFiber_ = scheduler->create(owner_, syncBody_);
    blinkFiber_ = scheduler->create(owner_, blinkBody_);
    autoscrollFiber_ = scheduler->create(owner_, autoscrollBody_);
}

void VtermImpl::runSyncWatchdog() {
    plt::Fiber* const self = scheduler_.current();
    for (;;) {
        if (!synchronizedOutputMode) {
            self->park();
            continue;
        }
        const u64 now = monotonicNowUs();
        if (synchronizedOutputDeadline > now && self->parkFor(synchronizedOutputDeadline - now)) {
            // A wake re-evaluates the mode and the deadline from scratch.
            continue;
        }
        if (expireSynchronizedOutput(false)) {
            host.requestFrame();
        }
    }
}

void VtermImpl::runBlink() {
    plt::Fiber* const self = scheduler_.current();
    for (;;) {
        if (!animationActive()) {
            self->park();
            continue;
        }
        const u64 now = monotonicNowUs();
        if (nextBlink > now && self->parkFor(nextBlink - now)) {
            continue;
        }
        if (advanceAnimation(false)) {
            expose();
            host.requestFrame();
        }
    }
}

void VtermImpl::runAutoscroll() {
    plt::Fiber* const self = scheduler_.current();
    for (;;) {
        if (input.selectionAutoscrollDeadline == 0) {
            self->park();
            continue;
        }
        const u64 now = monotonicNowUs();
        if (input.selectionAutoscrollDeadline > now && self->parkFor(input.selectionAutoscrollDeadline - now)) {
            continue;
        }
        input.advanceSelectionAutoscroll(false);
        host.requestFrame();
    }
}

size_t VtermInputSpec::getLength() const {
    return length ? length : StringView(input).length();
}

void VtermImpl::unhandledInput(unsigned char ch) {
    (void)ch;
}

void VtermImpl::redraw() {
    input.refreshHyperlink();
    if (synchronizedOutputMode) {
        return;
    }
    outputPending = true;
}

size_t VtermImpl::cellCapacity() const {
    size_t count = frame_pri->info().cellCapacity;
    if (altScreenInitialized) {
        count += frame_alt->info().cellCapacity;
    }
    return count;
}

void VtermImpl::updateExtraCellCount() {
    size_t count = cellCapacity();
    // A11 (Q2 of R5-qa). One store serves every terminal behind the
    // window, and it is sized by the sum over the live panes - not by
    // whoever wrote to it last, and not by an upper bound taken off the
    // window.
    //
    // "The last writer" was harmless before A8, because every terminal
    // held the window's grid and so "the last one" and "all of them"
    // were the same number. A pane is smaller than the window, so a
    // pane-sized budget had the store collecting on behalf of terminals
    // that are not this one, and the more panes there were the further
    // under the truth it fell. The window-sized floor that stood here
    // until now was the other half of the same mistake in the other
    // direction: an upper bound for one pane, and short by a whole pane
    // for every pane after the first.
    //
    // The caller counts itself rather than being counted: a terminal
    // sizes the store from inside its own construction, before
    // SessionSet has a record of it, so asking the set for everybody
    // else and adding its own is what makes the sum exact at every
    // moment instead of at most of them. With no set at all - the
    // headless adapters - a terminal is the only pane there is.
    count += host.cellCapacityExcept(this);
    extras_.store->setCellCount(count);
}

void VtermImpl::collectCellExtrasIfNeeded(bool force) {
    CellExtraStore& extras = *extras_.store;
    const bool hardLimit = extras.hardLimitExceeded();
    if (processInputDepth != 0) {
        return;
    }
    if (!extras.shouldCollect() && !force) {
        presentedSinceGcSafePoint = false;
        return;
    }
    if (!presentedSinceGcSafePoint && !hardLimit && !force) {
        return;
    }
    collectCellExtras();
    presentedSinceGcSafePoint = false;
}

void VtermImpl::collectExtras(Vector<TerminalCell*>& cells, Vector<u32*>& roots) {
    if (activeHyperlink != 0) {
        roots.pushBack(&activeHyperlink);
    }
    if (inputGraphemeScreen != nullptr && inputGraphemeHyperlink != 0) {
        roots.pushBack(&inputGraphemeHyperlink);
    }
    frame_pri->collectExtraCells(cells);
    if (altScreenInitialized) {
        frame_alt->collectExtraCells(cells);
    }
    // The composition preview and the slice the overlay points at live
    // outside both frames, and their clusters reference the same store:
    // a collection that skipped them would leave the preview pointing
    // into the freed pool.
    const auto collectPreeditCells = [&](Vector<TerminalCell>& preedit) {
        for (size_t index = 0; index != preedit.length(); ++index) {
            TerminalCell& cell = preedit.mut(index);
            if (cell.hasExtra()) {
                cells.pushBack(&cell);
            }
        }
    };
    collectPreeditCells(preeditCells);
    collectPreeditCells(preeditWindow);
}

void VtermImpl::collectCellExtras() {
    // R7: nothing is handed over here. This terminal is a client like any
    // other, so the store asks it for its cells along with everybody
    // else's - and a terminal that gathered its own would hand the same
    // roots over twice.
    Vector<TerminalCell*> none;
    extras_.store->collect(none, nullptr, 0);
}

bool VtermImpl::advanceAnimation(bool force) {
    refreshBlinkingText();
    if (!animationActive()) {
        return false;
    }
    const u64 now = monotonicNowUs();
    if (!force && now < nextBlink) {
        return false;
    }
    blinkVisible = !blinkVisible;
    changePresentation();
    nextBlink = now + 500'000;
    wakeTimers();
    return true;
}

bool VtermImpl::expireSynchronizedOutput(bool force) {
    if (!synchronizedOutputMode || (!force && monotonicNowUs() < synchronizedOutputDeadline)) {
        return false;
    }
    synchronizedOutputMode = false;
    redraw();
    return true;
}

bool VtermImpl::mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY) {
    if (mouseTrk.mode != MouseTrackingMode::VT200_Highlight || !mouseHighlight.active) {
        return false;
    }
    mouseHighlight.active = false;
    endY = min(max(endY, mouseHighlight.firstRow), mouseHighlight.lastRow);
    StringBuilder response;
    if (mouseTrk.enc == MouseTrackingEnc::SGR || mouseTrk.enc == MouseTrackingEnc::SGRPixels) {
        response << StringView(u8"<") << mouseHighlight.startX << StringView(u8";") << mouseHighlight.startY << StringView(u8";") << endX << StringView(u8";") << endY << StringView(u8";") << mouseX << StringView(u8";") << mouseY << StringView(u8"T");
    } else {
        const auto coordinate = [](u16 value) {
            return (u8)(32 + min<u16>(max<u16>(value, 1), 223));
        };
        const u8 kind = mouseHighlight.startX == endX && mouseHighlight.startY == endY ? u8't' : u8'T';
        const u8 startX = coordinate(mouseHighlight.startX);
        const u8 startY = coordinate(mouseHighlight.startY);
        response.append(&kind, 1);
        response.append(&startX, 1);
        response.append(&startY, 1);
        if (mouseHighlight.startX != endX || mouseHighlight.startY != endY) {
            const u8 coordinates[] = {coordinate(endX), coordinate(endY), coordinate(mouseX), coordinate(mouseY)};
            response.append(coordinates, sizeof(coordinates));
        }
    }
    writeCsiResponse(StringView(response));
    return true;
}

void VtermImpl::setLocatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons) {
    locator.column = max<u16>(1, column);
    locator.row = max<u16>(1, row);
    locator.pixelX = max<u16>(1, pixelX);
    locator.pixelY = max<u16>(1, pixelY);
    locator.buttons = buttons & 15;
    if (locator.enabled && locator.filter) {
        const u16 x = locator.pixels ? locator.pixelX : locator.column;
        const u16 y = locator.pixels ? locator.pixelY : locator.row;
        if (y < locator.filterTop || y > locator.filterBottom || x < locator.filterLeft || x > locator.filterRight) {
            StringBuilder response;
            response << StringView(u8"10;") << (unsigned)(locator.buttons) << StringView(u8";") << y << StringView(u8";") << x << StringView(u8";0&w");
            writeCsiResponse(StringView(response));
            locator.filter = false;
            if (locator.enabled == 2) {
                locator.enabled = 0;
            }
        }
    }
}

void VtermImpl::reportLocatorButton(u8 button, bool pressed) {
    if (!locator.enabled || (pressed ? !locator.reportDown : !locator.reportUp) || button < 1 || button > 4) {
        return;
    }
    const u8 bits[] = {0, 4, 2, 1, 8};
    if (pressed) {
        locator.buttons |= bits[button];
    } else {
        locator.buttons &= ~bits[button];
    }
    const u32 event = 2 + (button - 1) * 2 + (pressed ? 0 : 1);
    const u16 x = locator.pixels ? locator.pixelX : locator.column;
    const u16 y = locator.pixels ? locator.pixelY : locator.row;
    StringBuilder response;
    response << event << StringView(u8";") << (unsigned)(locator.buttons) << StringView(u8";") << y << StringView(u8";") << x << StringView(u8";0&w");
    writeCsiResponse(StringView(response));
    if (locator.enabled == 2) {
        locator.enabled = 0;
    }
}

VtermImpl::KittyKeyboardState& VtermImpl::kittyKeyboardState() {
    return altScreenBufferMode ? kittyKeyboardAlt : kittyKeyboardPri;
}

const VtermImpl::KittyKeyboardState& VtermImpl::kittyKeyboardState() const {
    return altScreenBufferMode ? kittyKeyboardAlt : kittyKeyboardPri;
}

u8 VtermImpl::getKittyKeyboardFlags() const {
    return kittyKeyboardState().flags;
}

void VtermImpl::setHasFocus(bool hasFocus_) {
    if (hasFocus != hasFocus_) {
        hasFocus = hasFocus_;
        changePresentation();
        // Only a transition is an event: activation sweeps deactivate
        // over every session, and reporting per sweep would spam the
        // child with focus-out it already knows about.
        if (mouseTrk.focusEventMode) {
            writeCsiResponse(hasFocus ? "I" : "O");
        }
    }
    showCursor();
    redraw();
}

void VtermImpl::pageUp() {
    if (altScrollMode && altScreenBufferMode) {
        for (int k = 0; k < (marginBottom - marginTop) / 2; ++k) {
            sendKey(InputKey::Up);
        }
    } else {
        cf->scrollView(pane_.rows / 2);
        refreshBlinkingText();
        redraw();
    }
}

u32 VtermImpl::scrollView(i32 rows) {
    if (rows != 0) {
        cf->scrollView(rows);
        refreshBlinkingText();
        redraw();
    }
    return cf->info().viewOffset;
}

u32 VtermImpl::scrollViewTo(u32 offset) {
    const u32 current = cf->info().viewOffset;
    if (offset == current) {
        return current;
    }
    // scrollView moves by a delta and a positive delta scrolls up into
    // history, so a larger target offset is a positive move.
    return scrollView((i32)(offset) - (i32)(current));
}

void VtermImpl::pageDown() {
    if (altScrollMode && altScreenBufferMode) {
        for (int k = 0; k < (marginBottom - marginTop) / 2; ++k) {
            sendKey(InputKey::Down);
        }
    } else {
        cf->scrollView(-(i32)(pane_.rows / 2));
        refreshBlinkingText();
        redraw();
    }
}

void VtermImpl::mouseWheelUp(u16 count) {
    if (altScrollMode && altScreenBufferMode) {
        for (u16 k = 0; k < count; ++k) {
            sendKey(InputKey::Up);
        }
    } else {
        cf->scrollView(count);
        refreshBlinkingText();
        redraw();
    }
}

void VtermImpl::mouseWheelDown(u16 count) {
    if (altScrollMode && altScreenBufferMode) {
        for (u16 k = 0; k < count; ++k) {
            sendKey(InputKey::Down);
        }
    } else {
        cf->scrollView(-(i32)(count));
        refreshBlinkingText();
        redraw();
    }
}

void VtermImpl::mouseWheelRight(u16 count) {
    if (altScrollMode && altScreenBufferMode) {
        for (u16 k = 0; k < count; ++k) {
            sendKey(InputKey::Right);
        }
    }
}

void VtermImpl::mouseWheelLeft(u16 count) {
    if (altScrollMode && altScreenBufferMode) {
        for (u16 k = 0; k < count; ++k) {
            sendKey(InputKey::Left);
        }
    }
}

void VtermImpl::resetTerminal() {
    parser->reset();
    switchScreenBufferMode(false, true);
    resetScreen();
    resetAttrs();
    sgrStackNext = 0;
    sgrStackCount = 0;
    userPreferenceCharset = Charset::DecSuppl;
    userPreferenceCharsetId = ((u16)('%') << 8) | '5';
    userPreferenceCharset96 = false;
    osc_RESET_PALETTE();
    if (assignedDefaultColors) {
        csi_DECAC_TEXT_RESET();
    }

    noClearColumnMode = false;
    switchColMode(ColMode::C80);

    cf->dropHistory();
    marginTop = 0;
    marginBottom = pane_.rows;
    clearScreen();

    switchScreenBufferMode(false);
    altScrollMode = config().altScrollMode;
    altSendsEscape = config().altSendsEscape;
    modifyOtherKeys = config().modifyOtherKeys;
    memcpy(modifyKeyResources, initialModifyKeyResources, sizeof(modifyKeyResources));
    clearIntMap(savedPrivModes);
    clearIntMap(userDefinedKeys);
    userDefinedKeysLocked = false;
    delete kittyClipboardWriteStream;
    kittyClipboardWriteStream = nullptr;
    kittyClipboardWriteLength = 0;
    kittyClipboardWriteId.reset();
    kittyKeyboardPri.flags = 0;
    kittyKeyboardPri.stack.clear();
    kittyKeyboardAlt.flags = 0;
    kittyKeyboardAlt.stack.clear();
    savedCursorPri.isSet = false;
    savedCursorAlt.isSet = false;
    activeHyperlink = 0;
    nextHyperlink = 1;
    currentSemantic = 0;
    semanticUntilEndOfLine = false;
    inactiveSemantic = 0;
    inactiveSemanticUntilEndOfLine = false;
    semanticClick = SemanticClick::None;
    inactiveSemanticClick = SemanticClick::None;
    titleModes = 0;
    titleDepth = 0;
    clearIntMap(notifications);

    horizMarginMode = false;
    hMargin = 0;
    nColsEff = pane_.columns;

    osc_TITLE_0(config().title);
}

void VtermImpl::resetScreen(bool resetTabStops) {
    utf8dec.reset();
    showCursorMode = true;
    cursorShape = TerminalCursor::Style::filled_block;
    cursorStyleParam = 2;
    cursorBlinkMode = false;
    haveBlinkingText = false;
    blinkVisible = true;
    nextBlink = monotonicNowUs() + 500'000;
    autoWrapMode = true;
    autoRepeatMode = true;
    smoothScrollMode = false;
    autoNewlineMode = false;
    keyboardLocked = false;
    insertMode = false;
    eraseModeAll = false;
    isoProtectionActive = false;
    rectangularAttributeExtent = false;
    checksumFlags = 0;
    attrs.protected_char = 0;
    bkspSendsDel = true;
    localEcho = false;
    bracketedPasteMode = false;
    synchronizedOutputMode = false;
    graphemeClusterMode = true;
    colorSchemeUpdateMode = false;
    inBandResizeMode = false;
    pasteMimeNotificationsMode = false;
    screenReverseVideo = false;
    eightBitInput = false;
    reverseWrapMode = false;
    extendedReverseWrapMode = false;
    nationalReplacementMode = false;
    ledState = 0;
    recordLeds(ledState);
    send8BitControls = false;
    altScrollMode = config().altScrollMode;
    altSendsEscape = config().altSendsEscape;

    compatLevel = CompatibilityLevel::VT400;
    cursorKeyMode = CursorKeyMode::ANSI;
    keypadMode = KeypadMode::Normal;
    originMode = OriginMode::Absolute;
    charsetState = CharsetState{};

    savedCursor->isSet = false;

    mouseTrk.setMode(MouseTrackingMode::Disabled);
    mouseTrk.setEncoding(MouseTrackingEnc::Default);
    mouseTrk.focusEventMode = false;
    locator = LocatorState{};

    if (resetTabStops) {
        tabStops.clear();
        tabStopsCustomized = false;
        tabStopsRestored = false;
    }
    cf->clearSelection();
}

void VtermImpl::resetAttrs() {
    attrs.uc_pt = 0;
    attrs.bold = 0;
    attrs.faint = 0;
    attrs.italic = 0;
    attrs.underline_style = 0;
    attrs.blink = 0;
    attrs.conceal = 0;
    attrs.strike = 0;
    attrs.overline = 0;
    attrs.inverse = 0;
    reverseVideo = false;
    fgPalIx = defaultFgPalIx;
    setFgFromPalIx();
    bgPalIx = defaultBgPalIx;
    setBgFromPalIx();
    underlineColorDefault = true;
    setAttrUnderlineColor(attrForeground());
}

void VtermImpl::clearScreen() {
    posX = 0;
    posY = 0;
    lastCol = false;
    fillScreen(' ');
}

void VtermImpl::fillScreen(u16 ch) {
    cf->fillCells(ch, attrs);
}

void VtermImpl::switchColMode(ColMode colMode_, bool force) {
    const bool changed = colMode != colMode_;
    if (!changed && !force) {
        return;
    }

    const u16 columns = colMode_ == ColMode::C80 ? 80 : 132;
    if (windowColumns() != columns) {
        windowOperation(8, windowRows(), columns);
    }
    marginTop = 0;
    marginBottom = pane_.rows;
    hMargin = 0;
    nColsEff = pane_.columns;
    posX = 0;
    posY = 0;
    lastCol = false;
    if (!noClearColumnMode) {
        fillScreen(' ');
    }

    colMode = colMode_;
}

void VtermImpl::switchScreenBufferMode(bool altScreenBufferMode_, bool clearAlternate) {
    if (altScreenBufferMode != altScreenBufferMode_ || (altScreenBufferMode_ && clearAlternate)) {
        input.stopSelectionAutoscroll();
    }
    if (altScreenBufferMode == altScreenBufferMode_) {
        if (clearAlternate) {
            if (altScreenBufferMode_) {
                createAlternateScreen();
                currentSemantic = 0;
                semanticUntilEndOfLine = false;
                semanticClick = SemanticClick::None;
                marginTop = 0;
                marginBottom = pane_.rows;
                hMargin = 0;
                nColsEff = pane_.columns;
                altScreenInitialized = true;
                cf = frame_alt;
                cf->expose();
                changePresentation();
            } else if (altScreenInitialized) {
                createInactiveAlternateScreen();
                inactiveSemantic = 0;
                inactiveSemanticUntilEndOfLine = false;
                inactiveSemanticClick = SemanticClick::None;
                altScreenInitialized = false;
            }
            updateExtraCellCount();
            refreshBlinkingText();
        }
        return;
    }

    if (altScreenBufferMode_) {
        if (clearAlternate || !altScreenInitialized) {
            createAlternateScreen();
            inactiveSemantic = 0;
            inactiveSemanticUntilEndOfLine = false;
            inactiveSemanticClick = SemanticClick::None;
            marginTop = 0;
            marginBottom = pane_.rows;
            hMargin = 0;
            nColsEff = pane_.columns;
            altScreenInitialized = true;
        } else if (const ScreenInfo info = frame_alt->info(); info.columns != pane_.columns || info.rows != pane_.rows) {
            Screen::Cursor cursorState;
            resizeScreen(frame_alt, frameAltPool, cursorState, &savedCursorAlt);
            marginTop = 0;
            marginBottom = pane_.rows;
            hMargin = 0;
            nColsEff = pane_.columns;
        }
        cf = frame_alt;
        cf->expose();

        savedCursor = &savedCursorAlt;
        altScreenBufferMode = true;
    } else {
        if (const ScreenInfo info = frame_pri->info(); info.columns != pane_.columns || info.rows != pane_.rows || info.saveLines != config().saveLines) {
            Screen::Cursor cursorState{Point(posX, posY), lastCol};
            resizeScreen(frame_pri, framePriPool, cursorState, &savedCursorPri);
            posX = cursorState.position.x;
            posY = cursorState.position.y;
            lastCol = cursorState.pendingWrap;
            marginTop = 0;
            marginBottom = pane_.rows;
            hMargin = 0;
            nColsEff = pane_.columns;
        }
        cf = frame_pri;
        cf->expose();
        if (clearAlternate) {
            createInactiveAlternateScreen();
            altScreenInitialized = false;
        }
        savedCursor = &savedCursorPri;
        altScreenBufferMode = false;
    }
    stl::xchg(currentSemantic, inactiveSemantic);
    stl::xchg(semanticUntilEndOfLine, inactiveSemanticUntilEndOfLine);
    stl::xchg(semanticClick, inactiveSemanticClick);
    if (!altScreenBufferMode_ && clearAlternate) {
        inactiveSemantic = 0;
        inactiveSemanticUntilEndOfLine = false;
        inactiveSemanticClick = SemanticClick::None;
    }
    updateExtraCellCount();
    refreshBlinkingText();
    changePresentation();
}

void VtermImpl::normalizeCursorPos() {
    if (nColsEff < posX + 1) {
        posX = nColsEff - 1;
    }

    if (pane_.rows < posY + 1) {
        posY = pane_.rows - 1;
    }

    lastCol = false;
}

bool VtermImpl::isCursorInsideMargins() const {
    return posX >= hMargin && posX < nColsEff && posY >= marginTop && posY < marginBottom;
}

void VtermImpl::activeColumns(u16& begin, u16& end) const {
    if (posX < nColsEff && posY >= marginTop && posY < marginBottom) {
        begin = posX < hMargin ? 0 : hMargin;
        end = nColsEff;
    } else {
        begin = 0;
        end = pane_.columns;
    }
}

void VtermImpl::activeLine(u16& begin, u16& end) {
    activeColumns(begin, end);
    if (!cf->lineAttribute(posY)) {
        return;
    }
    end = doubleWidthEnd(end);
    if (posX >= end) {
        posX = end - 1;
        lastCol = false;
        activeColumns(begin, end);
        end = doubleWidthEnd(end);
    }
}

u16 VtermImpl::doubleWidthEnd(u16 normalEnd) const {
    // A line rendition doubles source cells from the left edge of the
    // screen.  Its source coordinate space is therefore always the first
    // half of the screen; the right margin may only shorten it.  In
    // particular, the left margin must not shift this boundary as the cursor
    // crosses it.
    return min(normalEnd, max<u16>(1, pane_.columns / 2));
}

void VtermImpl::eraseRow(u16 pY) {
    eraseRangeInRow(pY, hMargin, nColsEff - hMargin);
    if (hMargin == 0 && nColsEff == pane_.columns) {
        cf->setLineAttribute(pY, 0);
    }
}

void VtermImpl::eraseRows(u16 startY, u16 count) {
    for (u16 pY = startY; pY < startY + count; ++pY) {
        eraseRow(pY);
    }
}

void VtermImpl::copyRow(u16 dstY, u16 srcY) {
    if (dstY == srcY) {
        return;
    }

    cf->copyRow(dstY, srcY, hMargin, nColsEff - hMargin, eraseAttrs);
}

void VtermImpl::insertRows(u16 startY, u16 count) {
    if (hMargin == 0 && nColsEff == pane_.columns) {
        cf->rotateRows(startY, marginBottom, count);
    } else {
        cf->scrollRectangle(startY, hMargin, marginBottom, nColsEff, count, eraseAttrs);
        return;
    }

    for (u16 pY = startY; pY < startY + count; ++pY) {
        eraseRow(pY);
    }
}

void VtermImpl::deleteRows(u16 startY, u16 count) {
    if (hMargin == 0 && nColsEff == pane_.columns) {
        cf->rotateRows(startY, marginBottom, -(i32)(count));
    } else {
        cf->scrollRectangle(startY, hMargin, marginBottom, nColsEff, -(i32)(count), eraseAttrs);
        return;
    }

    for (u16 pY = marginBottom - count; pY < marginBottom; ++pY) {
        eraseRow(pY);
    }
}

void VtermImpl::insertCols(u16 startX, u16 count) {
    for (u16 r = marginTop; r < marginBottom; ++r) {
        cf->insertCells(r, startX, nColsEff, count, eraseAttrs);
    }
}

void VtermImpl::deleteCols(u16 startX, u16 count) {
    for (u16 r = marginTop; r < marginBottom; ++r) {
        cf->deleteCells(r, startX, nColsEff, count, eraseAttrs);
    }
}

void VtermImpl::eraseRangeInRow(u16 row, u16 start, u16 count) {
    if (!count) {
        return;
    }
    cf->eraseCells(row, start, count, eraseAttrs);
}

void VtermImpl::eraseEcmaRangeInRow(u16 row, u16 start, u16 count) {
    if (eraseModeAll || !isoProtectionActive) {
        eraseRangeInRow(row, start, count);
        return;
    }
    if (!count) {
        return;
    }
    cf->selectiveEraseCells(row, start, count, eraseAttrs, TerminalCell::isoProtection);
}

void VtermImpl::eraseEcmaRow(u16 row) {
    if (eraseModeAll || !isoProtectionActive) {
        eraseRangeInRow(row, 0, pane_.columns);
        cf->setLineAttribute(row, 0);
        return;
    }
    const bool retained = cf->hasProtection(row, TerminalCell::isoProtection);
    eraseEcmaRangeInRow(row, 0, pane_.columns);
    if (!retained) {
        cf->setLineAttribute(row, 0);
    }
}

void VtermImpl::selectiveEraseRangeInRow(u16 row, u16 start, u16 count) {
    if (!count) {
        return;
    }
    cf->selectiveEraseCells(row, start, count, eraseAttrs, TerminalCell::decProtection);
}

void VtermImpl::rectangleOrigin(u16& rowBase, u16& columnBase, u16& rowLimit, u16& columnLimit) const {
    if (originMode == OriginMode::ScrollingRegion) {
        rowBase = marginTop;
        columnBase = hMargin;
        rowLimit = marginBottom;
        columnLimit = nColsEff;
    } else {
        rowBase = 0;
        columnBase = 0;
        rowLimit = pane_.rows;
        columnLimit = pane_.columns;
    }
}

bool VtermImpl::rectangleFromParams(CsiRectangle parameters, Rectangle& rectangle) const {
    u16 rowBase, columnBase, rowLimit, columnLimit;
    rectangleOrigin(rowBase, columnBase, rowLimit, columnLimit);
    const u32 rows = rowLimit - rowBase;
    const u32 columns = columnLimit - columnBase;
    const u32 rawTop = parameters.top ? parameters.top : 1;
    const u32 rawLeft = parameters.left ? parameters.left : 1;
    const u32 rawBottom = parameters.bottom ? parameters.bottom : rows;
    const u32 rawRight = parameters.right ? parameters.right : columns;
    if (rawTop > rawBottom || rawLeft > rawRight) {
        return false;
    }

    rectangle.top = rowBase + min(rawTop, rows) - 1;
    rectangle.left = columnBase + min(rawLeft, columns) - 1;
    rectangle.bottom = rowBase + min(rawBottom, rows);
    rectangle.right = columnBase + min(rawRight, columns);
    return true;
}

void VtermImpl::inputGraphicChar(unsigned char ch) {
    if ((ch & 0x80) == 0) {
        if (utf8dec.checkPrematureEOS()) {
            placeGraphicChar();
        }

        Charset cs;
        if (charsetState.ss) {
            cs = charsetState.g[charsetState.ss];
            charsetState.ss = 0;
        } else {
            cs = charsetState.g[charsetState.gl];
        }

        if (cs == Charset::UTF8) {
            if (utf8dec.onUnicode(ch < 127 ? ch : 0)) {
                placeGraphicChar();
            }
        } else if (ch >= 32 && (cs == Charset::IsoLatin1 || ch < 127)) {
            if (utf8dec.onUnicode(translateCharset(cs, ch))) {
                placeGraphicChar();
            }
        }
    } else {
        Charset cs = charsetState.g[charsetState.gr];
        if (cs == Charset::UTF8) {
            for (int completed = utf8dec.pushByte(ch); completed > 0; --completed) {
                placeGraphicChar();
            }
        } else if (ch >= 160 && (cs == Charset::IsoLatin1 || ch < 255)) {
            // ISO 2022 invokes a 94/96-character G-set in GR by moving its
            // 7-bit code positions into the right half.  Strip that bit and
            // use the same translation path as GL; NRC sets are deliberately
            // outside charCodes, so indexing the table directly was both
            // incorrect and out of bounds for LS1R/LS2R/LS3R.
            if (utf8dec.onUnicode(translateCharset(cs, ch & 0x7f))) {
                placeGraphicChar();
            }
        }
    }
}

void VtermImpl::resetGraphemeInput() {
    inputGraphemeScreen = nullptr;
}

[[gnu::always_inline]] inline u8 VtermImpl::codepointData(u32 codepoint) {
    constexpr u8 valid = 0x80;
    constexpr u8 simple = 0x04;
    u8& cached = (*unicodeProperties)[codepoint];
    if ((cached & valid) == 0) {
        const CodepointProperties properties = config().widths.codepointProperties(codepoint);
        cached = valid | properties.width | (properties.simpleGrapheme ? simple : 0);
    }
    return cached;
}

void VtermImpl::placeGraphicChar() {
    const u32 pt = utf8dec.getUnicode();
    if (inputGraphemeScreen != cf) {
        inputGraphemeBreaker.reset();
    }
    const u8 data = codepointData(pt);
    placeGraphicChar(inputGraphemeBreaker.breakBefore(pt, (data & 0x04) != 0), data & 0x03);
}

void VtermImpl::placeGraphicChar(bool graphemeBoundary) {
    placeGraphicChar(graphemeBoundary, codepointData(utf8dec.getUnicode()) & 0x03);
}

void VtermImpl::placeGraphicChar(bool graphemeBoundary, u8 width) {
    u32 pt = utf8dec.getUnicode();
    u8 w = width;
    bool advanceCursor = true;
    u16 lineBegin, lineCols;
    activeLine(lineBegin, lineCols);

    // A width-zero control starts a new grapheme but has no printable cell.
    // The same applies to a leading joiner, which has nothing to join. C1
    // codepoints are the exception among controls: they only reach this
    // path decoded out of UTF-8 text - the raw bytes are the parser's -
    // and kitty-style parsing places them as ordinary text instead of
    // dropping them.
    if (graphemeBoundary && w == 0 && !(pt >= 0x80 && pt <= 0x9f)) {
        const GraphemeClass graphemeClass = unicodeCodepointProperties(pt).graphemeClass;
        if (graphemeClass == GraphemeClass::Control || graphemeClass == GraphemeClass::Zwj) {
            inputGraphemeBreaker.reset();
            return;
        }
        // There is no preceding cell to extend at the left edge.  Retain the
        // degenerate cluster in the current cell without advancing: a later
        // printable character replaces it instead of being shifted right.
        if (posX == 0 && !lastCol) {
            advanceCursor = false;
        }
    }

    if (inputGraphemeScreen == cf && !graphemeBoundary) {
        const u32 previous = inputGrapheme.empty() ? inputGraphemeBase : inputGrapheme.data()[inputGrapheme.size() - 1];
        const GraphemeWidthEffect widthEffect = graphemeClusterMode ? config().widths.graphemeWidthEffect(previous, pt) : GraphemeWidthEffect::Unchanged;
        if (widthEffect == GraphemeWidthEffect::Wide && !inputGraphemeWide && inputGraphemeX == lineCols - 1 && !autoWrapMode) {
            // A cluster cannot grow into half of a wide cell.  Keep the
            // already displayed narrow cluster and discard this width
            // transition, as xterm.js, Alacritty and Ghostty do.
            inputGraphemeBreaker.setBoundaryAfter(previous);
            return;
        }
        if (inputGrapheme.empty()) {
            inputGrapheme.pushBack(inputGraphemeBase);
        }
        if (!inputGrapheme.pushBack(pt)) {
            return;
        }
        u16 targetX = inputGraphemeX;
        u16 targetY = inputGraphemeY;
        bool wide = inputGraphemeWide;
        switch (widthEffect) {
            case GraphemeWidthEffect::Wide:
                if (!wide && lineCols - lineBegin >= 2) {
                    if (targetX == lineCols - 1) {
                        if (autoWrapMode) {
                            cf->eraseCells(targetY, targetX, 1, eraseAttrs);
                            const u16 wrapColumn = targetX > lineBegin ? targetX - 1 : targetX;
                            cf->setWrapped(targetY, wrapColumn);
                            inp_CR();
                            inp_LF();
                            targetX = posX;
                            targetY = posY;
                            wide = true;
                        }
                    } else {
                        wide = true;
                    }
                    if (wide) {
                        if (targetX + 1 == lineCols - 1) {
                            posX = targetX + 1;
                            lastCol = true;
                        } else {
                            posX = targetX + 2;
                            lastCol = false;
                        }
                    }
                }
                break;
            case GraphemeWidthEffect::Narrow:
                if (wide) {
                    wide = false;
                    posX = targetX + 1;
                    lastCol = false;
                }
                break;
            case GraphemeWidthEffect::Unchanged:
                break;
        }
        cf->writeGrapheme(targetY, targetX, inputGrapheme.data(), inputGrapheme.size(), wide, inputGraphemeAttrs, inputGraphemeHyperlink, inputGraphemeSemantic, eraseAttrs);
        inputGraphemeX = targetX;
        inputGraphemeY = targetY;
        inputGraphemeWide = wide;
        return;
    }

    if (posX >= lineCols) {
        posX = lineCols - 1;
        lastCol = false;
    }
    if (autoWrapMode && lastCol) {
        cf->setWrapped(posY, posX);
        inp_CR();
        inp_LF();
        activeLine(lineBegin, lineCols);
    }

    if (w == 2 && posX == lineCols - 1 && autoWrapMode) {
        // The wide glyph belongs wholly to the next row.  Mark the last
        // occupied cell as the soft-wrap boundary, not the unused final
        // column: otherwise copying the logical line invents a space.
        const u16 wrapColumn = posX > lineBegin ? posX - 1 : posX;
        cf->setWrapped(posY, wrapColumn);
        inp_CR();
        inp_LF();
        activeLine(lineBegin, lineCols);
    }

    if (w == 2 && lineCols - lineBegin >= 2 && posX == lineCols - 1) {
        // DECAWM is disabled: there is no second cell and truncating the
        // glyph would violate the grid's wide-cell invariant.
        resetGraphemeInput();
        inputGraphemeBreaker.reset();
        return;
    }

    if (w == 0) {
        w = 1;
    }

    const u16 clusterX = posX;
    const u16 clusterY = posY;
    const bool wide = w == 2 && posX < lineCols - 1;
    if (insertMode && advanceCursor) {
        // A wide glyph occupies two cells; inserting a single cell would let
        // it overwrite one cell of existing content instead of shifting it.
        // Mirror placePreparedRun and insert directly: insert mode is not
        // bounded by the scrolling margins the way the ICH control is.
        cf->insertCells(posY, posX, lineCols, wide ? 2 : 1, eraseAttrs);
    }
    cf->writeCodepoint(posY, posX, pt, wide, attrs, activeHyperlink, currentSemantic, eraseAttrs);
    if (attrs.blink) {
        enableBlinkingText();
    }

    inputGrapheme.clear();
    inputGraphemeBase = pt;
    inputGraphemeScreen = cf;
    inputGraphemeX = clusterX;
    inputGraphemeY = clusterY;
    inputGraphemeWide = wide;
    inputGraphemeAttrs = attrs;
    inputGraphemeHyperlink = activeHyperlink;
    inputGraphemeSemantic = currentSemantic;

    if (!advanceCursor) {
        return;
    }

    if (wide) {
        ++posX;
    }

    if (posX == lineCols - 1) {
        lastCol = true;
    } else {
        ++posX;
    }
}

template <bool insert>
void VtermImpl::placeAsciiRun(const u8* input, size_t size) {
    bool checkBoundary = true;
    while (size > 0) {
        bool graphemeBoundary = true;
        if (checkBoundary) {
            if (inputGraphemeScreen != cf) {
                inputGraphemeBreaker.reset();
            }
            graphemeBoundary = inputGraphemeBreaker.breakBefore(*input);
            checkBoundary = false;
        }
        if (!graphemeBoundary) {
            utf8dec.setUnicode(*input++);
            placeGraphicChar(false);
            --size;
            continue;
        }
        if constexpr (!insert) {
            if (autoWrapMode && lastCol && horizMarginMode && isCursorInsideMargins() && posY == marginBottom - 1 && (hMargin != 0 || nColsEff != pane_.columns)) {
                const u16 lineWidth = nColsEff - hMargin;
                const u16 fullLines = min<size_t>(size / lineWidth, 0xffff);
                if (fullLines >= 2) {
                    bool plainRows = true;
                    for (u16 row = marginTop; row < marginBottom; ++row) {
                        if (cf->lineAttribute(row) != 0) {
                            plainRows = false;
                            break;
                        }
                    }
                    if (plainRows) {
                        const u16 regionHeight = marginBottom - marginTop;
                        if (fullLines < regionHeight) {
                            cf->setWrapped(posY, posX);
                            scrollRegionUp(fullLines);
                        }

                        const u16 survivors = min<u16>(fullLines, regionHeight);
                        const u16 firstLine = fullLines - survivors;
                        const u8* text = input + (size_t)(firstLine)*lineWidth;
                        const u16 doubleEnd = hMargin + max<u16>(1, lineWidth / 2);
                        u16 row = marginBottom - survivors;
                        for (u16 line = firstLine; line < fullLines; ++line, ++row, text += lineWidth) {
                            const Screen::WriteResult written = cf->writeAsciiRun(row, hMargin, nColsEff, doubleEnd, text, lineWidth, attrs, activeHyperlink, currentSemantic, eraseAttrs);
                            STD_ASSERT(written.count == lineWidth);
                            if (line + 1 != fullLines) {
                                cf->setWrapped(row, nColsEff - 1);
                            }
                        }

                        if (attrs.blink) {
                            enableBlinkingText();
                        }
                        const size_t consumed = (size_t)(fullLines)*lineWidth;
                        const u32 codepoint = input[consumed - 1];
                        inputGrapheme.clear();
                        inputGraphemeBase = codepoint;
                        inputGraphemeScreen = cf;
                        inputGraphemeX = nColsEff - 1;
                        inputGraphemeY = marginBottom - 1;
                        inputGraphemeWide = false;
                        inputGraphemeAttrs = attrs;
                        inputGraphemeHyperlink = activeHyperlink;
                        inputGraphemeSemantic = currentSemantic;
                        inputGraphemeBreaker.setBoundaryAfter(codepoint);
                        utf8dec.setUnicode(codepoint);
                        posX = nColsEff - 1;
                        posY = marginBottom - 1;
                        lastCol = true;
                        input += consumed;
                        size -= consumed;
                        continue;
                    }
                }
            }
        }
        if (autoWrapMode && lastCol) {
            cf->setWrapped(posY, posX);
            inp_CR();
            inp_LF();
        }

        u16 lineBegin, lineEnd;
        activeColumns(lineBegin, lineEnd);
        const u16 doubleEnd = doubleWidthEnd(lineEnd);
        const u16 requested = min<size_t>(size, 0xffff);
        Screen::WriteResult written;
        if constexpr (insert) {
            written = cf->writeAsciiRunInsert(posY, posX, lineEnd, doubleEnd, input, requested, attrs, activeHyperlink, currentSemantic, eraseAttrs);
        } else {
            written = cf->writeAsciiRun(posY, posX, lineEnd, doubleEnd, input, requested, attrs, activeHyperlink, currentSemantic, eraseAttrs);
        }
        if (written.count == 0) {
            inputGraphemeBreaker.setBoundaryAfter(*input);
            utf8dec.setUnicode(*input++);
            placeGraphicChar(true);
            --size;
            continue;
        }
        const u16 count = written.count;
        const u16 lineCols = written.end;
        const u16 startX = posX;
        const u16 endX = startX + count;
        if (attrs.blink) {
            enableBlinkingText();
        }

        const u16 clusterX = endX - 1;
        const u32 codepoint = input[count - 1];
        inputGrapheme.clear();
        inputGraphemeBase = codepoint;
        inputGraphemeScreen = cf;
        inputGraphemeX = clusterX;
        inputGraphemeY = posY;
        inputGraphemeWide = false;
        inputGraphemeAttrs = attrs;
        inputGraphemeHyperlink = activeHyperlink;
        inputGraphemeSemantic = currentSemantic;
        inputGraphemeBreaker.setBoundaryAfter(codepoint);
        utf8dec.setUnicode(codepoint);

        if (endX == lineCols) {
            posX = lineCols - 1;
            lastCol = true;
        } else {
            posX = endX;
            lastCol = false;
        }
        input += count;
        size -= count;
    }
}

void VtermImpl::placeRepeatedCodepoint(u32 codepoint, u32 count) {
    while (count != 0) {
        if (autoWrapMode && lastCol) {
            cf->setWrapped(posY, posX);
            inp_CR();
            inp_LF();
        }

        u16 lineBegin, lineCols;
        activeLine(lineBegin, lineCols);
        if (posX >= lineCols) {
            utf8dec.setUnicode(codepoint);
            placeGraphicChar(true, 1);
            --count;
            continue;
        }
        const u16 written = min<u32>(count, lineCols - posX);
        const u16 startX = posX;
        const u16 endX = startX + written;
        cf->writeRepeatedCodepoint(posY, startX, written, codepoint, attrs, activeHyperlink, currentSemantic, eraseAttrs);

        inputGrapheme.clear();
        inputGraphemeBase = codepoint;
        inputGraphemeScreen = cf;
        inputGraphemeX = endX - 1;
        inputGraphemeY = posY;
        inputGraphemeWide = false;
        inputGraphemeAttrs = attrs;
        inputGraphemeHyperlink = activeHyperlink;
        inputGraphemeSemantic = currentSemantic;

        if (endX == lineCols) {
            posX = lineCols - 1;
            lastCol = true;
        } else {
            posX = endX;
            lastCol = false;
        }
        count -= written;
    }
    utf8dec.setUnicode(codepoint);
    if (attrs.blink) {
        enableBlinkingText();
    }
}

// Assemble clusters before touching the screen. Only monotonic width
// transitions that fit on this row are admitted: shrinking can erase a
// neighbour outside the final span, and wrapping can relocate a cluster.
// Those clusters are replayed by the scalar path, with the breaker restored
// to their starting state. A feed's last cluster is displayed immediately;
// inputGrapheme retains its spelling for extensions in the next feed.
[[gnu::noinline]] size_t VtermImpl::placeGraphemeRun(const u32* codepoints, size_t count, u64 resetBefore) {
    if (autoWrapMode && lastCol) {
        return 0;
    }
    u16 lineBegin, lineCols;
    activeLine(lineBegin, lineCols);
    if (posX >= lineCols) {
        return 0;
    }
    constexpr size_t capacity = 64;
    STD_ASSERT(count <= capacity);
    ScreenGrapheme graphemes[capacity];
    u8 widths[capacity];
    GraphemeBreaker breaker = inputGraphemeBreaker;
    GraphemeBreaker clusterStart = breaker;
    const u16 available = lineCols - posX;
    u16 glyphCount = 0;
    u16 cellCount = 0;
    size_t consumed = 0;
    bool rowFull = false;
    bool hasWide = false;
    // Most runs consist entirely of independent glyphs. Collect their
    // widths without cluster checkpoints; only materialize descriptors
    // when a joining sequence actually needs them.
    if (breaker.simpleBoundary()) {
        while (consumed < count) {
            const u8 data = codepointData(codepoints[consumed]);
            const u8 width = data & 0x03;
            if ((data & 0x04) == 0 || width == 0) {
                break;
            }
            if (cellCount + width > available) {
                rowFull = true;
                break;
            }
            widths[consumed++] = width;
            cellCount += width;
            hasWide |= width == 2;
        }
        if (consumed != 0) {
            breaker.setBoundaryAfter(codepoints[consumed - 1], true);
        }
    }
    const bool clustered = consumed < count && !rowFull;
    glyphCount = consumed;
    if (clustered) {
        for (size_t index = 0; index < consumed; ++index) {
            graphemes[index] = {1, widths[index]};
        }
        if (consumed > 1) {
            clusterStart.setBoundaryAfter(codepoints[consumed - 2], true);
        }
    }
    while (clustered && consumed < count) {
        const u32 codepoint = codepoints[consumed];
        const u8 data = codepointData(codepoint);
        const GraphemeBreaker before = breaker;
        if ((resetBefore >> consumed) & 1) {
            breaker.reset();
        }
        if (breaker.breakBefore(codepoint, (data & 0x04) != 0)) {
            const u8 width = data & 0x03;
            if (width == 0 || cellCount + width > available) {
                breaker = before;
                break;
            }
            clusterStart = before;
            graphemes[glyphCount++] = {1, width};
            cellCount += width;
        } else {
            if (glyphCount == 0) {
                return 0;
            }
            ScreenGrapheme& grapheme = graphemes[glyphCount - 1];
            const GraphemeWidthEffect effect = graphemeClusterMode ? config().widths.graphemeWidthEffect(codepoints[consumed - 1], codepoint) : GraphemeWidthEffect::Unchanged;
            const bool grows = effect == GraphemeWidthEffect::Wide && grapheme.width == 1;
            if (grapheme.count == GraphemeBuffer::capacity || (effect == GraphemeWidthEffect::Narrow && grapheme.width == 2) || (grows && cellCount == available)) {
                consumed -= grapheme.count;
                cellCount -= grapheme.width;
                --glyphCount;
                breaker = clusterStart;
                break;
            }
            ++grapheme.count;
            grapheme.width += grows;
            cellCount += grows;
        }
        ++consumed;
    }
    if (glyphCount == 0) {
        return 0;
    }
    if (clustered) {
        cf->writeGraphemeRun(posY, posX, codepoints, graphemes, glyphCount, cellCount, attrs, activeHyperlink, currentSemantic, eraseAttrs);
    } else if (hasWide) {
        cf->writeGlyphRun(posY, posX, codepoints, widths, glyphCount, cellCount, attrs, activeHyperlink, currentSemantic, eraseAttrs);
    } else {
        cf->writeRun(posY, posX, codepoints, glyphCount, attrs, activeHyperlink, currentSemantic, eraseAttrs);
    }
    const ScreenGrapheme last = clustered ? graphemes[glyphCount - 1] : ScreenGrapheme{1, widths[glyphCount - 1]};
    const size_t lastStart = consumed - last.count;
    inputGrapheme.clear();
    if (last.count != 1) {
        for (size_t index = lastStart; index < consumed; ++index) {
            inputGrapheme.pushBack(codepoints[index]);
        }
    }
    inputGraphemeBase = codepoints[lastStart];
    inputGraphemeScreen = cf;
    inputGraphemeX = posX + cellCount - last.width;
    inputGraphemeY = posY;
    inputGraphemeWide = last.width == 2;
    inputGraphemeAttrs = attrs;
    inputGraphemeHyperlink = activeHyperlink;
    inputGraphemeSemantic = currentSemantic;
    inputGraphemeBreaker = breaker;
    utf8dec.setUnicode(codepoints[consumed - 1]);
    posX += cellCount;
    lastCol = posX == lineCols;
    posX -= lastCol;
    if (attrs.blink) {
        enableBlinkingText();
    }
    return consumed;
}

// Decoding owns byte recovery and protocol boundaries; placement sees
// normalized text blocks and keeps screen writes independent of byte classes.
int VtermImpl::placeUtf8Run(const u8* input, int size, u8& pendingTrace) {
    int consumed = 0;
    if (inputGraphemeScreen != cf) {
        inputGraphemeBreaker.reset();
    }
    do {
        Utf8Text text;
        text.pendingTrace = pendingTrace;
        const size_t bytes = utf8dec.decodeText(input + consumed, size - consumed, text);
        for (size_t index = 0; index < text.count;) {
            const u64 resets = text.resetBefore >> index;
            if (resets & 1) {
                resetGraphemeInput();
                inputGraphemeBreaker.reset();
            }
            if (text.simple && inputGraphemeBreaker.simpleBoundary()) {
                placePreparedRun<false>(text.codepoints + index, nullptr, text.count - index);
                inputGraphemeBreaker.setBoundaryAfter(text.codepoints[text.count - 1], true);
                break;
            }
            const size_t written = insertMode ? 0 : placeGraphemeRun(text.codepoints + index, text.count - index, resets);
            if (written != 0) {
                index += written;
            } else {
                utf8dec.setUnicode(text.codepoints[index++]);
                placeGraphicChar();
            }
        }
        if (text.resetAfter) {
            resetGraphemeInput();
            inputGraphemeBreaker.reset();
        }
        consumed += bytes;
        pendingTrace = text.pendingTrace;
        if (!text.full || bytes == 0) {
            break;
        }
    } while (consumed < size);
    return consumed;
}

template <bool hasWide>
void VtermImpl::placePreparedRun(const u32* input, const u8* widths, size_t size) {
    while (size > 0) {
        if (autoWrapMode && lastCol) {
            cf->setWrapped(posY, posX);
            inp_CR();
            inp_LF();
        }

        u16 lineBegin, lineCols;
        activeLine(lineBegin, lineCols);
        if (posX >= lineCols) {
            utf8dec.setUnicode(*input++);
            if constexpr (hasWide) {
                placeGraphicChar(true, *widths++);
            } else {
                placeGraphicChar(true, 1);
            }
            --size;
            continue;
        }
        const u16 available = lineCols - posX;
        u16 count = 0;
        u16 cellCount = 0;
        if constexpr (hasWide) {
            while (count < size && cellCount + widths[count] <= available) {
                cellCount += widths[count++];
            }
            if (count == 0) {
                utf8dec.setUnicode(*input++);
                placeGraphicChar(true, *widths++);
                --size;
                continue;
            }
        } else {
            count = min<size_t>(size, available);
            cellCount = count;
        }
        const u16 startX = posX;
        const u16 endX = startX + cellCount;
        if (insertMode) {
            cf->insertCells(posY, startX, lineCols, cellCount, eraseAttrs);
        }
        if constexpr (hasWide) {
            cf->writeGlyphRun(posY, startX, input, widths, count, cellCount, attrs, activeHyperlink, currentSemantic, eraseAttrs);
        } else {
            cf->writeRun(posY, startX, input, count, attrs, activeHyperlink, currentSemantic, eraseAttrs);
        }
        if (attrs.blink) {
            enableBlinkingText();
        }

        const u8 lastWidth = hasWide ? widths[count - 1] : 1;
        const bool lastWide = lastWidth == 2;
        const u16 clusterX = endX - lastWidth;
        const u32 codepoint = input[count - 1];
        inputGrapheme.clear();
        inputGraphemeBase = codepoint;
        inputGraphemeScreen = cf;
        inputGraphemeX = clusterX;
        inputGraphemeY = posY;
        inputGraphemeWide = lastWide;
        inputGraphemeAttrs = attrs;
        inputGraphemeHyperlink = activeHyperlink;
        inputGraphemeSemantic = currentSemantic;
        // The caller synchronizes grapheme boundaries for the batch;
        // placement retains the last glyph for a later extension or REP.
        utf8dec.setUnicode(codepoint);

        if (endX == lineCols) {
            posX = lineCols - 1;
            lastCol = true;
        } else {
            posX = endX;
            lastCol = false;
        }
        input += count;
        if constexpr (hasWide) {
            widths += count;
        }
        size -= count;
    }
}

void VtermImpl::inp_LF() {
    if (esc_IND()) {
        eraseRangeInRow(posY, posX, nColsEff - posX);
    }
}

void VtermImpl::inp_CR() {
    if (originMode == OriginMode::Absolute && posX < hMargin) {
        posX = 0;
    } else {
        posX = hMargin;
    }
    lastCol = false;
}

void VtermImpl::jumpToNextTabStop() {
    const u16 previous = posX;
    const bool insideMargins = isCursorInsideMargins();
    const u16 left = insideMargins ? hMargin : 0;
    const u16 right = insideMargins ? nColsEff : pane_.columns;
    if (!tabStopsCustomized) {
        do {
            posX = ((posX / 8) + 1) * 8;
        } while (posX < left);
        posX = min<int>(posX, right - 1);
    } else {
        const size_t ts = tabUpperBound(tabStops, posX);
        posX = ts == tabStops.length() || tabStops[ts] >= right ? right - 1 : tabStops[ts];
    }
    if (posX != previous) {
        lastCol = false;
    }
}

void VtermImpl::inp_HT() {
    if (moreFixMode && lastCol && autoWrapMode) {
        esc_IND();
        posX = 0;
        lastCol = false;
    }
    jumpToNextTabStop();
}

void VtermImpl::showCursor() {
    cursorTemporarilyHidden = false;
}

void VtermImpl::hideCursor() {
    cursorTemporarilyHidden = true;
}

bool VtermImpl::esc_IND() {
    const bool scrolled = performIndex();
    return scrolled;
}

bool VtermImpl::performIndex() {
    bool scrolled = false;
    if (posY == marginBottom - 1) {
        if (posX >= hMargin && posX < nColsEff) {
            scrollRegionUp(1);
            scrolled = true;
        }
    } else if (posY < pane_.rows - 1) {
        ++posY;
        lastCol = false;
    }
    if (semanticUntilEndOfLine) {
        currentSemantic = 0;
        semanticUntilEndOfLine = false;
    } else if (currentSemantic == 1 || currentSemantic == 2) {
        cf->setSemanticPrompt(posY, ScreenSemanticPrompt::Continuation);
    }
    return scrolled;
}

void VtermImpl::resetLeds() {
    ledState = 0;
}

void VtermImpl::setLed(u8 index, bool enabled) {
    const u8 bit = 1 << index;
    if (enabled) {
        ledState |= bit;
    } else {
        ledState &= ~bit;
    }
}

void VtermImpl::commitLeds() {
    recordLeds(ledState);
}

void VtermImpl::sgrReset() {
    resetAttrs();
}

void VtermImpl::sgrBold(bool enabled) {
    attrs.bold = enabled;
    if (attrForeground().source() != CellColor::Source::Direct) {
        setFgFromPalIx();
    }
}

void VtermImpl::sgrFaint(bool enabled) {
    attrs.faint = enabled;
}

void VtermImpl::sgrItalic(bool enabled) {
    attrs.italic = enabled;
}

void VtermImpl::sgrUnderline(u8 style) {
    attrs.underline_style = style;
}

void VtermImpl::sgrBlink(bool enabled) {
    attrs.blink = enabled;
}

void VtermImpl::sgrInverse(bool enabled) {
    reverseVideo = enabled;
    attrs.inverse = enabled;
}

void VtermImpl::sgrConceal(bool enabled) {
    attrs.conceal = enabled;
}

void VtermImpl::sgrStrike(bool enabled) {
    attrs.strike = enabled;
}

void VtermImpl::sgrOverline(bool enabled) {
    attrs.overline = enabled;
}

void VtermImpl::sgrForeground(CellColor color, int paletteIndex, bool brightenBold) {
    fgPalIx = paletteIndex;
    setAttrForeground(color);
    if (brightenBold && config().boldColors && attrs.bold && paletteIndex >= 0 && paletteIndex <= 7) {
        attrs.setForeground(CellColor::indexed(paletteIndex + 8));
    }
    if (underlineColorDefault) {
        setAttrUnderlineColor(attrForeground());
    }
}

void VtermImpl::sgrDefaultForeground() {
    fgPalIx = defaultFgPalIx;
    setFgFromPalIx();
}

void VtermImpl::sgrBackground(CellColor color, int paletteIndex) {
    bgPalIx = paletteIndex;
    setAttrBackground(color);
}

void VtermImpl::sgrDefaultBackground() {
    bgPalIx = defaultBgPalIx;
    setBgFromPalIx();
}

void VtermImpl::sgrUnderlineColor(CellColor color, int paletteIndex) {
    underlinePalIx = paletteIndex;
    setAttrUnderlineColor(color);
    underlineColorDefault = false;
}

void VtermImpl::sgrDefaultUnderlineColor() {
    underlineColorDefault = true;
    setAttrUnderlineColor(attrForeground());
}

void VtermImpl::csi_XTPUSHSGR(const u32* attributes, size_t count) {
    u32 valid = 1;
    if (count != 0) {
        valid = 0;
        for (size_t index = 0; index < count; ++index) {
            const u32 attribute = attributes[index];
            if (attribute > 0 && attribute <= 31) {
                valid |= (u32)(1) << attribute;
            }
        }
    }

    sgrStack[sgrStackNext] = {
        attrs,
        fgPalIx,
        bgPalIx,
        underlinePalIx,
        valid,
        underlineColorDefault,
    };
    sgrStackNext = (sgrStackNext + 1) % (sizeof(sgrStack) / sizeof(sgrStack[0]));
    if (sgrStackCount < sizeof(sgrStack) / sizeof(sgrStack[0])) {
        ++sgrStackCount;
    }
}

void VtermImpl::csi_XTPOPSGR() {
    if (sgrStackCount == 0) {
        return;
    }

    constexpr size_t capacity = sizeof(sgrStack) / sizeof(sgrStack[0]);
    sgrStackNext = (sgrStackNext + capacity - 1) % capacity;
    --sgrStackCount;
    const SavedSgr& saved = sgrStack[sgrStackNext];
    const u32 valid = saved.valid;
    if (valid & 1) {
        attrs = saved.attrs;
        fgPalIx = saved.fgPalIx;
        bgPalIx = saved.bgPalIx;
        underlinePalIx = saved.underlinePalIx;
        underlineColorDefault = saved.underlineColorDefault;
        reverseVideo = attrs.inverse;
        setAttrForeground(saved.attrs.foreground());
        setAttrBackground(saved.attrs.background());
        setAttrUnderlineColor(saved.attrs.inlineUnderlineColor());
        return;
    }

    if (valid & ((u32)(1) << 1)) {
        sgrBold(saved.attrs.bold);
    }
    if (valid & ((u32)(1) << 2)) {
        sgrFaint(saved.attrs.faint);
    }
    if (valid & ((u32)(1) << 3)) {
        sgrItalic(saved.attrs.italic);
    }

    const bool underline = valid & ((u32)(1) << 4);
    const bool doubleUnderline = valid & ((u32)(1) << 21);
    if (underline && doubleUnderline) {
        sgrUnderline(saved.attrs.underline_style);
    } else if (underline) {
        if (saved.attrs.underline_style != 0 && saved.attrs.underline_style != 2) {
            sgrUnderline(saved.attrs.underline_style);
        } else if (attrs.underline_style != 2) {
            sgrUnderline(0);
        }
    } else if (doubleUnderline) {
        if (saved.attrs.underline_style == 2) {
            sgrUnderline(2);
        } else if (attrs.underline_style == 2) {
            sgrUnderline(0);
        }
    }

    if (valid & ((u32)(1) << 5)) {
        sgrBlink(saved.attrs.blink);
    }
    if (valid & ((u32)(1) << 7)) {
        sgrInverse(saved.attrs.inverse);
    }
    if (valid & ((u32)(1) << 8)) {
        sgrConceal(saved.attrs.conceal);
    }
    if (valid & ((u32)(1) << 9)) {
        sgrStrike(saved.attrs.strike);
    }
    if (valid & ((u32)(1) << 30)) {
        fgPalIx = saved.fgPalIx;
        if (fgPalIx >= 0 && fgPalIx <= 255) {
            const int index = config().boldColors && attrs.bold && fgPalIx < 8 ? fgPalIx + 8 : fgPalIx;
            setAttrForeground(CellColor::indexed(index));
        } else {
            setAttrForeground(saved.attrs.foreground());
        }
        if (underlineColorDefault) {
            setAttrUnderlineColor(attrForeground());
        }
    }
    if (valid & ((u32)(1) << 31)) {
        bgPalIx = saved.bgPalIx;
        setAttrBackground(saved.attrs.background());
    }
}

void VtermImpl::sgrFinish() {
    if (underlineColorDefault) {
        setAttrUnderlineColor(reverseVideo ? attrBackground() : attrForeground());
    }
}

void VtermImpl::esc_RI() {
    if (posY == marginTop) {
        if (posX >= hMargin && posX < nColsEff) {
            csi_SD(1);
        }
    } else if (posY > 0) {
        --posY;
        lastCol = false;
    }
}

void VtermImpl::csi_ecma48_SL(u32 count) {
    if (isCursorInsideMargins()) {
        count = min<u32>(count, nColsEff - hMargin);
        deleteCols(hMargin, (u16)(count));
    }
}

void VtermImpl::csi_ecma48_SR(u32 count) {
    if (isCursorInsideMargins()) {
        count = min<u32>(count, nColsEff - hMargin);
        insertCols(hMargin, (u16)(count));
    }
}

void VtermImpl::setCursorStyle(u8 reportStyle, TerminalCursor::Style shape, bool blink) {
    cursorStyleParam = reportStyle;
    cursorShape = shape;
    cursorBlinkMode = blink;
    changePresentation();
    refreshCursorStyle();
}

void VtermImpl::refreshCursorStyle() {
    blinkVisible = true;
    nextBlink = monotonicNowUs() + 500'000;
    wakeTimers();
}

void VtermImpl::csi_DECIC(u32 count) {
    if (isCursorInsideMargins()) {
        count = min<u32>(count, nColsEff - posX);
        insertCols(posX, (u16)(count));
    }
}

void VtermImpl::csi_DECDC(u32 count) {
    if (isCursorInsideMargins()) {
        count = min<u32>(count, nColsEff - posX);
        deleteCols(posX, (u16)(count));
    }
}

void VtermImpl::esc_FI() {
    if (posX >= hMargin && posX == nColsEff - 1) {
        deleteCols(hMargin, 1);
        lastCol = false;
    } else if (posX < pane_.columns - 1) {
        ++posX;
        lastCol = false;
    }
}

void VtermImpl::esc_BI() {
    if (posX == hMargin && posX < nColsEff) {
        insertCols(hMargin, 1);
        lastCol = false;
    } else if (posX > 0) {
        --posX;
        lastCol = false;
    }
}

void VtermImpl::esc_NEL() {
    esc_IND();
    inp_CR();
}

void VtermImpl::esc_HTS() {
    if (!tabStopsCustomized) {
        for (unsigned column = 8; column < pane_.columns; column += 8) {
            tabStops.pushBack((u16)(column));
        }
        tabStopsCustomized = true;
    }
    if (!tabContains(tabStops, posX)) {
        tabInsertAt(tabStops, tabLowerBound(tabStops, posX), posX);
    }
}

void VtermImpl::esc_SPA() {
    isoProtectionActive = true;
    attrs.protected_char |= TerminalCell::isoProtection;
}

void VtermImpl::esc_EPA() {
    attrs.protected_char &= ~TerminalCell::isoProtection;
}

bool VtermImpl::horizontalMarginMode() const {
    return horizMarginMode;
}

void VtermImpl::csi_SCOSC() {
    esc_DECSC();
}

void VtermImpl::csi_SCORC() {
    esc_DECRC();
}

void VtermImpl::esc_DECSC() {
    if (originMode == OriginMode::ScrollingRegion) {
        savedCursor->posX = posX - hMargin;
        savedCursor->posY = posY - marginTop;
    } else {
        savedCursor->posX = posX;
        savedCursor->posY = posY;
    }
    savedCursor->lastCol = lastCol;
    savedCursor->attrs = attrs;
    savedCursor->eraseAttrs = eraseAttrs;
    savedCursor->originMode = originMode;
    savedCursor->charsetState = charsetState;
    // Palette indices travel with the resolved colors: bold-color
    // remapping and DECRQSS re-resolve from them.
    savedCursor->fgPalIx = fgPalIx;
    savedCursor->bgPalIx = bgPalIx;
    savedCursor->underlinePalIx = underlinePalIx;
    savedCursor->underlineColorDefault = underlineColorDefault;
    savedCursor->isSet = true;
}

void VtermImpl::esc_DECRC() {
    if (!savedCursor->isSet) {
        // xterm: DECRC without a preceding save restores the initial
        // state — home, default rendition, default charsets, absolute
        // origin — matching the post-DECSTR saved cursor.
        posX = 0;
        posY = 0;
        lastCol = false;
        originMode = OriginMode::Absolute;
        charsetState = CharsetState{};
        resetAttrs();
        attrs.protected_char = 0;
        eraseAttrs.protected_char = 0;
        return;
    }
    if (savedCursor->isSet) {
        originMode = savedCursor->originMode;
        if (originMode == OriginMode::ScrollingRegion) {
            posX = hMargin + min<u16>(savedCursor->posX, nColsEff - hMargin - 1);
            posY = marginTop + min<u16>(savedCursor->posY, marginBottom - marginTop - 1);
        } else {
            posX = min<u16>(savedCursor->posX, pane_.columns - 1);
            posY = min<u16>(savedCursor->posY, pane_.rows - 1);
        }
        lastCol = savedCursor->lastCol;
        attrs = savedCursor->attrs;
        eraseAttrs = savedCursor->eraseAttrs;
        reverseVideo = attrs.inverse;
        charsetState = savedCursor->charsetState;
        fgPalIx = savedCursor->fgPalIx;
        bgPalIx = savedCursor->bgPalIx;
        underlinePalIx = savedCursor->underlinePalIx;
        underlineColorDefault = savedCursor->underlineColorDefault;
    }
}

void VtermImpl::csi_CUU(u32 count) {
    const u16 top = posY >= marginTop ? marginTop : 0;
    count = min<u32>(count, posY - top);
    posY -= count;
    lastCol = false;
}

void VtermImpl::csi_CUD(u32 count) {
    const u16 bottom = posY < marginBottom ? marginBottom : pane_.rows;
    count = min<u32>(count, bottom - posY - 1);
    posY += count;
    lastCol = false;
}

void VtermImpl::csi_CUF(u32 count) {
    const bool insideMargins = posX >= hMargin && posX < nColsEff;
    const u16 right = insideMargins ? nColsEff : pane_.columns;
    count = min<u32>(count, right - posX - 1);
    posX += count;
    lastCol = false;
}

void VtermImpl::csi_CUB(u32 count) {
    moveCursorBackward(count);
}

void VtermImpl::moveCursorBackward(u32 count) {
    const bool insideMargins = posX >= hMargin && posX < nColsEff;
    const bool canReverseWrap = autoWrapMode && (reverseWrapMode || extendedReverseWrapMode);
    bool findCycle = extendedReverseWrapMode && (u64)count > (u64)pane_.columns * pane_.rows;
    bool cycleStarted = false;
    u16 cycleRow = 0;
    u32 cycleRemaining = 0;
    if (count && lastCol && canReverseWrap) {
        lastCol = false;
        if (--count == 0) {
            return;
        }
    }
    while (count > 0) {
        const u16 leftEdge = insideMargins ? hMargin : 0;
        const u16 left = posX >= leftEdge ? posX - leftEdge : posX;
        if (left > 0) {
            const u32 step = min<u32>(count, left);
            posX -= step;
            count -= step;
            continue;
        }
        if (!canReverseWrap) {
            break;
        }
        if (findCycle) {
            if (!cycleStarted) {
                cycleStarted = true;
                cycleRow = posY;
                cycleRemaining = count;
            } else if (posY == cycleRow) {
                const u32 cycleLength = cycleRemaining - count;
                count %= cycleLength;
                findCycle = false;
                if (count == 0) {
                    break;
                }
            }
        }
        const u16 rightEdge = (insideMargins ? nColsEff : pane_.columns) - 1;
        if (extendedReverseWrapMode && posY == marginTop) {
            posY = marginBottom - 1;
            posX = rightEdge;
            --count;
            continue;
        }
        if (posY == 0) {
            break;
        }
        u16 wrapColumn = rightEdge;
        bool wrapped = false;
        for (u16 column = rightEdge;; --column) {
            if (cf->wrapped(posY - 1, column)) {
                wrapColumn = column;
                wrapped = true;
                break;
            }
            if (column == leftEdge) {
                break;
            }
        }
        if (!extendedReverseWrapMode && !wrapped) {
            break;
        }
        --posY;
        posX = wrapped ? wrapColumn : rightEdge;
        --count;
    }
    lastCol = false;
}

void VtermImpl::csi_CNL(u32 count) {
    csi_CUD(count);
    inp_CR();
}

void VtermImpl::csi_CPL(u32 count) {
    csi_CUU(count);
    inp_CR();
}

void VtermImpl::csi_CHA(u32 column) {
    if (originMode == OriginMode::ScrollingRegion) {
        column = max<u32>(1, min<u32>(column, nColsEff - hMargin));
        posX = hMargin + column - 1;
    } else {
        column = max<u32>(1, min<u32>(column, pane_.columns));
        posX = column - 1;
    }
    lastCol = false;
}

void VtermImpl::csi_HPA(u32 column) {
    csi_CHA(column);
}

void VtermImpl::csi_HPR(u32 count) {
    const u16 right = originMode == OriginMode::ScrollingRegion ? nColsEff : pane_.columns;
    posX = (u16)(min<u64>((u64)(posX) + count, right - 1));
    lastCol = false;
}

void VtermImpl::csi_VPA(u32 row) {
    if (originMode == OriginMode::ScrollingRegion) {
        row = max<u32>(1, min<u32>(row, marginBottom - marginTop));
        posY = marginTop + row - 1;
    } else {
        row = max<u32>(1, min<u32>(row, pane_.rows));
        posY = row - 1;
    }
    lastCol = false;
}

void VtermImpl::csi_VPR(u32 count) {
    const u16 bottom = originMode == OriginMode::ScrollingRegion ? marginBottom : pane_.rows;
    posY = (u16)(min<u64>((u64)(posY) + count, bottom - 1));
    lastCol = false;
}

void VtermImpl::csi_CUP(u32 row, u32 column) {
    switch (originMode) {
        case OriginMode::Absolute:
            row = max<u32>(1, min<u32>(row, pane_.rows)) - 1;
            column = max<u32>(1, min<u32>(column, pane_.columns)) - 1;
            break;
        case OriginMode::ScrollingRegion:
            row = marginTop + max<u32>(1, min<u32>(row, marginBottom - marginTop)) - 1;
            column = hMargin + max<u32>(1, min<u32>(column, nColsEff - hMargin)) - 1;
            break;
    }

    posX = column;
    posY = row;
    lastCol = false;
}

void VtermImpl::csi_SU(u32 count) {
    count = min<u32>(count, marginBottom - marginTop);
    const bool pendingWrap = lastCol;
    scrollRegionUp((u16)(count));
    lastCol = pendingWrap;
}

void VtermImpl::scrollRegionUp(u16 count) {
    if (horizMarginMode) {
        deleteRows(marginTop, count);
    } else {
        cf->scrollRows(marginTop, marginBottom, -(i32)(count), eraseAttrs);
        lastCol = false;
    }
}

void VtermImpl::scrollRegionDown(u16 count) {
    if (horizMarginMode) {
        insertRows(marginTop, count);
    } else {
        cf->scrollRows(marginTop, marginBottom, count, eraseAttrs);
        lastCol = false;
    }
}

void VtermImpl::csi_SD(u32 count) {
    count = min<u32>(count, marginBottom - marginTop);
    const bool pendingWrap = lastCol;
    scrollRegionDown((u16)(count));
    lastCol = pendingWrap;
}

void VtermImpl::csi_CHT(u32 count) {
    count = min<u32>(count, pane_.columns);
    if (count == 1) {
        inp_HT();
    } else {
        for (u32 k = 0; k < count; ++k) {
            jumpToNextTabStop();
        }
    }
}

void VtermImpl::csi_CBT(u32 count) {
    count = min<u32>(count, pane_.columns);
    for (u32 k = 0; k < count; ++k) {
        const u16 left = originMode == OriginMode::ScrollingRegion ? hMargin : 0;
        if (!tabStopsCustomized) {
            if (posX > 0 && posX % 8 == 0) {
                posX -= 8;
            } else {
                posX = (posX / 8) * 8;
            }
            posX = max(posX, left);
        } else {
            const size_t ts = tabLowerBound(tabStops, posX);
            if (ts == 0 || tabStops[ts - 1] < left) {
                posX = left;
            } else {
                posX = tabStops[ts - 1];
            }
        }
        lastCol = false;
    }
}

void VtermImpl::csi_REP(u32 count) {
    const u32 preceding = utf8dec.getUnicode();
    if (!preceding) {
        return;
    }
    const u8 data = codepointData(preceding);
    const u8 width = data & 0x03;
    if (width == 0) {
        return;
    }
    const u64 observableCells = ((u64)(config().saveLines) + pane_.rows + 1) * pane_.columns;
    if (count > observableCells) {
        count = (u32)(observableCells + (count - observableCells) % pane_.columns);
    }
    if ((data & 0x04) == 0 || insertMode) {
        for (u32 k = 0; k < count; ++k) {
            placeGraphicChar();
        }
        return;
    }

    if (inputGraphemeScreen != cf) {
        inputGraphemeBreaker.reset();
    }
    if (!inputGraphemeBreaker.breakBefore(preceding, true)) {
        placeGraphicChar(false, width);
        if (--count == 0) {
            return;
        }
        inputGraphemeBreaker.breakBefore(preceding, true);
    }

    if (width == 1) {
        placeRepeatedCodepoint(preceding, count);
        inputGraphemeBreaker.setBoundaryAfter(preceding);
        return;
    }

    constexpr u16 batchLimit = 64;
    u32 codepoints[batchLimit];
    u8 widths[batchLimit];
    for (u16 index = 0; index < batchLimit; ++index) {
        codepoints[index] = preceding;
        widths[index] = width;
    }
    while (count != 0) {
        const u16 batch = min<u32>(count, batchLimit);
        placePreparedRun<true>(codepoints, widths, batch);
        count -= batch;
    }
    inputGraphemeBreaker.setBoundaryAfter(preceding);
}

void VtermImpl::eraseDisplayAfter() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, posX, pane_.columns - posX);
    for (u16 row = posY + 1; row < pane_.rows; ++row) {
        eraseEcmaRow(row);
    }
}

void VtermImpl::eraseDisplayBefore() {
    normalizeCursorPos();
    for (u16 row = 0; row < posY; ++row) {
        eraseEcmaRow(row);
    }
    eraseEcmaRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::eraseDisplayAll() {
    normalizeCursorPos();
    for (u16 row = 0; row < pane_.rows; ++row) {
        eraseEcmaRow(row);
    }
}

void VtermImpl::eraseScrollback() {
    cf->dropHistory();
}

void VtermImpl::eraseLineAfter() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, posX, pane_.columns - posX);
}

void VtermImpl::eraseLineBefore() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::eraseLineAll() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, 0, pane_.columns);
}

void VtermImpl::selectiveEraseDisplayAfter() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, posX, pane_.columns - posX);
    for (u16 row = posY + 1; row < pane_.rows; ++row) {
        selectiveEraseRangeInRow(row, 0, pane_.columns);
    }
}

void VtermImpl::selectiveEraseDisplayBefore() {
    normalizeCursorPos();
    for (u16 row = 0; row < posY; ++row) {
        selectiveEraseRangeInRow(row, 0, pane_.columns);
    }
    selectiveEraseRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::selectiveEraseDisplayAll() {
    normalizeCursorPos();
    for (u16 row = 0; row < pane_.rows; ++row) {
        selectiveEraseRangeInRow(row, 0, pane_.columns);
    }
}

void VtermImpl::selectiveEraseLineAfter() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, posX, pane_.columns - posX);
}

void VtermImpl::selectiveEraseLineBefore() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::selectiveEraseLineAll() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, 0, pane_.columns);
}

void VtermImpl::setDecProtection(bool enabled) {
    if (enabled) {
        attrs.protected_char |= TerminalCell::decProtection;
    } else {
        attrs.protected_char &= ~TerminalCell::decProtection;
    }
}

void VtermImpl::csi_DECFRA(u32 codepoint, CsiRectangle parameters) {
    if (codepoint >= 32 && codepoint <= 0x10ffff) {
        Rectangle rectangle;
        if (!rectangleFromParams(parameters, rectangle)) {
            return;
        }
        cf->fillRectangle(rectangle.top, rectangle.left, rectangle.bottom, rectangle.right, codepoint, attrs, eraseAttrs);
    }
}

void VtermImpl::csi_DECERA(CsiRectangle parameters, bool selective) {
    Rectangle rectangle;
    if (!rectangleFromParams(parameters, rectangle)) {
        return;
    }
    for (u16 y = rectangle.top; y < rectangle.bottom; ++y) {
        if (selective) {
            selectiveEraseRangeInRow(y, rectangle.left, rectangle.right - rectangle.left);
        } else {
            eraseRangeInRow(y, rectangle.left, rectangle.right - rectangle.left);
        }
    }
}

void VtermImpl::csi_DECCRA(CsiRectangle parameters, u32 targetRow, u32 targetColumn) {
    Rectangle source;
    if (!rectangleFromParams(parameters, source)) {
        return;
    }
    u16 rowBase, columnBase, rowLimit, columnLimit;
    rectangleOrigin(rowBase, columnBase, rowLimit, columnLimit);
    const u16 targetTop = rowBase + min<u32>(targetRow, rowLimit - rowBase) - 1;
    const u16 targetLeft = columnBase + min<u32>(targetColumn, columnLimit - columnBase) - 1;
    const u16 height = min<u16>(source.bottom - source.top, rowLimit - targetTop);
    const u16 width = min<u16>(source.right - source.left, columnLimit - targetLeft);
    cf->copyRectangle(source.top, source.left, targetTop, targetLeft, height, width, eraseAttrs);
}

void VtermImpl::changeRectangleAttributes(CsiRectangle parameters, CellAttributeChange change) {
    Rectangle rectangle;
    if (!rectangleFromParams(parameters, rectangle)) {
        return;
    }
    if (rectangularAttributeExtent || rectangle.bottom == rectangle.top + 1) {
        cf->changeRectangleAttributes(rectangle.top, rectangle.left, rectangle.bottom, rectangle.right, change);
    } else {
        u16 rowBase, columnBase, rowLimit, columnLimit;
        rectangleOrigin(rowBase, columnBase, rowLimit, columnLimit);
        cf->changeRectangleAttributes(rectangle.top, rectangle.left, rectangle.top + 1, columnLimit, change);
        for (u16 row = rectangle.top + 1; row + 1 < rectangle.bottom; ++row) {
            cf->changeRectangleAttributes(row, columnBase, row + 1, columnLimit, change);
        }
        cf->changeRectangleAttributes(rectangle.bottom - 1, columnBase, rectangle.bottom, rectangle.right, change);
    }
    refreshBlinkingText();
}

void VtermImpl::setAttributeChangeExtent(bool rectangular) {
    rectangularAttributeExtent = rectangular;
}

void VtermImpl::csi_XTCHECKSUM(u32 flags) {
    checksumFlags = flags & 0x1f;
}

void VtermImpl::csi_DECRQCRA(u32 requestId, CsiRectangle parameters) {
    Rectangle rectangle;
    if (!rectangleFromParams(parameters, rectangle)) {
        return;
    }
    const u16 checksum = cf->checksum(rectangle.top, rectangle.left, rectangle.bottom, rectangle.right, checksumFlags);
    StringBuilder response;
    response << requestId << StringView(u8"!~") << Hex{checksum, 4, true};
    writeDcsResponse(StringView(response));
}

void VtermImpl::csi_IL(u32 count) {
    if (isCursorInsideMargins()) {
        count = min<u32>(count, marginBottom - posY);
        insertRows(posY, (u16)(count));
        inp_CR();
    }
}

void VtermImpl::csi_DL(u32 count) {
    if (isCursorInsideMargins()) {
        count = min<u32>(count, marginBottom - posY);
        deleteRows(posY, (u16)(count));
        inp_CR();
    }
}

void VtermImpl::csi_ICH(u32 count) {
    if (posX >= hMargin && posX < nColsEff) {
        count = min<u32>(count, nColsEff - posX);
        cf->insertCells(posY, posX, nColsEff, (u16)(count), eraseAttrs);
    }
    lastCol = false;
}

void VtermImpl::csi_DCH(u32 count) {
    if (posX >= hMargin && posX < nColsEff) {
        count = min<u32>(count, nColsEff - posX);
        cf->deleteCells(posY, posX, nColsEff, (u16)(count), eraseAttrs);
    }
    lastCol = false;
}

void VtermImpl::csi_ECH(u32 count) {
    const u32 len = pane_.columns - posX;
    count = min(count, len);
    eraseEcmaRangeInRow(posY, posX, count);
    lastCol = false;
}

void VtermImpl::csi_STBM(u32 top, u32 bottom, bool valid) {
    const u32 newMarginTop = top > 0 ? top - 1 : 0;
    const u32 newMarginBottom = bottom == 0 ? pane_.rows : min<u32>(bottom, pane_.rows);
    const bool illegal = newMarginTop >= pane_.rows || newMarginBottom <= newMarginTop + 1;
    if (!valid || illegal) {
        // A rejected region is a complete no-op, the cursor stays.
        return;
    }
    marginTop = (u16)(newMarginTop);
    marginBottom = (u16)(newMarginBottom);

    if (originMode == OriginMode::Absolute) {
        posX = 0;
        posY = 0;
    } else {
        posX = hMargin;
        posY = marginTop;
    }
    lastCol = false;
}

void VtermImpl::csi_SLRM(u32 left, u32 right, bool valid) {
    const u32 newMarginLeft = left > 0 ? left - 1 : 0;
    const u32 newMarginRight = right == 0 ? pane_.columns : right;
    const bool illegal = newMarginLeft >= pane_.columns || newMarginRight > pane_.columns || newMarginRight <= newMarginLeft + 1;
    if (!valid || illegal) {
        // xterm: a rejected region is a complete no-op, the cursor stays.
        return;
    }
    hMargin = (u16)(newMarginLeft);
    nColsEff = (u16)(newMarginRight);

    if (originMode == OriginMode::Absolute) {
        posX = 0;
        posY = 0;
    } else {
        posX = hMargin;
        posY = marginTop;
    }
    lastCol = false;
}

void VtermImpl::clearTabStop() {
    if (!tabStopsCustomized) {
        for (unsigned column = 8; column < pane_.columns; column += 8) {
            tabStops.pushBack((u16)(column));
        }
        tabStopsCustomized = true;
    }
    const size_t it = tabLowerBound(tabStops, posX);
    if (it < tabStops.length() && tabStops[it] == posX) {
        tabEraseAt(tabStops, it);
    }
}

void VtermImpl::clearAllTabStops() {
    tabStops.clear();
    tabStopsCustomized = true;
    tabStopsRestored = false;
}

void VtermImpl::resetTabStops() {
    tabStops.clear();
    tabStopsCustomized = false;
    tabStopsRestored = false;
}

void VtermImpl::setKeyboardLocked(bool enabled) {
    keyboardLocked = enabled;
}

void VtermImpl::setInsertMode(bool enabled) {
    insertMode = enabled;
}

void VtermImpl::setEraseModeAll(bool enabled) {
    eraseModeAll = enabled;
}

void VtermImpl::setLocalEcho(bool enabled) {
    localEcho = enabled;
}

void VtermImpl::setAutoNewline(bool enabled) {
    autoNewlineMode = enabled;
}

ParserModeState VtermImpl::parserModeState() const {
    ParserModeState result;
    result.mouseTracking = mouseTrk.mode;
    result.mouseEncoding = mouseTrk.enc;
    result.keyboardLocked = keyboardLocked;
    result.insertMode = insertMode;
    result.eraseModeAll = eraseModeAll;
    result.localEcho = localEcho;
    result.autoNewline = autoNewlineMode;
    result.ansiMode = compatLevel != CompatibilityLevel::VT52;
    result.applicationCursorKeys = cursorKeyMode == CursorKeyMode::Application;
    result.column132 = colMode == ColMode::C132;
    result.smoothScroll = smoothScrollMode;
    result.screenReverseVideo = screenReverseVideo;
    result.originMode = originMode == OriginMode::ScrollingRegion;
    result.autoWrap = autoWrapMode;
    result.autoRepeat = autoRepeatMode;
    result.cursorBlink = cursorBlinkMode;
    result.allowColumnMode = allowColumnMode;
    result.moreFix = moreFixMode;
    result.nationalReplacement = nationalReplacementMode;
    result.reverseWrap = reverseWrapMode;
    result.showCursor = showCursorMode;
    result.alternateScreen = altScreenBufferMode;
    result.applicationKeypad = keypadMode == KeypadMode::Application;
    result.backspaceSendsBackspace = !bkspSendsDel;
    result.horizontalMargins = horizMarginMode;
    result.noClearColumn = noClearColumnMode;
    result.focusEvents = mouseTrk.focusEventMode;
    result.alternateScroll = altScrollMode;
    result.eightBitInput = eightBitInput;
    result.altSendsEscape = altSendsEscape;
    result.extendedReverseWrap = extendedReverseWrapMode;
    result.bracketedPaste = bracketedPasteMode;
    result.synchronizedOutput = synchronizedOutputMode;
    result.graphemeCluster = graphemeClusterMode;
    result.colorSchemeUpdates = colorSchemeUpdateMode;
    result.inBandResize = inBandResizeMode;
    result.pasteMimeNotifications = pasteMimeNotificationsMode;
    result.savedCursor = savedCursor->isSet;
    return result;
}

void VtermImpl::setAnsiMode(bool enabled) {
    charsetState = CharsetState{};
    compatLevel = enabled ? CompatibilityLevel::VT400 : CompatibilityLevel::VT52;
}

void VtermImpl::setApplicationCursorKeys(bool enabled) {
    cursorKeyMode = enabled ? CursorKeyMode::Application : CursorKeyMode::ANSI;
}

void VtermImpl::setColumn132(bool enabled) {
    if (allowColumnMode) {
        switchColMode(enabled ? ColMode::C132 : ColMode::C80, true);
    }
}

void VtermImpl::setSmoothScroll(bool enabled) {
    smoothScrollMode = enabled;
}

void VtermImpl::setScreenReverseVideo(bool enabled) {
    if (screenReverseVideo == enabled) {
        return;
    }
    screenReverseVideo = enabled;
    changePresentation();
}

void VtermImpl::setOriginMode(bool enabled) {
    originMode = enabled ? OriginMode::ScrollingRegion : OriginMode::Absolute;
    posX = enabled ? hMargin : 0;
    posY = enabled ? marginTop : 0;
    lastCol = false;
}

void VtermImpl::setAutoWrap(bool enabled) {
    autoWrapMode = enabled;
    lastCol = false;
}

void VtermImpl::setAutoRepeat(bool enabled) {
    autoRepeatMode = enabled;
}

void VtermImpl::setAllowColumnMode(bool enabled) {
    allowColumnMode = enabled;
}

void VtermImpl::setMoreFix(bool enabled) {
    moreFixMode = enabled;
}

void VtermImpl::setNationalReplacement(bool enabled) {
    nationalReplacementMode = enabled;
}

void VtermImpl::setReverseWrap(bool enabled) {
    reverseWrapMode = enabled;
}

void VtermImpl::setMouseTracking(MouseTrackingMode mode) {
    mouseTrk.setMode(mode);
    if (mode == MouseTrackingMode::VT200_Highlight || mode == MouseTrackingMode::Disabled) {
        mouseHighlight.active = false;
    }
}

void VtermImpl::setCursorBlink(bool enabled) {
    if (cursorBlinkMode != enabled) {
        cursorBlinkMode = enabled;
        changePresentation();
    }
    if (!enabled) {
        blinkVisible = true;
    }
    nextBlink = monotonicNowUs() + 500'000;
    wakeTimers();
}

void VtermImpl::setCursorVisible(bool enabled) {
    if (showCursorMode == enabled) {
        return;
    }
    showCursorMode = enabled;
    changePresentation();
}

void VtermImpl::setAlternateScreen(bool enabled, bool clear) {
    switchScreenBufferMode(enabled, clear);
}

void VtermImpl::setBackspaceSendsBackspace(bool enabled) {
    bkspSendsDel = !enabled;
}

void VtermImpl::setHorizontalMargins(bool enabled) {
    if (compatLevel >= CompatibilityLevel::VT400) {
        horizMarginMode = enabled;
        hMargin = 0;
        nColsEff = pane_.columns;
    }
}

void VtermImpl::setNoClearColumn(bool enabled) {
    if (compatLevel >= CompatibilityLevel::VT500) {
        noClearColumnMode = enabled;
    }
}

void VtermImpl::setFocusEvents(bool enabled) {
    mouseTrk.focusEventMode = enabled;
}

void VtermImpl::setMouseEncoding(MouseTrackingEnc encoding, bool enabled) {
    if (enabled) {
        mouseTrk.setEncoding(encoding);
    } else if (mouseTrk.enc == encoding) {
        mouseTrk.setEncoding(MouseTrackingEnc::Default);
    }
}

void VtermImpl::setAlternateScroll(bool enabled) {
    altScrollMode = enabled;
}

void VtermImpl::setEightBitInput(bool enabled) {
    eightBitInput = enabled;
}

void VtermImpl::setAltSendsEscape(bool enabled) {
    altSendsEscape = enabled;
}

void VtermImpl::setSavedAlternateScreen(bool enabled) {
    if (enabled) {
        esc_DECSC();
        switchScreenBufferMode(true, true);
    } else {
        savedCursor = &savedCursorPri;
        esc_DECRC();
        switchScreenBufferMode(false, true);
    }
}

void VtermImpl::setExtendedReverseWrap(bool enabled) {
    extendedReverseWrapMode = enabled;
}

void VtermImpl::setBracketedPaste(bool enabled) {
    bracketedPasteMode = enabled;
}

void VtermImpl::setSynchronizedOutput(bool enabled) {
    synchronizedOutputMode = enabled;
    if (enabled) {
        synchronizedOutputDeadline = monotonicNowUs() + 150'000;
    }
    wakeTimers();
}

void VtermImpl::setGraphemeCluster(bool enabled) {
    graphemeClusterMode = enabled;
}

void VtermImpl::setColorSchemeUpdates(bool enabled) {
    colorSchemeUpdateMode = enabled;
}

void VtermImpl::setInBandResize(bool enabled) {
    inBandResizeMode = enabled;
    if (enabled) {
        reportInBandResize();
    }
}

void VtermImpl::setPasteMimeNotifications(bool enabled) {
    pasteMimeNotificationsMode = enabled;
}

void VtermImpl::savePrivateMode(u32 mode, bool enabled) {
    SavedMode& saved = savedPrivModes[mode];
    saved.key = mode;
    saved.enabled = enabled;
}

bool VtermImpl::restorePrivateMode(u32 mode, bool& enabled) const {
    const SavedMode* const saved = savedPrivModes.find(mode);
    if (saved == nullptr) {
        return false;
    }
    enabled = saved->enabled;
    return true;
}

void VtermImpl::setFgFromPalIx() {
    if (fgPalIx < 0) {
        setAttrForeground(CellColor::defaultForeground());
    } else if (fgPalIx > 255) {
        return;
    } else {
        setAttrForeground(CellColor::indexed(fgPalIx));
    }
    if (config().boldColors && attrs.bold && fgPalIx >= 0 && fgPalIx <= 7) {
        attrs.setForeground(CellColor::indexed(fgPalIx + 8));
    }
    if (underlineColorDefault) {
        setAttrUnderlineColor(attrForeground());
    }
}

void VtermImpl::setBgFromPalIx() {
    if (bgPalIx < 0) {
        setAttrBackground(CellColor::defaultBackground());
    } else if (bgPalIx > 255) {
        return;
    } else {
        setAttrBackground(CellColor::indexed(bgPalIx));
    }
}

/* 64 - VT420 family
    *  9 - National Replacement Character-sets
    * 15 - DEC technical set
    * 21 - horizontal scrolling
    * 22 - color
    */
#define DEVICE_ID "64;1;2;4;6;8;9;15;21;22;28;29c"

void VtermImpl::csi_priDA() {
    writeCsiResponse("?" DEVICE_ID);
}

void VtermImpl::csi_secDA() {
    writeCsiResponse(">41;14;0c");
}

void VtermImpl::csi_terDA() {
    writeDcsResponse("!|00000000");
}

void VtermImpl::csi_DECRQDE() {
    StringBuilder response;
    response << pane_.rows << StringView(u8";") << pane_.columns << StringView(u8";1;1;1\"w");
    writeCsiResponse(StringView(response));
}

void VtermImpl::csi_DECREQTPARM(u32 permission) {
    StringBuilder response;
    response << permission + 2 << StringView(u8";1;1;128;128;1;0x");
    writeCsiResponse(StringView(response));
}

void VtermImpl::csi_XTSMGRAPHICS(u32 item, u32 action, u32 value) {
    (void)value;
    StringBuilder response;
    if (item != 1 && item != 2) {
        response << StringView(u8"?") << item << StringView(u8";1S");
    } else if (action != 1 && action != 4) {
        // Register and geometry limits are fixed; setting is refused.
        response << StringView(u8"?") << item << StringView(u8";2S");
    } else if (item == 1) {
        response << StringView(u8"?1;0;") << (u32)(SixelPatch::paletteEntries) << StringView(u8"S");
    } else {
        response << StringView(u8"?2;0;") << (u32)(pane_.columns) * SixelPatch::width << StringView(u8";") << (u32)(pane_.rows) * SixelPatch::height << StringView(u8"S");
    }
    writeCsiResponse(StringView(response));
}

void VtermImpl::csi_XTVERSION() {
    StringBuilder response;
    response << StringView(u8">|") << config().brandName << StringView(u8" " SHITTY_VERSION);
    writeDcsResponse(StringView(response));
}

void VtermImpl::csi_SETMARK() {
    startSemanticPrompt({});
}

void VtermImpl::reportMode(u32 mode, bool privateMode, u8 state) {
    StringBuilder response;
    if (privateMode) {
        response << StringView(u8"?");
    }
    response << mode << StringView(u8";") << (unsigned)(state) << StringView(u8"$y");
    writeCsiResponse(StringView(response));
}

void VtermImpl::dsrOperatingStatus() {
    writeCsiResponse("0n");
}

void VtermImpl::dsrCursorPosition(bool privateMode) {
    StringBuilder response;
    if (privateMode) {
        response << StringView(u8"?");
    }
    if (originMode == OriginMode::Absolute) {
        response << (posY + 1) << StringView(u8";") << (posX + 1);
    } else {
        response << (posY - marginTop + 1) << StringView(u8";") << (posX - hMargin + 1);
    }
    if (privateMode) {
        response << StringView(u8";1R");
    } else {
        response << StringView(u8"R");
    }
    writeCsiResponse(StringView(response));
}

void VtermImpl::dsrPrinter() {
    writeCsiResponse("?13n");
}

void VtermImpl::dsrUserDefinedKeys() {
    writeCsiResponse(userDefinedKeysLocked ? "?21n" : "?20n");
}

void VtermImpl::dsrKeyboard() {
    writeCsiResponse("?27;1;0;0n");
}

void VtermImpl::dsrLocator() {
    writeCsiResponse("?50n");
}

void VtermImpl::dsrLocatorType() {
    writeCsiResponse("?57;1n");
}

void VtermImpl::dsrMacroSpace() {
    writeCsiResponse("0*{");
}

void VtermImpl::dsrMemoryChecksum(u32 requestId) {
    StringBuilder response;
    response << requestId << StringView(u8"!~0000");
    writeDcsResponse(StringView(response));
}

void VtermImpl::dsrDataIntegrity() {
    writeCsiResponse("?70n");
}

void VtermImpl::dsrMultipleSession() {
    writeCsiResponse("?83n");
}

void VtermImpl::dsrColorScheme() {
    reportColorScheme();
}

void VtermImpl::csi_DECRQTSR_COLOR(u32 model) {
    StringBuilder response(8192);
    response << StringView(u8"2$s");
    for (u32 index = 0; index < 256; ++index) {
        if (index != 0) {
            response << StringView(u8"/");
        }
        const Color color = colors.palette[index];
        response << index << StringView(u8";") << model << StringView(u8";");
        if (model == 2) {
            response << (color.red * 100u + 127u) / 255u << StringView(u8";") << (color.green * 100u + 127u) / 255u << StringView(u8";") << (color.blue * 100u + 127u) / 255u;
            continue;
        }

        const u32 red = color.red;
        const u32 green = color.green;
        const u32 blue = color.blue;
        const u32 maximum = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);
        const u32 minimum = red < green ? (red < blue ? red : blue) : (green < blue ? green : blue);
        const u32 chroma = maximum - minimum;
        const u32 sum = maximum + minimum;
        u32 hue = 0;
        if (chroma != 0) {
            float standardHue;
            if (maximum == color.red) {
                standardHue = 60.0f * ((float)(color.green) - color.blue) / chroma;
            } else if (maximum == color.green) {
                standardHue = 120.0f + 60.0f * ((float)(color.blue) - color.red) / chroma;
            } else {
                standardHue = 240.0f + 60.0f * ((float)(color.red) - color.green) / chroma;
            }
            if (standardHue < 0.0f) {
                standardHue += 360.0f;
            }
            hue = ((u32)(standardHue + 120.5f)) % 360;
        }
        const u32 luminosity = (sum * 100u + 255u) / 510u;
        const u32 saturationDenominator = 255u - (u32)(__builtin_abs((int)(sum)-255));
        const u32 saturation = saturationDenominator == 0 ? 0 : (chroma * 100u + saturationDenominator / 2u) / saturationDenominator;
        response << hue << StringView(u8";") << luminosity << StringView(u8";") << saturation;
    }
    writeDcsResponse(StringView(response));
}

void VtermImpl::csi_DECRQPSR_TABS() {
    StringBuilder response;
    response << StringView(u8"2$u");
    bool first = true;
    if (tabStopsCustomized) {
        for (u16 column : tabStops) {
            if (column >= pane_.columns) {
                break;
            }
            if (!first) {
                response << StringView(u8"/");
            }
            response << (u32)(column) + 1;
            first = false;
        }
    } else {
        for (u32 column = 8; column < pane_.columns; column += 8) {
            if (!first) {
                response << StringView(u8"/");
            }
            response << column + 1;
            first = false;
        }
    }
    writeDcsResponse(StringView(response));
}

void VtermImpl::csi_DECRQPSR_CURSOR() {
    StringBuilder response;
    const auto appendByte = [&response](u8 byte) {
        response.append(&byte, 1);
    };
    u8 rendition = 0;
    rendition |= attrs.bold ? 1 : 0;
    rendition |= attrs.underlined() ? 2 : 0;
    rendition |= attrs.blink ? 4 : 0;
    rendition |= attrs.inverse ? 8 : 0;
    rendition |= attrs.conceal ? 16 : 0;
    u8 flags = 0;
    flags |= originMode == OriginMode::ScrollingRegion ? 1 : 0;
    flags |= charsetState.ss == 2 ? 2 : 0;
    flags |= charsetState.ss == 3 ? 4 : 0;
    flags |= lastCol ? 8 : 0;

    response << StringView(u8"1$u") << (u32)(posY) + 1 << StringView(u8";") << (u32)(posX) + 1 << StringView(u8";1;");
    appendByte('@' + rendition);
    response << StringView(u8";");
    appendByte('@' + ((attrs.protected_char & TerminalCell::decProtection) ? 1 : 0));
    response << StringView(u8";");
    appendByte('@' + flags);
    response << StringView(u8";") << (u32)(charsetState.gl) << StringView(u8";") << (u32)(charsetState.gr) << StringView(u8";");
    appendByte('@' + charsetState.size96);
    response << StringView(u8";");
    for (u16 id : charsetState.ids) {
        const u8 bytes[] = {(u8)(id >> 8), (u8)(id)};
        response.append(bytes + (bytes[0] == 0), bytes[0] == 0 ? 1 : 2);
    }
    writeDcsResponse(StringView(response));
}

void VtermImpl::csi_DECRQUPSS() {
    StringBuilder response;
    response << (userPreferenceCharset96 ? StringView(u8"1!u") : StringView(u8"0!u"));
    const u8 bytes[] = {
        (u8)(userPreferenceCharsetId >> 8),
        (u8)(userPreferenceCharsetId),
    };
    response.append(bytes + (bytes[0] == 0), bytes[0] == 0 ? 1 : 2);
    writeDcsResponse(StringView(response));
}

void VtermImpl::esch_DECALN() {
    originMode = OriginMode::Absolute;
    marginTop = 0;
    marginBottom = pane_.rows;
    hMargin = 0;
    nColsEff = pane_.columns;
    posX = 0;
    posY = 0;
    lastCol = false;

    TerminalCell origAttrs = attrs;
    TerminalCell origEraseAttrs = eraseAttrs;

    resetAttrs();
    attrs.protected_char = 0;
    fillScreen('E');

    attrs = origAttrs;
    eraseAttrs = origEraseAttrs;
    reverseVideo = attrs.inverse;
}

void VtermImpl::setLineAttribute(u8 attribute) {
    cf->setLineAttribute(posY, attribute);
    if (attribute) {
        posX = min<u16>(posX, max(1, pane_.columns / 2) - 1);
    }
    lastCol = false;
}

void VtermImpl::esc_RIS() {
    resetTerminal();
}

void VtermImpl::csi_DECSTR() {
    resetScreen(false);
    resetAttrs();
    userPreferenceCharset = Charset::DecSuppl;
    userPreferenceCharsetId = ((u16)('%') << 8) | '5';
    userPreferenceCharset96 = false;
    activeHyperlink = 0;
    horizMarginMode = false;
    marginTop = 0;
    marginBottom = pane_.rows;
    hMargin = 0;
    nColsEff = pane_.columns;
    savedCursor->posX = 0;
    savedCursor->posY = 0;
    savedCursor->lastCol = false;
    savedCursor->attrs = attrs;
    savedCursor->eraseAttrs = eraseAttrs;
    savedCursor->originMode = OriginMode::Absolute;
    savedCursor->charsetState = CharsetState{};
    savedCursor->fgPalIx = fgPalIx;
    savedCursor->bgPalIx = bgPalIx;
    savedCursor->underlinePalIx = underlinePalIx;
    savedCursor->underlineColorDefault = underlineColorDefault;
    savedCursor->isSet = true;
}

void VtermImpl::dcs_DECUDK(bool clearDefinitions, bool lockDefinitions, const ParserUdkDefinition* definitions, size_t definitionCount, StringView values) {
    if (userDefinedKeysLocked) {
        return;
    }
    if (clearDefinitions) {
        clearIntMap(userDefinedKeys);
    }

    for (size_t index = 0; index < definitionCount; ++index) {
        const ParserUdkDefinition& definition = definitions[index];
        const auto* value = (const char*)(values.data()) + definition.valueOffset;
        UserKey& defined = userDefinedKeys[(u64)(definition.key)];
        defined.key = (u64)(definition.key);
        defined.text.reset();
        defined.text.append(value, definition.valueLength);
    }
    userDefinedKeysLocked = lockDefinitions;
}

void VtermImpl::writeDecrqssResponse(StringView value) {
    StringBuilder response;
    response << StringView(u8"1$r") << value;
    writeDcsResponse(StringView(response));
}

void VtermImpl::dcs_DECRSTS_HLS(u32 index, u32 hue, u32 luminosity, u32 saturation) {
    applyPaletteColor((u16)(index), decHlsColor(hue, luminosity, saturation));
}

void VtermImpl::dcs_DECRSTS_RGB(u32 index, u32 red, u32 green, u32 blue) {
    if (red > 100) {
        red = 100;
    }
    if (green > 100) {
        green = 100;
    }
    if (blue > 100) {
        blue = 100;
    }
    applyPaletteColor(
        (u16)(index),
        {
            (u8)((red * 255 + 50) / 100),
            (u8)((green * 255 + 50) / 100),
            (u8)((blue * 255 + 50) / 100),
        }
    );
}

void VtermImpl::dcs_DECRSTS_TABS_BEGIN() {
    tabStops.clear();
    tabStopsCustomized = true;
    tabStopsRestored = true;
}

void VtermImpl::dcs_DECRSTS_TAB(u32 column) {
    if (column > (u32)(UINT16_MAX) + 1) {
        return;
    }
    const u16 zeroBased = (u16)(column - 1);
    const size_t position = tabLowerBound(tabStops, zeroBased);
    if (position == tabStops.length() || tabStops[position] != zeroBased) {
        tabInsertAt(tabStops, position, zeroBased);
    }
}

void VtermImpl::dcs_DECRSTS_CURSOR(u32 row, u32 column, u8 rendition, u8 protection, u8 flags, u8 gl, u8 gr, u8 sizeFlags, const Charset* charsets, const u16* charsetIds) {
    resetAttrs();
    attrs.bold = (rendition & 1) != 0;
    attrs.underline_style = rendition & 2 ? 1 : 0;
    attrs.blink = (rendition & 4) != 0;
    attrs.inverse = (rendition & 8) != 0;
    attrs.conceal = (rendition & 16) != 0;
    attrs.protected_char = protection & 1 ? TerminalCell::decProtection : 0;
    reverseVideo = attrs.inverse;

    originMode = flags & 1 ? OriginMode::ScrollingRegion : OriginMode::Absolute;
    charsetState.ss = flags & 4 ? 3 : (flags & 2 ? 2 : 0);
    charsetState.gl = gl;
    charsetState.gr = gr;
    charsetState.size96 = sizeFlags & 0x0f;
    for (size_t index = 0; index < 4; ++index) {
        charsetState.g[index] = charsets[index];
        charsetState.ids[index] = charsetIds[index];
    }

    posY = (u16)(min<u32>(row, pane_.rows) - 1);
    posX = (u16)(min<u32>(column, pane_.columns) - 1);
    lastCol = flags & 8;
    changePresentation();
}

void VtermImpl::dcs_DECAUPSS(Charset charset, u16 id, bool is96) {
    if (is96 && id == (((u16)('%') << 8) | '5')) {
        return;
    }
    userPreferenceCharset = charset == Charset::DecUserPref ? Charset::DecSuppl : charset;
    userPreferenceCharsetId = id;
    userPreferenceCharset96 = is96;
}

void VtermImpl::dcs_SIXEL(const ParserSixelImage& image) {
    const u16 availableCells = nColsEff > posX ? nColsEff - posX : 0;
    const u16 widthCells = (u16)(min<u32>((image.width + SixelPatch::width - 1) / SixelPatch::width, availableCells));
    const u32 heightCells = (image.height + SixelPatch::height - 1) / SixelPatch::height;
    if (widthCells == 0 || heightCells == 0) {
        return;
    }

    // One palette block per image, one extra append per covered cell:
    // the parser hands the picture whole, so nothing is rewritten.
    const u8* palette = extras_.store->internSixelPalette(image.palette);
    Vector<u8> patches;
    patches.grow((size_t)(widthCells)*SixelPatch::pixelCount);

    for (u32 cellRow = 0; cellRow < heightCells; ++cellRow) {
        u8* out = patches.mutData();
        for (u16 cellColumn = 0; cellColumn < widthCells; ++cellColumn) {
            for (u32 py = 0; py < SixelPatch::height; ++py) {
                const u32 y = cellRow * SixelPatch::height + py;
                const u8* source = y < image.height ? image.pixels + (size_t)(y)*image.pitch : nullptr;
                for (u32 px = 0; px < SixelPatch::width; ++px) {
                    const u32 x = (u32)(cellColumn)*SixelPatch::width + px;
                    out[py * SixelPatch::width + px] = source != nullptr && x < image.width ? source[x] : 0;
                }
            }
            out += SixelPatch::pixelCount;
        }
        cf->writeSixelCells(posY, posX, widthCells, patches.data(), palette, attrs, activeHyperlink, eraseAttrs);
        if (cellRow + 1 < heightCells) {
            performIndex();
        }
    }
}

void VtermImpl::dcs_DECRQSS_DECSCL() {
    StringBuilder value;
    value << 60 + (u8)(compatLevel) << StringView(u8";") << (send8BitControls ? 0 : 1) << StringView(u8"\"p");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_SGR() {
    StringBuilder value;
    value << StringView(u8"0");
    if (attrs.bold) {
        value << StringView(u8";1");
    }
    if (attrs.faint) {
        value << StringView(u8";2");
    }
    if (attrs.italic) {
        value << StringView(u8";3");
    }
    if (attrs.underlined()) {
        value << StringView(u8";4");
        if (attrs.underline_style > 1) {
            value << StringView(u8":") << (unsigned)(attrs.underline_style);
        }
    }
    if (attrs.blink) {
        value << StringView(u8";5");
    }
    if (reverseVideo) {
        value << StringView(u8";7");
    }
    if (attrs.conceal) {
        value << StringView(u8";8");
    }
    if (attrs.strike) {
        value << StringView(u8";9");
    }
    if (attrs.overline) {
        value << StringView(u8";53");
    }
    if (fgPalIx >= 0 && fgPalIx < 8) {
        value << StringView(u8";") << 30 + fgPalIx;
    } else if (fgPalIx >= 8 && fgPalIx < 16) {
        value << StringView(u8";") << 90 + fgPalIx - 8;
    } else if (fgPalIx >= 0) {
        value << StringView(u8";38:5:") << fgPalIx;
    } else if (attrForeground().source() == CellColor::Source::Direct) {
        const Color color = attrForeground().color();
        value << StringView(u8";38:2::") << (unsigned)(color.red) << StringView(u8":") << (unsigned)(color.green) << StringView(u8":") << (unsigned)(color.blue);
    }
    if (bgPalIx >= 0 && bgPalIx < 8) {
        value << StringView(u8";") << 40 + bgPalIx;
    } else if (bgPalIx >= 8 && bgPalIx < 16) {
        value << StringView(u8";") << 100 + bgPalIx - 8;
    } else if (bgPalIx >= 0) {
        value << StringView(u8";48:5:") << bgPalIx;
    } else if (attrBackground().source() == CellColor::Source::Direct) {
        const Color color = attrBackground().color();
        value << StringView(u8";48:2::") << (unsigned)(color.red) << StringView(u8":") << (unsigned)(color.green) << StringView(u8":") << (unsigned)(color.blue);
    }
    if (!underlineColorDefault) {
        if (underlinePalIx >= 0) {
            value << StringView(u8";58:5:") << underlinePalIx;
        } else {
            const Color color = attrUnderlineColor().color();
            value << StringView(u8";58:2::") << (unsigned)(color.red) << StringView(u8":") << (unsigned)(color.green) << StringView(u8":") << (unsigned)(color.blue);
        }
    }
    value << StringView(u8"m");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_DECSTBM() {
    StringBuilder value;
    value << marginTop + 1 << StringView(u8";") << marginBottom << StringView(u8"r");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_DECSLRM() {
    StringBuilder value;
    value << hMargin + 1 << StringView(u8";") << nColsEff << StringView(u8"s");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_DECSLPP() {
    // A5-3: the page length a DECRQSS answers with is this terminal's own
    // row count, not the window's. The two were the same number until A8
    // gave the terminal its own grid; the path here runs through
    // windowInfo() rather than the window grid, which is why A8's grep did
    // not catch it.
    StringBuilder value;
    value << pane_.rows << StringView(u8"t");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_DECSCUSR() {
    StringBuilder value;
    value << (unsigned)(cursorStyleParam) << StringView(u8" q");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_DECSCA() {
    StringBuilder value;
    value << ((attrs.protected_char & TerminalCell::decProtection) ? 1 : 0) << StringView(u8"\"q");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_DECSACE() {
    StringBuilder value;
    value << (rectangularAttributeExtent ? 2 : 0) << StringView(u8"*x");
    writeDecrqssResponse(StringView(value));
}

void VtermImpl::dcs_DECRQSS_UNKNOWN() {
    writeDcsResponse("0$r");
}

void VtermImpl::dcs_XTGETTCAP(StringView encoded, StringView value) {
    StringBuilder replies;
    replies << (send8BitControls ? StringView(u8"\x90") : StringView(u8"\x1bP"));
    replies << (value.empty() ? StringView(u8"0+r") : StringView(u8"1+r"));
    replies << encoded;
    if (!value.empty()) {
        static constexpr u8 hex[] = u8"0123456789abcdef";
        replies << StringView(u8"=");
        for (u8 ch : value) {
            const u8 pair[] = {hex[ch >> 4], hex[ch & 15]};
            replies.append(pair, sizeof(pair));
        }
    }
    replies << (send8BitControls ? StringView(u8"\x9c") : StringView(u8"\x1b\\"));
    const StringView output(replies);
    writePty(output);
}

void VtermImpl::recordOsc(u32 command, StringView payload) {
    if (trace != nullptr) {
        trace->osc(command, payload);
    }
}

void VtermImpl::recordBell() {
    if (trace != nullptr) {
        trace->bell();
    }
    host.requestAttention();
}

void VtermImpl::recordLeds(u8 state) {
    if (trace != nullptr) {
        trace->leds(state);
    }
}

void VtermImpl::notifyTitleChanged(StringView title) {
    const VtermTitleChanged event{this, title};
    host.titleChanged(event);
}

void VtermImpl::publishTitle(u32 command, StringView title) {
    titleSet = title != config().title;
    presentedTitle.reset();
    presentedTitle.append(title.data(), title.length());
    notifyTitleChanged(stringView(presentedTitle));
    recordOsc(command, title);
}

namespace {
    // The name behind a foreground group: its leader's comm. False when
    // the process is gone before its name could be read.
    static bool foregroundProcessName(pid_t group, Buffer& name) {
#ifdef __APPLE__
        char buffer[64] = {};
        if (proc_name((int)(group), buffer, sizeof(buffer)) <= 0) {
            return false;
        }
        name.append(buffer, strlen(buffer));
        return true;
#else
        StringBuilder path;
        path << StringView(u8"/proc/") << (i64)(group) << StringView(u8"/comm");
        Buffer file{StringView(path)};
        Buffer content;
        try {
            readFileContent(file, content);
        } catch (Exception&) {
            return false;
        }
        StringView comm{content};
        while (!comm.empty() && comm[comm.length() - 1] == '\n') {
            comm = StringView(comm.data(), comm.length() - 1);
        }
        if (comm.empty()) {
            return false;
        }
        name.append(comm.data(), comm.length());
        return true;
#endif
    }
}

void VtermImpl::refreshForegroundName() {
    const pid_t group = ptyStream_.pty->foregroundProcessGroup();
    if (group <= 0) {
        return;
    }
    Buffer name;
    if (!foregroundProcessName(group, name)) {
        return;
    }
    if (stringView(foregroundName_) == StringView(name)) {
        return;
    }
    const bool first = foregroundName_.length() == 0;
    foregroundName_.reset();
    foregroundName_.append(name.data(), name.length());
    if (first && titleSet) {
        // The very first look only takes the baseline: a title set
        // before the poll ever saw the pty belongs to the foreground
        // it is now looking at.
        return;
    }
    titleSet = false;
    presentedTitle.reset();
    presentedTitle.append(name.data(), name.length());
    notifyTitleChanged(stringView(presentedTitle));
}

void VtermImpl::publishCwd(StringView path) {
    if (trace != nullptr) {
        trace->cwd(path);
    }
    if (!titleSet) {
        presentedTitle.reset();
        presentedTitle.append(path.data(), path.length());
        notifyTitleChanged(stringView(presentedTitle));
    }
}

void VtermImpl::publishNotify(StringView id, StringView title, StringView body, bool close) {
    if (trace != nullptr) {
        trace->notify(id, title, body, close);
    }
    if (!close) {
        host.requestAttention();
    }
}

void VtermImpl::publishProgress(u32 state, u32 percent) {
    if (trace != nullptr) {
        trace->progress(state, percent);
    }
    if (state == 2 || state == 4) {
        host.requestAttention();
    }
}

plt::WindowInfo VtermImpl::windowInfo() const {
    return host.info();
}

// T5.1 SS2.7, T5.5: these two, and the two grid* calls in
// windowOperation() below, are the *window's* questions - how much grid
// fits in a given number of the window's pixels. They divide
// windowInfo().width, and the insets they take out of it are the
// window's content insets, which include whatever chrome reserved. This
// pane's insets are the border alone (A10), so substituting them here
// would answer with more columns than the window has and CSI 18t /
// CSI 19t would tell the application to draw wider than the pane.
// Nothing crashes; the report is simply wrong. The fallback is the
// window's grid for the same reason: it answers the same question with
// the cell size still unknown.
//
// The substitution got cheaper the day the insets started arriving
// through VtHost: host.contentInsets() and pane_.insets are now both in
// reach of one method, where they used to sit on different objects. It
// is guarded by
// TheWindowReportsCountTheWindowsOwnReserveAndNotThePanesBorder in
// lib/shitty/vt_headless_ut.cpp, which is the one fixture where the
// window's reserve is not zero.
u32 VtermImpl::columnsForPixelWidth(u32 width) const {
    if (windowGeometry_.cellPixelWidth == 0) {
        return windowGeometry_.columns;
    }
    return vtGridColumns(width, host.contentInsets(), windowGeometry_.cellPixelWidth);
}

u32 VtermImpl::rowsForPixelHeight(u32 height) const {
    if (windowGeometry_.cellPixelHeight == 0) {
        return windowGeometry_.rows;
    }
    return vtGridRows(height, host.contentInsets(), windowGeometry_.cellPixelHeight);
}

u32 VtermImpl::windowColumns() const {
    return columnsForPixelWidth(windowInfo().width);
}

u32 VtermImpl::windowRows() const {
    return rowsForPixelHeight(windowInfo().height);
}

u32 VtermImpl::screenColumns() const {
    return columnsForPixelWidth(windowInfo().screenPixelWidth);
}

u32 VtermImpl::screenRows() const {
    return rowsForPixelHeight(windowInfo().screenPixelHeight);
}

void VtermImpl::windowOperation(u32 operation, u32 first, u32 second) {
    if (trace != nullptr) {
        trace->windowOperation(operation, first, second);
    }
    VtHost* const window = &host;
    const auto resize = [&](u32 pixelWidth, u32 pixelHeight) {
        if (pixelWidth == 0 || pixelHeight == 0) {
            return;
        }
        window->requestResize(pixelWidth, pixelHeight);
        window->surfaceResized(pixelWidth, pixelHeight);
    };
    switch (operation) {
        case 1:
            window->requestRestore();
            return;
        case 2:
            window->requestIconify();
            return;
        case 3:
            window->requestMove((i32)(first), (i32)(second));
            return;
        case 5:
            window->requestFocus();
            return;
        case 7:
            window->requestFrame();
            return;
        case 9: {
            if (first == 0) {
                window->requestMaximized(false);
            } else if (first == 1) {
                window->requestMaximized(true);
            } else if (first == 2 || first == 3) {
                const plt::WindowInfo info = window->info();
                window->requestMaximized(true);
                resize(first == 2 ? info.width : info.screenPixelWidth, first == 3 ? info.height : info.screenPixelHeight);
            }
            return;
        }
        case 10:
            window->requestFullscreen(first == 1 || (first == 2 && !window->info().fullscreen));
            return;
        default:
            break;
    }
    u32 pixelWidth = 0;
    u32 pixelHeight = 0;
    if (operation == 4 && first != 0 && second != 0) {
        pixelWidth = second;
        pixelHeight = first;
    } else if (operation == 8 && first != 0 && second != 0) {
        const VtInsets insets = host.contentInsets();
        pixelWidth = vtGridPixelWidth(second, insets, windowGeometry_.cellPixelWidth);
        pixelHeight = vtGridPixelHeight(first, insets, windowGeometry_.cellPixelHeight);
    } else {
        return;
    }
    resize(pixelWidth, pixelHeight);
}

void VtermImpl::osc_TITLE_0(StringView payload) {
    iconTitle.reset();
    iconTitle.append(payload.data(), payload.length());
    windowTitle = iconTitle;
    publishTitle(0, payload);
}

void VtermImpl::osc_TITLE_1(StringView payload) {
    iconTitle.reset();
    iconTitle.append(payload.data(), payload.length());
    recordOsc(1, payload);
}

void VtermImpl::osc_TITLE_2(StringView payload) {
    windowTitle.reset();
    windowTitle.append(payload.data(), payload.length());
    publishTitle(2, payload);
}

void VtermImpl::osc_PALETTE(u32 index, Color color, bool query) {
    if (index >= 256 + TerminalColors::specialCount) {
        return;
    }
    const bool special = index >= 256;
    const u16 colorIndex = (u16)(special ? index - 256 : index);
    if (query) {
        StringBuilder reply;
        reply << StringView(u8"4;") << index << StringView(u8";") << (special ? colors.special[colorIndex] : colors.palette[colorIndex]);
        writeOscResponse(StringView(reply));
        return;
    }
    if (special) {
        colors.special[colorIndex] = color;
        colors.changed();
        exposeFrames();
    } else {
        applyPaletteColor(colorIndex, color);
    }
}

void VtermImpl::osc_SPECIAL_COLOR(u32 index, Color color, bool query) {
    if (index >= TerminalColors::specialCount) {
        return;
    }
    if (query) {
        StringBuilder reply;
        reply << StringView(u8"5;") << index << StringView(u8";") << colors.special[index];
        writeOscResponse(StringView(reply));
        return;
    }
    colors.special[index] = color;
    colors.changed();
    exposeFrames();
}

void VtermImpl::osc_SPECIAL_COLOR_MODE(u32 index, u32 value) {
    if (index > TerminalColors::specialCount) {
        return;
    }
    const u8 bit = (u8)(1u << index);
    const u8 modes = value == 0 ? (u8)(colors.specialModes & ~bit) : (u8)(colors.specialModes | bit);
    if (modes == colors.specialModes) {
        return;
    }
    colors.specialModes = modes;
    colors.changed();
    exposeFrames();
}

void VtermImpl::osc_RAW(u32 command, StringView payload) {
    recordOsc(command, payload);
}

void VtermImpl::osc_CWD(StringView path, bool valid) {
    if (valid) {
        publishCwd(path);
    }
}

void VtermImpl::osc_HYPERLINK(StringView id, bool hasId, StringView uri) {
    if (uri.empty()) {
        activeHyperlink = 0;
        return;
    }

    StringBuilder identity;
    if (hasId) {
        identity << StringView(u8"id=") << id << StringView(u8";uri=") << uri;
    } else {
        identity << StringView(u8"uri=") << uri;
    }
    const StringView identityView(identity);

    CellExtraStore& extras = *extras_.store;
    if (const u32 known = extras.findHyperlink(identityView); known != 0) {
        activeHyperlink = known;
        return;
    }
    if (nextHyperlink == 0) {
        activeHyperlink = 0;
        return;
    }
    activeHyperlink = extras.getOrCreateHyperlink(identityView, uri, nextHyperlink++);
}

void VtermImpl::osc_NOTIFY(StringView payload) {
    publishNotify({}, stringView(windowTitle), payload, false);
}

void VtermImpl::osc_PROGRESS(u32 state, u32 percent, bool percentPresent) {
    switch (state) {
        case 0:
            progressPercent = 0;
            break;
        case 1:
            progressPercent = percentPresent ? percent : 0;
            break;
        case 2:
            if (percentPresent) {
                progressPercent = percent;
            } else {
                progressPercent = 0;
            }
            break;
        case 3:
            progressPercent = 0;
            break;
        case 4:
            if (percentPresent) {
                progressPercent = percent;
            }
            break;
        default:
            return;
    }
    publishProgress(state, progressPercent);
}

void VtermImpl::writeDynamicColorResponse(u32 command, Color color) {
    StringBuilder response;
    response << command << StringView(u8";") << color;
    writeOscResponse(StringView(response));
}

void VtermImpl::osc_DEFAULT_FOREGROUND(Color color, bool query) {
    if (query) {
        return writeDynamicColorResponse(10, colors.defaultForeground);
    }
    colors.defaultForeground = color;
    if (!(selectionColorMask & 1)) {
        selectionFgColor = color;
    }
    colors.changed();
    defaultFgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_DEFAULT_BACKGROUND(Color color, bool query) {
    if (query) {
        return writeDynamicColorResponse(11, colors.defaultBackground);
    }
    colors.defaultBackground = color;
    if (!(selectionColorMask & 2)) {
        selectionBgColor = color;
    }
    colors.changed();
    defaultBgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_CURSOR_COLOR(Color color, bool query) {
    if (query) {
        return writeDynamicColorResponse(12, cursorColor);
    }
    cursorColor = color;
    changePresentation();
}

void VtermImpl::osc_SELECTION_BACKGROUND(Color color, bool query) {
    if (query) {
        return writeDynamicColorResponse(17, selectionBgColor);
    }
    selectionBgColor = color;
    selectionColorMask |= 2;
    changePresentation();
}

void VtermImpl::osc_SELECTION_FOREGROUND(Color color, bool query) {
    if (query) {
        return writeDynamicColorResponse(19, selectionFgColor);
    }
    selectionFgColor = color;
    selectionColorMask |= 1;
    changePresentation();
}

void VtermImpl::osc_CLIPBOARD_QUERY(bool primary, bool clipboard, u8 replySelector, bool selectorsEmpty) {
    if (!config().allowOsc52Read || (!primary && !clipboard)) {
        return;
    }
    const bool tryClipboard = primary && clipboard;
    const bool eightBit = send8BitControls;
    spawnTransaction([this, primary, tryClipboard, replySelector, selectorsEmpty, eightBit] {
        const plt::LockGuard guard(*ptyMutex_);
        u8 chunk[8 * 1024];
        ScopedPtr<Input> source{selectionTarget(host, primary)->read()};
        size_t count = source->read(chunk, sizeof(chunk));
        if (count == 0 && tryClipboard) {
            delete source.ptr;
            source.ptr = host.secondary()->read();
            count = source->read(chunk, sizeof(chunk));
        }
        Output& output = *ptyOutput_;
        StringBuilder header;
        header << (eightBit ? StringView(u8"\x9d") : StringView(u8"\x1b]")) << StringView(u8"52;");
        if (selectorsEmpty) {
            header << StringView(u8"s0");
        } else if (replySelector != 0) {
            header.append(&replySelector, 1);
        }
        header << StringView(u8";");
        const StringView prefix(header);
        output.write(prefix.data(), prefix.length());
        Base64Encoder encoder;
        while (count != 0) {
            encoder.write(output, StringView(chunk, count));
            count = source->read(chunk, sizeof(chunk));
        }
        encoder.finish(output);
        const StringView suffix = eightBit ? StringView(u8"\x9c") : StringView(u8"\x1b\\");
        output.write(suffix.data(), suffix.length());
        output.flush();
    });
}

void VtermImpl::osc_CLIPBOARD_WRITE(StringView decoded, bool valid, bool primary, bool clipboard) {
    if (!valid) {
        return;
    }
    if (primary) {
        writeSelection(*host.primary(), decoded);
    }
    if (clipboard) {
        writeSelection(*host.secondary(), decoded);
    }
}

void VtermImpl::writeKittyClipboardStatus(StringView type, StringView id, StringView status) {
    Buffer cleanId;
    copyKittyClipboardId(cleanId, id);
    StringBuilder packet;
    writeKittyClipboardPacket(packet, send8BitControls, type, status, StringView(cleanId));
    writePty(StringView(packet));
}

void VtermImpl::osc_KITTY_CLIPBOARD_READ(StringView id, StringView mimeTypes, bool primary, bool valid) {
    const bool targets = valid && mimeTypes == StringView(u8".");
    if (!valid) {
        writeKittyClipboardStatus(StringView(u8"read"), id, StringView(u8"ENOSYS"));
        return;
    }
    if (!targets && !config().allowOsc52Read) {
        writeKittyClipboardStatus(StringView(u8"read"), id, StringView(u8"EPERM"));
        return;
    }
    const StringView mimeType = targets ? StringView(u8".") : selectKittyClipboardMime(mimeTypes);
    if (mimeType.empty()) {
        writeKittyClipboardStatus(StringView(u8"read"), id, StringView(u8"ENOSYS"));
        return;
    }

    Buffer cleanId;
    copyKittyClipboardId(cleanId, id);
    Buffer idCopy(cleanId);
    Buffer mimeCopy;
    mimeCopy.append(mimeType.data(), mimeType.length());
    const bool eightBit = send8BitControls;
    spawnTransaction([this, idCopy, mimeCopy, primary, targets, eightBit] {
        const plt::LockGuard guard(*ptyMutex_);
        Output& output = *ptyOutput_;
        const StringView idView(idCopy);
        const StringView mimeView(mimeCopy);
        writeKittyClipboardPacket(output, eightBit, StringView(u8"read"), StringView(u8"OK"), idView, {}, {}, primary);
        if (targets) {
            writeKittyClipboardPacket(output, eightBit, StringView(u8"read"), StringView(u8"DATA"), idView, StringView(u8"."), StringView(u8"text/plain\n"), primary);
        } else {
            const ScopedPtr<Input> source{selectionTarget(host, primary)->read()};
            for (;;) {
                u8 chunk[4096];
                const size_t count = source->read(chunk, sizeof(chunk));
                if (count == 0) {
                    break;
                }
                writeKittyClipboardPacket(output, eightBit, StringView(u8"read"), StringView(u8"DATA"), idView, mimeView, StringView(chunk, count), primary);
            }
        }
        writeKittyClipboardPacket(output, eightBit, StringView(u8"read"), StringView(u8"DONE"), idView, {}, {}, primary);
        output.flush();
    });
}

void VtermImpl::osc_KITTY_CLIPBOARD_WRITE(StringView id, bool primary) {
    delete kittyClipboardWriteStream;
    kittyClipboardWriteStream = nullptr;
    kittyClipboardWriteLength = 0;
    copyKittyClipboardId(kittyClipboardWriteId, id);
    plt::Clipboard* const target = selectionTarget(host, primary);
    kittyClipboardWriteStream = target->write();
}

void VtermImpl::osc_KITTY_CLIPBOARD_WRITE_DATA(StringView id, StringView mimeType, StringView content, bool valid) {
    (void)id;
    if (kittyClipboardWriteStream == nullptr) {
        return;
    }
    if (!valid) {
        abortKittyClipboardWrite(StringView(u8"EINVAL"));
        return;
    }
    if (content.empty()) {
        kittyClipboardWriteStream->finish();
        delete kittyClipboardWriteStream;
        kittyClipboardWriteStream = nullptr;
        kittyClipboardWriteLength = 0;
        writeKittyClipboardStatus(StringView(u8"write"), StringView(kittyClipboardWriteId), StringView(u8"DONE"));
        return;
    }
    if (!kittyClipboardMimeSupported(mimeType)) {
        abortKittyClipboardWrite(StringView(u8"ENOSYS"));
        return;
    }
    constexpr size_t maximumWrite = 8 * 1024 * 1024;
    if (content.length() > maximumWrite - kittyClipboardWriteLength) {
        abortKittyClipboardWrite(StringView(u8"EIO"));
        return;
    }
    kittyClipboardWriteLength += content.length();
    kittyClipboardWriteStream->write(content.data(), content.length());
}

void VtermImpl::abortKittyClipboardWrite(StringView status) {
    delete kittyClipboardWriteStream;
    kittyClipboardWriteStream = nullptr;
    kittyClipboardWriteLength = 0;
    writeKittyClipboardStatus(StringView(u8"write"), StringView(kittyClipboardWriteId), status);
}

void VtermImpl::osc_KITTY_CLIPBOARD_WRITE_ALIAS(StringView id, StringView mimeType, StringView aliases, bool valid) {
    (void)id;
    (void)mimeType;
    (void)aliases;
    if (kittyClipboardWriteStream == nullptr || valid) {
        return;
    }
    abortKittyClipboardWrite(StringView(u8"EINVAL"));
}

void VtermImpl::osc_KITTY_CLIPBOARD_INVALID(StringView id, bool write) {
    if (write || kittyClipboardWriteStream != nullptr) {
        delete kittyClipboardWriteStream;
        kittyClipboardWriteStream = nullptr;
        kittyClipboardWriteLength = 0;
        writeKittyClipboardStatus(StringView(u8"write"), kittyClipboardWriteId.empty() ? id : StringView(kittyClipboardWriteId), StringView(u8"EINVAL"));
    }
}

bool VtermImpl::pasteMimeNotification(bool primary) {
    osc_KITTY_CLIPBOARD_READ({}, StringView(u8"."), primary, true);
    return true;
}

void VtermImpl::osc_RESET_PALETTE() {
    memcpy(colors.palette, originalPalette256, sizeof(colors.palette));
    colors.changed();
    exposeFrames();
}

void VtermImpl::osc_RESET_PALETTE(u32 index) {
    if (index > 255) {
        return;
    }
    applyPaletteColor((u16)(index), originalPalette256[index]);
}

void VtermImpl::osc_RESET_SPECIAL_COLOR() {
    memcpy(colors.special, colors.originalSpecial, sizeof(colors.special));
    colors.changed();
    exposeFrames();
}

void VtermImpl::osc_RESET_SPECIAL_COLOR(u32 index) {
    if (index >= TerminalColors::specialCount) {
        return;
    }
    colors.special[index] = colors.originalSpecial[index];
    colors.changed();
    exposeFrames();
}

void VtermImpl::osc_RESET_DEFAULT_FOREGROUND() {
    colors.defaultForeground = config().fg;
    colors.changed();
    defaultFgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_RESET_DEFAULT_BACKGROUND() {
    colors.defaultBackground = config().bg;
    colors.changed();
    defaultBgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_RESET_CURSOR_COLOR() {
    cursorColor = config().cr;
    changePresentation();
}

void VtermImpl::osc_RESET_SELECTION_BACKGROUND() {
    selectionBgColor = config().bg;
    selectionColorMask &= ~2;
    changePresentation();
}

void VtermImpl::osc_RESET_SELECTION_FOREGROUND() {
    selectionFgColor = config().fg;
    selectionColorMask &= ~1;
    changePresentation();
}

void VtermImpl::startSemanticPrompt(StringView payload) {
    currentSemantic = 1;
    semanticUntilEndOfLine = false;
    const StringView kind = semanticOption(payload, StringView(u8"k"));
    const bool continuation = kind == StringView(u8"c") || kind == StringView(u8"s");
    cf->setSemanticPrompt(posY, continuation ? ScreenSemanticPrompt::Continuation : ScreenSemanticPrompt::Prompt);
}

bool VtermImpl::cursorIsAtPrompt() const {
    if (altScreenBufferMode) {
        return false;
    }
    return cf->semanticPrompt(posY) != ScreenSemanticPrompt::None || currentSemantic == 1 || currentSemantic == 2;
}

void VtermImpl::osc_SHELL_A(StringView payload) {
    // OSC 133;A marks the prompt start without moving the cursor, with
    // Kitty, Contour, VTE and foot; Ghostty, iTerm2 and WezTerm move to
    // a fresh line first, as the Semantic Prompts proposal specifies.
    // The explicit fresh-line forms are OSC 133;L and OSC 133;N, so the
    // marker keeps the majority reading and stays put.
    recordOsc(133, payload);
    startSemanticPrompt(payload);
    promptReported = false;
    semanticClick = SemanticClick::None;
    const StringView clickEvents = semanticOption(payload, StringView(u8"click_events"));
    if (clickEvents == StringView(u8"1")) {
        semanticClick = SemanticClick::Absolute;
    } else if (clickEvents == StringView(u8"2")) {
        semanticClick = SemanticClick::Relative;
    } else {
        const StringView click = semanticOption(payload, StringView(u8"cl"));
        if (click == StringView(u8"line")) {
            semanticClick = SemanticClick::Line;
        } else if (click == StringView(u8"m")) {
            semanticClick = SemanticClick::Multiple;
        } else if (click == StringView(u8"v")) {
            semanticClick = SemanticClick::ConservativeVertical;
        } else if (click == StringView(u8"w")) {
            semanticClick = SemanticClick::SmartVertical;
        }
    }
}

void VtermImpl::osc_SHELL_B(StringView payload) {
    currentSemantic = 2;
    semanticUntilEndOfLine = false;
    promptColumn = posX;
    promptReported = false;
    recordOsc(133, payload);
}

void VtermImpl::osc_SHELL_C(StringView payload) {
    if (posX == 0 && cf->semanticPrompt(posY) != ScreenSemanticPrompt::None) {
        cf->setSemanticPrompt(posY, ScreenSemanticPrompt::None);
    }
    currentSemantic = 3;
    semanticUntilEndOfLine = false;
    recordOsc(133, payload);
}

void VtermImpl::osc_SHELL_D(StringView payload) {
    currentSemantic = 0;
    promptReported = false;
    semanticUntilEndOfLine = false;
    recordOsc(133, payload);
}

void VtermImpl::osc_SHELL_I(StringView payload) {
    currentSemantic = 2;
    semanticUntilEndOfLine = true;
    recordOsc(133, payload);
}

void VtermImpl::osc_SHELL_L(StringView payload) {
    const u16 leftMargin = posX < hMargin ? 0 : hMargin;
    if (posX != leftMargin) {
        inp_CR();
        esc_IND();
    }
    recordOsc(133, payload);
}

void VtermImpl::osc_SHELL_N(StringView payload) {
    // The explicit fresh-line prompt start: a command whose output ended
    // mid-row gets its prompt on the next row, then everything OSC 133;A
    // records. Ghostty and WezTerm implement the same reading of N.
    const u16 leftMargin = posX < hMargin ? 0 : hMargin;
    if (posX != leftMargin) {
        inp_CR();
        esc_IND();
    }
    osc_SHELL_A(payload);
}

void VtermImpl::osc_SHELL_P(StringView payload) {
    startSemanticPrompt(payload);
    recordOsc(133, payload);
}

void VtermImpl::osc_SHELL_UNKNOWN(StringView payload) {
    recordOsc(133, payload);
}

void VtermImpl::osc_UNKNOWN(u32 command, StringView payload) {
    recordOsc(command, payload);
    if (command == promptReportOsc) {
        // Only between the prompt's start and the command's: a report from
        // anywhere else names no line on this screen.
        if (promptAtInput() && decodePromptReport(payload, promptLine)) {
            if (!promptReported && config().verbose) {
                // Once a prompt, so the log says the integration is heard.
                fprintf(stderr, "%.*s: prompt editor: line reported, input at column %u, cursor at %u,%u\n", (int)(config().brandName.length()), (const char*)(config().brandName.data()), (unsigned)(promptColumn), (unsigned)(posX), (unsigned)(posY));
            }
            promptReported = true;
        } else if (config().verbose) {
            fprintf(stderr, "%.*s: prompt editor: report refused, semantic=%u, %zu bytes\n", (int)(config().brandName.length()), (const char*)(config().brandName.data()), (unsigned)(currentSemantic), (size_t)(payload.length()));
        }
        return;
    }
    if (command == 1337 && payload == StringView(u8"Capabilities")) {
        // iTerm2 feature reporting: the same string children get in
        // TERM_FEATURES, for applications that ask instead.
        StringBuilder response;
        response << StringView(u8"1337;Capabilities=");
        appendTermFeatures(response, config().widths);
        writeOscResponse(StringView(response));
    }
}

void VtermImpl::osc_NOTIFICATION_CAPABILITIES(StringView id) {
    StringBuilder response;
    response << StringView(u8"99;i=") << id << StringView(u8":p=?;p=title,body,close");
    writeOscResponse(StringView(response));
}

void VtermImpl::osc_NOTIFICATION_CLOSE(StringView id) {
    if (id.empty()) {
        return;
    }
    // The notification backend owns completed IDs.  Retaining them in the
    // terminal just to validate close requests makes terminal state grow
    // without bound, while forwarding an unknown close is harmless.
    publishNotify(id, {}, {}, true);
    notifications.erase(id.hash64());
}

void VtermImpl::osc_NOTIFICATION_TITLE(StringView id, StringView payload, bool encoded, bool finalChunk) {
    applyNotificationPart(id, payload, encoded, finalChunk, false);
}

void VtermImpl::osc_NOTIFICATION_BODY(StringView id, StringView payload, bool encoded, bool finalChunk) {
    applyNotificationPart(id, payload, encoded, finalChunk, true);
}

void VtermImpl::applyNotificationPart(StringView id, StringView payload, bool encoded, bool finalChunk, bool body) {
    const u64 key = id.hash64();
    Notification& notification = notifications[key];
    notification.key = key;
    NotificationPart& destination = body ? notification.body : notification.title;
    const auto flushEncoded = [](NotificationPart& part) {
        if (part.encodedOffset == (size_t)-1) {
            return true;
        }
        size_t decodedSize = part.text.used() - part.encodedOffset;
        if (!base64DecodeInPlace((u8*)(part.text.mutData()) + part.encodedOffset, decodedSize) || part.encodedOffset + decodedSize > 8192) {
            return false;
        }
        part.text.seekAbsolute(part.encodedOffset + decodedSize);
        part.encodedOffset = (size_t)-1;
        return true;
    };

    if (encoded) {
        if (destination.encodedOffset == (size_t)-1) {
            destination.encodedOffset = destination.text.used();
        }
        const size_t decodedCapacity = 8192 - destination.encodedOffset;
        const size_t encodedCapacity = (decodedCapacity + 2) / 3 * 4;
        if (destination.text.used() - destination.encodedOffset + payload.length() > encodedCapacity) {
            notifications.erase(key);
            return;
        }
        destination.text.append(payload.data(), payload.length());
        if (!payload.empty() && payload[payload.length() - 1] == '=' && !flushEncoded(destination)) {
            notifications.erase(key);
            return;
        }
    } else {
        if (!flushEncoded(destination) || destination.text.used() + payload.length() > 8192) {
            notifications.erase(key);
            return;
        }
        destination.text.append(payload.data(), payload.length());
    }

    if (!finalChunk) {
        return;
    }
    if (!flushEncoded(notification.title) || !flushEncoded(notification.body)) {
        notifications.erase(key);
        return;
    }

    const auto escapeSafeUtf8 = [](const Buffer& text) {
        const u8* value = (const u8*)(text.data());
        for (size_t k = 0; k < text.used();) {
            const u8 first = value[k++];
            if (first <= 0x1f || first == 0x7f) {
                return false;
            }
            if (first < 0x80) {
                continue;
            }
            u32 codepoint;
            u32 minimum;
            size_t continuation;
            if ((first & 0xe0) == 0xc0) {
                codepoint = first & 0x1f;
                minimum = 0x80;
                continuation = 1;
            } else if ((first & 0xf0) == 0xe0) {
                codepoint = first & 0x0f;
                minimum = 0x800;
                continuation = 2;
            } else if ((first & 0xf8) == 0xf0) {
                codepoint = first & 0x07;
                minimum = 0x10000;
                continuation = 3;
            } else {
                return false;
            }
            if (k + continuation > text.used()) {
                return false;
            }
            for (size_t n = 0; n < continuation; ++n) {
                const u8 ch = value[k++];
                if ((ch & 0xc0) != 0x80) {
                    return false;
                }
                codepoint = (codepoint << 6) | (ch & 0x3f);
            }
            if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) || (codepoint >= 0x7f && codepoint <= 0x9f)) {
                return false;
            }
        }
        return true;
    };
    if (!escapeSafeUtf8(notification.title.text) || !escapeSafeUtf8(notification.body.text)) {
        notifications.erase(key);
        return;
    }
    if (notification.title.text.empty()) {
        notification.title.text.xchg(notification.body.text);
        notification.body.text.reset();
    }
    if (notification.title.text.empty()) {
        notifications.erase(key);
        return;
    }
    publishNotify(id, stringView(notification.title.text), stringView(notification.body.text), false);
    notifications.erase(key);
}

void VtermImpl::reportInBandResize() {
    StringBuilder response;
    response << StringView(u8"48;") << pane_.rows << StringView(u8";") << pane_.columns << StringView(u8";") << pane_.rows * windowGeometry_.cellPixelHeight << StringView(u8";") << pane_.columns * windowGeometry_.cellPixelWidth << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::reportColorScheme() {
    // Shitty has no runtime profile or operating-system theme switching.  Its
    // configured background therefore remains the authoritative preference;
    // application-originated OSC color changes must not affect this report.
    const u32 brightness = 299 * config().bg.red + 587 * config().bg.green + 114 * config().bg.blue;
    const u8 scheme = brightness >= 128000 ? 2 : 1;
    StringBuilder response;
    response << StringView(u8"?997;") << (unsigned)(scheme) << StringView(u8"n");
    writeCsiResponse(StringView(response));
}

void VtermImpl::writeTitleResponse(char kind, StringView title) {
    StringBuilder response;
    response << StringView(u8"\x1b]");
    response.append(&kind, 1);
    if (titleModes & 2) {
        for (u8 byte : title) {
            response << Hex{byte, 2, true};
        }
    } else {
        response << title;
    }
    response << StringView(u8"\x1b\\");
    writePty(StringView((const u8*)(response.data()), response.used()));
}

void VtermImpl::resetTitleModes() {
    titleModes = 0;
}

void VtermImpl::setTitleMode(u8 bit, bool enabled) {
    if (enabled) {
        titleModes |= bit;
    } else {
        titleModes &= ~bit;
    }
}

void VtermImpl::applyPaletteColor(u16 index, Color color) {
    colors.palette[index] = color;
    colors.changed();
    exposeFrames();
}

void VtermImpl::csi_DECSCL(CompatibilityLevel level, bool enable8BitControls) {
    resetTerminal();
    compatLevel = level;
    send8BitControls = enable8BitControls;
}

void VtermImpl::xtResizePixels(u32 height, bool heightPresent, u32 width, bool widthPresent) {
    const auto info = windowInfo();
    const auto dimension = [](u32 value, bool present, u32 current, u32 maximum) {
        return present ? value ? value : maximum : current;
    };
    windowOperation(4, dimension(height, heightPresent, info.height, info.screenPixelHeight), dimension(width, widthPresent, info.width, info.screenPixelWidth));
}

void VtermImpl::xtResizeCells(u32 height, bool heightPresent, u32 width, bool widthPresent) {
    const auto dimension = [](u32 value, bool present, u32 current, u32 maximum) {
        return present ? value ? value : maximum : current;
    };
    windowOperation(8, dimension(height, heightPresent, windowRows(), screenRows()), dimension(width, widthPresent, windowColumns(), screenColumns()));
}

void VtermImpl::xtWindowOperation(u32 operation, u32 first, u32 second) {
    windowOperation(operation, first, second);
}

void VtermImpl::xtReportWindowState() {
    writeCsiResponse(windowInfo().iconified ? "2t" : "1t");
}

void VtermImpl::xtReportWindowPosition() {
    StringBuilder response;
    const auto info = windowInfo();
    response << StringView(u8"3;") << (u16)(info.x) << StringView(u8";") << (u16)(info.y) << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportWindowPixelSize(bool compositorSize) {
    StringBuilder response;
    if (compositorSize) {
        response << StringView(u8"4;") << windowGeometry_.pixelHeight << StringView(u8";") << windowGeometry_.pixelWidth << StringView(u8"t");
    } else {
        response << StringView(u8"4;") << pane_.rows * windowGeometry_.cellPixelHeight << StringView(u8";") << pane_.columns * windowGeometry_.cellPixelWidth << StringView(u8"t");
    }
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportScreenPixelSize() {
    StringBuilder response;
    const auto info = windowInfo();
    response << StringView(u8"5;") << info.screenPixelHeight << StringView(u8";") << info.screenPixelWidth << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportCellSize() {
    StringBuilder response;
    response << StringView(u8"6;") << windowGeometry_.cellPixelHeight << StringView(u8";") << windowGeometry_.cellPixelWidth << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportGridSize() {
    StringBuilder response;
    response << StringView(u8"8;") << pane_.rows << StringView(u8";") << pane_.columns << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportScreenGridSize() {
    StringBuilder response;
    response << StringView(u8"9;") << screenRows() << StringView(u8";") << screenColumns() << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportIconTitle() {
    writeTitleResponse('L', stringView(iconTitle));
}

void VtermImpl::xtReportWindowTitle() {
    writeTitleResponse('l', stringView(windowTitle));
}

void VtermImpl::xtPushTitle(bool icon, bool window) {
    if (titleDepth == 10) {
        for (size_t index = 1; index < 10; ++index) {
            stl::xchg(titleStack[index - 1], titleStack[index]);
        }
        titleDepth -= 1;
    }
    SavedTitles& saved = titleStack[titleDepth++];
    saved.hasIcon = icon;
    saved.hasWindow = window;
    saved.icon.reset();
    saved.window.reset();
    if (icon) {
        saved.icon = iconTitle;
    }
    if (window) {
        saved.window = windowTitle;
    }
}

void VtermImpl::xtPopTitle(bool icon, bool window) {
    if (titleDepth == 0) {
        return;
    }
    titleDepth -= 1;
    SavedTitles& saved = titleStack[titleDepth];
    for (size_t index = titleDepth; index-- > 0 && (!saved.hasIcon || !saved.hasWindow);) {
        if (!saved.hasIcon && titleStack[index].hasIcon) {
            saved.hasIcon = true;
            saved.icon = titleStack[index].icon;
        }
        if (!saved.hasWindow && titleStack[index].hasWindow) {
            saved.hasWindow = true;
            saved.window = titleStack[index].window;
        }
    }
    if (icon && saved.hasIcon) {
        iconTitle = saved.icon;
        recordOsc(1, stringView(iconTitle));
    }
    if (window && saved.hasWindow) {
        windowTitle = saved.window;
        publishTitle(2, stringView(windowTitle));
    }
}

void VtermImpl::xtResizeRows(u32 rows) {
    windowOperation(8, rows, windowColumns());
}

void VtermImpl::csi_XTHIMOUSE(u32 start, u32 startX, u32 startY, u32 firstRow, u32 lastRow) {
    if (mouseTrk.mode == MouseTrackingMode::VT200_Highlight && start != 0) {
        mouseHighlight.active = true;
        mouseHighlight.startX = max<u32>(1, startX);
        mouseHighlight.startY = max<u32>(1, startY);
        mouseHighlight.firstRow = max<u32>(1, firstRow);
        mouseHighlight.lastRow = max<u32>(mouseHighlight.firstRow, lastRow);
    } else {
        mouseHighlight.active = false;
    }
}

void VtermImpl::setLocatorReporting(bool enabled, bool oneShot, bool pixels) {
    locator.enabled = enabled ? oneShot ? 2 : 1 : 0;
    locator.pixels = pixels;
    locator.filter = false;
}

void VtermImpl::resetLocatorEvents() {
    locator.reportDown = false;
    locator.reportUp = false;
    locator.filter = false;
}

void VtermImpl::setLocatorButtonDown(bool enabled) {
    locator.reportDown = enabled;
}

void VtermImpl::setLocatorButtonUp(bool enabled) {
    locator.reportUp = enabled;
}

void VtermImpl::csi_DECRQLP() {
    if (locator.enabled) {
        const u16 x = locator.pixels ? locator.pixelX : locator.column;
        const u16 y = locator.pixels ? locator.pixelY : locator.row;
        StringBuilder response;
        response << StringView(u8"1;") << (unsigned)(locator.buttons) << StringView(u8";") << y << StringView(u8";") << x << StringView(u8";0&w");
        writeCsiResponse(StringView(response));
        if (locator.enabled == 2) {
            locator.enabled = 0;
        }
    } else {
        writeCsiResponse("0&w");
    }
}

void VtermImpl::csi_DECEFR(u32 top, u32 left, u32 bottom, u32 right) {
    locator.filterTop = top ? (u16)(top) : locator.pixels ? locator.pixelY : locator.row;
    locator.filterLeft = left ? (u16)(left) : locator.pixels ? locator.pixelX : locator.column;
    locator.filterBottom = bottom ? (u16)(bottom) : locator.filterTop;
    locator.filterRight = right ? (u16)(right) : locator.filterLeft;
    locator.filter = locator.enabled != 0;
}

void VtermImpl::csi_DECAC_TEXT(u8 foreground, u8 background) {
    colors.defaultForeground = colors.palette[foreground];
    colors.defaultBackground = colors.palette[background];
    colors.changed();
    defaultFgPalIx = -1;
    defaultBgPalIx = -1;
    assignedDefaultColors = true;
    exposeFrames();
}

void VtermImpl::csi_DECAC_TEXT_RESET() {
    colors.defaultForeground = config().fg;
    colors.defaultBackground = config().bg;
    colors.changed();
    defaultFgPalIx = -1;
    defaultBgPalIx = -1;
    assignedDefaultColors = false;
    exposeFrames();
}

// VT525 window-frame colors. xterm — the reference implementation for
// DECAC outside Windows — ignores the frame item too ("window frames:
// not implemented"): there is no DEC-style frame to paint.
void VtermImpl::csi_DECAC_FRAME(u8, u8) {
}

void VtermImpl::csi_DECAC_FRAME_RESET() {
}

void VtermImpl::resetModifyKeyResources() {
    memcpy(modifyKeyResources, initialModifyKeyResources, sizeof(modifyKeyResources));
    modifyOtherKeys = modifyKeyResources[4];
}

void VtermImpl::setModifyKeyResource(u8 resource, u8 value, bool useDefault) {
    modifyKeyResources[resource] = useDefault ? initialModifyKeyResources[resource] : value;
    modifyOtherKeys = modifyKeyResources[4];
}

void VtermImpl::reportModifyKeyResource(u8 resource) {
    StringBuilder response;
    response << StringView(u8">") << (unsigned)(resource) << StringView(u8";") << (unsigned)(modifyKeyResources[resource]) << StringView(u8"m");
    writeCsiResponse(StringView(response));
}

void VtermImpl::csi_kittyKeyboardPush(u32 flags) {
    constexpr size_t maxStackDepth = 16;
    auto& state = kittyKeyboardState();
    if (state.stack.length() == maxStackDepth) {
        memmove(state.stack.mutData(), state.stack.data() + 1, (maxStackDepth - 1) * sizeof(u8));
        state.stack.popBack();
    }
    state.stack.pushBack(state.flags);
    state.flags = flags & 0x1f;
}

void VtermImpl::csi_kittyKeyboardPop(u32 count) {
    auto& state = kittyKeyboardState();
    for (u32 k = 0; k < count; ++k) {
        if (state.stack.empty()) {
            state.flags = 0;
            break;
        }
        state.flags = state.stack.popBack();
    }
}

void VtermImpl::setKittyKeyboardFlags(u8 flags) {
    kittyKeyboardState().flags = flags;
}

void VtermImpl::addKittyKeyboardFlags(u8 flags) {
    kittyKeyboardState().flags |= flags;
}

void VtermImpl::removeKittyKeyboardFlags(u8 flags) {
    kittyKeyboardState().flags &= ~flags;
}

void VtermImpl::csi_kittyKeyboardQuery() {
    StringBuilder response;
    response << StringView(u8"?") << (unsigned)(getKittyKeyboardFlags()) << StringView(u8"u");
    writeCsiResponse(StringView(response));
}

namespace {
    using Key = InputKey;
    using InputSpec = VtermInputSpec;

#define ESC "\x1b"
#define CSI ESC "["
#define SS3 ESC "O"

#define MC "\xff"

    static const InputSpec is_modOtherKeys2[] = {
        {Key::Tab, CSI "27;" MC ";9~"},
        {Key::Enter, CSI "27;" MC ";13~"},
        {Key::Space, CSI "27;" MC ";32~"},
        {Key::Backspace, CSI "27;" MC ";127~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Alt[] = {
        {Key::Backspace, "\xc3\xbf"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Alt_altSendsEscape[] = {
        {Key::Backspace, ESC "\x7f"},
        {Key::Space, ESC " "},
        {Key::Tab, ESC "\t"},
        {Key::Enter, ESC "\r"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Control_modOtherKeys[] = {
        {Key::Tab, CSI "27;" MC ";9~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_ControlAlt_altSendsEscape[] = {
        {Key::Space, ESC "\x00", 2},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Control[] = {
        {Key::Space, "\x00", 1},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Shift[] = {
        {Key::Tab, CSI "Z"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_modOtherKeys[] = {
        {Key::Enter, CSI "27;" MC ";13~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Ansi[] = {
        {Key::Space, " "},
        {Key::Backspace, "\x7f"},
        {Key::Tab, "\t"},
        {Key::Enter, "\r"},
        {Key::Insert, CSI "2~"},
        {Key::Delete, CSI "3~"},
        {Key::PageUp, CSI "5~"},
        {Key::PageDown, CSI "6~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Mod_Ansi[] = {
        {Key::Insert, CSI "2;" MC "~"},
        {Key::Delete, CSI "3;" MC "~"},
        {Key::PageUp, CSI "5;" MC "~"},
        {Key::PageDown, CSI "6;" MC "~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Ansi_FunctionKeys[] = {
        {Key::F1, SS3 "P"},
        {Key::KeypadF1, SS3 "P"},
        {Key::F2, SS3 "Q"},
        {Key::KeypadF2, SS3 "Q"},
        {Key::F3, SS3 "R"},
        {Key::KeypadF3, SS3 "R"},
        {Key::F4, SS3 "S"},
        {Key::KeypadF4, SS3 "S"},
        {Key::F5, CSI "15~"},
        {Key::F6, CSI "17~"},
        {Key::F7, CSI "18~"},
        {Key::F8, CSI "19~"},
        {Key::F9, CSI "20~"},
        {Key::F10, CSI "21~"},
        {Key::F11, CSI "23~"},
        {Key::F12, CSI "24~"},
        {Key::F13, CSI "25~"},
        {Key::F14, CSI "26~"},
        {Key::F15, CSI "28~"},
        {Key::F16, CSI "29~"},
        {Key::F17, CSI "31~"},
        {Key::F18, CSI "32~"},
        {Key::F19, CSI "33~"},
        {Key::F20, CSI "34~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Mod_Ansi_FunctionKeys[] = {
        {Key::F1, CSI "1;" MC "P"},
        {Key::KeypadF1, CSI "1;" MC "P"},
        {Key::F2, CSI "1;" MC "Q"},
        {Key::KeypadF2, CSI "1;" MC "Q"},
        {Key::F3, CSI "1;" MC "R"},
        {Key::KeypadF3, CSI "13;" MC "~"},
        {Key::F4, CSI "1;" MC "S"},
        {Key::KeypadF4, CSI "1;" MC "S"},
        {Key::F5, CSI "15;" MC "~"},
        {Key::F6, CSI "17;" MC "~"},
        {Key::F7, CSI "18;" MC "~"},
        {Key::F8, CSI "19;" MC "~"},
        {Key::F9, CSI "20;" MC "~"},
        {Key::F10, CSI "21;" MC "~"},
        {Key::F11, CSI "23;" MC "~"},
        {Key::F12, CSI "24;" MC "~"},
        {Key::F13, CSI "25;" MC "~"},
        {Key::F14, CSI "26;" MC "~"},
        {Key::F15, CSI "28;" MC "~"},
        {Key::F16, CSI "29;" MC "~"},
        {Key::F17, CSI "31;" MC "~"},
        {Key::F18, CSI "32;" MC "~"},
        {Key::F19, CSI "33;" MC "~"},
        {Key::F20, CSI "34;" MC "~"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Ansi_KeypadKeys[] = {
        {Key::KeypadSpace, " "},
        {Key::KeypadTab, "\t"},
        {Key::KeypadEnter, "\r"},
        {Key::KeypadMultiply, "*"},
        {Key::KeypadAdd, "+"},
        {Key::KeypadSeparator, ","},
        {Key::KeypadSubtract, "-"},
        {Key::KeypadDivide, "/"},
        {Key::KeypadDelete, "."},
        {Key::KeypadDecimal, "."},
        {Key::KeypadInsert, "0"},
        {Key::Keypad0, "0"},
        {Key::KeypadEnd, "1"},
        {Key::Keypad1, "1"},
        {Key::KeypadDown, "2"},
        {Key::Keypad2, "2"},
        {Key::KeypadPageDown, "3"},
        {Key::Keypad3, "3"},
        {Key::KeypadLeft, "4"},
        {Key::Keypad4, "4"},
        {Key::KeypadBegin, "5"},
        {Key::Keypad5, "5"},
        {Key::KeypadRight, "6"},
        {Key::Keypad6, "6"},
        {Key::KeypadHome, "7"},
        {Key::Keypad7, "7"},
        {Key::KeypadUp, "8"},
        {Key::Keypad8, "8"},
        {Key::KeypadPageUp, "9"},
        {Key::Keypad9, "9"},
        {Key::KeypadEqual, "="},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Appl_KeypadKeys[] = {
        {Key::KeypadSpace, SS3 " "},
        {Key::KeypadTab, SS3 "I"},
        {Key::KeypadEnter, SS3 "M"},
        {Key::KeypadMultiply, SS3 "j"},
        {Key::KeypadAdd, SS3 "k"},
        {Key::KeypadSeparator, SS3 "l"},
        {Key::KeypadSubtract, SS3 "m"},
        {Key::KeypadDelete, SS3 "n"},
        {Key::KeypadDecimal, SS3 "n"},
        {Key::KeypadDivide, SS3 "o"},
        {Key::KeypadInsert, SS3 "p"},
        {Key::Keypad0, SS3 "p"},
        {Key::KeypadEnd, SS3 "q"},
        {Key::Keypad1, SS3 "q"},
        {Key::KeypadDown, SS3 "r"},
        {Key::Keypad2, SS3 "r"},
        {Key::KeypadPageDown, SS3 "s"},
        {Key::Keypad3, SS3 "s"},
        {Key::KeypadLeft, SS3 "t"},
        {Key::Keypad4, SS3 "t"},
        {Key::KeypadBegin, SS3 "u"},
        {Key::Keypad5, SS3 "u"},
        {Key::KeypadRight, SS3 "v"},
        {Key::Keypad6, SS3 "v"},
        {Key::KeypadHome, SS3 "w"},
        {Key::Keypad7, SS3 "w"},
        {Key::KeypadUp, SS3 "x"},
        {Key::Keypad8, SS3 "x"},
        {Key::KeypadPageUp, SS3 "y"},
        {Key::Keypad9, SS3 "y"},
        {Key::KeypadEqual, SS3 "X"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Mod_Appl_KeypadKeys[] = {
        {Key::KeypadSpace, SS3 MC " "},
        {Key::KeypadTab, SS3 MC "I"},
        {Key::KeypadEnter, SS3 MC "M"},
        {Key::KeypadMultiply, SS3 MC "j"},
        {Key::KeypadAdd, SS3 MC "k"},
        {Key::KeypadSeparator, SS3 MC "l"},
        {Key::KeypadSubtract, SS3 MC "m"},
        {Key::KeypadDelete, SS3 MC "n"},
        {Key::KeypadDecimal, SS3 MC "n"},
        {Key::KeypadDivide, SS3 MC "o"},
        {Key::KeypadInsert, SS3 MC "p"},
        {Key::Keypad0, SS3 MC "p"},
        {Key::KeypadEnd, SS3 MC "q"},
        {Key::Keypad1, SS3 MC "q"},
        {Key::KeypadDown, SS3 MC "r"},
        {Key::Keypad2, SS3 MC "r"},
        {Key::KeypadPageDown, SS3 MC "s"},
        {Key::Keypad3, SS3 MC "s"},
        {Key::KeypadLeft, SS3 MC "t"},
        {Key::Keypad4, SS3 MC "t"},
        {Key::KeypadBegin, SS3 MC "u"},
        {Key::Keypad5, SS3 MC "u"},
        {Key::KeypadRight, SS3 MC "v"},
        {Key::Keypad6, SS3 MC "v"},
        {Key::KeypadHome, SS3 MC "w"},
        {Key::Keypad7, SS3 MC "w"},
        {Key::KeypadUp, SS3 MC "x"},
        {Key::Keypad8, SS3 MC "x"},
        {Key::KeypadPageUp, SS3 MC "y"},
        {Key::Keypad9, SS3 MC "y"},
        {Key::KeypadEqual, SS3 MC "X"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_VT52_KeypadKeys[] = {
        {Key::KeypadSpace, ESC "? "},
        {Key::KeypadTab, ESC "?I"},
        {Key::KeypadEnter, ESC "?M"},
        {Key::KeypadMultiply, ESC "?j"},
        {Key::KeypadAdd, ESC "?k"},
        {Key::KeypadSeparator, ESC "?l"},
        {Key::KeypadSubtract, ESC "?m"},
        {Key::KeypadDelete, ESC "?n"},
        {Key::KeypadDecimal, ESC "?n"},
        {Key::KeypadDivide, ESC "?o"},
        {Key::KeypadInsert, ESC "?p"},
        {Key::Keypad0, ESC "?p"},
        {Key::KeypadEnd, ESC "?q"},
        {Key::Keypad1, ESC "?q"},
        {Key::KeypadDown, ESC "?r"},
        {Key::Keypad2, ESC "?r"},
        {Key::KeypadPageDown, ESC "?s"},
        {Key::Keypad3, ESC "?s"},
        {Key::KeypadLeft, ESC "?t"},
        {Key::Keypad4, ESC "?t"},
        {Key::KeypadBegin, ESC "?u"},
        {Key::Keypad5, ESC "?u"},
        {Key::KeypadRight, ESC "?v"},
        {Key::Keypad6, ESC "?v"},
        {Key::KeypadHome, ESC "?w"},
        {Key::Keypad7, ESC "?w"},
        {Key::KeypadUp, ESC "?x"},
        {Key::Keypad8, ESC "?x"},
        {Key::KeypadPageUp, ESC "?y"},
        {Key::Keypad9, ESC "?y"},
        {Key::KeypadEqual, ESC "?X"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_VT52_FunctionKeys[] = {
        {Key::F1, ESC "P"},
        {Key::KeypadF1, ESC "P"},
        {Key::F2, ESC "Q"},
        {Key::KeypadF2, ESC "Q"},
        {Key::F3, ESC "R"},
        {Key::KeypadF3, ESC "R"},
        {Key::F4, ESC "S"},
        {Key::KeypadF4, ESC "S"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Ansi_CursorKeys[] = {
        {Key::Up, CSI "A"},
        {Key::Down, CSI "B"},
        {Key::Right, CSI "C"},
        {Key::Left, CSI "D"},
        {Key::Home, CSI "H"},
        {Key::End, CSI "F"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Appl_CursorKeys[] = {
        {Key::Up, SS3 "A"},
        {Key::Down, SS3 "B"},
        {Key::Right, SS3 "C"},
        {Key::Left, SS3 "D"},
        {Key::Home, SS3 "H"},
        {Key::End, SS3 "F"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Mod_CursorKeys[] = {
        {Key::Up, CSI "1;" MC "A"},
        {Key::Down, CSI "1;" MC "B"},
        {Key::Right, CSI "1;" MC "C"},
        {Key::Left, CSI "1;" MC "D"},
        {Key::Home, CSI "1;" MC "H"},
        {Key::End, CSI "1;" MC "F"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_VT52_CursorKeys[] = {
        {Key::Up, ESC "A"},
        {Key::Down, ESC "B"},
        {Key::Right, ESC "C"},
        {Key::Left, ESC "D"},
        {Key::Home, ESC "H"},
        {Key::End, ESC "F"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_ReturnKey_ANL[] = {
        {Key::Enter, "\r\n"},
        {Key::KeypadEnter, "\r\n"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_BackspaceKey_BkSp[] = {
        {Key::Backspace, "\b"},
        {Key::Unknown, nullptr},
    };

    static const InputSpec is_Alt_BackspaceKey_BkSp[] = {
        {Key::Backspace, ESC "\b"},
        {Key::Unknown, nullptr},
    };

#undef ESC
#undef CSI
#undef SS3

    inline u8 getModifierCode(VtModifier modifiers) {
        if (modifiers == VtModifier::none) {
            return 0;
        }
        return 1 + (((modifiers & VtModifier::shift) != VtModifier::none) ? 1 : 0) + (((modifiers & VtModifier::alt) != VtModifier::none) ? 2 : 0) + (((modifiers & VtModifier::control) != VtModifier::none) ? 4 : 0) + (((modifiers & VtModifier::super) != VtModifier::none) ? 8 : 0);
    }

    struct KittyKeySpec {
        u32 code = 0;
        char final = 'u';
    };

    static bool isKittyModifierKey(InputKey key) {
        return (key >= InputKey::LeftShift && key <= InputKey::RightSuper) || key == InputKey::CapsLock || key == InputKey::NumLock;
    }

    static bool kittyKeyPreservesViewport(InputKey key) {
        return key == InputKey::CapsLock || key == InputKey::NumLock || isKittyModifierKey(key);
    }

    static bool isKittyRecoveryKey(InputKey key) {
        return key == InputKey::Enter || key == InputKey::Tab || key == InputKey::Backspace;
    }

    static VtModifier kittyToLegacyModifiers(u16 modifiers) {
        VtModifier result = VtModifier::none;
        if (modifiers & 1) {
            result = result | VtModifier::shift;
        }
        if (modifiers & 2) {
            result = result | VtModifier::alt;
        }
        if (modifiers & 4) {
            result = result | VtModifier::control;
        }
        if (modifiers & 8) {
            result = result | VtModifier::super;
        }
        return result;
    }

    static bool validKittyAssociatedText(u32 codepoint) {
        return codepoint >= 0x20 && !(codepoint >= 0x7f && codepoint <= 0x9f);
    }

    static u32 kittyAssociatedText(InputKey key) {
        if (key >= InputKey::Keypad0 && key <= InputKey::Keypad9) {
            return '0' + (u32)(key) - (u32)(InputKey::Keypad0);
        }
        switch (key) {
            case InputKey::Enter:
            case InputKey::KeypadEnter:
                return '\r';
            case InputKey::Tab:
            case InputKey::KeypadTab:
                return '\t';
            case InputKey::Backspace:
                return '\x7f';
            case InputKey::KeypadDecimal:
                return '.';
            case InputKey::KeypadDivide:
                return '/';
            case InputKey::KeypadMultiply:
                return '*';
            case InputKey::KeypadSubtract:
                return '-';
            case InputKey::KeypadAdd:
                return '+';
            case InputKey::KeypadEqual:
                return '=';
            case InputKey::KeypadSeparator:
                return ',';
            case InputKey::KeypadSpace:
                return ' ';
            default:
                return 0;
        }
    }

    static KittyKeySpec kittyKeySpec(InputKey key) {
        using Key = InputKey;
        if (key >= Key::F13 && key <= Key::F35) {
            return {57376 + (u32)(key) - (u32)(Key::F13), 'u'};
        }
        if (key >= Key::MediaPlay && key <= Key::VolumeMute) {
            return {57428 + (u32)(key) - (u32)(Key::MediaPlay), 'u'};
        }
        switch (key) {
            case Key::Enter:
                return {13, 'u'};
            case Key::Backspace:
                return {127, 'u'};
            case Key::Tab:
                return {9, 'u'};
            case Key::Insert:
                return {2, '~'};
            case Key::Delete:
                return {3, '~'};
            case Key::Up:
                return {1, 'A'};
            case Key::Down:
                return {1, 'B'};
            case Key::Right:
                return {1, 'C'};
            case Key::Left:
                return {1, 'D'};
            case Key::Home:
                return {1, 'H'};
            case Key::End:
                return {1, 'F'};
            case Key::PageUp:
                return {5, '~'};
            case Key::PageDown:
                return {6, '~'};
            case Key::Clear:
                return {1, 'E'};
            case Key::F1:
            case Key::KeypadF1:
                return {1, 'P'};
            case Key::F2:
            case Key::KeypadF2:
                return {1, 'Q'};
            case Key::F3:
                return {13, '~'};
            case Key::KeypadF3:
                return {1, 'R'};
            case Key::F4:
            case Key::KeypadF4:
                return {1, 'S'};
            case Key::F5:
                return {15, '~'};
            case Key::F6:
                return {17, '~'};
            case Key::F7:
                return {18, '~'};
            case Key::F8:
                return {19, '~'};
            case Key::F9:
                return {20, '~'};
            case Key::F10:
                return {21, '~'};
            case Key::F11:
                return {23, '~'};
            case Key::F12:
                return {24, '~'};
            case Key::Keypad0:
                return {57399, 'u'};
            case Key::Keypad1:
                return {57400, 'u'};
            case Key::Keypad2:
                return {57401, 'u'};
            case Key::Keypad3:
                return {57402, 'u'};
            case Key::Keypad4:
                return {57403, 'u'};
            case Key::Keypad5:
                return {57404, 'u'};
            case Key::Keypad6:
                return {57405, 'u'};
            case Key::Keypad7:
                return {57406, 'u'};
            case Key::Keypad8:
                return {57407, 'u'};
            case Key::Keypad9:
                return {57408, 'u'};
            case Key::KeypadDecimal:
                return {57409, 'u'};
            case Key::KeypadDivide:
                return {57410, 'u'};
            case Key::KeypadMultiply:
                return {57411, 'u'};
            case Key::KeypadSubtract:
                return {57412, 'u'};
            case Key::KeypadAdd:
                return {57413, 'u'};
            case Key::KeypadEnter:
                return {57414, 'u'};
            case Key::KeypadEqual:
                return {57415, 'u'};
            case Key::KeypadSeparator:
                return {57416, 'u'};
            case Key::KeypadLeft:
                return {57417, 'u'};
            case Key::KeypadRight:
                return {57418, 'u'};
            case Key::KeypadUp:
                return {57419, 'u'};
            case Key::KeypadDown:
                return {57420, 'u'};
            case Key::KeypadPageUp:
                return {57421, 'u'};
            case Key::KeypadPageDown:
                return {57422, 'u'};
            case Key::KeypadHome:
                return {57423, 'u'};
            case Key::KeypadEnd:
                return {57424, 'u'};
            case Key::KeypadInsert:
                return {57425, 'u'};
            case Key::KeypadDelete:
                return {57426, 'u'};
            case Key::KeypadBegin:
                return {57427, 'u'};
            case Key::CapsLock:
                return {57358, 'u'};
            case Key::ScrollLock:
                return {57359, 'u'};
            case Key::NumLock:
                return {57360, 'u'};
            case Key::PrintScreen:
                return {57361, 'u'};
            case Key::Pause:
                return {57362, 'u'};
            case Key::Menu:
                return {57363, 'u'};
            case Key::LeftShift:
                return {57441, 'u'};
            case Key::LeftControl:
                return {57442, 'u'};
            case Key::LeftAlt:
                return {57443, 'u'};
            case Key::LeftSuper:
                return {57444, 'u'};
            case Key::RightShift:
                return {57447, 'u'};
            case Key::RightControl:
                return {57448, 'u'};
            case Key::RightAlt:
                return {57449, 'u'};
            case Key::RightSuper:
                return {57450, 'u'};
            default:
                return {};
        }
    }

    static void makePalette256(const VtConfig& config, Color p[]) {
        memcpy(p, config.palette.colors, sizeof(config.palette.colors));

        for (u8 r = 0; r < 6; ++r) {
            for (u8 g = 0; g < 6; ++g) {
                for (u8 b = 0; b < 6; ++b) {
                    u8 ri = r ? 55 + 40 * r : 0;
                    u8 gi = g ? 55 + 40 * g : 0;
                    u8 bi = b ? 55 + 40 * b : 0;
                    p[16 + 36 * r + 6 * g + b] = {ri, gi, bi};
                }
            }
        }

        for (u8 s = 0; s < 24; ++s) {
            u8 i = 8 + 10 * s;
            p[232 + s] = {i, i, i};
        }
    }

    /* These tables perform translation of built-in "hard" character sets
    * to 16-bit Unicode points. All sets are defined as 96 characters, even
    * those originally designated by DEC as 94-character sets.
    *
    * These tables are referenced by VtermImpl::charCodes (see below).
    */

    static const u16 uc_DecSpec[] = {
        0x0020,
        0x0021,
        0x0022,
        0x0023,
        0x0024,
        0x0025,
        0x0026,
        0x0027,
        0x0028,
        0x0029,
        0x002a,
        0x002b,
        0x002c,
        0x002d,
        0x002e,
        0x002f,
        0x0030,
        0x0031,
        0x0032,
        0x0033,
        0x0034,
        0x0035,
        0x0036,
        0x0037,
        0x0038,
        0x0039,
        0x003a,
        0x003b,
        0x003c,
        0x003d,
        0x003e,
        0x003f,

        0x0040,
        0x0041,
        0x0042,
        0x0043,
        0x0044,
        0x0045,
        0x0046,
        0x0047,
        0x0048,
        0x0049,
        0x004a,
        0x004b,
        0x004c,
        0x004d,
        0x004e,
        0x004f,
        0x0050,
        0x0051,
        0x0052,
        0x0053,
        0x0054,
        0x0055,
        0x0056,
        0x0057,
        0x0058,
        0x0059,
        0x005a,
        0x005b,
        0x005c,
        0x005d,
        0x005e,
        0x0020,

        0x25c6,
        0x2592,
        0x2409,
        0x240c,
        0x240d,
        0x240a,
        0x00b0,
        0x00b1,
        0x2424,
        0x240b,
        0x2518,
        0x2510,
        0x250c,
        0x2514,
        0x253c,
        0x23ba,
        0x23bb,
        0x2500,
        0x23bc,
        0x23bd,
        0x251c,
        0x2524,
        0x2534,
        0x252c,
        0x2502,
        0x2264,
        0x2265,
        0x03c0,
        0x2260,
        0x00a3,
        0x00b7,
        0x0020,
    };

    static const u16 uc_DecSuppl[] = {
        0x0020,
        0x00a1,
        0x00a2,
        0x00a3,
        0x0024,
        0x00a5,
        0x0026,
        0x00a7,
        0x00a4,
        0x00a9,
        0x00aa,
        0x00ab,
        0x002c,
        0x002d,
        0x002e,
        0x002f,
        0x00b0,
        0x00b1,
        0x00b2,
        0x00b3,
        0x0034,
        0x00b5,
        0x00b6,
        0x00b7,
        0x0038,
        0x00b9,
        0x00ba,
        0x00bb,
        0x00bc,
        0x00bd,
        0x003e,
        0x00bf,

        0x00c0,
        0x00c1,
        0x00c2,
        0x00c3,
        0x00c4,
        0x00c5,
        0x00c6,
        0x00c7,
        0x00c8,
        0x00c9,
        0x00ca,
        0x00cb,
        0x00cc,
        0x00cd,
        0x00ce,
        0x00cf,
        0x0050,
        0x00d1,
        0x00d2,
        0x00d3,
        0x00d4,
        0x00d5,
        0x00d6,
        0x0152,
        0x00d8,
        0x00d9,
        0x00da,
        0x00db,
        0x00dc,
        0x0178,
        0x005e,
        0x00df,

        0x00e0,
        0x00e1,
        0x00e2,
        0x00e3,
        0x00e4,
        0x00e5,
        0x00e6,
        0x00e7,
        0x00e8,
        0x00e9,
        0x00ea,
        0x00eb,
        0x00ec,
        0x00ed,
        0x00ee,
        0x00ef,
        0x0070,
        0x00f1,
        0x00f2,
        0x00f3,
        0x00f4,
        0x00f5,
        0x00f6,
        0x0153,
        0x00f8,
        0x00f9,
        0x00fa,
        0x00fb,
        0x00fc,
        0x00ff,
        0x007e,
        0x007f,
    };

    static const u16 uc_DecTechn[] = {
        0x0020,
        0x23b7,
        0x250c,
        0x2500,
        0x2320,
        0x2321,
        0x2502,
        0x23a1,
        0x23a3,
        0x23a4,
        0x23a6,
        0x239b,
        0x239d,
        0x239e,
        0x23a0,
        0x23a8,
        0x23ac,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x0020,
        0x2264,
        0x2260,
        0x2265,
        0x222b,

        0x2234,
        0x221d,
        0x221e,
        0x00f7,
        0x0394,
        0x2207,
        0x03a6,
        0x0393,
        0x223c,
        0x2243,
        0x0398,
        0x00d7,
        0x039b,
        0x21d4,
        0x21d2,
        0x2261,
        0x03a0,
        0x03a8,
        0x0020,
        0x03a3,
        0x0020,
        0x0020,
        0x221a,
        0x03a9,
        0x039e,
        0x03a5,
        0x2282,
        0x2283,
        0x2229,
        0x222a,
        0x2227,
        0x2228,

        0x00ac,
        0x03b1,
        0x03b2,
        0x03c7,
        0x03b4,
        0x03b5,
        0x03c6,
        0x03b3,
        0x03b7,
        0x03b9,
        0x03b8,
        0x03ba,
        0x03bb,
        0x0020,
        0x03bd,
        0x2202,
        0x03c0,
        0x03c8,
        0x03c1,
        0x03c3,
        0x03c4,
        0x0020,
        0x0192,
        0x03c9,
        0x03be,
        0x03c5,
        0x03b6,
        0x2190,
        0x2191,
        0x2192,
        0x2193,
        0x007f,
    };

    static const u16 uc_IsoLatin1[] = {
        0x00a0,
        0x00a1,
        0x00a2,
        0x00a3,
        0x00a4,
        0x00a5,
        0x00a6,
        0x00a7,
        0x00a8,
        0x00a9,
        0x00aa,
        0x00ab,
        0x00ac,
        0x00ad,
        0x00ae,
        0x00af,
        0x00b0,
        0x00b1,
        0x00b2,
        0x00b3,
        0x00b4,
        0x00b5,
        0x00b6,
        0x00b7,
        0x00b8,
        0x00b9,
        0x00ba,
        0x00bb,
        0x00bc,
        0x00bd,
        0x00be,
        0x00bf,

        0x00c0,
        0x00c1,
        0x00c2,
        0x00c3,
        0x00c4,
        0x00c5,
        0x00c6,
        0x00c7,
        0x00c8,
        0x00c9,
        0x00ca,
        0x00cb,
        0x00cc,
        0x00cd,
        0x00ce,
        0x00cf,
        0x00d0,
        0x00d1,
        0x00d2,
        0x00d3,
        0x00d4,
        0x00d5,
        0x00d6,
        0x00d7,
        0x00d8,
        0x00d9,
        0x00da,
        0x00db,
        0x00dc,
        0x00dd,
        0x00de,
        0x00df,

        0x00e0,
        0x00e1,
        0x00e2,
        0x00e3,
        0x00e4,
        0x00e5,
        0x00e6,
        0x00e7,
        0x00e8,
        0x00e9,
        0x00ea,
        0x00eb,
        0x00ec,
        0x00ed,
        0x00ee,
        0x00ef,
        0x00f0,
        0x00f1,
        0x00f2,
        0x00f3,
        0x00f4,
        0x00f5,
        0x00f6,
        0x00f7,
        0x00f8,
        0x00f9,
        0x00fa,
        0x00fb,
        0x00fc,
        0x00fd,
        0x00fe,
        0x00ff,
    };

    static const u16 uc_IsoUK[] = {
        0x0020,
        0x0021,
        0x0022,
        0x00a3,
        0x0024,
        0x0025,
        0x0026,
        0x0027,
        0x0028,
        0x0029,
        0x002a,
        0x002b,
        0x002c,
        0x002d,
        0x002e,
        0x002f,
        0x0030,
        0x0031,
        0x0032,
        0x0033,
        0x0034,
        0x0035,
        0x0036,
        0x0037,
        0x0038,
        0x0039,
        0x003a,
        0x003b,
        0x003c,
        0x003d,
        0x003e,
        0x003f,

        0x0040,
        0x0041,
        0x0042,
        0x0043,
        0x0044,
        0x0045,
        0x0046,
        0x0047,
        0x0048,
        0x0049,
        0x004a,
        0x004b,
        0x004c,
        0x004d,
        0x004e,
        0x004f,
        0x0050,
        0x0051,
        0x0052,
        0x0053,
        0x0054,
        0x0055,
        0x0056,
        0x0057,
        0x0058,
        0x0059,
        0x005a,
        0x005b,
        0x005c,
        0x005d,
        0x005e,
        0x005f,

        0x0060,
        0x0061,
        0x0062,
        0x0063,
        0x0064,
        0x0065,
        0x0066,
        0x0067,
        0x0068,
        0x0069,
        0x006a,
        0x006b,
        0x006c,
        0x006d,
        0x006e,
        0x006f,
        0x0070,
        0x0071,
        0x0072,
        0x0073,
        0x0074,
        0x0075,
        0x0076,
        0x0077,
        0x0078,
        0x0079,
        0x007a,
        0x007b,
        0x007c,
        0x007d,
        0x007e,
        0x007f,
    };

}

const u16* VtermImpl::charCodes[] = {

    nullptr,
    uc_DecSpec,
    uc_DecSuppl,
    uc_DecSuppl,
    uc_DecTechn,
    uc_IsoLatin1,
    uc_IsoUK
};

u32 VtermImpl::translateCharset(Charset charset, unsigned char ch) const {
    const bool userPreference = charset == Charset::DecUserPref;
    if (userPreference) {
        charset = userPreferenceCharset;
    }
    if (charset <= Charset::IsoUK) {
        return charCodes[(u8)(charset)][ch - 32];
    }
    if (!nationalReplacementMode && !userPreference) {
        return ch;
    }

    struct NrcMapping {
        u8 first;
        u16 second;
    };

    const auto lookup = [ch](const NrcMapping* table, size_t size) -> u32 {
        for (size_t index = 0; index < size; ++index) {
            if (table[index].first == ch) {
                return table[index].second;
            }
        }
        return ch;
    };
#define NRC_TABLE(name, ...) static const NrcMapping name[] = {__VA_ARGS__}
    NRC_TABLE(dutch, {'#', 0x00a3}, {'@', 0x00be}, {'[', 0x0133}, {'\\', 0x00bd}, {']', 0x007c}, {'{', 0x00a8}, {'|', 0x0192}, {'}', 0x00bc}, {'~', 0x00b4});
    NRC_TABLE(finnish, {'[', 0x00c4}, {'\\', 0x00d6}, {']', 0x00c5}, {'^', 0x00dc}, {'`', 0x00e9}, {'{', 0x00e4}, {'|', 0x00f6}, {'}', 0x00e5}, {'~', 0x00fc});
    NRC_TABLE(french, {'#', 0x00a3}, {'@', 0x00e0}, {'[', 0x00b0}, {'\\', 0x00e7}, {']', 0x00a7}, {'{', 0x00e9}, {'|', 0x00f9}, {'}', 0x00e8}, {'~', 0x00a8});
    NRC_TABLE(frenchCanadian, {'@', 0x00e0}, {'[', 0x00e2}, {'\\', 0x00e7}, {']', 0x00ea}, {'^', 0x00ee}, {'`', 0x00f4}, {'{', 0x00e9}, {'|', 0x00f9}, {'}', 0x00e8}, {'~', 0x00fb});
    NRC_TABLE(german, {'@', 0x00a7}, {'[', 0x00c4}, {'\\', 0x00d6}, {']', 0x00dc}, {'{', 0x00e4}, {'|', 0x00f6}, {'}', 0x00fc}, {'~', 0x00df});
    NRC_TABLE(italian, {'#', 0x00a3}, {'@', 0x00a7}, {'[', 0x00b0}, {'\\', 0x00e7}, {']', 0x00e9}, {'`', 0x00f9}, {'{', 0x00e0}, {'|', 0x00f2}, {'}', 0x00e8}, {'~', 0x00ec});
    NRC_TABLE(norwegian, {'@', 0x00c4}, {'[', 0x00c6}, {'\\', 0x00d8}, {']', 0x00c5}, {'^', 0x00dc}, {'`', 0x00e4}, {'{', 0x00e6}, {'|', 0x00f8}, {'}', 0x00e5}, {'~', 0x00fc});
    NRC_TABLE(portuguese, {'[', 0x00c3}, {'\\', 0x00c7}, {']', 0x00d5}, {'{', 0x00e3}, {'|', 0x00e7}, {'}', 0x00f5});
    NRC_TABLE(spanish, {'#', 0x00a3}, {'@', 0x00a7}, {'[', 0x00a1}, {'\\', 0x00d1}, {']', 0x00bf}, {'{', 0x00b0}, {'|', 0x00f1}, {'}', 0x00e7});
    NRC_TABLE(swedish, {'@', 0x00c9}, {'[', 0x00c4}, {'\\', 0x00d6}, {']', 0x00c5}, {'^', 0x00dc}, {'`', 0x00e9}, {'{', 0x00e4}, {'|', 0x00f6}, {'}', 0x00e5}, {'~', 0x00fc});
    NRC_TABLE(swiss, {'#', 0x00f9}, {'@', 0x00e0}, {'[', 0x00e9}, {'\\', 0x00e7}, {']', 0x00ea}, {'^', 0x00ee}, {'_', 0x00e8}, {'`', 0x00f4}, {'{', 0x00e4}, {'|', 0x00f6}, {'}', 0x00fc}, {'~', 0x00fb});
    NRC_TABLE(serboCroatian, {'@', 0x017d}, {'[', 0x0160}, {'\\', 0x0110}, {']', 0x0106}, {'^', 0x010c}, {'`', 0x017e}, {'{', 0x0161}, {'|', 0x0111}, {'}', 0x0107}, {'~', 0x010d});
    NRC_TABLE(turkish, {'&', 0x011f}, {'@', 0x0130}, {'[', 0x015e}, {'\\', 0x00d6}, {']', 0x00c7}, {'^', 0x00dc}, {'`', 0x011e}, {'{', 0x015f}, {'|', 0x00f6}, {'}', 0x00e7}, {'~', 0x00fc});
#undef NRC_TABLE

#define LOOKUP(name) return lookup(name, sizeof(name) / sizeof(name[0]))
    switch (charset) {
        case Charset::NrcDutch:
            LOOKUP(dutch);
        case Charset::NrcFinnish:
            LOOKUP(finnish);
        case Charset::NrcFrench:
            LOOKUP(french);
        case Charset::NrcFrenchCanadian:
            LOOKUP(frenchCanadian);
        case Charset::NrcGerman:
            LOOKUP(german);
        case Charset::NrcItalian:
            LOOKUP(italian);
        case Charset::NrcNorwegianDanish:
            LOOKUP(norwegian);
        case Charset::NrcPortuguese:
            LOOKUP(portuguese);
        case Charset::NrcSpanish:
            LOOKUP(spanish);
        case Charset::NrcSwedish:
            LOOKUP(swedish);
        case Charset::NrcSwiss:
            LOOKUP(swiss);
        case Charset::NrcSerboCroatian:
            LOOKUP(serboCroatian);
        case Charset::NrcTurkish:
            LOOKUP(turkish);
        case Charset::NrcGreek: {
            static const u16 greek[] = {0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039a, 0x039b, 0x039c, 0x039d, 0x03a7, 0x039f, 0x03a0, 0x03a1, 0x03a3, 0x03a4, 0x03a5, 0x03a6, 0x039e, 0x03a8, 0x03a9};
            return ch >= 'a' && ch <= 'x' ? greek[ch - 'a'] : ch;
        }
        case Charset::NrcHebrew:
            return ch >= '`' && ch <= 'z' ? 0x05d0 + ch - '`' : ch;
        case Charset::NrcRussian: {
            static const u16 russian[] = {0x042e, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413, 0x0425, 0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f, 0x042f, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412, 0x042c, 0x042b, 0x0417, 0x0428, 0x042d, 0x0429, 0x0427};
            return ch >= '`' && ch <= '~' ? russian[ch - '`'] : ch;
        }
        default:
            return ch;
    }
#undef LOOKUP
}

void VtermImpl::paneResized(const VtGeometry& geometry) {
    pane_ = geometry;
    // Both, unconditionally, exactly as the window resize did: resizeGrid
    // decides for itself whether the grid actually moved, and a caller
    // that tried to decide that here would need its own copy of the
    // comparison - and would get the CSI 48 report wrong, which
    // resizeGrid owes the child even when the grid did not change.
    resizeGrid();
    // T10/A6-4: and every row on top, whether the grid moved or not.
    // A tab is laid out as a whole, so this arrives at every pane of it
    // the moment any one of them is divided, closed or dragged - and the
    // renderer keys its retained cells on the shape of the *frame*, not
    // of one pane. It refuses a reshaped frame that arrives with a
    // partial grid, so the pane whose own grid happened not to change
    // would send nothing, the frame would be refused, and the window
    // would ask for it again forever. Screen::expose, not Vterm::expose:
    // the latter only redraws, which marks no rows (see show()).
    cf->expose();
    redraw();
}

const VtConfig& VtermImpl::config() const {
    return *configSlot_.config;
}

VtermImpl::VtermImpl(ObjPool& owner, VtGeometry& windowGeometry, const VtConfigSlot& configSlot, VtCellExtras& extras, SmallObjAllocator& smallObjects, plt::Scheduler& scheduler, VtHost& host_, const VtGeometry& geometry, PtyHandle& pty, VtermTraceFactory* traceFactory_, Output* dump_)
    : input(this)
    , owner_(owner)
    , windowGeometry_(windowGeometry)
    , configSlot_(configSlot)
    , extras_(extras)
    , smallObjects_(smallObjects)
    , scheduler_(scheduler)
    , host(host_)
    // A8: the first screen is built from the geometry the owner handed
    // over, not from the window - see Vterm::create.
    , pane_(geometry)
    , ptyStream_(pty)
    , ptyOutput_(&ptyStream_)
    , ptyMutex_(scheduler.createMutex(owner))
    , trace(traceFactory_ == nullptr ? nullptr : traceFactory_->construct(createTestApi()))
    , dump(dump_)
    , unicodeProperties(UnicodeMap<u8>::create(owner))
    , parser(Parser::create(&owner, *this, trace, config().osc52SelectClipboard))
    , notifications(&owner)
    , savedPrivModes(&owner)
    , userDefinedKeys(&owner)
    , nColsEff(pane_.columns)
    , hMargin(0)
{
    try {
        createPrimaryScreen();
        createInactiveAlternateScreen();
    } catch (...) {
        delete framePriPool;
        delete frameAltPool;
        throw;
    }
    cf = frame_pri;
    outputRows.grow((size_t)(pane_.rows));
    makePalette256(config(), colors.palette);
    memcpy(originalPalette256, colors.palette, sizeof(originalPalette256));
    colors.defaultForeground = config().fg;
    colors.defaultBackground = config().bg;
    for (auto& special : colors.special) {
        special = config().fg;
    }
    for (auto& special : colors.originalSpecial) {
        special = config().fg;
    }
    cursorColor = config().cr;
    selectionFgColor = config().fg;
    selectionBgColor = config().bg;
    initialModifyKeyResources[0] = 0;
    initialModifyKeyResources[1] = 2;
    initialModifyKeyResources[2] = 2;
    initialModifyKeyResources[3] = 1;
    initialModifyKeyResources[4] = config().modifyOtherKeys;
    initialModifyKeyResources[6] = 0;
    initialModifyKeyResources[7] = 0;
    windowTitle.reset();
    windowTitle.append(config().title.data(), config().title.length());
    iconTitle.reset();
    iconTitle.append(config().title.data(), config().title.length());
    presentedTitle.reset();
    presentedTitle.append(config().title.data(), config().title.length());

    defaultFgPalIx = -1;
    defaultBgPalIx = -1;
    fgPalIx = defaultFgPalIx;
    bgPalIx = defaultBgPalIx;
    extras_.changedListeners.pushBack(owner.make<CallVtermCollectExtras>(this));
}

CallVtermCollectExtras::CallVtermCollectExtras(VtermImpl* terminal_)
    : terminal(terminal_) {
}

void CallVtermCollectExtras::collectExtras(Vector<TerminalCell*>& cells, Vector<u32*>& roots) {
    terminal->collectExtras(cells, roots);
}

void VtermImpl::configChanged() {
    Buffer nextWindowTitle(config().title);
    Buffer nextIconTitle(config().title);
    Buffer nextPresentedTitle(config().title);
    Color nextPalette[256];
    makePalette256(config(), nextPalette);
    memcpy(colors.palette, nextPalette, sizeof(colors.palette));
    memcpy(originalPalette256, nextPalette, sizeof(originalPalette256));
    colors.defaultForeground = config().fg;
    colors.defaultBackground = config().bg;
    for (size_t index = 0; index < TerminalColors::specialCount; ++index) {
        colors.special[index] = config().fg;
        colors.originalSpecial[index] = config().fg;
    }
    cursorColor = config().cr;
    selectionFgColor = config().fg;
    selectionBgColor = config().bg;
    selectionColorMask = 0;
    assignedDefaultColors = false;
    defaultFgPalIx = -1;
    defaultBgPalIx = -1;
    altScrollMode = config().altScrollMode;
    altSendsEscape = config().altSendsEscape;
    initialModifyKeyResources[4] = config().modifyOtherKeys;
    modifyKeyResources[4] = config().modifyOtherKeys;
    modifyOtherKeys = config().modifyOtherKeys;
    parser->setOsc52SelectClipboard(config().osc52SelectClipboard);
    windowTitle.xchg(nextWindowTitle);
    iconTitle.xchg(nextIconTitle);
    presentedTitle.xchg(nextPresentedTitle);
    titleSet = false;
    colors.changed();
    updateExtraCellCount();
    resizeGrid();
    exposeFrames();
    changePresentation();
    redraw();
    host.requestFrame();
    notifyTitleChanged(stringView(presentedTitle));
    if (colorSchemeUpdateMode) {
        reportColorScheme();
    }
}

void VtermImpl::presentationInvalidated() {
    cf->expose();
    redraw();
}

void VtermImpl::show() {
    // Screen::expose, not Vterm::expose: the latter only redraws, which
    // marks no rows, and the renderer needs every row back to shed the
    // outgoing terminal's retained cells.
    cf->expose();
    redraw();
    // No focus here: the presenting client supplies its real focus state;
    // inventing focus-in first would flicker a lie at a child watching for
    // the events.
    // The title itself may not have changed, but the listener now sees
    // this terminal as the visible source and can update window chrome.
    notifyTitleChanged(stringView(presentedTitle));
}

void VtermImpl::hide() {
    // Losing the window is the same event as losing focus, and focus
    // already knows every piece of pointer state that must not survive
    // it: held buttons, an open selection, the autoscroll timer that
    // would otherwise keep scrolling a terminal nobody is looking at, and
    // the half-consumed key state. A background terminal is also
    // genuinely unfocused, so the child hears about it.
    input.focus(false);
}

void VtermImpl::resizeGrid() {
    const ScreenInfo info = cf->info();
    const u16 previousColumns = info.columns;
    const u16 previousRows = info.rows;
    // The history capacity is part of what makes a screen current: a
    // configuration that only changed saveLines still needs the rebuild.
    const bool historyCurrent = cf == frame_alt || info.saveLines == config().saveLines;
    if (previousColumns == pane_.columns && previousRows == pane_.rows && historyCurrent) {
        if (synchronizedOutputMode) {
            setSynchronizedOutput(false);
        }
        if (inBandResizeMode) {
            reportInBandResize();
        }
        return;
    }

    hideCursor();
    resetGraphemeInput();

    Screen::Cursor cursorState{Point(posX, posY), lastCol};
    if (cf == frame_pri) {
        resizeScreen(frame_pri, framePriPool, cursorState, &savedCursorPri);
        cf = frame_pri;
    } else {
        resizeScreen(frame_alt, frameAltPool, cursorState, &savedCursorAlt);
        cf = frame_alt;
    }
    if (synchronizedOutputMode) {
        setSynchronizedOutput(false);
    }
    changePresentation();
    posX = cursorState.position.x;
    posY = cursorState.position.y;
    lastCol = cursorState.pendingWrap;

    // The rebuild resets the vertical scrolling region.  Reset the
    // horizontal region to the resized page as well; retaining a clipped
    // right edge made subsequent growth keep a stale narrow region.
    marginTop = 0;
    marginBottom = pane_.rows;
    nColsEff = pane_.columns;
    hMargin = 0;
    if (tabStopsCustomized && !tabStopsRestored) {
        while (!tabStops.empty() && tabStops.back() >= pane_.columns) {
            tabStops.popBack();
        }
        if (pane_.columns > previousColumns) {
            unsigned column = ((unsigned)(previousColumns) + 7) & ~7u;
            for (; column < pane_.columns; column += 8) {
                tabStops.pushBack((u16)(column));
            }
        }
    }
    const bool pendingWrap = lastCol;
    normalizeCursorPos();
    lastCol = pendingWrap;
    showCursor();

    outputRows.grow((size_t)(pane_.rows));
    updateExtraCellCount();
    refreshBlinkingText();
    if (inBandResizeMode) {
        reportInBandResize();
    }
}

void VtermImpl::getLocalEcho(const u8* const begin, const u8* const end, Buffer& out) {
    StringBuilder output((end - begin) * 2);
    for (const u8* p = begin; p < end; ++p) {
        if (*p == '\r' || *p >= ' ') {
            output.append(p, 1);
        } else {
            const u8 bytes[] = {u8'^', (u8)(*p + 0x40)};
            output.append(bytes, sizeof(bytes));
        }
    }
    out.xchg(output);
}

void VtermImpl::sendKey(InputKey key, VtModifier modifiers_) {
    const UserKey* const userDefined = userDefinedKeys.find((u64)(key));
    if (userDefined != nullptr) {
        sendUserInput(StringView((const u8*)(userDefined->text.data()), userDefined->text.used()));
        return;
    }
    const auto writeKey = [&](const char* data, size_t size) {
        if (!send8BitControls) {
            sendUserInput(StringView((const u8*)(data), size));
            return;
        }
        u8 folded[32];
        STD_INSIST(size <= sizeof(folded));
        size_t output = 0;
        for (size_t input = 0; input < size; ++input) {
            const u8 byte = data[input];
            if (byte == 0x1b && input + 1 < size) {
                const u8 next = data[input + 1];
                if (next >= 0x40 && next <= 0x5f) {
                    folded[output++] = next + 0x40;
                    ++input;
                    continue;
                }
            }
            folded[output++] = byte;
        }
        sendUserInput(StringView(folded, output));
    };
    modifiers = modifiers_;
    const auto& spec = getInputSpec(key);
    if (modifiers == VtModifier::none) {
        writeKey(spec.input, spec.getLength());
    } else {
        char buf[32];
        int k = 0;
        const u8 modifierCode = getModifierCode(modifiers);
        const char* end = spec.input + spec.getLength();
        for (const char* p = spec.input; p != end; ++p) {
            if (*p == *MC) {
                if (modifierCode >= 10) {
                    buf[k++] = '0' + modifierCode / 10;
                }
                buf[k++] = '0' + modifierCode % 10;
            } else {
                buf[k++] = *p;
            }
        }
        buf[k] = '\0';
        writeKey(buf, k);
    }
}

bool VtermImpl::modifyOtherKeyEncoded(u8 ch, VtModifier modifiers_) const {
    if (modifyOtherKeys == 1) {
        return (modifiers_ & VtModifier::control) != VtModifier::none && ch > ' ';
    }
    if (modifyOtherKeys != 2) {
        return false;
    }

    const char* const controlAltOnly = "!#$%&*()-+=?.,:;<>'\"";
    for (const char* current = controlAltOnly; *current != '\0'; ++current) {
        if (ch == (u8)(*current)) {
            return (modifiers_ & (VtModifier::control | VtModifier::alt)) != VtModifier::none;
        }
    }
    return modifiers_ != VtModifier::none;
}

void VtermImpl::sendCharacter(u8 ch, VtModifier modifiers) {
    using VM = VtModifier;

    auto uch = &ch;

    if (eightBitInput && (modifiers & VM::alt) != VM::none) {
        ch |= 0x80;
        sendUserInput(StringView(&ch, 1));
    } else if (modifyOtherKeyEncoded(ch, modifiers)) {
        if (ch < ' ' && (modifiers & VM::control) != VM::none) {
            const char* ctrlmap = ((modifiers & VM::shift) != VM::none) ? "@ABCDEFGHIJKLMNOPQRSTUVWXYZ{|}^/" : " abcdefghijklmnopqrstuvwxyz[\\]^/";
            ch = ctrlmap[ch];
        }

        u8 wbuf[16] = {'\x1b', '[', '2', '7', ';'};
        u8 pos = 5;
        // The modifier code runs up to 16 (all four modifiers): two
        // decimal digits, not a single '0'+code byte.
        const u8 code = getModifierCode(modifiers);
        if (code > 9) {
            wbuf[pos++] = '0' + code / 10;
        }
        wbuf[pos++] = '0' + code % 10;
        wbuf[pos++] = ';';

        if (ch > 99) {
            wbuf[pos] = ch / 100;
            ch -= 100 * wbuf[pos];
            wbuf[pos] += '0';
            ++pos;
        }
        if (pos > 7 || ch > 9) {
            wbuf[pos] = ch / 10;
            ch -= 10 * wbuf[pos];
            wbuf[pos] += '0';
            ++pos;
        }
        wbuf[pos++] = '0' + ch;
        wbuf[pos++] = '~';
        wbuf[pos] = '\0';

        sendUserInput(StringView(wbuf, pos));
    } else if ((modifiers & VM::alt) != VM::none) {
        if (altSendsEscape) {
            static u8 wbuf[2] = {'\x1b', '\0'};
            wbuf[1] = ch;
            sendUserInput(StringView(wbuf, 2));
        } else {
            Buffer utf8_out;
            auto sinkFn = [&](char encoded) {
                utf8_out.append(&encoded, 1);
            };
            Utf8Encoder::pushUnicode(ch | 0x80, sinkFn);
            sendUserInput(StringView((const u8*)(utf8_out.data()), utf8_out.used()));
        }
    } else {
        sendUserInput(StringView(uch, 1));
    }
}

void VtermImpl::writeKittyKey(InputKey key, u16 modifiers, VtermKeyEventType event) {
    const KittyKeySpec spec = kittyKeySpec(key);
    if (!spec.code) {
        return;
    }

    const u8 flags = getKittyKeyboardFlags();
    const bool reportEvent = (flags & 0x02) && event != VtermKeyEventType::Press;
    if (isKittyRecoveryKey(key) && !(flags & 0x08) && !(modifiers & 15)) {
        if (event == VtermKeyEventType::Release) {
            return;
        }
        sendKey(key, kittyToLegacyModifiers(modifiers));
        return;
    }

    if (isKittyModifierKey(key) && !(getKittyKeyboardFlags() & 0x08)) {
        return;
    }

    if (event == VtermKeyEventType::Release && !(getKittyKeyboardFlags() & 0x02)) {
        return;
    }

    const u32 text = kittyAssociatedText(key);
    const bool reportText = (flags & 0x10) && event != VtermKeyEventType::Release && !(modifiers & (4 | 8)) && validKittyAssociatedText(text);
    StringBuilder sequence;
    sequence << StringView(u8"\x1b[");
    if (spec.code != 1 || spec.final == 'u' || modifiers || reportEvent || reportText) {
        sequence << spec.code;
    }
    if (modifiers || reportEvent || reportText) {
        sequence << StringView(u8";");
        if (modifiers || reportEvent) {
            sequence << modifiers + 1;
        }
        if (reportEvent) {
            sequence << StringView(u8":") << (unsigned)(event);
        }
        if (reportText) {
            sequence << StringView(u8";") << text;
        }
    }
    sequence.append(&spec.final, 1);
    sendUserInput(StringView((const u8*)(sequence.data()), sequence.used()), !kittyKeyPreservesViewport(key));
}

void VtermImpl::writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event) {
    if (!key || (event == VtermKeyEventType::Release && !(getKittyKeyboardFlags() & 0x02))) {
        return;
    }

    StringBuilder sequence;
    sequence << StringView(u8"\x1b[") << key;
    if (getKittyKeyboardFlags() & 0x04) {
        const u32 alternateShifted = shiftedKey != key ? shiftedKey : 0;
        const u32 alternateBase = baseLayoutKey != key ? baseLayoutKey : 0;
        if (alternateShifted) {
            sequence << StringView(u8":") << alternateShifted;
            if (alternateBase) {
                sequence << StringView(u8":") << alternateBase;
            }
        } else if (alternateBase) {
            sequence << StringView(u8"::") << alternateBase;
        }
    }
    const u8 flags = getKittyKeyboardFlags();
    const bool reportEvent = (flags & 0x02) && event != VtermKeyEventType::Press;
    const u32 text = (modifiers & 1) && shiftedKey ? shiftedKey : key;
    const bool reportText = (flags & 0x10) && event != VtermKeyEventType::Release && !(modifiers & (4 | 8)) && validKittyAssociatedText(text);
    if (modifiers || reportEvent || reportText) {
        sequence << StringView(u8";");
        if (modifiers || reportEvent) {
            sequence << modifiers + 1;
        }
        if (reportEvent) {
            sequence << StringView(u8":") << (unsigned)(event);
        }
        if (reportText) {
            sequence << StringView(u8";") << text;
        }
    }
    sequence << StringView(u8"u");
    sendUserInput(StringView((const u8*)(sequence.data()), sequence.used()));
}

void VtermImpl::writeCsiResponse(StringView payload) {
    const StringView prefix = send8BitControls ? StringView(u8"\x9b") : StringView(u8"\x1b[");
    writeProtocolResponse(prefix, payload);
}

void VtermImpl::writeDcsResponse(StringView payload) {
    const StringView prefix = send8BitControls ? StringView(u8"\x90") : StringView(u8"\x1bP");
    const StringView suffix = send8BitControls ? StringView(u8"\x9c") : StringView(u8"\x1b\\");
    writeProtocolResponse(prefix, payload, suffix);
}

void VtermImpl::writeOscResponse(StringView payload) {
    const StringView prefix = send8BitControls ? StringView(u8"\x9d") : StringView(u8"\x1b]");
    const StringView suffix = send8BitControls ? StringView(u8"\x9c") : StringView(u8"\x1b\\");
    writeProtocolResponse(prefix, payload, suffix);
}

void VtermImpl::writeProtocolResponse(StringView prefix, StringView payload, StringView suffix) {
    StringBuilder response(static_cast<Buffer&&>(protocolResponseScratch));
    response.reset();
    response << prefix << payload << suffix;
    writePty(StringView((const u8*)(response.data()), response.used()));
    protocolResponseScratch = static_cast<Buffer&&>(response);
}

void VtermImpl::sendUserInput(StringView bytes, bool scrollToBottom) {
    if (bytes.empty()) {
        return;
    }
    if (keyboardLocked) {
        return;
    }

    if (scrollToBottom && cf->scrollView(-0x7fffffff)) {
        refreshBlinkingText();
        redraw();
    }

    if (localEcho) {
        Buffer localEcho;
        getLocalEcho(bytes.data(), bytes.data() + bytes.length(), localEcho);
        processInput((const u8*)(localEcho.data()), (int)(localEcho.used()));
    }
    writePty(bytes);
}

void VtermImpl::writePtyLocked(StringView bytes) {
    ptyOutput_->write(bytes.data(), bytes.length());
    ptyOutput_->flush();
}

void VtermImpl::writePty(StringView bytes) {
    if (bytes.empty()) {
        return;
    }
    plt::Scheduler* const scheduler = &scheduler_;
    if (scheduler->current() != nullptr && ptyMutex_->heldByCurrent()) {
        writePtyLocked(bytes);
        return;
    }
    // The caller must never park on PTY backpressure. A client-owned
    // transaction fiber copies the bytes, serializes them with every other
    // terminal write and waits on the handle's output stream if necessary.
    spawnPtyWrite(bytes);
}

using Key = InputKey;
using Mod = VtModifier;

const VtermImpl::InputSpecTable* VtermImpl::getInputSpecTable() {
    // The table is shared by all instances and must stay instance-agnostic:
    // predicates take the VtermImpl as an argument. Capturing `this` here
    // bound every later instance to the first one's lifetime and state.
    static const InputSpecTable ist[] = {
        {[](const VtermImpl& self) {
        return (self.autoNewlineMode == true);
    }, is_ReturnKey_ANL},

        {[](const VtermImpl& self) {
        return ((self.modifiers & Mod::alt) != Mod::none && self.bkspSendsDel == false);
    }, is_Alt_BackspaceKey_BkSp},

        {[](const VtermImpl& self) {
        return (self.modifyOtherKeys == 2 && self.modifiers != Mod::none);
    }, is_modOtherKeys2},

        {[](const VtermImpl& self) {
        return (self.modifyOtherKeys > 0 && self.modifiers != Mod::none && !(self.altSendsEscape && self.modifiers == Mod::alt));
    }, is_modOtherKeys},

        {[](const VtermImpl& self) {
        return (self.modifyOtherKeys > 0 && (self.modifiers & Mod::control) != Mod::none);
    }, is_Control_modOtherKeys},

        {[](const VtermImpl& self) {
        return (self.altSendsEscape && (self.modifiers & Mod::control_alt) == Mod::control_alt);
    }, is_ControlAlt_altSendsEscape},

        {[](const VtermImpl& self) {
        return (self.altSendsEscape && (self.modifiers & Mod::alt) != Mod::none);
    }, is_Alt_altSendsEscape},

        {[](const VtermImpl& self) {
        return ((self.modifiers & Mod::alt) != Mod::none);
    }, is_Alt},

        {[](const VtermImpl& self) {
        return ((self.modifiers & Mod::control) != Mod::none);
    }, is_Control},

        {[](const VtermImpl& self) {
        return ((self.modifiers & Mod::shift) != Mod::none);
    }, is_Shift},

        {[](const VtermImpl& self) {
        return (self.bkspSendsDel == false);
    }, is_BackspaceKey_BkSp},

        {[](const VtermImpl& self) {
        return (self.compatLevel == CompatibilityLevel::VT52 && self.keypadMode == KeypadMode::Application);
    }, is_VT52_KeypadKeys},
        {[](const VtermImpl& self) {
        return (self.compatLevel == CompatibilityLevel::VT52);
    }, is_VT52_CursorKeys},
        {[](const VtermImpl& self) {
        return (self.compatLevel == CompatibilityLevel::VT52);
    }, is_VT52_FunctionKeys},

        {[](const VtermImpl& self) {
        return (self.modifiers != Mod::none && self.modifyKeyResources[3] != 0 && self.keypadMode == KeypadMode::Application);
    }, is_Mod_Appl_KeypadKeys},
        {[](const VtermImpl& self) {
        return (self.keypadMode == KeypadMode::Application);
    }, is_Appl_KeypadKeys},
        {[](const VtermImpl& self) {
        return (self.modifiers != Mod::none && self.modifyKeyResources[1] != 0);
    }, is_Mod_CursorKeys},
        {[](const VtermImpl& self) {
        return (self.cursorKeyMode == CursorKeyMode::Application);
    }, is_Appl_CursorKeys},

        {[](const VtermImpl& self) {
        return (self.modifiers != Mod::none && self.modifyKeyResources[1] != 0);
    }, is_Mod_Ansi},
        {[](const VtermImpl& self) {
        return (self.modifiers != Mod::none && self.modifyKeyResources[2] != 0);
    }, is_Mod_Ansi_FunctionKeys},

        {[](const VtermImpl&) {
        return true;
    }, is_Ansi},
        {[](const VtermImpl&) {
        return true;
    }, is_Ansi_CursorKeys},
        {[](const VtermImpl&) {
        return true;
    }, is_Ansi_FunctionKeys},
        {[](const VtermImpl&) {
        return true;
    }, is_Ansi_KeypadKeys},

        {[](const VtermImpl&) {
        return true;
    }, nullptr}
    };
    return ist;
}

const VtermImpl::InputSpec* VtermImpl::selectInputSpecs(size_t& cursor) {
    const InputSpecTable* ist = getInputSpecTable();
    for (; ist[cursor].specs != nullptr; ++cursor) {
        if (ist[cursor].predicate(*this)) {
            return ist[cursor++].specs;
        }
    }
    return nullptr;
}

const VtermImpl::InputSpec& VtermImpl::getInputSpec(Key key) {
    static InputSpec nullSpec = {Key::Unknown, ""};

    size_t cursor = 0;
    const InputSpec* specs;
    while ((specs = selectInputSpecs(cursor)) != nullptr) {
        for (int k = 0; specs[k].key != Key::Unknown; ++k) {
            if (specs[k].key == key) {
                return specs[k];
            }
        }
    }

    return nullSpec;
}

bool VtermImpl::presentationChanged() const {
    return presentedRevision != presentationRevision();
}

void VtermImpl::syncPresentationCursor(const TerminalCursor& before) {
    cursorTemporarilyHidden = false;
    const TerminalCursor after = presentationCursor(cf->info().viewOffset);
    if (before.posX != after.posX || before.posY != after.posY || before.style != after.style || !(before.color == after.color)) {
        changePresentation();
    }
}

void VtermImpl::changePresentation() {
    if (++revision == 0) {
        revision = 1;
    }
}

u64 VtermImpl::presentationRevision() const {
    return splitMix64((u64)(revision) << 32 | cf->info().revision);
}

void VtermImpl::parserResetGraphemeInput() {
    if (utf8dec.checkPrematureEOS()) {
        placeGraphicChar();
    }
    resetGraphemeInput();
}

void VtermImpl::parserBell() {
    recordBell();
}

bool VtermImpl::parserAutoNewlineMode() const {
    return autoNewlineMode;
}

CompatibilityLevel VtermImpl::parserCompatibilityLevel() const {
    return compatLevel;
}

void VtermImpl::parserSetCompatibilityLevel(CompatibilityLevel level) {
    compatLevel = level;
    if (level == CompatibilityLevel::VT100) {
        send8BitControls = false;
    }
}

void VtermImpl::parserSet8BitControls(bool enabled) {
    send8BitControls = enabled;
}

void VtermImpl::parserSetApplicationKeypad(bool enabled) {
    keypadMode = enabled ? KeypadMode::Application : KeypadMode::Normal;
}

void VtermImpl::parserMoveCursorBackward(u32 count) {
    moveCursorBackward(count);
}

bool VtermImpl::parserHexTitleInput() const {
    return titleModes & 1;
}

void VtermImpl::parserSingleShift(u8 index) {
    charsetState.ss = index;
}

void VtermImpl::parserLockingShiftGl(u8 index) {
    charsetState.gl = index;
}

void VtermImpl::parserLockingShiftGr(u8 index) {
    charsetState.gr = index;
}

void VtermImpl::parserResetCharsets(bool isoLatin1) {
    charsetState = CharsetState{};
    if (isoLatin1) {
        charsetState.g[charsetState.gr] = Charset::IsoLatin1;
        charsetState.g[3] = Charset::IsoLatin1;
        charsetState.ids[charsetState.gr] = 'A';
        charsetState.ids[3] = 'A';
        charsetState.size96 = (1u << charsetState.gr) | (1u << 3);
    }
}

void VtermImpl::parserDesignateCharset(u8 index, Charset charset, u16 id, bool is96) {
    if (index < 4) {
        charsetState.g[index] = charset;
        charsetState.ids[index] = id;
        const u8 bit = 1u << index;
        charsetState.size96 = is96 ? charsetState.size96 | bit : charsetState.size96 & ~bit;
    }
}

bool VtermImpl::parserHighlightMouseTracking() const {
    return mouseTrk.mode == MouseTrackingMode::VT200_Highlight;
}

namespace {
    [[gnu::always_inline]] static inline size_t printableAsciiPrefix(const u8* input, size_t size);
}

bool VtermImpl::windowOperationsAllowed() const {
    return config().allowWindowOps;
}

void VtermImpl::parserWritePty(StringView bytes) {
    writePty(bytes);
}

bool VtermImpl::parserGroundUtf8Enabled() const {
    return charsetState.g[charsetState.gr] == Charset::UTF8;
}

void VtermImpl::parserGroundHigh(u8 byte) {
    if (charsetState.g[charsetState.gr] == Charset::UTF8) {
        for (int completed = utf8dec.pushByte(byte); completed > 0; --completed) {
            placeGraphicChar();
        }
    } else {
        inputGraphicChar(byte);
    }
}

void VtermImpl::parserGroundAscii(u8 byte) {
    inputGraphicChar(byte);
}

bool VtermImpl::parserUtf8BulkEligible() const {
    return charsetState.ss == 0 && charsetState.g[charsetState.gl] == Charset::UTF8 && charsetState.g[charsetState.gr] == Charset::UTF8;
}

size_t VtermImpl::parserPlaceAscii(StringView bytes) {
    if (utf8dec.expectsContinuation() || charsetState.ss != 0 || charsetState.g[charsetState.gl] != Charset::UTF8) {
        return 0;
    }
    const size_t lines = placeAsciiLines(bytes.data(), bytes.length());
    if (lines != 0) {
        return lines;
    }
    const size_t count = printableAsciiPrefix(bytes.data(), bytes.length());
    if (insertMode) {
        placeAsciiRun<true>(bytes.data(), count);
    } else {
        placeAsciiRun<false>(bytes.data(), count);
    }
    return count;
}

size_t VtermImpl::parserPlaceUtf8Run(StringView bytes, u8& pendingTrace) {
    const int consumed = placeUtf8Run(bytes.data(), bytes.length(), pendingTrace);
    return consumed > 0 ? (size_t)(consumed) : 0;
}

namespace {
    [[gnu::always_inline]] static inline size_t printableAsciiPrefix(const u8* input, size_t size) {
        using Bytes = u8 __attribute__((vector_size(16)));
#if !defined(__SSE2__)
        using Bits = unsigned __int128;
#endif
        constexpr Bytes spaces = {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20};
        constexpr Bytes deletes = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
        size_t offset = 0;
        while (size - offset >= sizeof(Bytes)) {
            Bytes word;
            memcpy(&word, input + offset, sizeof(word));
            const Bytes invalidBytes = (word < spaces) | (word >= deletes);
#if defined(__SSE2__)
            const u32 invalid = _mm_movemask_epi8(__builtin_bit_cast(__m128i, invalidBytes));
            if (invalid != 0) {
                return offset + __builtin_ctz(invalid);
            }
#else
            const Bits invalid = __builtin_bit_cast(Bits, invalidBytes);
            const u64 low = invalid;
            if (low != 0) {
                return offset + __builtin_ctzll(low) / 8;
            }
            const u64 high = invalid >> 64;
            if (high != 0) {
                return offset + 8 + __builtin_ctzll(high) / 8;
            }
#endif
            offset += sizeof(word);
        }
        while (offset < size && input[offset] >= 0x20 && input[offset] < 0x7f) {
            ++offset;
        }
        return offset;
    }
}

size_t VtermImpl::placeAsciiLines(const u8* input, size_t size) {
    if (insertMode || posX != 0 || lastCol || inputGraphemeScreen != nullptr || horizMarginMode || hMargin != 0 || nColsEff != pane_.columns || marginTop != 0 || marginBottom != pane_.rows || semanticUntilEndOfLine) {
        return 0;
    }

    const u8* cursor = input;
    const u8* const end = input + size;
    constexpr u16 maxLines = 256;
    u16 lengths[maxLines];
    u16 lineCount = 0;
    u32 preceding = 0;
    bool havePreceding = false;
    while (cursor != end && lineCount != maxLines) {
        const size_t length = printableAsciiPrefix(cursor, end - cursor);
        if (length > nColsEff || (size_t)(end - cursor) - length < 2 || cursor[length] != '\r' || cursor[length + 1] != '\n') {
            break;
        }
        const u32 row = (u32)(posY) + lineCount;
        if (row < pane_.rows && cf->lineAttribute(row) != 0) {
            break;
        }
        if (length != 0) {
            preceding = cursor[length - 1];
            havePreceding = true;
        }
        lengths[lineCount] = (u16)(length);
        ++lineCount;
        cursor += length + 2;
    }
    if (lineCount < 2) {
        return 0;
    }

    cf->writeAsciiLines(posY, input, lengths, lineCount, attrs, activeHyperlink, currentSemantic, eraseAttrs);
    if (havePreceding) {
        utf8dec.setUnicode(preceding);
    }
    posY = min<u32>((u32)(posY) + lineCount, pane_.rows - 1);
    posX = 0;
    lastCol = false;
    if (attrs.blink) {
        enableBlinkingText();
    }
    return cursor - input;
}

bool VtermImpl::processInput(const u8* input, int inputSize, bool refresh) {
    const StringView slice(input, inputSize);
    return processInput(&slice, 1, refresh);
}

bool VtermImpl::processInput(const StringView* slices, size_t count, bool refresh) {
    ++processInputDepth;
    bool changed;
    try {
        changed = processInputImpl(slices, count, refresh);
    } catch (...) {
        --processInputDepth;
        throw;
    }
    --processInputDepth;
    if (processInputDepth == 0 && refresh) {
        collectCellExtrasIfNeeded();
    }
    return changed;
}

[[gnu::noinline]] bool VtermImpl::processInputImpl(const StringView* slices, size_t count, bool refresh) {
    const TerminalCursor cursorBefore = presentationCursor(cf->info().viewOffset);
    hideCursor();
    for (size_t index = 0; index < count; ++index) {
        parser->feed(slices[index]);
    }
    syncPresentationCursor(cursorBefore);
    const bool changed = presentationChanged();
    if (refresh && changed) {
        redraw();
    }
    return changed;
}

void VtermImpl::getHyperlink(int pX, int pY, Buffer& out) const {
    out.reset();
    const StringView link = resolveHyperlink(pX, pY).payload;
    out.append(link.data(), link.length());
}

Point VtermImpl::selectionPoint(int pX, int pY) const {
    return cf->logicalPoint(mouseSelectionCell(pX, pY, mouseGeometry(pane_, windowGeometry_), pane_.columns, pane_.rows));
}

void VtermImpl::selectStart(int pX, int pY, bool cycleSnapTo) {
    if (cycleSnapTo) {
        selectExtend(pX, pY, true);
        return;
    }

    Point pt = selectionPoint(pX, pY);

    cf->beginSelection(pt);
    selectUpdatesTop = false;
    selectUpdatesLeft = false;
    selectPivotFixed = true;

    changePresentation();
    hideCursor();
    redraw();
}

void VtermImpl::selectExtend(int pX, int pY, bool cycleSnapTo) {
    if (cycleSnapTo) {
        cf->cycleSelectionSnap();
    }
    if (!selectPivotFixed) {
        const Point pt = selectionPoint(pX, pY);
        const Rect selection = cf->currentSelection();
        if (selection.rectangular) {
            selectUpdatesLeft = pt.x < selection.mid().x;
            selectUpdatesTop = pt.y < selection.mid().y;
        } else {
            selectUpdatesLeft = selectUpdatesTop = pt < selection.mid();
        }
        selectPivotFixed = true;
    }
    selectUpdate(pX, pY);
    hideCursor();
}

void VtermImpl::selectUpdate(int pX, int pY) {
    Point pt = selectionPoint(pX, pY);

    Rect selection = cf->currentSelection();

    if (selection.rectangular) {
        if (selectUpdatesLeft && pt.x > selection.br.x) {
            stl::xchg(selection.tl.x, selection.br.x);
            selectUpdatesLeft = false;
        } else if (!selectUpdatesLeft && pt.x < selection.tl.x) {
            stl::xchg(selection.tl.x, selection.br.x);
            selectUpdatesLeft = true;
        }

        if (selectUpdatesTop && pt.y > selection.br.y) {
            stl::xchg(selection.tl.y, selection.br.y);
            selectUpdatesTop = false;
        } else if (!selectUpdatesTop && pt.y < selection.tl.y) {
            stl::xchg(selection.tl.y, selection.br.y);
            selectUpdatesTop = true;
        }

        if (selectUpdatesTop && selectUpdatesLeft) {
            selection.tl = pt;
        } else if (selectUpdatesTop) {
            selection.br.x = pt.x;
            selection.tl.y = pt.y;
        } else if (selectUpdatesLeft) {
            selection.tl.x = pt.x;
            selection.br.y = pt.y;
        } else {
            selection.br = pt;
        }
    } else if (selectUpdatesTop) {
        if (selection.br < pt) {
            selection.tl = selection.br;
            selection.br = pt;
            selectUpdatesTop = selectUpdatesLeft = false;
        } else {
            selection.tl = pt;
        }
    } else {
        if (pt < selection.tl) {
            selection.br = selection.tl;
            selection.tl = pt;
            selectUpdatesTop = selectUpdatesLeft = true;
        } else {
            selection.br = pt;
        }
    }
    cf->updateSelection(selection);
    changePresentation();
    redraw();
}

bool VtermImpl::selectFinish(Buffer& utf8_selection) {
    selectPivotFixed = false;
    changePresentation();
    showCursor();
    redraw();

    return cf->selectedText(utf8_selection);
}

void VtermImpl::selectClear() {
    cf->clearSelection();
    changePresentation();
    redraw();
}

void VtermImpl::selectRectangularModeToggle() {
    Rect selection = cf->currentSelection();
    selection.toggleRectangular();
    if (selection.rectangular && selection.br.x < selection.tl.x) {
        // A valid linear selection is ordered by row and may therefore have
        // its top endpoint to the right of its bottom endpoint.  Rectangular
        // selection requires independently ordered axes.  Preserve which
        // horizontal edge is being dragged while normalizing the corners.
        stl::xchg(selection.tl.x, selection.br.x);
        selectUpdatesLeft = true;
    }
    cf->updateSelection(selection);
    changePresentation();
    redraw();
}

void VtermImpl::pasteSelection(StringView utf8_selection) {
    StringBuilder output(utf8_selection.length() + 12);

    if (bracketedPasteMode) {
        output << StringView(u8"\x1b[200~");
    }

    for (const auto ch : utf8_selection) {
        const char outputByte = ch == '\n' ? '\r' : ch;
        output.append(&outputByte, 1);
    }

    if (bracketedPasteMode) {
        output << StringView(u8"\x1b[201~");
    }

    if (!output.empty()) {
        sendUserInput(StringView((const u8*)(output.data()), output.used()));
    }
}

Vterm* Vterm::create(ObjPool& owner, VtGeometry& windowGeometry, const VtConfigSlot& configSlot, VtCellExtras& extras, SmallObjAllocator& smallObjects, plt::Scheduler& scheduler, VtHost& host, const VtGeometry& geometry, PtyHandle& pty, VtermTraceFactory* traceFactory) {
    const VtConfig& config = *configSlot.config;
    Output* dump = nullptr;
    if (!config.dump.empty()) {
        const int rawFd = ::open((const char*)(config.dump.data()), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (rawFd < 0) {
            Errno().raise(StringBuilder() << StringView(u8"can not open dump file ") << config.dump);
        }
        auto* fd = owner.make<ScopedFD>(rawFd);
        dump = createOutBuf(&owner, *createFDRegular(&owner, *fd));
    }

    // Nothing sizes the extra store from `geometry` here, and that is the
    // point (R5-qa, Q2): a pane's grid is the wrong base for a store the
    // whole window shares, and this seeding was redundant anyway -
    // resetTerminal() below reaches updateExtraCellCount() before create()
    // returns, and that is the one place the store's budget comes from.
    //
    // Resize and invalidation delivery belongs to whoever owns the
    // terminal's lifetime - the session set, or the headless host -
    // because the embedder's listener lists have no way out for a
    // registration whose session died. The same owner keeps the pointer:
    // a freshly built terminal is nobody's active one.
    VtermImpl* const vterm = owner.make<VtermImpl>(owner, windowGeometry, configSlot, extras, smallObjects, scheduler, host, geometry, pty, traceFactory, dump);
    vterm->resetTerminal();
    vterm->startTimers();
    return vterm;
}
