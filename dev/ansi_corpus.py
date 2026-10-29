#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""Build a cat-able benchmark stream out of the recorded real-program
sessions in the test tree.

Sources, every one a capture of a real program talking to a terminal:
tst/realworld/input (282 sessions: shells, editors, TUIs, pagers) and
the recordings behind the tmux fuzz corpus (an Alacritty shell session,
the iTerm2 feature streams, the 24-bit-color and 256colors scripts, the
UTF-8 demo). The esctest stream is a conformance suite, not a program,
and stays out.

The sources together are a few megabytes; the stream repeats them in
passes until it reaches the requested size. Verbatim repetition would
let a row cache answer every pass after the first, so each pass rotates
the ASCII letters and digits of the printable text by its pass number -
the escape sequences, controls and non-ASCII text stay byte for byte,
so every pass exercises the same parser paths over different rows.
--plain keeps the text verbatim.

Sequences a terminal answers (device attributes, cursor reports, mode
and colour queries, terminfo and setting requests) are dropped: their
replies would land in the shell behind cat, and the terminals under
test would be answering rather than drawing. Window operations other
than the title stack are dropped for the same reason: a stream must
not move or resize the window it is measured in, and so is the mode
that lets DECCOLM resize it. Each session ends
with the cleanup a well-behaved program performs on exit, so a capture
that stopped inside the alternate screen or with mouse reporting on
does not hide the sessions after it.

Usage:
    ansi_corpus.py [--size MIB] [--plain] OUTPUT
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tst" / "realworld"))

from zstd_codec import decompress  # noqa: E402

TMUX_STREAMS = ("alacritty", "iterm2", "24-bit-color.out", "256colors.out", "UTF-8-demo.txt")

# One token per escape sequence; the text between tokens is what the
# rotation touches. The CSI, OSC and string forms follow ECMA-48; an
# OSC or control string with no terminator ends at the next ESC.
SEQUENCE = re.compile(
    rb"\x1b\[[\x30-\x3f]*[\x20-\x2f]*[\x40-\x7e]"
    rb"|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?"
    rb"|\x1b[PX^_][^\x07\x1b]*(?:\x07|\x1b\\)?"
    rb"|\x1b[\x20-\x2f]*[\x30-\x7e]",
    re.DOTALL,
)

# Sequences the terminal replies to, and window operations that move
# the window; 22/23 (title stack) is the one XTWINOPS kept.
DROP = re.compile(
    rb"\x1b\[[>=?]?[0-9;]*c"  # DA1, DA2, DA3
    rb"|\x1b\[\??[56]n"  # DSR, DECXCPR
    rb"|\x1b\[\??[0-9;]*\$p"  # DECRQM
    rb"|\x1b\[>[0-9;]*q"  # XTVERSION
    rb"|\x1b\[[0-9;]*\*y"  # DECRQCRA
    rb"|\x1b\[[0-9;]*\$[wu]"  # DECRQPSR, DECRQTSR
    rb"|\x1b\[(?!2[23][;t])[0-9;]*t"  # XTWINOPS other than the title stack
    # Allow 80/132 (vttest in the Alacritty recording): without it every
    # DECCOLM is a no-op, with it xterm and shitty resize the window.
    rb"|\x1b\[\?40h"
    rb"|\x1b\[\?u"  # kitty keyboard query
    rb"|\x1bP[+$]q"  # XTGETTCAP, DECRQSS
    rb"|\x1bP[0-9;]*q"  # sixel: one 260 KB image in the iTerm2 streams, not drawn by every terminal
    rb"|\x1b\](?:4;[0-9]+|1[0-9]|52;[^;]*);\?"  # colour and clipboard queries
    rb"|\x1bZ",  # DECID
    re.DOTALL,
)

# What a program restores on exit: main screen, no mouse reporting, no
# bracketed paste, visible cursor, plain attributes, synchronized
# output off, normal keypad and cursor keys, kitty keyboard flags
# popped, scrolling region reset.
CLEANUP = (
    b"\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"
    b"\x1b[?2004l\x1b[?25h\x1b[0m\x1b[?2026l\x1b>\x1b[?1l\x1b[<u\x1b[r\r\n"
)


def realworld_sessions():
    for path in sorted((ROOT / "tst" / "realworld" / "input").glob("*.zst")):
        yield decompress(path.read_bytes())


def tmux_streams():
    corpus = ROOT / "tst" / "tmux" / "corpus"
    for stream in TMUX_STREAMS:
        members = sorted(corpus.glob(stream + ".[0-9]*"), key=lambda p: int(p.suffix[1:]))
        yield b"".join(member.read_bytes() for member in members)


def tokenize(data):
    """Alternating text and sequence runs, replies and window ops dropped."""
    text, sequences = [], []
    at = 0
    for match in SEQUENCE.finditer(data):
        text.append(data[at:match.start()])
        sequences.append(b"" if DROP.match(match.group()) else match.group())
        at = match.end()
    text.append(data[at:])
    sequences.append(b"")
    return text, sequences


def rotation(step):
    table = bytearray(range(256))
    for first, span in ((ord("a"), 26), (ord("A"), 26), (ord("0"), 10)):
        for index in range(span):
            table[first + index] = first + (index + step) % span
    return bytes(table)


def render(text, sequences, table):
    parts = []
    for run, sequence in zip(text, sequences):
        parts.append(run.translate(table) if table is not None else run)
        parts.append(sequence)
    return b"".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--size", type=float, default=150.0, help="target size in MiB (default 150)")
    parser.add_argument("--plain", action="store_true", help="repeat the text verbatim instead of rotating it")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    sessions = [tokenize(session) for session in realworld_sessions()]
    sessions += [tokenize(stream) for stream in tmux_streams()]
    target = int(args.size * 1024 * 1024)
    written = 0
    passes = 0
    with args.output.open("wb") as output:
        while written < target:
            table = None if args.plain else rotation(passes)
            for text, sequences in sessions:
                chunk = render(text, sequences, table) + CLEANUP
                output.write(chunk)
                written += len(chunk)
            passes += 1
    print(f"{args.output}: {written / (1024 * 1024):.1f} MiB, {passes} passes over {len(sessions)} sessions")


if __name__ == "__main__":
    main()
