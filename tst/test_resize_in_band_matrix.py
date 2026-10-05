# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class ResizeInBandMatrixTest(unittest.TestCase):
    def test_exact_duplicate_resize_does_not_emit_spurious_report(self):
        with Shitty(columns=6, rows=3) as terminal:
            terminal.write(b"\x1b[?2048h")
            terminal.read_input()
            terminal.resize(6, 3)
            self.assertEqual(terminal.read_input(), b"")

    def test_each_distinct_completed_resize_emits_one_report(self):
        with Shitty(columns=6, rows=3) as terminal:
            terminal.write(b"\x1b[?2048h")
            terminal.read_input()
            terminal.resize(7, 4)
            terminal.resize(9, 2)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[48;4;7;4;7t\x1b[48;2;9;2;9t",
            )

    def test_reenable_reports_current_size_after_disabled_resizes(self):
        with Shitty(columns=6, rows=3) as terminal:
            terminal.write(b"\x1b[?2048h\x1b[?2048l")
            terminal.read_input()
            terminal.resize(9, 5)
            self.assertEqual(terminal.read_input(), b"")
            terminal.write(b"\x1b[?2048h")
            self.assertEqual(terminal.read_input(), b"\x1b[48;5;9;5;9t")

    def test_report_uses_selected_eight_bit_csi_form(self):
        with Shitty(columns=6, rows=3) as terminal:
            terminal.write(b"\x1b G\x1b[?2048h")
            self.assertEqual(terminal.read_input(), b"\x9b48;3;6;3;6t")
            terminal.resize(7, 4)
            self.assertEqual(terminal.read_input(), b"\x9b48;4;7;4;7t")

    def test_non_unit_glyph_report_uses_grid_area_not_border_or_remainder(self):
        with Shitty(columns=5, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"\x1b[?2048h")
            terminal.read_input()
            terminal.resize_pixels(27, 35)
            self.assertEqual(terminal.read_input(), b"\x1b[48;3;5;24;20t")


if __name__ == "__main__":
    unittest.main()
