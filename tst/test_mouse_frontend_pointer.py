# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class MouseFrontendPointerTest(unittest.TestCase):
    def test_vt200_reports_left_middle_right_press_and_release(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            for button, code in ((0, 0), (2, 1), (1, 2)):
                terminal.button(button, True, x=5, y=4)
                terminal.button(button, False, x=5, y=4)
                self.assertEqual(
                    terminal.read_input(),
                    f"\x1b[<{code};4;3M\x1b[<{code};4;3m".encode(),
                )

    def test_x10_reports_only_press_and_strips_modifiers(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?9h\x1b[?1006h")
            terminal.button(0, True, modifiers=6, x=5, y=4)
            terminal.button(0, False, modifiers=6, x=5, y=4)
            self.assertEqual(terminal.read_input(), b"\x1b[<0;4;3M")

    def test_extended_buttons_have_bounded_press_only_protocol(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            for button in range(3, 8):
                terminal.button(button, True)
                terminal.button(button, False)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<128;1;1M\x1b[<129;1;1M"
                b"\x1b[<130;1;1M\x1b[<131;1;1M",
            )

    def test_button_reporting_encodes_all_modifiers(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.button(0, True, modifiers=6, x=5, y=4)
            self.assertEqual(terminal.read_input(), b"\x1b[<24;4;3M")

    def test_button_event_motion_requires_a_primary_button_and_deduplicates(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1002h\x1b[?1006h")
            terminal.pointer(5, 4)
            self.assertEqual(terminal.read_input(), b"")
            terminal.button(0, True, x=5, y=4)
            terminal.pointer(5, 4)
            terminal.pointer(5.4, 4.4)
            terminal.pointer(6, 4)
            terminal.button(0, False, x=6, y=4)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<0;4;3M\x1b[<32;4;3M"
                b"\x1b[<32;5;3M\x1b[<0;5;3m",
            )

    def test_any_event_motion_reports_without_buttons_and_with_modifiers(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1003h\x1b[?1006h")
            terminal.pointer(5, 4, modifiers=6)
            terminal.pointer(5, 4, modifiers=6)
            self.assertEqual(terminal.read_input(), b"\x1b[<59;4;3M")

    def test_tracking_mode_transition_resets_motion_deduplication(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1003h\x1b[?1006h")
            terminal.pointer(5, 4)
            terminal.write(b"\x1b[?1003l\x1b[?1003h")
            terminal.pointer(5.4, 4.4)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<35;4;3M\x1b[<35;4;3M",
            )

    def test_sgr_pixel_motion_deduplicates_pixels_not_cells(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1003h\x1b[?1016h")
            terminal.pointer(5, 4)
            terminal.pointer(5.4, 4)
            terminal.pointer(6, 4)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<35;4;3M\x1b[<35;5;3M",
            )

    def test_content_scale_converts_logical_pointer_to_framebuffer_pixels(self):
        with Shitty(columns=16, rows=8) as terminal:
            terminal.write(b"\x1b[?1003h\x1b[?1016h")
            terminal.pointer(3, 2, scale_x=2, scale_y=2)
            self.assertEqual(terminal.read_input(), b"\x1b[<35;5;3M")

    def test_shift_override_drags_a_local_selection_during_mouse_reporting(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"one two\x1b[?1000h\x1b[?1006h")
            terminal.button(0, True, x=2, y=2, modifiers=1, time=1)
            terminal.pointer(5, 2, modifiers=1)
            self.assertEqual(
                terminal.button(
                    0, False, x=5, y=2, modifiers=1, time=1.01
                ),
                b"one",
            )

            terminal.button(0, True, x=8, y=2, modifiers=1, time=2)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 6, 0))
            terminal.pointer(9, 2, modifiers=1)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 7, 0))
            self.assertEqual(
                terminal.button(
                    0, False, x=9, y=2, modifiers=1, time=2.01
                ),
                b"one two",
            )
            self.assertEqual(terminal.read_input(), b"")

    def test_shift_primary_extends_left_endpoint_and_continues_drag(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"abcdefgh\x1b[?1000h\x1b[?1006h")
            terminal.button(0, True, x=5, y=2, modifiers=1, time=1)
            terminal.pointer(8, 2, modifiers=1)
            self.assertEqual(
                terminal.button(
                    0, False, x=8, y=2, modifiers=1, time=1.01
                ),
                b"def",
            )

            terminal.button(0, True, x=3, y=2, modifiers=1, time=2)
            self.assertEqual(terminal.snapshot().selection, (1, 0, 6, 0))
            terminal.pointer(2, 2, modifiers=1)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 6, 0))
            self.assertEqual(
                terminal.button(
                    0, False, x=2, y=2, modifiers=1, time=2.01
                ),
                b"abcdef",
            )
            self.assertEqual(terminal.read_input(), b"")

    def test_double_and_triple_click_cycle_word_and_line_selection(self):
        with Shitty(columns=12, rows=3) as terminal:
            terminal.write(b"one two")
            terminal.button(0, True, x=6, y=2, time=1.0)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.01), b""
            )
            terminal.button(0, True, x=6, y=2, time=1.1)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.11), b"two"
            )
            terminal.button(0, True, x=6, y=2, time=1.2)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.21), b"one two"
            )
            terminal.button(0, True, x=6, y=2, time=1.3)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.31), b""
            )

    def test_shift_primary_extends_completed_word_selection_with_drag(self):
        with Shitty(columns=16, rows=3) as terminal:
            terminal.write(b"one two three")
            terminal.button(0, True, x=6, y=2, time=1)
            terminal.button(0, False, x=6, y=2, time=1.01)
            terminal.button(0, True, x=6, y=2, time=1.1)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.11),
                b"two",
            )
            self.assertEqual(terminal.snapshot().selection, (4, 0, 4, 0))

            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.button(0, True, x=9, y=2, modifiers=1, time=2)
            terminal.pointer(10, 2, modifiers=1)
            self.assertEqual(
                terminal.button(
                    0, False, x=10, y=2, modifiers=1, time=2.01
                ),
                b"two three",
            )
            self.assertEqual(terminal.read_input(), b"")

    def test_shift_primary_extends_completed_line_selection_with_drag(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(
                b"first\x1b[2;1Hsecond\x1b[3;1Hthird\x1b[4;1Hfourth"
            )
            terminal.button(0, True, x=2, y=3, time=1)
            terminal.button(0, False, x=2, y=3, time=1.01)
            terminal.button(0, True, x=2, y=3, time=1.1)
            terminal.button(0, False, x=2, y=3, time=1.11)
            terminal.button(0, True, x=2, y=3, time=1.2)
            self.assertEqual(
                terminal.button(0, False, x=2, y=3, time=1.21),
                b"second",
            )
            self.assertEqual(terminal.snapshot().selection, (0, 1, 0, 1))

            terminal.write(b"\x1b[?1000h\x1b[?1006h")
            terminal.button(0, True, x=2, y=4, modifiers=1, time=2)
            terminal.pointer(2, 5, modifiers=1)
            self.assertEqual(
                terminal.button(
                    0, False, x=2, y=5, modifiers=1, time=2.01
                ),
                b"second\nthird\nfourth",
            )
            self.assertEqual(terminal.read_input(), b"")

    def test_selection_drag_finishes_after_pointer_leaves_window(self):
        # On the defaults, panes included. T8 pinned +panes here around
        # the defect F-panes has since fixed - pointerPresence(false)
        # dropped the press grab, so the release arrived with no held
        # terminal and the drag was never finished. Unpinned again on
        # purpose: panes are on by default, so this is now the path
        # nearly every user's drag takes, and a pin would hide exactly
        # the code that carries it.
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"abcdefgh")
            terminal.button(0, True, x=2, y=2, time=1)
            terminal.pointer(x=5, y=2)
            terminal.pointer_presence(False)
            terminal.pointer(x=7, y=2)

            self.assertEqual(
                terminal.button(0, False, x=7, y=2, time=1.01),
                b"abcde",
            )
            completed = terminal.snapshot().selection

            terminal.pointer_presence(True)
            terminal.pointer(x=9, y=2)

            self.assertEqual(terminal.snapshot().selection, completed)

    def test_click_timeout_and_distance_restart_at_character_selection(self):
        with Shitty(columns=12, rows=3) as terminal:
            terminal.write(b"one two")
            terminal.button(0, True, x=6, y=2, time=1.0)
            terminal.button(0, False, x=6, y=2, time=1.01)
            terminal.button(0, True, x=6, y=2, time=1.6)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.61), b""
            )
            terminal.button(0, True, x=1, y=2, time=1.7)
            self.assertEqual(
                terminal.button(0, False, x=1, y=2, time=1.71), b""
            )

    def test_click_thresholds_are_inclusive_and_clock_rewind_resets(self):
        with Shitty(columns=12, rows=3) as terminal:
            terminal.write(b"one two")
            terminal.button(0, True, x=6, y=2, time=1)
            terminal.button(0, False, x=6, y=2, time=1.01)
            terminal.button(0, True, x=2, y=2, time=1.5)
            self.assertEqual(
                terminal.button(0, False, x=2, y=2, time=1.51), b"one two"
            )
            terminal.button(0, True, x=2, y=2, time=1)
            self.assertEqual(
                terminal.button(0, False, x=2, y=2, time=1.01), b""
            )

    def test_pointer_updates_dec_locator_position(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[1;2'z")
            terminal.pointer(5, 4)
            terminal.write(b"\x1b['|")
            self.assertEqual(terminal.read_input(), b"\x1b[1;0;3;4;0&w")

    def test_highlight_release_uses_frontend_button_coordinates(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(
                b"\x1b[?1001h\x1b[?1006h\x1b[1;2;1;1;4T"
            )
            terminal.button(0, True, x=5, y=4)
            terminal.button(0, False, x=5, y=4)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<0;4;3M\x1b[<2;1;4;3;4;3T",
            )

    def test_right_button_extends_selection_and_middle_button_pastes_it(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"abcd")
            terminal.button(0, True, x=2, y=2, time=1)
            terminal.button(0, False, x=2, y=2, time=1.01)
            terminal.button(1, True, x=5, y=2, time=2)
            self.assertEqual(
                terminal.button(1, False, x=5, y=2, time=2.01), b"abc"
            )
            terminal.button(2, True, x=4, y=2, time=3)
            terminal.button(2, False, x=4, y=2, time=3.01)
            self.assertEqual(terminal.read_input(), b"abc")


if __name__ == "__main__":
    unittest.main()
