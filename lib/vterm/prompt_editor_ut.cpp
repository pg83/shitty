/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "prompt_editor.h"

#include "unicode_width.h"

#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

using namespace stl;

namespace {
    // Codepoints of a UTF-8 literal, the way a line arrives from zsh.
    void codepoints(StringView text, Vector<u32>& out) {
        out.clear();
        PromptLine line;
        StringBuilder payload;
        payload << StringView(u8"c=0;") << text;
        STD_INSIST(decodePromptReport(StringView(payload), line));
        for (const u32 codepoint : line.text) {
            out.pushBack(codepoint);
        }
    }

    const UnicodeWidths& widths() {
        static const UnicodeWidths full(0);
        return full;
    }
}

STD_TEST_SUITE(PromptEditor) {
    // The report: the cursor and the line, escapes undone byte for byte -
    // a multibyte character too - and anything that is not one refused.
    STD_TEST(AReportIsReadWithItsEscapes) {
        PromptLine line;
        STD_INSIST(decodePromptReport(StringView(u8"c=3;a\\x5cb\\x0ac\\xd0\\xb6"), line));
        STD_INSIST(line.cursor == 3);
        STD_INSIST(line.text.length() == 6);
        STD_INSIST(line.text[0] == 'a' && line.text[1] == '\\' && line.text[2] == 'b' && line.text[3] == '\n' && line.text[4] == 'c' && line.text[5] == 0x436);

        // A cursor past the end is the end.
        STD_INSIST(decodePromptReport(StringView(u8"c=99;ab"), line));
        STD_INSIST(line.cursor == 2);
        // The empty line is a line.
        STD_INSIST(decodePromptReport(StringView(u8"c=0;"), line));
        STD_INSIST(line.text.length() == 0 && line.cursor == 0);

        PromptLine kept;
        STD_INSIST(decodePromptReport(StringView(u8"c=1;xy"), kept));
        STD_INSIST(!decodePromptReport(StringView(u8"x=1;ab"), kept));
        STD_INSIST(!decodePromptReport(StringView(u8"c=;ab"), kept));
        STD_INSIST(!decodePromptReport(StringView(u8"c=1ab"), kept));
        STD_INSIST(!decodePromptReport(StringView(u8"c=1;a\\x4"), kept));
        STD_INSIST(!decodePromptReport(StringView(u8"c=1;a\\y41"), kept));
        // Refused, and nothing taken from it.
        STD_INSIST(kept.cursor == 1 && kept.text.length() == 2 && kept.text[0] == 'x');
    }

    // What goes back is what came: escape a line holding every character the
    // escaping is for and read it back as a report.
    STD_TEST(TheEscapingRoundTrips) {
        const u32 text[] = {'e', '\\', '\n', 0x1b, 0x07, 0x7f, ':', ';', 0x436, 0x1f600, 'z'};
        const size_t count = sizeof(text) / sizeof(text[0]);
        StringBuilder escaped;
        promptEscape(text, count, escaped);
        // Nothing an OSC or the widget's read would stop at is left bare.
        for (const u8 byte : StringView(escaped)) {
            STD_INSIST(byte >= 0x20 && byte != 0x7f);
        }
        StringBuilder report;
        report << StringView(u8"c=4;") << StringView(escaped);
        PromptLine line;
        STD_INSIST(decodePromptReport(StringView(report), line));
        STD_INSIST(line.text.length() == count && line.cursor == 4);
        for (size_t at = 0; at < count; ++at) {
            STD_INSIST(line.text[at] == text[at]);
        }
    }

    // The widget's sequence, exactly: its key, the cursor, the line, BEL.
    STD_TEST(SettingTheLineIsOneSequence) {
        const u32 text[] = {'l', 's', ' ', '\\'};
        StringBuilder out;
        promptSetSequence(text, 4, 2, out);
        STD_INSIST(StringView(out) == StringView(u8"\x1b[7701~2:ls \\x5c\x07"));
        out.reset();
        promptSetSequence(text, 4, 40, out);
        STD_INSIST(StringView(out) == StringView(u8"\x1b[7701~4:ls \\x5c\x07"));
    }

    // Where each character was drawn: from the origin, wrapped at the grid's
    // width, a wide character that does not fit moved whole to the next row,
    // and after a newline at column 0.
    STD_TEST(TheLineIsLaidOutAsZleDrawsIt) {
        Vector<u32> text;
        codepoints(StringView(u8"ab\xe7\x8c\xab" "cd\ne"), text);
        const PromptOrigin origin{100, 6, 10};
        // Premise: the wide character is really two cells, and at column 8
        // it fits; at 9 it would not.
        STD_INSIST(widths().codepointWidth(0x732b) == 2);
        Vector<PromptCell> cells;
        promptLayout(origin, text.data(), text.length(), widths(), cells);
        STD_INSIST(cells.length() == 7);
        STD_INSIST(cells[0].row == 100 && cells[0].column == 6);
        STD_INSIST(cells[1].row == 100 && cells[1].column == 7);
        STD_INSIST(cells[2].row == 100 && cells[2].column == 8 && cells[2].width == 2);
        STD_INSIST(cells[3].row == 101 && cells[3].column == 0);
        STD_INSIST(cells[4].row == 101 && cells[4].column == 1);
        STD_INSIST(cells[5].row == 101 && cells[5].column == 2);
        STD_INSIST(cells[6].row == 102 && cells[6].column == 0);

        // One column further in, the wide character no longer fits and
        // goes whole to the next row.
        promptLayout(PromptOrigin{100, 7, 10}, text.data(), text.length(), widths(), cells);
        STD_INSIST(cells[1].row == 100 && cells[1].column == 8);
        STD_INSIST(cells[2].row == 101 && cells[2].column == 0);
    }

    // A cell is the character drawn on it; past a row's text it is the end of
    // that row's text; outside the rows of the line it is nothing.
    STD_TEST(ACellIsAPositionInTheLine) {
        Vector<u32> text;
        codepoints(StringView(u8"ls\xe7\x8c\xab\nx"), text);
        const PromptOrigin origin{50, 4, 20};
        size_t index = 99;
        // Premise: the fixture's positions are distinct, so a mapping that
        // collapses them cannot pass.
        STD_INSIST(text.length() == 5);

        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 50, 4, index) && index == 0);
        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 50, 5, index) && index == 1);
        // Both cells of the wide character are it.
        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 50, 6, index) && index == 2);
        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 50, 7, index) && index == 2);
        // Past the first line's text: its end, the newline.
        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 50, 15, index) && index == 3);
        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 51, 0, index) && index == 4);
        // Past the last line's text - an autosuggestion, a right prompt: the end.
        STD_INSIST(promptIndexAt(origin, text.data(), text.length(), widths(), 51, 9, index) && index == 5);

        // The prompt itself, the rows above and below: not the line.
        STD_INSIST(!promptIndexAt(origin, text.data(), text.length(), widths(), 50, 3, index));
        STD_INSIST(!promptIndexAt(origin, text.data(), text.length(), widths(), 49, 10, index));
        STD_INSIST(!promptIndexAt(origin, text.data(), text.length(), widths(), 52, 0, index));

        // The empty line is its origin's row and nothing else.
        STD_INSIST(promptIndexAt(origin, nullptr, 0, widths(), 50, 10, index) && index == 0);
        STD_INSIST(!promptIndexAt(origin, nullptr, 0, widths(), 51, 0, index));
    }

    // A selection's cells, first to last and in either order, are the
    // characters they cover.
    STD_TEST(ASelectionIsARangeOfTheLine) {
        Vector<u32> text;
        codepoints(StringView(u8"echo hello world"), text);
        const PromptOrigin origin{7, 2, 80};
        size_t from = 0;
        size_t to = 0;
        // "hello": columns 7..11.
        STD_INSIST(promptRangeOf(origin, text.data(), text.length(), widths(), 7, 7, 7, 11, from, to));
        STD_INSIST(from == 5 && to == 10);
        STD_INSIST(promptRangeOf(origin, text.data(), text.length(), widths(), 7, 11, 7, 7, from, to));
        STD_INSIST(from == 5 && to == 10);
        // Out to past the end: up to the end.
        STD_INSIST(promptRangeOf(origin, text.data(), text.length(), widths(), 7, 13, 7, 60, from, to));
        STD_INSIST(from == 11 && to == 16);
        // Over the prompt: not an edit of the line.
        STD_INSIST(!promptRangeOf(origin, text.data(), text.length(), widths(), 7, 0, 7, 11, from, to));
    }

    // An edit is the whole new line and the cursor after it.
    STD_TEST(AnEditSetsTheWholeLine) {
        PromptLine line;
        STD_INSIST(decodePromptReport(StringView(u8"c=16;echo hello world"), line));
        StringBuilder out;
        // Deleted: "hello ".
        promptReplace(line, 5, 11, nullptr, 0, out);
        STD_INSIST(StringView(out) == StringView(u8"\x1b[7701~5:echo world\x07"));
        // Replaced by typing.
        out.reset();
        const u32 typed[] = {'h', 'i'};
        promptReplace(line, 5, 10, typed, 2, out);
        STD_INSIST(StringView(out) == StringView(u8"\x1b[7701~7:echo hi world\x07"));
        // Everything, as cmd+A then Backspace.
        out.reset();
        promptReplace(line, 0, line.text.length(), nullptr, 0, out);
        STD_INSIST(StringView(out) == StringView(u8"\x1b[7701~0:\x07"));
        // Out of range is clamped, never past the line.
        out.reset();
        promptReplace(line, 14, 90, nullptr, 0, out);
        STD_INSIST(StringView(out) == StringView(u8"\x1b[7701~14:echo hello wor\x07"));
    }
}
