# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The reference image is as wide as the grid's columns and as tall as
its rows.

RENDER_IMAGE sizes the throwaway surface it draws into by asking
gridPixelWidth() for the columns and gridPixelHeight() for the rows
(test_mode.cpp). Both counts are a bare unsigned, both functions take
one, and handing either count to the other axis compiles and renders a
picture - a wrong-shaped one that every reader of this reply then
believes, because the reply carries its own width and height and the
harness checks the pixel buffer against them rather than against the
grid.

T5.2 measured the pair by mutation and found it half-guarded: the width
taken from the row count reddened sixteen tests, while the height taken
from the column count changed the suite's red set by nothing at all.
The tests that would have seen it - the bitmap-strike renders, which
assert an exact (width, height) - depend on a FreeType strike that does
not load everywhere, so on a machine where they are already red the
second half of the pair is watched by no one.

Hence this: the same assertion made from the default font, on a grid and
a cell that are both deliberately non-square, so neither count can stand
in for the other and neither metric can stand in for the other.
"""

import unittest

from harness import Shitty


# The default -border, per side, and it is what contentInsets() puts
# around the image (harness.py sizes its own window info the same way).
BORDER = 2

COLUMNS = 11
ROWS = 7


class RenderImageGeometryTest(unittest.TestCase):
    def test_the_image_takes_its_width_from_the_columns_and_its_height_from_the_rows(self):
        with Shitty(columns=COLUMNS, rows=ROWS) as terminal:
            terminal.write(b"\x1b[?25l")
            metrics = terminal.load_font("monospace")
            width, height, _ = terminal.render_image("monospace")

        # The fixture is asserted before it is leaned on. A square cell
        # hides a swapped pair of glyph metrics, a square grid hides a
        # swapped pair of counts, and either would make both equalities
        # below pass with the axes crossed.
        self.assertNotEqual(metrics["px"], metrics["py"])
        self.assertNotEqual(COLUMNS, ROWS)

        self.assertEqual(width, COLUMNS * metrics["px"] + 2 * BORDER)
        self.assertEqual(height, ROWS * metrics["py"] + 2 * BORDER)
