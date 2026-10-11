#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""Throughput shootout against alacritty, kitty, ghostty and zetta.

Every terminal cats the same payloads through its GUI with Menlo 12pt
on macOS or DejaVu Sans Mono 12px on Linux and an 80x24 grid.
The grid is verified through TIOCGWINSZ before anything is measured; shitty's
-geometry is calibrated automatically because the window manager may
not honour the requested grid exactly.
Zetta's exact backing-pixel cell size is set from a verified Shitty or
Ghostty probe, including the display scale. Its measured cell must match
before it is benchmarked. Other cell-size differences are reported.
Scrollback is requested as 500 lines where supported. Older Ghostty builds
use a 1MiB byte budget instead; bytes cannot be converted exactly to lines.

Keep every benchmark window visible and in front for the entire run.
Ghostty suspends rendering when fully occluded; covered or minimized runs
do not measure the same rendering workload. Visibility is not checked by
this script.

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
# GPUI logical pixels map to Cocoa points on macOS, not CSS pixels.
FONT_PIXELS = FONT_SIZE
SCROLLBACK_LINES = 500
# Older Ghostty versions store page metadata and 8-byte cells, not raw
# text bytes. Leave room for multiple pages, including the active screen.
# This is a byte budget, not a promise of exactly 500 retained lines.
GHOSTTY_SCROLLBACK_BYTES = 1 << 20
# A gigabyte of ASCII, so the app startup (~0.15s) reads as noise, not
# as a quarter of the wall time. The random payload is CPU-bound at a
# tenth of the throughput; 100MB already dwarfs the startup there.
ASCII_PAYLOAD_BYTES = 1_000_000_000
RANDOM_PAYLOAD_BYTES = 100_000_000
RUN_TIMEOUT = 300
PROBE_TIMEOUT = 15

# The child can start before the GUI applies its requested initial size,
# and a provisional size can already be the target grid at the wrong
# display scale (Shitty on Retina starts at 80x24 with 1x cells). Wait for
# the target grid to hold unchanged, bounded so sizing failures still report.
WINSZ_PROBE = (
    "import fcntl, struct, sys, termios, time\n"
    "deadline = time.monotonic() + 2\n"
    "last, since = None, time.monotonic()\n"
    "while True:\n"
    '    size = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\\0" * 8))\n'
    "    now = time.monotonic()\n"
    "    if size != last:\n"
    "        last, since = size, now\n"
    "    rows, cols, xp, yp = size\n"
    f"    if ((cols, rows) == ({COLUMNS}, {ROWS}) and now - since >= 0.3) or now >= deadline:\n"
    "        break\n"
    "    time.sleep(0.02)\n"
    'open(sys.argv[1], "w").write(f"{cols} {rows} {xp} {yp}")\n'
)

class Terminal:
    def __init__(self, name, executable):
        self.name = name
        self.executable = executable
        self.skip_random = False
        self.cell = None
        self.cell_units = "PTY units"

    def available(self):
        return self.executable is not None and Path(self.executable).exists()

    def argv(self, command):
        raise NotImplementedError


class Shitty(Terminal):
    def __init__(self, repo):
        super().__init__("shitty", str(repo / ".build" / "st"))
        self.cell_units = "backing pixels"
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
        super().__init__(
            "alacritty",
            which("alacritty") or "/Applications/Alacritty.app/Contents/MacOS/alacritty",
        )
        # TIOCGWINSZ carries cols * cell_width in physical pixels.
        self.cell_units = "backing pixels"

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
        self.cell_units = "backing pixels"
        self.scrollback_args = [f"--scrollback-limit={GHOSTTY_SCROLLBACK_BYTES}"]
        self.scrollback_description = f"{GHOSTTY_SCROLLBACK_BYTES} bytes (line limit unavailable)"
        if self.available():
            try:
                config = subprocess.run(
                    [self.executable, "+show-config", "--default"],
                    capture_output=True, text=True, timeout=PROBE_TIMEOUT,
                )
            except (OSError, subprocess.TimeoutExpired):
                config = None
            if config is not None and config.returncode == 0 and re.search(
                r"^scrollback-limit-lines\s*=", config.stdout, re.M
            ):
                self.scrollback_args = [
                    "--scrollback-limit-bytes=unlimited",
                    f"--scrollback-limit-lines={SCROLLBACK_LINES}",
                ]
                self.scrollback_description = f"{SCROLLBACK_LINES} lines requested (page-granular)"

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
            # Cocoa sizes the grid separately from its native titlebar.
            *(["--window-decoration=none"] if sys.platform != "darwin" else []),
            *self.scrollback_args,
            "--quit-after-last-window-closed=true",
            "-e", "sh", "-c", command,
        ]


