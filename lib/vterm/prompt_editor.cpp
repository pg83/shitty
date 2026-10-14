/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "prompt_editor.h"

#include "unicode_width.h"
#include "utf8.h"

#include <std/str/builder.h>

using namespace stl;

namespace {
    void writeDecimal(size_t value, StringBuilder& out) {
        char digits[24];
        size_t count = 0;
        do {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0) {
            const u8 digit = (u8)(digits[--count]);
            out << StringView(&digit, 1);
        }
    }

    int hexValue(u8 c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    // The width ZLE gives a character: a control is drawn as `^X`, two
    // cells; everything else as the terminal measures it.
    u16 promptWidth(u32 codepoint, const UnicodeWidths& widths) {
        if (codepoint < 0x20 || codepoint == 0x7f) {
            return 2;
        }
        const int width = widths.codepointWidth(codepoint);
        return width < 0 ? 0 : (u16)(width);
    }
}

bool decodePromptReport(StringView payload, PromptLine& out) {
    const u8* const data = (const u8*)(payload.data());
    const size_t length = payload.length();
    if (length < 3 || data[0] != 'c' || data[1] != '=') {
        return false;
    }
    size_t at = 2;
    size_t cursor = 0;
    bool digits = false;
    while (at < length && data[at] >= '0' && data[at] <= '9') {
        cursor = cursor * 10 + (size_t)(data[at] - '0');
        digits = true;
        ++at;
    }
    if (!digits || at >= length || data[at] != ';') {
        return false;
    }
    ++at;
    // Unescaped into bytes first, then decoded: an escape may stand for a
    // byte of a multibyte character as well as for a control.
    Vector<u8> bytes;
    while (at < length) {
        const u8 c = data[at];
        if (c == '\\') {
            if (at + 3 >= length || data[at + 1] != 'x') {
                return false;
            }
            const int high = hexValue(data[at + 2]);
            const int low = hexValue(data[at + 3]);
            if (high < 0 || low < 0) {
                return false;
            }
            bytes.pushBack((u8)(high * 16 + low));
            at += 4;
            continue;
        }
        bytes.pushBack(c);
        ++at;
    }
    PromptLine line;
    size_t offset = 0;
    while (offset < bytes.length()) {
        u32 codepoint = 0;
        const size_t used = Utf8Decoder::decodeOne(bytes.data() + offset, bytes.length() - offset, codepoint);
        if (used == 0) {
            line.text.pushBack(Unicode_Replacement_Character);
            ++offset;
            continue;
        }
        line.text.pushBack(codepoint);
        offset += used;
    }
    line.cursor = cursor > line.text.length() ? line.text.length() : cursor;
    out.text.clear();
    for (const u32 codepoint : line.text) {
        out.text.pushBack(codepoint);
    }
    out.cursor = line.cursor;
    return true;
}

void promptEscape(const u32* text, size_t count, StringBuilder& out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t at = 0; at < count; ++at) {
        const u32 codepoint = text[at];
        if (codepoint < 0x20 || codepoint == 0x7f || codepoint == '\\') {
            const u8 escaped[4] = {'\\', 'x', (u8)(hex[codepoint >> 4]), (u8)(hex[codepoint & 15])};
            out << StringView(escaped, 4);
            continue;
        }
        u8 encoded[4];
        size_t used = 0;
        Utf8Encoder::pushUnicode(codepoint, [&](u32 byte) {
            encoded[used++] = (u8)(byte);
        });
        out << StringView(encoded, used);
    }
}

void promptSetSequence(const u32* text, size_t count, size_t cursor, StringBuilder& out) {
    out << StringView(u8"\x1b[7701~");
    writeDecimal(cursor > count ? count : cursor, out);
    out << StringView(u8":");
    promptEscape(text, count, out);
    out << StringView(u8"\x07");
}

StringView promptUndoSequence() {
    return StringView(u8"\x1b[7703~");
}

StringView promptRedoSequence() {
    return StringView(u8"\x1b[7702~");
}

