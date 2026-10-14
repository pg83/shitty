# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The underscore through the FreeType backend: the cell keeps room
below the baseline for the whole bar.

The glyph is drawn into a strip exactly one cell tall, and every renderer
reads that strip, so ink below the cell is gone everywhere - on Linux the
underscore of DejaVu Sans Mono at the default 15px went missing that way.
The cell used to split its height by the ascender ratio, which says
nothing about where the underscore sits."""

import tempfile
import unittest
from pathlib import Path

from font_fixture import make_box_font
from harness import Shitty, TEST_PLATFORM


BORDER = 2
UPEM = 1000
ADVANCE = 600
ASCENDER = 800
DESCENDER = -200
# A bar below the descender, as some coding fonts draw it.
BAR = (50, -300, ADVANCE - 50, -240)
SIZES = range(8, 41)
DEJAVU = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")


def old_cell(size):
    """The cell the backend used to give a scalable face: height and
    baseline, the height shared out by the ascender ratio."""
    face_height = ASCENDER - DESCENDER
    height = int(size * face_height / UPEM + 1 + 0.5)
    return height, int(height * ASCENDER / face_height + 0.5)


def render(font, size, text):
    with Shitty(columns=len(text), rows=1, extra_arguments=("-fontsize", str(size))) as terminal:
        terminal.write(b"\x1b[?25l" + text.encode())
        metrics = terminal.load_font(str(font))
        width, _, pixels = terminal.render_image(str(font))
        return metrics, width, pixels


def inked_rows(pixels, width, column, cell_width, cell_height):
    background = pixels[:3]
    rows = []
    for y in range(cell_height):
        for x in range(cell_width):
            offset = 3 * ((BORDER + y) * width + BORDER + column * cell_width + x)
            if pixels[offset : offset + 3] != background:
                rows.append(y)
                break
    return rows


@unittest.skipIf(TEST_PLATFORM == "cocoa", "the cell rule under test is the FreeType backend's")
class UnderscoreDescentTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.font = Path(directory.name) / "bar.ttf"
        self.font.write_bytes(
            make_box_font(
                "Shitty Descent Fixture",
                ADVANCE,
                [ord("M"), ord("_")],
                ascender=ASCENDER,
                descender=DESCENDER,
                boxes={ord("_"): BAR},
            )
        )

    def test_the_bar_below_the_descender_is_drawn(self):
        # Premise: at these sizes the old cell ends above the bar's top
        # edge, so the bar was cut away whole - a size where it only
        # lost a row would let a half-fix pass.
        cut = [size for size in SIZES if old_cell(size)[0] - old_cell(size)[1] <= size * -BAR[3] / UPEM]
        self.assertGreaterEqual(len(cut), 5, cut)
        for size in SIZES:
            with self.subTest(size=size):
                metrics, width, pixels = render(self.font, size, "M_")
                cell_width, cell_height = metrics["px"], metrics["py"]
                old_height, baseline = old_cell(size)
                capital = inked_rows(pixels, width, 0, cell_width, cell_height)
                bar = inked_rows(pixels, width, 1, cell_width, cell_height)
                # Premise: the baseline is where the computation says -
                # the capital stands on it.
                self.assertTrue(capital, "no ink in the capital")
                self.assertEqual(max(capital), baseline - 1, capital)
                self.assertTrue(bar, f"no ink in the underscore, cell {cell_width}x{cell_height}")
                self.assertTrue(all(row >= baseline for row in bar), (bar, baseline))
                # The bar reaches the cell's lowest row: the room is what
                # the bar needs, not more.
                self.assertEqual(max(bar), cell_height - 1, (bar, cell_height))
                self.assertGreaterEqual(cell_height, old_height)

    def test_a_bar_inside_the_cell_leaves_the_cell_alone(self):
        # A bar well above the descender needs no room the old cell did
        # not have: the cell does not grow for nothing.
        self.font.write_bytes(
            make_box_font(
                "Shitty Descent Fixture",
                ADVANCE,
                [ord("M"), ord("_")],
                ascender=ASCENDER,
                descender=DESCENDER,
                boxes={ord("_"): (50, -100, ADVANCE - 50, -40)},
            )
        )
        for size in SIZES:
            with self.subTest(size=size):
                metrics, _, _ = render(self.font, size, "M_")
                self.assertEqual(metrics["py"], old_cell(size)[0])


@unittest.skipIf(TEST_PLATFORM == "cocoa", "the cell rule under test is the FreeType backend's")
@unittest.skipUnless(DEJAVU.exists(), f"needs {DEJAVU}")
class DejaVuUnderscoreTest(unittest.TestCase):
    """The font that showed it: the default monospace face on most Linux
    systems, its bar right on the descender."""

    def test_the_underscore_has_ink_at_every_size(self):
        for size in SIZES:
            with self.subTest(size=size):
                metrics, width, pixels = render(DEJAVU, size, "a_b___")
                cell_width, cell_height = metrics["px"], metrics["py"]
                for column in (1, 3, 4, 5):
                    rows = inked_rows(pixels, width, column, cell_width, cell_height)
                    self.assertTrue(rows, f"no ink in column {column}, cell {cell_width}x{cell_height}")
                    self.assertGreater(min(rows), cell_height // 2, rows)


if __name__ == "__main__":
    unittest.main()
