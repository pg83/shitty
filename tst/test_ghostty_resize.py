# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class GhosttyResizeTest(unittest.TestCase):
    def assert_private_mode(self, terminal, mode, enabled):
        terminal.write(f"\x1b[?{mode}$p".encode())
        state = 1 if enabled else 2
        self.assertEqual(
            terminal.read_input(),
            f"\x1b[?{mode};{state}$y".encode(),
        )

    def test_resize_clears_synchronized_output_on_unchanged_grid(self):
        with Shitty(
            columns=80,
            rows=24,
            glyph_px=9,
            glyph_py=18,
        ) as terminal:
            terminal.write(b"\x1b[?2026h")
            self.assert_private_mode(terminal, 2026, True)

            terminal.resize_pixels(725, 437)

            self.assertEqual(terminal.winsize_full(), (80, 24, 720, 432))
            self.assert_private_mode(terminal, 2026, False)

    def test_resize_rejects_zero_dimensions_before_mutation(self):
        with Shitty(
            columns=10,
            rows=5,
            glyph_px=9,
            glyph_py=18,
        ) as terminal:
            before_size = terminal.winsize_full()
            before_model = terminal.model_digest()
            before_frame = terminal.snapshot()

            with self.assertRaises(RuntimeError):
                terminal.resize(0, 5)
            with self.assertRaises(RuntimeError):
                terminal.resize(10, 0)

            self.assertEqual(terminal.winsize_full(), before_size)
            self.assertEqual(terminal.model_digest(), before_model)
            self.assertEqual(terminal.snapshot(), before_frame)

    def test_resize_reports_mode_2048_geometry(self):
        with Shitty(
            columns=80,
            rows=24,
            glyph_px=9,
            glyph_py=18,
        ) as terminal:
            terminal.write(b"\x1b[?2048h")
            terminal.read_input()

            terminal.resize(100, 40)

            self.assertEqual(
                terminal.read_input(),
                b"\x1b[48;40;100;720;900t",
            )

    def test_resize_suppresses_mode_2048_report_when_disabled(self):
        with Shitty(
            columns=80,
            rows=24,
            glyph_px=9,
            glyph_py=18,
        ) as terminal:
            terminal.resize(100, 40)
            self.assertEqual(terminal.read_input(), b"")

    def test_consuming_resize_effect_does_not_change_terminal_state(self):
        with (
            Shitty(
                columns=10,
                rows=5,
                glyph_px=9,
                glyph_py=18,
            ) as consumed,
            Shitty(
                columns=10,
                rows=5,
                glyph_px=9,
                glyph_py=18,
            ) as pending,
        ):
            consumed.write(b"\x1b[?2048h")
            pending.write(b"\x1b[?2048h")
            consumed.read_input()

            consumed.resize(20, 10)
            pending.resize(20, 10)
            consumed.read_input()

            self.assertEqual(consumed.winsize_full(), pending.winsize_full())
            self.assertEqual(consumed.model_digest(), pending.model_digest())
            self.assertEqual(consumed.snapshot(), pending.snapshot())
            self.assert_private_mode(consumed, 2048, True)
            pending.read_input()
            self.assert_private_mode(pending, 2048, True)


if __name__ == "__main__":
    unittest.main()
