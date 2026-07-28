/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "test_mode.h"

#include "cell_extra_store.h"
#include "clipboard.h"
#include "composer.h"
#include "desktop_actions.h"
#include "grapheme.h"
#include "font_pack.h"
#include "hex.h"
#include "input_sink.h"
#include "keyboard.h"
#include "listener.h"
#include "options.h"
#include "mouse_protocol.h"
#include "mouse_frontend.h"
#include "pty.h"
#include "reference_renderer.h"
#include "startup.h"
#include "utf8.h"
#include "vk_renderer.h"
#include "vterm.h"
#include "vterm_host.h"
#include "vterm_test.h"
#include "vterm_trace.h"

#include <std/dbg/assert.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <map>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace stl;

namespace {
    extern "C" int openpty(int*, int*, char*, const termios*, const winsize*);

    struct TestPty final: public Pty, public Listener {
        TestPty(Composer& composer, int fd);

        int fd() const override;
        ssize_t read(u8* buffer, size_t size) override;
        ssize_t write(const u8* buffer, size_t size) override;
        void outputReady() override;
        void onListen(void*) override;

        void setReadHandler(std::function<ssize_t(u8*, size_t)> handler);
        void setWriteHandler(std::function<ssize_t(const u8*, size_t)> handler);
        std::string takeReadData();

        void applySize();

        Composer& composer_;
        int fd_;
        std::function<ssize_t(u8*, size_t)> onRead;
        std::function<ssize_t(const u8*, size_t)> onWrite;
        std::string readData;
    };

    struct TestUtf8Decoder {
        TestUtf8Decoder();

        std::vector<u32> push(const std::string& input);

        std::vector<u32> output;
        Utf8Decoder decoder;
    };

    struct FailFontChange final: public Listener {
        void arm();
        void onListen(void*) override;

        bool armed = false;
    };
}

TestPty::TestPty(Composer& composer, int fd)
    : composer_(composer)
    , fd_(fd)
    , onRead([this](u8* buffer, size_t size) {
        return ::read(fd_, buffer, size);
    })
    , onWrite([this](const u8* buffer, size_t size) {
        return ::write(fd_, buffer, size);
    })
{
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("test PTY nonblocking setup failed");
    }
    applySize();
    composer_.resizedListeners.pushBack(this);
}

int TestPty::fd() const {
    return fd_;
}

ssize_t TestPty::read(u8* buffer, size_t size) {
    const ssize_t count = onRead(buffer, size);
    if (count > 0) {
        readData.append((const char*)(buffer), (size_t)(count));
    }
    return count;
}

ssize_t TestPty::write(const u8* buffer, size_t size) {
    return onWrite(buffer, size);
}

void TestPty::outputReady() {
}

void TestPty::onListen(void*) {
    applySize();
}

void TestPty::applySize() {
    pty_resize(fd_, composer_.columns, composer_.rows);
}

void TestPty::setReadHandler(std::function<ssize_t(u8*, size_t)> handler) {
    onRead = std::move(handler);
}

void TestPty::setWriteHandler(std::function<ssize_t(const u8*, size_t)> handler) {
    onWrite = std::move(handler);
}

std::string TestPty::takeReadData() {
    std::string result;
    result.swap(readData);
    return result;
}

TestUtf8Decoder::TestUtf8Decoder() {
}

std::vector<u32> TestUtf8Decoder::push(const std::string& input) {
    for (const unsigned char ch : input) {
        if (ch < 0x80) {
            if (decoder.checkPrematureEOS()) {
                output.push_back(decoder.getUnicode());
            }
            if (decoder.onUnicode(ch)) {
                output.push_back(decoder.getUnicode());
            }
        } else {
            for (int completed = decoder.pushByte(ch); completed > 0; --completed) {
                output.push_back(decoder.getUnicode());
            }
        }
    }
    std::vector<u32> result;
    result.swap(output);
    return result;
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

namespace {

    void writeAll(int fd, StringView data) {
        size_t offset = 0;
        while (offset < data.length()) {
            const ssize_t count = write(fd, data.data() + offset, data.length() - offset);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("test control write failed");
            }
            offset += (size_t)(count);
        }
    }

    void writeAll(int fd, const std::string& data) {
        writeAll(fd, StringView((const u8*)(data.data()), data.size()));
    }

    void writeAll(int fd, const char* data) {
        writeAll(fd, StringView(data));
    }

    std::string toString(const StringBuilder& builder) {
        return std::string((const char*)(builder.data()), builder.used());
    }

    bool readLine(int fd, std::string& buffered, std::string& line) {
        while (true) {
            const size_t newline = buffered.find('\n');
            if (newline != std::string::npos) {
                line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
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
                throw std::runtime_error("test control read failed");
            }
            buffered.append(chunk, (size_t)(count));
        }
    }

    u8 hexDigit(char ch) {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        throw std::runtime_error("invalid hex input");
    }

    std::string decodeHex(const std::string& input) {
        if (input.size() % 2) {
            throw std::runtime_error("odd-length hex input");
        }
        std::string output;
        output.reserve(input.size() / 2);
        for (size_t k = 0; k < input.size(); k += 2) {
            output.push_back((char)((hexDigit(input[k]) << 4) | hexDigit(input[k + 1])));
        }
        return output;
    }

    u8 hexDigit(u8 ch) {
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

    Buffer decodeHex(StringView input) {
        if (input.length() % 2) {
            Errno(EINVAL).raise(StringView(u8"odd-length hex input"));
        }
        Buffer output(input.length() / 2);
        for (size_t index = 0; index < input.length(); index += 2) {
            const u8 byte = (hexDigit(input[index]) << 4) | hexDigit(input[index + 1]);
            output.append(&byte, 1);
        }
        return output;
    }

    std::string encodeHex(StringView input) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(input.length() * 2);
        for (const u8 ch : input) {
            output.push_back(digits[ch >> 4]);
            output.push_back(digits[ch & 15]);
        }
        return output;
    }

    std::string encodeHex(const std::string& input) {
        return encodeHex(StringView((const u8*)(input.data()), input.size()));
    }

    void appendHex(StringBuilder& output, StringView input) {
        for (const u8 byte : input) {
            output << Hex{byte, 2};
        }
    }

    struct TestClipboard final: public Clipboard {
        StringView readPrimary() override;
        StringView readClipboard() override;
        void writePrimary(StringView content) override;
        void writeClipboard(StringView content) override;

        Buffer primary;
        Buffer system;
        u64 generation = 0;
    };

    struct TestDesktopActions final: public DesktopActions {
        bool handlesUriScheme(StringView scheme) override;
        void openUri(StringView uri) override;
        void pointerIcon(PointerIcon icon) override;

        Buffer openedUri;
        PointerIcon icon = PointerIcon::Text;
        u64 openCount = 0;
    };

    struct DisplayCell {
        TerminalCell source{};
        Color foreground;
        Color background;
        Color underlineColor;
        u32 hyperlink = 0;
        u32 grapheme = 0;
        u8 lineAttribute = 0;
    };

    struct TestDisplay final: public VtermHost {
        TestDisplay(Composer& composer, std::string& actions, std::string& printerOutput);

        void attach(TestApi& testApi);
        bool update(const TerminalUpdate& update);
        void osc(int command, StringView argument) override;
        bool handlesOsc() const override;
        void title(StringView) override;
        void cwd(StringView) override;
        void bell() override;
        bool handlesPrinter() const override;
        void print(StringView output) override;
        void leds(u8 state) override;
        void notify(StringView id, StringView title, StringView body, bool close) override;
        void progress(u32 state, u32 percent) override;
        void windowOperation(u32 operation, u32 first, u32 second) override;
        VtermWindowInfo windowInfo() override;

        void applyWindowSize(u32 pixelWidth, u32 pixelHeight);
        DisplayCell materialize(const TerminalCell& cell, u8 lineAttribute, const TerminalColors& colors) const;
        void failNextPresent();
        std::string snapshot() const;
        std::string modelSnapshot() const;
        std::string modelDigest() const;
        std::string renderState() const;
        TerminalUpdate renderUpdate() const;
        std::string scrollbackState() const;
        std::string screenText() const;

        bool failNextUpdate = false;
        u16 columns = 0;
        u16 rows = 0;
        u32 viewOffset = 0;
        u32 historyRows = 0;
        u64 refreshCount = 0;
        bool screenReverse = false;
        bool blinkVisible = true;
        bool cursorBlink = false;
        Color selectionForeground;
        Color selectionBackground;
        u8 selectionColorMask = 0;
        u32 hoveredHyperlink = 0;
        u32 hoveredLinkBegin = 0;
        u32 hoveredLinkEnd = 0;
        size_t graphemeCells = 0;
        size_t graphemeCodepoints = 0;
        size_t lastUpdateCells = 0;
        size_t lastUpdateSpans = 0;
        TerminalCursor cursor;
        Rect selection;
        std::vector<DisplayCell> cells;
        mutable Vector<TerminalCellSpan> renderSpans;
        std::vector<TerminalCell> modelCells;
        Vector<u8> modelLineAttributes;
        std::vector<std::vector<u32>> cellGraphemes;
        std::vector<CellColor> modelUnderlineColors;
        Composer& composer;
        std::string& actions;
        std::string& printerOutput;
        Buffer currentCwd;
        TestApi* testApi = nullptr;
        const TerminalColors* colors = nullptr;
        VtermWindowInfo currentWindow;
        u16 restoredPixelWidth = 0;
        u16 restoredPixelHeight = 0;
        bool haveRestoredWindow = false;
    };

    struct TestTerminal {
        TestTerminal(Vterm& terminal, TestApi& testApi, TestPty& pty, TestDisplay& display);

        void feedPtyOutput(const u8* data, size_t size);
        void update();
        void redraw();
        void resize(u16 width, u16 height);
        int writePty(VtKey key, VtModifier modifiers = VtModifier::none, bool userInput = false);
        int writePty(u8 byte, VtModifier modifiers = VtModifier::none, bool userInput = false);
        int writePty(const char* text, bool userInput = false);
        int writePty(const u8* data, size_t size, bool userInput = false);
        int writeKittyKey(VtKey key, u16 modifiers, VtermKeyEventType event);
        int writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType event);
        bool readPty();
        bool servicePty(bool readable, bool writable);
        bool flushPtyOutput();
        size_t pendingPtyOutputBytes();
        u64 droppedPtyResponses();
        MouseTrackingState getMouseTrackingState();
        u8 getKittyKeyboardFlags();
        bool getScreenReverseVideo();
        u8 getLedState();
        bool getReverseWrapMode();
        bool getNationalReplacementMode();
        bool getAnsiMode(u32 mode);
        bool getPrivateMode(u32 mode);
        TerminalCursor::Style getCursorStyle();
        TerminalPen getPenState();
        RectangleOrigin getRectangleOrigin();
        size_t getHyperlinkCount();
        std::string getHyperlink(int x, int y);
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
        bool selectFinish(std::string& selection);
        void selectRectangularModeToggle();
        void pasteSelection(const std::string& selection);
        void setHasFocus(bool focused);
        bool expireSynchronizedOutput(bool force = false);
        bool advanceAnimation(bool force = false);

        Vterm& terminal;
        TestApi& testApi;
        TestPty& pty;
        TestDisplay& display;
        bool present();
        void refreshState();
    };

    template <typename Cell>
    unsigned cellUnderline(const Cell& cell) {
        return cell.underline;
    }

    template <>
    unsigned cellUnderline(const TerminalCell& cell) {
        return cell.underlined();
    }

    template <typename Cell>
    unsigned cellFlags(const Cell& cell, u8 lineAttribute) {
        return (cell.dwidth << 0) | (cell.dwidth_cont << 1) | (cell.bold << 2) | (cell.italic << 3) | (cellUnderline(cell) << 4) | (cell.inverse << 5) | (cell.wrap << 6) | (cell.faint << 7) | (cell.blink << 8) | (cell.conceal << 9) | (cell.strike << 10) | (cell.overline << 11) | (cell.underline_style << 12) | ((cell.protected_char != 0) << 15) | (lineAttribute << 16) | (cell.drawn << 18);
    }

    unsigned cellFlags(const DisplayCell& cell) {
        return cellFlags(cell.source, cell.lineAttribute);
    }

    unsigned cellFlags(const TerminalCell& cell) {
        return cellFlags(cell, 0);
    }

    struct ModelDigest {
        u64 first = 14695981039346656037ull;
        u64 second = 1099511628211ull;

        void add(u64 value) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                const u8 byte = (u8)(value >> shift);
                first = (first ^ byte) * 1099511628211ull;
                second = (second ^ (byte + 0x9d)) * 14029467366897019727ull;
            }
        }
    };

}

