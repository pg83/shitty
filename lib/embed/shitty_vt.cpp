/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "shitty_vt.h"

#include <lib/vterm/pty.h>
#include <lib/vterm/vterm.h>
#include <lib/vterm/screen.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/grapheme.h>
#include <lib/vterm/vt_config.h>
#include <lib/vterm/vt_geometry.h>
#include <lib/vterm/terminal_types.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/str/view.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/ptr/scoped.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <new>
#include <string.h>
#include <plt/fiber.h>
#include <plt/input.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/clipboard.h>
#include <plt/platform_headless.h>

using namespace stl;

namespace {
    struct EmbedClipboard;
    struct EmbedHost;
    struct ReplyPty;
}

// The embedder object behind the opaque C handle: every piece
// Vterm::create wants, owned here, plus the capture buffers the C
// calls drain. Everything lives in one pool, the handle included; the
// terminal is made last, so it dies first when free() drops the pool.
struct shitty_vt {
    stl::ObjPool* pool = nullptr;
    plt::Platform* platform = nullptr;
    VtGeometry geometry;
    VtConfig config;
    VtConfigSlot slot;
    VtCellExtras extras;
    stl::SmallObjAllocator* smallObjects = nullptr;
    EmbedHost* host = nullptr;
    ReplyPty* pty = nullptr;
    Vterm* terminal = nullptr;
    // The embedder's struct, never copied. Null while the terminal is
    // being constructed - nothing the constructor publishes is the
    // application's doing, so no callback fires before shitty_vt_new
    // returns - and null for good when the embedder passed none.
    const shitty_vt_callbacks* callbacks = nullptr;
};

namespace {
    // A caller-owned Input over a private copy of the selection: the
    // read may park its fiber, and the selection can move underneath a
    // parked reader. The caller releases with plain delete, which hands
    // the object back to the shared allocator.
    struct SelectionInput final: public Input {
        SelectionInput(stl::SmallObjAllocator* allocator, const void* data, size_t len);

        void operator delete(SelectionInput* input, std::destroying_delete_t) noexcept;

        size_t readImpl(void* data, size_t len) override;

        stl::SmallObjAllocator* allocator;
        Buffer bytes_;
        size_t offset_ = 0;
    };

    struct ClipboardOutput final: public Output {
        ClipboardOutput(stl::SmallObjAllocator* allocator, EmbedClipboard& owner);

        void operator delete(ClipboardOutput* output, std::destroying_delete_t) noexcept;

        size_t writeImpl(const void* data, size_t len) override;
        void finishImpl() override;

        stl::SmallObjAllocator* allocator;
        EmbedClipboard& owner;
        Buffer staged_;
    };

    // One selection: accumulates writes, publishes on finish, and
    // reports the publication to the embedder.
    struct EmbedClipboard final: public plt::Clipboard {
        EmbedClipboard(shitty_vt& vt, int which);

        Input* read() override;
        Output* write() override;

        void publish(const void* data, size_t len);

        shitty_vt& vt;
        int which;
        Buffer content_;
    };

    struct EmbedHost final: public VtHost {
        explicit EmbedHost(shitty_vt& vt);

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

        shitty_vt& vt;
        EmbedClipboard primary_;
        EmbedClipboard secondary_;
    };

    // The pty face: one reusable chunk toward the terminal, and every
    // send lands in the reply buffer take_replies drains. No child; the
    // read side never delivers.
    struct ReplyPty final: public PtyHandle {
        explicit ReplyPty(plt::Scheduler& scheduler);

        void resize(const PtySize& size) override;
        void engage() override;
        Chunk* allocate(size_t len) override;
        void send(Chunk* chunk, size_t len) override;
        Chunk* acquire() override;
        void release(Chunk* chunks) override;
        pid_t foregroundProcessGroup() override;

        size_t take(uint8_t* out, size_t cap);

        struct ReplyChunk final: public Chunk {
            void* data() override;
            size_t length() override;
            Chunk* next() override;

            Buffer payload_;
            size_t used_ = 0;
            bool loaned_ = false;
        };

        plt::Scheduler& scheduler;
        Buffer replies_;
        size_t drained_ = 0;
        ReplyChunk chunk_;
    };
}

SelectionInput::SelectionInput(SmallObjAllocator* allocator_, const void* data, size_t len)
    : allocator(allocator_)
    , bytes_(data, len)
{
}

void SelectionInput::operator delete(SelectionInput* input, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = input->allocator;
    owner->release(input);
}

size_t SelectionInput::readImpl(void* data, size_t len) {
    const size_t left = bytes_.used() - offset_;
    const size_t count = len < left ? len : left;
    memcpy(data, (const u8*)(bytes_.data()) + offset_, count);
    offset_ += count;
    return count;
}

ClipboardOutput::ClipboardOutput(SmallObjAllocator* allocator_, EmbedClipboard& owner_)
    : allocator(allocator_)
    , owner(owner_)
{
}

void ClipboardOutput::operator delete(ClipboardOutput* output, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = output->allocator;
    owner->release(output);
}

size_t ClipboardOutput::writeImpl(const void* data, size_t len) {
    staged_.append(data, len);
    return len;
}

