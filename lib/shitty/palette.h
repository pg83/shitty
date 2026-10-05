/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/str/view.h>

#include <stddef.h>

namespace stl {
    class ObjPool;
}

// The command palette (Cmd+K; Ctrl+Shift+K in pt on Linux): one list of
// what can be opened - bookmarks, ssh and Teleport hosts, folders, the
// config's apps and environments, a few actions - searched as one, or
// narrowed by a prefix: @ hosts, / folders, > actions, ! apps, $ env.
// Everything here is plain data and functions of it, the same on the Mac
// (ui_palette.mm) and in the window pt draws on Wayland
// (ui_wayland_chrome.cpp); what an item does is palettePlan()'s answer,
// carried out by the caller.

enum class PaletteKind : u8 {
    Bookmark,
    SshHost,
    TeleportHost,
    Folder,
    App,
    Env,
    Action
};

// What a prefix narrows the list to.
enum class PaletteMode : u8 {
    All,
    Actions,
    Hosts,
    Folders,
    Apps,
    Env
};

enum class PaletteAction : u8 {
    None,
    NewTab,
    // Puts the palette in / mode: a folder to open is asked for next.
    OpenFolder,
    // Puts the palette in "> clone " mode: the URL is typed next.
    Clone,
    // A typed `> clone URL`: clones it.
    CloneUrl
};

struct PaletteItem {
    PaletteKind kind = PaletteKind::Action;
    PaletteAction action = PaletteAction::None;
    stl::StringView title;
    stl::StringView subtitle;
    // Where it comes from or what it is now: "ssh config", "Teleport",
    // "current"; empty for none.
    stl::StringView badge;
    // Stable across runs, for the recents: "ssh:prod", "dir:/home/x".
    stl::StringView key;
    // A bookmark: drawn with a star and ranked above the rest.
    bool star = false;
    u64 bookmark = 0;
    // What it opens: run by the shell (empty: the shell itself), where, and
    // with which variables ("K=V" lines).
    stl::StringView command;
    stl::StringView directory;
    stl::StringView variables;
    // The detail pane's rows.
    stl::StringView detailLabels[4];
    stl::StringView detailValues[4];
};

// A line of the list as drawn: a section's heading, or an item with the
// byte offsets of its title's matched characters (the first
// paletteMarksMax of them; a query longer than that is rare).
inline constexpr size_t paletteMarksMax = 32;
struct PaletteRow {
    bool heading = false;
    stl::StringView title;
    size_t item = 0;
    u8 markCount = 0;
    u16 marks[paletteMarksMax] = {};
};

// The prefix the text starts with, and the text after it (blanks after
// the prefix skipped).
PaletteMode paletteMode(stl::StringView text, stl::StringView& rest);
// The prefix that selects a mode, "" for All.
stl::StringView palettePrefix(PaletteMode mode);
stl::StringView paletteModeName(PaletteMode mode);

// The rows for `text`. With nothing typed: the recents (by `recents`, the
// item keys most recent first), then the actions. Typed: the matches,
// sectioned Bookmarks, SSH Hosts, Folders, Apps, Environments, Actions,
// each best first, bookmarks and recents ahead of equals; in All mode a
// section shows at most `perSection`. Replaces what `out` held.
void paletteQuery(const stl::Vector<PaletteItem>& items, stl::StringView text, const stl::Vector<stl::StringView>& recents, stl::Vector<PaletteRow>& out, size_t perSection = 5);

// What an empty list says instead: where that kind of row comes from, or
// that nothing matched. Lines and example lines may be empty.
struct PaletteHint {
    stl::StringBuilder title;
    stl::StringBuilder lines[2];
    stl::StringView example[3];
};

// Fills `out` for a list with no rows in `mode`, `query` being the text
// after the prefix; `configPath` is named with the home as ~. False when
// there is nothing to say (All with nothing typed).
bool paletteEmptyHint(PaletteMode mode, stl::StringView query, stl::StringView configPath, stl::StringView home, PaletteHint& out);

// The palette's box, the same on both platforms, in points.
namespace PaletteMetrics {
    inline constexpr float width = 720;
    inline constexpr float input = 48;
    inline constexpr float chips = 32;
    inline constexpr float list = 340;
    inline constexpr float footer = 32;
    inline constexpr float detail = 250;
    inline constexpr float pad = 6;
    inline constexpr float heading = 24;
    inline constexpr float row = 36;
    inline constexpr float radius = 12;
    // Below the top of the terminal panel.
    inline constexpr float top = 56;
}
// A row's top in the list, from the list's top, before scrolling.
float paletteRowTop(const stl::Vector<PaletteRow>& rows, size_t index);
// The row at `y` from the list's top, scrolled by `scroll`, or -1.
long long paletteRowAt(const stl::Vector<PaletteRow>& rows, float y, float scroll);
// The scroll that shows row `index` whole, kept as near `scroll` as it can.
float paletteScrollTo(const stl::Vector<PaletteRow>& rows, size_t index, float scroll, float height);

// The index of the first row that is an item, or rows.length().
size_t paletteFirstItem(const stl::Vector<PaletteRow>& rows);
// The next (or previous, step -1) item row from `row`, round the ends.
size_t paletteStep(const stl::Vector<PaletteRow>& rows, size_t row, int step);

// The palette's own items: the actions. Appended.
void paletteActions(stl::ObjPool& pool, stl::Vector<PaletteItem>& out);
// A typed `> clone URL` as an item: git clone into `cloneDirectory`/<repo>.
// False when the text is not one.
bool paletteCloneItem(stl::StringView text, stl::StringView cloneDirectory, stl::ObjPool& pool, PaletteItem& out);
// In / mode, the directories a typed path completes to: "~/Pro" lists
// ~/Projects, ~/Programs. Each is a Folder item. Appended.
void paletteDirectoryItems(stl::StringView typed, stl::StringView home, stl::ObjPool& pool, stl::Vector<PaletteItem>& out);
// What a typed path completes to on Tab: the longest common start of the
// matching directories, with a / after a single one. Empty for none.
void paletteComplete(stl::StringView typed, stl::StringView home, stl::StringBuilder& out);

// Where an item is opened.
enum class PaletteTarget : u8 {
    NewTab,
    // Typed into the shell at the active tab's prompt.
    CurrentTab
};

// What opening an item takes. A new tab runs `command` with the
// shell in `directory`; the current tab gets `typed` as a line.
struct PalettePlan {
    stl::StringBuilder command;
    stl::StringBuilder directory;
    stl::StringBuilder typed;
    stl::StringBuilder title;
};
// `teleportLogin` is put before a Teleport host when set.
void palettePlan(const PaletteItem& item, PaletteTarget target, stl::StringView teleportLogin, PalettePlan& out);
// `text` quoted for a POSIX shell, appended.
void paletteQuote(stl::StringView text, stl::StringBuilder& out);

// The recents file: one item key a line, most recent first, at most 8.
void paletteLoadRecents(stl::StringView path, stl::ObjPool& pool, stl::Vector<stl::StringView>& out);
void paletteRemember(stl::StringView path, stl::StringView key, stl::Vector<stl::StringView>& recents, stl::ObjPool& pool);
