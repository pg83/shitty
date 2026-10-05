# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The window st draws itself on Wayland, looked at.

ui_wayland_chrome.cpp draws the window around the terminal - the tab list
with the window's buttons on it, the terminal as a rounded panel - and the
Vulkan renderer rounds the panel's bottom corners. Here a headless sway
runs st on a software Vulkan (lavapipe), grim takes the screen, and the
pixels are read where the layout says things are: the buttons in their
colours, the panel's corner cut round, and the list gone after
Ctrl+Shift+B. The chrome is pt's look on Linux, st's bare window there has
none: the tests that look at the chrome ask st for it out loud (CHROME), and
one looks at st as it comes. Skipped wherever sway, grim, wtype or lavapipe
are missing.
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("SHITTY_TEST_BINARY", ROOT / "st"))
LAVAPIPE = Path("/usr/share/vulkan/icd.d/lvp_icd.json")
TOOLS = ("sway", "swaymsg", "grim", "wtype")
MISSING = [tool for tool in TOOLS if shutil.which(tool) is None]
AVAILABLE = sys.platform.startswith("linux") and not MISSING and LAVAPIPE.exists() and BINARY.exists()

# Where the layout puts things, from the window's top-left corner as the
# shell sees it (window_chrome.h: buttons 18 in and 17 down, 12 across and
# 8 apart; the panel a gap of 8 clear of the window's edges).
BUTTON_LEFT = 18 + 6
BUTTON_TOP = 17 + 6
BUTTON_STEP = 20
PANEL_GAP = 8
RED = (0xFF, 0x5F, 0x57)
TERMINAL = (0xC0, 0x00, 0xC0)
# What pt is by default and st is not, on Linux (bin/st/main.cpp).
CHROME = ("-tabs", "+no-decorations", "-tabBar", "sidebar", "-layeredWindow")
YELLOW = (0xFE, 0xBC, 0x2E)
GREEN = (0x28, 0xC8, 0x40)


