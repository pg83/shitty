# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from harness import Shitty


ROOT = Path(__file__).resolve().parents[1]
COLOR_FONT = ROOT / "tests" / "fonts" / "NotoColorEmoji.ttf"


def run_fontconfig(*arguments):
    try:
        return subprocess.run(
            arguments,
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as error:
        raise unittest.SkipTest(
            f"{arguments[0]} is unavailable; fontconfig discovery cannot run"
        ) from error
    except subprocess.CalledProcessError as error:
        raise unittest.SkipTest(
            f"{arguments[0]} failed; fontconfig discovery cannot run"
        ) from error


def indexed_fontconfig_matches():
    for family in fontconfig_families():
        matched = run_fontconfig(
            "fc-match", "--format=%{file}\n%{index}\n", family
        )
        fields = matched.stdout.splitlines()
        if len(fields) != 2:
            continue
        try:
            index = int(fields[1])
        except ValueError:
            continue
        if fields[0] and index > 0:
            yield family, fields[0], index


def fontconfig_families():
    listed = run_fontconfig(
        "fc-list", "--format=%{family[0]}\n"
    )
    return tuple(sorted(
        family for family in set(listed.stdout.splitlines()) if family
    ))


def loadable_fontconfig_matches(terminal, families=None):
    candidates = (
        fontconfig_families()
        if families is None
        else tuple(sorted(set(families)))
    )
    for family in candidates:
        try:
            variants = terminal.resolve_fontconfig(family)
            if not variants["regular"]:
                continue
            loaded = terminal.load_font(family, "")
        except RuntimeError:
            continue
        yield family, variants, loaded


def loadable_indexed_fontconfig_matches(terminal):
    for family, filename, index in indexed_fontconfig_matches():
        try:
            variants = terminal.resolve_fontconfig(family)
            if face_from_variants(variants, "regular") != (filename, index):
                continue
            loaded = terminal.load_font(family, "")
        except RuntimeError:
            continue
        if loaded["regular_face_index"] == index:
            yield family, filename, index, variants, loaded


def face_from_variants(variants, name):
    return variants[name], variants[name + "_index"]


def write_bitmap_font(path, width, ascent, descent):
    height = ascent + descent
    bitmap_row = "00" * ((width + 7) // 8)
    bitmap = "\n".join(bitmap_row for _ in range(height))
    path.write_text(
        f"""STARTFONT 2.1
FONT -misc-shitty-test-medium-r-normal--{height}-160-75-75-c-{width * 10}-iso10646-1
SIZE {height} 75 75
FONTBOUNDINGBOX {width} {height} 0 -{descent}
STARTPROPERTIES 7
FONT_ASCENT {ascent}
FONT_DESCENT {descent}
PIXEL_SIZE {height}
POINT_SIZE {height * 10}
RESOLUTION_X 75
RESOLUTION_Y 75
AVERAGE_WIDTH {width * 10}
ENDPROPERTIES
CHARS 1
STARTCHAR M
ENCODING 77
SWIDTH 500 0
DWIDTH {width} 0
BBX {width} {height} 0 -{descent}
BITMAP
{bitmap}
ENDCHAR
ENDFONT
""",
        encoding="ascii",
    )


class FontResolverTest(unittest.TestCase):
    def test_missing_or_incompatible_double_width_font_keeps_primary_fallback(self):
        with Shitty() as terminal:
            primary = terminal.load_font("monospace", "")
            fallback = terminal.load_font("monospace", "Arial")
        self.assertEqual(fallback["double_width"], 0)
        self.assertEqual(
            (fallback["px"], fallback["py"]),
            (primary["px"], primary["py"]),
        )

    def test_fontconfig_resolves_family_and_alias_to_existing_files(self):
        with Shitty() as terminal:
            installed = next(
                loadable_fontconfig_matches(terminal),
                None,
            )
            if installed is None:
                self.skipTest(
                    "no installed fontconfig family resolves and loads"
                )
            for family in ("monospace", installed[0]):
                with self.subTest(family=family):
                    selected = next(
                        loadable_fontconfig_matches(terminal, (family,)),
                        None,
                    )
                    self.assertIsNotNone(selected)
                    _, variants, _ = selected
                    self.assertTrue(variants["regular"])
                    self.assertTrue(os.path.isfile(variants["regular"]))

    def test_fontconfig_loads_all_four_style_faces_when_available(self):
        with Shitty() as terminal:
            for _, variants, loaded in loadable_fontconfig_matches(terminal):
                if not all(
                    variants[name]
                    for name in ("regular", "bold", "italic", "bold_italic")
                ):
                    continue
                if (
                    loaded["bold"],
                    loaded["italic"],
                    loaded["bold_italic"],
                ) == (1, 1, 1):
                    break
            else:
                self.skipTest("no compatible four-face family is installed")

    def test_fontconfig_discovery_continues_after_unusable_candidate(self):
        candidates = ("!shitty-missing-test-family!", "monospace")
        with Shitty() as terminal:
            selected = list(
                loadable_fontconfig_matches(terminal, candidates)
            )
        self.assertEqual([match[0] for match in selected], ["monospace"])

    def test_fontconfig_list_absence_is_a_deliberate_skip(self):
        with mock.patch.object(
            subprocess,
            "run",
            side_effect=FileNotFoundError,
        ):
            with self.assertRaisesRegex(
                unittest.SkipTest,
                "fc-list is unavailable",
            ):
                fontconfig_families()

    def test_fontconfig_match_absence_is_a_deliberate_skip(self):
        with (
            mock.patch(
                __name__ + ".fontconfig_families",
                return_value=("monospace",),
            ),
            mock.patch.object(
                subprocess,
                "run",
                side_effect=FileNotFoundError,
            ),
        ):
            with self.assertRaisesRegex(
                unittest.SkipTest,
                "fc-match is unavailable",
            ):
                list(indexed_fontconfig_matches())

    def test_fontconfig_family_loads_without_a_search_path(self):
        with Shitty() as terminal:
            loaded = terminal.load_font("monospace", "")
        self.assertGreater(loaded["px"], 0)
        self.assertGreater(loaded["py"], 0)

    def test_font_file_path_is_not_treated_as_a_family(self):
        with Shitty(extra_arguments=("-fontsize", "32")) as terminal:
            variants = terminal.resolve_fontconfig(COLOR_FONT)
            primary = terminal.load_font(COLOR_FONT, "")
            fallback = terminal.load_font("monospace", COLOR_FONT)
        self.assertEqual(variants["regular"], str(COLOR_FONT))
        self.assertLessEqual(primary["py"], 64)
        self.assertEqual(fallback["double_width"], 1)

    def test_fontconfig_preserves_collection_face_index(self):
        with Shitty() as terminal:
            match = next(
                loadable_indexed_fontconfig_matches(terminal),
                None,
            )
            if match is None:
                self.skipTest(
                    "no indexed collection resolves and loads"
                )
            _, filename, index, variants, loaded = match
            metrics = terminal.font_face_metrics(filename, index, ord("M"))

        self.assertEqual(variants["regular"], filename)
        self.assertEqual(variants["regular_index"], index)
        self.assertEqual(metrics["face_index"], index)
        self.assertEqual(loaded["regular_face_index"], index)

    def test_collection_font_uses_single_and_double_width_advances(self):
        loaded = None
        primary_metrics = None
        double_width_metrics = None
        selected_family = None
        with Shitty() as terminal:
            for (
                family,
                _,
                _,
                variants,
                _,
            ) in loadable_indexed_fontconfig_matches(terminal):
                primary_face = face_from_variants(variants, "regular")
                primary = terminal.font_face_metrics(*primary_face, ord("M"))
                double_width = terminal.font_face_metrics(
                    *primary_face, 0x3000
                )
                if not (
                    primary["valid"]
                    and primary["has_glyph"]
                    and double_width["has_glyph"]
                    and primary["advance"] != primary["max_advance"]
                    and double_width["advance"] != double_width["max_advance"]
                    and double_width["advance"] == 2 * primary["advance"]
                ):
                    continue
                try:
                    loaded = terminal.load_font(family, family)
                except RuntimeError:
                    continue
                primary_metrics = primary
                double_width_metrics = double_width
                selected_family = family
                break
        if loaded is None:
            self.skipTest(
                "no indexed collection distinguishes cell and maximum advances"
            )

        self.assertEqual(
            loaded["px"], primary_metrics["advance"], selected_family
        )
        self.assertEqual(loaded["double_width"], 1, selected_family)
        self.assertEqual(
            loaded["double_width_face_index"],
            loaded["regular_face_index"],
            selected_family,
        )
        self.assertEqual(
            2 * loaded["px"],
            double_width_metrics["advance"],
            selected_family,
        )
        self.assertEqual(
            primary_metrics["py"],
            loaded["py"],
            selected_family,
        )

    def test_scaled_overlay_keeps_primary_cell_width(self):
        selected = None
        with Shitty() as terminal:
            for (
                _,
                variants,
                _,
            ) in loadable_fontconfig_matches(terminal):
                primary_face = face_from_variants(variants, "regular")
                primary = terminal.font_face_metrics(*primary_face, ord("M"))
                if not primary["valid"] or not primary["has_glyph"]:
                    continue
                for name in ("bold", "italic", "bold_italic"):
                    overlay_face = face_from_variants(variants, name)
                    if not overlay_face[0]:
                        continue
                    overlay = terminal.font_face_metrics(
                        *overlay_face, ord("M")
                    )
                    if (
                        overlay["valid"]
                        and overlay["has_glyph"]
                        and (overlay["py"], overlay["baseline"])
                        == (primary["py"], primary["baseline"])
                        and overlay["max_advance"] != primary["advance"]
                    ):
                        selected = primary_face, overlay_face, primary, overlay
                        break
                if selected is not None:
                    break
            if selected is None:
                self.skipTest(
                    "no scalable overlay with distinct width metrics is installed"
                )
            primary_face, overlay_face, primary, overlay = selected
            loaded_overlay = terminal.load_overlay(
                *primary_face, *overlay_face
            )

        self.assertNotEqual(overlay["max_advance"], primary["advance"])
        self.assertEqual(
            (overlay["py"], overlay["baseline"]),
            (primary["py"], primary["baseline"]),
        )
        self.assertTrue(loaded_overlay["compatible"])
        self.assertEqual(loaded_overlay["px"], primary["advance"])

    def test_scaled_overlay_rejects_vertical_metric_mismatch(self):
        selected = None
        candidates = []
        with Shitty() as terminal:
            for (
                _,
                variants,
                _,
            ) in loadable_fontconfig_matches(terminal):
                face = face_from_variants(variants, "regular")
                metrics = terminal.font_face_metrics(*face, ord("M"))
                if not metrics["valid"] or not metrics["has_glyph"]:
                    continue
                for primary_face, primary in candidates:
                    if (metrics["py"], metrics["baseline"]) != (
                        primary["py"],
                        primary["baseline"],
                    ):
                        selected = primary_face, face
                        break
                if selected is not None:
                    break
                candidates.append((face, metrics))
            if selected is None:
                self.skipTest(
                    "no scalable faces with distinct vertical metrics are installed"
                )
            loaded_overlay = terminal.load_overlay(*selected[0], *selected[1])

        self.assertFalse(loaded_overlay["compatible"])

    def test_fixed_overlay_inherits_width_and_checks_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            primary = root / "primary.bdf"
            wide_overlay = root / "wide-overlay.bdf"
            shifted_overlay = root / "shifted-overlay.bdf"
            write_bitmap_font(primary, width=8, ascent=12, descent=4)
            write_bitmap_font(wide_overlay, width=10, ascent=12, descent=4)
            write_bitmap_font(shifted_overlay, width=10, ascent=11, descent=5)

            with Shitty() as terminal:
                compatible = terminal.load_overlay(
                    primary, 0, wide_overlay, 0
                )
                incompatible = terminal.load_overlay(
                    primary, 0, shifted_overlay, 0
                )

        self.assertTrue(compatible["compatible"])
        self.assertEqual(
            (compatible["px"], compatible["py"], compatible["baseline"]),
            (8, 16, 12),
        )
        self.assertFalse(incompatible["compatible"])


if __name__ == "__main__":
    unittest.main()
