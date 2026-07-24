# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import hashlib
import unittest
from pathlib import Path

from harness import Shitty
from test_font_resolver import loadable_indexed_fontconfig_matches


ROOT = Path(__file__).resolve().parents[1]
COLOR_FONT = ROOT / "tests" / "fonts" / "NotoColorEmoji.ttf"


class ColorFontRenderTest(unittest.TestCase):
    def test_fixed_color_mask_preserves_zero_baseline(self):
        with Shitty(extra_arguments=("-fontsize", "32")) as terminal:
            glyph = terminal.font_glyph_metrics(COLOR_FONT, 0, 0)

        self.assertEqual(
            (
                glyph["px"],
                glyph["py"],
                glyph["baseline"],
                glyph["color"],
                glyph["length"],
                glyph["nonzero"],
                glyph["first_row"],
                glyph["last_row"],
            ),
            (40, 38, 0, 0, 40 * 38, 0, -1, -1),
        )

    def test_color_zwj_grapheme_renders_to_image(self):
        with Shitty(
            columns=2,
            rows=1,
            extra_arguments=("-fontsize", "32"),
        ) as terminal:
            terminal.write(b"\x1b[?25l" + "👩‍💻".encode())
            width, height, pixels = terminal.render_image(
                COLOR_FONT,
                COLOR_FONT,
            )

        self.assertEqual((width, height), (84, 42))
        self.assertEqual(
            hashlib.sha256(pixels).hexdigest(),
            "9a0ea45ef565bc6fca3da680d205b275bc2a8d3d97bce5c7088c9733df514337",
        )
        chromatic_colors = {
            pixels[offset : offset + 3]
            for offset in range(0, len(pixels), 3)
            if max(pixels[offset : offset + 3])
            != min(pixels[offset : offset + 3])
        }
        self.assertGreater(len(chromatic_colors), 16)

    def test_indexed_primary_preserves_color_zwj_shaping(self):
        with Shitty(
            columns=2,
            rows=1,
            extra_arguments=("-fontsize", "32", "-border", "2"),
        ) as terminal:
            terminal.write(b"\x1b[?25l" + "👩‍💻".encode())
            selected = None
            for (
                family,
                filename,
                index,
                variants,
                _,
            ) in loadable_indexed_fontconfig_matches(terminal):
                try:
                    loaded = terminal.load_font(family, COLOR_FONT)
                    width, height, pixels = terminal.render_image(
                        family,
                        COLOR_FONT,
                    )
                except RuntimeError:
                    continue
                selected = (
                    filename,
                    index,
                    variants,
                    loaded,
                    width,
                    height,
                    pixels,
                )
                break
            if selected is None:
                self.skipTest(
                    "no indexed collection resolves and loads with color fallback"
                )
            (
                filename,
                index,
                variants,
                loaded,
                width,
                height,
                pixels,
            ) = selected

        self.assertEqual(
            (variants["regular"], variants["regular_index"]),
            (filename, index),
        )
        self.assertEqual(loaded["regular_face_index"], index)
        self.assertEqual(loaded["double_width"], 1)
        self.assertEqual(
            (width, height),
            (4 + 2 * loaded["px"], 4 + loaded["py"]),
        )
        chromatic_colors = {
            pixels[offset : offset + 3]
            for offset in range(0, len(pixels), 3)
            if max(pixels[offset : offset + 3])
            != min(pixels[offset : offset + 3])
        }
        self.assertGreater(len(chromatic_colors), 16)


if __name__ == "__main__":
    unittest.main()
