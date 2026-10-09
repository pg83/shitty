#!/usr/bin/env python3
# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""Process lifecycle regressions for compare.py; no GUI required."""

import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

import compare


class FakeTerminal:
    name = "fake"

    def __init__(self, program):
        self.program = program
        self.calls = 0

    def argv(self, command):
        self.calls += 1
        return [sys.executable, "-c", self.program]


class CompareTests(unittest.TestCase):
    def assert_stopped(self, pid):
        # An orphan may briefly remain a zombie until init reaps it.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            status = Path(f"/proc/{pid}/stat")
            if status.exists() and status.read_text().split(") ", 1)[1].startswith("Z"):
                return
            time.sleep(0.01)
        self.fail(f"process {pid} is still running")

    def child_program(self, pidfile, exit_parent):
        return (
            "import os, time\n"
            "pid = os.fork()\n"
            "if pid == 0:\n"
            "    time.sleep(60)\n"
            "else:\n"
            f"    open({str(pidfile)!r}, 'w').write(str(pid))\n"
            + ("    os._exit(0)\n" if exit_parent else "    time.sleep(60)\n")
        )

    def test_exited_parent_with_inherited_stderr_does_not_hang(self):
        with tempfile.TemporaryDirectory() as work:
            pidfile = Path(work) / "pid"
            started = time.monotonic()
            result = compare.run([sys.executable, "-c", self.child_program(pidfile, True)], timeout=2)
            self.assertEqual(result.returncode, 0)
            self.assertLess(time.monotonic() - started, 2)
            self.assert_stopped(int(pidfile.read_text()))

    def test_timeout_cleans_descendants_and_preserves_unrelated_process(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                     start_new_session=True)
        try:
            with tempfile.TemporaryDirectory() as work:
                pidfile = Path(work) / "pid"
                with self.assertRaises(subprocess.TimeoutExpired):
                    compare.run([sys.executable, "-c", self.child_program(pidfile, False)], timeout=0.3)
                self.assert_stopped(int(pidfile.read_text()))
                self.assertIsNone(unrelated.poll())
        finally:
            unrelated.kill()
            unrelated.wait(timeout=5)

    def test_crash_is_rejected_even_when_time_reports_measurements(self):
        terminal = FakeTerminal("import os; os._exit(139)")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = compare.bench(terminal, Path("random.bin"), 3, timeout=2)
        self.assertIsNone(result)
        self.assertEqual(terminal.calls, 1)
        self.assertIn("exit 139", output.getvalue())

    def test_timeout_is_reported_and_next_terminal_can_run(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            failed = compare.bench(FakeTerminal("import time; time.sleep(60)"),
                                   Path("random.bin"), 1, timeout=0.1)
            success = compare.bench(FakeTerminal("import time; time.sleep(0.1)"),
                                    Path("random.bin"), 1, timeout=2)
        self.assertIsNone(failed)
        self.assertIn("timed out", output.getvalue())
        self.assertGreater(success["real"], 0)

    def test_ctrl_c_cleans_descendants(self):
        with tempfile.TemporaryDirectory() as work:
            pidfile = Path(work) / "pid"
            program = self.child_program(pidfile, False)
            # The launched parent interrupts the test runner after recording its child.
            program = program.rsplit("    time.sleep(60)\n", 1)[0] + (
                "    import signal\n"
                "    os.kill(os.getppid(), signal.SIGINT)\n"
                "    time.sleep(60)\n"
            )
            with self.assertRaises(KeyboardInterrupt):
                compare.run([sys.executable, "-c", program], timeout=2)
            self.assert_stopped(int(pidfile.read_text()))


if __name__ == "__main__":
    unittest.main()
