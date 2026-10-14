#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The full Cartesian key-encoding matrix over keyboard layouts.

Axes: encoding mode (legacy, modifyOtherKeys 1 and 2, every kitty flag
combination) x layout (US, Russian, QWERTZ, a layout with no base key)
x every modifier subset x event type. Expected bytes come from an
independent oracle implementing the documented semantics - the kitty
protocol spec, xterm's modifyOtherKeys, and the layout rule (the active
layout wins while it prints ASCII, the base layout is the fallback) -
so a mismatch is either an oracle bug to fix here or a terminal bug to
report, never a hole in the matrix.

Each case ends with a canary: a plain 'x' text event that must come
through verbatim, catching state leaked across keystrokes (a pending
packet or a text suppression that never met its text).

The matrix is sharded: --group N --group-count M runs the cases whose
stable hash lands in bucket N.
"""

import argparse
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Shitty

SHIFT = 0x0001
CONTROL = 0x0002
ALT = 0x0004
SUPER = 0x0008

RELEASE = 0
PRESS = 1
REPEAT = 2

# mode -> None or kitty flags
MODES = [("legacy", 0), ("mok1", 0), ("mok2", 0)] + [
    ("kitty", flags) for flags in range(1, 32)
]

# name, physical key, layout codepoint, base codepoint
LAYOUTS = (
    ("us", "D", ord("d"), ord("d")),
    ("ru", "D", ord("в"), ord("d")),
    ("qwertz", "Y", ord("z"), ord("y")),
    ("nobase", "D", ord("ъ"), 0),
)

EVENTS = ("press", "repeat", "release")

CANARY = b"x"


def kitty_mods(mods):
    return (
        (1 if mods & SHIFT else 0)
        | (2 if mods & ALT else 0)
        | (4 if mods & CONTROL else 0)
        | (8 if mods & SUPER else 0)
    )


def legacy_mod_code(mods):
    if mods == 0:
        return 0
    return 1 + kitty_mods(mods)


def text_codepoint(layout, mods):
    character = chr(layout)
    if mods & SHIFT:
        character = character.upper()
    return ord(character)


def control_byte(key, shifted):
    """keyboard.cpp's controlCharacter for the keys in this matrix."""
    if ord("A") <= key <= ord("Z"):
        return bytes([key - ord("A") + 1])
    if 0 < key < 128:
        return bytes([key])
    return None


def serialize_kitty(flags, key, shifted, base, kmods, event):
    """vterm's writeKittyKey; event: 1 press, 2 repeat, 3 release."""
    if not key or (event == 3 and not flags & 2):
        return b""
    out = f"\x1b[{key}"
    if flags & 4:
        alternate_shifted = shifted if shifted != key else 0
        alternate_base = base if base != key else 0
        if alternate_shifted:
            out += f":{alternate_shifted}"
            if alternate_base:
                out += f":{alternate_base}"
        elif alternate_base:
            out += f"::{alternate_base}"
    report_event = bool(flags & 2) and event != 1
    text = shifted if (kmods & 1) and shifted else key
    report_text = (
        bool(flags & 16)
        and event != 3
        and not (kmods & (4 | 8))
        and text >= 0x20
        and not (0x7F <= text <= 0x9F)
    )
    if kmods or report_event or report_text:
        out += ";"
        if kmods or report_event:
            out += str(kmods + 1)
        if report_event:
            out += f":{event}"
        if report_text:
            out += f";{text}"
    return (out + "u").encode()


def mok2_encodes(char, mods):
    """vterm's modifyOtherKeyEncoded for mode 2."""
    if chr(char) in "!#$%&*()-+=?.,:;<>'\"":
        return bool(mods & (CONTROL | ALT))
    return mods != 0


class Oracle:
    """Reference reaction to one gesture; mirrors the input router."""

    def __init__(self, mode, flags, layout, base, mods):
        self.mode = mode
        self.flags = flags
        self.layout = layout
        self.base = base
        self.mods = mods
        self.pending = None
        self.suppress = 0

    def key(self, action):
        flags = self.flags
        kmods = kitty_mods(self.mods)
        event = {PRESS: 1, REPEAT: 2, RELEASE: 3}[action]
        pressed = action != RELEASE
        if flags:
            primary = self.layout or self.base
            has_acs = kmods & (2 | 4 | 8)
            # A repeat that produces text stays on the plain UTF-8 path;
            # report-event forces CSI only for a release without text.
            report_release = bool(flags & 2) and event == 3
            if primary and (has_acs or flags & 8 or report_release):
                if pressed and not has_acs:
                    self.pending = event
                    return b""
                out = serialize_kitty(flags, primary, 0, self.base, kmods, event)
                if pressed and ((kmods & (2 | 8) and not kmods & 4) or flags & 8):
                    self.suppress += 1
                return out
            return b""
        if not pressed:
            return b""
        if self.mods & CONTROL:
            # The layout's key wins while it prints ASCII; the base layout
            # is the fallback for non-Latin layouts.
            rule = (
                self.layout
                if 0x20 <= self.layout < 0x7F
                else self.base
            )
            if self.mode == "mok2":
                if 0x20 <= rule < 0x7F and mok2_encodes(rule, self.mods):
                    code = legacy_mod_code(self.mods)
                    return f"\x1b[27;{code};{rule}~".encode()
                return b""
            if ord("a") <= rule <= ord("z"):
                rule -= ord("a") - ord("A")
            byte = control_byte(rule, bool(self.mods & SHIFT))
            if byte is None or rule == 0:
                return b""
            if self.mods & ALT:
                return b"\x1b" + byte
            return byte
        return b""

    def text(self, codepoint):
        if self.pending is not None:
            event = self.pending
            self.pending = None
            primary = self.layout or self.base
            alternate = codepoint if codepoint != primary else 0
            return serialize_kitty(
                self.flags, primary, alternate, self.base, kitty_mods(self.mods), event
            )
        if self.suppress:
            self.suppress -= 1
            return b""
        return self.plain_text(codepoint, self.mods)

    def plain_text(self, codepoint, mods):
        if codepoint < 0x80:
            if self.mode == "mok2" and mok2_encodes(codepoint, mods):
                code = legacy_mod_code(mods)
                return f"\x1b[27;{code};{codepoint}~".encode()
            if mods & ALT:
                return b"\x1b" + bytes([codepoint])
            return bytes([codepoint])
        prefix = b"\x1b" if mods & ALT else b""
        return prefix + chr(codepoint).encode()

    def canary(self):
        return self.plain_text(ord("x"), 0)