void ClipboardOutput::finishImpl() {
    owner.publish(staged_.data(), staged_.used());
}

EmbedClipboard::EmbedClipboard(shitty_vt& vt_, int which_)
    : vt(vt_)
    , which(which_)
{
}

Input* EmbedClipboard::read() {
    return vt.smallObjects->make<SelectionInput>(vt.smallObjects, content_.data(), content_.used());
}

Output* EmbedClipboard::write() {
    return vt.smallObjects->make<ClipboardOutput>(vt.smallObjects, *this);
}

void EmbedClipboard::publish(const void* data, size_t len) {
    content_.reset();
    content_.append(data, len);
    if (vt.callbacks != nullptr && vt.callbacks->clipboard_set != nullptr) {
        vt.callbacks->clipboard_set(vt.callbacks->user, which, (const uint8_t*)(data), len);
    }
}

EmbedHost::EmbedHost(shitty_vt& vt_)
    : vt(vt_)
    , primary_(vt_, 0)
    , secondary_(vt_, 1)
{
}

plt::Clipboard* EmbedHost::primary() {
    return &primary_;
}

plt::Clipboard* EmbedHost::secondary() {
    return &secondary_;
}

plt::WindowInfo EmbedHost::info() {
    plt::WindowInfo info;
    info.width = vt.geometry.pixelWidth;
    info.height = vt.geometry.pixelHeight;
    info.screenPixelWidth = vt.geometry.pixelWidth;
    info.screenPixelHeight = vt.geometry.pixelHeight;
    info.focused = true;
    return info;
}

void EmbedHost::requestFrame() {
    if (vt.callbacks != nullptr && vt.callbacks->damaged != nullptr) {
        vt.callbacks->damaged(vt.callbacks->user);
    }
}

void EmbedHost::requestResize(u32 width, u32 height) {
    const u16 columns = width < UINT16_MAX ? width : UINT16_MAX;
    const u16 rows = height < UINT16_MAX ? height : UINT16_MAX;
    if (vt.callbacks != nullptr && vt.callbacks->resize_request != nullptr) {
        vt.callbacks->resize_request(vt.callbacks->user, columns, rows);
    }
    vt.geometry.resizeCells(columns, rows, this);
}

void EmbedHost::requestResizeCells(u32 columns, u32 rows) {
    requestResize(columns, rows);
}

VtGridSize EmbedHost::gridSize(u32 width, u32 height) {
    return {width, height};
}

void EmbedHost::requestMaximized(bool) {
}

void EmbedHost::requestFullscreen(bool) {
}

void EmbedHost::requestIconify() {
}

void EmbedHost::requestRestore() {
}

void EmbedHost::requestMove(i32, i32) {
}

void EmbedHost::requestFocus() {
}

void EmbedHost::requestAttention() {
    if (vt.callbacks != nullptr && vt.callbacks->bell != nullptr) {
        vt.callbacks->bell(vt.callbacks->user);
    }
}

void EmbedHost::requestPointerIcon(plt::PointerIcon) {
}

void EmbedHost::requestOpenUri(StringView uri) {
    if (vt.callbacks != nullptr && vt.callbacks->open_uri != nullptr) {
        vt.callbacks->open_uri(vt.callbacks->user, (const uint8_t*)(uri.data()), uri.length());
    }
}

