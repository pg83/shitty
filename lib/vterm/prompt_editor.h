/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/vector.h>
#include <std/str/view.h>
#include <std/sys/types.h>

#include <stddef.h>

namespace stl {
    class StringBuilder;
}

class UnicodeWidths;

// The command line at a zsh prompt, edited by the terminal as a text field
// while zsh keeps owning it.
//
// zsh stays the one place the line lives: Tab, history, ctrl+R and the
// plugins that draw over it all go on working on its $BUFFER. The shell
// integration (lib/shitty/shell/zsh) reports that buffer and $CURSOR on
// every redraw in a private OSC, and takes a new pair back through a widget
// bound to a private key. What the terminal adds is only what zsh cannot
// see: where on the screen each character of the line was drawn, so a click
// or a selection can be turned into positions in the buffer - read from zsh,
// never guessed from arrow keys.
//
// Portable and free of the screen, so all of it is in reach of a test.

// The OSC number of the report: `OSC 7701 ; c=<cursor> ; <line> ST`.
constexpr u32 promptReportOsc = 7701;

// The line as zsh last reported it: its characters, one codepoint each -
// zsh counts $CURSOR in characters - and the cursor between them.
struct PromptLine {
    stl::Vector<u32> text;
    size_t cursor = 0;
};

// The OSC payload after its number: `c=<cursor>;<line>`, the line escaped
// as promptEscape() writes it. False, out untouched, for anything else. A
// cursor past the end is clamped to it.
bool decodePromptReport(stl::StringView payload, PromptLine& out);

// The line as the integration carries it both ways: UTF-8, with every C0
// control, DEL and the backslash written `\xNN`, so it survives an OSC and
// zsh's `printf %b` turns it back byte for byte.
void promptEscape(const u32* text, size_t count, stl::StringBuilder& out);

// What sets zsh's line: the widget's private key, `<cursor>:<escaped line>`
// and BEL, which the widget reads up to. Appends to `out`.
void promptSetSequence(const u32* text, size_t count, size_t cursor, stl::StringBuilder& out);

// The keys the integration binds to zsh's own undo and redo, in every
// keymap - ctrl+_ is undo only in emacs mode.
stl::StringView promptUndoSequence();
stl::StringView promptRedoSequence();

// Where the line starts on the screen: the cell the cursor was in when zsh
// said the input begins (OSC 133;B), as an absolute line number so that
// scrolling does not move it, and how wide the grid is.
struct PromptOrigin {
    i64 row = 0;
    u16 column = 0;
    u16 columns = 0;
};

// Where a character of the line was drawn. ZLE draws the line from the
// origin, wraps it at the grid's width - a wide character that does not
// fit goes whole to the next row - and starts every line after a newline at
// column 0. A character of width zero sits in the cell before it.
struct PromptCell {
    i64 row = 0;
    u16 column = 0;
    u16 width = 0;
};
void promptLayout(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, stl::Vector<PromptCell>& out);

// The position in the line a cell stands for, in the rows the line was
// drawn on: the character drawn there, or, past the end of a row's text -
// an autosuggestion, the right prompt, nothing - the end of that row's text.
// False for a cell outside those rows or before the origin.
bool promptIndexAt(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, i64 row, u16 column, size_t& index);

// The cells [first, last] of a selection, as the positions [from, to) of the
// characters they cover. False when either end is outside the line.
bool promptRangeOf(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, i64 firstRow, u16 firstColumn, i64 lastRow, u16 lastColumn, size_t& from, size_t& to);

// Where ZLE puts the cursor for a position in the line: on the character
// there, or after the last one.
void promptCursorCell(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, size_t index, i64& row, u16& column);

// The origin, found from where the cursor is now. The column the input
// starts at is the one OSC 133;B was given at and does not move, but the row
// does - the screen scrolls under a long line, ctrl+L redraws it at the top -
// so it is worked back from the cursor, which ZLE leaves on $CURSOR once it
// has drawn. False when the cursor is not where the line says it should be:
// the terminal and zsh disagree about the line, and nothing is edited.
bool promptOriginFromCursor(u16 inputColumn, u16 columns, const PromptLine& line, const UnicodeWidths& widths, i64 cursorRow, u16 cursorColumn, PromptOrigin& out);

// An edit of the line: [from, to) replaced by `insert`, the cursor after
// it. Appends the sequence that sets it in zsh to `out`.
void promptReplace(const PromptLine& line, size_t from, size_t to, const u32* insert, size_t insertCount, stl::StringBuilder& out);
