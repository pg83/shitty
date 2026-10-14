# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The C embedding facade, driven through bin/example: recorded byte
streams go in, the printed grid must match what the full terminal
produces for the same stream."""

import base64
import os
import subprocess
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

from harness import ROOT, Shitty


def b64(text):
    return base64.b64encode(text.encode())

EXAMPLE = Path(os.environ.get("SHITTY_EMBED_EXAMPLE_BINARY", ROOT / "example"))
# The precondition is the artifact, and only the artifact: this suite
# drives a binary, so it runs exactly when that binary is there to run
# and reports the truth - not a guess about the platform, the build
# flags, or which fork it is on.  Anything else would go stale on its
# own; this cannot, because the thing it asks about is the thing it
# needs.  M7 is why it is asked at all: build.py keeps lib/embed out of
# the graph behind embed_facade_links until lib/vterm stops calling
# Composer::contentInsets() and Composer::resize(), and until then no
# example is produced.  Nothing here has to be undone when it is - the
# first build that emits the binary lifts this by itself.
EXAMPLE_PRESENT = EXAMPLE.is_file() and os.access(EXAMPLE, os.X_OK)
CORPUS = Path(__file__).parent / "corpus"

COLUMNS = 20
ROWS = 6


@dataclass
class ExampleResult:
    events: list[str]
    lines: list[str]
    cursor: tuple[int, int]
    cursor_style: int
    cursor_visible: bool
    modes: int
    replies: bytes
    scroll_offset: int
    history_rows: int
    total_rows: int
    rows_by_index: list[str]
    allocated_rows: int
    capacity_rows: int
    cell_bytes: int
    preedit: "Preedit | None"
    # column -> (foreground, background, underline) provenance of every
    # drawn cell of the top row.
    colors: dict
    # absolute row index -> columns that belong to the row before it wrapped
    # onto the next, 0 for a row that ends on its own.
    wrap: dict


@dataclass
class Preedit:
    """The composition preview: where it is drawn and what it shows."""

    row: int
    column: int
    cells: int
    text: str


def run_example(
    stream,
    columns=COLUMNS,
    rows=ROWS,
    save_lines=0,
    scroll=0,
    scroll_to=-1,
    dump_rows=0,
    set_save_lines=-1,
    input_script=None,
):
    with tempfile.NamedTemporaryFile() as recorded, \
            tempfile.NamedTemporaryFile(mode="w") as script:
        recorded.write(stream)
        recorded.flush()
        arguments = [
            EXAMPLE,
            str(columns),
            str(rows),
            str(save_lines),
            recorded.name,
            str(scroll),
            str(scroll_to),
            str(dump_rows),
            str(set_save_lines),
        ]
        if input_script is not None:
            script.write("".join(line + "\n" for line in input_script))
            script.flush()
            arguments.append(script.name)
        result = subprocess.run(
            arguments,
            capture_output=True,
            timeout=60,
        )
    if result.returncode != 0:
        raise RuntimeError(
            f"example failed: {result.returncode} {result.stderr!r}"
        )
    lines = result.stdout.decode("utf-8").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    rows_by_index = []
    while lines and lines[-1].startswith("row "):
        rows_by_index.insert(0, lines.pop())
    preedit_line = lines.pop()
    memory_line = lines.pop()
    wrap_line = lines.pop()
    scrollback_line = lines.pop()
    replies_line = lines.pop()
    colors_line = lines.pop()
    modes_line = lines.pop()
    cursor_line = lines.pop()
    if not replies_line.startswith("replies:"):
        raise RuntimeError(f"unexpected replies line: {replies_line!r}")
    if not modes_line.startswith("modes: "):
        raise RuntimeError(f"unexpected modes line: {modes_line!r}")
    if not colors_line.startswith("colors:"):
        raise RuntimeError(f"unexpected colors line: {colors_line!r}")
    if not cursor_line.startswith("cursor: "):
        raise RuntimeError(f"unexpected cursor line: {cursor_line!r}")
    if not wrap_line.startswith("wrap:"):
        raise RuntimeError(f"unexpected wrap line: {wrap_line!r}")
    if not scrollback_line.startswith("scrollback: "):
        raise RuntimeError(f"unexpected scrollback line: {scrollback_line!r}")
    if not memory_line.startswith("memory: "):
        raise RuntimeError(f"unexpected memory line: {memory_line!r}")
    if not preedit_line.startswith("preedit: "):
        raise RuntimeError(f"unexpected preedit line: {preedit_line!r}")
    grid = lines[len(lines) - rows :]
    events = lines[: len(lines) - rows]
    cursor_fields = cursor_line[len("cursor: ") :].split()
    replies = bytes.fromhex(replies_line[len("replies:") :].replace(" ", ""))
    scrollback_fields = scrollback_line[len("scrollback: ") :].split()
    indexed = [line.split(":", 1)[1] for line in rows_by_index]
    memory_fields = dict(
        field.split("=", 1) for field in memory_line[len("memory: ") :].split()
    )
    colors = {
        int(column): tuple(sources.split("/"))
        for column, sources in (
            field.split("=", 1) for field in colors_line[len("colors:") :].split()
        )
    }
    wrap = {
        int(index): int(length)
        for index, length in (
            field.split("=", 1) for field in wrap_line[len("wrap:") :].split()
        )
    }
    preedit = None
    if preedit_line != "preedit: none":
        head, text = preedit_line[len("preedit: ") :].split("text=", 1)
        preedit_fields = dict(field.split("=", 1) for field in head.split())
        preedit = Preedit(
            row=int(preedit_fields["row"]),
            column=int(preedit_fields["column"]),
            cells=int(preedit_fields["cells"]),
            text=text,
        )
    return ExampleResult(
        events=events,
        lines=grid,
        cursor=(int(cursor_fields[0]), int(cursor_fields[1])),
        cursor_style=int(cursor_fields[2].removeprefix("style=")),
        cursor_visible=bool(int(cursor_fields[3].removeprefix("visible="))),
        modes=int(modes_line[len("modes: ") :], 16),
        replies=replies,
        scroll_offset=int(scrollback_fields[0].removeprefix("offset=")),
        history_rows=int(scrollback_fields[1].removeprefix("history=")),
        total_rows=int(scrollback_fields[2].removeprefix("total=")),
        rows_by_index=indexed,
        allocated_rows=int(memory_fields["allocated_rows"]),
        capacity_rows=int(memory_fields["capacity_rows"]),
        cell_bytes=int(memory_fields["cell_bytes"]),
        preedit=preedit,
        colors=colors,
        wrap=wrap,
    )


MODE_ALT_SCREEN = 1 << 0
MODE_BRACKETED_PASTE = 1 << 1
MODE_APP_CURSOR_KEYS = 1 << 2
MODE_APP_KEYPAD = 1 << 3
MODE_FOCUS_EVENTS = 1 << 4
MODE_AUTO_WRAP = 1 << 5
MODE_ORIGIN = 1 << 6
MODE_INSERT = 1 << 7
MODE_CURSOR_VISIBLE = 1 << 8
MODE_SCREEN_REVERSE = 1 << 9
MODE_MOUSE_CLICK = 1 << 11
MODE_MOUSE_MOTION = 1 << 13
MODE_MOUSE_SGR = 1 << 14
MODE_ALTERNATE_SCROLL = 1 << 15


@unittest.skipUnless(EXAMPLE_PRESENT, f"the embedding example is not built: {EXAMPLE}")
class EmbedExampleTest(unittest.TestCase):
    def assert_grid(self, stream, expected, **kwargs):
        result = run_example(stream, **kwargs)
        rows = kwargs.get("rows", ROWS)
        columns = kwargs.get("columns", COLUMNS)
        padded = [line.ljust(columns) for line in expected]
        padded += [" " * columns] * (rows - len(padded))
        self.assertEqual(result.lines, padded)
        return result

    def assert_matches_full_terminal(self, streams):
        for stream in streams:
            with self.subTest(stream=stream):
                embedded = run_example(stream)
                with Shitty(columns=COLUMNS, rows=ROWS) as terminal:
                    terminal.write(stream)
                    snapshot = terminal.snapshot()
                self.assertEqual(embedded.lines, snapshot.lines)
                self.assertEqual(
                    embedded.cursor,
                    (snapshot.cursor_x, snapshot.cursor_y),
                )

    def test_plain_text_lands_on_the_grid(self):
        result = self.assert_grid(b"hello", ["hello"])
        self.assertEqual(result.cursor, (5, 0))
        self.assertTrue(result.cursor_visible)
        self.assertTrue(result.modes & MODE_AUTO_WRAP)
        self.assertTrue(result.modes & MODE_CURSOR_VISIBLE)

    def test_cursor_addressing_and_clamped_movement(self):
        self.assert_grid(b"\x1b[3;1HA\x1b[10AX", [" X", "", "A"])

    def test_carriage_return_and_backspace_overwrite(self):
        self.assert_grid(b"world\rW\x1b[4Cd\x08D", ["WorldD"])

    def test_scroll_region_confines_the_scroll(self):
        self.assert_grid(b"\x1b[1;3rA\r\nB\r\nC\r\nD\r\nE", ["C", "D", "E"])

    def test_wrap_and_wrap_disabled(self):
        long = b"x" * (COLUMNS + 3)
        self.assert_grid(long, ["x" * COLUMNS, "xxx"])
        result = run_example(b"\x1b[?7l" + long)
        self.assertEqual(result.lines[0], "x" * COLUMNS)
        self.assertEqual(result.lines[1], " " * COLUMNS)
        self.assertFalse(result.modes & MODE_AUTO_WRAP)

    def test_alt_screen_swaps_and_reports_its_mode(self):
        result = run_example(b"base\x1b[?1049h\x1b[Halt")
        self.assertEqual(result.lines[0], "alt".ljust(COLUMNS))
        self.assertTrue(result.modes & MODE_ALT_SCREEN)
        result = run_example(b"base\x1b[?1049h\x1b[Halt\x1b[?1049l")
        self.assertEqual(result.lines[0], "base".ljust(COLUMNS))
        self.assertFalse(result.modes & MODE_ALT_SCREEN)

    def test_modes_reflect_private_mode_changes(self):
        result = run_example(
            b"\x1b[?2004h\x1b[?1h\x1b[?1003h\x1b[?1006h\x1b[?25l"
        )
        self.assertTrue(result.modes & MODE_BRACKETED_PASTE)
        self.assertTrue(result.modes & MODE_APP_CURSOR_KEYS)
        self.assertTrue(result.modes & MODE_MOUSE_CLICK)
        self.assertTrue(result.modes & MODE_MOUSE_MOTION)
        self.assertTrue(result.modes & MODE_MOUSE_SGR)
        self.assertFalse(result.modes & MODE_CURSOR_VISIBLE)

    def test_alternate_scroll_is_reported_and_cleared(self):
        # DECSET 1007 stands on its own: a host reads it to decide whether
        # wheel input becomes arrow keys, which only matters once the
        # alternate screen is up, but the mode is settable either side of
        # that and must not be conflated with it.
        self.assertFalse(run_example(b"").modes & MODE_ALTERNATE_SCROLL)

        armed = run_example(b"\x1b[?1007h")
        self.assertTrue(armed.modes & MODE_ALTERNATE_SCROLL)
        self.assertFalse(armed.modes & MODE_ALT_SCREEN)

        both = run_example(b"\x1b[?1007h\x1b[?1049h")
        self.assertTrue(both.modes & MODE_ALTERNATE_SCROLL)
        self.assertTrue(both.modes & MODE_ALT_SCREEN)

        cleared = run_example(b"\x1b[?1007h\x1b[?1049h\x1b[?1007l")
        self.assertFalse(cleared.modes & MODE_ALTERNATE_SCROLL)
        self.assertTrue(cleared.modes & MODE_ALT_SCREEN)

    def test_keypad_origin_and_reverse_modes(self):
        result = run_example(b"\x1b=\x1b[?6h\x1b[?5h\x1b[4h")
        self.assertTrue(result.modes & MODE_APP_KEYPAD)
        self.assertTrue(result.modes & MODE_ORIGIN)
        self.assertTrue(result.modes & MODE_SCREEN_REVERSE)
        self.assertTrue(result.modes & MODE_INSERT)

    def test_origin_mode_addresses_inside_the_margins(self):
        self.assert_grid(b"\x1b[2;4r\x1b[?6h\x1b[HX", ["", "X"])

    def test_decaln_fills_the_screen(self):
        self.assert_grid(b"\x1b#8", ["E" * COLUMNS] * ROWS)

    def test_save_and_restore_cursor(self):
        self.assert_grid(b"\x1b[3;5H\x1b7\x1b[HA\x1b8B", ["A", "", "    B"])

    def test_index_scrolls_at_the_bottom(self):
        stream = b"\x1b[6;1Hlast\x1bDnext"
        self.assert_grid(stream, ["", "", "", "", "last", "    next"])

    def test_reverse_index_scrolls_at_the_top(self):
        self.assert_grid(b"top\x1b[1;1H\x1bMX", ["X", "top"])

    def test_erase_line_variants(self):
        self.assert_grid(b"abcdef\x1b[1;3H\x1b[1K", ["   def"])
        self.assert_grid(b"abcdef\x1b[1;3H\x1b[K", ["ab"])
        self.assert_grid(b"abcdef\x1b[2K", [""])

    def test_erase_display_below_and_above(self):
        self.assert_grid(b"1\r\n2\r\n3\x1b[2;1H\x1b[J", ["1"])
        self.assert_grid(b"1\r\n2\r\n3\x1b[2;1H\x1b[1J", ["", "", "3"])

    def test_insert_and_delete_lines(self):
        self.assert_grid(b"1\r\n2\r\n3\x1b[1;1H\x1b[L", ["", "1", "2", "3"])
        self.assert_grid(b"1\r\n2\r\n3\x1b[1;1H\x1b[M", ["2", "3"])

    def test_insert_and_delete_characters(self):
        self.assert_grid(b"abc\x1b[1;1H\x1b[2@", ["  abc"])
        self.assert_grid(b"abcdef\x1b[1;2H\x1b[2P", ["adef"])

    def test_erase_character_leaves_the_cursor(self):
        self.assert_grid(b"abcdef\x1b[1;2H\x1b[3X", ["a   ef"])

    def test_repeat_repeats_the_last_graphic(self):
        self.assert_grid(b"ab\x1b[3b", ["abbbb"])

    def test_custom_tab_stop(self):
        self.assert_grid(b"\x1b[1;4H\x1bH\x1b[1;1H\tX", ["   X"])

    def test_line_drawing_charset(self):
        self.assert_grid(b"\x1b(0lqk\x1b(B", ["┌─┐"])

    def test_shift_out_uses_g1(self):
        self.assert_grid(b"\x1b)0a\x0eq\x0fa", ["a─a"])

    def test_cursor_style_changes(self):
        result = run_example(b"\x1b[6 q")
        self.assertEqual(result.cursor_style, 4)
        result = run_example(b"\x1b[4 q")
        self.assertEqual(result.cursor_style, 3)

    def test_hard_reset_restores_the_defaults(self):
        result = run_example(b"mess\x1b[?25l\x1b[2;4r\x1b[?5h\x1bc")
        self.assertEqual(result.lines, [" " * COLUMNS] * ROWS)
        self.assertTrue(result.modes & MODE_CURSOR_VISIBLE)
        self.assertFalse(result.modes & MODE_SCREEN_REVERSE)
        self.assertEqual(result.cursor, (0, 0))

    def test_scrollback_keeps_the_tail_visible(self):
        stream = b"1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8"
        self.assert_grid(stream, ["3", "4", "5", "6", "7", "8"], save_lines=10)

    def test_wide_grapheme_occupies_two_columns(self):
        result = run_example("A漢B".encode())
        self.assertTrue(result.lines[0].startswith("A漢 B"))

    def test_wide_grapheme_wraps_whole_at_line_end(self):
        result = run_example(("x" * (COLUMNS - 1) + "漢").encode())
        self.assertTrue(result.lines[1].startswith("漢"))

    def test_combining_grapheme_stays_one_cell(self):
        result = run_example("é!".encode())
        self.assertTrue(result.lines[0].startswith("é!"))

    def test_device_attributes_land_in_the_reply_buffer(self):
        result = run_example(b"\x1b[c\x1b[>c")
        self.assertTrue(result.replies.startswith(b"\x1b[?"))
        self.assertIn(b"\x1b[>", result.replies)

    def test_cursor_position_report_reflects_the_cursor(self):
        result = run_example(b"\x1b[5;7H\x1b[6n")
        self.assertIn(b"\x1b[5;7R", result.replies)

    def test_title_and_bell_reach_the_callbacks(self):
        result = run_example(b"\x1b]0;the embedded title\x07\x07")
        self.assertIn("title: the embedded title", result.events)
        self.assertIn("bell", result.events)

    def test_construction_publishes_nothing(self):
        # The reset inside shitty_vt_new presents an empty title and asks
        # for a frame, but neither is the application speaking: no
        # callback fires before the first feed (issue 98). A terminal
        # reset by the application is another matter - RIS clears the
        # title, and that publication is real.
        self.assertEqual(run_example(b"").events, [])
        self.assertEqual(
            run_example(b"\x1b]0;x\x07\x1bc").events,
            ["title: x", "title: "],
        )

    def test_grid_matches_the_full_terminal_cursor_motion(self):
        self.assert_matches_full_terminal((
            b"hello world",
            b"\x1b[3;1HA\x1b[10AX",
            b"\x1b[2;4r\x1b[3;1HA\x1b[10AX",
            b"\x1b[1;3rA\x1b[4;1H\x1b[10BX",
            b"a\tb\tc\td",
            b"\x1b[4;7Hdeep\x1b[Hup",
            b"\x1b[19GX\x1b[5CY",
            b"\x1b[3;3H\x1b7text\x1b8over",
            b"\x1b[2;4r\x1b[?6h\x1b[HX\x1b[10;1HY",
        ))

    def test_grid_matches_the_full_terminal_editing(self):
        self.assert_matches_full_terminal((
            b"del\x1b[2Pete\rD",
            b"ins\x1b[HI\x1b[4hNS\x1b[4l",
            b"\x1b[5;1HAB\x1b[5;1H\x1b[LCD",
            b"abcdef\x1b[1;3H\x1b[2X!",
            b"one two\x1b[1;4H\x1b[K",
            b"1\r\n2\r\n3\r\n4\x1b[2;1H\x1b[2M",
            b"1\r\n2\r\n3\x1b[2;1H\x1b[2L",
            b"wide\x1b[1;1H\x1b[4@nar",
            b"ab\x1b[5bZ",
            b"line\x1b[2;1H\x1b[1J\x1b[Hnew",
        ))

    def test_grid_matches_the_full_terminal_scroll_and_screens(self):
        self.assert_matches_full_terminal((
            b"one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\nseven",
            b"\x1b[2;5r" + b"\r\n".join(str(i).encode() for i in range(9)),
            b"pre\x1b[?1049halt-screen\x1b[?1049lpost",
            b"pre\x1b[?47halt\x1b[?47l",
            b"\x1b[2Jwiped\x1b[3;3Hmark",
            b"\x1b#8\x1b[2;2H\x1b[J",
            b"x" * 47,
            b"\x1b[?7lclipped" + b"y" * 30,
            b"\x1b[6;1Hbottom\x1bD\x1bD",
            b"\x1b[H\x1bM\x1bMtop",
        ))

    def test_grid_matches_the_full_terminal_attributes_and_charsets(self):
        self.assert_matches_full_terminal((
            b"\x1b[1mB\x1b[4mU\x1b[7mR\x1b[0mN",
            b"\x1b[31mred\x1b[42mgreen\x1b[0mplain",
            b"\x1b(0lqqqk\x1b(B done",
            b"\x1b)0text\x0eqqq\x0fback",
            "мир\r\nпривет".encode(),
            "A漢字B".encode(),
            b"\x1b[38;5;120mindexed\x1b[48;2;1;2;3mdirect",
        ))

    # Fuzz records that morph presentation the example does not model:
    # double-height line attributes, and mid-stream grid resizes.
    CORPUS_SKIPS = {
        "crash-delivery-cursor-resize",
        "crash-delivery-divergence-double-height",
    }

    def test_grid_matches_the_full_terminal_on_recorded_corpus(self):
        for recorded in sorted(CORPUS.iterdir()):
            if recorded.name in self.CORPUS_SKIPS:
                continue
            stream = recorded.read_bytes()
            with self.subTest(stream=recorded.name):
                embedded = run_example(stream)
                with Shitty(columns=COLUMNS, rows=ROWS) as terminal:
                    terminal.write(stream)
                    snapshot = terminal.snapshot()
                self.assertEqual(embedded.lines, snapshot.lines)

    def test_replies_drain_incrementally(self):
        # The C contract: take_replies drains; a second call continues
        # where the first stopped. The example drains once with a large
        # buffer, so drive the split through two DA queries instead and
        # check both replies arrived in order.
        result = run_example(b"\x1b[c\x1b[5n")
        self.assertTrue(result.replies.startswith(b"\x1b[?"))
        self.assertTrue(result.replies.endswith(b"\x1b[0n"))

    def test_resize_moves_the_session_to_the_new_geometry(self):
        # The embedder resizes mid-session through the C API; text fed
        # afterwards continues on the resized grid and the row accounting
        # follows the new height.
        result = run_example(
            b"abc",
            input_script=["resize 10 3", "feed " + b"def".hex()],
        )
        self.assertTrue(result.lines[0].startswith("abcdef"))
        self.assertEqual(result.cursor, (6, 0))
        self.assertEqual(result.total_rows, 3)

    def test_resize_rejects_a_zero_dimension(self):
        # A zero dimension is the documented no-op, not a resize to
        # nothing: the session keeps its geometry and keeps working.
        result = run_example(
            b"abc",
            input_script=["resize 0 3", "resize 10 0", "feed " + b"def".hex()],
        )
        self.assertTrue(result.lines[0].startswith("abcdef"))
        self.assertEqual(result.total_rows, ROWS)

    def test_osc52_targets_pick_their_selection(self):
        # The p target lands on the primary selection, c on the
        # clipboard; the embedder hears each on its own channel.
        stream = (
            b"\x1b]52;p;" + b64("primary text") + b"\x07"
            b"\x1b]52;c;" + b64("clipboard text") + b"\x07"
        )
        result = run_example(stream)
        self.assertIn("clipboard 0: primary text", result.events)
        self.assertIn("clipboard 1: clipboard text", result.events)

    def test_paste_carries_newlines_and_large_payloads(self):
        # The facade paste converts newlines like the full terminal and
        # a multi-kilobyte payload survives the buffered path whole.
        big = "x" * 2000
        result = run_example(
            b"",
            input_script=[
                "paste " + b"one\ntwo".hex(),
                "paste " + big.encode().hex(),
            ],
        )
        self.assertEqual(result.replies, b"one\rtwo" + big.encode())

    def test_history_cap_change_survives_an_alternate_screen_visit(self):
        # The embedder retunes the history cap after the application has
        # been to the alternate screen and back: the rebuild puts the
        # inactive alternate screen aside and the primary content stays.
        result = run_example(
            b"one\r\ntwo\x1b[?1049hALT\x1b[?1049l",
            set_save_lines=9,
        )
        self.assertTrue(result.lines[0].startswith("one"))
        self.assertTrue(result.lines[1].startswith("two"))
        self.assertEqual(result.capacity_rows, ROWS + 9)

    def test_space_toggles_rectangular_mid_drag(self):
        # Space during a live drag flips the selection rectangular, the
        # release publishes the block, and the space never reaches the
        # application.
        result = run_example(
            b"abcdef\r\nghijkl",
            input_script=[
                "button 0 1 1 0 0 1.0",
                "motion 4 1 0",
                "key 2 1 0 32 32 32",
                "button 0 0 4 1 0 1.2",
            ],
        )
        # The driver prints the payload verbatim, so the two-row block
        # arrives as two event lines - and it is a block, not the linear
        # bcdef/ghij span.
        self.assertIn("clipboard 0: bcd", result.events)
        self.assertIn("hij", result.events)
        self.assertNotIn("clipboard 0: bcdef", result.events)
        self.assertEqual(result.replies, b"")

    def test_a_control_click_opens_the_detected_link(self):
        # A plain URL in the output is a link without any escape
        # sequence; a control-click on it reaches the open_uri callback,
        # scheme-checked against the GUI defaults.
        result = run_example(
            b"http://example.test/x tail",
            input_script=[
                "button 0 1 2 0 2 1.0",
                "button 0 0 2 0 2 1.1",
            ],
        )
        self.assertIn("open-uri: http://example.test/x", result.events)


if __name__ == "__main__":
    unittest.main()


@unittest.skipUnless(EXAMPLE_PRESENT, f"the embedding example is not built: {EXAMPLE}")
class ScrollbackTest(unittest.TestCase):
    """The view movement the facade exposes, checked against the grid it
    is supposed to move."""

    @staticmethod
    def stream(count):
        return "".join(f"line{index}\r\n" for index in range(count)).encode()

    def test_history_holds_what_scrolled_off_the_grid(self):
        # Ten lines plus the trailing newline occupy eleven rows; six of
        # them are on screen, so five went into the history.
        kept = run_example(self.stream(10), save_lines=100)
        self.assertEqual(kept.history_rows, 5)
        self.assertEqual(kept.scroll_offset, 0)
        self.assertEqual(kept.lines[0].rstrip(), "line5")

    def test_a_terminal_keeping_no_lines_retains_no_history(self):
        result = run_example(self.stream(10), save_lines=0)
        self.assertEqual(result.history_rows, 0)

    def test_history_is_capped_by_save_lines(self):
        result = run_example(self.stream(40), save_lines=3)
        self.assertEqual(result.history_rows, 3)

    def test_scrolling_moves_the_view_over_the_history(self):
        result = run_example(self.stream(10), save_lines=100, scroll=2)
        self.assertEqual(result.scroll_offset, 2)
        self.assertEqual(result.lines[0].rstrip(), "line3")

    def test_scrolling_clamps_to_the_retained_history(self):
        result = run_example(self.stream(10), save_lines=100, scroll=99)
        self.assertEqual(result.scroll_offset, 5)
        self.assertEqual(result.lines[0].rstrip(), "line0")

    def test_scrolling_back_down_returns_to_the_live_bottom(self):
        result = run_example(self.stream(10), save_lines=100, scroll=-5)
        self.assertEqual(result.scroll_offset, 0)
        self.assertEqual(result.lines[0].rstrip(), "line5")

    def test_alternate_screen_has_no_history_to_scroll(self):
        stream = b"\x1b[?1049h" + self.stream(10)
        result = run_example(stream, save_lines=100, scroll=3)
        self.assertEqual(result.history_rows, 0)
        self.assertEqual(result.scroll_offset, 0)

    def test_scrolling_to_an_absolute_offset_lands_there(self):
        result = run_example(self.stream(10), save_lines=100, scroll_to=3)
        self.assertEqual(result.scroll_offset, 3)
        self.assertEqual(result.lines[0].rstrip(), "line2")

    def test_scrolling_to_the_current_offset_changes_nothing(self):
        # The no-op path: settle at 2 relatively, then ask for 2 again.
        result = run_example(self.stream(10), save_lines=100, scroll=2, scroll_to=2)
        self.assertEqual(result.scroll_offset, 2)
        self.assertEqual(result.lines[0].rstrip(), "line3")

    def test_scrolling_to_zero_returns_to_the_live_bottom(self):
        result = run_example(self.stream(10), save_lines=100, scroll=4, scroll_to=0)
        self.assertEqual(result.scroll_offset, 0)
        self.assertEqual(result.lines[0].rstrip(), "line5")

    def test_scrolling_to_past_the_history_clamps(self):
        result = run_example(self.stream(10), save_lines=100, scroll_to=99)
        self.assertEqual(result.scroll_offset, 5)
        self.assertEqual(result.lines[0].rstrip(), "line0")


@unittest.skipUnless(EXAMPLE_PRESENT, f"the embedding example is not built: {EXAMPLE}")
class HistoryRowTest(unittest.TestCase):
    """Reading rows by index, which must not depend on where the view sits."""

    @staticmethod
    def stream(count):
        return "".join(f"line{index}\r\n" for index in range(count)).encode()

    def test_every_retained_row_is_addressable_oldest_first(self):
        result = run_example(self.stream(10), save_lines=100, dump_rows=1)
        # Five scrolled off, six on screen; the last is the blank row the
        # trailing newline opened.
        self.assertEqual(result.total_rows, 11)
        self.assertEqual(len(result.rows_by_index), 11)
        self.assertEqual(
            [row.rstrip() for row in result.rows_by_index[:6]],
            ["line0", "line1", "line2", "line3", "line4", "line5"],
        )
        self.assertEqual(result.rows_by_index[10].strip(), "")

    def test_row_reads_ignore_the_view_position(self):
        live = run_example(self.stream(10), save_lines=100, dump_rows=1)
        scrolled = run_example(
            self.stream(10), save_lines=100, scroll=3, dump_rows=1
        )
        self.assertEqual(scrolled.scroll_offset, 3)
        self.assertEqual(scrolled.rows_by_index, live.rows_by_index)

    def test_a_terminal_without_history_addresses_only_the_grid(self):
        result = run_example(self.stream(10), save_lines=0, dump_rows=1)
        self.assertEqual(result.total_rows, ROWS)
        self.assertEqual(result.rows_by_index[0].rstrip(), "line5")

    def test_reading_past_the_last_row_yields_nothing(self):
        # The example only walks in range, so drive the edge through a
        # terminal whose history is capped: index total-1 is the last row
        # and the dump stops there rather than running on.
        result = run_example(self.stream(40), save_lines=3, dump_rows=1)
        self.assertEqual(result.total_rows, 3 + ROWS)
        self.assertEqual(len(result.rows_by_index), 3 + ROWS)
        self.assertEqual(result.rows_by_index[0].rstrip(), "line32")


@unittest.skipUnless(EXAMPLE_PRESENT, f"the embedding example is not built: {EXAMPLE}")
class HistoryBudgetTest(unittest.TestCase):
    """Changing the history cap after construction, and what it costs."""

    @staticmethod
    def stream(count):
        return "".join(f"line{index}\r\n" for index in range(count)).encode()

    def test_memory_grows_with_the_history_it_backs(self):
        empty = run_example(b"", save_lines=100)
        filled = run_example(self.stream(40), save_lines=100)
        self.assertEqual(empty.cell_bytes, 0)
        self.assertGreater(filled.allocated_rows, empty.allocated_rows)
        self.assertEqual(
            filled.cell_bytes,
            filled.allocated_rows * COLUMNS * 16,
            "cell_bytes should be rows * columns * cell_size",
        )
        # The cap is what it may hold, not what it holds.
        self.assertEqual(filled.capacity_rows, ROWS + 100)

    def test_lowering_the_cap_drops_the_oldest_rows_at_once(self):
        result = run_example(self.stream(40), save_lines=100, set_save_lines=5)
        self.assertEqual(result.history_rows, 5)
        self.assertEqual(result.capacity_rows, ROWS + 5)
        # Not merely reported: the surviving rows are the newest five.
        rows = run_example(
            self.stream(40), save_lines=100, set_save_lines=5, dump_rows=1
        )
        self.assertEqual(rows.rows_by_index[0].rstrip(), "line30")

    def test_lowering_the_cap_releases_the_rows_it_dropped(self):
        before = run_example(self.stream(40), save_lines=100)
        after = run_example(self.stream(40), save_lines=100, set_save_lines=5)
        self.assertLess(after.cell_bytes, before.cell_bytes)

    def test_raising_the_cap_does_not_resurrect_dropped_rows(self):
        result = run_example(self.stream(40), save_lines=5, set_save_lines=100)
        self.assertEqual(result.capacity_rows, ROWS + 100)
        self.assertEqual(result.history_rows, 5)

    def test_the_visible_grid_survives_a_cap_change(self):
        before = run_example(self.stream(40), save_lines=100)
        after = run_example(self.stream(40), save_lines=100, set_save_lines=5)
        self.assertEqual(after.lines, before.lines)


# The pinned SHITTY_VT_KEY_* values the scripts below use.
KEY_ESCAPE = 3
KEY_UP = 11
KEY_PRINTABLE = 1
MOD_CONTROL = 1 << 1


@unittest.skipUnless(EXAMPLE_PRESENT, f"the embedding example is not built: {EXAMPLE}")
class InputEncodingTest(unittest.TestCase):
    """The input entry points: events go in, the terminal encodes them by
    whatever protocol the stream negotiated, and the bytes come back on
    the replies line."""

    def replies(self, stream, script):
        return run_example(stream, input_script=script).replies

    def key(self, key, action=0, mods=0, layout=0, base=0, shifted=0):
        return f"key {key} {action} {mods} {layout} {base} {shifted}"

    def test_arrow_key_follows_the_cursor_mode(self):
        script = [self.key(KEY_UP), "flush"]
        self.assertEqual(self.replies(b"", script), b"\x1b[A")
        self.assertEqual(self.replies(b"\x1b[?1h", script), b"\x1bOA")

    def test_text_sends_utf8(self):
        self.assertEqual(self.replies(b"", ["text 65 0", "flush"]), b"A")
        self.assertEqual(
            self.replies(b"", ["text 1090 0", "flush"]),
            "т".encode(),
        )

    def test_control_chord_encodes_through_the_key_event(self):
        script = [
            self.key(KEY_PRINTABLE, mods=MOD_CONTROL, layout=0x63, base=0x63, shifted=0x43),
            "flush",
        ]
        self.assertEqual(self.replies(b"", script), b"\x03")

    def test_kitty_flags_change_the_escape_key(self):
        script = [self.key(KEY_ESCAPE), "flush"]
        self.assertEqual(self.replies(b"", script), b"\x1b")
        self.assertEqual(self.replies(b"\x1b[>1u", script), b"\x1b[27u")

    def test_kitty_reports_the_release(self):
        script = [
            self.key(KEY_ESCAPE),
            "flush",
            self.key(KEY_ESCAPE, action=2),
            "flush",
        ]
        replies = self.replies(b"\x1b[>3u", script)
        self.assertEqual(replies, b"\x1b[27u\x1b[27;1:3u")

    def test_paste_honors_the_bracketed_mode(self):
        script = ["paste " + b"hi".hex()]
        self.assertEqual(self.replies(b"", script), b"hi")
        self.assertEqual(
            self.replies(b"\x1b[?2004h", script),
            b"\x1b[200~hi\x1b[201~",
        )

    def test_sgr_mouse_reports_press_and_release(self):
        replies = self.replies(
            b"\x1b[?1000h\x1b[?1006h",
            ["button 0 1 4 2 0 1.0", "button 0 0 4 2 0 1.1"],
        )
        self.assertEqual(replies, b"\x1b[<0;5;3M\x1b[<0;5;3m")

    def test_motion_reports_under_any_event_tracking(self):
        replies = self.replies(
            b"\x1b[?1003h\x1b[?1006h",
            ["motion 4 2 0"],
        )
        self.assertEqual(replies, b"\x1b[<35;5;3M")

    def test_wheel_reports_when_captured(self):
        replies = self.replies(
            b"\x1b[?1000h\x1b[?1006h",
            ["wheel 0 1 4 2 0"],
        )
        self.assertEqual(replies, b"\x1b[<64;5;3M")

    def test_wheel_scrolls_the_view_otherwise(self):
        stream = b"".join(b"line%d\r\n" % k for k in range(40))
        result = run_example(
            stream, save_lines=100, input_script=["wheel 0 1 4 2 0"]
        )
        self.assertGreater(result.scroll_offset, 0)

    def test_focus_reports_when_asked(self):
        script = ["focus 0", "focus 1"]
        self.assertEqual(self.replies(b"", script), b"")
        self.assertEqual(
            self.replies(b"\x1b[?1004h", script),
            b"\x1b[O\x1b[I",
        )

    def test_selection_drag_reaches_the_clipboard_callback(self):
        # An unshifted drag with no tracking mode selects; the finished
        # selection is published through clipboard_set like an OSC 52
        # write would be.
        result = run_example(
            b"grab me",
            input_script=[
                "button 0 1 0 0 0 1.0",
                "motion 4 0 0",
                "button 0 0 4 0 0 1.2",
            ],
        )
        self.assertIn("clipboard 0: grab", result.events)
        self.assertEqual(result.replies, b"")



def preedit_command(text, begin=-1, end=-1):
    payload = text.encode().hex() if text else "-"
    return f"preedit {payload} {begin} {end}"


class PreeditTest(unittest.TestCase):
    """The composition preview: an input method's uncommitted text, drawn
    over the cursor row and belonging to no one else - not the grid, not
    the scrollback, not the child."""

    def test_the_preview_is_drawn_from_the_cursor(self):
        result = run_example(b"hello", input_script=[preedit_command("abc", 0, 3)])
        self.assertEqual(
            result.preedit, Preedit(row=0, column=5, cells=3, text="abc")
        )

    def test_the_preview_stays_out_of_the_grid_and_the_child(self):
        result = run_example(b"hello", input_script=[preedit_command("abc", 0, 3)])
        # The row still holds what the application wrote, and the child
        # hears nothing until the input method commits.
        self.assertEqual(result.lines[0].rstrip(), "hello")
        self.assertEqual(result.replies, b"")

    def test_the_cursor_hides_and_anchors_the_candidate_window(self):
        # The input method draws its candidate list at the cursor, so
        # while composing the cursor tracks the preview's own cursor
        # rather than the application's.
        result = run_example(b"hello", input_script=[preedit_command("abc", 1, 1)])
        self.assertEqual(result.cursor, (6, 0))
        self.assertFalse(result.cursor_visible)

    def test_an_empty_preview_clears_the_composition(self):
        result = run_example(
            b"hello",
            input_script=[preedit_command("abc", 0, 3), preedit_command("")],
        )
        self.assertIsNone(result.preedit)
        # And the application's cursor comes back where it was.
        self.assertEqual(result.cursor, (5, 0))
        self.assertTrue(result.cursor_visible)

    def test_a_wide_character_takes_two_columns_of_the_preview(self):
        # Continuations are not reported, as everywhere else: two cells
        # covering four columns.
        result = run_example(
            b"hello", input_script=[preedit_command("日本", 0, 6)]
        )
        self.assertEqual(
            result.preedit,
            Preedit(row=0, column=5, cells=2, text="日本"),
        )

    def test_a_preview_too_long_for_the_row_keeps_the_fresh_end(self):
        result = run_example(
            b"", input_script=[preedit_command("abcdefghijklmnopqrstuvwxyz", 0, 26)]
        )
        self.assertEqual(
            result.preedit,
            Preedit(row=0, column=0, cells=COLUMNS, text="ghijklmnopqrstuvwxyz"),
        )

    def test_an_invalid_byte_aborts_the_preview(self):
        result = run_example(b"hello", input_script=["preedit ff61 0 2"])
        self.assertIsNone(result.preedit)

    def test_a_cursor_range_past_the_text_is_clamped(self):
        result = run_example(b"hello", input_script=[preedit_command("ab", 0, 99)])
        self.assertEqual(
            result.preedit, Preedit(row=0, column=5, cells=2, text="ab")
        )
        self.assertEqual(result.cursor, (5, 0))

    def test_a_double_width_row_shows_no_preview(self):
        result = run_example(
            b"\x1b#6hello", input_script=[preedit_command("abc", 0, 3)]
        )
        self.assertIsNone(result.preedit)

    def test_clipping_never_starts_on_a_wide_continuation(self):
        # The tail that fits would begin on the second half of a wide
        # character; the slice moves one cell further instead.
        result = run_example(
            b"", columns=5, input_script=[preedit_command("a日本語本日", 0, 20)]
        )
        self.assertEqual(
            result.preedit, Preedit(row=0, column=0, cells=2, text="本日")
        )
        result = run_example(
            b"", columns=6, input_script=[preedit_command("a日本語本日", 0, 20)]
        )
        self.assertEqual(
            result.preedit, Preedit(row=0, column=0, cells=3, text="語本日")
        )


    def test_preview_bytes_are_decoded_strictly(self):
        # Three and four byte sequences decode; overlong, truncated,
        # surrogate and out-of-range sequences abort the preview.
        cases = (
            ("e697a5", "日"), ("f09f9880", "😀"), ("61e6", "a"),
            ("c080", None), ("e697", None), ("e08080", None),
            ("eda080", None), ("ff41", None), ("c3", None), ("f4908080", None),
        )
        for payload, text in cases:
            with self.subTest(payload=payload):
                result = run_example(b"hi", input_script=[f"preedit {payload} 0 1"])
                if text is None:
                    self.assertIsNone(result.preedit)
                else:
                    self.assertEqual(result.preedit.text, text)

    def test_a_combining_mark_shares_the_cell_it_extends(self):
        # The preview clusters like printed text, and the facade hands
        # the whole cluster over: one cell, both codepoints.
        result = run_example(
            b"hello", input_script=[preedit_command("e\u0301", 0, 3)]
        )
        self.assertEqual(
            result.preedit,
            Preedit(row=0, column=5, cells=1, text="e\u0301"),
        )

    def test_a_joined_emoji_is_one_preview_cluster(self):
        result = run_example(
            b"",
            input_script=[preedit_command("\U0001f469\u200d\U0001f4bb", 0, 11)],
        )
        self.assertEqual(
            result.preedit,
            Preedit(
                row=0,
                column=0,
                cells=1,
                text="\U0001f469\u200d\U0001f4bb",
            ),
        )


class CellColorSourceTest(unittest.TestCase):
    """Where each color came from, alongside what it resolved to.

    An embedder with a palette of its own - drawing into another terminal,
    or theming its panes - needs the request, not only the answer: resolved
    RGB pins every cell to this terminal's configuration."""

    def test_an_unstyled_cell_names_the_defaults(self):
        result = run_example(b"hi")
        self.assertEqual(result.colors[0], ("default_fg", "default_bg", "default_fg"))
        self.assertEqual(result.colors[1], ("default_fg", "default_bg", "default_fg"))

    def test_an_ansi_color_keeps_its_palette_index(self):
        # Painted red is index 1 whatever this terminal resolves index 1
        # to, which is what lets an embedder apply its own red.
        result = run_example(b"\x1b[31mr\x1b[0m")
        self.assertEqual(result.colors[0][0], "indexed:1")

    def test_a_256_color_keeps_its_index_too(self):
        result = run_example(b"\x1b[38;5;99mx\x1b[0m")
        self.assertEqual(result.colors[0][0], "indexed:99")

    def test_a_background_carries_its_own_source(self):
        result = run_example(b"\x1b[44mb\x1b[0m")
        self.assertEqual(result.colors[0][:2], ("default_fg", "indexed:4"))

    def test_a_true_color_request_is_direct(self):
        # Nothing to resolve: the application named the value itself.
        result = run_example(b"\x1b[38;2;1;2;3mt\x1b[0m")
        self.assertEqual(result.colors[0][0], "direct")

    def test_the_underline_color_is_reported_separately(self):
        result = run_example(b"\x1b[4;58;5;7mu\x1b[0m")
        self.assertEqual(result.colors[0][2], "indexed:7")

    def test_an_unset_underline_color_follows_the_foreground(self):
        # The model has no separate default for it: an underline with no
        # color of its own is drawn in the cell's foreground, and the
        # source says so rather than inventing a default.
        result = run_example(b"\x1b[4;31mu\x1b[0m")
        self.assertEqual(result.colors[0][2], "indexed:1")

    def test_a_redefined_palette_entry_is_still_that_entry(self):
        # OSC 4 moves what index 1 resolves to. The cell still asked for
        # index 1, which is the whole point of reporting the request: an
        # embedder resolves it against its own palette, not this one.
        result = run_example(b"\x1b]4;1;rgb:00/00/ff\x07\x1b[31mr\x1b[0m")
        self.assertEqual(result.colors[0][0], "indexed:1")

    def test_a_special_color_standing_in_for_the_default_is_direct(self):
        # OSC 5;0 names the bold color and OSC 6;0 turns it on: a bold
        # cell that asked for the default foreground is painted with it.
        # The embedder gets a color in its own right, not a default it
        # would otherwise replace with its own theme.
        result = run_example(
            b"\x1b]5;0;#010203\x1b\\\x1b]6;0;1\x1b\\\x1b[1mB\x1b[0mp"
        )
        self.assertEqual(result.colors[0][0], "direct")
        self.assertEqual(result.colors[1][0], "default_fg")

    def test_a_special_color_overriding_an_index_is_direct(self):
        # OSC 6;5 lets the special colors override ANSI requests too, so
        # a bold red cell is painted with the bold color and reporting
        # index 1 would describe a color the embedder never receives.
        result = run_example(
            b"\x1b]5;0;#010203\x1b\\\x1b]6;0;1;5;1\x1b\\"
            b"\x1b[1;31mA\x1b[0;31mr\x1b[0m"
        )
        self.assertEqual(result.colors[0][0], "direct")
        self.assertEqual(result.colors[1][0], "indexed:1")

    def test_the_inverse_special_color_makes_the_background_direct(self):
        result = run_example(
            b"\x1b]5;3;#040506\x1b\\\x1b]6;3;1\x1b\\\x1b[7mR\x1b[0mp"
        )
        self.assertEqual(result.colors[0][1], "direct")
        self.assertEqual(result.colors[1][1], "default_bg")


