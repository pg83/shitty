# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


class KeyboardTest(unittest.TestCase):
    def test_large_paste_is_queued_without_blocking_control_loop(self):
        payload = b"0123456789abcdef" * 16384
        with Shitty(columns=8, rows=2) as terminal:
            terminal.paste(payload)

            received = bytearray()
            complete = False
            while not complete:
                received.extend(terminal.read_input())
                complete = terminal.flush_output_result()
            received.extend(terminal.read_input())
            self.assertEqual(received, payload)

    def test_cursor_key_normal_and_application_modes(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.key("UP")
            terminal.write(b"\x1b[?1h")
            terminal.key("UP")
            self.assertEqual(terminal.read_input(), b"\x1b[A\x1bOA")

    def test_application_keypad_encodes_numeric_key_aliases(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b=")
            terminal.key("KP_0")
            terminal.key("KP_1")
            terminal.key("KP_5")
            terminal.key("KP_9")
            terminal.key("KP_DOT")
            self.assertEqual(
                terminal.read_input(),
                b"\x1bOp\x1bOq\x1bOu\x1bOy\x1bOn",
            )

    def test_vt52_application_keypad(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?2l\x1b=")
            terminal.key("KP_1")
            terminal.key("KP_PLUS")
            self.assertEqual(terminal.read_input(), b"\x1b?q\x1b?k")

    def test_function_key_and_modified_arrow(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.key("F5")
            terminal.key("LEFT", modifiers=1)
            self.assertEqual(terminal.read_input(), b"\x1b[15~\x1b[1;2D")

    def test_alt_and_control_character_encoding(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.char("a", modifiers=4)
            terminal.char("a", modifiers=2)
            self.assertEqual(terminal.read_input(), b"\x1ba\x1b[27;5;97~")

    def test_bracketed_paste_normalizes_newlines(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?2004h")
            terminal.paste(b"one\ntwo")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[200~one\rtwo\x1b[201~",
            )

    def test_plain_paste_normalizes_newlines_without_markers(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.paste(b"\none\n\ntwo\n")
            self.assertEqual(terminal.read_input(), b"\rone\r\rtwo\r")

    def test_plain_text_drop_normalizes_newlines_without_markers(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.drop(b"one\ntwo\n")
            self.assertEqual(terminal.read_input(), b"one\rtwo\r")

    def test_bracketed_paste_mode_wraps_dropped_text(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?2004h")
            terminal.drop(b"one\ntwo")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[200~one\rtwo\x1b[201~",
            )

    def test_dropped_text_neutralizes_control_bytes(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.drop(b"a\x1bb")
            self.assertEqual(
                terminal.read_input(),
                "a␛b".encode("utf-8"),
            )

    def test_empty_drop_sends_nothing_in_bracketed_paste_mode(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?2004h")
            terminal.drop(b"")
            terminal.char("x")
            self.assertEqual(terminal.read_input(), b"x")

    def test_dropped_plain_path_is_inserted_with_separator(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.drop_path(b"/home/user/file.txt")
            self.assertEqual(terminal.read_input(), b"/home/user/file.txt ")

    def test_dropped_path_with_spaces_is_shell_quoted(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.drop_path(b"/home/user/my file.txt")
            self.assertEqual(
                terminal.read_input(),
                b"'/home/user/my file.txt' ",
            )

    def test_dropped_path_escapes_single_quotes(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.drop_path(b"/tmp/it's here")
            self.assertEqual(
                terminal.read_input(),
                b"'/tmp/it'\\''s here' ",
            )

    def test_dropped_path_respects_bracketed_paste_mode(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?2004h")
            terminal.drop_path(b"/home/user/file.txt")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[200~/home/user/file.txt \x1b[201~",
            )

    def test_keyboard_lock_discards_user_input(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[2h")
            terminal.char("x")
            terminal.key("UP")
            self.assertEqual(terminal.read_input(), b"")
            terminal.write(b"\x1b[2l")
            terminal.char("x")
            self.assertEqual(terminal.read_input(), b"x")

    def test_local_echo_renders_control_notation(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[12l")
            terminal.char(3)
            self.assertEqual(terminal.read_input(), b"\x03")
            self.assertEqual(terminal.snapshot().lines[0][:2], "^C")

    def test_newline_mode_appends_line_feed_to_return(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[20h")
            terminal.key("RETURN")
            self.assertEqual(terminal.read_input(), b"\r\n")

    def test_backarrow_key_mode_switches_backspace_byte(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.key("BACKSPACE")
            terminal.write(b"\x1b[?67h")
            terminal.key("BACKSPACE")
            self.assertEqual(terminal.read_input(), b"\x7f\x08")

    def test_kitty_key_flags_control_optional_fields(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>7u")
            self.assertEqual(terminal.state()[3], 7)
            terminal.kitty_key(ord("a"), shifted=ord("A"), modifiers=1)
            self.assertEqual(terminal.read_input(), b"\x1b[97:65;2u")

    def test_kitty_ctrl_base_layout_covers_press_repeat_and_release(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>7u")
            for key, layout_codepoint, base_codepoint in (
                ("C", 1089, 99),
                ("D", 1074, 100),
                ("A", 1092, 97),
            ):
                for action in (1, 2, 0):
                    terminal.layout_key(
                        key,
                        layout_codepoint,
                        base_codepoint,
                        modifiers=2,
                        action=action,
                    )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[99;5u\x1b[99;5:2u\x1b[99;5:3u"
                b"\x1b[100;5u\x1b[100;5:2u\x1b[100;5:3u"
                b"\x1b[97;5u\x1b[97;5:2u\x1b[97;5:3u",
            )

    def test_kitty_ctrl_base_layout_keeps_ascii_remap_primary(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>7u")
            terminal.layout_key("C", "j", "c", modifiers=2)
            self.assertEqual(terminal.read_input(), b"\x1b[106::99;5u")

    def test_kitty_ctrl_base_layout_requires_alternate_key_flag(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>3u")
            terminal.layout_key("C", "с", "c", modifiers=2)
            self.assertEqual(terminal.read_input(), b"\x1b[1089;5u")

    def test_kitty_ctrl_base_layout_preserves_lock_modifiers(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>7u")
            terminal.layout_key("B", "и", "b", modifiers=2 | 32)
            terminal.layout_key("B", "и", "b", modifiers=2 | 32, action=0)
            terminal.layout_key("B", "и", "b", modifiers=2 | 16)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[98;133u\x1b[98;133:3u\x1b[98;69u",
            )

    def test_kitty_ctrl_base_layout_preserves_other_modifiers(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>7u")
            terminal.layout_key("B", "и", "b", modifiers=2 | 1)
            terminal.layout_key("B", "и", "b", modifiers=2 | 4)
            terminal.layout_key("B", "и", "b", modifiers=2 | 8)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[98;6u\x1b[98;7u\x1b[98;13u",
            )

    def test_kitty_ctrl_base_layout_requires_control(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>7u")
            terminal.layout_key("C", "с", "c", modifiers=4)
            self.assertEqual(terminal.read_input(), b"\x1b[1089::99;3u")

    def test_kitty_ctrl_base_layout_requires_printable_ascii_base(self):
        with Shitty(
            columns=8,
            rows=2,
            extra_arguments=("-kittyCtrlBaseLayout",),
        ) as terminal:
            terminal.write(b"\x1b[>7u")
            for base in (0, 0x1F, 0x7F, 0x80):
                terminal.layout_key("C", "с", base, modifiers=2)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[1089;5u"
                b"\x1b[1089::31;5u"
                b"\x1b[1089::127;5u"
                b"\x1b[1089::128;5u",
            )

    def test_kitty_supports_all_defined_enhancement_flags(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>31u\x1b[?u")
            self.assertEqual(terminal.state()[3], 31)
            self.assertEqual(terminal.read_input(), b"\x1b[?31u")

    def test_kitty_associated_text_is_reported_for_text_keys(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>28u")
            terminal.kitty_key(ord("a"))
            terminal.kitty_key(ord("a"), shifted=ord("A"), modifiers=1)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[97;;97u\x1b[97:65;2;65u",
            )

    def test_kitty_reports_modifier_keys_with_all_keys_flag(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>10u")
            terminal.kitty_special("LEFT_SHIFT", modifiers=1, event=1)
            terminal.kitty_special("LEFT_SHIFT", event=3)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[57441;2u\x1b[57441;1:3u",
            )

    def test_kitty_keyboard_stack_is_screen_local(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>3u")
            self.assertEqual(terminal.state()[3], 3)
            terminal.write(b"\x1b[?1049h")
            self.assertEqual(terminal.state()[3], 0)
            terminal.write(b"\x1b[>5u")
            self.assertEqual(terminal.state()[3], 5)
            terminal.write(b"\x1b[?1049l")
            self.assertEqual(terminal.state()[3], 3)

    def test_kitty_push_pop_set_and_query(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(
                b"\x1b[>1u"
                b"\x1b[=6;2u"
                b"\x1b[?u"
                b"\x1b[=2;3u"
                b"\x1b[?u"
                b"\x1b[<u"
                b"\x1b[?u"
            )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[?7u\x1b[?5u\x1b[?0u",
            )

    def test_kitty_functional_key_encodings(self):
        expected = {
            "F1": b"\x1b[P",
            "F2": b"\x1b[Q",
            "F3": b"\x1b[13~",
            "F4": b"\x1b[S",
            "F5": b"\x1b[15~",
            "F13": b"\x1b[57376u",
            "KP_0": b"\x1b[57399u",
            "KP_ENTER": b"\x1b[57414u",
            "CAPS_LOCK": b"\x1b[57358u",
            "MENU": b"\x1b[57363u",
        }
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>1u")
            for name, encoded in expected.items():
                with self.subTest(name=name):
                    terminal.kitty_special(name)
                    self.assertEqual(terminal.read_input(), encoded)

    def test_kitty_disambiguates_enter_tab_and_backspace(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>1u")
            terminal.kitty_special("RETURN")
            terminal.kitty_special("TAB")
            terminal.kitty_special("BACKSPACE")
            self.assertEqual(
                terminal.read_input(),
                b"\r\t\x7f",
            )

    def test_kitty_event_types_include_release_when_requested(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>2u")
            terminal.kitty_special("UP", modifiers=5, event=1)
            terminal.kitty_special("UP", modifiers=5, event=2)
            terminal.kitty_special("UP", modifiers=5, event=3)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[1;6A\x1b[1;6:2A\x1b[1;6:3A",
            )

    def test_modify_other_keys_levels(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>4;0m")
            terminal.char("a", modifiers=1)
            self.assertEqual(terminal.read_input(), b"a")
            terminal.write(b"\x1b[>4;2m")
            terminal.char("a", modifiers=1)
            self.assertEqual(terminal.read_input(), b"\x1b[27;2;97~")

    def test_xtmodkeys_sets_queries_and_resets_every_resource(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(
                b"\x1b[>0;4m\x1b[>1;0m\x1b[>2;0m"
                b"\x1b[>3;4m\x1b[>4;2m\x1b[>6;3m\x1b[>7;1m"
                b"\x1b[?0m\x1b[?1m\x1b[?2m\x1b[?3m"
                b"\x1b[?4m\x1b[?6m\x1b[?7m"
            )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[>0;4m\x1b[>1;0m\x1b[>2;0m"
                b"\x1b[>3;4m\x1b[>4;2m\x1b[>6;3m\x1b[>7;1m",
            )
            terminal.key("LEFT", modifiers=1)
            terminal.key("F5", modifiers=4)
            self.assertEqual(terminal.read_input(), b"\x1b[D\x1b[15~")

            terminal.write(b"\x1b[>1m\x1b[?1m\x1b[>m\x1b[?4m")
            self.assertEqual(
                terminal.read_input(), b"\x1b[>1;2m\x1b[>4;1m"
            )

    def test_dec_user_defined_keys_program_preserve_and_lock(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1bP0;1|17/6869\x1b\\")
            terminal.key("F6")
            self.assertEqual(terminal.read_input(), b"hi")

            terminal.write(b"\x1bP1;0|18/58\x1b\\")
            terminal.write(b"\x1bP1;1|17/6e6f\x1b\\")
            terminal.key("F6")
            terminal.key("F7")
            self.assertEqual(terminal.read_input(), b"hiX")


if __name__ == "__main__":
    unittest.main()