class Zetta(Terminal):
    def __init__(self, work):
        super().__init__(
            "zetta",
            which("zetta") or "/Applications/Zetta.app/Contents/MacOS/zetta",
        )
        self.cell_units = "backing pixels"
        self.cell_size = None
        self.config = work.resolve() / "zetta.json"

    def argv(self, command):
        # An explicit config prevents command handoff to a running Zetta.
        # GPUI logical pixels are Cocoa points on macOS, matching FONT_SIZE.
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
            *(["--cell-size", "%dx%d" % self.cell_size] if self.cell_size else []),
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
    if xp <= 0 or yp <= 0:
        terminal.cell = None
    if isinstance(terminal, Zetta) and terminal.cell_size is not None:
        if terminal.cell != terminal.cell_size:
            print(f"{terminal.name}: reported cell {terminal.cell} != requested "
                  f"{terminal.cell_size} backing pixels, skipping")
            return False
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
    parser.add_argument("--probe-only", action="store_true",
                        help="report terminal geometry without generating payloads or benchmarking")
    arguments = parser.parse_args()
    if arguments.runs < 1 or not 0 < arguments.timeout < float("inf"):
        parser.error("--runs and --timeout must be positive and finite")

    repo = Path(__file__).resolve().parent.parent
    arguments.work.mkdir(parents=True, exist_ok=True)

    installed = [Shitty(repo), Alacritty(), Kitty(), Ghostty(), Zetta(arguments.work)]
    terminals = installed
    if arguments.terminal:
        terminals = [t for t in terminals if t.name in arguments.terminal]

    ready = []
    for terminal in terminals:
        if not terminal.available():
            print(f"{terminal.name}: not installed, skipping")
            continue
        if isinstance(terminal, Shitty):
            terminal.calibrate(arguments.work)
        if isinstance(terminal, Zetta):
            reference = next((t for t in ready if t.cell and isinstance(t, (Shitty, Ghostty))), None)
            # A Zetta-only run still uses the same measured reference; do not
            # hard-code Retina dimensions that would double the size on Linux.
            if reference is None:
                for candidate in installed:
                    if not isinstance(candidate, (Shitty, Ghostty)) or not candidate.available():
                        continue
                    if candidate in terminals:
                        continue  # Its earlier probe already failed.
                    if isinstance(candidate, Shitty):
                        candidate.calibrate(arguments.work)
                    if verify(candidate, arguments.work) and candidate.cell:
                        reference = candidate
                        break
            if reference is None or any(v != int(v) for v in reference.cell):
                print("zetta: no verified integer backing-pixel cell reference, skipping")
                continue
            terminal.cell_size = tuple(int(v) for v in reference.cell)
        if verify(terminal, arguments.work):
            ready.append(terminal)
    if not ready:
        sys.exit("no terminals to compare")

    for terminal in ready:
        cell = (("%.2fx%.2f " % terminal.cell) + terminal.cell_units
                if terminal.cell else "unknown")
        print(f"{terminal.name}: grid {COLUMNS}x{ROWS}, reported cell {cell}")
        if isinstance(terminal, Ghostty):
            print(f"{terminal.name}: scrollback {terminal.scrollback_description}")
    if (len({(terminal.cell, terminal.cell_units) for terminal in ready}) != 1
            or any(t.cell is None or t.cell_units != "backing pixels" for t in ready)):
        print("warning: cell sizes differ or are unknown; physical size is not normalized")
    else:
        print("Cell sizes match in backing pixels.")
    print("Keep each benchmark window visible and in front throughout its run; visibility is not checked.",
          flush=True)
    if arguments.probe_only:
        return

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
