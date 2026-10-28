/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/grapheme.h>
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
// current store through it. A collection renumbers the refs and moves
// the content, so it walks the listeners - the caches keyed on extra
// refs or pointers are void - even though the store object stays.
struct VtCellExtras {
    void replace(CellExtraStore* next);
    void notifyChanged();

    CellExtraStore* store = nullptr;
    stl::IntrusiveList changedListeners;
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
    virtual void collect(stl::Vector<TerminalCell*>& cells, u32* const* roots, size_t rootCount) = 0;

    static CellExtraStore* create(VtCellExtras& extras, stl::ObjPool& pool, size_t cellCount);
};