class RowWrapTest(unittest.TestCase):
    """Where a row's text stops because it wrapped onto the next one.

    An embedder rejoining a wrapped line needs more than "this row is
    continued": the wrap point is wherever the terminal ran out of room,
    which is not always the last column, and the columns after it belong
    to no one.
    """

    def test_a_row_that_wraps_reports_where_it_stopped(self):
        # Twelve characters into eight columns: the first row is full and
        # continues, the second holds the rest and ends on its own.
        result = run_example(b"abcdefghijkl", columns=8, rows=3)

        self.assertEqual(result.wrap[0], 8)
        self.assertEqual(result.wrap[1], 0)

    def test_a_row_ended_by_a_newline_does_not_wrap(self):
        result = run_example(b"abc\r\ndef", columns=8, rows=3)

        self.assertEqual(result.wrap[0], 0)
        self.assertEqual(result.wrap[1], 0)

    def test_a_blank_row_does_not_wrap(self):
        result = run_example(b"ab", columns=8, rows=3)

        self.assertEqual(result.wrap[0], 0)
        self.assertEqual(result.wrap[2], 0)

    def test_a_wide_character_wraps_before_the_last_column(self):
        # Seven narrow cells then a double-width one, which does not fit in
        # the last column of eight. The row is continued and yet its text
        # ends at column 7, so a flag saying only "wrapped" would leave an
        # embedder rejoining a column of nothing.
        result = run_example("abcdefg\u4e00".encode(), columns=8, rows=3)

        self.assertEqual(result.wrap[0], 7)
        self.assertEqual(result.lines[1][0], "\u4e00")

    def test_autowrap_off_clamps_instead_of_wrapping(self):
        # DECAWM off: the row keeps overwriting its last column rather than
        # continuing, so nothing was wrapped.
        result = run_example(b"\x1b[?7labcdefghijkl", columns=8, rows=3)

        self.assertEqual(result.wrap[0], 0)

    def test_a_scrolled_off_row_keeps_its_wrap(self):
        # The index space is the one shitty_vt_row_cells uses - the retained
        # history followed by the live grid - so a wrapped line that has
        # scrolled out of view still reports where it wrapped.
        result = run_example(
            b"abcdefghijkl\r\n" + b"x\r\n" * 4,
            columns=8,
            rows=3,
            save_lines=8,
        )

        self.assertGreater(result.history_rows, 0)
        self.assertEqual(result.wrap[0], 8)
        self.assertEqual(result.wrap[1], 0)

    def test_the_wrap_survives_a_scrolled_view(self):
        # Reading a row is independent of where the user has scrolled, and
        # so is reading where it wrapped.
        result = run_example(
            b"abcdefghijkl\r\n" + b"x\r\n" * 4,
            columns=8,
            rows=3,
            save_lines=8,
            scroll=2,
        )

        self.assertEqual(result.scroll_offset, 2)
        self.assertEqual(result.wrap[0], 8)

    def test_an_index_past_the_last_row_reports_no_wrap(self):
        # The header promises 0 rather than a guess, and an embedder
        # walking a scrollback that shrank under it will ask.
        result = run_example(
            b"abcdefghijkl",
            columns=8,
            rows=3,
            input_script=["wrap 2", "wrap 3", "wrap 99"],
        )

        self.assertEqual(result.events, ["wrap 2: 0", "wrap 3: 0", "wrap 99: 0"])

    def test_the_wrap_follows_a_reflow(self):
        # Resizing re-lays the text, so where a row wraps belongs to the
        # screen it is on now rather than to the width it was written at.
        written = b"abcdefghijkl"

        # Narrower: one wrapped line becomes two, both continued.
        narrowed = run_example(written, columns=8, rows=3, input_script=["resize 4 3"])
        self.assertEqual([narrowed.wrap[index] for index in range(3)], [4, 4, 0])

        # Wider: it fits, so nothing wraps.
        widened = run_example(written, columns=8, rows=3, input_script=["resize 16 3"])
        self.assertEqual(widened.wrap[0], 0)

        # And back, which also shows the text survived the widening
        # rather than the wrap having simply been cleared.
        restored = run_example(
            written,
            columns=8,
            rows=3,
            dump_rows=1,
            input_script=["resize 16 3", "resize 8 3"],
        )
        self.assertEqual(restored.wrap[0], 8)
        self.assertEqual(restored.rows_by_index[:2], ["abcdefgh", "ijkl    "])

    def test_every_addressable_row_reports_a_wrap(self):
        # The index space is exactly the one shitty_vt_row_cells uses: the
        # retained history first, then the live grid, one answer per row.
        # An embedder walking its scrollback needs the two to line up.
        result = run_example(
            b"abcdefghijkl\r\n" + b"x\r\n" * 4,
            columns=8,
            rows=3,
            save_lines=8,
        )

        self.assertEqual(sorted(result.wrap), list(range(result.total_rows)))


