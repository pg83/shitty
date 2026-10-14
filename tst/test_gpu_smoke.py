# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import os
import unittest

from harness import Shitty


REQUIRED = os.environ.get("SHITTY_TEST_VULKAN_REQUIRED") == "1"
# Arms whichever GPU backend the build has; the name is older than the
# second backend (see the note in test_gpu_parity.py).
SHADOW_ENVIRONMENT = {"SHITTY_TEST_VULKAN": "1"}


class GpuSmokeTest(unittest.TestCase):
    # The GPU renderer shadows the reference renderer frame for frame,
    # through the cycle each backend really uses: Vulkan runs acquire,
    # submit and present over a VK_EXT_headless_surface swapchain
    # (lavapipe in CI), Metal draws into a texture and reads it back.
    # This is the coverage that was missing when PR 62's crash - a
    # FontFaceMiss unwinding after frame acquisition - shipped unnoticed.
    #
    # What decides whether this runs is whether the build made a shadow
    # renderer, not which platform it is on. The class was skipped
    # outside Linux while Vulkan was the only backend that could draw
    # into something readable, and stayed skipped after Metal could.
    presentation_arguments = ()

    def shadowed(self, **kwargs):
        arguments = (*self.presentation_arguments, *kwargs.pop("extra_arguments", ()))
        terminal = Shitty(extra_environment=SHADOW_ENVIRONMENT, extra_arguments=arguments, **kwargs)
        if not terminal.vulkan_shadow():
            terminal.close()
            if REQUIRED:
                self.fail("gpu shadow required but unavailable")
            self.skipTest("no gpu shadow renderer in this build")
        return terminal

    @staticmethod
    def assert_images_close(reference, gpu, tolerance=3):
        if reference[:2] != gpu[:2]:
            raise AssertionError(
                f"image sizes differ: {reference[:2]} != {gpu[:2]}"
            )
        worst = max(
            (abs(left - right) for left, right in zip(reference[2], gpu[2])),
            default=0,
        )
        if worst > tolerance:
            raise AssertionError(
                f"rendered images differ by {worst}, tolerance {tolerance}"
            )

    def test_plain_frames_present(self):
        # The snapshot is the terminal's model, not its picture: a shadow
        # that drew nothing at all would satisfy it, and for as long as
        # that was the whole test, "present" was a word in the name and
        # nowhere in the assertions.
        with self.shadowed(columns=60, rows=16) as terminal:
            terminal.write(b"hello gpu\r\nsecond line")
            terminal.present()
            snapshot = terminal.snapshot()
            drawn = (terminal.reference_image(), terminal.vulkan_image())
        self.assertEqual(snapshot.lines[0].rstrip(), "hello gpu")
        self.assert_images_close(*drawn)

    def test_uncovered_script_flood_presents(self):
        # PR 62's reproducer: every fresh cluster unwinds the frame after
        # the miss, and the retry must never corrupt the swapchain state.
        line = "हिन्दी বাংলা ద్రావిడ " * 4
        with self.shadowed(columns=90, rows=20, save_lines=100) as terminal:
            for _ in range(60):
                terminal.write((line + "\r\n").encode())
                terminal.present()
            terminal.write(b"\x1b[2J\x1b[HDONE marker")
            terminal.present()
            snapshot = terminal.snapshot()
            drawn = (terminal.reference_image(), terminal.vulkan_image())
        self.assertEqual(snapshot.lines[0].rstrip()[:11], "DONE marker")
        self.assert_images_close(*drawn)

    def test_repaint_without_terminal_update_survives_repeated_readback(self):
        with self.shadowed(columns=40, rows=10, glyph_px=8, glyph_py=16) as terminal:
            terminal.write(b"retained repaint")
            terminal.present()
            expected = terminal.vulkan_image()
            for _ in range(5):
                terminal.repaint()
                self.assertEqual(terminal.vulkan_image(), expected)

    def test_resize_storm_leaves_a_matching_frame(self):
        # Named for what it checks. It was test_resize_storm_retires_
        # swapchains, and it never looked at a retirement: no counter, no
        # probe, just the picture at the end - and on a Metal build there
        # are no swapchains to retire at all. Asking whether
        # collectRetiredSwapchains() ran would take a counter in the
        # Vulkan backend and a control command to read it back, which is
        # a change to the renderer and not to a test.
        with self.shadowed(columns=40, rows=10, glyph_px=8, glyph_py=16) as terminal:
            terminal.write(b"before")
            terminal.present()
            for step in range(12):
                terminal.resize(41 + step, 11 + step % 4)
            for _ in range(5):
                terminal.repaint()
            terminal.write(b" after resize storm")
            terminal.present()
            self.assert_images_close(
                terminal.reference_image(),
                terminal.vulkan_image(),
            )

    def test_resize_during_retained_update_rebuilds_frame(self):
        with self.shadowed(columns=8, rows=2, glyph_px=8, glyph_py=16) as terminal:
            terminal.write(b"before")
            terminal.present()
            terminal.fail_next_present()
            terminal.write(b"+failed")
            terminal.resize(5, 3)
            terminal.present()
            self.assert_images_close(
                terminal.reference_image(),
                terminal.vulkan_image(),
            )

    def test_font_change_replaces_gpu_resources(self):
        with self.shadowed(
            columns=20,
            rows=4,
            glyph_px=8,
            glyph_py=16,
            extra_arguments=("-fontsize", "16"),
        ) as terminal:
            terminal.write(b"before font change")
            terminal.present()
            before = terminal.font_state()
            terminal.chord_font_increase()
            after = terminal.font_state()
            self.assertEqual(after[0], before[0] + 1)
            self.assertNotEqual(after[1:5], before[1:5])
            terminal.write(b" after")
            terminal.present()
            self.assert_images_close(
                terminal.reference_image(),
                terminal.vulkan_image(),
            )

    def assert_shadow_matches(self, terminal):
        self.assert_images_close(
            terminal.reference_image(),
            terminal.vulkan_image(),
        )

    def test_selection_drag_and_link_hover_repaint_their_rows(self):
        with self.shadowed(columns=20, rows=5, glyph_px=10, glyph_py=10) as terminal:
            terminal.write(
                b"see http://x.test/y now\r\n"
                b"\x1b]8;;http://a.test/\x1b\\linked\x1b]8;;\x1b\\ plain\r\n"
                b"\x1b[5mblink\x1b[0m tail"
            )
            terminal.present()
            # F-T8-ci premise, kept when F-vk-alpha took the pin off:
            # this test's own subject has to be shown to be in the frame
            # rather than assumed. Each stage is compared against the
            # picture before it, so a stage that painted nothing fails
            # here instead of passing on two frames that agree because
            # both are the bare grid.
            bare = terminal.reference_image()[2]
            terminal.button(0, True, x=12, y=2, time=1)
            terminal.present()
            terminal.pointer(52, 12)
            terminal.present()
            terminal.pointer(82, 22)
            terminal.present()
            terminal.button(0, False, x=82, y=22, time=1.5)
            terminal.present()
            selected = terminal.reference_image()[2]
            self.assertNotEqual(bare, selected, "the drag selected nothing")
            self.assert_shadow_matches(terminal)
            # Hovering the detected link and the OSC 8 link with control
            # held repaints the link rows, and leaving them again.
            hovered = []
            for x, y in ((72, 2), (22, 12), (122, 32), (22, 12), (72, 2)):
                terminal.pointer(x, y, modifiers=2)
                terminal.present()
                hovered.append(terminal.reference_image()[2])
            self.assertTrue(
                any(frame != selected for frame in hovered),
                "no hover repainted a link row",
            )
            self.assert_shadow_matches(terminal)
            for _ in range(2):
                terminal.blink_tick()
                terminal.present()
            terminal.write(b"\x1b]11;#202020\x1b\\")
            terminal.present()
            terminal.write(b"\x1b[?5h")
            terminal.present()
            self.assert_shadow_matches(terminal)

    def test_composition_preview_overlays_the_grid(self):
        with self.shadowed(columns=20, rows=5, glyph_px=10, glyph_py=10) as terminal:
            terminal.write(b"typing here")
            terminal.present()
            # F-T8-ci premise, as above, and kept for the same reason:
            # the preview has to reach the pixels before a comparison of
            # them means anything.
            bare = terminal.reference_image()[2]
            terminal.preedit("abc", 0, 3)
            terminal.present()
            terminal.preedit("x", 0, 1)
            terminal.present()
            self.assertNotEqual(bare, terminal.reference_image()[2],
                                "the composition preview drew nothing")
            self.assert_shadow_matches(terminal)
            terminal.preedit("")
            terminal.present()
            self.assert_shadow_matches(terminal)
            self.assertEqual(terminal.snapshot().lines[0].rstrip(), "typing here")


class VulkanBlitSmokeTest(GpuSmokeTest):
    # The same cycle through the offscreen blit fallback: the shader
    # writes an intermediate storage image and every present blits it
    # into the swapchain, the path a surface without storage usage takes.
    #
    # Vulkan's path, and only Vulkan's: -vulkanBlit is accepted by every
    # build (options.cpp) but read by that backend alone, so on a Metal
    # build these six are the six above run a second time - about seven
    # tenths of a second.
    #
    # Skipping them there would take a control command that names the
    # shadow's backend, which is a change to the harness protocol rather
    # than to a test. Inferring it from the platform instead is what this
    # file spent two commits removing: the platform decided whether these
    # tests ran, and it was wrong about it for as long as Metal could
    # draw into something readable.
    presentation_arguments = ("-vulkanBlit",)


if __name__ == "__main__":
    unittest.main()
