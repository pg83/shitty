#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""Throughput shootout against alacritty, kitty, ghostty and zetta.

Every terminal cats the same payloads through its GUI with the setup
equalized first: Menlo 12pt on macOS or DejaVu Sans Mono 12px on Linux,
an 80x24 grid and the same cell box in
pixels, scrollback of 500 lines. Each configuration is verified inside
the terminal through TIOCGWINSZ before anything is measured; shitty's
-geometry is calibrated automatically because the window manager may
not honour the requested grid exactly.

Payloads: 1GB of printable ASCII with newlines (the scroll path) and
100MB of seeded pseudo-random bytes (the invalid-UTF-8 parser path).
kitty skips the random payload: it reacts to the embedded escape junk
with title changes and bells instead of drawing.

Usage:
    compare.py [--runs N] [--timeout SECONDS] [--work DIR] [--terminal NAME ...] [--verbose]
"""

import argparse
import json
import os
import random
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

COLUMNS = 80
ROWS = 24
FONT = "Menlo" if sys.platform == "darwin" else "DejaVu Sans Mono"
FONT_SIZE = 12
# Shitty uses points on macOS and pixels on Linux. The other terminals
# expose points (Alacritty, Kitty, Ghostty) or logical pixels (Zetta).
FONT_POINTS = FONT_SIZE if sys.platform == "darwin" else FONT_SIZE * 72 / 96
FONT_PIXELS = FONT_SIZE * 96 / 72 if sys.platform == "darwin" else FONT_SIZE
SCROLLBACK_LINES = 500
# A gigabyte of ASCII, so the app startup (~0.15s) reads as noise, not
# as a quarter of the wall time. The random payload is CPU-bound at a
# tenth of the throughput; 100MB already dwarfs the startup there.
ASCII_PAYLOAD_BYTES = 1_000_000_000
RANDOM_PAYLOAD_BYTES = 100_000_000
RUN_TIMEOUT = 300
PROBE_TIMEOUT = 15

WINSZ_PROBE = (
    "import fcntl, struct, sys, termios\n"
    'rows, cols, xp, yp = struct.unpack("HHHH",'
    ' fcntl.ioctl(0, termios.TIOCGWINSZ, b"\\0" * 8))\n'
    'open(sys.argv[1], "w").write(f"{cols} {rows} {xp} {yp}")\n'
)

class Terminal:
    def __init__(self, name, executable):
        self.name = name
        self.executable = executable
        self.skip_random = False
        self.cell = None

    def available(self):
        return self.executable is not None and Path(self.executable).exists()

    def argv(self, command):
        raise NotImplementedError


class Shitty(Terminal):
    def __init__(self, repo):
        super().__init__("shitty", str(repo / ".build" / "st"))
        # The window manager scales the requested geometry; calibrate()
        # replaces this with whatever request yields the target grid.
        self.geometry = f"{COLUMNS}x{ROWS}"

    def argv(self, command):
        return [
            self.executable,
            "-font", FONT,
            "-fontsize", str(FONT_SIZE),
            "-geometry", self.geometry,
            "-e", "sh", "-c", command,
        ]

    def calibrate(self, work):
        for _ in range(4):
            grid = probe(self, work)
            if grid is None or (grid[0], grid[1]) == (COLUMNS, ROWS):
                return
            cols, rows = grid[0], grid[1]
            want_cols, want_rows = map(int, self.geometry.split("x"))
            self.geometry = "x".join((
                str(max(1, round(want_cols * COLUMNS / cols))),
                str(max(1, round(want_rows * ROWS / rows))),
            ))


class Alacritty(Terminal):
    def __init__(self):
        super().__init__("alacritty", which("alacritty"))

    def argv(self, command):
        return [
            self.executable,
            "-o", f"window.dimensions.columns={COLUMNS}",
            "-o", f"window.dimensions.lines={ROWS}",
            "-o", f'font.normal.family="{FONT}"',
            "-o", f"font.size={FONT_POINTS}",
            "-o", f"scrolling.history={SCROLLBACK_LINES}",
            "-e", "sh", "-c", command,
        ]


class Kitty(Terminal):
    def __init__(self):
        super().__init__("kitty", which("kitty"))
        self.skip_random = True

    def argv(self, command):
        return [
            self.executable,
            "--config", "NONE",
            "-o", f"font_family={FONT}",
            "-o", f"font_size={FONT_POINTS}",
            # kitty rounds the advance up where the others truncate; pull
            # the cell back to the shared width.
            "-o", "modify_font=cell_width -1px",
            "-o", f"scrollback_lines={SCROLLBACK_LINES}",
            "-o", "remember_window_size=no",
            "-o", f"initial_window_width={COLUMNS}c",
            "-o", f"initial_window_height={ROWS}c",
            "-o", "macos_quit_when_last_window_closed=yes",
            "sh", "-c", command,
        ]


class Ghostty(Terminal):
    def __init__(self):
        super().__init__(
            "ghostty",
            which("ghostty") or "/Applications/Ghostty.app/Contents/MacOS/ghostty",
        )

    def argv(self, command):
        return [
            self.executable,
            "--config-default-files=false",
            "--gtk-single-instance=false",
            "--abnormal-command-exit-runtime=0",
            f"--font-family={FONT}",
            f"--font-size={FONT_POINTS}",
            # DejaVu Sans Mono needs one extra pixel to match the other terminals.
            *(["--adjust-cell-height=1"] if sys.platform != "darwin" else []),
            f"--window-width={COLUMNS}",
            f"--window-height={ROWS}",
            "--window-padding-x=0",
            "--window-padding-y=0",
            # GTK decorations reduce the requested terminal grid height.
            "--window-decoration=none",
            # Bytes, not lines; sized for the shared 500 lines.
            f"--scrollback-limit={SCROLLBACK_LINES * (COLUMNS + 2)}",
            "--quit-after-last-window-closed=true",
            "-e", "sh", "-c", command,
        ]


class Zetta(Terminal):
    def __init__(self, work):
        super().__init__(
            "zetta",
            which("zetta") or "/Applications/Zetta.app/Contents/MacOS/zetta",
        )
        self.config = work.resolve() / "zetta.json"

    def argv(self, command):
        # An explicit config prevents command handoff to a running Zetta.
        # GPUI font sizes are logical pixels, so convert the shared points.
        # --command injects text into an interactive shell; a profile runs
        # the child directly and closes the window when that child exits.
        self.config.write_text(json.dumps({
            "terminal_font_family": FONT,
            "terminal_font_size": FONT_PIXELS,
            "max_scroll_history_lines": SCROLLBACK_LINES,
            "profiles": [{
                "name": "compare",
                "program": "sh",
                "args": ["-c", command],
            }],
        }))
        return [
            self.executable,
            "--config", str(self.config),
            "--no-mux",
            # Opens the window at the grid, with the pty already that size.
            "--geometry", f"{COLUMNS}x{ROWS}",
            "--profile", "compare",
        ]


def which(name):
    from shutil import which as lookup
    return lookup(name)


def run(argv, timeout=RUN_TIMEOUT):
    # A crashed terminal can leave children holding stderr open. A file lets
    # us wait for the launcher itself without waiting for pipe EOF forever.
    with tempfile.TemporaryFile() as stderr:
        process = subprocess.Popen(
            argv,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            process.wait(timeout=timeout)
        finally:
            # Include descendants even when the launcher has already exited.
            # Never kill unrelated instances of the terminal by executable name.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
        # Bound diagnostics in case random escape sequences produce lots of logs.
        size = stderr.seek(0, os.SEEK_END)
        stderr.seek(max(0, size - 65536))
        output = stderr.read().decode("utf-8", errors="replace")
    return subprocess.CompletedProcess(argv, process.returncode, stderr=output)


def probe(terminal, work):
    """The grid and window pixels as seen from inside the terminal."""
    result = work / f"winsz-{terminal.name}.txt"
    result.unlink(missing_ok=True)
    script = work / "winsz.py"
    script.write_text(WINSZ_PROBE)
    try:
        # Keep the probe window alive briefly while GUI startup completes.
        command = f"python3 {shlex.quote(str(script))} {shlex.quote(str(result))}; sleep 0.35"
        completed = run(terminal.argv(command), timeout=PROBE_TIMEOUT)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if completed.returncode != 0 or not result.exists():
        return None
    cols, rows, xp, yp = map(int, result.read_text().split())
    return cols, rows, xp, yp


def verify(terminal, work):
    grid = probe(terminal, work)
    if grid is None:
        print(f"{terminal.name}: window probe failed, skipping")
        return False
    cols, rows, xp, yp = grid
    if (cols, rows) != (COLUMNS, ROWS):
        print(f"{terminal.name}: grid {cols}x{rows} != {COLUMNS}x{ROWS}, skipping")
        return False
    terminal.cell = (xp / cols, yp / rows)
    return True


def ensure_payloads(work):
    ascii_payload = work / "ascii.bin"
    random_payload = work / "random.bin"
    rng = random.Random(0x5117)
    if not ascii_payload.exists() or ascii_payload.stat().st_size != ASCII_PAYLOAD_BYTES:
        printable = bytes(range(0x20, 0x7F))
        line = bytes(rng.choice(printable) for _ in range(COLUMNS)) + b"\n"
        with ascii_payload.open("wb") as out:
            written = 0
            while written < ASCII_PAYLOAD_BYTES:
                out.write(line[: ASCII_PAYLOAD_BYTES - written])
                written += len(line)
    if not random_payload.exists() or random_payload.stat().st_size != RANDOM_PAYLOAD_BYTES:
        with random_payload.open("wb") as out:
            for _ in range(RANDOM_PAYLOAD_BYTES // 1_000_000):
                out.write(rng.randbytes(1_000_000))
    return [
        ("ascii", ascii_payload, ASCII_PAYLOAD_BYTES),
        ("random", random_payload, RANDOM_PAYLOAD_BYTES),
    ]


def bench(terminal, payload, runs, timeout=RUN_TIMEOUT, verbose=False):
    """Best successful wall and user time; failed payloads have no result."""
    best = None
    for attempt in range(1, runs + 1):
        if verbose:
            print(f"  {terminal.name}: {payload.stem}, run {attempt}/{runs} "
                  f"(timeout {timeout:g}s)", flush=True)
        command = f"cat {shlex.quote(str(payload))}"
        argv = ["/usr/bin/time", "-p", *terminal.argv(command)]
        try:
            completed = run(argv, timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"  {terminal.name}: timed out after {timeout:g}s; skipping payload", flush=True)
            return None
        except OSError as error:
            print(f"  {terminal.name}: launch failed: {error}; skipping payload", flush=True)
            return None
        if completed.returncode != 0:
            print(f"  {terminal.name}: failed (exit {completed.returncode}); skipping payload", flush=True)
            if completed.stderr.strip():
                print(completed.stderr.strip()[-2000:], flush=True)
            return None
        measured = dict(
            (key, float(value))
            for key, value in re.findall(r"^(real|user|sys) +([0-9.]+)", completed.stderr, re.M)
        )
        if not {"real", "user", "sys"} <= measured.keys() or measured["real"] <= 0:
            print(f"  {terminal.name}: missing or invalid timing; skipping payload", flush=True)
            return None
        if best is None or measured["real"] < best["real"]:
            best = measured
        time.sleep(0.3)
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=RUN_TIMEOUT,
                        help="maximum seconds per run (default: %(default)s)")
    parser.add_argument("--work", type=Path, default=Path("/tmp/shitty-compare"))
    parser.add_argument("--terminal", action="append", help="limit to these names")
    parser.add_argument("--verbose", action="store_true", help="show progress for each benchmark run")
    arguments = parser.parse_args()
    if arguments.runs < 1 or not 0 < arguments.timeout < float("inf"):
        parser.error("--runs and --timeout must be positive and finite")

    repo = Path(__file__).resolve().parent.parent
    arguments.work.mkdir(parents=True, exist_ok=True)

    terminals = [Shitty(repo), Alacritty(), Kitty(), Ghostty(), Zetta(arguments.work)]
    if arguments.terminal:
        terminals = [t for t in terminals if t.name in arguments.terminal]

    ready = []
    for terminal in terminals:
        if not terminal.available():
            print(f"{terminal.name}: not installed, skipping")
            continue
        if isinstance(terminal, Shitty):
            terminal.calibrate(arguments.work)
        if verify(terminal, arguments.work):
            ready.append(terminal)
    if not ready:
        sys.exit("no terminals to compare")

    cells = {terminal.cell for terminal in ready}
    for terminal in ready:
        cell = "%.1fx%.1f" % terminal.cell
        print(f"{terminal.name}: grid {COLUMNS}x{ROWS}, cell {cell}px")
    if len(cells) != 1:
        print("warning: cell sizes differ, the comparison is not apples to apples")

    payloads = ensure_payloads(arguments.work)
    for label, payload, size in payloads:
        mib = size / (1 << 20)
        print(f"\n{label} {mib:.0f}MiB (cat, best wall of {arguments.runs}):")
        rows = []
        for terminal in ready:
            if label == "random" and terminal.skip_random:
                rows.append((terminal.name, None))
                continue
            rows.append((terminal.name, bench(terminal, payload, arguments.runs,
                                              timeout=arguments.timeout, verbose=arguments.verbose)))
        rows.sort(key=lambda row: row[1]["real"] if row[1] else float("inf"))
        if arguments.verbose:
            print()
        print(f"  {'terminal':<12} {'wall':>7} {'user':>7} {'throughput':>12}")
        for name, timing in rows:
            if timing is None:
                print(f"  {name:<12} {'-':>7} {'-':>7} {'-':>12}")
                continue
            rate = mib / timing["real"]
            print(
                f"  {name:<12} {timing['real']:>6.2f}s {timing['user']:>6.2f}s"
                f" {rate:>8.0f} MiB/s"
            )


if __name__ == "__main__":
    main()