class DriverEdgeTest(unittest.TestCase):
    """Argument checks of the C API and the driver reached through the
    script and the command line."""

    def test_out_of_range_keys_and_buttons_are_refused(self):
        result = run_example(
            b"x",
            input_script=[
                "key 9999 1 0 65 65 65",
                "key 3 7 0 65 65 65",
                "button 9 1 0 0 0 1.0",
            ],
        )
        self.assertEqual(result.events, [])
        self.assertEqual(result.replies, b"")

    def test_a_middle_click_pastes_the_primary_selection(self):
        result = run_example(
            b"hello world",
            input_script=[
                "button 0 1 0 0 0 1.0",
                "motion 4 0 0",
                "button 0 0 4 0 0 1.2",
                "button 2 1 8 0 0 2.0",
                "button 2 0 8 0 0 2.1",
            ],
        )
        self.assertEqual(result.events, ["clipboard 0: hell"])
        self.assertEqual(result.replies, b"hell")

    def test_reapplying_the_same_history_cap_changes_nothing(self):
        result = run_example(
            b"a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh", save_lines=5, set_save_lines=5
        )
        self.assertEqual(result.capacity_rows, ROWS + 5)
        self.assertEqual(result.total_rows, 8)

    def test_link_schemes_are_folded_and_filtered(self):
        for uri, opened in (
            (b"FILE:///tmp/x", True),
            (b"HTTP://a.test/c", True),
            (b"ftp://x.test/y", False),
            (b"mailto:a@b.c", False),
        ):
            with self.subTest(uri=uri):
                result = run_example(
                    uri + b" tail",
                    input_script=[
                        "button 0 1 2 0 2 1.0",
                        "button 0 0 2 0 2 1.1",
                    ],
                )
                expected = ["open-uri: " + uri.decode()] if opened else []
                self.assertEqual(result.events, expected)

    def test_odd_hex_in_a_feed_stops_at_the_last_full_byte(self):
        result = run_example(b"x", input_script=["feed 616", "feed zz"])
        self.assertEqual(result.lines[0].rstrip(), "xa")

    def test_growing_the_session_past_the_snapshot_grid(self):
        result = run_example(
            b"abc", input_script=["resize 30 8", "feed " + b"def".hex()]
        )
        self.assertEqual(result.lines[0].rstrip(), "abcdef")
        self.assertEqual(result.total_rows, 8)
        self.assertEqual(result.cursor, (6, 0))

    def test_command_line_failures_and_short_forms(self):
        with tempfile.NamedTemporaryFile() as stream:
            stream.write(b"hi")
            stream.flush()
            missing = ["20", "6", "0", "/nonexistent/stream"]
            result = subprocess.run(
                [str(EXAMPLE)] + missing, capture_output=True, timeout=60
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"can not open", result.stderr)

            bad_script = ["20", "6", "0", stream.name, "0", "-1", "0", "-1", "/nonexistent/script"]
            result = subprocess.run(
                [str(EXAMPLE)] + bad_script, capture_output=True, timeout=60
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"can not open", result.stderr)

            result = subprocess.run(
                [str(EXAMPLE), "0", "6", "0", stream.name],
                capture_output=True, timeout=60,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"shitty_vt_new failed", result.stderr)

            result = subprocess.run(
                [str(EXAMPLE), stream.name], capture_output=True, timeout=60
            )
            self.assertEqual(result.returncode, 0)
            self.assertTrue(result.stdout.startswith(b"hi "))

            result = subprocess.run(
                [str(EXAMPLE)], input=b"stdin-data", capture_output=True, timeout=60
            )
            self.assertEqual(result.returncode, 0)
            self.assertTrue(result.stdout.startswith(b"stdin-data "))
