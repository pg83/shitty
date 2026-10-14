# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import math
from pathlib import Path
import struct


FONT_ROOT = Path(__file__).resolve().parents[1] / "ext" / "fonts"
COLOR_EMOJI_FONT = FONT_ROOT / "NotoColorEmoji.ttf"
NERD_FONT = FONT_ROOT / "JetBrainsMonoNerdFont-Regular.ttf"
AMIRI_FONT = FONT_ROOT / "Amiri-Regular.ttf"


def _align4(value):
    return (value + 3) & ~3


def _checksum(data):
    padded = data + b"\0" * (-len(data) & 3)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def _cmap():
    end_codes = (0x004D, 0x3000, 0xFFFF)
    start_codes = end_codes
    deltas = (
        (1 - 0x004D) & 0xFFFF,
        (2 - 0x3000) & 0xFFFF,
        1,
    )
    segments = len(end_codes)
    search_power = 1 << int(math.log2(segments))
    subtable = struct.pack(
        ">HHHHHHH",
        4,
        16 + 8 * segments,
        0,
        2 * segments,
        2 * search_power,
        int(math.log2(search_power)),
        2 * segments - 2 * search_power,
    )
    subtable += struct.pack(f">{segments}H", *end_codes)
    subtable += struct.pack(">H", 0)
    subtable += struct.pack(f">{segments}H", *start_codes)
    subtable += struct.pack(f">{segments}H", *deltas)
    subtable += struct.pack(f">{segments}H", 0, 0, 0)
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + subtable


def _name(family, style):
    values = (
        (1, family),
        (2, style),
        (4, f"{family} {style}"),
        (6, family.replace(" ", "") + "-" + style.replace(" ", "")),
    )
    strings = bytearray()
    records = bytearray()
    for name_id, value in values:
        encoded = value.encode("utf-16-be")
        records.extend(
            struct.pack(
                ">HHHHHH",
                3,
                1,
                0x0409,
                name_id,
                len(encoded),
                len(strings),
            )
        )
        strings.extend(encoded)
    return struct.pack(">HHH", 0, len(values), 6 + len(records)) + records + strings


def _os2(advance, ascender, descender, weight, selection):
    return struct.pack(
        ">HhHHHhhhhhhhhhhh10sIIII4sHHHhhhHH",
        0,
        advance,
        weight,
        5,
        0,
        650,
        600,
        0,
        75,
        650,
        600,
        0,
        350,
        50,
        250,
        0,
        b"\x02\x0b\x06\x09\x02\x02\x02\x02\x02\x04",
        1,
        0,
        0,
        0,
        b"SHIT",
        selection,
        0x004D,
        0x3000,
        ascender,
        descender,
        0,
        ascender,
        -descender,
    )