bool EmbedHost::uriSchemeAllowed(StringView scheme) {
    // The parsed-option default of the full terminal.
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

void EmbedHost::titleChanged(const VtermTitleChanged& event) {
    if (vt.callbacks != nullptr && vt.callbacks->title_changed != nullptr) {
        vt.callbacks->title_changed(vt.callbacks->user, (const uint8_t*)(event.title.data()), event.title.length());
    }
}

void EmbedHost::resized() {
    if (vt.terminal != nullptr) {
        vt.terminal->windowResized();
    }
}

void* ReplyPty::ReplyChunk::data() {
    return payload_.mutData();
}

size_t ReplyPty::ReplyChunk::length() {
    return used_;
}

PtyHandle::Chunk* ReplyPty::ReplyChunk::next() {
    return nullptr;
}

ReplyPty::ReplyPty(plt::Scheduler& scheduler_)
    : scheduler(scheduler_)
{
}

void ReplyPty::resize(const PtySize&) {
}

void ReplyPty::engage() {
}

PtyHandle::Chunk* ReplyPty::allocate(size_t len) {
    if (chunk_.loaned_) {
        return nullptr;
    }
    chunk_.payload_.reset();
    chunk_.payload_.grow(len);
    chunk_.payload_.seekAbsolute(len);
    chunk_.used_ = len;
    chunk_.loaned_ = true;
    return &chunk_;
}

void ReplyPty::send(Chunk* chunk, size_t len) {
    if (chunk != &chunk_) {
        return;
    }
    chunk_.loaned_ = false;
    replies_.append(chunk_.payload_.data(), len);
}

PtyHandle::Chunk* ReplyPty::acquire() {
    scheduler.current()->park();
    return nullptr;
}

void ReplyPty::release(Chunk*) {
}

pid_t ReplyPty::foregroundProcessGroup() {
    return 0;
}

size_t ReplyPty::take(uint8_t* out, size_t cap) {
    const size_t left = replies_.used() - drained_;
    const size_t count = cap < left ? cap : left;
    memcpy(out, (const uint8_t*)(replies_.data()) + drained_, count);
    drained_ += count;
    if (drained_ == replies_.used()) {
        replies_.reset();
        drained_ = 0;
    }
    return count;
}

namespace {
    // The VGA palette, the same 16 colors the full terminal defaults to.
    constexpr Color ansiDefaults[AnsiPalette::colorCount] = {
        {0x00, 0x00, 0x00},
        {0xaa, 0x00, 0x00},
        {0x00, 0xaa, 0x00},
        {0xaa, 0x55, 0x00},
        {0x00, 0x00, 0xaa},
        {0xaa, 0x00, 0xaa},
        {0x00, 0xaa, 0xaa},
        {0xaa, 0xaa, 0xaa},
        {0x55, 0x55, 0x55},
        {0xff, 0x55, 0x55},
        {0x55, 0xff, 0x55},
        {0xff, 0xff, 0x55},
        {0x55, 0x55, 0xff},
        {0xff, 0x55, 0xff},
        {0x55, 0xff, 0xff},
        {0xff, 0xff, 0xff},
    };

    static void fillConfig(VtConfig& config, u16 saveLines) {
        config.saveLines = saveLines;
        config.brandName = StringView(u8"shitty-vt");
        config.fg = {0xff, 0xff, 0xff};
        config.bg = {0x00, 0x00, 0x00};
        config.cr = config.fg;
        for (size_t index = 0; index < AnsiPalette::colorCount; ++index) {
            config.palette[index] = ansiDefaults[index];
        }
    }

    // The pending frame, forced out if everything was already consumed:
    // the walk and the cursor both read presentation state off it.
    static const TerminalUpdate* currentUpdate(shitty_vt* vt) {
        const TerminalUpdate* update = vt->terminal->output();
        if (update == nullptr) {
            vt->terminal->expose();
            update = vt->terminal->output();
        }
        return update;
    }

    static u32 packAttributes(const TerminalCell& cell) {
        u32 attributes = 0;
        attributes |= cell.bold ? SHITTY_VT_ATTR_BOLD : 0;
        attributes |= cell.faint ? SHITTY_VT_ATTR_FAINT : 0;
        attributes |= cell.italic ? SHITTY_VT_ATTR_ITALIC : 0;
        attributes |= cell.blink ? SHITTY_VT_ATTR_BLINK : 0;
        attributes |= cell.inverse ? SHITTY_VT_ATTR_INVERSE : 0;
        attributes |= cell.conceal ? SHITTY_VT_ATTR_CONCEAL : 0;
        attributes |= cell.strike ? SHITTY_VT_ATTR_STRIKE : 0;
        attributes |= cell.overline ? SHITTY_VT_ATTR_OVERLINE : 0;
        return attributes;
    }

    // One cell handed to an embedder's callback. The position is passed
    // through rather than derived, so a caller reading the history by
    // index reports that index, and the preview reports where it is
    // drawn. The continuation of a wide cell is not reported.
    // Where a color came from, in the shape the header documents: the
    // kind in the low byte, the palette entry in the high one.
    static u16 packColorSource(CellColor color) {
        switch (color.source()) {
            case CellColor::Source::DefaultForeground:
                return SHITTY_VT_COLOR_DEFAULT_FOREGROUND;
            case CellColor::Source::DefaultBackground:
                return SHITTY_VT_COLOR_DEFAULT_BACKGROUND;
            case CellColor::Source::Indexed:
                return (u16)(SHITTY_VT_COLOR_INDEXED | (color.index() << 8));
            case CellColor::Source::Direct:
                return SHITTY_VT_COLOR_DIRECT;
        }
        return SHITTY_VT_COLOR_DIRECT;
    }

    static void emitCell(shitty_vt* vt, const TerminalColors* colors, const TerminalCell& cell, u16 row, u16 column, shitty_vt_cell_fn fn, void* user) {
        if (cell.dwidth_cont) {
            return;
        }
        CellExtraStore* const extras = vt->extras.store;
        shitty_vt_cell out{};
        u32 single = 0;
        if (cell.hasExtra()) {
            const GraphemeView grapheme = extras->grapheme(cell);
            out.grapheme = grapheme.data();
            out.grapheme_len = grapheme.count;
        } else if (cell.uc_pt != 0) {
            single = cell.uc_pt;
            out.grapheme = &single;
            out.grapheme_len = 1;
        }
        const CellColor foreground = cell.foreground();
        const CellColor background = cell.background();
        // The store is reached through a virtual call that cannot inline, and
        // a cell with no extra already carries its underline color inline.
        // Taking that branch here rather than inside the store is worth about
        // a tenth of what reading a cell costs, on a grid where almost no
        // cell has an extra at all.
        const CellColor underline = cell.hasExtra() ? extras->underlineColor(cell) : cell.inlineUnderlineColor();
        out.foreground_source = packColorSource(foreground);
        out.background_source = packColorSource(background);
        out.underline_source = packColorSource(underline);
        if (colors != nullptr) {
            out.foreground = colors->resolveForeground(cell).packed();
            out.background = colors->resolveBackground(cell).packed();
            out.underline_color = colors->resolve(underline).packed();
            // A configured special color stood in for the request. The
            // embedder is handed a color, not a palette entry it could
            // resolve for itself.
            //
            // Guarded, because detecting that substitution costs two more
            // resolutions on every cell and nothing can substitute while no
            // special color is enabled - which is every terminal that was
            // never asked to use one.
            if (colors->specialModes != 0) {
                if (colors->resolve(foreground).packed() != out.foreground) {
                    out.foreground_source = SHITTY_VT_COLOR_DIRECT;
                }
                if (colors->resolve(background).packed() != out.background) {
                    out.background_source = SHITTY_VT_COLOR_DIRECT;
                }
            }
        }
        out.attributes = (u16)(packAttributes(cell));
        out.underline_style = (u8)(cell.underline_style);
        out.width = cell.dwidth ? 2 : 1;
        fn(user, row, column, &out);
    }

    // One row of cells, in the row's own columns.
    static void emitRow(shitty_vt* vt, const TerminalColors* colors, const ScreenRowRef& source, u16 row, shitty_vt_cell_fn fn, void* user) {
        if (source.cells == nullptr) {
            return;
        }
        for (u16 column = 0; column < vt->geometry.columns; ++column) {
            emitCell(vt, colors, source.cells[column], row, column, fn, user);
        }
    }

}

shitty_vt* shitty_vt_new(uint16_t columns, uint16_t rows, uint16_t save_lines, const shitty_vt_callbacks* callbacks) {
    if (columns == 0 || rows == 0) {
        return nullptr;
    }
    try {
        ScopedPtr<ObjPool> pool{ObjPool::fromMemoryRaw()};
        shitty_vt* const vt = pool->make<shitty_vt>();
        vt->pool = pool.ptr;
        vt->platform = plt::createHeadlessPlatform(*pool.ptr);
        fillConfig(vt->config, save_lines);
        vt->slot.config = &vt->config;
        vt->geometry.setCellPixelSize(1, 1);
        vt->geometry.resizeCells(columns, rows, nullptr);
        vt->extras.store = CellExtraStore::create(vt->extras, *pool.ptr, 0);
        vt->smallObjects = SmallObjAllocator::create(pool.ptr);
        vt->host = pool->make<EmbedHost>(*vt);
        vt->pty = pool->make<ReplyPty>(*vt->platform->scheduler());
        vt->terminal = Vterm::create(*pool.ptr, vt->geometry, vt->slot, vt->extras, *vt->smallObjects, *vt->platform->scheduler(), *vt->host, *vt->pty, nullptr);
        // A library terminal has nothing to lose focus to; applications
        // that ask for focus events learn of changes when the embedder
        // grows an input surface.
        vt->terminal->focus(true);
        // Armed last: construction publishes a reset title and a frame
        // request, and neither is the application speaking (issue 98).
        vt->callbacks = callbacks;
        pool.drop();
        return vt;
    } catch (...) {
        return nullptr;
    }
}

void shitty_vt_free(shitty_vt* vt) {
    if (vt == nullptr) {
        return;
    }
    delete vt->pool;
}

void shitty_vt_feed(shitty_vt* vt, const uint8_t* bytes, size_t len) {
    if (bytes == nullptr || len == 0) {
        return;
    }
    vt->terminal->feedPty(StringView((const u8*)(bytes), len));
}

void shitty_vt_resize(shitty_vt* vt, uint16_t columns, uint16_t rows) {
    if (columns == 0 || rows == 0) {
        return;
    }
    vt->geometry.resizeCells(columns, rows, vt->host);
}

size_t shitty_vt_take_replies(shitty_vt* vt, uint8_t* out, size_t cap) {
    return vt->pty->take(out, cap);
}

void shitty_vt_each_cell(shitty_vt* vt, shitty_vt_cell_fn fn, void* user) {
    if (fn == nullptr) {
        return;
    }
    const TerminalUpdate* update = currentUpdate(vt);
    if (update == nullptr || update->shapes == nullptr) {
        return;
    }
    Screen* const screen = update->shapes;
    for (u16 row = 0; row < vt->geometry.rows; ++row) {
        emitRow(vt, update->colors, screen->viewRow(row), row, fn, user);
    }
    vt->terminal->consume();
}

uint32_t shitty_vt_scroll(shitty_vt* vt, int32_t rows) {
    return vt->terminal->scrollView(rows);
}

uint32_t shitty_vt_scroll_to(shitty_vt* vt, uint32_t offset) {
    return vt->terminal->scrollViewTo(offset);
}

uint32_t shitty_vt_scroll_offset(const shitty_vt* vt) {
    const TerminalUpdate* update = currentUpdate(const_cast<shitty_vt*>(vt));
    if (update == nullptr || update->shapes == nullptr) {
        return 0;
    }
    return update->shapes->info().viewOffset;
}

uint32_t shitty_vt_history_rows(const shitty_vt* vt) {
    const TerminalUpdate* update = currentUpdate(const_cast<shitty_vt*>(vt));
    if (update == nullptr || update->shapes == nullptr) {
        return 0;
    }
    return update->shapes->info().historyRows;
}

uint32_t shitty_vt_total_rows(const shitty_vt* vt) {
    const TerminalUpdate* update = currentUpdate(const_cast<shitty_vt*>(vt));
    if (update == nullptr || update->shapes == nullptr) {
        return 0;
    }
    const ScreenInfo info = update->shapes->info();
    return info.historyRows + info.rows;
}

void shitty_vt_row_cells(shitty_vt* vt, uint32_t index, shitty_vt_cell_fn fn, void* user) {
    if (fn == nullptr) {
        return;
    }
    const TerminalUpdate* update = currentUpdate(vt);
    if (update == nullptr || update->shapes == nullptr) {
        return;
    }
    Screen* const screen = update->shapes;
    const ScreenInfo info = screen->info();
    if (index >= info.historyRows + info.rows) {
        return;
    }
    // Logical row 0 is the top of the live screen and the history runs
    // negative from there, so an oldest-first index sits historyRows
    // above it. viewRow subtracts the current offset, so add it back and
    // the read is independent of where the user has scrolled.
    const i32 logical = (i32)(index) - (i32)(info.historyRows);
    const i32 view = logical + (i32)(info.viewOffset);
    emitRow(vt, update->colors, screen->viewRow(view), (u16)(index), fn, user);
    vt->terminal->consume();
}

uint16_t shitty_vt_row_wrap_length(const shitty_vt* vt, uint32_t index) {
    const TerminalUpdate* update = currentUpdate(const_cast<shitty_vt*>(vt));
    if (update == nullptr || update->shapes == nullptr) {
        return 0;
    }
    Screen* const screen = update->shapes;
    const ScreenInfo info = screen->info();
    if (index >= info.historyRows + info.rows) {
        return 0;
    }
    // The same index arithmetic shitty_vt_row_cells does, so the two agree
    // on which row an index names wherever the view happens to sit.
    const i32 logical = (i32)(index) - (i32)(info.historyRows);
    const i32 view = logical + (i32)(info.viewOffset);
    const ScreenRowRef source = screen->viewRow(view);
    if (source.cells == nullptr) {
        return 0;
    }
    // The wrap bit marks the cell the row ran out of room at, so the text
    // is everything up to and including it. Taking the first one set is
    // what the model itself does when it measures a row this way.
    for (u16 column = 0; column < info.columns; ++column) {
        if (source.cells[column].wrap) {
            return (u16)(column + 1);
        }
    }
    return 0;
}

void shitty_vt_memory_usage(const shitty_vt* vt, shitty_vt_memory* out) {
    if (out == nullptr) {
        return;
    }
    *out = shitty_vt_memory{};
    const TerminalUpdate* update = currentUpdate(const_cast<shitty_vt*>(vt));
    if (update == nullptr || update->shapes == nullptr) {
        return;
    }
    const ScreenInfo info = update->shapes->info();
    const u32 allocated = update->shapes->materializedRows();
    out->allocated_rows = allocated;
    out->capacity_rows = (u32)(info.rows) + info.saveLines;
    out->columns = info.columns;
    out->cell_size = (u32)(sizeof(TerminalCell));
    out->cell_bytes = (u64)(allocated)*info.columns * sizeof(TerminalCell);
}

void shitty_vt_set_save_lines(shitty_vt* vt, uint16_t save_lines) {
    if (vt->config.saveLines == save_lines) {
        return;
    }
    vt->config.saveLines = save_lines;
    // The terminal re-reads the configuration and rebuilds whatever the
    // change invalidated, which for this setting is the primary screen.
    vt->terminal->configChanged();
}

namespace {
    // The C input codes are pinned ABI; every one must equal the input-layer
    // value it mirrors, or shitty_vt_key would deliver the wrong key.
#define SHITTY_VT_KEY_CODES(check) check(SHITTY_VT_KEY_UNKNOWN, Unknown) check(SHITTY_VT_KEY_PRINTABLE, Printable) check(SHITTY_VT_KEY_SPACE, Space) check(SHITTY_VT_KEY_ESCAPE, Escape) check(SHITTY_VT_KEY_ENTER, Enter) check(SHITTY_VT_KEY_BACKSPACE, Backspace) check(SHITTY_VT_KEY_TAB, Tab) check(SHITTY_VT_KEY_INSERT, Insert) check(SHITTY_VT_KEY_DELETE, Delete) check(SHITTY_VT_KEY_HOME, Home) check(SHITTY_VT_KEY_END, End) check(SHITTY_VT_KEY_UP, Up) check(SHITTY_VT_KEY_DOWN, Down) check(SHITTY_VT_KEY_LEFT, Left) check(SHITTY_VT_KEY_RIGHT, Right) check(SHITTY_VT_KEY_PAGE_UP, PageUp) check(SHITTY_VT_KEY_PAGE_DOWN, PageDown) check(SHITTY_VT_KEY_CLEAR, Clear) check(SHITTY_VT_KEY_F1, F1) check(SHITTY_VT_KEY_F2, F2) check(SHITTY_VT_KEY_F3, F3) check(SHITTY_VT_KEY_F4, F4) check(SHITTY_VT_KEY_F5, F5) check(SHITTY_VT_KEY_F6, F6) check(SHITTY_VT_KEY_F7, F7) check(SHITTY_VT_KEY_F8, F8) check(SHITTY_VT_KEY_F9, F9) check(SHITTY_VT_KEY_F10, F10) check(SHITTY_VT_KEY_F11, F11) check(SHITTY_VT_KEY_F12, F12) check(SHITTY_VT_KEY_F13, F13) check(SHITTY_VT_KEY_F14, F14) check(SHITTY_VT_KEY_F15, F15) check(SHITTY_VT_KEY_F16, F16) check(SHITTY_VT_KEY_F17, F17) check(SHITTY_VT_KEY_F18, F18) check(SHITTY_VT_KEY_F19, F19) check(SHITTY_VT_KEY_F20, F20) check(SHITTY_VT_KEY_F21, F21) check(SHITTY_VT_KEY_F22, F22) check(SHITTY_VT_KEY_F23, F23) check(SHITTY_VT_KEY_F24, F24) check(SHITTY_VT_KEY_F25, F25) check(SHITTY_VT_KEY_F26, F26) check(SHITTY_VT_KEY_F27, F27) check(SHITTY_VT_KEY_F28, F28) check(SHITTY_VT_KEY_F29, F29) check(SHITTY_VT_KEY_F30, F30) check(SHITTY_VT_KEY_F31, F31) check(SHITTY_VT_KEY_F32, F32) check(SHITTY_VT_KEY_F33, F33) check(SHITTY_VT_KEY_F34, F34) check(SHITTY_VT_KEY_F35, F35) check(SHITTY_VT_KEY_KEYPAD_0, Keypad0) check(SHITTY_VT_KEY_KEYPAD_1, Keypad1) check(SHITTY_VT_KEY_KEYPAD_2, Keypad2) check(SHITTY_VT_KEY_KEYPAD_3, Keypad3) check(SHITTY_VT_KEY_KEYPAD_4, Keypad4) check(SHITTY_VT_KEY_KEYPAD_5, Keypad5) check(SHITTY_VT_KEY_KEYPAD_6, Keypad6) check(SHITTY_VT_KEY_KEYPAD_7, Keypad7) check(SHITTY_VT_KEY_KEYPAD_8, Keypad8) check(SHITTY_VT_KEY_KEYPAD_9, Keypad9) check(SHITTY_VT_KEY_KEYPAD_DECIMAL, KeypadDecimal) check(SHITTY_VT_KEY_KEYPAD_DIVIDE, KeypadDivide) check(SHITTY_VT_KEY_KEYPAD_MULTIPLY, KeypadMultiply) check(SHITTY_VT_KEY_KEYPAD_SUBTRACT, KeypadSubtract) check(SHITTY_VT_KEY_KEYPAD_ADD, KeypadAdd) check(SHITTY_VT_KEY_KEYPAD_ENTER, KeypadEnter) check(SHITTY_VT_KEY_KEYPAD_EQUAL, KeypadEqual) check(SHITTY_VT_KEY_KEYPAD_SEPARATOR, KeypadSeparator) check(SHITTY_VT_KEY_KEYPAD_F1, KeypadF1) check(SHITTY_VT_KEY_KEYPAD_F2, KeypadF2) check(SHITTY_VT_KEY_KEYPAD_F3, KeypadF3) check(SHITTY_VT_KEY_KEYPAD_F4, KeypadF4) check(SHITTY_VT_KEY_KEYPAD_INSERT, KeypadInsert) check(SHITTY_VT_KEY_KEYPAD_DELETE, KeypadDelete) check(SHITTY_VT_KEY_KEYPAD_UP, KeypadUp) check(SHITTY_VT_KEY_KEYPAD_DOWN, KeypadDown) check(SHITTY_VT_KEY_KEYPAD_LEFT, KeypadLeft) check(SHITTY_VT_KEY_KEYPAD_RIGHT, KeypadRight) check(SHITTY_VT_KEY_KEYPAD_HOME, KeypadHome) check(SHITTY_VT_KEY_KEYPAD_END, KeypadEnd) check(SHITTY_VT_KEY_KEYPAD_PAGE_UP, KeypadPageUp) check(SHITTY_VT_KEY_KEYPAD_PAGE_DOWN, KeypadPageDown) check(SHITTY_VT_KEY_KEYPAD_BEGIN, KeypadBegin) check(SHITTY_VT_KEY_KEYPAD_SPACE, KeypadSpace) check(SHITTY_VT_KEY_KEYPAD_TAB, KeypadTab) check(SHITTY_VT_KEY_CAPS_LOCK, CapsLock) check(SHITTY_VT_KEY_SCROLL_LOCK, ScrollLock) check(SHITTY_VT_KEY_NUM_LOCK, NumLock) check(SHITTY_VT_KEY_PRINT_SCREEN, PrintScreen) check(SHITTY_VT_KEY_PAUSE, Pause) check(SHITTY_VT_KEY_MENU, Menu) check(SHITTY_VT_KEY_LEFT_SHIFT, LeftShift) check(SHITTY_VT_KEY_LEFT_CONTROL, LeftControl) check(SHITTY_VT_KEY_LEFT_ALT, LeftAlt) check(SHITTY_VT_KEY_LEFT_SUPER, LeftSuper) check(SHITTY_VT_KEY_RIGHT_SHIFT, RightShift) check(SHITTY_VT_KEY_RIGHT_CONTROL, RightControl) check(SHITTY_VT_KEY_RIGHT_ALT, RightAlt) check(SHITTY_VT_KEY_RIGHT_SUPER, RightSuper) check(SHITTY_VT_KEY_MEDIA_PLAY, MediaPlay) check(SHITTY_VT_KEY_MEDIA_PAUSE, MediaPause) check(SHITTY_VT_KEY_MEDIA_PLAY_PAUSE, MediaPlayPause) check(SHITTY_VT_KEY_MEDIA_REVERSE, MediaReverse) check(SHITTY_VT_KEY_MEDIA_STOP, MediaStop) check(SHITTY_VT_KEY_MEDIA_FAST_FORWARD, MediaFastForward) check(SHITTY_VT_KEY_MEDIA_REWIND, MediaRewind) check(SHITTY_VT_KEY_MEDIA_TRACK_NEXT, MediaTrackNext) check(SHITTY_VT_KEY_MEDIA_TRACK_PREVIOUS, MediaTrackPrevious) check(SHITTY_VT_KEY_MEDIA_RECORD, MediaRecord) check(SHITTY_VT_KEY_VOLUME_DOWN, VolumeDown) check(SHITTY_VT_KEY_VOLUME_UP, VolumeUp) check(SHITTY_VT_KEY_VOLUME_MUTE, VolumeMute)

#define SHITTY_VT_CHECK_KEY(value, name) static_assert((value) == (u32)(plt::InputKey::name));
    SHITTY_VT_KEY_CODES(SHITTY_VT_CHECK_KEY)
#undef SHITTY_VT_CHECK_KEY
#undef SHITTY_VT_KEY_CODES
    static_assert(SHITTY_VT_KEY_COUNT == (u32)(plt::InputKey::Count));

    static_assert(SHITTY_VT_MOD_SHIFT == plt::InputShift);
    static_assert(SHITTY_VT_MOD_CONTROL == plt::InputControl);
    static_assert(SHITTY_VT_MOD_ALT == plt::InputAlt);
    static_assert(SHITTY_VT_MOD_SUPER == plt::InputSuper);
    static_assert(SHITTY_VT_MOD_CAPS_LOCK == plt::InputCapsLock);
    static_assert(SHITTY_VT_MOD_NUM_LOCK == plt::InputNumLock);
    static_assert(SHITTY_VT_MOD_ALT_GRAPH == plt::InputAltGraph);

    static_assert(SHITTY_VT_KEY_PRESS == (u32)(plt::InputAction::Press));
    static_assert(SHITTY_VT_KEY_REPEAT == (u32)(plt::InputAction::Repeat));
    static_assert(SHITTY_VT_KEY_RELEASE == (u32)(plt::InputAction::Release));

    static_assert(SHITTY_VT_MOUSE_LEFT == (u32)(plt::PointerButton::Primary));
    static_assert(SHITTY_VT_MOUSE_RIGHT == (u32)(plt::PointerButton::Secondary));
    static_assert(SHITTY_VT_MOUSE_MIDDLE == (u32)(plt::PointerButton::Middle));
    static_assert(SHITTY_VT_MOUSE_AUX1 == (u32)(plt::PointerButton::Auxiliary1));
    static_assert(SHITTY_VT_MOUSE_AUX5 == (u32)(plt::PointerButton::Auxiliary5));

    // The paste payload as a stream for the terminal's own paste path,
    // read to the end within the call.
    struct PasteInput final: public Input {
        PasteInput(const u8* data, size_t len)
            : data_(data)
            , left_(len)
        {
        }

        size_t readImpl(void* out, size_t len) override {
            const size_t count = len < left_ ? len : left_;
            memcpy(out, data_, count);
            data_ += count;
            left_ -= count;
            return count;
        }

        const u8* data_;
        size_t left_;
    };
}

int shitty_vt_key(shitty_vt* vt, const shitty_vt_key_event* event) {
    if (event == nullptr || event->key >= SHITTY_VT_KEY_COUNT || event->action > SHITTY_VT_KEY_RELEASE) {
        return 0;
    }
    plt::KeyInput input;
    input.key = (plt::InputKey)(event->key);
    input.action = (plt::InputAction)(event->action);
    input.modifiers = event->modifiers;
    input.layoutCodepoint = event->layout_codepoint;
    input.baseCodepoint = event->base_codepoint;
    input.shiftedCodepoint = event->shifted_codepoint;
    return vt->terminal->key(input) ? 1 : 0;
}

int shitty_vt_text(shitty_vt* vt, uint32_t codepoint, uint16_t modifiers) {
    plt::TextInput input;
    input.codepoint = codepoint;
    input.modifiers = modifiers;
    return vt->terminal->text(input) ? 1 : 0;
}

void shitty_vt_input_flush(shitty_vt* vt) {
    vt->terminal->flush();
}

int shitty_vt_mouse_button(shitty_vt* vt, int button, int pressed, int32_t column, int32_t row, uint16_t modifiers, double time) {
    if (button < 0 || button > SHITTY_VT_MOUSE_AUX5) {
        return 0;
    }
    VtPointerButton input;
    input.button = (plt::PointerButton)(button);
    input.pressed = pressed != 0;
    input.position = {column, row, column, row};
    input.modifiers = modifiers;
    input.time = time;
    return vt->terminal->pointerButton(input) ? 1 : 0;
}

int shitty_vt_mouse_motion(shitty_vt* vt, int32_t column, int32_t row, uint16_t modifiers) {
    VtPointerMotion input;
    input.position = {column, row, column, row};
    input.modifiers = modifiers;
    return vt->terminal->pointerMotion(input) ? 1 : 0;
}

int shitty_vt_mouse_scroll(shitty_vt* vt, double dx, double dy, int32_t column, int32_t row, uint16_t modifiers) {
    VtScroll input;
    input.x = dx;
    input.y = dy;
    input.position = {column, row, column, row};
    input.modifiers = modifiers;
    return vt->terminal->scroll(input) ? 1 : 0;
}

void shitty_vt_paste(shitty_vt* vt, const uint8_t* bytes, size_t len) {
    if (bytes == nullptr && len != 0) {
        return;
    }
    PasteInput source((const u8*)(bytes), len);
    vt->terminal->dropText(source);
}

void shitty_vt_focus(shitty_vt* vt, int focused) {
    vt->terminal->focus(focused != 0);
}

void shitty_vt_preedit(shitty_vt* vt, const uint8_t* text, size_t len, int32_t cursor_begin, int32_t cursor_end) {
    if (text == nullptr && len != 0) {
        return;
    }
    vt->terminal->preedit(StringView((const u8*)(text), len), cursor_begin, cursor_end);
}

void shitty_vt_preedit_cells(shitty_vt* vt, shitty_vt_cell_fn fn, void* user) {
    if (fn == nullptr) {
        return;
    }
    const TerminalUpdate* update = currentUpdate(vt);
    if (update == nullptr || update->overlayCells == nullptr) {
        return;
    }
    for (u16 index = 0; index < update->overlayCount; ++index) {
        emitCell(vt, update->colors, update->overlayCells[index], update->overlayRow, (u16)(update->overlayColumn + index), fn, user);
    }
}

shitty_vt_cursor shitty_vt_cursor_state(const shitty_vt* vt) {
    shitty_vt_cursor result{};
    const TerminalUpdate* update = currentUpdate(const_cast<shitty_vt*>(vt));
    if (update == nullptr) {
        return result;
    }
    result.column = update->cursor.posX;
    result.row = update->cursor.posY;
    result.style = (u8)(update->cursor.style);
    result.visible = update->cursor.style != TerminalCursor::Style::hidden ? 1 : 0;
    return result;
}

uint32_t shitty_vt_modes(const shitty_vt* vt) {
    const VtermState state = vt->terminal->state();
    u32 modes = 0;
    modes |= state.alternateScreen ? SHITTY_VT_MODE_ALT_SCREEN : 0;
    modes |= state.bracketedPaste ? SHITTY_VT_MODE_BRACKETED_PASTE : 0;
    modes |= state.applicationCursorKeys ? SHITTY_VT_MODE_APP_CURSOR_KEYS : 0;
    modes |= state.applicationKeypad ? SHITTY_VT_MODE_APP_KEYPAD : 0;
    modes |= state.focusEvents ? SHITTY_VT_MODE_FOCUS_EVENTS : 0;
    modes |= state.autoWrap ? SHITTY_VT_MODE_AUTO_WRAP : 0;
    modes |= state.originMode ? SHITTY_VT_MODE_ORIGIN : 0;
    modes |= state.insertMode ? SHITTY_VT_MODE_INSERT : 0;
    modes |= state.showCursor ? SHITTY_VT_MODE_CURSOR_VISIBLE : 0;
    modes |= state.screenReverse ? SHITTY_VT_MODE_SCREEN_REVERSE : 0;
    modes |= state.synchronizedOutput ? SHITTY_VT_MODE_SYNCHRONIZED_OUTPUT : 0;
    modes |= state.mouseTracking != MouseTrackingMode::Disabled ? SHITTY_VT_MODE_MOUSE_CLICK : 0;
    modes |= state.mouseTracking == MouseTrackingMode::VT200_ButtonEvent ? SHITTY_VT_MODE_MOUSE_DRAG : 0;
    modes |= state.mouseTracking == MouseTrackingMode::VT200_AnyEvent ? SHITTY_VT_MODE_MOUSE_MOTION : 0;
    modes |= state.mouseEncoding == MouseTrackingEnc::SGR || state.mouseEncoding == MouseTrackingEnc::SGRPixels ? SHITTY_VT_MODE_MOUSE_SGR : 0;
    modes |= state.alternateScroll ? SHITTY_VT_MODE_ALTERNATE_SCROLL : 0;
    return modes;
}
