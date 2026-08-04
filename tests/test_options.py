# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty, run_startup_failure


class OptionTest(unittest.TestCase):
    def test_kitty_ctrl_base_layout_is_listed_in_help(self):
        result = run_startup_failure(extra_arguments=("-help",))
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"-kittyCtrlBaseLayout", result.stdout)

    def test_shell_and_login_argv_are_built_from_selected_executable(self):
        with Shitty(extra_arguments=("-shell", "/bin/sh")) as terminal:
            self.assertEqual(terminal.launch_command(), ("/bin/sh", ["sh"]))

        with Shitty(
            extra_arguments=("-shell", "/bin/sh", "-login")
        ) as terminal:
            self.assertEqual(terminal.launch_command(), ("/bin/sh", ["-sh"]))

        with Shitty(
            extra_arguments=("-shell", "/bin/sh", "/bin/bash")
        ) as terminal:
            self.assertEqual(
                terminal.launch_command(),
                ("/bin/bash", ["bash"]),
            )

        with Shitty(
            extra_arguments=("-login", "-e", "/tmp/tool", "one", "two")
        ) as terminal:
            self.assertEqual(
                terminal.launch_command(),
                ("/tmp/tool", ["/tmp/tool", "one", "two"]),
            )

    def test_dash_e_ends_option_parsing_and_preserves_command_argv(self):
        command = (
            "probe",
            "-fontsize",
            "99",
            "+rv",
            "-unknown",
            "plain argument",
        )
        with Shitty(
            extra_arguments=("-fontsize", "18", "-e", *command)
        ) as terminal:
            self.assertEqual(terminal.options()["fontsize"], 18)
            self.assertEqual(terminal.argv()[1:], ["-e", *command])

    def test_geometry_accepts_valid_grid_and_rejects_invalid_or_overflow(self):
        with Shitty(extra_arguments=("-geometry", "7x3")) as terminal:
            options = terminal.options()
            self.assertEqual((options["columns"], options["rows"]), (7, 3))

        for value in ("0x24", "80x0", "80X24", "70000x24", "80x70000"):
            with self.subTest(value=value):
                result = run_startup_failure(
                    extra_arguments=("-geometry", value)
                )
                self.assertEqual(result.returncode, 255)
                self.assertIn(b"-geometry", result.stdout)

    def test_colors_accept_short_and_full_rgb_and_reverse_swaps_defaults(self):
        with Shitty(
            extra_arguments=(
                "-fg", "#1aF", "-bg", "010203", "-cr", "#abcdef"
            )
        ) as terminal:
            options = terminal.options()
            self.assertEqual(options["fg"], 0x11AAFF)
            self.assertEqual(options["bg"], 0x010203)
            self.assertEqual(options["cr"], 0xABCDEF)

        with Shitty(
            extra_arguments=("-fg", "#123", "-bg", "#456", "-rv")
        ) as terminal:
            options = terminal.options()
            self.assertEqual(options["fg"], 0x445566)
            self.assertEqual(options["bg"], 0x112233)
            self.assertEqual(options["cr"], options["fg"])

        for value in ("#12", "#1234", "#ggg", "#1234567"):
            with self.subTest(value=value):
                result = run_startup_failure(extra_arguments=("-fg", value))
                self.assertEqual(result.returncode, 255)
                self.assertIn(b"-fg", result.stdout)

    def test_boolean_minus_plus_and_advanced_values(self):
        with Shitty(
            extra_arguments=(
                "-altScroll", "+boldColors", "-autoCopy",
                "-allowOsc52Read", "true",
                "-allowWindowOps", "true",
            )
        ) as terminal:
            options = terminal.options()
            self.assertEqual(options["alt_scroll"], 1)
            self.assertEqual(options["bold_colors"], 0)
            self.assertEqual(options["auto_copy"], 1)
            self.assertEqual(options["allow_osc52_read"], 1)
            self.assertEqual(options["allow_window_ops"], 1)

        result = run_startup_failure(
            extra_arguments=("-allowOsc52Read", "yes")
        )
        self.assertEqual(result.returncode, 255)
        self.assertIn(b"expected true or false", result.stdout)

    def test_unique_abbreviations_work_and_ambiguous_ones_fail(self):
        with Shitty(
            extra_arguments=("-fontsi", "19", "-geom", "7x3", "-altS")
        ) as terminal:
            options = terminal.options()
            self.assertEqual(options["fontsize"], 19)
            self.assertEqual((options["columns"], options["rows"]), (7, 3))
            self.assertEqual(options["alt_scroll"], 1)

        for abbreviation in ("-a", "-b", "-f"):
            with self.subTest(abbreviation=abbreviation):
                result = run_startup_failure(
                    extra_arguments=(abbreviation,)
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"ambiguous option", result.stderr)

    def test_fontpath_option_is_removed(self):
        result = run_startup_failure(
            extra_arguments=("-fontpath", "/tmp/fonts")
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"unknown option", result.stderr)

    def test_font_size_accepts_inclusive_one_and_255_boundaries(self):
        for value in (1, 255):
            with self.subTest(source="cli", value=value):
                with Shitty(
                    extra_arguments=("-fontsize", str(value))
                ) as terminal:
                    self.assertEqual(terminal.options()["fontsize"], value)
            with self.subTest(source="env", value=value):
                with Shitty(font_size_env=str(value)) as terminal:
                    self.assertEqual(terminal.options()["fontsize"], value)

    def test_font_size_rejects_values_outside_byte_range(self):
        for value in ("0", "256", "-1"):
            with self.subTest(source="cli", value=value):
                result = run_startup_failure(
                    extra_arguments=("-fontsize", value)
                )
                self.assertEqual(result.returncode, 255)
                self.assertIn(b"expected integer within 1..255", result.stdout)
            with self.subTest(source="env", value=value):
                result = run_startup_failure(font_size_env=value)
                self.assertEqual(result.returncode, 255)
                self.assertIn(b"expected integer within 1..255", result.stdout)

    def test_numeric_options_reject_trailing_garbage(self):
        cases = (
            ("fontsize", ("-fontsize", "16wat")),
            ("border", ("-border", "2px")),
            ("saveLines", ("-saveLines", "500rows")),
            ("geometry", ("-geometry", "80x24junk")),
            ("modifyOtherKeys", ("-modifyOtherKeys", "1foo")),
        )
        for name, arguments in cases:
            with self.subTest(name=name):
                result = run_startup_failure(extra_arguments=arguments)
                self.assertEqual(result.returncode, 255)
                self.assertIn(f"-{name}".encode(), result.stdout)

        result = run_startup_failure(font_size_env="16wat")
        self.assertEqual(result.returncode, 255)
        self.assertIn(b"SHITTY_FONT_SIZE", result.stdout)

    def test_font_size_source_priority_is_cli_then_env_then_default(self):
        with Shitty(font_size_env=None) as terminal:
            self.assertEqual(terminal.options()["fontsize"], 16)

        with Shitty(font_size_env="23") as terminal:
            self.assertEqual(terminal.options()["fontsize"], 23)

        with Shitty(
            font_size_env="23",
            extra_arguments=("-fontsize", "31"),
        ) as terminal:
            self.assertEqual(terminal.options()["fontsize"], 31)


if __name__ == "__main__":
    unittest.main()