void promptLayout(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, Vector<PromptCell>& out) {
    out.clear();
    i64 row = origin.row;
    u32 column = origin.column;
    const u32 columns = origin.columns == 0 ? 1 : origin.columns;
    for (size_t at = 0; at < count; ++at) {
        const u32 codepoint = text[at];
        if (codepoint == '\n') {
            // The newline stands at the end of its row, where a click past
            // the text lands, and the next line starts at column 0.
            out.pushBack(PromptCell{row, (u16)(column < columns ? column : columns - 1), 0});
            ++row;
            column = 0;
            continue;
        }
        const u16 width = promptWidth(codepoint, widths);
        if (width == 0) {
            const u32 before = column == 0 ? 0 : column - 1;
            out.pushBack(PromptCell{row, (u16)(before), 0});
            continue;
        }
        if (column + width > columns) {
            ++row;
            column = 0;
        }
        out.pushBack(PromptCell{row, (u16)(column), width});
        column += width;
    }
}

bool promptIndexAt(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, i64 row, u16 column, size_t& index) {
    if (row < origin.row || (row == origin.row && column < origin.column)) {
        return false;
    }
    Vector<PromptCell> cells;
    promptLayout(origin, text, count, widths, cells);
    const i64 lastRow = count == 0 ? origin.row : cells[count - 1].row + (text[count - 1] == '\n' ? 1 : 0);
    if (row > lastRow) {
        return false;
    }
    // The character drawn over the cell, or the first one drawn after it on
    // the same row - a wide character that wrapped leaves a cell at the end
    // of the row before it that belongs to nothing.
    for (size_t at = 0; at < count; ++at) {
        const PromptCell& cell = cells[at];
        if (cell.row < row || (cell.width == 0 && text[at] != '\n')) {
            continue;
        }
        if (cell.row > row) {
            index = at;
            return true;
        }
        if (text[at] == '\n' || column < cell.column + cell.width) {
            index = at;
            return true;
        }
    }
    index = count;
    return true;
}

bool promptRangeOf(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, i64 firstRow, u16 firstColumn, i64 lastRow, u16 lastColumn, size_t& from, size_t& to) {
    size_t first = 0;
    size_t last = 0;
    if (!promptIndexAt(origin, text, count, widths, firstRow, firstColumn, first) || !promptIndexAt(origin, text, count, widths, lastRow, lastColumn, last)) {
        return false;
    }
    if (last < first) {
        const size_t swap = first;
        first = last;
        last = swap;
    }
    from = first;
    // The last cell is inside the selection: the character on it goes too.
    to = last < count ? last + 1 : count;
    return true;
}

void promptCursorCell(const PromptOrigin& origin, const u32* text, size_t count, const UnicodeWidths& widths, size_t index, i64& row, u16& column) {
    Vector<PromptCell> cells;
    promptLayout(origin, text, count, widths, cells);
    if (index < count) {
        row = cells[index].row;
        column = cells[index].column;
        return;
    }
    if (count == 0) {
        row = origin.row;
        column = origin.column;
        return;
    }
    const PromptCell& last = cells[count - 1];
    if (text[count - 1] == '\n') {
        row = last.row + 1;
        column = 0;
        return;
    }
    row = last.row;
    column = (u16)(last.column + last.width);
}

bool promptOriginFromCursor(u16 inputColumn, u16 columns, const PromptLine& line, const UnicodeWidths& widths, i64 cursorRow, u16 cursorColumn, PromptOrigin& out) {
    const PromptOrigin probe{0, inputColumn, columns};
    i64 row = 0;
    u16 column = 0;
    promptCursorCell(probe, line.text.data(), line.text.length(), widths, line.cursor, row, column);
    // At the very end of a full row the cursor waits in the last column for
    // the next character to wrap it.
    if (column >= columns && columns != 0) {
        column = (u16)(columns - 1);
    }
    if (column != cursorColumn) {
        return false;
    }
    out = PromptOrigin{cursorRow - row, inputColumn, columns};
    return true;
}

void promptReplace(const PromptLine& line, size_t from, size_t to, const u32* insert, size_t insertCount, StringBuilder& out) {
    const size_t count = line.text.length();
    if (from > count) {
        from = count;
    }
    if (to > count) {
        to = count;
    }
    if (to < from) {
        to = from;
    }
    Vector<u32> next;
    for (size_t at = 0; at < from; ++at) {
        next.pushBack(line.text[at]);
    }
    for (size_t at = 0; at < insertCount; ++at) {
        next.pushBack(insert[at]);
    }
    for (size_t at = to; at < count; ++at) {
        next.pushBack(line.text[at]);
    }
    promptSetSequence(next.data(), next.length(), from + insertCount, out);
}