def read_ppm(path):
    data = Path(path).read_bytes()
    fields = []
    at = 0
    while len(fields) < 4:
        while data[at : at + 1].isspace():
            at += 1
        start = at
        while not data[at : at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    at += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[at:]


def pixel(image, x, y):
    width, _, pixels = image
    offset = (y * width + x) * 3
    return tuple(pixels[offset : offset + 3])


def near(colour, want, slack=24):
    return all(abs(a - b) <= slack for a, b in zip(colour, want))


@unittest.skipUnless(AVAILABLE, f"needs Linux, lavapipe, {BINARY.name} and {', '.join(TOOLS)}; missing: {MISSING}")
class WaylandChromeTest(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        runtime = self.directory / "runtime"
        runtime.mkdir(mode=0o700)
        home = self.directory / "home"
        home.mkdir()
        # A terminal background nothing else on the screen has, and solid:
        # the panel's corners are told from it by colour alone.
        settings = home / ".config" / "shitty"
        settings.mkdir(parents=True)
        (settings / "shitty.toml").write_text('bg = "#c000c0"\nbackgroundOpacity = 100\n')
        config = self.directory / "sway.conf"
        config.write_text(
            "output HEADLESS-1 resolution 1280x800 bg #6a8fb5 solid_color\n"
            "default_border none\n"
            'for_window [app_id=".*"] floating enable\n'
        )
        self.environment = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(runtime),
            "WLR_BACKENDS": "headless",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "WLR_RENDERER": "pixman",
            "VK_ICD_FILENAMES": str(LAVAPIPE),
            "SHELL": "/bin/sh",
            "LANG": "C.UTF-8",
        }
        self.sway = subprocess.Popen(["sway", "-c", str(config)], env=self.environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.st = None
        socket = None
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and socket is None:
            names = [p.name for p in runtime.iterdir()]
            wayland = [n for n in names if n.startswith("wayland-") and not n.endswith(".lock")]
            ipc = [n for n in names if n.startswith("sway-ipc.")]
            if wayland and ipc:
                socket = wayland[0]
                self.environment["SWAYSOCK"] = str(runtime / ipc[0])
            else:
                time.sleep(0.1)
        if socket is None:
            self.skipTest("headless sway did not start")
        self.environment["WAYLAND_DISPLAY"] = socket

    def launch(self, *arguments):
        self.st = subprocess.Popen([str(BINARY), *arguments], env=self.environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def tearDown(self):
        for process in (self.st, self.sway):
            if process is None:
                continue
            try:
                os.killpg(process.pid, signal.SIGKILL) if process is self.st else process.kill()
            except OSError:
                pass
            process.wait()
        shutil.rmtree(self.directory, ignore_errors=True)

    def window(self):
        """The st window's rectangle, as the shell sees it."""
        tree = json.loads(subprocess.run(["swaymsg", "-t", "get_tree"], env=self.environment, capture_output=True, check=True).stdout)
        stack = [tree]
        while stack:
            node = stack.pop()
            if node.get("app_id") and node.get("pid") == self.st.pid:
                rect = node["rect"]
                return rect["x"], rect["y"], rect["width"], rect["height"]
            stack.extend(node.get("nodes", []))
            stack.extend(node.get("floating_nodes", []))
        return None

    def shot(self):
        path = self.directory / "shot.ppm"
        subprocess.run(["grim", "-t", "ppm", str(path)], env=self.environment, check=True)
        return read_ppm(path)

    def settled(self, predicate, timeout=20):
        """The screen once `predicate(image, window)` holds on it."""
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            rect = self.window()
            if rect is not None:
                last = self.shot()
                if predicate(last, rect):
                    return last, rect
            time.sleep(0.25)
        self.fail("the window never looked as expected")

    def test_the_window_is_drawn_with_its_buttons_and_a_round_panel(self):
        self.launch(*CHROME)

        def buttons_lit(image, rect):
            x, y, _, _ = rect
            return near(pixel(image, x + BUTTON_LEFT, y + BUTTON_TOP), RED)

        image, (x, y, width, height) = self.settled(buttons_lit)
        self.assertTrue(near(pixel(image, x + BUTTON_LEFT + BUTTON_STEP, y + BUTTON_TOP), YELLOW))
        self.assertTrue(near(pixel(image, x + BUTTON_LEFT + 2 * BUTTON_STEP, y + BUTTON_TOP), GREEN))

        # The panel's bottom-right corner is cut round: its very corner pixel
        # is the window's surface, not the terminal a little way in from it.
        right = x + width - PANEL_GAP - 1
        bottom = y + height - PANEL_GAP - 1
        inside = pixel(image, right - 30, bottom - 30)
        corner = pixel(image, right, bottom)
        # Premise: in from the corner is the terminal, in its own colour.
        self.assertTrue(near(inside, TERMINAL), inside)
        surface = pixel(image, x + width - 3, y + height // 2)
        # The corner is not the terminal's colour, and is the surface's (the
        # panel's shadow darkens it a little).
        self.assertFalse(near(corner, inside, 32), (corner, inside))
        self.assertTrue(near(corner, surface), (corner, surface, inside))

    def test_ctrl_shift_b_puts_the_list_away(self):
        self.launch(*CHROME)

        def buttons_lit(image, rect):
            x, y, _, _ = rect
            return near(pixel(image, x + BUTTON_LEFT, y + BUTTON_TOP), RED)

        image, (x, y, width, height) = self.settled(buttons_lit)
        probe = (x + 120, y + height - 60)
        terminal = pixel(image, x + width - 60, y + height - 60)
        # Premise: the probe is on the list, which is not the terminal.
        self.assertNotEqual(pixel(image, *probe), terminal)
        # The chord goes to whichever window has the keyboard: make it this
        # one rather than trust the shell to have focused it already.
        subprocess.run(["swaymsg", f"[pid={self.st.pid}] focus"], env=self.environment, stdout=subprocess.DEVNULL, check=True)
        # wtype makes a new virtual keyboard with a keymap of its own each
        # time it runs; a key sent at once can reach the terminal before
        # that keymap does. -s waits before the first key.
        subprocess.run(["wtype", "-s", "300", "-M", "ctrl", "-M", "shift", "-k", "b", "-m", "shift", "-m", "ctrl"], env=self.environment, check=True)

        def list_away(image, rect):
            x, y, width, height = rect
            return not near(pixel(image, x + BUTTON_LEFT, y + BUTTON_TOP), RED) and pixel(image, x + 120, y + height - 60) == pixel(image, x + width - 60, y + height - 60)

        self.settled(list_away)

    def test_st_is_a_bare_window_on_linux(self):
        self.launch()

        # The terminal, in its own colour, right up to the window's edges:
        # no surface around it and no buttons, where the chrome puts them.
        def terminal_shown(image, rect):
            x, y, width, height = rect
            return near(pixel(image, x + width // 2, y + height // 2), TERMINAL)

        image, (x, y, width, height) = self.settled(terminal_shown)
        # Premise: the chrome's first button sits inside the window, so a
        # window that drew one could not hide it outside the rectangle.
        self.assertLess(BUTTON_LEFT, width)
        self.assertLess(BUTTON_TOP, height)
        self.assertFalse(near(pixel(image, x + BUTTON_LEFT, y + BUTTON_TOP), RED))
        for corner in ((x + 2, y + 2), (x + width - 3, y + height - 3), (x + 2, y + height - 3)):
            self.assertTrue(near(pixel(image, *corner), TERMINAL), corner)


if __name__ == "__main__":
    unittest.main()
