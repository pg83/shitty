# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import os
import signal
import tempfile
import time
import unittest
from pathlib import Path

from harness import Shitty, run_startup_failure


class ColorSchemeTest(unittest.TestCase):
    def test_named_scheme_sets_defaults_and_ansi_palette(self):
        with Shitty(
            pin_vga=False, extra_arguments=("-colorScheme", "3024 Night")
        ) as terminal:
            options = terminal.options()
            self.assertEqual(options["fg"], 0xA5A2A2)
            self.assertEqual(options["bg"], 0x090300)
            self.assertEqual(options["cr"], options["fg"])

            terminal.write(b"\x1b]4;1;?\x1b\\")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b]4;1;rgb:dbdb/2d2d/2020\x1b\\",
            )

    def test_explicit_colors_override_a_named_scheme(self):
        with Shitty(
            pin_vga=False, extra_arguments=(
                "-colorScheme", "3024 Night",
                "-fg", "#010203",
                "-color1", "#040506",
            )
        ) as terminal:
            self.assertEqual(terminal.options()["fg"], 0x010203)
            terminal.write(b"\x1b]4;1;?\x1b\\")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b]4;1;rgb:0404/0505/0606\x1b\\",
            )

    def test_cli_scheme_replaces_lower_priority_config_colors(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "shitty.toml"
            config.write_text(
                'colorScheme = "Nord"\n'
                'fg = "#010203"\n'
                'color1 = "#040506"\n'
            )
            with Shitty(
                pin_vga=False, extra_arguments=(
                    "-config", config,
                    "-colorScheme", "3024 Night",
                )
            ) as terminal:
                self.assertEqual(terminal.options()["fg"], 0xA5A2A2)
                terminal.write(b"\x1b]4;1;?\x1b\\")
                self.assertEqual(
                    terminal.read_input(),
                    b"\x1b]4;1;rgb:dbdb/2d2d/2020\x1b\\",
                )

    def test_scheme_names_are_case_insensitive_and_listed(self):
        with Shitty(
            pin_vga=False, extra_arguments=("-colorScheme", "3024 night")
        ) as terminal:
            self.assertEqual(terminal.options()["bg"], 0x090300)

        result = run_startup_failure(extra_arguments=("-listColorSchemes",))
        self.assertEqual(result.returncode, 0)
        names = result.stdout.decode().splitlines()
        self.assertGreaterEqual(len(names), 500)
        self.assertEqual(names[0], "default")
        self.assertIn("kitty", names)
        self.assertIn("classic", names)
        self.assertIn("3024 Night", names)
        self.assertIn("Nord", names)

    def test_classic_scheme_restores_the_pre_brand_defaults(self):
        with Shitty(
            pin_vga=False, extra_arguments=("-colorScheme", "classic")
        ) as terminal:
            options = terminal.options()
            self.assertEqual(options["fg"], 0xFFFFFF)
            self.assertEqual(options["bg"], 0x000000)
            terminal.write(b"\x1b]4;1;?;12;?\x1b\\")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b]4;1;rgb:cdcd/0000/0000\x1b\\"
                b"\x1b]4;12;rgb:5c5c/5c5c/ffff\x1b\\",
            )

    def test_the_default_scheme_is_catppuccin_mocha(self):
        # T8 moved the default off the built-in "default" (Retro Legends
        # on pure black) and onto a named scheme. Both halves are read
        # back: fg/bg come from the scheme's own two fields, the palette
        # query from its ansi list, and a scheme wired up by only one of
        # the two would pass on either half alone.
        with Shitty(pin_vga=False) as terminal:
            options = terminal.options()
            self.assertEqual(options["fg"], 0xCDD6F4)
            self.assertEqual(options["bg"], 0x1E1E2E)
            self.assertEqual(options["cr"], options["fg"])

            terminal.write(b"\x1b]4;0;?;1;?\x1b\\")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b]4;0;rgb:4545/4747/5a5a\x1b\\"
                b"\x1b]4;1;rgb:f3f3/8b8b/a8a8\x1b\\",
            )

    def test_explicit_colors_override_the_default_scheme(self):
        with Shitty(
            pin_vga=False, extra_arguments=("-fg", "#010203", "-color1", "#040506")
        ) as terminal:
            self.assertEqual(terminal.options()["fg"], 0x010203)
            # bg was not given, so it stays the default scheme's.
            self.assertEqual(terminal.options()["bg"], 0x1E1E2E)
            terminal.write(b"\x1b]4;1;?\x1b\\")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b]4;1;rgb:0404/0505/0606\x1b\\",
            )

    def test_unknown_named_scheme_fails_with_discovery_hint(self):
        result = run_startup_failure(
            extra_arguments=("-colorScheme", "no such scheme")
        )
        self.assertEqual(result.returncode, 255)
        self.assertIn(b"unknown scheme", result.stdout)
        self.assertIn(b"-listColorSchemes", result.stdout)

    def test_private_dsr_reports_dark_and_light_configured_schemes(self):
        cases = (
            ((), b"\x1b[?997;1n"),
            (("-bg", "#ffffff"), b"\x1b[?997;2n"),
        )
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                with Shitty(extra_arguments=arguments) as terminal:
                    terminal.write(b"\x1b[?996n")
                    self.assertEqual(terminal.read_input(), expected)

    def test_application_dynamic_colors_do_not_change_reported_scheme(self):
        with Shitty() as terminal:
            terminal.write(
                b"\x1b[?2031h"
                b"\x1b]10;#000000\x1b\\"
                b"\x1b]11;#ffffff\x1b\\"
            )
            self.assertEqual(terminal.read_input(), b"")

            terminal.write(b"\x1b[?996n")
            self.assertEqual(terminal.read_input(), b"\x1b[?997;1n")

    def test_config_reload_reports_the_reapplied_color_scheme_when_enabled(self):
        def reload_config(terminal, path, text, expected_background):
            path.write_text(text)
            os.kill(terminal.process.pid, signal.SIGUSR1)
            deadline = time.monotonic() + 2
            while terminal.options()["bg"] != expected_background:
                if time.monotonic() >= deadline:
                    self.fail(
                        f"config background did not become "
                        f"{expected_background:#08x}"
                    )
                time.sleep(0.01)

        dark = 'fg = "#ffffff"\nbg = "#000000"\n'
        light = 'fg = "#000000"\nbg = "#ffffff"\n'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.toml"
            path.write_text(dark)
            with Shitty(pin_vga=False, extra_arguments=("-config", path)) as terminal:
                reload_config(terminal, path, light, 0xFFFFFF)
                self.assertEqual(terminal.read_all_input(), b"")

                terminal.write(b"\x1b[?2031h")
                reload_config(terminal, path, dark, 0x000000)
                self.assertEqual(
                    terminal.read_all_input(),
                    b"\x1b[?997;1n",
                )

                reload_config(terminal, path, light, 0xFFFFFF)
                self.assertEqual(
                    terminal.read_all_input(),
                    b"\x1b[?997;2n",
                )

    def test_color_scheme_reply_uses_selected_control_width(self):
        with Shitty() as terminal:
            terminal.write(b"\x1b G\x1b[?996n")
            self.assertEqual(terminal.read_input(), b"\x9b?997;1n")


if __name__ == "__main__":
    unittest.main()