StringView TestClipboard::readPrimary() {
    return StringView(primary);
}

StringView TestClipboard::readClipboard() {
    return StringView(system);
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

void TestDesktopActions::openUri(StringView uri) {
    openedUri.reset();
    openedUri.append(uri.data(), uri.length());
    ++openCount;
}

bool TestDesktopActions::handlesUriScheme(StringView scheme) {
    return scheme == StringView(u8"https") || scheme == StringView(u8"mailto");
}

void TestDesktopActions::pointerIcon(PointerIcon icon_) {
    icon = icon_;
}

TestDisplay::TestDisplay(Composer& composer_, std::string& actions, std::string& printerOutput)
    : composer(composer_)
    , actions(actions)
    , printerOutput(printerOutput)
{
    currentWindow.x = 10;
    currentWindow.y = 20;
    currentWindow.screenPixelWidth = 1920;
    currentWindow.screenPixelHeight = 1080;
}

void TestDisplay::attach(TestApi& testApiValue) {
    testApi = &testApiValue;
}

DisplayCell TestDisplay::materialize(const TerminalCell& cell, u8 lineAttribute, const TerminalColors& colors_) const {
    DisplayCell result;
    result.source = cell;
    result.foreground = colors_.resolveForeground(cell);
    result.background = colors_.resolveBackground(cell);
    result.underlineColor = result.foreground;
    result.lineAttribute = lineAttribute;
    if (cell.hasExtra()) {
        const CellExtraView extra = composer.cellExtras->view(cell);
        result.hyperlink = extra.hyperlinkDisplayId;
        result.grapheme = extra.grapheme.empty() ? 0 : cell.extraRef();
        if (extra.underlineColor != cell.foreground()) {
            result.underlineColor = colors_.resolve(extra.underlineColor);
        }
    } else if (cell.inlineUnderlineColor() != cell.foreground()) {
        result.underlineColor = colors_.resolve(cell.inlineUnderlineColor());
    }
    return result;
}

bool TestDisplay::update(const TerminalUpdate& update) {
    if (failNextUpdate) {
        failNextUpdate = false;
        return false;
    }
    const size_t count = (size_t)(composer.columns) * composer.rows;
    if (columns != composer.columns || rows != composer.rows) {
        columns = composer.columns;
        rows = composer.rows;
        cells.resize(count);
        modelCells.resize(count);
        modelLineAttributes.grow(count);
    }
    STD_ASSERT(update.colors != nullptr);
    colors = update.colors;
    lastUpdateCells = 0;
    lastUpdateSpans = update.spanCount;
    for (size_t spanIndex = 0; spanIndex < update.spanCount; ++spanIndex) {
        const TerminalCellSpan& span = update.spans[spanIndex];
        lastUpdateCells += span.count;
        STD_ASSERT((size_t)(span.index) + span.count <= count);
        STD_ASSERT(span.cells != nullptr);
        for (u32 index = 0; index < span.count; ++index) {
            cells[span.index + index] = materialize(span.cells[index], span.lineAttribute, *update.colors);
        }
    }
    cellGraphemes.resize(count);
    modelUnderlineColors.resize(count);
    for (u16 row = 0; row < rows; ++row) {
        for (u16 column = 0; column < columns; ++column) {
            const size_t index = (size_t)(row)*columns + column;
            const VtermTestCell inspected = testApi->cell(row, column);
            modelCells[index] = inspected.cell;
            modelLineAttributes.mut(index) = inspected.lineAttribute;
            if (inspected.graphemeSize == 0) {
                cellGraphemes[index].clear();
            } else {
                cellGraphemes[index].assign(inspected.grapheme, inspected.grapheme + inspected.graphemeSize);
            }
            modelUnderlineColors[index] = inspected.underlineColor;
        }
    }
    cursor = update.cursor;
    selection = update.selection;
    viewOffset = update.viewOffset;
    historyRows = update.historyRows;
    screenReverse = update.screenReverse;
    blinkVisible = update.blinkVisible;
    cursorBlink = update.cursorBlink;
    selectionForeground = update.selectionForeground;
    selectionBackground = update.selectionBackground;
    selectionColorMask = update.selectionColorMask;
    hoveredHyperlink = update.hoveredHyperlink;
    hoveredLinkBegin = update.hoveredLinkBegin;
    hoveredLinkEnd = update.hoveredLinkEnd;
    graphemeCells = 0;
    graphemeCodepoints = 0;
    for (const auto& cell : cells) {
        if (!cell.grapheme) {
            continue;
        }
        const auto grapheme = composer.cellExtras->grapheme(cell.grapheme);
        if (grapheme.empty()) {
            continue;
        }
        ++graphemeCells;
        graphemeCodepoints += grapheme.size();
    }
    ++refreshCount;
    return true;
}

void TestDisplay::osc(int command, StringView argument) {
    actions += "OSC " + std::to_string(command) + " " + encodeHex(argument) + "\n";
}

bool TestDisplay::handlesOsc() const {
    return composer.vterm != nullptr;
}

void TestDisplay::title(StringView) {
}

void TestDisplay::cwd(StringView path) {
    currentCwd.reset();
    currentCwd.append(path.data(), path.length());
}

void TestDisplay::bell() {
    actions += "BELL\n";
}

bool TestDisplay::handlesPrinter() const {
    return composer.vterm != nullptr;
}

void TestDisplay::print(StringView output) {
    printerOutput.append((const char*)(output.data()), output.length());
}

void TestDisplay::leds(u8 state) {
    if (composer.vterm == nullptr) {
        return;
    }
    actions += "LEDS " + std::to_string(state) + "\n";
}

void TestDisplay::notify(StringView id, StringView title, StringView body, bool close) {
    if (close) {
        actions += "NOTIFY_CLOSE " + encodeHex(id) + "\n";
    } else {
        actions += "NOTIFY " + encodeHex(id) + " " + encodeHex(title) + " " + encodeHex(body) + "\n";
    }
}

void TestDisplay::progress(u32 state, u32 percent) {
    actions += "PROGRESS " + std::to_string(state) + " " + std::to_string(percent) + "\n";
}

void TestDisplay::applyWindowSize(u32 pixelWidth, u32 pixelHeight) {
    composer.resize((u16)(pixelWidth), (u16)(pixelHeight));
}

void TestDisplay::windowOperation(u32 operation, u32 first, u32 second) {
    actions += "WINDOW " + std::to_string(operation) + " " + std::to_string(first) + " " + std::to_string(second) + "\n";
    if (operation == 1) {
        currentWindow.iconified = false;
    } else if (operation == 2) {
        currentWindow.iconified = true;
    } else if (operation == 3) {
        currentWindow.x = (i32)(first);
        currentWindow.y = (i32)(second);
    } else if (operation == 4 && first && second) {
        applyWindowSize(second, first);
    } else if (operation == 8 && first && second) {
        const u32 pixelWidth = 2 * opts.border + second * composer.glyphWidth;
        const u32 pixelHeight = 2 * opts.border + first * composer.glyphHeight;
        applyWindowSize(pixelWidth, pixelHeight);
    } else if (operation == 9) {
        if (first == 0) {
            if (haveRestoredWindow) {
                applyWindowSize(restoredPixelWidth, restoredPixelHeight);
                currentWindow.maximized = false;
                haveRestoredWindow = false;
            }
        } else if (first <= 3) {
            if (!haveRestoredWindow) {
                restoredPixelWidth = composer.pixelWidth;
                restoredPixelHeight = composer.pixelHeight;
                haveRestoredWindow = true;
            }
            const u32 pixelWidth = first == 2 ? composer.pixelWidth : currentWindow.screenPixelWidth;
            const u32 pixelHeight = first == 3 ? composer.pixelHeight : currentWindow.screenPixelHeight;
            applyWindowSize(pixelWidth, pixelHeight);
            currentWindow.maximized = true;
        }
    } else if (operation == 10) {
        const bool enable = first == 1 || (first == 2 && !currentWindow.fullscreen);
        if (enable && !currentWindow.fullscreen) {
            if (!haveRestoredWindow) {
                restoredPixelWidth = composer.pixelWidth;
                restoredPixelHeight = composer.pixelHeight;
                haveRestoredWindow = true;
            }
            applyWindowSize(currentWindow.screenPixelWidth, currentWindow.screenPixelHeight);
            currentWindow.fullscreen = true;
        } else if (!enable && currentWindow.fullscreen) {
            if (haveRestoredWindow) {
                applyWindowSize(restoredPixelWidth, restoredPixelHeight);
                haveRestoredWindow = false;
            }
            currentWindow.fullscreen = false;
        }
    }
}

VtermWindowInfo TestDisplay::windowInfo() {
    return currentWindow;
}

void TestDisplay::failNextPresent() {
    failNextUpdate = true;
}

std::string TestDisplay::snapshot() const {
    StringBuilder output;
    output << StringView(u8"OK ") << columns << StringView(u8" ") << rows << StringView(u8" ") << cursor.posX << StringView(u8" ") << cursor.posY << StringView(u8" ") << (unsigned)(cursor.style) << StringView(u8" ") << viewOffset << StringView(u8" ") << refreshCount << StringView(u8" ") << selection.tl.x << StringView(u8" ") << selection.tl.y << StringView(u8" ") << selection.br.x << StringView(u8" ") << selection.br.y << StringView(u8" ") << (unsigned)(selection.rectangular) << StringView(u8" ");
    for (const auto& cell : cells) {
        const unsigned flags = cellFlags(cell);
        const u32 codepoint = cell.source.uc_pt ? cell.source.uc_pt : ' ';
        output << Hex{codepoint, 8} << Hex{flags, 8} << Hex{cell.foreground.red, 2} << Hex{cell.foreground.green, 2} << Hex{cell.foreground.blue, 2} << Hex{cell.background.red, 2} << Hex{cell.background.green, 2} << Hex{cell.background.blue, 2} << Hex{cell.underlineColor.red, 2} << Hex{cell.underlineColor.green, 2} << Hex{cell.underlineColor.blue, 2} << Hex{cell.hyperlink, 8} << Hex{cell.source.semantic, 8};
    }
    output << StringView(u8"\n");
    return toString(output);
}

std::string TestDisplay::modelSnapshot() const {
    StringBuilder output;
    output << StringView(u8"OK ") << columns << StringView(u8" ") << rows << StringView(u8" ") << cursor.posX << StringView(u8" ") << cursor.posY << StringView(u8" ") << (unsigned)(cursor.style) << StringView(u8" ") << viewOffset << StringView(u8" ") << refreshCount << StringView(u8" ") << selection.tl.x << StringView(u8" ") << selection.tl.y << StringView(u8" ") << selection.br.x << StringView(u8" ") << selection.br.y << StringView(u8" ") << (unsigned)(selection.rectangular) << StringView(u8" ");
    for (size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells[index];
        const auto& modelCell = modelCells[index];
        const unsigned flags = cellFlags(modelCell, modelLineAttributes[index]);
        const u32 codepoint = cell.source.uc_pt ? cell.source.uc_pt : ' ';
        output << Hex{codepoint, 8} << Hex{flags, 8} << Hex{cell.foreground.red, 2} << Hex{cell.foreground.green, 2} << Hex{cell.foreground.blue, 2} << Hex{cell.background.red, 2} << Hex{cell.background.green, 2} << Hex{cell.background.blue, 2} << Hex{cell.underlineColor.red, 2} << Hex{cell.underlineColor.green, 2} << Hex{cell.underlineColor.blue, 2} << Hex{cell.hyperlink, 8} << Hex{cell.source.semantic, 8} << Hex{(u32)(modelCell.foreground().legacyIndex()), 8} << Hex{(u32)(modelCell.background().legacyIndex()), 8} << Hex{(u32)(modelUnderlineColors[index].legacyIndex()), 8} << Hex{cellGraphemes[index].size(), 8};
        for (const u32 codepoint : cellGraphemes[index]) {
            output << Hex{codepoint, 8};
        }
    }
    output << StringView(u8"\n");
    return toString(output);
}

std::string TestDisplay::modelDigest() const {
    ModelDigest digest;
    digest.add(columns);
    digest.add(rows);
    digest.add(cursor.style == TerminalCursor::Style::hidden ? (u64)-1 : cursor.posX);
    digest.add(cursor.style == TerminalCursor::Style::hidden ? (u64)-1 : cursor.posY);
    digest.add((u8)(cursor.style));
    digest.add(viewOffset);
    digest.add(selection.tl.x);
    digest.add(selection.tl.y);
    digest.add(selection.br.x);
    digest.add(selection.br.y);
    digest.add(selection.rectangular);
    digest.add(cells.size());
    for (size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells[index];
        const auto& modelCell = modelCells[index];
        digest.add(cell.source.uc_pt ? cell.source.uc_pt : ' ');
        digest.add(cellFlags(modelCell, modelLineAttributes[index]));
        digest.add(cell.foreground.red);
        digest.add(cell.foreground.green);
        digest.add(cell.foreground.blue);
        digest.add(cell.background.red);
        digest.add(cell.background.green);
        digest.add(cell.background.blue);
        digest.add(cell.underlineColor.red);
        digest.add(cell.underlineColor.green);
        digest.add(cell.underlineColor.blue);
        digest.add(cell.hyperlink);
        digest.add(cell.source.semantic);
        digest.add((u32)(modelCell.foreground().legacyIndex()));
        digest.add((u32)(modelCell.background().legacyIndex()));
        digest.add((u32)(modelUnderlineColors[index].legacyIndex()));
        digest.add(cellGraphemes[index].size());
        for (const u32 codepoint : cellGraphemes[index]) {
            digest.add(codepoint);
        }
    }

    StringBuilder output;
    output << StringView(u8"OK ") << Hex{digest.first, 16} << StringView(u8" ") << Hex{digest.second, 16} << StringView(u8"\n");
    return toString(output);
}

std::string TestDisplay::scrollbackState() const {
    StringBuilder output;
    output << StringView(u8"OK ") << historyRows << StringView(u8" ") << historyRows + rows << StringView(u8" ") << rows << StringView(u8" ") << historyRows - viewOffset << StringView(u8"\n");
    return toString(output);
}

std::string TestDisplay::renderState() const {
    StringBuilder output;
    output << StringView(u8"OK ") << (unsigned)(screenReverse) << StringView(u8" ") << (unsigned)(blinkVisible) << StringView(u8" ") << (unsigned)(cursorBlink) << StringView(u8" ") << (unsigned)(selectionColorMask) << StringView(u8" ") << (unsigned)(selectionForeground.red) << StringView(u8" ") << (unsigned)(selectionForeground.green) << StringView(u8" ") << (unsigned)(selectionForeground.blue) << StringView(u8" ") << (unsigned)(selectionBackground.red) << StringView(u8" ") << (unsigned)(selectionBackground.green) << StringView(u8" ") << (unsigned)(selectionBackground.blue) << StringView(u8" ") << graphemeCells << StringView(u8" ") << graphemeCodepoints << StringView(u8"\n");
    return toString(output);
}

TerminalUpdate TestDisplay::renderUpdate() const {
    renderSpans.clear();
    renderSpans.grow(rows);
    for (u16 row = 0; row < rows; ++row) {
        renderSpans.pushBack({
            (u32)(row)*columns,
            columns,
            modelCells.data() + (size_t)(row)*columns,
            modelLineAttributes[(size_t)(row)*columns],
        });
    }
    return {
        .spans = renderSpans.data(),
        .spanCount = renderSpans.length(),
        .colors = colors,
        .viewOffset = viewOffset,
        .historyRows = historyRows,
        .cursor = cursor,
        .selection = selection,
        .snappedSelection = selection,
        .selectionForeground = selectionForeground,
        .selectionBackground = selectionBackground,
        .selectionColorMask = selectionColorMask,
        .hoveredHyperlink = hoveredHyperlink,
        .hoveredLinkBegin = hoveredLinkBegin,
        .hoveredLinkEnd = hoveredLinkEnd,
        .screenReverse = screenReverse,
        .blinkVisible = blinkVisible,
        .cursorBlink = cursorBlink,
    };
}

std::string TestDisplay::screenText() const {
    std::string output;
    output.reserve(cells.size() + rows);
    for (size_t index = 0; index < cells.size(); ++index) {
        const u32 codepoint = cells[index].source.uc_pt;
        output.push_back(codepoint >= 0x20 && codepoint <= 0x7e ? (char)(codepoint) : ' ');
        if ((index + 1) % columns == 0) {
            output.push_back('\n');
        }
    }
    return output;
}

TestTerminal::TestTerminal(Vterm& terminal, TestApi& testApi, TestPty& pty, TestDisplay& display)
    : terminal(terminal)
    , testApi(testApi)
    , pty(pty)
    , display(display)
{
    refreshState();
}

bool TestTerminal::present() {
    while (true) {
        const TerminalUpdate* const output = terminal.output();
        if (output == nullptr) {
            return true;
        }
        if (!display.update(*output)) {
            return false;
        }
        terminal.consume();
        refreshState();
    }
}

void TestTerminal::refreshState() {
}

void TestTerminal::feedPtyOutput(const u8* data, size_t size) {
    terminal.feedPty(StringView(data, size));
    update();
}

void TestTerminal::update() {
    flushPtyOutput();
    present();
    refreshState();
}

void TestTerminal::redraw() {
    terminal.expose();
    update();
}

void TestTerminal::resize(u16 width, u16 height) {
    display.applyWindowSize(width, height);
    update();
}

int TestTerminal::writePty(VtKey key, VtModifier modifiers, bool) {
    testApi.key(key, modifiers);
    update();
    return 1;
}

int TestTerminal::writePty(u8 byte, VtModifier modifiers, bool) {
    testApi.character(byte, modifiers);
    update();
    return 1;
}

int TestTerminal::writePty(const char* text, bool userInput) {
    return writePty((const u8*)(text), strlen(text), userInput);
}

int TestTerminal::writePty(const u8* data, size_t size, bool userInput) {
    terminal.sendBytes(StringView(data, size), userInput);
    update();
    return size;
}

int TestTerminal::writeKittyKey(VtKey key, u16 modifiers, VtermKeyEventType keyEvent) {
    testApi.kittyKey(key, modifiers, keyEvent);
    update();
    return 1;
}

int TestTerminal::writeKittyKey(u32 key, u32 shiftedKey, u32 baseLayoutKey, u16 modifiers, VtermKeyEventType keyEvent) {
    testApi.kittyKey(key, shiftedKey, baseLayoutKey, modifiers, keyEvent);
    update();
    return 1;
}

bool TestTerminal::flushPtyOutput() {
    while (true) {
        const StringView output = terminal.ptyOutput();
        if (output.empty()) {
            return true;
        }
        const ssize_t count = pty.write(output.data(), output.length());
        if (count > 0) {
            terminal.consumePtyOutput((size_t)(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

size_t TestTerminal::pendingPtyOutputBytes() {
    return terminal.ptyOutput().length();
}

u64 TestTerminal::droppedPtyResponses() {
    return testApi.inspect().droppedPtyResponses;
}

bool TestTerminal::readPty() {
    constexpr size_t maxDrainBytes = 256 * 1024;
    constexpr u64 maxDrainMicroseconds = 4'000;
    u8 buffer[8192];
    size_t drained = 0;
    bool finished = false;
    const u64 deadline = monotonicNowUs() + maxDrainMicroseconds;
    while (drained < maxDrainBytes && monotonicNowUs() < deadline) {
        const ssize_t count = pty.read(buffer, sizeof(buffer));
        if (count > 0) {
            terminal.feedPty(StringView(buffer, count));
            drained += count;
            continue;
        }
        if (count == 0 || (count < 0 && errno == EIO)) {
            finished = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        finished = true;
        break;
    }
    flushPtyOutput();
    present();
    refreshState();
    return finished;
}

bool TestTerminal::servicePty(bool readable, bool writable) {
    if (writable) {
        flushPtyOutput();
    }
    return readable && readPty();
}

MouseTrackingState TestTerminal::getMouseTrackingState() {
    return testApi.inspect().mouse;
}

u8 TestTerminal::getKittyKeyboardFlags() {
    return testApi.inspect().kittyKeyboardFlags;
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

std::string TestTerminal::getHyperlink(int x, int y) {
    const StringView result = testApi.hyperlinkAt(x, y);
    return std::string((const char*)(result.data()), result.length());
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

bool TestTerminal::selectFinish(std::string& selection) {
    const VtermTextResult result = testApi.selectionFinish();
    selection.assign((const char*)(result.text.data()), result.text.length());
    update();
    return result.status;
}

void TestTerminal::selectRectangularModeToggle() {
    testApi.selectionRectangular();
    update();
}

void TestTerminal::pasteSelection(const std::string& selection) {
    testApi.paste(StringView((const u8*)(selection.data()), selection.size()));
    update();
}

void TestTerminal::setHasFocus(bool focused) {
    display.composer.input->focus(focused);
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

namespace {

    std::string drainInput(int fd) {
        std::string output;
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
            if (count == 0) {
                break;
            }
            throw std::runtime_error("test PTY read failed");
        }
        return output;
    }

    VtKey parseKey(const std::string& name) {
        static const std::map<std::string, VtKey> keys = {
            {"SPACE", VtKey::Space},
            {"RETURN", VtKey::Return},
            {"BACKSPACE", VtKey::Backspace},
            {"TAB", VtKey::Tab},
            {"UP", VtKey::Up},
            {"DOWN", VtKey::Down},
            {"LEFT", VtKey::Left},
            {"RIGHT", VtKey::Right},
            {"INSERT", VtKey::Insert},
            {"DELETE", VtKey::Delete},
            {"HOME", VtKey::Home},
            {"END", VtKey::End},
            {"PAGE_UP", VtKey::PageUp},
            {"PAGE_DOWN", VtKey::PageDown},
            {"F1", VtKey::F1},
            {"F2", VtKey::F2},
            {"F3", VtKey::F3},
            {"F4", VtKey::F4},
            {"F5", VtKey::F5},
            {"F6", VtKey::F6},
            {"F7", VtKey::F7},
            {"F8", VtKey::F8},
            {"F9", VtKey::F9},
            {"F10", VtKey::F10},
            {"F11", VtKey::F11},
            {"F12", VtKey::F12},
            {"F13", VtKey::F13},
            {"F14", VtKey::F14},
            {"F15", VtKey::F15},
            {"F16", VtKey::F16},
            {"F17", VtKey::F17},
            {"F18", VtKey::F18},
            {"F19", VtKey::F19},
            {"F20", VtKey::F20},
            {"KP_F1", VtKey::KP_F1},
            {"KP_F2", VtKey::KP_F2},
            {"KP_F3", VtKey::KP_F3},
            {"KP_F4", VtKey::KP_F4},
            {"KP_PLUS", VtKey::KP_Plus},
            {"KP_MINUS", VtKey::KP_Minus},
            {"KP_STAR", VtKey::KP_Star},
            {"KP_SLASH", VtKey::KP_Slash},
            {"KP_COMMA", VtKey::KP_Comma},
            {"KP_DOT", VtKey::KP_Dot},
            {"KP_EQUAL", VtKey::KP_Equal},
            {"KP_TAB", VtKey::KP_Tab},
            {"KP_SPACE", VtKey::KP_Space},
            {"KP_ENTER", VtKey::KP_Enter},
            {"KP_LEFT", VtKey::KP_Left},
            {"KP_RIGHT", VtKey::KP_Right},
            {"KP_UP", VtKey::KP_Up},
            {"KP_DOWN", VtKey::KP_Down},
            {"KP_HOME", VtKey::KP_Home},
            {"KP_END", VtKey::KP_End},
            {"KP_PAGE_UP", VtKey::KP_PageUp},
            {"KP_PAGE_DOWN", VtKey::KP_PageDown},
            {"KP_INSERT", VtKey::KP_Insert},
            {"KP_DELETE", VtKey::KP_Delete},
            {"KP_BEGIN", VtKey::KP_Begin},
            {"KP_0", VtKey::KP_0},
            {"KP_1", VtKey::KP_1},
            {"KP_2", VtKey::KP_2},
            {"KP_3", VtKey::KP_3},
            {"KP_4", VtKey::KP_4},
            {"KP_5", VtKey::KP_5},
            {"KP_6", VtKey::KP_6},
            {"KP_7", VtKey::KP_7},
            {"KP_8", VtKey::KP_8},
            {"KP_9", VtKey::KP_9},
            {"CAPS_LOCK", VtKey::CapsLock},
            {"SCROLL_LOCK", VtKey::ScrollLock},
            {"NUM_LOCK", VtKey::NumLock},
            {"PRINT", VtKey::Print},
            {"PAUSE", VtKey::Pause},
            {"MENU", VtKey::Menu},
            {"LEFT_SHIFT", VtKey::LeftShift},
            {"LEFT_CONTROL", VtKey::LeftControl},
            {"LEFT_ALT", VtKey::LeftAlt},
            {"LEFT_SUPER", VtKey::LeftSuper},
            {"RIGHT_SHIFT", VtKey::RightShift},
            {"RIGHT_CONTROL", VtKey::RightControl},
            {"RIGHT_ALT", VtKey::RightAlt},
            {"RIGHT_SUPER", VtKey::RightSuper},
        };
        const auto found = keys.find(name);
        if (found == keys.end()) {
            throw std::runtime_error("unknown key");
        }
        return found->second;
    }
}

int runTestMode(Composer& composer, TestModeInput& input, int controlFd, int argc, char* argv[]) {
    int io[2];
    if (openpty(&io[0], &io[1], nullptr, nullptr, nullptr) < 0) {
        throw std::runtime_error("test openpty failed");
    }
    termios childTtyAttrs;
    if (tcgetattr(io[1], &childTtyAttrs) < 0) {
        close(io[0]);
        close(io[1]);
        throw std::runtime_error("test tcgetattr failed");
    }
    termios ttyAttrs = childTtyAttrs;
    cfmakeraw(&ttyAttrs);
    if (tcsetattr(io[1], TCSANOW, &ttyAttrs) < 0) {
        close(io[0]);
        close(io[1]);
        throw std::runtime_error("test tcsetattr failed");
    }
    const int flags = fcntl(io[1], F_GETFL, 0);
    if (flags < 0 || fcntl(io[1], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(io[0]);
        close(io[1]);
        throw std::runtime_error("test socket setup failed");
    }

    {
        unsigned glyphWidth = 1;
        unsigned glyphHeight = 1;
        if (const char* geometry = std::getenv("SHITTY_TEST_GLYPH")) {
            std::istringstream input(geometry);
            char separator = 0;
            if (!(input >> glyphWidth >> separator >> glyphHeight) || separator != 'x' || !glyphWidth || !glyphHeight || input.peek() != EOF) {
                throw std::runtime_error("invalid test glyph geometry");
            }
        }
        composer.setGlyphSize(glyphWidth, glyphHeight);
    }
    const u16 width = 2 * opts.border + opts.nCols * composer.glyphWidth;
    const u16 height = 2 * opts.border + opts.nRows * composer.glyphHeight;
    composer.resize(width, height);
    TestPty terminalPty(composer, io[0]);
    composer.pty = &terminalPty;
    std::string actions;
    std::string printerOutput;
    TestClipboard clipboard;
    TestDesktopActions desktopActions;
    composer.clipboard = &clipboard;
    composer.desktopActions = &desktopActions;
    TestDisplay display(composer, actions, printerOutput);
    VtermTrace& vtermTrace = *VtermTrace::create(composer);
    Vterm& vterm = *Vterm::create(composer, display, &vtermTrace);
    composer.vterm = &vterm;
    TestApi& testApi = *vterm.testApi();
    display.attach(testApi);
    TestTerminal terminal(vterm, testApi, terminalPty, display);
    FailFontChange failFontChange;
    composer.fontChangedListeners.pushFront(&failFontChange);
    pid_t childPid = -1;
    int childExitStatus = -1;

    struct ScriptedPtyRead {
        std::string data;
        int error = 0;
        bool eof = false;
    };

    std::deque<ScriptedPtyRead> scriptedPtyReads;

    struct ScriptedPtyWrite {
        size_t count = 0;
        int error = 0;
    };

    std::deque<ScriptedPtyWrite> scriptedPtyWrites;
    std::string writtenPtyData;
    TestUtf8Decoder testUtf8Decoder;
    const auto installScriptedPtyReader = [&]() {
        terminalPty.setReadHandler([&scriptedPtyReads](u8* buffer, size_t size) {
            if (scriptedPtyReads.empty()) {
                errno = EAGAIN;
                return (ssize_t)(-1);
            }
            auto& item = scriptedPtyReads.front();
            if (item.eof) {
                scriptedPtyReads.pop_front();
                return (ssize_t)(0);
            }
            if (item.error) {
                errno = item.error;
                scriptedPtyReads.pop_front();
                return (ssize_t)(-1);
            }
            const size_t count = std::min(size, item.data.size());
            std::copy_n(item.data.data(), count, buffer);
            item.data.erase(0, count);
            if (item.data.empty()) {
                scriptedPtyReads.pop_front();
            }
            return (ssize_t)(count);
        });
    };
    terminal.redraw();
    writeAll(controlFd, "READY\n");

    const auto pumpChild = [&]() {
        terminal.flushPtyOutput();
        pollfd source{io[0], POLLIN, 0};
        if (poll(&source, 1, 0) > 0 && (source.revents & POLLIN)) {
            terminal.readPty();
        }
        terminal.flushPtyOutput();
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
            // The child's final output may have entered the PTY buffer after
            // the poll above.  By reap time every write has completed, so one
            // more drain keeps the reported exit status consistent with the
            // final screen.
            terminal.readPty();
            terminal.flushPtyOutput();
        }
    };

    std::string buffered;
    std::string line;
    while (readLine(controlFd, buffered, line)) {
        try {
            if (line.compare(0, 6, "WRITE ") == 0) {
                const std::string input = decodeHex(line.substr(6));
                terminal.feedPtyOutput((const u8*)input.data(), input.size());
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 15, "MEASURE_WIDTHS ") == 0) {
                std::istringstream args(line.substr(15));
                std::string encoded;
                std::string input;
                size_t count = 0;
                while (args >> encoded) {
                    input += "\x1b"
                             "c";
                    input += decodeHex(encoded);
                    input += "\x1b[6n";
                    ++count;
                }
                if (!count) {
                    throw std::runtime_error("empty width measurement");
                }
                drainInput(io[1]);
                terminal.feedPtyOutput((const u8*)input.data(), input.size());
                // The kernel delivers PTY master writes to the slave through
                // an asynchronous worker, so under load the reports may not
                // be readable immediately.  Every measurement produces
                // exactly one R-terminated cursor position report; wait
                // until all of them have arrived.
                std::string replies = drainInput(io[1]);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while ((size_t)(std::count(replies.begin(), replies.end(), 'R')) < count && std::chrono::steady_clock::now() < deadline) {
                    terminal.flushPtyOutput();
                    pollfd pending{io[1], POLLIN, 0};
                    if (poll(&pending, 1, 100) > 0 && (pending.revents & POLLIN)) {
                        replies += drainInput(io[1]);
                    }
                }
                writeAll(controlFd, "OK " + encodeHex(replies) + "\n");
            } else if (line == "OPTIONS") {
                const auto packedColor = [](Color color) {
                    return ((u32)(color.red) << 16) | ((u32)(color.green) << 8) | color.blue;
                };
                writeAll(controlFd, "OK fontsize=" + std::to_string(opts.fontsize) + " border=" + std::to_string(opts.border) + " columns=" + std::to_string(opts.nCols) + " rows=" + std::to_string(opts.nRows) + " save_lines=" + std::to_string(opts.saveLines) + " fg=" + std::to_string(packedColor(opts.fg)) + " bg=" + std::to_string(packedColor(opts.bg)) + " cr=" + std::to_string(packedColor(opts.cr)) + " alt_scroll=" + std::to_string(opts.altScrollMode) + " bold_colors=" + std::to_string(opts.boldColors) + " auto_copy=" + std::to_string(opts.autoCopyMode) + " allow_osc52_read=" + std::to_string(opts.allowOsc52Read) + " allow_window_ops=" + std::to_string(opts.allowWindowOps) + "\n");
            } else if (line == "ARGV") {
                std::string arguments;
                for (int index = 0; index < argc; ++index) {
                    if (index) {
                        arguments.push_back('\0');
                    }
                    arguments += argv[index];
                }
                writeAll(controlFd, "OK " + encodeHex(arguments) + "\n");
            } else if (line == "LAUNCH_COMMAND") {
                const LaunchCommand command = buildLaunchCommand(argc, argv, opts.shell, opts.login);
                std::string encoded = command.executable;
                for (const auto& argument : command.arguments) {
                    encoded.push_back('\0');
                    encoded += argument;
                }
                writeAll(controlFd, "OK " + encodeHex(encoded) + "\n");
            } else if (line.compare(0, 10, "FONT_LOAD ") == 0) {
                const std::string request = decodeHex(line.substr(10));
                const size_t first = request.find('\0');
                if (first == std::string::npos) {
                    throw std::runtime_error("invalid font load request");
                }
                ObjPool::Ref fontPool = ObjPool::fromMemory();
                const StringView fontname((const u8*)(request.data()), first);
                const StringView dwfontname((const u8*)(request.data() + first + 1), request.size() - first - 1);
                Fontpack* fonts = Fontpack::create(composer, *fontPool, fontname, dwfontname, opts.fontsize);
                writeAll(controlFd, "OK " + std::to_string(fonts->getPx()) + " " + std::to_string(fonts->getPy()) + " " + std::to_string(fonts->hasBold()) + " " + std::to_string(fonts->hasItalic()) + " " + std::to_string(fonts->hasBoldItalic()) + " " + std::to_string(fonts->hasDoubleWidth()) + "\n");
            } else if (line.compare(0, 13, "RENDER_IMAGE ") == 0) {
                const std::string request = decodeHex(line.substr(13));
                const size_t first = request.find('\0');
                if (first == std::string::npos) {
                    throw std::runtime_error("invalid render image request");
                }
                ObjPool::Ref renderPool = ObjPool::fromMemory();
                Composer renderComposer(renderPool.mutPtr());
                const StringView fontname((const u8*)(request.data()), first);
                const StringView dwfontname((const u8*)(request.data() + first + 1), request.size() - first - 1);
                Fontpack* fonts = Fontpack::create(renderComposer, *renderPool, fontname, dwfontname, opts.fontsize);
                renderComposer.fonts = fonts;
                renderComposer.setCellExtras(composer.cellExtras);
                renderComposer.setGlyphSize(fonts->getPx(), fonts->getPy());
                renderComposer.resize(2 * opts.border + display.columns * fonts->getPx(), 2 * opts.border + display.rows * fonts->getPy());
                ReferenceRenderer* renderer = ReferenceRenderer::create(renderComposer);
                const ReferenceImage image = renderer->render(display.renderUpdate());
                const std::string pixels((const char*)(image.pixels), image.length);
                writeAll(controlFd, "OK " + std::to_string(image.width) + " " + std::to_string(image.height) + " " + encodeHex(pixels) + "\n");
            } else if (line.compare(0, 16, "GRAPHEME_BREAKS ") == 0) {
                std::istringstream args(line.substr(16));
                std::string token;
                std::string boundaries;
                GraphemeBreaker breaker;
                while (args >> token) {
                    size_t consumed = 0;
                    const unsigned long value = std::stoul(token, &consumed, 16);
                    if (consumed != token.size() || value > 0x10ffff) {
                        throw std::runtime_error("invalid codepoint");
                    }
                    boundaries += breaker.breakBefore(value) ? '1' : '0';
                }
                if (boundaries.empty()) {
                    throw std::runtime_error("empty grapheme sequence");
                }
                writeAll(controlFd, "OK " + boundaries + "\n");
            } else if (line.compare(0, 6, "INPUT ") == 0) {
                const std::string input = decodeHex(line.substr(6));
                const std::u8string bytes(input.begin(), input.end());
                terminal.writePty(bytes.data(), bytes.size(), false);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 6, "SPAWN ") == 0) {
                if (childPid > 0) {
                    throw std::runtime_error("child already running");
                }
                if (tcsetattr(io[1], TCSANOW, &childTtyAttrs) < 0) {
                    throw std::runtime_error("test child tcsetattr failed");
                }
                const std::string encoded = decodeHex(line.substr(6));
                std::vector<std::string> arguments;
                size_t start = 0;
                while (start < encoded.size()) {
                    const size_t end = encoded.find('\0', start);
                    arguments.push_back(encoded.substr(start, end == std::string::npos ? std::string::npos : end - start));
                    if (end == std::string::npos) {
                        break;
                    }
                    start = end + 1;
                }
                if (arguments.empty() || arguments[0].empty()) {
                    throw std::runtime_error("empty child command");
                }
                const char* ttyPath = ttyname(io[1]);
                if (!ttyPath) {
                    throw std::runtime_error("test child tty has no path");
                }
                const std::string childTtyPath = ttyPath;
                childExitStatus = -1;
                childPid = fork();
                if (childPid < 0) {
                    throw std::runtime_error("test fork failed");
                }
                if (childPid == 0) {
                    setsid();
                    close(io[1]);
                    const int childTty = open(childTtyPath.c_str(), O_RDWR);
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
                    configureTerminalChildEnvironment();
                    std::vector<char*> argv;
                    for (auto& argument : arguments) {
                        argv.push_back(argument.data());
                    }
                    argv.push_back(nullptr);
                    execvp(argv[0], argv.data());
                    _exit(127);
                }
                writeAll(controlFd, "OK\n");
            } else if (line == "PUMP") {
                pumpChild();
                writeAll(controlFd, "OK\n");
            } else if (line == "READ_PTY") {
                writeAll(controlFd, "OK " + std::to_string(terminal.readPty()) + "\n");
            } else if (line == "READ_CHILD_OUTPUT") {
                writeAll(controlFd, "OK " + encodeHex(terminalPty.takeReadData()) + "\n");
            } else if (line.compare(0, 16, "PTY_READ_SCRIPT ") == 0) {
                scriptedPtyReads.clear();
                std::istringstream args(line.substr(16));
                std::string token;
                while (args >> token) {
                    if (token == "z") {
                        scriptedPtyReads.push_back({"", 0, true});
                    } else if (token.size() > 1 && token[0] == 'd') {
                        scriptedPtyReads.push_back({decodeHex(token.substr(1)), 0, false});
                    } else if (token.size() > 1 && token[0] == 'e') {
                        size_t consumed = 0;
                        const int error = std::stoi(token.substr(1), &consumed);
                        if (consumed != token.size() - 1 || error <= 0) {
                            throw std::runtime_error("invalid PTY errno");
                        }
                        scriptedPtyReads.push_back({"", error, false});
                    } else {
                        throw std::runtime_error("invalid PTY read script");
                    }
                }
                if (scriptedPtyReads.empty()) {
                    throw std::runtime_error("empty PTY read script");
                }
                installScriptedPtyReader();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 16, "PTY_READ_REPEAT ") == 0) {
                std::istringstream args(line.substr(16));
                unsigned byte;
                size_t count;
                int eof;
                if (!(args >> byte >> count >> eof) || byte > 255 || count == 0 || count > 64 * 1024 * 1024 || eof < 0 || eof > 1) {
                    throw std::runtime_error("invalid repeated PTY input");
                }
                scriptedPtyReads.clear();
                scriptedPtyReads.push_back({std::string(count, (char)(byte)), 0, false});
                if (eof) {
                    scriptedPtyReads.push_back({"", 0, true});
                }
                installScriptedPtyReader();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 17, "PTY_WRITE_SCRIPT ") == 0) {
                scriptedPtyWrites.clear();
                writtenPtyData.clear();
                std::istringstream args(line.substr(17));
                std::string token;
                while (args >> token) {
                    size_t consumed = 0;
                    if (token.size() > 1 && token[0] == 'n') {
                        const unsigned long count = std::stoul(token.substr(1), &consumed);
                        if (consumed != token.size() - 1 || count == 0) {
                            throw std::runtime_error("invalid PTY write count");
                        }
                        scriptedPtyWrites.push_back({count, 0});
                    } else if (token.size() > 1 && token[0] == 'e') {
                        const int error = std::stoi(token.substr(1), &consumed);
                        if (consumed != token.size() - 1 || error <= 0) {
                            throw std::runtime_error("invalid PTY write errno");
                        }
                        scriptedPtyWrites.push_back({0, error});
                    } else {
                        throw std::runtime_error("invalid PTY write script");
                    }
                }
                if (scriptedPtyWrites.empty()) {
                    throw std::runtime_error("empty PTY write script");
                }
                terminalPty.setWriteHandler([&scriptedPtyWrites, &writtenPtyData](const u8* buffer, size_t size) {
                    if (scriptedPtyWrites.empty()) {
                        errno = EAGAIN;
                        return (ssize_t)(-1);
                    }
                    const auto item = scriptedPtyWrites.front();
                    scriptedPtyWrites.pop_front();
                    if (item.error) {
                        errno = item.error;
                        return (ssize_t)(-1);
                    }
                    const size_t count = std::min(size, item.count);
                    writtenPtyData.append((const char*)(buffer), count);
                    return (ssize_t)(count);
                });
                writeAll(controlFd, "OK\n");
            } else if (line == "WAIT_READ_PTY") {
                pollfd source{io[0], POLLIN, 0};
                int ready = 0;
                do {
                    ready = poll(&source, 1, 1000);
                } while (ready < 0 && errno == EINTR);
                if (ready <= 0 || !(source.revents & POLLIN)) {
                    throw std::runtime_error("PTY input timeout");
                }
                terminal.readPty();
                writeAll(controlFd, "OK\n");
            } else if (line == "FAIL_NEXT_PRESENT") {
                display.failNextPresent();
                writeAll(controlFd, "OK\n");
            } else if (line == "FAIL_NEXT_FONT_CHANGE") {
                failFontChange.arm();
                writeAll(controlFd, "OK\n");
            } else if (line == "PRESENT") {
                terminal.redraw();
                writeAll(controlFd, "OK\n");
            } else if (line == "GPU_ATTRIBUTE_MASKS") {
                TerminalCell cell{};
                cell.dwidth = true;
                const u32 doubleWidth = Renderer::rendererCellAttributesForTest(cell);
                cell.dwidth = false;
                cell.dwidth_cont = true;
                const u32 continuation = Renderer::rendererCellAttributesForTest(cell);
                writeAll(controlFd, "OK " + std::to_string(doubleWidth) + " " + std::to_string(continuation) + "\n");
            } else if (line == "POLL_CHILD") {
                pumpChild();
                writeAll(controlFd, "OK " + std::to_string(childPid > 0) + " " + std::to_string(childExitStatus) + " " + encodeHex(display.screenText()) + "\n");
            } else if (line == "CHILD_STATUS") {
                writeAll(controlFd, "OK " + std::to_string(childPid > 0) + " " + std::to_string(childExitStatus) + "\n");
            } else if (line == "PAGE_UP") {
                terminal.pageUp();
                writeAll(controlFd, "OK\n");
            } else if (line == "PAGE_DOWN") {
                terminal.pageDown();
                writeAll(controlFd, "OK\n");
            } else if (line == "WHEEL_UP") {
                terminal.mouseWheelUp();
                writeAll(controlFd, "OK\n");
            } else if (line == "WHEEL_DOWN") {
                terminal.mouseWheelDown();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 7, "SCROLL ") == 0) {
                std::istringstream args(line.substr(7));
                double x;
                double y;
                unsigned modifiers;
                int pixelX;
                int pixelY;
                if (!(args >> x >> y >> modifiers >> pixelX >> pixelY) || modifiers > 7) {
                    throw std::runtime_error("invalid scroll event");
                }
                composer.input->scroll({x, y, pixelX, pixelY, (u16)(modifiers)});
                terminal.update();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 8, "POINTER ") == 0) {
                std::istringstream args(line.substr(8));
                double x, y, scaleX, scaleY;
                unsigned modifiers;
                if (!(args >> x >> y >> modifiers >> scaleX >> scaleY) || modifiers > 7) {
                    throw std::runtime_error("invalid pointer event");
                }
                const int pixelX = mouseFramebufferCoordinate(x, scaleX);
                const int pixelY = mouseFramebufferCoordinate(y, scaleY);
                composer.input->pointerMotion({pixelX, pixelY, (u16)(modifiers)});
                terminal.update();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 7, "BUTTON ") == 0) {
                std::istringstream args(line.substr(7));
                int button;
                unsigned pressed, modifiers;
                double x, y, time, scaleX, scaleY;
                if (!(args >> button >> pressed >> x >> y >> modifiers >> time >> scaleX >> scaleY) || button < 0 || button > 7 || pressed > 1 || modifiers > 7) {
                    throw std::runtime_error("invalid button event");
                }
                const int pixelX = mouseFramebufferCoordinate(x, scaleX);
                const int pixelY = mouseFramebufferCoordinate(y, scaleY);
                const u64 clipboardGeneration = clipboard.generation;
                composer.input->pointerButton({(PointerButton)(button), pressed != 0, pixelX, pixelY, (u16)(modifiers), time});
                terminal.update();
                std::string selection;
                if (clipboard.generation != clipboardGeneration) {
                    const StringView content = composer.clipboard->readPrimary();
                    selection.assign((const char*)(content.data()), content.length());
                }
                writeAll(controlFd, "OK " + encodeHex(selection) + "\n");
            } else if (line.compare(0, 7, "RESIZE ") == 0) {
                std::istringstream args(line.substr(7));
                unsigned columns;
                unsigned rows;
                if (!(args >> columns >> rows) || !columns || !rows) {
                    throw std::runtime_error("invalid resize");
                }
                terminal.resize(2 * opts.border + columns * composer.glyphWidth, 2 * opts.border + rows * composer.glyphHeight);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 14, "RESIZE_PIXELS ") == 0) {
                std::istringstream args(line.substr(14));
                unsigned pixelWidth;
                unsigned pixelHeight;
                if (!(args >> pixelWidth >> pixelHeight) || pixelWidth <= 2 * opts.border || pixelHeight <= 2 * opts.border) {
                    throw std::runtime_error("invalid pixel resize");
                }
                terminal.resize(pixelWidth, pixelHeight);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 12, "WINDOW_INFO ") == 0) {
                std::istringstream args(line.substr(12));
                i64 x;
                i64 y;
                u64 pixelWidth;
                u64 pixelHeight;
                u64 screenWidth;
                u64 screenHeight;
                unsigned iconified;
                unsigned maximized;
                unsigned fullscreen;
                if (!(args >> x >> y >> pixelWidth >> pixelHeight >> screenWidth >> screenHeight >> iconified >> maximized >> fullscreen) || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || pixelWidth > UINT16_MAX || pixelHeight > UINT16_MAX || screenWidth > UINT32_MAX || screenHeight > UINT32_MAX || iconified > 1 || maximized > 1 || fullscreen > 1) {
                    throw std::runtime_error("invalid window info");
                }
                display.currentWindow.x = x;
                display.currentWindow.y = y;
                display.currentWindow.screenPixelWidth = screenWidth;
                display.currentWindow.screenPixelHeight = screenHeight;
                display.currentWindow.iconified = iconified;
                display.currentWindow.maximized = maximized;
                display.currentWindow.fullscreen = fullscreen;
                display.applyWindowSize(pixelWidth, pixelHeight);
                writeAll(controlFd, "OK\n");
            } else if (line == "WINSIZE") {
                winsize size{};
                if (ioctl(io[0], TIOCGWINSZ, &size) < 0) {
                    throw std::runtime_error("test TIOCGWINSZ failed");
                }
                writeAll(controlFd, "OK " + std::to_string(size.ws_col) + " " + std::to_string(size.ws_row) + "\n");
            } else if (line == "FONT_STATE") {
                StringBuilder output;
                output << StringView(u8"OK ") << composer.fontSize << StringView(u8" ") << composer.glyphWidth << StringView(u8" ") << composer.glyphHeight << StringView(u8" ") << composer.pixelWidth << StringView(u8" ") << composer.pixelHeight << StringView(u8" ") << composer.columns << StringView(u8" ") << composer.rows << StringView(u8" ") << (unsigned)(composer.contentScale * 1000.0f + 0.5f) << StringView(u8" ") << opts.border << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "LAST_UPDATE") {
                StringBuilder output;
                output << StringView(u8"OK ") << display.lastUpdateCells << StringView(u8" ") << display.lastUpdateSpans << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line.compare(0, 15, "FRONTEND_SCALE ") == 0) {
                unsigned xNumerator = 0;
                unsigned xDenominator = 0;
                unsigned yNumerator = 0;
                unsigned yDenominator = 0;
                char trailing = 0;
                if (sscanf(line.c_str() + 15, "%u %u %u %u %c", &xNumerator, &xDenominator, &yNumerator, &yDenominator, &trailing) != 4 || xNumerator == 0 || xDenominator == 0 || yNumerator == 0 || yDenominator == 0 || xNumerator > 10000 || xDenominator > 10000 || yNumerator > 10000 || yDenominator > 10000) {
                    Errno(EINVAL).raise(StringView(u8"invalid frontend scale"));
                }
                input.testContentScale((float)(xNumerator) / xDenominator, (float)(yNumerator) / yDenominator);
                terminal.update();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 4, "KEY ") == 0) {
                std::istringstream args(line.substr(4));
                std::string name;
                unsigned modifiers;
                if (!(args >> name >> modifiers) || modifiers > 7) {
                    throw std::runtime_error("invalid key");
                }
                terminal.writePty(parseKey(name), (VtModifier)(modifiers), true);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 5, "CHAR ") == 0) {
                std::istringstream args(line.substr(5));
                unsigned character;
                unsigned modifiers;
                if (!(args >> character >> modifiers) || character > 255 || modifiers > 7) {
                    throw std::runtime_error("invalid char");
                }
                terminal.writePty((u8)(character), (VtModifier)(modifiers), true);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 18, "CONTROL_CHARACTER ") == 0) {
                std::istringstream args(line.substr(18));
                int key;
                unsigned shifted;
                u8 character = 0;
                if (!(args >> key >> shifted) || shifted > 1 || !controlCharacter(key, shifted, character)) {
                    throw std::runtime_error("invalid control character");
                }
                writeAll(controlFd, "OK " + std::to_string(character) + "\n");
            } else if (line.compare(0, 17, "FRONTEND_CONTROL ") == 0) {
                std::istringstream args(line.substr(17));
                int key;
                unsigned shifted;
                unsigned alt;
                u8 character = 0;
                if (!(args >> key >> shifted >> alt) || shifted > 1 || alt > 1 || !controlCharacter(key, shifted, character)) {
                    throw std::runtime_error("invalid frontend control");
                }
                VtModifier modifiers = VtModifier::control;
                if (shifted) {
                    modifiers = modifiers | VtModifier::shift;
                }
                if (alt) {
                    modifiers = modifiers | VtModifier::alt;
                }
                terminal.writePty(character, modifiers, true);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 19, "FRONTEND_KEY_EVENT ") == 0) {
                std::istringstream args(line.substr(19));
                int key;
                int scancode;
                int action;
                int modifiers;
                if (!(args >> key >> scancode >> action >> modifiers) || action < 0 || action > 2 || modifiers < 0) {
                    throw std::runtime_error("invalid frontend key event");
                }
                input.testKeyEvent(key, scancode, action, modifiers);
                terminal.update();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 20, "FRONTEND_TEXT_EVENT ") == 0) {
                std::istringstream args(line.substr(20));
                unsigned codepoint;
                int modifiers;
                if (!(args >> codepoint >> modifiers) || codepoint > 0x10ffff || modifiers < 0) {
                    throw std::runtime_error("invalid frontend text event");
                }
                input.testTextInput(codepoint, modifiers);
                terminal.update();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 10, "KITTY_KEY ") == 0) {
                std::istringstream args(line.substr(10));
                u32 key;
                u32 shifted;
                u32 base;
                unsigned modifiers;
                unsigned event;
                if (!(args >> key >> shifted >> base >> modifiers >> event) || event < 1 || event > 3) {
                    throw std::runtime_error("invalid kitty key");
                }
                terminal.writeKittyKey(key, shifted, base, modifiers, (VtermKeyEventType)(event));
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 14, "KITTY_SPECIAL ") == 0) {
                std::istringstream args(line.substr(14));
                std::string name;
                unsigned modifiers;
                unsigned event;
                if (!(args >> name >> modifiers >> event) || event < 1 || event > 3) {
                    throw std::runtime_error("invalid kitty special key");
                }
                terminal.writeKittyKey(parseKey(name), modifiers, (VtermKeyEventType)(event));
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 6, "PASTE ") == 0) {
                terminal.pasteSelection(decodeHex(line.substr(6)));
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 6, "FOCUS ") == 0) {
                terminal.setHasFocus(line.substr(6) == "1");
                writeAll(controlFd, "OK\n");
            } else if (line == "POINTER_PRESENCE 0" || line == "POINTER_PRESENCE 1") {
                composer.input->pointerPresence(line.back() == '1');
                terminal.update();
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 18, "HIGHLIGHT_RELEASE ") == 0) {
                std::istringstream args(line.substr(18));
                unsigned endX, endY, mouseX, mouseY;
                if (!(args >> endX >> endY >> mouseX >> mouseY)) {
                    throw std::runtime_error("invalid highlight release");
                }
                terminal.mouseHighlightRelease(endX, endY, mouseX, mouseY);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 17, "LOCATOR_POSITION ") == 0) {
                std::istringstream args(line.substr(17));
                unsigned column, row, pixelX, pixelY, buttons;
                if (!(args >> column >> row >> pixelX >> pixelY >> buttons)) {
                    throw std::runtime_error("invalid locator position");
                }
                terminal.setLocatorPosition(column, row, pixelX, pixelY, buttons);
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 15, "LOCATOR_BUTTON ") == 0) {
                std::istringstream args(line.substr(15));
                unsigned button, pressed;
                if (!(args >> button >> pressed)) {
                    throw std::runtime_error("invalid locator button");
                }
                terminal.reportLocatorButton(button, pressed != 0);
                writeAll(controlFd, "OK\n");
            } else if (line == "SYNC_TIMEOUT") {
                terminal.expireSynchronizedOutput(true);
                writeAll(controlFd, "OK\n");
            } else if (line == "BLINK_TICK") {
                if (terminal.advanceAnimation(true)) {
                    terminal.redraw();
                }
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 13, "SELECT_START ") == 0 || line.compare(0, 14, "SELECT_UPDATE ") == 0) {
                const bool start = line.compare(0, 13, "SELECT_START ") == 0;
                std::istringstream args(line.substr(start ? 13 : 14));
                int column;
                int row;
                if (!(args >> column >> row)) {
                    throw std::runtime_error("invalid selection point");
                }
                if (start) {
                    terminal.selectStart(opts.border + column * composer.glyphWidth, opts.border + row * composer.glyphHeight, false);
                } else {
                    terminal.selectUpdate(opts.border + column * composer.glyphWidth, opts.border + row * composer.glyphHeight);
                }
                writeAll(controlFd, "OK\n");
            } else if (line == "SELECT_RECTANGULAR") {
                terminal.selectRectangularModeToggle();
                writeAll(controlFd, "OK\n");
            } else if (line == "SELECT_FINISH") {
                std::string selection;
                terminal.selectFinish(selection);
                writeAll(controlFd, "OK " + encodeHex(selection) + "\n");
            } else if (line.compare(0, 10, "HYPERLINK ") == 0) {
                std::istringstream args(line.substr(10));
                int column;
                int row;
                if (!(args >> column >> row)) {
                    throw std::runtime_error("invalid hyperlink point");
                }
                writeAll(controlFd, "OK " + encodeHex(terminal.getHyperlink(opts.border + column, opts.border + row)) + "\n");
            } else if (line == "HYPERLINK_COUNT") {
                writeAll(controlFd, "OK " + std::to_string(terminal.getHyperlinkCount()) + "\n");
            } else if (line == "DESKTOP_STATE") {
                StringBuilder output;
                output << StringView(u8"OK ") << (unsigned)(desktopActions.icon) << StringView(u8" ") << desktopActions.openCount << StringView(u8" ") << display.hoveredHyperlink << StringView(u8" ") << display.hoveredLinkBegin << StringView(u8" ") << display.hoveredLinkEnd << StringView(u8" ");
                if (desktopActions.openedUri.empty()) {
                    output << StringView(u8"-");
                } else {
                    appendHex(output, StringView(desktopActions.openedUri));
                }
                output << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "READ_ACTIONS") {
                writeAll(controlFd, "OK " + encodeHex(actions) + "\n");
                actions.clear();
            } else if (line == "READ_PRINTER") {
                writeAll(controlFd, "OK " + encodeHex(printerOutput) + "\n");
                printerOutput.clear();
            } else if (line == "STATE") {
                const auto& mouse = terminal.getMouseTrackingState();
                writeAll(controlFd, "OK " + std::to_string((unsigned)(mouse.mode)) + " " + std::to_string((unsigned)(mouse.enc)) + " " + std::to_string(mouse.focusEventMode) + " " + std::to_string(terminal.getKittyKeyboardFlags()) + "\n");
            } else if (line == "PROTOCOL_STATE") {
                writeAll(controlFd, "OK " + std::to_string(terminal.getScreenReverseVideo()) + " " + std::to_string(terminal.getLedState()) + " " + std::to_string(terminal.getReverseWrapMode()) + " " + std::to_string(terminal.getNationalReplacementMode()) + " 0\n");
            } else if (line == "CURSOR_STATE") {
                writeAll(controlFd, "OK " + std::to_string(terminal.getPrivateMode(25)) + " " + std::to_string(terminal.getPrivateMode(12)) + " " + std::to_string((unsigned)(terminal.getCursorStyle())) + "\n");
            } else if (line == "CONFORMANCE_STATE") {
                StringBuilder output;
                output << StringView(u8"OK screen=") << (terminal.getPrivateMode(47) ? StringView(u8"Alternate") : StringView(u8"Primary")) << StringView(u8" IRM=") << (unsigned)(terminal.getAnsiMode(4)) << StringView(u8" SRM=") << (unsigned)(terminal.getAnsiMode(12)) << StringView(u8" LNM=") << (unsigned)(terminal.getAnsiMode(20)) << StringView(u8" DECCKM=") << (unsigned)(terminal.getPrivateMode(1)) << StringView(u8" DECCOLM=") << (unsigned)(terminal.getPrivateMode(3)) << StringView(u8" DECSCLM=") << (unsigned)(terminal.getPrivateMode(4)) << StringView(u8" DECSCNM=") << (unsigned)(terminal.getPrivateMode(5)) << StringView(u8" DECOM=") << (unsigned)(terminal.getPrivateMode(6)) << StringView(u8" DECAWM=") << (unsigned)(terminal.getPrivateMode(7)) << StringView(u8" DECARM=") << (unsigned)(terminal.getPrivateMode(8)) << StringView(u8" DECTCEM=") << (unsigned)(terminal.getPrivateMode(25)) << StringView(u8" DECNKM=") << (unsigned)(terminal.getPrivateMode(66)) << StringView(u8" DECBKM=") << (unsigned)(terminal.getPrivateMode(67)) << StringView(u8" DECLRMM=") << (unsigned)(terminal.getPrivateMode(69)) << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "RECTANGLE_ORIGIN") {
                const RectangleOrigin origin = terminal.getRectangleOrigin();
                writeAll(controlFd, "OK " + std::to_string(origin.rowBase) + " " + std::to_string(origin.columnBase) + " " + std::to_string(origin.rowLimit) + " " + std::to_string(origin.columnLimit) + "\n");
            } else if (line == "PEN_STATE") {
                const TerminalPen pen = terminal.getPenState();
                StringBuilder output;
                output << StringView(u8"OK ") << cellFlags(pen.cell) << StringView(u8" ") << (unsigned)(pen.fg.red) << StringView(u8" ") << (unsigned)(pen.fg.green) << StringView(u8" ") << (unsigned)(pen.fg.blue) << StringView(u8" ") << (unsigned)(pen.bg.red) << StringView(u8" ") << (unsigned)(pen.bg.green) << StringView(u8" ") << (unsigned)(pen.bg.blue) << StringView(u8" ") << pen.cell.foreground().legacyIndex() << StringView(u8" ") << pen.cell.background().legacyIndex() << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "PARSER_TRACE_ON") {
                vtermTrace.clear();
                writeAll(controlFd, "OK\n");
            } else if (line == "PARSER_TRACE_CLEAR") {
                vtermTrace.clear();
                writeAll(controlFd, "OK\n");
            } else if (line == "READ_PARSER_TRACE") {
                writeAll(controlFd, "OK " + encodeHex(vtermTrace.drain()) + "\n");
            } else if (line.compare(0, 10, "UTF8_PUSH ") == 0) {
                const auto codepoints = testUtf8Decoder.push(decodeHex(line.substr(10)));
                StringBuilder output;
                output << StringView(u8"OK");
                for (const u32 codepoint : codepoints) {
                    output << StringView(u8" ") << Hex{codepoint};
                }
                output << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "RENDER_STATE") {
                writeAll(controlFd, display.renderState());
            } else if (line.compare(0, 13, "MOUSE_ENCODE ") == 0) {
                std::istringstream args(line.substr(13));
                unsigned encoding;
                unsigned type;
                unsigned modifiers;
                int motionButton;
                int button;
                int column;
                int row;
                if (!(args >> encoding >> type >> modifiers >> motionButton >> button >> column >> row) || encoding > 4 || type > 2) {
                    throw std::runtime_error("invalid mouse event");
                }
                writeAll(controlFd, "OK " + encodeHex(encodeMouseProtocol((MouseTrackingEnc)(encoding), (MouseEventType)(type), modifiers, motionButton, button, column, row)) + "\n");
            } else if (line.compare(0, 12, "SET_PRIMARY ") == 0) {
                const size_t separator = line.find(' ', 12);
                if (separator == std::string::npos) {
                    throw std::runtime_error("invalid primary selection");
                }
                const int autoCopy = std::stoi(line.substr(12, separator - 12));
                if (autoCopy < 0 || autoCopy > 1) {
                    throw std::runtime_error("invalid auto-copy state");
                }
                const std::string content = decodeHex(line.substr(separator + 1));
                const StringView selection((const u8*)(content.data()), content.size());
                composer.clipboard->writePrimary(selection);
                if (autoCopy) {
                    composer.clipboard->writeClipboard(selection);
                }
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 11, "SET_SYSTEM ") == 0) {
                const std::string content = decodeHex(line.substr(11));
                composer.clipboard->writeClipboard(StringView((const u8*)(content.data()), content.size()));
                writeAll(controlFd, "OK\n");
            } else if (line.compare(0, 14, "GET_SELECTION ") == 0) {
                const int primary = std::stoi(line.substr(14));
                if (primary < 0 || primary > 1) {
                    throw std::runtime_error("invalid selection kind");
                }
                const StringView content = primary ? composer.clipboard->readPrimary() : composer.clipboard->readClipboard();
                writeAll(controlFd, "OK " + encodeHex(std::string((const char*)(content.data()), content.length())) + "\n");
            } else if (line == "GET_CWD") {
                StringBuilder output;
                output << StringView(u8"OK ");
                appendHex(output, StringView(display.currentCwd));
                output << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line.compare(0, 9, "OSC7_CWD ") == 0) {
                display.currentCwd.reset();
                const std::string input = "\x1b]7;" + decodeHex(line.substr(9)) + "\x1b\\";
                terminal.feedPtyOutput((const u8*)(input.data()), input.size());
                StringBuilder output;
                output << StringView(u8"OK ");
                appendHex(output, StringView(display.currentCwd));
                output << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "SNAPSHOT") {
                writeAll(controlFd, display.snapshot());
            } else if (line == "MODEL_SNAPSHOT") {
                writeAll(controlFd, display.modelSnapshot());
            } else if (line == "MODEL_DIGEST") {
                writeAll(controlFd, display.modelDigest());
            } else if (line == "SCROLLBACK_STATE") {
                writeAll(controlFd, display.scrollbackState());
            } else if (line == "SCREEN_TEXT") {
                writeAll(controlFd, "OK " + encodeHex(display.screenText()) + "\n");
            } else if (line == "READ_INPUT") {
                writeAll(controlFd, "OK " + encodeHex(drainInput(io[1])) + "\n");
            } else if (line == "PENDING_OUTPUT") {
                writeAll(controlFd, "OK " + std::to_string(terminal.pendingPtyOutputBytes()) + "\n");
            } else if (line == "DROPPED_PTY_RESPONSES") {
                StringBuilder output;
                output << StringView(u8"OK ") << terminal.droppedPtyResponses() << StringView(u8"\n");
                writeAll(controlFd, StringView(output));
            } else if (line == "FLUSH_OUTPUT") {
                terminal.flushPtyOutput();
                writeAll(controlFd, "OK\n");
            } else if (line == "FLUSH_OUTPUT_RESULT") {
                writeAll(controlFd, "OK " + std::to_string(terminal.flushPtyOutput()) + "\n");
            } else if (line == "READ_WRITTEN_PTY") {
                writeAll(controlFd, "OK " + encodeHex(writtenPtyData) + "\n");
                writtenPtyData.clear();
            } else if (line == "PENDING_SCRIPTED_PTY_READ_BYTES") {
                size_t count = 0;
                for (const auto& item : scriptedPtyReads) {
                    count += item.data.size();
                }
                writeAll(controlFd, "OK " + std::to_string(count) + "\n");
            } else if (line.compare(0, 12, "SERVICE_PTY ") == 0) {
                std::istringstream args(line.substr(12));
                int readable;
                int writable;
                if (!(args >> readable >> writable) || readable < 0 || readable > 1 || writable < 0 || writable > 1) {
                    throw std::runtime_error("invalid PTY service event");
                }
                writeAll(controlFd, "OK " + std::to_string(terminal.servicePty(readable, writable)) + "\n");
            } else if (line == "QUIT") {
                if (childPid > 0) {
                    kill(childPid, SIGKILL);
                    waitpid(childPid, nullptr, 0);
                    childPid = -1;
                }
                writeAll(controlFd, "OK\n");
                break;
            } else {
                writeAll(controlFd, "ERR unknown command\n");
            }
        } catch (Exception& error) {
            const StringView message = error.description();
            writeAll(controlFd, "ERR " + std::string((const char*)(message.data()), message.length()) + "\n");
        } catch (const std::exception& error) {
            writeAll(controlFd, std::string("ERR ") + error.what() + "\n");
        }
    }

    close(io[0]);
    close(io[1]);
    composer.vterm = nullptr;
    composer.pty = nullptr;
    composer.clipboard = nullptr;
    composer.desktopActions = nullptr;
    return 0;
}
