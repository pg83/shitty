# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import TEST_PLATFORM, Shitty


RELEASE = 0
PRESS = 1
REPEAT = 2

SHIFT = 1
CONTROL = 2
ALT = 4
SUPER = 8
CAPS_LOCK = 16
NUM_LOCK = 32

KEY_ESCAPE = 256
KEY_ENTER = 257
KEY_TAB = 258
KEY_BACKSPACE = 259
KEY_INSERT = 260
KEY_DELETE = 261
KEY_RIGHT = 262
KEY_LEFT = 263
KEY_DOWN = 264
KEY_UP = 265
KEY_PAGE_UP = 266
KEY_PAGE_DOWN = 267
KEY_HOME = 268
KEY_END = 269
KEY_F1 = 290
KEY_F5 = 294
KEY_KP_0 = 320
KEY_KP_5 = 325
KEY_KP_DECIMAL = 330
KEY_KP_DIVIDE = 331
KEY_KP_MULTIPLY = 332
KEY_KP_SUBTRACT = 333
KEY_KP_ADD = 334
KEY_KP_ENTER = 335
KEY_KP_EQUAL = 336


class ContourInputGeneratorTest(unittest.TestCase):
    def test_control_character_matrix_and_lock_state(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>4;0m")
            for character, shifted, expected in (
                (" ", False, b"\x00"),
                ("[", False, b"\x1b"),
                ("\\", False, b"\x1c"),
                ("]", False, b"\x1d"),
                ("6", False, b"\x1e"),
                ("-", True, b"\x1f"),
            ):
                with self.subTest(character=character):
                    terminal.frontend_control(
                        character, shifted=shifted
                    )
                    self.assertEqual(terminal.read_input(), expected)

            for character in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
                for locks in (0, CAPS_LOCK, NUM_LOCK,
                              CAPS_LOCK | NUM_LOCK):
                    with self.subTest(character=character, locks=locks):
                        terminal.frontend_key_event(
                            ord(character), PRESS,
                            modifiers=CONTROL | locks,
                        )
                        self.assertEqual(
                            terminal.read_input(),
                            bytes((ord(character) - ord("A") + 1,)),
                        )

    def test_legacy_arrow_modifier_matrix(self):
        directions = {
            KEY_UP: b"A",
            KEY_DOWN: b"B",
            KEY_RIGHT: b"C",
            KEY_LEFT: b"D",
        }
        modifier_codes = {
            0: None,
            SHIFT: 2,
            ALT: 3,
            SHIFT | ALT: 4,
            CONTROL: 5,
            SHIFT | CONTROL: 6,
            ALT | CONTROL: 7,
            SHIFT | ALT | CONTROL: 8,
            SUPER: 9,
            SUPER | SHIFT: 10,
            SUPER | ALT: 11,
            SUPER | ALT | SHIFT: 12,
            SUPER | CONTROL: 13,
            SUPER | CONTROL | SHIFT: 14,
            SUPER | CONTROL | ALT: 15,
            SUPER | CONTROL | ALT | SHIFT: 16,
        }
        with Shitty(columns=8, rows=2) as terminal:
            for key, final in directions.items():
                for modifiers, code in modifier_codes.items():
                    with self.subTest(
                        key=key, modifiers=modifiers
                    ):
                        if (
                            TEST_PLATFORM == "cocoa"
                            and modifiers == SUPER
                            and key in (KEY_RIGHT, KEY_LEFT)
                        ):
                            # Cmd+Left/Right walk the tabs on macOS
                            # (the issue 82 reservation); the chord is
                            # the frontend's and never reaches the pty.
                            continue
                        if (
                            TEST_PLATFORM == "cocoa"
                            and modifiers == SUPER | ALT
                        ):
                            # Cmd+Opt+arrows move between the panes of a
                            # split on macOS; like the chord above they
                            # are the frontend's and never reach the pty.
                            continue
                        terminal.frontend_key_event(
                            key, PRESS, modifiers=modifiers
                        )
                        expected = (
                            b"\x1b[" + final
                            if code is None
                            else b"\x1b[1;" + str(code).encode() + final
                        )
                        self.assertEqual(
                            terminal.read_input(), expected
                        )

    def test_lock_state_is_ignored_by_legacy_function_keys(self):
        keys = {
            KEY_UP: b"\x1b[A",
            KEY_DOWN: b"\x1b[B",
            KEY_RIGHT: b"\x1b[C",
            KEY_LEFT: b"\x1b[D",
            KEY_HOME: b"\x1b[H",
            KEY_END: b"\x1b[F",
            KEY_PAGE_UP: b"\x1b[5~",
            KEY_PAGE_DOWN: b"\x1b[6~",
            KEY_INSERT: b"\x1b[2~",
            KEY_DELETE: b"\x1b[3~",
            KEY_F1: b"\x1bOP",
            KEY_F5: b"\x1b[15~",
        }
        with Shitty(columns=8, rows=2) as terminal:
            for key, expected in keys.items():
                for locks in (
                    CAPS_LOCK, NUM_LOCK, CAPS_LOCK | NUM_LOCK
                ):
                    with self.subTest(key=key, locks=locks):
                        terminal.frontend_key_event(
                            key, PRESS, modifiers=locks
                        )
                        self.assertEqual(
                            terminal.read_input(), expected
                        )

            terminal.write(b"\x1b[?1h")
            for locks in (
                CAPS_LOCK, NUM_LOCK, CAPS_LOCK | NUM_LOCK
            ):
                terminal.frontend_key_event(
                    KEY_UP, PRESS, modifiers=locks
                )
                self.assertEqual(terminal.read_input(), b"\x1bOA")

            for locks in (0, CAPS_LOCK, NUM_LOCK,
                          CAPS_LOCK | NUM_LOCK):
                terminal.frontend_key_event(
                    KEY_UP, PRESS, modifiers=CONTROL | locks
                )
                self.assertEqual(
                    terminal.read_input(), b"\x1b[1;5A"
                )

            terminal.frontend_key_event(
                KEY_UP, PRESS, modifiers=SHIFT | CAPS_LOCK
            )
            terminal.frontend_key_event(
                KEY_TAB, PRESS, modifiers=SHIFT | NUM_LOCK
            )
            self.assertEqual(
                terminal.read_input(), b"\x1b[1;2A\x1b[Z"
            )

    def test_modify_other_keys_two_ignores_locks(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>4;2m")
            for locks in (CAPS_LOCK, NUM_LOCK,
                          CAPS_LOCK | NUM_LOCK):
                with self.subTest(locks=locks):
                    terminal.frontend_key_event(
                        ord("A"), PRESS, modifiers=locks
                    )
                    terminal.frontend_text_event("a", modifiers=locks)
                    self.assertEqual(terminal.read_input(), b"a")

                    terminal.frontend_key_event(
                        ord("A"), PRESS,
                        modifiers=CONTROL | locks,
                    )
                    self.assertEqual(
                        terminal.read_input(), b"\x1b[27;5;97~"
                    )

            terminal.frontend_key_event(
                ord("A"), PRESS,
                modifiers=SHIFT | CONTROL | CAPS_LOCK,
            )
            self.assertEqual(
                terminal.read_input(), b"\x1b[27;6;97~"
            )

            terminal.frontend_key_event(
                ord("A"), PRESS, modifiers=SHIFT | NUM_LOCK
            )
            terminal.frontend_text_event(
                "a", modifiers=SHIFT | NUM_LOCK
            )
            self.assertEqual(
                terminal.read_input(), b"\x1b[27;2;97~"
            )

    def test_keypad_numlock_overrides_application_mode(self):
        keypad = {
            KEY_KP_0: b"0",
            KEY_KP_5: b"5",
            KEY_KP_DECIMAL: b".",
            KEY_KP_DIVIDE: b"/",
            KEY_KP_MULTIPLY: b"*",
            KEY_KP_SUBTRACT: b"-",
            KEY_KP_ADD: b"+",
            KEY_KP_ENTER: b"\r",
            KEY_KP_EQUAL: b"=",
        }
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b=")
            for key, expected in keypad.items():
                with self.subTest(key=key):
                    terminal.frontend_key_event(
                        key, PRESS, modifiers=NUM_LOCK
                    )
                    self.assertEqual(
                        terminal.read_input(), expected
                    )

            terminal.frontend_key_event(KEY_KP_5, PRESS)
            self.assertEqual(terminal.read_input(), b"\x1bOu")

    def test_backarrow_mode_and_reset(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.frontend_key_event(KEY_BACKSPACE, PRESS)
            terminal.frontend_key_event(
                KEY_BACKSPACE, PRESS, modifiers=CONTROL
            )
            terminal.write(b"\x1b[?67h")
            terminal.frontend_key_event(KEY_BACKSPACE, PRESS)
            terminal.frontend_key_event(
                KEY_BACKSPACE, PRESS, modifiers=CONTROL
            )
            self.assertEqual(
                terminal.read_input(), b"\x7f\x08\x08\x7f"
            )

            terminal.write(b"\x1b[?1h\x1b=\x1b[?67h\x1bc")
            terminal.frontend_key_event(KEY_UP, PRESS)
            terminal.frontend_key_event(KEY_BACKSPACE, PRESS)
            terminal.frontend_key_event(KEY_KP_5, PRESS)
            self.assertEqual(
                terminal.read_input(), b"\x1b[A\x7f5"
            )

    def test_kitty_lock_modifiers_and_event_types(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>3u")
            terminal.frontend_key_event(
                ord("A"), PRESS, modifiers=CAPS_LOCK
            )
            terminal.frontend_text_event(
                "A", modifiers=CAPS_LOCK
            )
            terminal.frontend_key_event(
                ord("A"), RELEASE, modifiers=CAPS_LOCK
            )
            self.assertEqual(
                terminal.read_input(),
                b"A\x1b[97;65:3u",
            )

            terminal.write(b"\x1b[>8u")
            terminal.frontend_key_event(
                KEY_ENTER, PRESS, modifiers=CAPS_LOCK
            )
            terminal.frontend_key_event(
                KEY_TAB, PRESS, modifiers=NUM_LOCK
            )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[13;65u\x1b[9;129u",
            )

    def test_kitty_key_fields_and_event_encoding(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>8u")
            terminal.kitty_key(ord("a"))
            terminal.kitty_special("RETURN")
            terminal.kitty_special("TAB")
            terminal.kitty_special("LEFT_SHIFT")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[97u\x1b[13u\x1b[9u\x1b[57441u",
            )

            terminal.write(b"\x1b[>3u")
            for event, suffix in (
                (1, b""),
                (2, b":2"),
                (3, b":3"),
            ):
                terminal.kitty_key(
                    ord("a"), modifiers=4, event=event
                )
                self.assertEqual(
                    terminal.read_input(),
                    b"\x1b[97;5" + suffix + b"u",
                )

            terminal.write(b"\x1b[>12u")
            terminal.kitty_key(
                ord("3"), shifted=ord("#"), modifiers=1
            )
            terminal.kitty_key(
                ord(";"), shifted=ord(":"), modifiers=1
            )
            terminal.kitty_key(ord("#"))
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[51:35;2u"
                b"\x1b[59:58;2u"
                b"\x1b[35u",
            )

    def test_kitty_disambiguation_matrix(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[=1u")
            terminal.kitty_key(ord("l"), modifiers=4)
            terminal.frontend_key_event(KEY_ESCAPE, PRESS)
            terminal.frontend_key_event(
                KEY_ESCAPE, REPEAT, modifiers=SHIFT
            )
            terminal.frontend_key_event(
                KEY_ESCAPE, RELEASE, modifiers=SHIFT
            )
            terminal.kitty_special("HOME")
            terminal.kitty_special("HOME", modifiers=4)
            terminal.kitty_special("END")
            terminal.kitty_special("END", modifiers=4)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[108;5u"
                b"\x1b[27u"
                b"\x1b[27;2u"
                b"\x1b[H\x1b[1;5H"
                b"\x1b[F\x1b[1;5F",
            )

            terminal.frontend_key_event(
                ord("A"), PRESS, modifiers=SHIFT
            )
            terminal.frontend_text_event("A", modifiers=SHIFT)
            terminal.frontend_key_event(
                ord(";"), PRESS, modifiers=SHIFT
            )
            terminal.frontend_text_event(":", modifiers=SHIFT)
            terminal.frontend_key_event(
                KEY_ENTER, PRESS, modifiers=CAPS_LOCK
            )
            terminal.frontend_key_event(
                KEY_TAB, PRESS, modifiers=NUM_LOCK
            )
            self.assertEqual(terminal.read_input(), b"A:\r\t")

            terminal.kitty_special(
                "RETURN", modifiers=1
            )
            terminal.kitty_special("TAB", modifiers=1)
            terminal.kitty_special(
                "BACKSPACE", modifiers=4
            )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[13;2u"
                b"\x1b[9;2u"
                b"\x1b[127;5u",
            )

    def test_kitty_function_and_report_flag_matrix(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[=1u")
            for name, expected in (
                ("F1", b"\x1b[P"),
                ("F2", b"\x1b[Q"),
                ("F3", b"\x1b[13~"),
                ("F4", b"\x1b[S"),
            ):
                terminal.kitty_special(name)
                self.assertEqual(terminal.read_input(), expected)

            terminal.kitty_special("F1", modifiers=1)
            terminal.kitty_special("F3", modifiers=4)
            terminal.kitty_special("F4", modifiers=4)
            terminal.key("F1", modifiers=SHIFT)
            terminal.key("F3", modifiers=CONTROL)
            terminal.key("F4", modifiers=CONTROL)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[1;2P"
                b"\x1b[13;5~"
                b"\x1b[1;5S"
                b"\x1b[1;2P"
                b"\x1b[1;5R"
                b"\x1b[1;5S",
            )

            terminal.write(b"\x1b[=8u")
            terminal.kitty_key(ord("a"))
            terminal.kitty_special("RETURN")
            terminal.kitty_special("TAB")
            terminal.kitty_special("LEFT_SHIFT")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[97u"
                b"\x1b[13u"
                b"\x1b[9u"
                b"\x1b[57441u",
            )

            terminal.kitty_key(
                ord("a"), modifiers=64
            )
            terminal.kitty_key(
                ord("5"), modifiers=128
            )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[97;65u"
                b"\x1b[53;129u",
            )

    @unittest.expectedFailure
    def test_contour_legacy_control_f3_uses_13_tilde(self):
        # Contour is in the 4-vote minority for modified top-row F3;
        # Alacritty, xterm, iTerm2, VTE and foot use CSI 1;mod R.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.key("F3", modifiers=CONTROL)
            self.assertEqual(terminal.read_input(), b"\x1b[13;5~")

    @unittest.expectedFailure
    def test_kitty_report_event_types_encodes_plain_repeat(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[=2u")
            terminal.frontend_key_event(ord("A"), PRESS)
            terminal.frontend_text_event("a")
            terminal.frontend_key_event(ord("A"), REPEAT)
            terminal.frontend_text_event("a")
            terminal.frontend_key_event(ord("A"), RELEASE)
            self.assertEqual(
                terminal.read_input(),
                b"a"
                b"\x1b[97;1:2u"
                b"\x1b[97;1:3u",
            )

    def test_kitty_keypad_associated_text(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>17u")
            terminal.kitty_special(
                "KP_5", modifiers=128
            )
            terminal.kitty_special("KP_5")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[57404;129;53u"
                b"\x1b[57404;;53u",
            )

    def test_legacy_multi_modifier_characters(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>4;0m")
            terminal.char("A", modifiers=SHIFT | ALT)
            terminal.frontend_control("A", alt=True)
            terminal.frontend_key_event(
                KEY_TAB, PRESS, modifiers=SHIFT | ALT
            )
            terminal.frontend_key_event(
                KEY_TAB, PRESS, modifiers=SHIFT | CONTROL
            )
            terminal.frontend_key_event(
                KEY_TAB, PRESS, modifiers=CONTROL | ALT
            )
            terminal.frontend_key_event(
                KEY_ESCAPE, PRESS, modifiers=ALT
            )
            terminal.frontend_control("/")
            terminal.frontend_control("2")
            self.assertEqual(
                terminal.read_input(),
                b"\x1bA"
                b"\x1b\x01"
                b"\x1b\x1b[Z"
                b"\x1b[Z"
                b"\x1b\t"
                b"\x1b\x1b"
                b"\x1f"
                b"\x00",
            )


if __name__ == "__main__":
    unittest.main()
