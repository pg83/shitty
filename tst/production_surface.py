# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_BINARY = Path(
    os.environ.get("SHITTY_PRODUCTION_BINARY", ROOT / "st")
)
PRETTY_BINARY = Path(
    os.environ.get("SHITTY_PRETTY_BINARY", ROOT / "pt")
)
TEST_VERSION = os.environ.get("SHITTY_TEST_VERSION")


class ProductionSurfaceTest(unittest.TestCase):
    def test_control_protocol_is_absent_from_production_binary(self):
        binary = PRODUCTION_BINARY.read_bytes()
        for marker in (
            b"--test-fd",
            b"SHITTY_TEST_GLYPH",
            b"test control write failed",
            b"FRONTEND_KEY_EVENT ",
            b"MODEL_SNAPSHOT",
        ):
            with self.subTest(marker=marker):
                self.assertNotIn(marker, binary)

    def test_production_binary_rejects_test_control_option(self):
        result = subprocess.run(
            [str(PRODUCTION_BINARY), "--test-fd", "1", "-help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"unknown option: --test-fd", result.stderr)

    def test_st_is_the_bare_window_and_pt_the_chrome_on_linux(self):
        # bin/st/main.cpp: on Linux st's own defaults turn tabs, panes and
        # decorations off; pt keeps the table's. Everywhere else the two
        # binaries agree.
        def defaults(binary):
            result = subprocess.run(
                [str(binary), "-help"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
                check=False,
            )
            self.assertEqual(result.returncode, 0)
            found = {}
            for name in ("tabs", "panes", "no-decorations"):
                match = re.search(rb"^  -" + name.encode() + rb" .*\(default: (\w+)\)$", result.stdout, re.MULTILINE)
                self.assertIsNotNone(match, name)
                found[name] = match.group(1).decode()
            return found

        pretty = {"tabs": "true", "panes": "true", "no-decorations": "false"}
        self.assertEqual(defaults(PRETTY_BINARY), pretty)
        if sys.platform.startswith("linux"):
            self.assertEqual(defaults(PRODUCTION_BINARY), {"tabs": "false", "panes": "false", "no-decorations": "true"})
        else:
            self.assertEqual(defaults(PRODUCTION_BINARY), pretty)

    def test_version_probe_exits_before_window_startup(self):
        expected = (
            f"Shitty {TEST_VERSION}\n"
            "Copyright (C) 2026 Shitty team\n"
        ).encode()
        for argument in ("-v", "-version"):
            with self.subTest(argument=argument):
                result = subprocess.run(
                    [str(PRODUCTION_BINARY), argument],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=5,
                    check=False,
                )
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, expected)
                self.assertEqual(result.stderr, b"")

    def test_pretty_surface_has_only_pretty_branding(self):
        version = subprocess.run(
            [str(PRETTY_BINARY), "-version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertEqual(version.returncode, 0)
        self.assertEqual(
            version.stdout,
            (
                f"Pretty {TEST_VERSION}\n"
                "Copyright (C) 2026 Pretty team\n"
            ).encode(),
        )
        self.assertEqual(version.stderr, b"")

        help_result = subprocess.run(
            [str(PRETTY_BINARY), "-help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertEqual(help_result.returncode, 0)
        self.assertIn(b"Usage:\n  pt [-option ...] [shell]", help_result.stdout)
        self.assertIn(b"(default: Pretty)", help_result.stdout)
        self.assertNotIn(b"Shitty", help_result.stdout)
        self.assertNotIn(b"shitty", help_result.stdout)
        self.assertEqual(help_result.stderr, b"")

        config_result = subprocess.run(
            [str(PRETTY_BINARY), "-config", str(ROOT / "bin" / "pt" / "pretty.toml"), "-version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertEqual(config_result.returncode, 0)
        self.assertEqual(config_result.stdout, version.stdout)
        self.assertEqual(config_result.stderr, b"")

    def test_pretty_uses_its_own_config_and_environment_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pretty_home = root / "pretty"
            pretty_home.mkdir()
            (pretty_home / "pretty.toml").write_text("unknownPrettyOption = 1\n")
            shitty_home = root / "shitty"
            shitty_home.mkdir()
            (shitty_home / "shitty.toml").write_text("unknownShittyOption = 1\n")
            environment = os.environ.copy()
            environment["XDG_CONFIG_HOME"] = directory
            result = subprocess.run(
                [str(PRETTY_BINARY), "-version"],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
                check=False,
            )
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"pretty.toml", result.stderr)
        self.assertIn(b"pretty: ", result.stderr)
        # The reported config path embeds the temporary directory, and the
        # hermetic build runner places that under the repository, whose
        # name would trip the brand check; only the message itself and the
        # path below the temporary directory carry brand identity.
        sanitized = result.stderr.replace(directory.encode(), b"")
        self.assertNotIn(b"shitty", sanitized.lower())

        environment = os.environ.copy()
        environment["PRETTY_FONT_SIZE"] = "invalid"
        font_result = subprocess.run(
            [str(PRETTY_BINARY)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertNotEqual(font_result.returncode, 0)
        self.assertIn(b"PRETTY_FONT_SIZE", font_result.stdout + font_result.stderr)
        self.assertNotIn(b"SHITTY_FONT_SIZE", font_result.stdout + font_result.stderr)


if __name__ == "__main__":
    unittest.main()
