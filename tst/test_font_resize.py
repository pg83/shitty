# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty, TEST_PLATFORM


RELEASE = 0
PRESS = 1
KEY_0 = 48
KEY_A = 65
KEY_EQUAL = 61
KEY_MINUS = 45
MOD_SHIFT = 1
MOD_CONTROL = 2
MOD_SUPER = 8

# The chords that do *not* bind here - the half of this knowledge that
# belongs to a test rather than to the harness. The harness presses what
# works on this platform; this list asserts that the other platform's
# chords do nothing on it.
if TEST_PLATFORM == "cocoa":
    INACTIVE_FONT_CHORDS = (
        (KEY_EQUAL, MOD_CONTROL | MOD_SHIFT),
        (KEY_MINUS, MOD_CONTROL),
        (KEY_0, MOD_CONTROL),
    )
else:
    INACTIVE_FONT_CHORDS = (
        (KEY_EQUAL, MOD_SUPER),
        (KEY_EQUAL, MOD_SUPER | MOD_SHIFT),
        (KEY_MINUS, MOD_SUPER),
        (KEY_0, MOD_SUPER),
    )


class FontResizeTest(unittest.TestCase):
    @staticmethod
    def shortcut(terminal, key, text, modifiers):
        terminal.frontend_key_event(key, PRESS, modifiers=modifiers)
        if text is not None:
            terminal.frontend_text_event(text, modifiers=modifiers)
        terminal.frontend_key_event(key, RELEASE, modifiers=modifiers)

    def test_font_bindings_resize_and_reset_without_reaching_child(self):
        with Shitty(
            columns=40,
            rows=8,
            save_lines=8,
            glyph_px=8,
            glyph_py=16,
            extra_arguments=("-fontsize", "16"),
        ) as terminal:
            terminal.write(b"one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix")
            terminal.wheel_up(1)
            terminal.select_start(0, 0)
            terminal.select_update(3, 0)
            before = terminal.snapshot()
            initial = terminal.font_state()
            horizontal_padding = initial[3] - initial[5] * initial[1]
            vertical_padding = initial[4] - initial[6] * initial[2]

            terminal.chord_font_increase()
            increased = terminal.font_state()

            self.assertEqual(increased[0], 17)
            self.assertEqual(increased[5:7], initial[5:7])
            self.assertEqual(
                increased[3],
                horizontal_padding + increased[5] * increased[1],
            )
            self.assertEqual(
                increased[4],
                vertical_padding + increased[6] * increased[2],
            )
            self.assertNotEqual(increased[3:5], initial[3:5])
            self.assertEqual(terminal.winsize(), increased[5:7])
            self.assertEqual(terminal.read_input(), b"")
            self.assertGreater(terminal.snapshot().refresh_count, before.refresh_count)
            self.assertEqual(terminal.select_finish(), b"one")

            terminal.chord_font_decrease()
            self.assertEqual(terminal.font_state()[0], 16)
            terminal.chord_font_increase()
            terminal.chord_font_increase()
            self.assertEqual(terminal.font_state()[0], 18)
            terminal.chord_font_reset()
            self.assertEqual(terminal.font_state()[0], 16)
            self.assertEqual(terminal.read_input(), b"")

            terminal.frontend_key_event(
                KEY_A,
                PRESS,
                modifiers=MOD_CONTROL,
            )
            terminal.frontend_key_event(
                KEY_A,
                RELEASE,
                modifiers=MOD_CONTROL,
            )
            self.assertEqual(terminal.read_input(), b"\x01")

    def test_inactive_font_chords_are_inert(self):
        with Shitty(
            columns=40,
            rows=8,
            save_lines=8,
            glyph_px=8,
            glyph_py=16,
            extra_arguments=("-fontsize", "16"),
        ) as terminal:
            for key, modifiers in INACTIVE_FONT_CHORDS:
                self.shortcut(terminal, key, None, modifiers)
                self.assertEqual(terminal.font_state()[0], 16)
                terminal.read_input()

    def test_font_change_in_an_immovable_window_keeps_pixels(self):
        # A tiled or maximized window is not ours to resize (issues 38,
        # 46): the pixels stay and the grid reflows over them in one
        # step - no self-resize bounce against the compositor.
        for state in ({"tiled": True}, {"maximized": True}):
            with self.subTest(state=state):
                with Shitty(
                    columns=40,
                    rows=8,
                    glyph_px=8,
                    glyph_py=16,
                    extra_arguments=("-fontsize", "16"),
                ) as terminal:
                    terminal.write(b"visible contents")
                    terminal.window_info(**state)
                    initial = terminal.font_state()

                    terminal.chord_font_increase()
                    increased = terminal.font_state()

                    self.assertEqual(increased[0], 17)
                    self.assertEqual(increased[3:5], initial[3:5])
                    border = increased[8]
                    self.assertEqual(
                        increased[5],
                        max(1, (increased[3] - 2 * border) // increased[1]),
                    )
                    self.assertEqual(
                        increased[6],
                        max(1, (increased[4] - 2 * border) // increased[2]),
                    )
                    self.assertEqual(terminal.winsize(), increased[5:7])

                    terminal.chord_font_decrease()
                    restored = terminal.font_state()
                    self.assertEqual(restored[0], 16)
                    # The real fontpack replaced the test one, so the grid
                    # follows its metrics; the pixels still never move.
                    self.assertEqual(restored[3:5], initial[3:5])
                    self.assertEqual(
                        restored[5],
                        max(1, (restored[3] - 2 * border) // restored[1]),
                    )
                    self.assertEqual(
                        restored[6],
                        max(1, (restored[4] - 2 * border) // restored[2]),
                    )
                    self.assertEqual(terminal.winsize(), restored[5:7])

    def test_font_change_rematerializes_every_visible_cell(self):
        with Shitty(
            columns=40,
            rows=8,
            glyph_px=8,
            glyph_py=16,
            extra_arguments=("-fontsize", "16"),
        ) as terminal:
            terminal.write(b"visible contents")
            terminal.snapshot()

            terminal.chord_font_increase()

            cells, spans = terminal.last_update()
            self.assertEqual(cells, 40 * 8)
            self.assertEqual(spans, 8)

    def test_font_change_reshapes_a_resized_screen(self):
        # Issue 51: a screen rebuilt through a resize must keep hearing
        # font changes; a deaf one serves spans shaped with the old
        # metrics and stale strip-cache hits forever after.
        with Shitty(
            columns=40,
            rows=8,
            glyph_px=8,
            glyph_py=16,
            extra_arguments=("-fontsize", "16"),
        ) as terminal:
            terminal.write(b"before resize")
            fresh = terminal.shape_generation()
            terminal.chord_font_increase()
            heard = terminal.shape_generation()
            self.assertNotEqual(heard, fresh)

            terminal.resize(50, 10)
            terminal.write(b" after resize")
            resized = terminal.shape_generation()
            terminal.chord_font_increase()
            self.assertNotEqual(terminal.shape_generation(), resized)


if __name__ == "__main__":
    unittest.main()
