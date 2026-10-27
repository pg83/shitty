# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class MouseFrontendScrollTest(unittest.TestCase):
    def test_fractional_local_scroll_accumulates_to_exact_lines(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"0\r\n1\r\n2\r\n3\r\n4")
            for _ in range(3):
                terminal.scroll(0, 0.25)
            self.assertEqual(terminal.snapshot().view_offset, 0)

            terminal.scroll(0, 0.25)
            self.assertEqual(terminal.snapshot().view_offset, 1)
            terminal.scroll(0, -1)
            self.assertEqual(terminal.snapshot().view_offset, 0)

    def test_fractional_reporting_scroll_restarts_after_reversal(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.scroll(0, 0.75, pixel_x=5, pixel_y=4)
            terminal.scroll(0, -0.5, pixel_x=5, pixel_y=4)
            terminal.scroll(0, 0.75, pixel_x=5, pixel_y=4)
            self.assertEqual(terminal.read_input(), b"")
            terminal.scroll(0, 0.25, pixel_x=5, pixel_y=4)
            self.assertEqual(terminal.read_input(), b"\x1b[<64;4;2M")

    def test_vertical_and_horizontal_wheel_buttons_are_distinct(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.scroll(1, 1, pixel_x=5, pixel_y=4)
            terminal.scroll(-1, -1, pixel_x=5, pixel_y=4)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<64;4;2M\x1b[<67;4;2M"
                b"\x1b[<65;4;2M\x1b[<66;4;2M",
            )

    def test_reporting_wheel_encodes_alt_and_control_modifiers(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.scroll(0, 1, modifiers=6, pixel_x=5, pixel_y=4)
            self.assertEqual(
                terminal.read_input(), b"\x1b[<88;4;2M"
            )

    def test_shift_override_switches_to_local_scroll_and_resets_remainders(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(
                b"0\r\n1\r\n2\r\n3\r\n4"
                b"\x1b[?1000h\x1b[?1006h"
            )
            terminal.scroll(0, 0.75)
            terminal.scroll(0, 0.25, modifiers=1)
            self.assertEqual(terminal.snapshot().view_offset, 0)
            terminal.scroll(0, 0.25)
            self.assertEqual(terminal.read_input(), b"")

            terminal.scroll(0, 1)
            self.assertEqual(terminal.read_input(), b"\x1b[<64;1;1M")

    def test_horizontal_local_delta_never_leaks_into_reporting_mode(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.scroll(0.75, 0)
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.scroll(0.25, 0)
            self.assertEqual(terminal.read_input(), b"")

    def test_single_scroll_event_is_clamped_to_one_hundred_steps(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.scroll(0, 1000)
            reply = terminal.read_input()
            self.assertEqual(reply.count(b"\x1b[<64;1;1M"), 100)
            self.assertEqual(len(reply), 100 * len(b"\x1b[<64;1;1M"))

    def test_alternate_scroll_mode_turns_raw_wheel_into_cursor_keys(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1049h\x1b[?1007h")
            terminal.scroll(0, 2)
            terminal.scroll(0, -1)
            self.assertEqual(
                terminal.read_input(), b"\x1b[A\x1b[A\x1b[B"
            )

    def test_sgr_pixel_mode_uses_framebuffer_coordinates(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1016h")
            terminal.scroll(0, 1, pixel_x=5, pixel_y=4)
            self.assertEqual(
                terminal.read_input(), b"\x1b[<64;4;2M"
            )


if __name__ == "__main__":
    unittest.main()
