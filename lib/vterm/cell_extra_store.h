/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/grapheme.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/terminal_types.h>

#include <std/lib/list.h>
#include <std/str/view.h>
#include <std/lib/vector.h>

namespace stl {
    class ObjPool;
}

struct CellExtraView {
    CellColor underlineColor;
    GraphemeView grapheme;
    u32 hyperlinkDisplayId = 0;
    const u8* sixelPixels = nullptr;
    const u8* sixelPalette = nullptr;
};

struct CellExtraStore;

// The slot a window's shared extras store lives in: the model reads the
// current store through it, and a collection replaces the store and
// walks the listeners - the caches keyed on extra refs are void.
struct VtCellExtras {
    void replace(CellExtraStore* next);

    CellExtraStore* store = nullptr;
    stl::IntrusiveList changedListeners;
};

// Everything in the window that holds a ref into the store, and the one
// place a collection can learn about it.
//
// R7: the store is one per window while collect() used to migrate only
// what the terminal that noticed the budget handed it. Every other
// terminal - the next pane, a background tab - kept refs into the pool
// collect() then deleted, so its cells read a stranger's grapheme or a
// freed sixel. So the collection enumerates its clients instead of
// trusting whoever called it.
//
// Both calls do nothing by default: a client that only caches by ref (a
// screen's shaping cache) owns none, and a client that owns refs may
// have no cache to drop.
//
// Unlinking is the destructor's, and now literally Listener's own. That
// is not tidiness: a registration outliving its owner would make the
// next collection walk freed memory - the same defect this interface
// exists to fix, arriving from the other side.
struct CellExtraClient: Listener {
    // Hand over every cell this client still owns and every non-cell ref
    // it holds. The refs are rewritten in place; the cells' are too.
    virtual void collectExtras(stl::Vector<TerminalCell*>& cells, stl::Vector<u32*>& roots);
    // The store was rebuilt: anything keyed by a ref of the old one is
    // now void.
    virtual void extrasCollected();

    // VtCellExtras::replace() walks changedListeners as Listeners, so
    // the store's clients are Listeners: a client held in that list
    // under any other base is a static_cast into a stranger's vtable
    // that compiles, links, and calls the wrong slot. The list is
    // walked a second time - by collect(), through collectExtras() - and
    // that walk still needs every entry to be a client, which is why the
    // registration is this type and not a bare Listener.
    void onListen(void* argument) override;
};

struct CellExtraStore {
    virtual CellExtraView view(const TerminalCell& cell) const noexcept = 0;
    virtual CellColor underlineColor(const TerminalCell& cell) const noexcept = 0;
    virtual GraphemeView grapheme(const TerminalCell& cell) const noexcept = 0;
    virtual GraphemeView grapheme(u32 ref) const noexcept = 0;
    virtual stl::StringView hyperlink(const TerminalCell& cell) const noexcept = 0;
    virtual u32 hyperlinkDisplayId(const TerminalCell& cell) const noexcept = 0;

    virtual u32 getOrCreateHyperlink(stl::StringView identity, stl::StringView payload, u32 displayId) = 0;
    virtual u32 findHyperlink(stl::StringView identity) const noexcept = 0;
    virtual size_t hyperlinkCount() const noexcept = 0;

    virtual void setUnderlineColor(TerminalCell& cell, CellColor color) = 0;
    virtual void setGrapheme(TerminalCell& cell, const u32* codepoints, size_t count) = 0;
    virtual void clearGrapheme(TerminalCell& cell) = 0;
    // Copies SixelPatch::paletteBytes into the store; the result stays
    // valid until the next collect(), so intern once per image and pass
    // the pointer to every setSixel() of that image within one input
    // pass.
    virtual const u8* internSixelPalette(const u8* palette) = 0;
    virtual void setSixel(TerminalCell& cell, const u8* pixels, const u8* palette) = 0;
    virtual void setHyperlink(TerminalCell& cell, u32 hyperlinkRef) = 0;
    virtual void clearHyperlink(TerminalCell& cell) = 0;
    virtual void clearExtra(TerminalCell& cell, CellColor underlineColor) = 0;

    virtual void setCellCount(size_t cellCount) noexcept = 0;
    virtual size_t slotBudget() const noexcept = 0;
    virtual bool shouldCollect() const noexcept = 0;
    virtual bool hardLimitExceeded() const noexcept = 0;
    // Rebuilds the store around everything still live in the window:
    // every registered CellExtraClient is asked for its cells and refs,
    // and the cells and roots arguments are the caller's own
    // contribution on top, for a caller that is not itself a client. Refs are rewritten in place,
    // and the old pool is released only once every client has been
    // asked.
    virtual void collect(stl::Vector<TerminalCell*>& cells, u32* const* roots, size_t rootCount) = 0;

    static CellExtraStore* create(VtCellExtras& extras, stl::ObjPool& pool, size_t cellCount);
};
