# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class ResizeSameGridPixelsTest(unittest.TestCase):
    def test_pixel_only_resize_preserves_grid_content_and_cursor(self):
        with Shitty(columns=5, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"abc\r\ndef")
            before = terminal.snapshot()
            terminal.resize_pixels(25, 35)
            after = terminal.snapshot()
            self.assertEqual((after.columns, after.rows), (5, 3))
            self.assertEqual(after.lines, before.lines)
            self.assertEqual(
                (after.cursor_x, after.cursor_y),
                (before.cursor_x, before.cursor_y),
            )

    def test_pixel_only_resize_preserves_history_view_and_selection(self):
        with Shitty(
            columns=5, rows=3, save_lines=5, glyph_px=4, glyph_py=8,
        ) as terminal:
            terminal.write(b"1\r\n2\r\n3\r\n4\r\n5")
            terminal.wheel_up(1)
            terminal.select_start(0, 0)
            terminal.select_update(1, 0)
            terminal.resize_pixels(27, 33)
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.view_offset, 1)
            self.assertEqual(snapshot.lines[0].rstrip(), "2")
            self.assertEqual(terminal.select_finish(), b"2")

    def test_pixel_only_resize_keeps_pty_grid_but_requests_refresh(self):
        with Shitty(columns=5, rows=3, glyph_px=4, glyph_py=8) as terminal:
            before = terminal.snapshot().refresh_count
            terminal.resize_pixels(25, 35)
            after = terminal.snapshot().refresh_count
            self.assertEqual(terminal.winsize(), (5, 3))
            self.assertGreater(after, before)

    def test_in_band_resize_reports_same_grid_for_pixel_only_change(self):
        with Shitty(columns=5, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"\x1b[?2048h")
            self.assertEqual(terminal.read_input(), b"\x1b[48;3;5;24;20t")
            terminal.resize_pixels(25, 35)
            self.assertEqual(terminal.read_input(), b"\x1b[48;3;5;24;20t")


if __name__ == "__main__":
    unittest.main()
