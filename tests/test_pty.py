# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import sys
import errno
import signal
import unittest

from harness import Shitty


class PtyTest(unittest.TestCase):
    def test_drain_yields_before_one_mibibyte_of_continuous_input(self):
        continuous_input = 1024 * 1024
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_repeat(0, continuous_input, eof=True)
            self.assertFalse(terminal.read_pty())
            remaining = terminal.pending_scripted_pty_read_bytes()
            self.assertGreater(remaining, 0)
            self.assertLess(remaining, continuous_input)

    def test_eof_finishes_even_before_the_first_payload(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_reads("eof")
            before = terminal.snapshot().refresh_count
            self.assertTrue(terminal.read_pty())
            self.assertEqual(terminal.snapshot().refresh_count, before)

    def test_hangup_error_drains_preceding_payload_and_presents_it(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_reads(b"before-hup", ("error", errno.EIO))
            before = terminal.snapshot().refresh_count
            self.assertTrue(terminal.read_pty())
            after = terminal.snapshot()
            self.assertEqual(after.lines[0], "before-h")
            self.assertEqual(after.lines[1], "up      ")
            self.assertEqual(after.refresh_count, before + 1)

    def test_fatal_read_error_finishes_without_presenting(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_reads(("error", errno.EBADF))
            before = terminal.snapshot().refresh_count
            self.assertTrue(terminal.read_pty())
            self.assertEqual(terminal.snapshot().refresh_count, before)

    def test_child_exit_status_is_reported_verbatim(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.spawn(sys.executable, "-c", "raise SystemExit(37)")
            status, _ = terminal.wait_child()
            self.assertEqual(status, 37)

    def test_child_exit_report_includes_output_flushed_at_exit(self):
        # Output written immediately before exit — the stdio flush-at-exit
        # pattern — must never be lost to the poll/waitpid race in the
        # child pump: by reap time every write has completed.
        for _ in range(10):
            with Shitty(columns=16, rows=2) as terminal:
                terminal.spawn(
                    sys.executable,
                    "-c",
                    "import os,time; time.sleep(0.01); os.write(1, b'final-marker')",
                )
                status, screen = terminal.wait_child()
                self.assertEqual(status, 0)
                self.assertIn("final-marker", screen)

    def test_child_signal_exit_uses_shell_status_convention(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.spawn(
                sys.executable,
                "-c",
                "import os,signal; os.kill(os.getpid(), signal.SIGTERM)",
            )
            status, _ = terminal.wait_child()
            self.assertEqual(status, 128 + signal.SIGTERM)

    def test_child_output_bytes_are_read_once_through_control(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.spawn(
                sys.executable,
                "-c",
                "import os,time; os.write(1, b'causal-marker'); time.sleep(1)",
            )
            terminal.wait_read_pty()
            self.assertEqual(terminal.read_child_output(), b"causal-marker")
            self.assertEqual(terminal.read_child_output(), b"")

    def test_blocking_child_tty_does_not_block_control_reply_reads(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.spawn(
                sys.executable,
                "-c",
                "import os,time,tty; "
                "tty.setraw(0); "
                "os.set_blocking(0, True); "
                "os.write(1, b'ready'); "
                "time.sleep(10)",
            )
            terminal.wait_read_pty()
            terminal.write(b"\x1b[5n")
            self.assertEqual(terminal.read_input(), b"\x1b[0n")

    def test_continuous_child_output_does_not_monopolize_poll_child(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.spawn(
                sys.executable,
                "-c",
                "import os; data = b'\\0' * 65536; "
                "exec('while True: os.write(1, data)')",
            )
            terminal.wait_read_pty()
            status, _ = terminal.poll_child()
            self.assertIsNone(status)

    def test_nonblocking_read_without_data_is_not_eof(self):
        with Shitty(columns=8, rows=2) as terminal:
            self.assertFalse(terminal.read_pty())

    def test_pump_without_output_does_not_publish_a_frame(self):
        with Shitty(columns=8, rows=2) as terminal:
            before = terminal.snapshot().refresh_count
            terminal.pump()
            self.assertEqual(terminal.snapshot().refresh_count, before)

    def test_failed_present_keeps_terminal_damage_for_retry(self):
        with Shitty(columns=8, rows=2) as terminal:
            before = terminal.snapshot()
            terminal.fail_next_present()
            terminal.write(b"retained")
            failed = terminal.snapshot()
            self.assertEqual(failed.refresh_count, before.refresh_count)
            terminal.present()
            retried = terminal.snapshot()
            self.assertEqual(retried.lines[0], "retained")
            self.assertEqual(retried.refresh_count, before.refresh_count + 1)

    def test_damage_after_failed_present_is_merged_into_retry(self):
        with Shitty(columns=8, rows=2) as terminal:
            before = terminal.snapshot().refresh_count
            terminal.fail_next_present()
            terminal.write(b"first")
            self.assertEqual(terminal.snapshot().refresh_count, before)

            terminal.write(b"+second")
            retried = terminal.snapshot()
            self.assertEqual(retried.lines, ["first+se", "cond    "])
            self.assertEqual(retried.refresh_count, before + 1)

    def test_one_nonblocking_drain_publishes_one_frame(self):
        with Shitty(columns=80, rows=24) as terminal:
            terminal.spawn(
                sys.executable,
                "-c",
                "import os,time; os.write(1, b'A' * 65536); time.sleep(1)",
            )
            before = terminal.snapshot().refresh_count
            terminal.wait_read_pty()
            after = terminal.snapshot().refresh_count
            self.assertEqual(after, before + 1)

    def test_one_drain_consumes_every_chunk_until_eagain(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_reads(
                b"ab", b"cd", b"ef", ("error", errno.EAGAIN)
            )
            before = terminal.snapshot().refresh_count
            self.assertFalse(terminal.read_pty())
            after = terminal.snapshot()
            self.assertEqual(after.lines[0], "abcdef  ")
            self.assertEqual(after.refresh_count, before + 1)


if __name__ == "__main__":
    unittest.main()
