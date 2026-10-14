/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "palette.h"
#include "sidebar_field.h"

#include <plt/poller.h>

#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/view.h>

struct Composer;

// What the window that draws the palette hears from it.
struct PaletteHost {
    // The list or the text changed: redraw.
    virtual void paletteChanged() = 0;
};

// One window's command palette, whatever draws it: what is listed, what
// is typed, which row is picked, and what a pick does. The Mac's panel
// (ui_palette.mm) and pt's chrome on Wayland (ui_wayland_chrome.cpp) feed
// it keys and text and draw rows(), detail() and field.
class PaletteSession final: public plt::PollCallback {
public:
    PaletteSession(Composer& composer, PaletteHost& host);
    ~PaletteSession() noexcept;

    // Lists everything afresh, empties the field and picks the first row.
    void open();
    void close();
    bool shown() const {
        return shown_;
    }

    // After the field changed: the rows again.
    void textChanged();
    // Up and Down.
    void move(int step);
    void select(size_t row);
    // Tab: completes a path in / mode, else fills the field with the
    // picked item's title. False when there was nothing to do.
    bool complete();
    // Return and its modified forms: opens the picked item. True when the
    // palette is done and should close; an action that asks for more
    // (Open Folder…, Clone Repository…) leaves it open.
    bool pick(PaletteTarget target);
    // Cmd+B: the picked host or folder as a bookmark. True when one was
    // made.
    bool bookmarkPicked();

    SidebarField field;
    const stl::Vector<PaletteRow>& rows() const {
        return rows_;
    }
    size_t selected() const {
        return selected_;
    }
    const PaletteItem* item(size_t row) const;
    // The mode the field's prefix asks for, for the chips.
    PaletteMode mode();
    // What to show instead of an empty list; false when the list has rows
    // or there is nothing to say.
    bool hint(PaletteHint& out);

private:
    void collect();
    void startTeleport();
    void ready(stl::PollFD event) override;
    void setText(stl::StringView text);
    void remember(const PaletteItem& item);

    Composer& composer;
    PaletteHost& host;
    bool shown_ = false;
    stl::ObjPool* pool = nullptr;
    stl::Vector<PaletteItem> base;
    stl::Vector<PaletteItem> items;
    stl::Vector<PaletteRow> rows_;
    size_t selected_ = 0;
    stl::Vector<stl::StringView> recents;
    stl::Buffer recentsPath;
    stl::Buffer home;
    // The env this window last opened or exported, marked "current".
    stl::Buffer currentEnv;
    // Teleport's nodes, as tsh printed them last, and the run in flight.
    stl::Buffer teleportJson;
    stl::Buffer teleportReading;
    u64 teleportAt = 0;
    int teleportFd = -1;
    plt::PollWaiter teleportWaiter;
};