def known_terminal_bugs(mode, flags, name, mods, event):
    """The ledger of confirmed terminal bugs the matrix has found.

    Every entry names a real defect awaiting a fix order; a case listed
    here must keep failing (an unexpected pass means the ledger is
    stale) and an unlisted case must pass. Delete the entry when the
    bug is fixed. Currently empty: the four launch findings (the kitty
    text-suppression leak and the three modifyOtherKeys=2 defects) are
    fixed.
    """
    return []


def run_case(terminal, mode, flags, physical, layout, base, mods, event):
    if mode == "mok1":
        terminal.write(b"\x1b[>4;1m")
    elif mode == "mok2":
        terminal.write(b"\x1b[>4;2m")
    elif flags:
        terminal.write(b"\x1b[=%d;1u" % flags)

    oracle = Oracle(mode, flags, layout, base, mods)
    expected = bytearray()
    text_follows = not mods & (CONTROL | SUPER)
    text = text_codepoint(layout, mods)

    def stroke(action):
        terminal.layout_key(physical, layout, base, modifiers=mods, action=action)
        expected.extend(oracle.key(action))
        if action != RELEASE and text_follows:
            terminal.frontend_text_event(text, modifiers=mods)
            expected.extend(oracle.text(text))

    stroke(PRESS)
    if event == "repeat":
        stroke(REPEAT)
    elif event == "release":
        stroke(RELEASE)

    # The canary: leaked pending packets or unmet suppressions eat it.
    terminal.frontend_text_event(ord("x"))
    expected.extend(oracle.canary())

    received = terminal.read_input()

    if mode in ("mok1", "mok2"):
        terminal.write(b"\x1b[>4;0m")
    elif flags:
        terminal.write(b"\x1b[=0;1u")

    # Recovery: leaked suppressions or pending packets from a buggy case
    # must not poison the next one - drain until a canary passes whole.
    for _ in range(8):
        terminal.frontend_text_event(ord("x"))
        if terminal.read_input() == CANARY:
            break

    return bytes(expected), received


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", type=int, required=True)
    parser.add_argument("--group-count", type=int, required=True)
    arguments = parser.parse_args()

    failures = []
    expected_failures = 0
    total = 0
    # F-T8-ci. -panes is on by default since T8, and on macOS it arms
    # the two split chords - cmd+d and cmd+shift+d, gated on that very
    # option in input_bindings.cpp. The physical key of three of the
    # four layouts here is D, so the matrix asks what bytes cmd+d makes
    # and the terminal answers by splitting the tab instead. Pinned off
    # for the same reason harness.py pins -naturalEditing off: this
    # suite asks what a chord produces at the pty, and an ambient
    # default must not be the one answering.
    #
    # The damage was not confined to the Super rows. key() clears a
    # consumed binding on release by chord key alone, ignoring
    # modifiers, so a swallowed cmd+d press left the flag standing and
    # ate the release of whatever pressed D next - which is why alt+d
    # releases went missing too.
    with Shitty(columns=8, rows=2, extra_arguments=("+panes",)) as terminal:
        for mode, flags in MODES:
            for name, physical, layout, base in LAYOUTS:
                for mods in range(16):
                    for event in EVENTS:
                        case = f"{mode}:{flags}:{name}:{mods}:{event}"
                        digest = zlib.crc32(case.encode())
                        if digest % arguments.group_count != arguments.group:
                            continue
                        total += 1
                        expected, received = run_case(
                            terminal, mode, flags, physical, layout, base, mods, event
                        )
                        bugs = known_terminal_bugs(mode, flags, name, mods, event)
                        if expected != received:
                            if bugs:
                                expected_failures += 1
                            else:
                                failures.append((case, expected, received))
                        elif bugs:
                            failures.append((
                                case,
                                b"a failure - the known-bug ledger lists "
                                + ",".join(bugs).encode(),
                                received,
                            ))

    print(f"ran {total} cases in bucket "
          f"{arguments.group}/{arguments.group_count}, "
          f"{expected_failures} pinned to known terminal bugs")
    for case, expected, received in failures:
        print(f"FAIL {case}: expected {expected!r}, received {received!r}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
