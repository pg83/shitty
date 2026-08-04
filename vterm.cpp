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
#include "vterm_trace.h"

#include "vterm_test.h"
#include "application.h"
#include "base64.h"
#include "cell_extra_store.h"
#include "color_spec.h"
#include "composer.h"
#include "input_bindings.h"
#include "input_handler.h"
#include "keyboard.h"
#include "mouse_frontend.h"
#include "mouse_protocol.h"
#include "parser.h"
#include "pty.h"
#include "screen.h"
#include "unicode_map.h"
#include "grapheme.h"

#include <utf8proc.h>
#include "hex.h"
#include "listener.h"
#include "options.h"
#include "utf8.h"
#include "utf8_dfa.h"

#include <plt/clipboard.h>
#include <plt/drop.h>
#include <plt/fiber.h>
#include <plt/mutex.h>
#include <plt/platform.h>
#include <plt/poller.h>
#include <plt/window.h>

#include <std/alg/minmax.h>
#include <std/alg/xchg.h>
#include <std/dbg/assert.h>
#include <std/mem/obj_pool.h>
#include <std/ios/input.h>
#include <std/ptr/scoped.h>
#include <std/mem/small_obj_allocator.h>
#include <std/thr/runable.h>
#include <std/rng/split_mix_64.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/sys/types.h>
#include <std/ios/out.h>
#include <std/ios/output.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/sys/fd.h>
#include <std/sys/throw.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <sys/types.h>

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
    constexpr u64 selectionAutoscrollInterval = 50'000;

    StringView stringView(const std::string& value) {
        return StringView((const u8*)(value.data()), value.size());
    }

    StringView semanticOption(StringView payload, StringView name) {
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

    bool kittyClipboardMimeSupported(StringView mimeType) {
        return mimeType.empty() || mimeType == StringView(u8"text/plain") || mimeType == StringView(u8"text/plain;charset=utf-8") || mimeType == StringView(u8"UTF8_STRING") || mimeType == StringView(u8"STRING") || mimeType == StringView(u8"TEXT");
    }

    StringView selectKittyClipboardMime(StringView mimeTypes) {
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

    void copyKittyClipboardId(Buffer& output, StringView input) {
        output.reset();
        for (const u8 byte : input) {
            if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '+' || byte == '.') {
                output.append(&byte, 1);
            }
        }
    }

    void writeKittyClipboardPacket(Output& output, bool send8BitControls, StringView type, StringView status, StringView id = {}, StringView mimeType = {}, StringView payload = {}, bool primary = false) {
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

    // One spawnable unit for a clipboard transaction; recycled through the
    // terminal free list because transactions come and go with every paste.
    struct FiberBlock {
        FiberBlock* next = nullptr;
        alignas(16) u8 stack[plt::lightFiberStack];
    };

    struct VtermImpl;

    // A one-shot fiber body carved out of the small-object allocator;
    // releases itself and recycles its stack when the fiber finishes.
    template <typename F>
    struct FiberTask final: public Runable {
        FiberTask(VtermImpl* terminal_, FiberBlock* block_, F&& body_)
            : terminal(terminal_)
            , block(block_)
            , body(static_cast<F&&>(body_))
        {
        }

        void run() override;

        VtermImpl* terminal;
        FiberBlock* block;
        F body;
    };

    plt::Clipboard* selectionTarget(Composer& composer, bool primary) {
        return primary ? composer.window->primary() : composer.window->secondary();
    }

    void writeSelection(plt::Clipboard& clipboard, StringView content) {
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

    struct CallVtermResize: Listener {
        explicit CallVtermResize(VtermImpl* parent);

        void onListen(void*) override;

        VtermImpl* parent;
    };

    struct CallVtermFontChanged: Listener {
        explicit CallVtermFontChanged(VtermImpl* parent);

        void onListen(void*) override;

        VtermImpl* parent;
    };

    struct CallVtermInputAction: Listener {
        CallVtermInputAction(VtermImpl* parent, InputActions action);

        void onListen(void*) override;

        VtermImpl* parent;
        InputActions action;
    };

    // A permanent timer fiber body dispatching to one VtermImpl method.
    struct VtermTimerBody final: public Runable {
        VtermTimerBody(VtermImpl* parent, void (VtermImpl::*method)());

        void run() override;

        VtermImpl* parent;
        void (VtermImpl::*method)();
    };

    struct VtermImpl final: public Vterm, public InputHandler, public ParserIface {
        VtermImpl(Composer& composer, VtermTraceFactory* traceFactory, Output* dump);

        ~VtermImpl();

        void feedPty(StringView bytes) override;
        void expose() override;
        void focus(bool focused) override;
        bool key(const KeyInput& input) override;
        bool text(const TextInput& input) override;
        bool pointerMotion(const PointerMotionInput& input) override;
        bool pointerButton(const PointerButtonInput& input) override;
        bool scroll(const ScrollInput& input) override;
        void pointerPresence(bool present) override;
        void flush() override;
        void key(InputKey key, VtModifier modifiers);
        void character(u8 byte, VtModifier modifiers);
        void sendBytes(StringView bytes, bool userInput) override;
        void kittyKey(InputKey key, u16 modifiers, VtermKeyEventType event);
        void kittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event);
        bool mouseHighlightRelease(u16 endX, u16 endY, u16 mouseX, u16 mouseY);
        void locatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons);
        void locatorButton(u8 button, bool pressed);
        void scrollUp(u16 count);
        void scrollDown(u16 count);
        void pageUp();
        void pageDown();
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
        bool expireSynchronizedOutput(bool force) override;
        bool advanceAnimation(bool force) override;
        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd) override;
        void dropText(Input& source) override;
        void dropUriList(Input& source) override;
        void dropBuffered(StringView text);
        bool bufferDropPayload(Input& source, Buffer& content);
        void buildQuotedEntry(StringView entry, StringBuilder& quoted);
        const TerminalUpdate* output() override;
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

        void resizeGrid();
        void fontChanged();
        void wireInputBindings();
        void createPrimaryScreen();
        void createAlternateScreen();
        void createInactiveAlternateScreen();
        void resizeScreen(Screen*& frame, ObjPool*& pool, Screen::Cursor& cursor);

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

        int writePty(InputKey key, VtModifier modifiers = VtModifier::none, bool userInput = true);
        int writePty(u8 ch, VtModifier modifiers = VtModifier::none, bool userInput = true);
        int writePty(const char* cstr, bool userInput = false);
        int writePty(const char* data, size_t size, bool userInput);
        int writePty(const u8* ucstr, size_t len, bool userInput = false);
        bool modifyOtherKeyEncoded(u8 ch, VtModifier modifiers) const;
        void writeProtocolResponse(StringView prefix, StringView payload, StringView suffix = {});
        int writeKittyKey(InputKey key, u16 modifiers, VtermKeyEventType event);
        int writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event);
        u8 getKittyKeyboardFlags() const;

        void setLocatorPosition(u16 column, u16 row, u16 pixelX, u16 pixelY, u8 buttons = 0);
        void reportLocatorButton(u8 button, bool pressed);

        void setHasFocus(bool);
        std::string getHyperlink(int pX, int pY) const;
        void mouseWheelUp(u16 count = 1);
        void mouseWheelDown(u16 count = 1);
        void mouseWheelRight(u16 count = 1);
        void mouseWheelLeft(u16 count = 1);
        void selectStart(int pX, int pY, bool cycleSnapTo);
        void selectExtend(int pX, int pY, bool cycleSnapTo);
        void selectUpdate(int pX, int pY);
        bool selectFinish(std::string& utf8_selection);
        void selectClear();
        void selectRectangularModeToggle();

        void pasteSelection(const std::string& utf8_selection);

        Point selectionPoint(int pX, int pY) const;
        std::string getLocalEcho(const u8* const begin, const u8* const end);
        template <typename F>
        void spawnTransaction(F&& body);
        void spawnPtyWrite(StringView bytes);

        bool processInput(const u8* input, int size, bool refresh = true);
        [[gnu::noinline]] bool processInputImpl(const u8* input, int size, bool refresh);

        bool presentationChanged() const;
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
        void updateExtraCellCount();

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
        void setMouseEncoding(MouseTrackingEnc encoding) override;
        void setAlternateScroll(bool enabled) override;
        void setEightBitInput(bool enabled) override;
        void setAltSendsEscape(bool enabled) override;
        void setSavedAlternateScreen(bool enabled) override;
        void setExtendedReverseWrap(bool enabled) override;
        void setBracketedPaste(bool enabled) override;
        void setSynchronizedOutput(bool enabled) override;
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
        void osc_PROGRESS(u32, u32) override;
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
        alignas(16) u8 syncStack_[plt::lightFiberStack];
        alignas(16) u8 blinkStack_[plt::lightFiberStack];
        alignas(16) u8 autoscrollStack_[plt::lightFiberStack];
        IntrusiveList copyListeners;
        IntrusiveList pasteListeners;
        IntrusiveList pastePrimaryListeners;
        IntrusiveList pageUpListeners;
        IntrusiveList pageDownListeners;
        Composer& composer;
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

        std::string inputResult;
        bool outputPending = false;
        Screen* updateScreen = nullptr;
        u64 presentedRevision = 0;
        u64 updatingRevision = 0;
        u32 revision = 1;

        Vector<TerminalCell*> extraCells;
        u32 processInputDepth = 0;
        FiberBlock* fiberBlocks_ = nullptr;
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
        std::string windowTitle;
        std::string iconTitle;
        bool titleSet = false;
        u8 titleModes = 0;

        struct SavedTitles {
            bool hasIcon = false;
            bool hasWindow = false;
            std::string icon;
            std::string window;
        };

        std::vector<SavedTitles> titleStack;

        struct NotificationPart {
            std::string text;
            size_t encodedOffset = (size_t)-1;
        };

        struct Notification {
            NotificationPart title;
            NotificationPart body;
        };

        std::map<std::string, Notification> notifications;
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
        bool rectangularAttributeExtent = false;
        u8 checksumFlags = 0;
        bool bkspSendsDel = true;
        bool localEcho = false;
        bool bracketedPasteMode = false;
        bool synchronizedOutputMode = false;
        bool colorSchemeUpdateMode = false;
        bool inBandResizeMode = false;
        bool pasteMimeNotificationsMode = false;
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
        std::map<u32, bool> savedPrivModes;
        std::map<InputKey, std::string> userDefinedKeys;
        bool userDefinedKeysLocked = false;
        Buffer kittyClipboardWriteId;
        // The open write transaction; deleting it without finish() aborts.
        stl::Output* kittyClipboardWriteStream = nullptr;
        size_t kittyClipboardWriteLength = 0;

        struct KittyKeyboardState {
            u8 flags = 0;
            std::vector<u8> stack;
        };

        KittyKeyboardState kittyKeyboardPri;
        KittyKeyboardState kittyKeyboardAlt;

        KittyKeyboardState& kittyKeyboardState();
        const KittyKeyboardState& kittyKeyboardState() const;

        bool horizMarginMode = false;
        u16 nColsEff = 0;
        u16 hMargin = 0;

        std::vector<u16> tabStops;
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
    body();
    VtermImpl* const owner = terminal;
    FiberBlock* const spent = block;
    owner->composer.smallObjects->release(this);
    // Still running on spent's stack: safe, nothing can reuse it before
    // the final cooperative switch out.
    spent->next = owner->fiberBlocks_;
    owner->fiberBlocks_ = spent;
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
        block = composer.pool->make<FiberBlock>();
    }
    FiberTask<F>* const task = composer.smallObjects->make<FiberTask<F>>(this, block, static_cast<F&&>(body));
    composer.platform->scheduler()->spawn(*task, block->stack, sizeof(block->stack));
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
        const plt::LockGuard guard(*composer.ptyMutex, *composer.platform->scheduler());
        composer.ptyOutput->write(data, view.length());
        composer.ptyOutput->flush();
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
    Composer& composer = terminal->composer;
    const bool bracketed = terminal->bracketedPasteMode;
    terminal->spawnTransaction([&composer, primary, bracketed] {
        const plt::LockGuard guard(*composer.ptyMutex, *composer.platform->scheduler());
        const ScopedPtr<Input> source{selectionTarget(composer, primary)->read()};
        PasteOutput paste(composer.pty->output(), bracketed);
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
    Composer& composer = terminal->composer;
    terminal->spawnTransaction([&composer] {
        const ScopedPtr<Input> source{composer.window->primary()->read()};
        const ScopedPtr<Output> target{composer.window->secondary()->write()};
        for (;;) {
            u8 chunk[8 * 1024];
            const size_t count = source->read(chunk, sizeof(chunk));
            if (count == 0) {
                break;
            }
            target->write(chunk, count);
        }
        target->finish();
    });
    return true;
}

ScreenHyperlink VtermInput::resolveLink(int pixelX, int pixelY) {
    return terminal->resolveHyperlink(pixelX, pixelY);
}

bool VtermInput::refreshHyperlink() {
    ScreenHyperlink next;
    if (pointerFocused && pointerPresent && pointerPositionKnown && (pointerModifiers & InputControl)) {
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
        terminal->composer.window->requestPointerIcon(active ? plt::PointerIcon::Pointer : plt::PointerIcon::Text);
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
    refreshHyperlinkAndRedraw();
}

int VtermInput::currentSelectionAutoscrollDirection() const {
    const unsigned selectionButtons = (1u << (unsigned)(PointerButton::Primary)) | (1u << (unsigned)(PointerButton::Secondary));
    if (!mouse.selectionOngoing() || !(mouse.buttons() & selectionButtons) || terminal->cf->currentSelection().null() || !pointerFocused || !pointerPresent || !pointerPositionKnown) {
        return 0;
    }
    const int top = opts.border;
    const int bottom = max(top, (int)(terminal->composer.pixelHeight) - opts.border - 1);
    if (pointerY <= top) {
        return -1;
    }
    if (pointerY >= bottom) {
        return 1;
    }
    return 0;
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
    terminal->writeKittyKey(pending.primary, 0, pending.base, pending.modifiers, pending.event);
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
        const bool baseLayoutShortcut =
            opts.kittyCtrlBaseLayout && (kittyFlags & 0x04) && (textMods & 4) &&
            layoutKey >= 0x80 &&
            input.baseCodepoint >= 0x20 && input.baseCodepoint < 0x7f;
        // Compatibility for consumers that ignore Kitty's base-layout field.
        const u32 primaryKey = baseLayoutShortcut ? input.baseCodepoint : layoutKey;
        const bool reportEvent = (kittyFlags & 0x02) && event != VtermKeyEventType::Press;
        if (primaryKey && ((textMods & (2 | 4 | 8)) || (kittyFlags & 0x08) || reportEvent)) {
            if (pressed && !(textMods & (2 | 4 | 8))) {
                pendingTextKey = {true, primaryKey, input.baseCodepoint, kittyMods, event};
                return true;
            }
            terminal->writeKittyKey(primaryKey, 0, input.baseCodepoint, kittyMods, event);
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
    if ((input.modifiers & InputNumLock) && input.key >= InputKey::Keypad0 && input.key <= InputKey::KeypadEqual) {
        static const u8 keypad[] = {
            '0',
            '1',
            '2',
            '3',
            '4',
            '5',
            '6',
            '7',
            '8',
            '9',
            '.',
            '/',
            '*',
            '-',
            '+',
            '\r',
            '=',
        };
        terminal->writePty(keypad[(u8)(input.key) - (u8)(InputKey::Keypad0)], modifiers, true);
        return true;
    }
    if (input.key == InputKey::Escape) {
        terminal->writePty((u8)('\x1b'), modifiers, true);
        return true;
    }
    if (input.key != InputKey::Unknown && input.key != InputKey::Printable && input.key != InputKey::Space) {
        if (input.key == InputKey::Tab && (modifiers & VtModifier::shift) != VtModifier::none) {
            if ((modifiers & VtModifier::alt) != VtModifier::none) {
                terminal->writePty((u8)('\x1b'), VtModifier::none, true);
            }
            terminal->writePty(InputKey::Tab, VtModifier::shift, true);
            return true;
        }
        if (input.key == InputKey::Backspace && (modifiers & VtModifier::control) != VtModifier::none) {
            terminal->writePty((u8)(terminal->bkspSendsDel ? '\b' : '\x7f'), VtModifier::none, true);
            return true;
        }
        terminal->writePty(input.key, modifiers, true);
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
            terminal->writePty((u8)(ruleKey), modifiers, true);
            return true;
        }
        int controlKey = (int)(ruleKey);
        if (controlKey >= 'a' && controlKey <= 'z') {
            controlKey -= 'a' - 'A';
        }
        u8 character = 0;
        if (controlCharacter(controlKey, input.modifiers & InputShift, character)) {
            terminal->writePty(character, modifiers, true);
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
    const VtModifier modifiers = legacyModifiers(input.modifiers);
    if (input.codepoint < 0x80) {
        terminal->writePty((u8)(input.codepoint), modifiers, true);
        return true;
    }
    u8 encoded[4];
    size_t size = 0;
    Utf8Encoder::pushUnicode(input.codepoint, [&](u8 byte) {
        encoded[size++] = byte;
    });
    if ((input.modifiers & InputAlt) && terminal->altSendsEscape) {
        terminal->writePty((const u8*)("\x1b"), 1, true);
    }
    terminal->writePty(encoded, size, true);
    return true;
}

void VtermInput::mouseProtocolCoordinates(MouseTrackingEnc encoding, int pixelX, int pixelY, u16& column, u16& row) const {
    const MouseGeometry geometry = {terminal->composer.pixelWidth, terminal->composer.pixelHeight, opts.border, terminal->composer.glyphWidth, terminal->composer.glyphHeight};
    const MouseProtocolPoint point = mouseProtocolPoint(encoding, pixelX, pixelY, geometry);
    column = point.column;
    row = point.row;
}

void VtermInput::sendMouseProtocol(MouseTrackingEnc encoding, MouseEventType type, u16 modifiers, int button, int column, int row) {
    const unsigned protocolModifiers = mouseProtocolModifiers(modifiers);
    StringBuilder report;
    if (encodeMouseProtocol(report, encoding, type, protocolModifiers, mouse.motionButton(), button, column, row)) {
        terminal->writePty((const u8*)(report.data()), report.used(), false);
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
    terminal->setLocatorPosition(locatorColumn, locatorRow, std::max(1, input.pixelX + 1), std::max(1, input.pixelY + 1), 0);
    if (protocolButton >= 1 && protocolButton <= 4) {
        terminal->reportLocatorButton(protocolButton, input.pressed);
    }
    if (!input.pressed && input.button == PointerButton::Primary && hyperlinkClick) {
        hyperlinkClick = false;
        return true;
    }
    if (input.pressed && input.button == PointerButton::Primary) {
        hyperlinkClick = false;
        if (input.modifiers & InputControl) {
            const ScreenHyperlink link = resolveLink(input.pixelX, input.pixelY);
            if (!link.payload.empty()) {
                hyperlinkClick = true;
                terminal->composer.window->requestOpenUri(link.payload);
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
        Composer& composer = terminal->composer;
        if (selected.status) {
            writeSelection(*composer.window->primary(), selected.text);
            if (opts.autoCopyMode) {
                writeSelection(*composer.window->secondary(), selected.text);
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
    terminal->setLocatorPosition(locatorColumn, locatorRow, std::max(1, input.pixelX + 1), std::max(1, input.pixelY + 1), 0);
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
        screen = Screen::createPrimary(composer, *next, composer.columns, composer.rows, &colors, opts.saveLines);
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
        screen = Screen::createAlternate(composer, *next, composer.columns, composer.rows, &colors);
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
        screen = Screen::createInactiveAlternate(composer, *next);
    } catch (...) {
        delete next;
        throw;
    }
    delete frameAltPool;
    frameAltPool = next;
    frame_alt = screen;
}

void VtermImpl::resizeScreen(Screen*& frame, ObjPool*& pool, Screen::Cursor& cursor) {
    ObjPool* const next = ObjPool::fromMemoryRaw();
    Screen* screen;
    try {
        screen = frame->resized(*next, composer.columns, composer.rows, cursor);
    } catch (...) {
        delete next;
        throw;
    }
    delete pool;
    pool = next;
    frame = screen;
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
        composer.window->requestFrame();
    }
}

void VtermImpl::expose() {
    redraw();
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
    composer.window->requestFrame();
}

void VtermImpl::key(InputKey key_, VtModifier modifiers_) {
    writePty(key_, modifiers_, true);
}

void VtermImpl::character(u8 byte, VtModifier modifiers_) {
    writePty(byte, modifiers_, true);
}

void VtermImpl::sendBytes(StringView bytes, bool userInput) {
    writePty(bytes.data(), bytes.length(), userInput);
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
    inputResult.clear();
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
    pasteSelection(std::string((const char*)(text.data()), text.length()));
}

ScreenHyperlink VtermImpl::resolveHyperlink(int pixelX, int pixelY) const {
    if (pixelX < opts.border || pixelY < opts.border || pixelX >= composer.pixelWidth - opts.border || pixelY >= composer.pixelHeight - opts.border) {
        return {};
    }
    const u16 column = (pixelX - opts.border) / composer.glyphWidth;
    const u16 row = (pixelY - opts.border) / composer.glyphHeight;
    const ScreenInfo info = cf->info();
    if (column >= info.columns || row >= info.rows) {
        return {};
    }
    return cf->hyperlinkAt(row, column);
}

StringView VtermImpl::hyperlinkAt(int pixelX, int pixelY) {
    const StringView payload = resolveHyperlink(pixelX, pixelY).payload;
    if (payload.empty()) {
        inputResult.clear();
    } else {
        inputResult.assign((const char*)(payload.data()), payload.length());
    }
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
        const plt::LockGuard guard(*composer.ptyMutex, *composer.platform->scheduler());
        PasteOutput paste(composer.ptyOutput, bracketed);
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
    plt::Scheduler* const scheduler = composer.platform->scheduler();
    if (scheduler->current() == nullptr) {
        Buffer content;
        if (bufferDropPayload(source, content)) {
            dropBuffered(StringView(content));
        }
        return;
    }
    // The stream is pulled on this fiber under the mutex: backpressure
    // propagates to the drag source instead of ballooning a buffer.
    const plt::LockGuard guard(*composer.ptyMutex, *scheduler);
    PasteOutput paste(composer.ptyOutput, bracketedPasteMode);
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
    plt::Scheduler* const scheduler = composer.platform->scheduler();
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
    const plt::LockGuard guard(*composer.ptyMutex, *scheduler);
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
            PasteOutput paste(composer.ptyOutput, bracketedPasteMode);
            paste.write(quoted.data(), quoted.used());
        }
        Buffer tail(StringView((const u8*)(pending.data()) + boundary, pending.length() - boundary));
        stl::xchg(pending, tail);
    }
}

void VtermImpl::preedit(StringView text, i32 cursorBegin, i32 cursorEnd) {
    preeditCells.clear();
    preeditCursorBeginCell = -1;
    preeditCursorEndCell = -1;

    const auto* bytes = (const utf8proc_uint8_t*)(text.data());
    utf8proc_ssize_t remaining = (utf8proc_ssize_t)(text.length());
    i32 offset = 0;
    while (remaining > 0) {
        utf8proc_int32_t codepoint = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes, remaining, &codepoint);
        if (consumed <= 0) {
            break;
        }
        if (cursorBegin >= 0 && preeditCursorBeginCell < 0 && offset >= cursorBegin) {
            preeditCursorBeginCell = (i32)(preeditCells.length());
        }
        const int width = codepointWidth((u32)(codepoint));
        if (width > 0) {
            TerminalCell cell{};
            cell.uc_pt = (u32)(codepoint);
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

void VtermImpl::overlayPreedit(TerminalUpdate& update) {
    if (preeditCells.empty()) {
        return;
    }
    const i32 row = (i32)(posY) + (i32)(update.viewOffset);
    if (row < 0 || row >= (i32)(composer.rows) || cf->lineAttribute(posY) != 0) {
        return;
    }
    const i32 columns = composer.columns;
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
    result.synchronizedOutput = synchronizedOutputMode;
    return result;
}

TestApi* VtermImpl::createTestApi() {
#ifdef SHITTY_FOR_TESTS
    return composer.pool->make<TestApiImpl>(this);
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
        result.rectangleOrigin = {0, 0, vterm->composer.rows, vterm->composer.columns};
    }
    result.hyperlinkCount = vterm->composer.cellExtras->hyperlinkCount();
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

bool TestApiImpl::tabStop(u16 column) const {
    if (column >= vterm->composer.columns) {
        return false;
    }
    if (!vterm->tabStopsCustomized) {
        return column % 8 == 0;
    }
    return std::find(vterm->tabStops.begin(), vterm->tabStops.end(), column) != vterm->tabStops.end();
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
    CellExtraStore& extras = *vterm->composer.cellExtras;
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
    CellExtraStore& extras = *vterm->composer.cellExtras;
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
    plt::Scheduler* const scheduler = composer.platform->scheduler();
    scheduler->spawn(syncBody_, syncStack_, sizeof(syncStack_));
    scheduler->spawn(blinkBody_, blinkStack_, sizeof(blinkStack_));
    scheduler->spawn(autoscrollBody_, autoscrollStack_, sizeof(autoscrollStack_));
}

void VtermImpl::runSyncWatchdog() {
    plt::Fiber* const self = composer.platform->scheduler()->current();
    syncFiber_ = self;
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
            composer.window->requestFrame();
        }
    }
}

void VtermImpl::runBlink() {
    plt::Fiber* const self = composer.platform->scheduler()->current();
    blinkFiber_ = self;
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
            composer.window->requestFrame();
        }
    }
}

void VtermImpl::runAutoscroll() {
    plt::Fiber* const self = composer.platform->scheduler()->current();
    autoscrollFiber_ = self;
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
        composer.window->requestFrame();
    }
}

size_t VtermInputSpec::getLength() const {
    return length ? length : strlen(input);
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

void VtermImpl::updateExtraCellCount() {
    size_t count = frame_pri->info().cellCapacity;
    if (altScreenInitialized) {
        count += frame_alt->info().cellCapacity;
    }
    composer.cellExtras->setCellCount(count);
}

void VtermImpl::collectCellExtrasIfNeeded(bool force) {
    CellExtraStore& extras = *composer.cellExtras;
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

void VtermImpl::collectCellExtras() {
    extraCells.clear();
    u32* roots[2];
    size_t rootCount = 0;
    if (activeHyperlink != 0) {
        roots[rootCount++] = &activeHyperlink;
    }
    if (inputGraphemeScreen != nullptr && inputGraphemeHyperlink != 0) {
        roots[rootCount++] = &inputGraphemeHyperlink;
    }
    frame_pri->collectExtraCells(extraCells);
    if (altScreenInitialized) {
        frame_alt->collectExtraCells(extraCells);
    }

    CellExtraStore& extras = *composer.cellExtras;
    extras.collect(extraCells, roots, rootCount);
    extraCells.clear();
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
    endY = std::clamp(endY, mouseHighlight.firstRow, mouseHighlight.lastRow);
    StringBuilder response;
    if (mouseTrk.enc == MouseTrackingEnc::SGR || mouseTrk.enc == MouseTrackingEnc::SGRPixels) {
        response << StringView(u8"<") << mouseHighlight.startX << StringView(u8";") << mouseHighlight.startY << StringView(u8";") << endX << StringView(u8";") << endY << StringView(u8";") << mouseX << StringView(u8";") << mouseY << StringView(u8"T");
    } else {
        const auto coordinate = [](u16 value) {
            return (u8)(32 + std::clamp<u16>(value, 1, 223));
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
    locator.column = std::max<u16>(1, column);
    locator.row = std::max<u16>(1, row);
    locator.pixelX = std::max<u16>(1, pixelX);
    locator.pixelY = std::max<u16>(1, pixelY);
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
    }
    if (mouseTrk.focusEventMode) {
        writeCsiResponse(hasFocus ? "I" : "O");
    }
    showCursor();
    redraw();
}

void VtermImpl::pageUp() {
    if (altScrollMode && altScreenBufferMode) {
        for (int k = 0; k < (marginBottom - marginTop) / 2; ++k) {
            writePty(InputKey::Up);
        }
    } else {
        cf->scrollView(composer.rows / 2);
        refreshBlinkingText();
        redraw();
    }
}

void VtermImpl::pageDown() {
    if (altScrollMode && altScreenBufferMode) {
        for (int k = 0; k < (marginBottom - marginTop) / 2; ++k) {
            writePty(InputKey::Down);
        }
    } else {
        cf->scrollView(-(i32)(composer.rows / 2));
        refreshBlinkingText();
        redraw();
    }
}

void VtermImpl::mouseWheelUp(u16 count) {
    if (altScrollMode && altScreenBufferMode) {
        for (u16 k = 0; k < count; ++k) {
            writePty(InputKey::Up);
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
            writePty(InputKey::Down);
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
            writePty(InputKey::Right);
        }
    }
}

void VtermImpl::mouseWheelLeft(u16 count) {
    if (altScrollMode && altScreenBufferMode) {
        for (u16 k = 0; k < count; ++k) {
            writePty(InputKey::Left);
        }
    }
}

void VtermImpl::resetTerminal() {
    switchScreenBufferMode(false, true);
    resetScreen();
    resetAttrs();
    sgrStackNext = 0;
    sgrStackCount = 0;
    osc_RESET_PALETTE();
    if (assignedDefaultColors) {
        csi_DECAC_TEXT_RESET();
    }

    noClearColumnMode = false;
    switchColMode(ColMode::C80);

    cf->dropHistory();
    marginTop = 0;
    marginBottom = composer.rows;
    clearScreen();

    switchScreenBufferMode(false);
    altScrollMode = opts.altScrollMode;
    altSendsEscape = opts.altSendsEscape;
    modifyOtherKeys = opts.modifyOtherKeys;
    std::copy(std::begin(initialModifyKeyResources), std::end(initialModifyKeyResources), std::begin(modifyKeyResources));
    savedPrivModes.clear();
    userDefinedKeys.clear();
    userDefinedKeysLocked = false;
    delete kittyClipboardWriteStream;
    kittyClipboardWriteStream = nullptr;
    kittyClipboardWriteLength = 0;
    kittyClipboardWriteId.reset();
    kittyKeyboardPri = {};
    kittyKeyboardAlt = {};
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
    titleStack.clear();
    notifications.clear();

    horizMarginMode = false;
    hMargin = 0;
    nColsEff = composer.columns;

    osc_TITLE_0(StringView((const u8*)(opts.title), std::strlen(opts.title)));
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
    rectangularAttributeExtent = false;
    checksumFlags = 0;
    attrs.protected_char = 0;
    bkspSendsDel = true;
    localEcho = false;
    bracketedPasteMode = false;
    synchronizedOutputMode = false;
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
    altScrollMode = opts.altScrollMode;
    altSendsEscape = opts.altSendsEscape;

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
    if (changed && windowColumns() != columns) {
        windowOperation(8, windowRows(), columns);
    }
    marginTop = 0;
    marginBottom = composer.rows;
    horizMarginMode = false;
    hMargin = 0;
    nColsEff = composer.columns;
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
                kittyKeyboardAlt = {};
                createAlternateScreen();
                currentSemantic = 0;
                semanticUntilEndOfLine = false;
                semanticClick = SemanticClick::None;
                marginTop = 0;
                marginBottom = composer.rows;
                hMargin = 0;
                nColsEff = composer.columns;
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
            kittyKeyboardAlt = {};
            createAlternateScreen();
            inactiveSemantic = 0;
            inactiveSemanticUntilEndOfLine = false;
            inactiveSemanticClick = SemanticClick::None;
            marginTop = 0;
            marginBottom = composer.rows;
            hMargin = 0;
            nColsEff = composer.columns;
            altScreenInitialized = true;
        } else if (const ScreenInfo info = frame_alt->info(); info.columns != composer.columns || info.rows != composer.rows) {
            Screen::Cursor cursorState;
            resizeScreen(frame_alt, frameAltPool, cursorState);
            marginTop = 0;
            marginBottom = composer.rows;
            hMargin = 0;
            nColsEff = composer.columns;
        }
        cf = frame_alt;
        cf->expose();

        savedCursor = &savedCursorAlt;
        altScreenBufferMode = true;
    } else {
        if (const ScreenInfo info = frame_pri->info(); info.columns != composer.columns || info.rows != composer.rows) {
            Screen::Cursor cursorState{Point(posX, posY), lastCol};
            resizeScreen(frame_pri, framePriPool, cursorState);
            posX = cursorState.position.x;
            posY = cursorState.position.y;
            lastCol = cursorState.pendingWrap;
            marginTop = 0;
            marginBottom = composer.rows;
            hMargin = 0;
            nColsEff = composer.columns;
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
    std::swap(currentSemantic, inactiveSemantic);
    std::swap(semanticUntilEndOfLine, inactiveSemanticUntilEndOfLine);
    std::swap(semanticClick, inactiveSemanticClick);
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

    if (composer.rows < posY + 1) {
        posY = composer.rows - 1;
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
        end = composer.columns;
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
    return std::min(normalEnd, std::max<u16>(1, composer.columns / 2));
}

void VtermImpl::eraseRow(u16 pY) {
    eraseRangeInRow(pY, hMargin, nColsEff - hMargin);
    if (hMargin == 0 && nColsEff == composer.columns) {
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
    if (hMargin == 0 && nColsEff == composer.columns) {
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
    if (hMargin == 0 && nColsEff == composer.columns) {
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
    if (eraseModeAll) {
        eraseRangeInRow(row, start, count);
        return;
    }
    if (!count) {
        return;
    }
    cf->selectiveEraseCells(row, start, count, eraseAttrs, TerminalCell::isoProtection);
}

void VtermImpl::eraseEcmaRow(u16 row) {
    if (eraseModeAll) {
        eraseRow(row);
        return;
    }
    const bool retained = cf->hasProtection(row, TerminalCell::isoProtection);
    eraseEcmaRangeInRow(row, 0, composer.columns);
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
        rowLimit = composer.rows;
        columnLimit = composer.columns;
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

    rectangle.top = rowBase + std::min(rawTop, rows) - 1;
    rectangle.left = columnBase + std::min(rawLeft, columns) - 1;
    rectangle.bottom = rowBase + std::min(rawBottom, rows);
    rectangle.right = columnBase + std::min(rawRight, columns);
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

u8 VtermImpl::codepointData(u32 codepoint) {
    constexpr u8 valid = 0x80;
    constexpr u8 simple = 0x04;
    u8& cached = (*unicodeProperties)[codepoint];
    if ((cached & valid) == 0) {
        const CodepointProperties properties = codepointProperties(codepoint);
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
    u16 lineBegin, lineCols;
    activeLine(lineBegin, lineCols);

    // A leading joiner has nothing to join.  Ghostty, Kitty and Foot discard
    // it; keeping it as a width-one cell also advances the cursor incorrectly.
    if (graphemeBoundary && w == 0 && pt == 0x200d) {
        inputGraphemeBreaker.reset();
        return;
    }

    if (inputGraphemeScreen == cf && !graphemeBoundary) {
        const u32 previous = inputGrapheme.empty() ? inputGraphemeBase : inputGrapheme.data()[inputGrapheme.size() - 1];
        const GraphemeWidthEffect widthEffect = graphemeWidthEffect(previous, pt);
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
    if (insertMode) {
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
            if (autoWrapMode && lastCol && horizMarginMode && isCursorInsideMargins() && posY == marginBottom - 1 && (hMargin != 0 || nColsEff != composer.columns)) {
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
                        const u16 doubleEnd = hMargin + std::max<u16>(1, lineWidth / 2);
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
        const u16 requested = std::min<size_t>(size, 0xffff);
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

// Decodes UTF-8 ahead and batches independent glyphs into span writes.
// Joining codepoints fall back to the standard cluster path.  Invalid bytes
// become replacement characters in the same batch, mirroring the streaming
// decoder's rules exactly; only a sequence split across the chunk boundary
// stops the run so the streaming decoder can carry its state across feeds.
int VtermImpl::placeUtf8Run(const u8* input, int size, u8& pendingTrace) {
    constexpr size_t batchLimit = 64;
    u32 batch[batchLimit];
    u8 widths[batchLimit];
    size_t batchCount = 0;
    bool batchWide = false;
    int consumed = 0;

    if (inputGraphemeScreen != cf) {
        inputGraphemeBreaker.reset();
    }
    const auto flush = [&]() {
        if (batchCount != 0) {
            if (batchWide) {
                placePreparedRun<true>(batch, widths, batchCount);
            } else {
                placePreparedRun<false>(batch, widths, batchCount);
            }
            batchCount = 0;
            batchWide = false;
        }
    };
    const auto place = [&](u32 codepoint, int advance) __attribute__((always_inline)) {
        const u8 data = codepointData(codepoint);
        const bool boundary = inputGraphemeBreaker.breakBefore(codepoint, (data & 0x04) != 0);
        const u8 width = data & 0x03;
        if (!boundary || width == 0) {
            flush();
            utf8dec.setUnicode(codepoint);
            placeGraphicChar(boundary, width);
            consumed += advance;
            return;
        }
        if (batchCount == batchLimit) {
            flush();
        }
        batch[batchCount] = codepoint;
        widths[batchCount++] = width;
        batchWide |= width == 2;
        consumed += advance;
        // A wide glyph can be discarded after wrapping into a narrow or
        // double-width row.  That resets grapheme state, so commit it before
        // determining the boundary of the following codepoint.
        if (width == 2) {
            flush();
        }
    };

    u8 state = Utf8Dfa::Ground;
    u32 codepoint = 0;
    int sequenceStart = 0;
    // Emitted replacement characters and printable bytes are simple
    // graphemes with width one: while the breaker's fast path holds, they
    // append to the batch with no per-codepoint branching, and the breaker
    // catches up in one setBoundaryAfter at the next full-service boundary.
    u32 lastBatched = 0;
    bool batchedBehind = false;
    bool simpleRun = inputGraphemeBreaker.simpleBoundary();
    const auto syncBreaker = [&]() __attribute__((always_inline)) {
        if (batchedBehind) {
            inputGraphemeBreaker.setBoundaryAfter(lastBatched, true);
            batchedBehind = false;
        }
    };
    while (consumed < size) {
        const u8 byte = input[consumed];
        const u8 cls = Utf8Dfa::cls[byte];
        if (cls == Utf8Dfa::Exit) {
            break;
        }
        const u8 action = Utf8Dfa::act[state][cls];
#if defined(SHITTY_FOR_TESTS)
        // A stray ground C1 stays observable as a control event, so the
        // ground dispatcher owns it. Production has no parser trace and
        // takes the replacement emission of the same action instead of
        // paying a run exit per byte.
        if (action & Utf8Dfa::Stop) [[unlikely]] {
            break;
        }
#endif
        sequenceStart = cls >= Utf8Dfa::LeadFirst ? consumed : sequenceStart;
        codepoint = cls >= Utf8Dfa::LeadFirst ? byte & Utf8Dfa::mask[cls] : (codepoint << 6) | (byte & 0x3f);
        state = Utf8Dfa::next[state][cls];
        ++consumed;
        if ((action & Utf8Dfa::Slow) != 0 || !simpleRun) [[unlikely]] {
            // Completed sequences need their width and grapheme class; a
            // non-simple boundary needs the full breaker. The stray-C1
            // reset only matters here: on the fast path it is equivalent
            // to the plain replacement it precedes.
            syncBreaker();
            if (action & Utf8Dfa::Reset) {
                inputGraphemeBreaker.reset();
            }
            const unsigned count = action & Utf8Dfa::CountMask;
            if (count != 0) {
                place((action & Utf8Dfa::FirstByte) ? byte : (action & Utf8Dfa::Slow) ? codepoint : Unicode_Replacement_Character, 0);
                if (count == 2) {
                    place((action & Utf8Dfa::SecondByte) ? byte : Unicode_Replacement_Character, 0);
                }
                simpleRun = inputGraphemeBreaker.simpleBoundary();
            }
            continue;
        }
        if (batchCount >= batchLimit - 2) {
            flush();
        }
        const unsigned count = action & Utf8Dfa::CountMask;
        const u32 first = (action & Utf8Dfa::FirstByte) ? byte : Unicode_Replacement_Character;
        const u32 second = (action & Utf8Dfa::SecondByte) ? byte : Unicode_Replacement_Character;
        batch[batchCount] = first;
        widths[batchCount] = 1;
        batch[batchCount + 1] = second;
        widths[batchCount + 1] = 1;
        batchCount += count;
        lastBatched = count == 0 ? lastBatched : count == 2 ? second : first;
        batchedBehind |= count != 0;
    }
    if (state >= Utf8Dfa::RewindFirst) {
        // A control or the chunk boundary interrupted a pending sequence.
        // Rewind to its lead: controls are transparent to the streaming
        // decoder, which owns the sequence from here. States below
        // RewindFirst already emitted everything — only the trace counter
        // of an aborted sequence is pending — and must not replay.
        consumed = sequenceStart;
    }
    pendingTrace = Utf8Dfa::pending[state];
    syncBreaker();
    flush();
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
            placeGraphicChar(true, *widths++);
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
            count = std::min<size_t>(size, available);
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

        const bool lastWide = widths[count - 1] == 2;
        const u16 clusterX = endX - widths[count - 1];
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
        // The grapheme breaker already advanced through every batched
        // codepoint; only the decoder mirror needs the last one.
        utf8dec.setUnicode(codepoint);

        if (endX == lineCols) {
            posX = lineCols - 1;
            lastCol = true;
        } else {
            posX = endX;
            lastCol = false;
        }
        input += count;
        widths += count;
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
    const u16 right = insideMargins ? nColsEff : composer.columns;
    if (!tabStopsCustomized) {
        do {
            posX = ((posX / 8) + 1) * 8;
        } while (posX < left);
        posX = std::min<int>(posX, right - 1);
    } else {
        auto ts = std::upper_bound(tabStops.begin(), tabStops.end(), posX);
        posX = ts == tabStops.end() || *ts >= right ? right - 1 : *ts;
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
    } else if (posY < composer.rows - 1) {
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
    if (brightenBold && opts.boldColors && attrs.bold && paletteIndex >= 0 && paletteIndex <= 7) {
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
            const int index = opts.boldColors && attrs.bold && fgPalIx < 8 ? fgPalIx + 8 : fgPalIx;
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
        count = std::min<u32>(count, nColsEff - hMargin);
        deleteCols(hMargin, (u16)(count));
    }
}

void VtermImpl::csi_ecma48_SR(u32 count) {
    if (isCursorInsideMargins()) {
        count = std::min<u32>(count, nColsEff - hMargin);
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
    } else if (posX < composer.columns - 1) {
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
        for (unsigned column = 8; column < composer.columns; column += 8) {
            tabStops.push_back((u16)(column));
        }
        tabStopsCustomized = true;
    }
    if (std::find(tabStops.begin(), tabStops.end(), posX) == tabStops.end()) {
        tabStops.push_back(posX);
        std::sort(tabStops.begin(), tabStops.end());
    }
}

void VtermImpl::esc_SPA() {
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
            posX = hMargin + std::min<u16>(savedCursor->posX, nColsEff - hMargin - 1);
            posY = marginTop + std::min<u16>(savedCursor->posY, marginBottom - marginTop - 1);
        } else {
            posX = std::min<u16>(savedCursor->posX, composer.columns - 1);
            posY = std::min<u16>(savedCursor->posY, composer.rows - 1);
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
    count = std::min<u32>(count, posY - top);
    posY -= count;
    lastCol = false;
}

void VtermImpl::csi_CUD(u32 count) {
    const u16 bottom = posY < marginBottom ? marginBottom : composer.rows;
    count = std::min<u32>(count, bottom - posY - 1);
    posY += count;
    lastCol = false;
}

void VtermImpl::csi_CUF(u32 count) {
    const bool insideMargins = posX >= hMargin && posX < nColsEff;
    const u16 right = insideMargins ? nColsEff : composer.columns;
    count = std::min<u32>(count, right - posX - 1);
    posX += count;
    lastCol = false;
}

void VtermImpl::csi_CUB(u32 count) {
    moveCursorBackward(count);
}

void VtermImpl::moveCursorBackward(u32 count) {
    const bool insideMargins = posX >= hMargin && posX < nColsEff;
    const bool canReverseWrap = autoWrapMode && (reverseWrapMode || extendedReverseWrapMode);
    if (count && lastCol && canReverseWrap) {
        lastCol = false;
        if (--count == 0) {
            return;
        }
    }
    if (posX == nColsEff) {
        count = std::min<u32>(count == UINT32_MAX ? count : count + 1, posX);
    }
    while (count > 0) {
        const u16 leftEdge = insideMargins ? hMargin : 0;
        const u16 left = posX >= leftEdge ? posX - leftEdge : posX;
        if (left > 0) {
            const u32 step = std::min<u32>(count, left);
            posX -= step;
            count -= step;
            continue;
        }
        if (!canReverseWrap) {
            break;
        }
        const u16 rightEdge = (insideMargins ? nColsEff : composer.columns) - 1;
        if (extendedReverseWrapMode && posY == marginTop) {
            posY = marginBottom - 1;
            posX = rightEdge;
            --count;
            continue;
        }
        if (posY == 0 || (!extendedReverseWrapMode && !cf->wrapped(posY - 1, nColsEff - 1))) {
            break;
        }
        --posY;
        posX = rightEdge;
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
        column = std::max<u32>(1, std::min<u32>(column, nColsEff - hMargin));
        posX = hMargin + column - 1;
    } else {
        column = std::max<u32>(1, std::min<u32>(column, composer.columns));
        posX = column - 1;
    }
    lastCol = false;
}

void VtermImpl::csi_HPA(u32 column) {
    csi_CHA(column);
}

void VtermImpl::csi_HPR(u32 count) {
    const u16 right = originMode == OriginMode::ScrollingRegion ? nColsEff : composer.columns;
    posX = (u16)(std::min<u64>((u64)(posX) + count, right - 1));
    lastCol = false;
}

void VtermImpl::csi_VPA(u32 row) {
    if (originMode == OriginMode::ScrollingRegion) {
        row = std::max<u32>(1, std::min<u32>(row, marginBottom - marginTop));
        posY = marginTop + row - 1;
    } else {
        row = std::max<u32>(1, std::min<u32>(row, composer.rows));
        posY = row - 1;
    }
    lastCol = false;
}

void VtermImpl::csi_VPR(u32 count) {
    const u16 bottom = originMode == OriginMode::ScrollingRegion ? marginBottom : composer.rows;
    posY = (u16)(std::min<u64>((u64)(posY) + count, bottom - 1));
    lastCol = false;
}

void VtermImpl::csi_CUP(u32 row, u32 column) {
    switch (originMode) {
        case OriginMode::Absolute:
            row = std::max<u32>(1, std::min<u32>(row, composer.rows)) - 1;
            column = std::max<u32>(1, std::min<u32>(column, composer.columns)) - 1;
            break;
        case OriginMode::ScrollingRegion:
            row = marginTop + std::max<u32>(1, std::min<u32>(row, marginBottom - marginTop)) - 1;
            column = hMargin + std::max<u32>(1, std::min<u32>(column, nColsEff - hMargin)) - 1;
            break;
    }

    posX = column;
    posY = row;
    lastCol = false;
}

void VtermImpl::csi_SU(u32 count) {
    count = std::min<u32>(count, marginBottom - marginTop);
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
    count = std::min<u32>(count, marginBottom - marginTop);
    const bool pendingWrap = lastCol;
    scrollRegionDown((u16)(count));
    lastCol = pendingWrap;
}

void VtermImpl::csi_CHT(u32 count) {
    count = std::min<u32>(count, composer.columns);
    if (count == 1) {
        inp_HT();
    } else {
        for (u32 k = 0; k < count; ++k) {
            jumpToNextTabStop();
        }
    }
}

void VtermImpl::csi_CBT(u32 count) {
    count = std::min<u32>(count, composer.columns);
    for (u32 k = 0; k < count; ++k) {
        const u16 left = originMode == OriginMode::ScrollingRegion ? hMargin : 0;
        if (!tabStopsCustomized) {
            if (posX > 0 && posX % 8 == 0) {
                posX -= 8;
            } else {
                posX = (posX / 8) * 8;
            }
            posX = std::max(posX, left);
        } else {
            auto ts = std::lower_bound(tabStops.begin(), tabStops.end(), posX);
            if (ts == tabStops.begin() || *(--ts) < left) {
                posX = left;
            } else {
                posX = *ts;
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
    const u64 observableCells = ((u64)(opts.saveLines) + composer.rows + 1) * composer.columns;
    if (count > observableCells) {
        count = (u32)(observableCells + (count - observableCells) % composer.columns);
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
    eraseEcmaRangeInRow(posY, posX, composer.columns - posX);
    for (u16 row = posY + 1; row < composer.rows; ++row) {
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
    for (u16 row = 0; row < composer.rows; ++row) {
        eraseEcmaRow(row);
    }
}

void VtermImpl::eraseScrollback() {
    normalizeCursorPos();
    cf->dropHistory();
}

void VtermImpl::eraseLineAfter() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, posX, composer.columns - posX);
}

void VtermImpl::eraseLineBefore() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::eraseLineAll() {
    normalizeCursorPos();
    eraseEcmaRangeInRow(posY, 0, composer.columns);
}

void VtermImpl::selectiveEraseDisplayAfter() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, posX, composer.columns - posX);
    for (u16 row = posY + 1; row < composer.rows; ++row) {
        selectiveEraseRangeInRow(row, 0, composer.columns);
    }
}

void VtermImpl::selectiveEraseDisplayBefore() {
    normalizeCursorPos();
    for (u16 row = 0; row < posY; ++row) {
        selectiveEraseRangeInRow(row, 0, composer.columns);
    }
    selectiveEraseRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::selectiveEraseDisplayAll() {
    normalizeCursorPos();
    for (u16 row = 0; row < composer.rows; ++row) {
        selectiveEraseRangeInRow(row, 0, composer.columns);
    }
}

void VtermImpl::selectiveEraseLineAfter() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, posX, composer.columns - posX);
}

void VtermImpl::selectiveEraseLineBefore() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, 0, posX + 1);
}

void VtermImpl::selectiveEraseLineAll() {
    normalizeCursorPos();
    selectiveEraseRangeInRow(posY, 0, composer.columns);
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
    const u16 targetTop = rowBase + std::min<u32>(targetRow, rowLimit - rowBase) - 1;
    const u16 targetLeft = columnBase + std::min<u32>(targetColumn, columnLimit - columnBase) - 1;
    const u16 height = std::min<u16>(source.bottom - source.top, rowLimit - targetTop);
    const u16 width = std::min<u16>(source.right - source.left, columnLimit - targetLeft);
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
        count = std::min<u32>(count, marginBottom - posY);
        insertRows(posY, (u16)(count));
        inp_CR();
    }
}

void VtermImpl::csi_DL(u32 count) {
    if (isCursorInsideMargins()) {
        count = std::min<u32>(count, marginBottom - posY);
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
    const u32 len = composer.columns - posX;
    count = std::min(count, len);
    eraseEcmaRangeInRow(posY, posX, count);
    lastCol = false;
}

void VtermImpl::csi_STBM(u32 top, u32 bottom, bool valid) {
    const u32 newMarginTop = top > 0 ? top - 1 : 0;
    const u32 newMarginBottom = bottom == 0 ? composer.rows : bottom;
    const bool illegal = newMarginTop >= composer.rows || newMarginBottom > composer.rows || newMarginBottom <= newMarginTop + 1;
    if (!valid || illegal) {
        // xterm: a rejected region is a complete no-op, the cursor stays.
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
    const u32 newMarginRight = right == 0 ? composer.columns : right;
    const bool illegal = newMarginLeft >= composer.columns || newMarginRight > composer.columns || newMarginRight <= newMarginLeft + 1;
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
        for (unsigned column = 8; column < composer.columns; column += 8) {
            tabStops.push_back((u16)(column));
        }
        tabStopsCustomized = true;
    }
    auto it = std::find(tabStops.begin(), tabStops.end(), posX);
    if (it != tabStops.end()) {
        tabStops.erase(it);
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
    result.colorSchemeUpdates = colorSchemeUpdateMode;
    result.inBandResize = inBandResizeMode;
    result.pasteMimeNotifications = pasteMimeNotificationsMode;
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
        nColsEff = composer.columns;
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

void VtermImpl::setMouseEncoding(MouseTrackingEnc encoding) {
    mouseTrk.setEncoding(encoding);
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
    savedPrivModes[mode] = enabled;
}

bool VtermImpl::restorePrivateMode(u32 mode, bool& enabled) const {
    const auto it = savedPrivModes.find(mode);
    if (it == savedPrivModes.end()) {
        return false;
    }
    enabled = it->second;
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
    if (opts.boldColors && attrs.bold && fgPalIx >= 0 && fgPalIx <= 7) {
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
#define DEVICE_ID "64;1;2;6;8;9;15;21;22;28;29c"

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
    response << composer.rows << StringView(u8";") << composer.columns << StringView(u8";1;1;1\"w");
    writeCsiResponse(StringView(response));
}

void VtermImpl::csi_DECREQTPARM(u32 permission) {
    StringBuilder response;
    response << permission + 2 << StringView(u8";1;1;128;128;1;0x");
    writeCsiResponse(StringView(response));
}

void VtermImpl::csi_XTVERSION() {
    writeDcsResponse(">|Shitty " SHITTY_VERSION);
}

void VtermImpl::csi_SETMARK() {
    currentSemantic = 1;
    semanticUntilEndOfLine = false;
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
            if (column >= composer.columns) {
                break;
            }
            if (!first) {
                response << StringView(u8"/");
            }
            response << (u32)(column) + 1;
            first = false;
        }
    } else {
        for (u32 column = 8; column < composer.columns; column += 8) {
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
    marginBottom = composer.rows;
    hMargin = 0;
    nColsEff = composer.columns;
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
        posX = std::min<u16>(posX, std::max(1, composer.columns / 2) - 1);
    }
    lastCol = false;
}

void VtermImpl::esc_RIS() {
    resetTerminal();
}

void VtermImpl::csi_DECSTR() {
    resetScreen(false);
    resetAttrs();
    activeHyperlink = 0;
    marginTop = 0;
    marginBottom = composer.rows;
    hMargin = 0;
    nColsEff = composer.columns;
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
        userDefinedKeys.clear();
    }

    for (size_t index = 0; index < definitionCount; ++index) {
        const ParserUdkDefinition& definition = definitions[index];
        const auto* value = (const char*)(values.data()) + definition.valueOffset;
        userDefinedKeys[definition.key] = std::string(value, definition.valueLength);
    }
    userDefinedKeysLocked = lockDefinitions;
}

void VtermImpl::writeDecrqssResponse(StringView value) {
    StringBuilder response;
    response << StringView(u8"1$r") << value;
    writeDcsResponse(StringView(response));
}

void VtermImpl::dcs_DECRSTS_HLS(u32 index, u32 hue, u32 luminosity, u32 saturation) {
    hue %= 360;
    if (luminosity > 100) {
        luminosity = 100;
    }
    if (saturation > 100) {
        saturation = 100;
    }

    const float light = (float)(luminosity);
    const float sat = (float)(saturation);
    const float chroma = (50.0f - __builtin_fabsf(light - 50.0f)) * sat / 50.0f;
    const float second = chroma * (60.0f - __builtin_fabsf((float)(hue % 120) - 60.0f)) / 60.0f;
    const float offset = light - chroma / 2.0f;
    const float scale = 255.0f / 100.0f;
    const u8 firstComponent = (u8)((chroma + offset) * scale + 0.5f);
    const u8 secondComponent = (u8)((second + offset) * scale + 0.5f);
    const u8 thirdComponent = (u8)(offset * scale + 0.5f);

    Color color;
    if (hue < 60) {
        color = {secondComponent, thirdComponent, firstComponent};
    } else if (hue < 120) {
        color = {firstComponent, thirdComponent, secondComponent};
    } else if (hue < 180) {
        color = {firstComponent, secondComponent, thirdComponent};
    } else if (hue < 240) {
        color = {secondComponent, firstComponent, thirdComponent};
    } else if (hue < 300) {
        color = {thirdComponent, firstComponent, secondComponent};
    } else {
        color = {thirdComponent, secondComponent, firstComponent};
    }
    applyPaletteColor((u16)(index), color);
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
    const auto position = std::lower_bound(tabStops.begin(), tabStops.end(), zeroBased);
    if (position == tabStops.end() || *position != zeroBased) {
        tabStops.insert(position, zeroBased);
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

    posY = (u16)(min<u32>(row, composer.rows) - 1);
    posX = (u16)(min<u32>(column, composer.columns) - 1);
    lastCol = flags & 8;
    changePresentation();
}

void VtermImpl::dcs_DECAUPSS(Charset charset, u16 id, bool is96) {
    userPreferenceCharset = charset == Charset::DecUserPref ? Charset::DecSuppl : charset;
    userPreferenceCharsetId = id;
    userPreferenceCharset96 = is96;
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
    StringBuilder value;
    value << windowRows() << StringView(u8"t");
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
    writePty(output.data(), output.length(), false);
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
    composer.window->requestAttention();
}

void VtermImpl::recordLeds(u8 state) {
    if (trace != nullptr) {
        trace->leds(state);
    }
}

void VtermImpl::publishTitle(u32 command, StringView title) {
    titleSet = title != StringView(opts.title);
    composer.window->requestTitle(title);
    recordOsc(command, title);
}

void VtermImpl::publishCwd(StringView path) {
    if (trace != nullptr) {
        trace->cwd(path);
    }
    if (!titleSet) {
        composer.window->requestTitle(path);
    }
}

void VtermImpl::publishNotify(StringView id, StringView title, StringView body, bool close) {
    if (trace != nullptr) {
        trace->notify(id, title, body, close);
    }
    if (!close) {
        composer.window->requestAttention();
    }
}

void VtermImpl::publishProgress(u32 state, u32 percent) {
    if (trace != nullptr) {
        trace->progress(state, percent);
    }
    if (state == 2 || state == 4) {
        composer.window->requestAttention();
    }
}

plt::WindowInfo VtermImpl::windowInfo() const {
    return composer.window->info();
}

u32 VtermImpl::columnsForPixelWidth(u32 width) const {
    if (composer.glyphWidth == 0) {
        return composer.columns;
    }
    const u32 border = 2u * opts.border;
    return std::max(1u, (width > border ? width - border : 0u) / composer.glyphWidth);
}

u32 VtermImpl::rowsForPixelHeight(u32 height) const {
    if (composer.glyphHeight == 0) {
        return composer.rows;
    }
    const u32 border = 2u * opts.border;
    return std::max(1u, (height > border ? height - border : 0u) / composer.glyphHeight);
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
    plt::Window* const window = composer.window;
    const auto resize = [&](u32 pixelWidth, u32 pixelHeight) {
        if (pixelWidth == 0 || pixelHeight == 0) {
            return;
        }
        window->requestResize(pixelWidth, pixelHeight);
        composer.resize((u16)(std::min(pixelWidth, (u32)(UINT16_MAX))), (u16)(std::min(pixelHeight, (u32)(UINT16_MAX))));
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
        pixelWidth = 2u * opts.border + second * composer.glyphWidth;
        pixelHeight = 2u * opts.border + first * composer.glyphHeight;
    } else {
        return;
    }
    resize(pixelWidth, pixelHeight);
}

void VtermImpl::osc_TITLE_0(StringView payload) {
    iconTitle.assign((const char*)(payload.data()), payload.length());
    windowTitle = iconTitle;
    publishTitle(0, payload);
}

void VtermImpl::osc_TITLE_1(StringView payload) {
    iconTitle.assign((const char*)(payload.data()), payload.length());
    recordOsc(1, payload);
}

void VtermImpl::osc_TITLE_2(StringView payload) {
    windowTitle.assign((const char*)(payload.data()), payload.length());
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

    CellExtraStore& extras = *composer.cellExtras;
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

void VtermImpl::osc_PROGRESS(u32 state, u32 percent) {
    publishProgress(state, percent);
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
    colors.changed();
    defaultFgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_DEFAULT_BACKGROUND(Color color, bool query) {
    if (query) {
        return writeDynamicColorResponse(11, colors.defaultBackground);
    }
    colors.defaultBackground = color;
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
    if (!opts.allowOsc52Read || (!primary && !clipboard)) {
        StringBuilder reply;
        reply << StringView(u8"52;");
        if (selectorsEmpty) {
            reply << StringView(u8"s0");
        } else if (replySelector != 0) {
            reply.append(&replySelector, 1);
        }
        reply << StringView(u8";");
        writeOscResponse(StringView(reply));
        return;
    }
    const bool tryClipboard = primary && clipboard;
    const bool eightBit = send8BitControls;
    spawnTransaction([this, primary, tryClipboard, replySelector, selectorsEmpty, eightBit] {
        const plt::LockGuard guard(*composer.ptyMutex, *composer.platform->scheduler());
        u8 chunk[8 * 1024];
        ScopedPtr<Input> source{selectionTarget(composer, primary)->read()};
        size_t count = source->read(chunk, sizeof(chunk));
        if (count == 0 && tryClipboard) {
            delete source.ptr;
            source.ptr = composer.window->secondary()->read();
            count = source->read(chunk, sizeof(chunk));
        }
        Output& output = *composer.pty->output();
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
        writeSelection(*composer.window->primary(), decoded);
    }
    if (clipboard) {
        writeSelection(*composer.window->secondary(), decoded);
    }
}

void VtermImpl::writeKittyClipboardStatus(StringView type, StringView id, StringView status) {
    Buffer cleanId;
    copyKittyClipboardId(cleanId, id);
    writeKittyClipboardPacket(*composer.ptyOutput, send8BitControls, type, status, StringView(cleanId));
}

void VtermImpl::osc_KITTY_CLIPBOARD_READ(StringView id, StringView mimeTypes, bool primary, bool valid) {
    const bool targets = valid && mimeTypes == StringView(u8".");
    if (!valid) {
        writeKittyClipboardStatus(StringView(u8"read"), id, StringView(u8"ENOSYS"));
        return;
    }
    if (!targets && !opts.allowOsc52Read) {
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
    std::string idCopy((const char*)(cleanId.data()), cleanId.used());
    std::string mimeCopy((const char*)(mimeType.data()), mimeType.length());
    const bool eightBit = send8BitControls;
    spawnTransaction([this, idCopy, mimeCopy, primary, targets, eightBit] {
        const plt::LockGuard guard(*composer.ptyMutex, *composer.platform->scheduler());
        Output& output = *composer.pty->output();
        const StringView idView((const u8*)(idCopy.data()), idCopy.size());
        const StringView mimeView((const u8*)(mimeCopy.data()), mimeCopy.size());
        writeKittyClipboardPacket(output, eightBit, StringView(u8"read"), StringView(u8"OK"), idView, {}, {}, primary);
        if (targets) {
            writeKittyClipboardPacket(output, eightBit, StringView(u8"read"), StringView(u8"DATA"), idView, StringView(u8"."), StringView(u8"text/plain\n"), primary);
        } else {
            const ScopedPtr<Input> source{selectionTarget(composer, primary)->read()};
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
    plt::Clipboard* const target = selectionTarget(composer, primary);
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
    std::copy(std::begin(originalPalette256), std::end(originalPalette256), std::begin(colors.palette));
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
    std::copy(std::begin(colors.originalSpecial), std::end(colors.originalSpecial), std::begin(colors.special));
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
    colors.defaultForeground = opts.fg;
    colors.changed();
    defaultFgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_RESET_DEFAULT_BACKGROUND() {
    colors.defaultBackground = opts.bg;
    colors.changed();
    defaultBgPalIx = -1;
    exposeFrames();
}

void VtermImpl::osc_RESET_CURSOR_COLOR() {
    cursorColor = opts.cr;
    changePresentation();
}

void VtermImpl::osc_RESET_SELECTION_BACKGROUND() {
    selectionBgColor = opts.bg;
    selectionColorMask &= ~2;
    changePresentation();
}

void VtermImpl::osc_RESET_SELECTION_FOREGROUND() {
    selectionFgColor = opts.fg;
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
    osc_SHELL_L(payload);
    startSemanticPrompt(payload);
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
}

void VtermImpl::osc_NOTIFICATION_CAPABILITIES(StringView id) {
    StringBuilder response;
    response << StringView(u8"99;i=") << id << StringView(u8":p=?;p=title,body,close");
    writeOscResponse(StringView(response));
}

void VtermImpl::osc_NOTIFICATION_CLOSE(StringView id) {
    const std::string key((const char*)(id.data()), id.length());
    if (key.empty()) {
        return;
    }
    // The notification backend owns completed IDs.  Retaining them in the
    // terminal just to validate close requests makes terminal state grow
    // without bound, while forwarding an unknown close is harmless.
    publishNotify(stringView(key), {}, {}, true);
    notifications.erase(key);
}

void VtermImpl::osc_NOTIFICATION_TITLE(StringView id, StringView payload, bool encoded, bool finalChunk) {
    applyNotificationPart(id, payload, encoded, finalChunk, false);
}

void VtermImpl::osc_NOTIFICATION_BODY(StringView id, StringView payload, bool encoded, bool finalChunk) {
    applyNotificationPart(id, payload, encoded, finalChunk, true);
}

void VtermImpl::applyNotificationPart(StringView id, StringView payload, bool encoded, bool finalChunk, bool body) {
    const std::string key((const char*)(id.data()), id.length());
    auto& notification = notifications[key];
    NotificationPart& destination = body ? notification.body : notification.title;
    const auto flushEncoded = [](NotificationPart& part) {
        if (part.encodedOffset == (size_t)-1) {
            return true;
        }
        size_t decodedSize = part.text.size() - part.encodedOffset;
        if (!base64DecodeInPlace((u8*)(part.text.data() + part.encodedOffset), decodedSize) || part.encodedOffset + decodedSize > 8192) {
            return false;
        }
        part.text.resize(part.encodedOffset + decodedSize);
        part.encodedOffset = (size_t)-1;
        return true;
    };

    if (encoded) {
        if (destination.encodedOffset == (size_t)-1) {
            destination.encodedOffset = destination.text.size();
        }
        const size_t decodedCapacity = 8192 - destination.encodedOffset;
        const size_t encodedCapacity = (decodedCapacity + 2) / 3 * 4;
        if (destination.text.size() - destination.encodedOffset + payload.length() > encodedCapacity) {
            notifications.erase(key);
            return;
        }
        destination.text.append((const char*)(payload.data()), payload.length());
        if (!payload.empty() && payload[payload.length() - 1] == '=' && !flushEncoded(destination)) {
            notifications.erase(key);
            return;
        }
    } else {
        if (!flushEncoded(destination) || destination.text.size() + payload.length() > 8192) {
            notifications.erase(key);
            return;
        }
        destination.text.append((const char*)(payload.data()), payload.length());
    }

    if (!finalChunk) {
        return;
    }
    if (!flushEncoded(notification.title) || !flushEncoded(notification.body)) {
        notifications.erase(key);
        return;
    }

    const auto escapeSafeUtf8 = [](const std::string& value) {
        for (size_t k = 0; k < value.size();) {
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
            if (k + continuation > value.size()) {
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
        notification.title.text = std::move(notification.body.text);
        notification.body.text.clear();
    }
    if (notification.title.text.empty()) {
        notifications.erase(key);
        return;
    }
    publishNotify(stringView(key), stringView(notification.title.text), stringView(notification.body.text), false);
    notifications.erase(key);
}

void VtermImpl::reportInBandResize() {
    StringBuilder response;
    response << StringView(u8"48;") << composer.rows << StringView(u8";") << composer.columns << StringView(u8";") << composer.rows * composer.glyphHeight << StringView(u8";") << composer.columns * composer.glyphWidth << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::reportColorScheme() {
    // Shitty has no runtime profile or operating-system theme switching.  Its
    // configured background therefore remains the authoritative preference;
    // application-originated OSC color changes must not affect this report.
    const u32 brightness = 299 * opts.bg.red + 587 * opts.bg.green + 114 * opts.bg.blue;
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
    writePty((const u8*)(response.data()), response.used());
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
        response << StringView(u8"4;") << composer.pixelHeight << StringView(u8";") << composer.pixelWidth << StringView(u8"t");
    } else {
        response << StringView(u8"4;") << composer.rows * composer.glyphHeight << StringView(u8";") << composer.columns * composer.glyphWidth << StringView(u8"t");
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
    response << StringView(u8"6;") << composer.glyphHeight << StringView(u8";") << composer.glyphWidth << StringView(u8"t");
    writeCsiResponse(StringView(response));
}

void VtermImpl::xtReportGridSize() {
    StringBuilder response;
    response << StringView(u8"8;") << composer.rows << StringView(u8";") << composer.columns << StringView(u8"t");
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
    SavedTitles saved;
    if (icon) {
        saved.hasIcon = true;
        saved.icon = iconTitle;
    }
    if (window) {
        saved.hasWindow = true;
        saved.window = windowTitle;
    }
    titleStack.push_back(std::move(saved));
    if (titleStack.size() > 10) {
        titleStack.erase(titleStack.begin());
    }
}

void VtermImpl::xtPopTitle(bool icon, bool window) {
    if (titleStack.empty()) {
        return;
    }
    SavedTitles saved = std::move(titleStack.back());
    titleStack.pop_back();
    for (auto it = titleStack.rbegin(); it != titleStack.rend() && (!saved.hasIcon || !saved.hasWindow); ++it) {
        if (!saved.hasIcon && it->hasIcon) {
            saved.hasIcon = true;
            saved.icon = it->icon;
        }
        if (!saved.hasWindow && it->hasWindow) {
            saved.hasWindow = true;
            saved.window = it->window;
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
        mouseHighlight.startX = std::max<u32>(1, startX);
        mouseHighlight.startY = std::max<u32>(1, startY);
        mouseHighlight.firstRow = std::max<u32>(1, firstRow);
        mouseHighlight.lastRow = std::max<u32>(mouseHighlight.firstRow, lastRow);
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
    colors.defaultForeground = opts.fg;
    colors.defaultBackground = opts.bg;
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
    std::copy(std::begin(initialModifyKeyResources), std::end(initialModifyKeyResources), std::begin(modifyKeyResources));
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
    if (state.stack.size() == maxStackDepth) {
        state.stack.erase(state.stack.begin());
    }
    state.stack.push_back(state.flags);
    state.flags = flags & 0x1f;
}

void VtermImpl::csi_kittyKeyboardPop(u32 count) {
    auto& state = kittyKeyboardState();
    for (u32 k = 0; k < count; ++k) {
        if (state.stack.empty()) {
            state.flags = 0;
            break;
        }
        state.flags = state.stack.back();
        state.stack.pop_back();
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

    const InputSpec is_modOtherKeys2[] = {
        {Key::Tab, CSI "27;" MC ";9~"},
        {Key::Enter, CSI "27;" MC ";13~"},
        {Key::Space, CSI "27;" MC ";32~"},
        {Key::Backspace, CSI "27;" MC ";127~"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Alt[] = {
        {Key::Backspace, "\xc3\xbf"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Alt_altSendsEscape[] = {
        {Key::Backspace, ESC "\x7f"},
        {Key::Space, ESC " "},
        {Key::Tab, ESC "\t"},
        {Key::Enter, ESC "\n"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Control_modOtherKeys[] = {
        {Key::Tab, CSI "27;" MC ";9~"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_ControlAlt_altSendsEscape[] = {
        {Key::Space, ESC "\x00", 2},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Control[] = {
        {Key::Space, "\x00", 1},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Shift[] = {
        {Key::Tab, CSI "Z"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_modOtherKeys[] = {
        {Key::Enter, CSI "27;" MC ";13~"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Ansi[] = {
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

    const InputSpec is_Mod_Ansi[] = {
        {Key::Insert, CSI "2;" MC "~"},
        {Key::Delete, CSI "3;" MC "~"},
        {Key::PageUp, CSI "5;" MC "~"},
        {Key::PageDown, CSI "6;" MC "~"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Ansi_FunctionKeys[] = {
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

    const InputSpec is_Mod_Ansi_FunctionKeys[] = {
        {Key::F1, CSI "1;" MC "P"},
        {Key::KeypadF1, CSI "1;" MC "P"},
        {Key::F2, CSI "1;" MC "Q"},
        {Key::KeypadF2, CSI "1;" MC "Q"},
        {Key::F3, CSI "13;" MC "~"},
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

    const InputSpec is_Ansi_KeypadKeys[] = {
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

    const InputSpec is_Appl_KeypadKeys[] = {
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

    const InputSpec is_Mod_Appl_KeypadKeys[] = {
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

    const InputSpec is_VT52_KeypadKeys[] = {
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

    const InputSpec is_VT52_FunctionKeys[] = {
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

    const InputSpec is_Ansi_CursorKeys[] = {
        {Key::Up, CSI "A"},
        {Key::Down, CSI "B"},
        {Key::Right, CSI "C"},
        {Key::Left, CSI "D"},
        {Key::Home, CSI "H"},
        {Key::End, CSI "F"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Appl_CursorKeys[] = {
        {Key::Up, SS3 "A"},
        {Key::Down, SS3 "B"},
        {Key::Right, SS3 "C"},
        {Key::Left, SS3 "D"},
        {Key::Home, SS3 "H"},
        {Key::End, SS3 "F"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Mod_CursorKeys[] = {
        {Key::Up, CSI "1;" MC "A"},
        {Key::Down, CSI "1;" MC "B"},
        {Key::Right, CSI "1;" MC "C"},
        {Key::Left, CSI "1;" MC "D"},
        {Key::Home, CSI "1;" MC "H"},
        {Key::End, CSI "1;" MC "F"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_VT52_CursorKeys[] = {
        {Key::Up, ESC "A"},
        {Key::Down, ESC "B"},
        {Key::Right, ESC "C"},
        {Key::Left, ESC "D"},
        {Key::Home, ESC "H"},
        {Key::End, ESC "F"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_ReturnKey_ANL[] = {
        {Key::Enter, "\r\n"},
        {Key::KeypadEnter, "\r\n"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_BackspaceKey_BkSp[] = {
        {Key::Backspace, "\b"},
        {Key::Unknown, nullptr},
    };

    const InputSpec is_Alt_BackspaceKey_BkSp[] = {
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

    bool isKittyModifierKey(InputKey key) {
        return key >= InputKey::LeftShift && key <= InputKey::RightSuper;
    }

    bool isKittyRecoveryKey(InputKey key) {
        return key == InputKey::Enter || key == InputKey::Tab || key == InputKey::Backspace;
    }

    VtModifier kittyToLegacyModifiers(u16 modifiers) {
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

    bool validKittyAssociatedText(u32 codepoint) {
        return codepoint >= 0x20 && !(codepoint >= 0x7f && codepoint <= 0x9f);
    }

    u32 kittyAssociatedText(InputKey key) {
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

    KittyKeySpec kittyKeySpec(InputKey key) {
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

    void makePalette256(Color p[]) {
        opts.getColor("color0", p[0]);
        opts.getColor("color1", p[1]);
        opts.getColor("color2", p[2]);
        opts.getColor("color3", p[3]);
        opts.getColor("color4", p[4]);
        opts.getColor("color5", p[5]);
        opts.getColor("color6", p[6]);
        opts.getColor("color7", p[7]);
        opts.getColor("color8", p[8]);
        opts.getColor("color9", p[9]);
        opts.getColor("color10", p[10]);
        opts.getColor("color11", p[11]);
        opts.getColor("color12", p[12]);
        opts.getColor("color13", p[13]);
        opts.getColor("color14", p[14]);
        opts.getColor("color15", p[15]);

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

    const u16 uc_DecSpec[] = {
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

    const u16 uc_DecSuppl[] = {
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

    const u16 uc_DecTechn[] = {
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

    const u16 uc_IsoLatin1[] = {
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

    const u16 uc_IsoUK[] = {
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

    const auto lookup = [ch](const std::pair<u8, u16>* table, size_t size) -> u32 {
        for (size_t index = 0; index < size; ++index) {
            if (table[index].first == ch) {
                return table[index].second;
            }
        }
        return ch;
    };
#define NRC_TABLE(name, ...) static const std::pair<u8, u16> name[] = {__VA_ARGS__}
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

CallVtermResize::CallVtermResize(VtermImpl* parent_)
    : parent(parent_)
{
}

void CallVtermResize::onListen(void*) {
    parent->resizeGrid();
    parent->redraw();
}

CallVtermFontChanged::CallVtermFontChanged(VtermImpl* parent_)
    : parent(parent_)
{
}

void CallVtermFontChanged::onListen(void*) {
    parent->fontChanged();
}

CallVtermInputAction::CallVtermInputAction(VtermImpl* parent_, InputActions action_)
    : parent(parent_)
    , action(action_)
{
}

void CallVtermInputAction::onListen(void*) {
    switch (action) {
        case InputActions::Copy:
            parent->input.copy();
            break;
        case InputActions::Paste:
            parent->input.paste(false);
            break;
        case InputActions::PastePrimary:
            parent->input.paste(true);
            break;
        case InputActions::PageUp:
            parent->pageUp();
            break;
        case InputActions::PageDown:
            parent->pageDown();
            break;
        default:
            STD_ASSERT(false);
    }
}

VtermImpl::VtermImpl(Composer& composer_, VtermTraceFactory* traceFactory_, Output* dump_)
    : input(this)
    , composer(composer_)
    , trace(traceFactory_ == nullptr ? nullptr : traceFactory_->construct(createTestApi()))
    , dump(dump_)
    , unicodeProperties(UnicodeMap<u8>::create(*composer.pool))
    , parser(Parser::create(composer.pool, *this, trace))
    , nColsEff(composer.columns)
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
    outputRows.grow((size_t)(composer.rows));
    makePalette256(colors.palette);
    std::copy(std::begin(colors.palette), std::end(colors.palette), std::begin(originalPalette256));
    colors.defaultForeground = opts.fg;
    colors.defaultBackground = opts.bg;
    std::fill(std::begin(colors.special), std::end(colors.special), opts.fg);
    std::fill(std::begin(colors.originalSpecial), std::end(colors.originalSpecial), opts.fg);
    cursorColor = opts.cr;
    selectionFgColor = opts.fg;
    selectionBgColor = opts.bg;
    initialModifyKeyResources[0] = 0;
    initialModifyKeyResources[1] = 2;
    initialModifyKeyResources[2] = 2;
    initialModifyKeyResources[3] = 0;
    initialModifyKeyResources[4] = opts.modifyOtherKeys;
    initialModifyKeyResources[6] = 0;
    initialModifyKeyResources[7] = 0;
    windowTitle = opts.title;
    iconTitle = opts.title;

    defaultFgPalIx = -1;
    defaultBgPalIx = -1;
    fgPalIx = defaultFgPalIx;
    bgPalIx = defaultBgPalIx;
}

void VtermImpl::wireInputBindings() {
    const auto add = [&](InputActions action, IntrusiveList& listeners) {
        listeners.pushBack(composer.pool->make<CallVtermInputAction>(this, action));
        composer.inputBindings->add(action, &listeners);
    };
    add(InputActions::Copy, copyListeners);
    add(InputActions::Paste, pasteListeners);
    add(InputActions::PastePrimary, pastePrimaryListeners);
    add(InputActions::PageUp, pageUpListeners);
    add(InputActions::PageDown, pageDownListeners);
}

void VtermImpl::fontChanged() {
    cf->expose();
    redraw();
}

void VtermImpl::resizeGrid() {
    const ScreenInfo info = cf->info();
    const u16 previousColumns = info.columns;
    const u16 previousRows = info.rows;
    if (previousColumns == composer.columns && previousRows == composer.rows) {
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
        resizeScreen(frame_pri, framePriPool, cursorState);
        cf = frame_pri;
    } else {
        resizeScreen(frame_alt, frameAltPool, cursorState);
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
    marginBottom = composer.rows;
    nColsEff = composer.columns;
    hMargin = 0;
    if (tabStopsCustomized && !tabStopsRestored) {
        while (!tabStops.empty() && tabStops.back() >= composer.columns) {
            tabStops.pop_back();
        }
        if (composer.columns > previousColumns) {
            unsigned column = ((unsigned)(previousColumns) + 7) & ~7u;
            for (; column < composer.columns; column += 8) {
                tabStops.push_back((u16)(column));
            }
        }
    }
    const bool pendingWrap = lastCol;
    normalizeCursorPos();
    lastCol = pendingWrap;
    showCursor();

    outputRows.grow((size_t)(composer.rows));
    updateExtraCellCount();
    refreshBlinkingText();
    if (inBandResizeMode) {
        reportInBandResize();
    }
}

std::string VtermImpl::getLocalEcho(const u8* const begin, const u8* const end) {
    StringBuilder output((end - begin) * 2);
    for (const u8* p = begin; p < end; ++p) {
        if (*p == '\r' || *p >= ' ') {
            output.append(p, 1);
        } else {
            const u8 bytes[] = {u8'^', (u8)(*p + 0x40)};
            output.append(bytes, sizeof(bytes));
        }
    }
    return std::string((const char*)(output.data()), output.used());
}

int VtermImpl::writePty(InputKey key, VtModifier modifiers_, bool userInput) {
    const auto userDefined = userDefinedKeys.find(key);
    if (userDefined != userDefinedKeys.end()) {
        return writePty(userDefined->second.data(), userDefined->second.size(), userInput);
    }
    const auto writeKey = [&](const char* data, size_t size) {
        if (!send8BitControls) {
            return writePty(data, size, userInput);
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
        return writePty(folded, output, userInput);
    };
    modifiers = modifiers_;
    const auto& spec = getInputSpec(key);
    if (modifiers == VtModifier::none) {
        return writeKey(spec.input, spec.getLength());
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
        return writeKey(buf, k);
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

int VtermImpl::writePty(u8 ch, VtModifier modifiers, bool userInput) {
    using VM = VtModifier;

    auto uch = &ch;

    if (eightBitInput && (modifiers & VM::alt) != VM::none) {
        ch |= 0x80;
        return writePty(&ch, 1, userInput);
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

        return writePty(wbuf, pos, userInput);
    } else if ((modifiers & VM::alt) != VM::none) {
        if (altSendsEscape) {
            static u8 wbuf[2] = {'\x1b', '\0'};
            wbuf[1] = ch;
            return writePty(wbuf, 2, userInput);
        } else {
            std::vector<char> utf8_out;
            auto sinkFn = [&](char ch) {
                utf8_out.push_back(ch);
            };
            Utf8Encoder::pushUnicode(ch | 0x80, sinkFn);
            return writePty(utf8_out.data(), utf8_out.size(), userInput);
        }
    } else {
        return writePty(uch, 1, userInput);
    }
}

int VtermImpl::writeKittyKey(InputKey key, u16 modifiers, VtermKeyEventType event) {
    const KittyKeySpec spec = kittyKeySpec(key);
    if (!spec.code) {
        return 0;
    }

    const u8 flags = getKittyKeyboardFlags();
    const bool reportEvent = (flags & 0x02) && event != VtermKeyEventType::Press;
    if (isKittyRecoveryKey(key) && !(flags & 0x08) && !(modifiers & 15)) {
        if (event == VtermKeyEventType::Release) {
            return 0;
        }
        return writePty(key, kittyToLegacyModifiers(modifiers), true);
    }

    if (isKittyModifierKey(key) && !(getKittyKeyboardFlags() & 0x08)) {
        return 0;
    }

    if (event == VtermKeyEventType::Release && !(getKittyKeyboardFlags() & 0x02)) {
        return 0;
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
    return writePty((const u8*)(sequence.data()), sequence.used(), true);
}

int VtermImpl::writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event) {
    if (!key || (event == VtermKeyEventType::Release && !(getKittyKeyboardFlags() & 0x02))) {
        return 0;
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
    return writePty((const u8*)(sequence.data()), sequence.used(), true);
}

int VtermImpl::writePty(const char* cstr, bool userInput) {
    return writePty(cstr, strlen(cstr), userInput);
}

int VtermImpl::writePty(const char* data, size_t size, bool userInput) {
    return writePty((const u8*)(data), size, userInput);
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
    writePty((const u8*)(response.data()), response.used(), false);
    protocolResponseScratch = static_cast<Buffer&&>(response);
}

int VtermImpl::writePty(const u8* ucstr, size_t len, bool userInput) {
    if (len == 0) {
        return 0;
    }
    if (userInput && keyboardLocked) {
        return len;
    }

    if (userInput && cf->scrollView(-0x7fffffff)) {
        refreshBlinkingText();
        redraw();
    }

    if (userInput && localEcho) {
        const std::string localEcho = getLocalEcho(ucstr, ucstr + len);
        processInput((const u8*)localEcho.data(), (int)localEcho.size());
    }
    Output* const output = composer.ptyOutput;
    const StringView bytes(ucstr, len);
    plt::Scheduler* const scheduler = composer.platform->scheduler();
    if (scheduler->current() != nullptr && composer.ptyMutex->heldByCurrent(*scheduler)) {
        output->write(bytes.data(), bytes.length());
        output->flush();
        return len;
    }
    // Never park the caller on PTY backpressure: input delivery must stay
    // live while the stream is clogged. The fast path takes the free mutex
    // and hands the kernel what it accepts right now; only a leftover — or
    // a stream owned by a transaction — replays from a fiber of its own,
    // and spawning it before unlock() hands the mutex straight over, so
    // nothing interleaves into the middle of this write.
    if (composer.ptyMutex->tryLock()) {
        const size_t accepted = composer.pty->tryWrite(bytes.data(), bytes.length());
        if (accepted != bytes.length()) {
            spawnPtyWrite(StringView(bytes.data() + accepted, bytes.length() - accepted));
        }
        composer.ptyMutex->unlock();
        return len;
    }
    spawnPtyWrite(bytes);
    return len;
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
        return (self.modifyOtherKeys > 0 && self.modifiers != Mod::none);
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
        return (self.modifiers != Mod::none && self.modifyKeyResources[0] != 0);
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
    [[gnu::always_inline]] size_t printableAsciiPrefix(const u8* input, size_t size);
}

bool VtermImpl::windowOperationsAllowed() const {
    return opts.allowWindowOps;
}

void VtermImpl::parserWritePty(StringView bytes) {
    writePty(bytes.data(), bytes.length());
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
    return !utf8dec.expectsContinuation() && charsetState.ss == 0 && charsetState.g[charsetState.gl] == Charset::UTF8 && charsetState.g[charsetState.gr] == Charset::UTF8;
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
    [[gnu::always_inline]] size_t printableAsciiPrefix(const u8* input, size_t size) {
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
    if (insertMode || posX != 0 || lastCol || inputGraphemeScreen != nullptr || horizMarginMode || hMargin != 0 || nColsEff != composer.columns || marginTop != 0 || marginBottom != composer.rows || semanticUntilEndOfLine) {
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
        if (row < composer.rows && cf->lineAttribute(row) != 0) {
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
    posY = min<u32>((u32)(posY) + lineCount, composer.rows - 1);
    posX = 0;
    lastCol = false;
    if (attrs.blink) {
        enableBlinkingText();
    }
    return cursor - input;
}

bool VtermImpl::processInput(const u8* input, int inputSize, bool refresh) {
    ++processInputDepth;
    bool changed;
    try {
        changed = processInputImpl(input, inputSize, refresh);
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

[[gnu::noinline]] bool VtermImpl::processInputImpl(const u8* input, int inputSize, bool refresh) {
    const TerminalCursor cursorBefore = presentationCursor(cf->info().viewOffset);
    hideCursor();
    parser->feed(StringView(input, inputSize));
    syncPresentationCursor(cursorBefore);
    const bool changed = presentationChanged();
    if (refresh && changed) {
        redraw();
    }
    return changed;
}

std::string VtermImpl::getHyperlink(int pX, int pY) const {
    const StringView link = resolveHyperlink(pX, pY).payload;
    return link.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(link.data()), link.length());
}

Point VtermImpl::selectionPoint(int pX, int pY) const {
    const int contentWidth = std::max(0, (int)composer.pixelWidth - 2 * opts.border);
    const int contentHeight = std::max(1, (int)composer.pixelHeight - 2 * opts.border);
    pX = std::min(std::max(0, pX - opts.border), contentWidth);
    pY = std::min(std::max(0, pY - opts.border), contentHeight - 1);
    return cf->logicalPoint(Point(std::min(pX / composer.glyphWidth, (int)composer.columns), std::min(pY / composer.glyphHeight, (int)composer.rows - 1)));
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
            std::swap(selection.tl.x, selection.br.x);
            selectUpdatesLeft = false;
        } else if (!selectUpdatesLeft && pt.x < selection.tl.x) {
            std::swap(selection.tl.x, selection.br.x);
            selectUpdatesLeft = true;
        }

        if (selectUpdatesTop && pt.y > selection.br.y) {
            std::swap(selection.tl.y, selection.br.y);
            selectUpdatesTop = false;
        } else if (!selectUpdatesTop && pt.y < selection.tl.y) {
            std::swap(selection.tl.y, selection.br.y);
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

bool VtermImpl::selectFinish(std::string& utf8_selection) {
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
        std::swap(selection.tl.x, selection.br.x);
        selectUpdatesLeft = true;
    }
    cf->updateSelection(selection);
    changePresentation();
    redraw();
}

void VtermImpl::pasteSelection(const std::string& utf8_selection) {
    StringBuilder output(utf8_selection.size() + 12);

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
        writePty((const u8*)(output.data()), output.used(), true);
    }
}

Vterm* Vterm::create(Composer& composer, VtermTraceFactory* traceFactory) {
    Output* dump = nullptr;
    if (opts.dump != nullptr) {
        const int rawFd = ::open(opts.dump, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (rawFd < 0) {
            Errno().raise(StringBuilder() << StringView(u8"can not open dump file ") << StringView(opts.dump));
        }
        auto* fd = composer.pool->make<ScopedFD>(rawFd);
        dump = createOutBuf(composer.pool, *createFDRegular(composer.pool, *fd));
    }

    composer.cellExtras->setCellCount((size_t)(composer.columns) * (composer.rows + opts.saveLines));
    try {
        VtermImpl* const vterm = composer.pool->make<VtermImpl>(composer, traceFactory, dump);
        composer.vterm = vterm;
        vterm->resetTerminal();
        composer.resizedListeners.pushBack(composer.pool->make<CallVtermResize>(vterm));
        composer.fontChangedListeners.pushBack(composer.pool->make<CallVtermFontChanged>(vterm));
        vterm->wireInputBindings();
        composer.inputHandlers.pushBack(vterm);
        vterm->startTimers();
        return vterm;
    } catch (...) {
        composer.vterm = nullptr;
        throw;
    }
}