def make_font(
    family,
    advance,
    wide_advance,
    maximum_advance,
    ascender=800,
    descender=-200,
    style="Regular",
):
    bold = "Bold" in style
    italic = "Italic" in style
    selection = (0x20 if bold else 0) | (0x01 if italic else 0)
    if not selection:
        selection = 0x40
    glyphs = 4
    glyph = struct.pack(">hhhhh", 0, 0, 0, 0, 0)
    glyf = glyph * glyphs
    tables = {
        b"OS/2": _os2(
            advance,
            ascender,
            descender,
            700 if bold else 400,
            selection,
        ),
        b"cmap": _cmap(),
        b"glyf": glyf,
        b"head": struct.pack(
            ">IIIIHHqqhhhhHHhhh",
            0x00010000,
            0x00010000,
            0,
            0x5F0F3CF5,
            0,
            1000,
            0,
            0,
            0,
            descender,
            maximum_advance,
            ascender,
            (0x01 if bold else 0) | (0x02 if italic else 0),
            8,
            2,
            0,
            0,
        ),
        b"hhea": struct.pack(
            ">IhhhHhhhhhhhhhhhH",
            0x00010000,
            ascender,
            descender,
            0,
            maximum_advance,
            0,
            0,
            maximum_advance,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            glyphs,
        ),
        b"hmtx": b"".join(
            struct.pack(">Hh", width, 0)
            for width in (advance, advance, wide_advance, maximum_advance)
        ),
        b"loca": struct.pack(">HHHHH", 0, 5, 10, 15, 20),
        b"maxp": struct.pack(
            ">IH13H",
            0x00010000,
            glyphs,
            0,
            0,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        b"name": _name(family, style),
        b"post": struct.pack(">IihhIIIII", 0x00030000, -0x000C0000 if italic else 0, -75, 50, 1, 0, 0, 0, 0),
    }

    return _pack_tables(tables)


def _pack_tables(tables):
    tags = sorted(tables)
    count = len(tags)
    power = 1 << int(math.log2(count))
    offset = _align4(12 + 16 * count)
    records = bytearray()
    body = bytearray(offset)
    table_offsets = {}
    for tag in tags:
        data = tables[tag]
        table_offsets[tag] = offset
        records.extend(struct.pack(">4sIII", tag, _checksum(data), offset, len(data)))
        body.extend(data)
        body.extend(b"\0" * (-len(body) & 3))
        offset = len(body)

    body[0:12] = struct.pack(
        ">IHHHH",
        0x00010000,
        count,
        16 * power,
        int(math.log2(power)),
        16 * count - 16 * power,
    )
    body[12 : 12 + len(records)] = records
    adjustment = (0xB1B0AFBA - _checksum(bytes(body))) & 0xFFFFFFFF
    struct.pack_into(">I", body, table_offsets[b"head"] + 8, adjustment)
    return bytes(body)


def _box_glyph(xmin, ymin, xmax, ymax):
    header = struct.pack(">hhhhh", 1, xmin, ymin, xmax, ymax)
    end_points = struct.pack(">H", 3)
    instructions = struct.pack(">H", 0)
    flags = bytes([0x01] * 4)
    xs = struct.pack(">hhhh", xmin, xmax - xmin, 0, -(xmax - xmin))
    ys = struct.pack(">hhhh", ymin, 0, ymax - ymin, 0)
    return header + end_points + instructions + flags + xs + ys


def _liga_cmap():
    # f -> glyph 1, i -> glyph 2.
    end_codes = (0x0066, 0x0069, 0xFFFF)
    start_codes = end_codes
    deltas = (
        (1 - 0x0066) & 0xFFFF,
        (2 - 0x0069) & 0xFFFF,
        1,
    )
    segments = len(end_codes)
    search_power = 1 << int(math.log2(segments))
    subtable = struct.pack(
        ">HHHHHHH",
        4,
        16 + 8 * segments,
        0,
        2 * segments,
        2 * search_power,
        int(math.log2(search_power)),
        2 * segments - 2 * search_power,
    )
    subtable += struct.pack(f">{segments}H", *end_codes)
    subtable += struct.pack(">H", 0)
    subtable += struct.pack(f">{segments}H", *start_codes)
    subtable += struct.pack(f">{segments}H", *deltas)
    subtable += struct.pack(f">{segments}H", 0, 0, 0)
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + subtable


def _gsub_liga(first_glyph, second_glyph, ligature_glyph):
    # One LookupType 4 ligature: first + second -> ligature, wired as the
    # default script's single `liga` feature.
    ligature = struct.pack(">HHH", ligature_glyph, 2, second_glyph)
    ligature_set = struct.pack(">HH", 1, 4) + ligature
    coverage = struct.pack(">HHH", 1, 1, first_glyph)
    coverage_offset = 8 + len(ligature_set)
    subtable = (
        struct.pack(">HHHH", 1, coverage_offset, 1, 8) + ligature_set + coverage
    )
    lookup = struct.pack(">HHHH", 4, 0, 1, 8) + subtable
    lookup_list = struct.pack(">HH", 1, 4) + lookup
    feature = struct.pack(">HHH", 0, 1, 0)
    feature_list = struct.pack(">H4sH", 1, b"liga", 8) + feature
    lang_sys = struct.pack(">HHHH", 0, 0xFFFF, 1, 0)
    script = struct.pack(">HH", 4, 0) + lang_sys
    script_list = struct.pack(">H4sH", 1, b"DFLT", 8) + script
    script_offset = 10
    feature_offset = script_offset + len(script_list)
    lookup_offset = feature_offset + len(feature_list)
    return (
        struct.pack(">HHHHH", 1, 0, script_offset, feature_offset, lookup_offset)
        + script_list
        + feature_list
        + lookup_list
    )


def make_liga_font(family, advance, ascender=800, descender=-200):
    # Four glyphs: empty notdef, full-cell boxes for f and i, and an
    # fi ligature confined to the left half of one cell - exactly the
    # shape that collapses "fi" into one cell when the shaper applies
    # the font's `liga` feature.
    glyphs = [
        struct.pack(">hhhhh", 0, 0, 0, 0, 0),
        _box_glyph(50, 0, advance - 50, 700),
        _box_glyph(50, 0, advance - 50, 700),
        _box_glyph(50, 0, advance // 2, 700),
    ]
    glyf = bytearray()
    loca = [0]
    for glyph in glyphs:
        glyf.extend(glyph)
        loca.append(len(glyf))
    count = len(glyphs)
    tables = {
        b"GSUB": _gsub_liga(1, 2, 3),
        b"OS/2": _os2(advance, ascender, descender, 400, 0x40),
        b"cmap": _liga_cmap(),
        b"glyf": bytes(glyf),
        b"head": struct.pack(
            ">IIIIHHqqhhhhHHhhh",
            0x00010000,
            0x00010000,
            0,
            0x5F0F3CF5,
            0,
            1000,
            0,
            0,
            0,
            descender,
            advance,
            ascender,
            0,
            8,
            2,
            0,
            0,
        ),
        b"hhea": struct.pack(
            ">IhhhHhhhhhhhhhhhH",
            0x00010000,
            ascender,
            descender,
            0,
            advance,
            0,
            0,
            advance,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            count,
        ),
        b"hmtx": b"".join(
            struct.pack(">Hh", advance, 0) for _ in range(count)
        ),
        b"loca": struct.pack(f">{count + 1}H", *(offset // 2 for offset in loca)),
        b"maxp": struct.pack(
            ">IH13H",
            0x00010000,
            count,
            4,
            1,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        b"name": _name(family, "Regular"),
        b"post": struct.pack(">IihhIIIII", 0x00030000, 0, -75, 50, 1, 0, 0, 0, 0),
    }
    return _pack_tables(tables)


def _segment_cmap(mapping):
    # One single-codepoint segment per entry, BMP only.
    codes = sorted(mapping)
    end_codes = tuple(codes) + (0xFFFF,)
    start_codes = end_codes
    deltas = tuple((mapping[code] - code) & 0xFFFF for code in codes) + (1,)
    segments = len(end_codes)
    search_power = 1 << int(math.log2(segments))
    subtable = struct.pack(
        ">HHHHHHH",
        4,
        16 + 8 * segments,
        0,
        2 * segments,
        2 * search_power,
        int(math.log2(search_power)),
        2 * segments - 2 * search_power,
    )
    subtable += struct.pack(f">{segments}H", *end_codes)
    subtable += struct.pack(">H", 0)
    subtable += struct.pack(f">{segments}H", *start_codes)
    subtable += struct.pack(f">{segments}H", *deltas)
    subtable += struct.pack(f">{segments}H", *((0,) * segments))
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + subtable


def make_box_font(family, advance, codepoints, ascender=800, descender=-200, boxes=None):
    # Every listed BMP codepoint maps to a solid box filling most of the
    # cell - unmistakable ink for coverage assertions. `boxes` puts a
    # codepoint's box elsewhere: {codepoint: (xmin, ymin, xmax, ymax)}.
    boxes = boxes or {}
    glyphs = [struct.pack(">hhhhh", 0, 0, 0, 0, 0)]
    mapping = {}
    for code in sorted(codepoints):
        mapping[code] = len(glyphs)
        glyphs.append(_box_glyph(*boxes.get(code, (50, 0, advance - 50, 700))))
    glyf = bytearray()
    loca = [0]
    for glyph in glyphs:
        glyf.extend(glyph)
        loca.append(len(glyf))
    count = len(glyphs)
    tables = {
        b"OS/2": _os2(advance, ascender, descender, 400, 0x40),
        b"cmap": _segment_cmap(mapping),
        b"glyf": bytes(glyf),
        b"head": struct.pack(
            ">IIIIHHqqhhhhHHhhh",
            0x00010000,
            0x00010000,
            0,
            0x5F0F3CF5,
            0,
            1000,
            0,
            0,
            0,
            descender,
            advance,
            ascender,
            0,
            8,
            2,
            0,
            0,
        ),
        b"hhea": struct.pack(
            ">IhhhHhhhhhhhhhhhH",
            0x00010000,
            ascender,
            descender,
            0,
            advance,
            0,
            0,
            advance,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            count,
        ),
        b"hmtx": b"".join(
            struct.pack(">Hh", advance, 0) for _ in range(count)
        ),
        b"loca": struct.pack(f">{count + 1}H", *(offset // 2 for offset in loca)),
        b"maxp": struct.pack(
            ">IH13H",
            0x00010000,
            count,
            4,
            1,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        b"name": _name(family, "Regular"),
        b"post": struct.pack(">IihhIIIII", 0x00030000, 0, -75, 50, 1, 0, 0, 0, 0),
    }
    return _pack_tables(tables)


def make_optical_font(family, advance=1000, ascender=800, descender=-200):
    # A and Cyrillic A put a narrow stem at the left edge; V and Cyrillic
    # U put the same stem at the right edge. In a four-cell AVAV run the
    # ordinary grid therefore produces alternating huge and tiny gaps.
    # The plus is visible but deliberately outside the optical alphabet,
    # so A+V is an exact whole-run passthrough fixture.
    glyphs = [
        struct.pack(">hhhhh", 0, 0, 0, 0, 0),
        _box_glyph(50, 0, 250, 700),
        _box_glyph(650, 0, 850, 700),
        _box_glyph(400, 0, 600, 700),
    ]
    mapping = {
        ord("+"): 3,
        ord("A"): 1,
        ord("V"): 2,
        ord("А"): 1,
        ord("У"): 2,
    }
    glyf = bytearray()
    loca = [0]
    for glyph in glyphs:
        glyf.extend(glyph)
        loca.append(len(glyf))
    count = len(glyphs)
    tables = {
        b"OS/2": _os2(advance, ascender, descender, 400, 0x40),
        b"cmap": _segment_cmap(mapping),
        b"glyf": bytes(glyf),
        b"head": struct.pack(
            ">IIIIHHqqhhhhHHhhh",
            0x00010000,
            0x00010000,
            0,
            0x5F0F3CF5,
            0,
            1000,
            0,
            0,
            0,
            descender,
            advance,
            ascender,
            0,
            8,
            2,
            0,
            0,
        ),
        b"hhea": struct.pack(
            ">IhhhHhhhhhhhhhhhH",
            0x00010000,
            ascender,
            descender,
            0,
            advance,
            0,
            0,
            advance,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            count,
        ),
        b"hmtx": b"".join(
            struct.pack(">Hh", advance, bearing)
            for bearing in (0, 50, 650, 400)
        ),
        b"loca": struct.pack(
            f">{count + 1}H", *(offset // 2 for offset in loca)
        ),
        b"maxp": struct.pack(
            ">IH13H",
            0x00010000,
            count,
            4,
            1,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        b"name": _name(family, "Regular"),
        b"post": struct.pack(
            ">IihhIIIII", 0x00030000, 0, -75, 50, 1, 0, 0, 0, 0
        ),
    }
    return _pack_tables(tables)


def make_collection(*fonts):
    header_size = _align4(12 + 4 * len(fonts))
    result = bytearray(header_size)
    offsets = []
    for font in fonts:
        face_offset = len(result)
        offsets.append(face_offset)
        mutable = bytearray(font)
        table_count = struct.unpack_from(">H", mutable, 4)[0]
        for index in range(table_count):
            record_offset = 12 + 16 * index
            table_offset = struct.unpack_from(">I", mutable, record_offset + 8)[0]
            struct.pack_into(">I", mutable, record_offset + 8, table_offset + face_offset)
        result.extend(mutable)
        result.extend(b"\0" * (-len(result) & 3))
    struct.pack_into(">4sII", result, 0, b"ttcf", 0x00010000, len(fonts))
    struct.pack_into(f">{len(offsets)}I", result, 12, *offsets)
    return bytes(result)
