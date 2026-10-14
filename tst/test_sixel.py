# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest

from harness import Shitty


BORDER = 2

# Register 1 painted pure red across exactly one 6x12 cell: six columns
# of a full band, next band, six columns again.
RED_CELL = b"\x1bPq#1;2;100;0;0#1!6~-!6~\x1b\\"


def pixel(image, x, y):
    width, _, pixels = image
    offset = (y * width + x) * 3
    return tuple(pixels[offset:offset + 3])


class SixelTest(unittest.TestCase):
    def test_patch_renders_cell_pixels(self):
        with Shitty(columns=8, rows=4, glyph_px=6, glyph_py=12) as terminal:
            terminal.write(b"\x1b[?25l" + RED_CELL)
            terminal.present()
            image = terminal.reference_image()
        for y in range(12):
            for x in range(6):
                self.assertEqual(
                    pixel(image, BORDER + x, BORDER + y), (255, 0, 0),
                    f"pixel {x},{y} of the image cell",
                )
        self.assertNotEqual(pixel(image, BORDER + 6, BORDER), (255, 0, 0))

    def test_transparent_pixels_show_pen_background(self):
        # T8: opacity pinned back to 100, the same as
        # test_dynamic_colors.test_border_follows_dynamic_background. The
        # pen *background* is the thing measured here, and the shipped
        # 60% default scales exactly that and nothing else - the painted
        # red column above it is unaffected either way.
        with Shitty(columns=8, rows=4, glyph_px=6, glyph_py=12,
                    extra_arguments=("-backgroundOpacity", "100", "-backgroundBlur", "off")) as terminal:
            # One painted column; the pen background is VGA blue, so the
            # transparent remainder of the cell must show it.
            terminal.write(b"\x1b[?25l\x1b[44m\x1bPq#1;2;100;0;0#1~\x1b\\")
            terminal.present()
            image = terminal.reference_image()
        for y in range(6):
            self.assertEqual(pixel(image, BORDER, BORDER + y), (255, 0, 0))
        self.assertEqual(pixel(image, BORDER + 1, BORDER), (0, 0, 170))
        self.assertEqual(pixel(image, BORDER, BORDER + 6), (0, 0, 170))

    def test_cursor_lands_on_last_image_row(self):
        with Shitty(columns=8, rows=4, glyph_px=6, glyph_py=12) as terminal:
            terminal.write(b"\x1b[2;3H")
            # 24 rows of pixels: two cell rows starting at the cursor.
            terminal.write(b"\x1bPq#1;2;100;0;0#1!6~-!6~-!6~-!6~\x1b\\")
            state = terminal.snapshot()
        self.assertEqual(state.cursor_y, 2)
        self.assertEqual(state.cursor_x, 2)

    def test_image_scrolls_at_the_bottom_margin(self):
        with Shitty(columns=8, rows=4, glyph_px=6, glyph_py=12) as terminal:
            terminal.write(b"\x1b[?25l\x1b[4;1H")
            terminal.write(b"\x1bPq#1;2;100;0;0#1!6~-!6~-!6~-!6~\x1b\\")
            state = terminal.snapshot()
            terminal.present()
            image = terminal.reference_image()
        self.assertEqual(state.cursor_y, 3)
        # Two cell rows of image, scrolled up by one: rows 2 and 3.
        self.assertEqual(pixel(image, BORDER, BORDER + 2 * 12), (255, 0, 0))
        self.assertEqual(pixel(image, BORDER, BORDER + 3 * 12), (255, 0, 0))
        self.assertNotEqual(pixel(image, BORDER, BORDER + 12), (255, 0, 0))

    def test_image_clips_at_the_right_edge(self):
        with Shitty(columns=4, rows=4, glyph_px=6, glyph_py=12) as terminal:
            terminal.write(b"\x1b[?25l\x1b[1;4H")
            # Twelve columns from the last cell: one visible, one clipped.
            terminal.write(b"\x1bPq#1;2;100;0;0#1!12~-!12~\x1b\\")
            state = terminal.snapshot()
            terminal.present()
            image = terminal.reference_image()
        self.assertEqual(state.cursor_x, 3)
        self.assertEqual(pixel(image, BORDER + 3 * 6, BORDER), (255, 0, 0))

    def test_hyperlink_covers_image_cells(self):
        with Shitty(columns=8, rows=4, glyph_px=6, glyph_py=12) as terminal:
            terminal.write(b"\x1b]8;;https://example.test\x1b\\" + RED_CELL + b"\x1b]8;;\x1b\\")
            state = terminal.snapshot()
        self.assertNotEqual(state.cell(0, 0).hyperlink, 0)

    def test_xtsmgraphics_reports_logical_geometry(self):
        with Shitty(columns=8, rows=4) as terminal:
            terminal.write(b"\x1b[?1;1;0S\x1b[?2;1;0S\x1b[?3;1;0S\x1b[?2;2;0S")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[?1;0;256S\x1b[?2;0;48;48S\x1b[?3;1S\x1b[?2;2S",
            )


    def test_bands_past_the_declared_height_are_dropped(self):
        with Shitty(columns=10, rows=4) as terminal:
            terminal.write(
                b"\x1bPq\"1;1;10;6#0;2;100;0;0#0!10~-!10~-!10~\x1b\\\x1b[6n"
            )
            self.assertEqual(terminal.read_input(), b"\x1b[2;1R")


if __name__ == "__main__":
    unittest.main()
