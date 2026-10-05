/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/input_handler.h>

#include <std/sys/types.h>

namespace stl {
    class IntrusiveList;
}

struct Composer;

enum class InputActions : u8 {
    Copy,
    Paste,
    PastePrimary,
    PageUp,
    PageDown,
    IncFontSize,
    DecFontSize,
    ResetFontSize,
    NewTab,
    CloseTab,
    // A4: divide the focused pane. Vertical is cmd+d - the divider
    // stands upright and the panes sit side by side - and horizontal is
    // cmd+shift+d, which is how iTerm2, Ghostty and Warp all spell the
    // pair. Bound only while -panes is on, for the same reason cmd+b is
    // bound only while -sidebarTabs is: a chord claimed to do nothing is
    // a chord taken away from the program running inside.
    SplitVertical,
    SplitHorizontal,
    // Moving between the panes of a split: ctrl+shift+h/j/k/l, and
    // cmd+opt+arrows on macOS. Bound only while -panes is on, like the
    // splits. Plain ctrl+h/j/k/l are Backspace, newline, kill-line and
    // clear to the shell, and stay the shell's.
    FocusPaneLeft,
    FocusPaneDown,
    FocusPaneUp,
    FocusPaneRight,
    PrevTab,
    NextTab,
    // Direct tab selection, iTerm style: the ninth chord jumps to the
    // last tab however many there are. Contiguous, so ordinal arithmetic
    // against SelectTab1 is safe.
    SelectTab1,
    SelectTab2,
    SelectTab3,
    SelectTab4,
    SelectTab5,
    SelectTab6,
    SelectTab7,
    SelectTab8,
    SelectTab9,
    Clear,
    // The sidebar tab list's visibility. This one legitimately changes
    // how many columns the grid has - it is a user's deliberate act,
    // the equivalent of resizing the window, and A7 separates it from
    // the hover strip precisely on that point.
    ToggleSidebar,
    // The command palette (palette.h): Cmd+K, Ctrl+Shift+K in pt on Linux.
    CommandPalette,
    // The natural-editing gestures: what the chord means, sent to the
    // shell as the readline bytes the platform's editors agree on.
    WordLeft,
    WordRight,
    LineStart,
    LineEnd,
    KillLine,
    EraseWord,
    // The command-line editor at a zsh prompt (prompt_editor.h): select the
    // command, undo and redo an edit of it. Bound only while -promptEditor
    // is on.
    SelectCommandLine,
    UndoCommandLine,
    RedoCommandLine,
    Count,
};

struct InputBindings: public InputHandler {
    virtual void add(InputActions action, stl::IntrusiveList* listeners) = 0;

    static InputBindings* create(Composer& composer);
};
