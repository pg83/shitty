# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import errno
import unittest

from harness import Shitty


class PtyOutputTest(unittest.TestCase):
    protocol_high_water = 4096

    def test_simultaneous_read_and_write_flushes_older_bytes_before_reply(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.input(b"older")
            terminal.script_pty_writes(64)
            terminal.script_pty_reads(b"\x1b[5n", ("error", errno.EAGAIN))

            self.assertFalse(terminal.service_pty(readable=True, writable=True))
            self.assertEqual(terminal.read_written_pty(), b"older")
            self.assertEqual(terminal.pending_output(), len(b"\x1b[0n"))

    def test_partial_writes_resume_at_exact_unsent_byte_after_backpressure(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(2, 3, ("error", errno.EAGAIN))
            terminal.input(b"abcdefghi")
            self.assertEqual(terminal.read_written_pty(), b"abcde")
            self.assertEqual(terminal.pending_output(), 4)

            terminal.script_pty_writes(1, 3)
            self.assertTrue(terminal.flush_output_result())
            self.assertEqual(terminal.read_written_pty(), b"fghi")
            self.assertEqual(terminal.pending_output(), 0)

    def test_interrupted_write_is_retried_without_duplication(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(
                ("error", errno.EINTR), 1, ("error", errno.EINTR), 8
            )
            terminal.input(b"retry")
            self.assertEqual(terminal.read_written_pty(), b"retry")
            self.assertEqual(terminal.pending_output(), 0)

    def test_fatal_write_keeps_payload_available_for_later_retry(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(("error", errno.EPIPE))
            terminal.input(b"retained")
            self.assertEqual(terminal.read_written_pty(), b"")
            self.assertEqual(terminal.pending_output(), 8)

            terminal.script_pty_writes(8)
            self.assertTrue(terminal.flush_output_result())
            self.assertEqual(terminal.read_written_pty(), b"retained")

    def test_protocol_response_is_dropped_atomically_at_high_water(self):
        queries = (
            b"\x1b[5n",
            b"\x1bP$qm\x1b\\",
            b"\x1b]10;?\x1b\\",
        )
        for query in queries:
            with self.subTest(query=query):
                with Shitty(columns=8, rows=2) as terminal:
                    terminal.script_pty_writes(("error", errno.EAGAIN))
                    terminal.input(b"x" * (self.protocol_high_water - 2))

                    terminal.write(query)

                    self.assertEqual(
                        terminal.pending_output(),
                        self.protocol_high_water - 2,
                    )
                    self.assertEqual(terminal.dropped_pty_responses(), 1)

    def test_protocol_query_flood_is_bounded(self):
        response_size = len(b"\x1b[0n")
        overflow = 7
        query_count = self.protocol_high_water // response_size + overflow
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.write(b"\x1b[5n" * query_count)

            self.assertEqual(terminal.pending_output(), self.protocol_high_water)
            self.assertEqual(terminal.dropped_pty_responses(), overflow)

    def test_user_input_is_accepted_above_protocol_high_water(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.input(b"x" * self.protocol_high_water)
            terminal.paste(b"user")

            self.assertEqual(
                terminal.pending_output(), self.protocol_high_water + 4
            )
            self.assertEqual(terminal.dropped_pty_responses(), 0)

            terminal.script_pty_writes(self.protocol_high_water + 4)
            self.assertTrue(terminal.flush_output_result())
            self.assertTrue(terminal.read_written_pty().endswith(b"user"))

    def test_protocol_responses_resume_as_soon_as_space_is_available(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.input(b"x" * self.protocol_high_water)
            terminal.write(b"\x1b[5n")
            self.assertEqual(terminal.dropped_pty_responses(), 1)

            terminal.script_pty_writes(self.protocol_high_water)
            self.assertTrue(terminal.flush_output_result())
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.write(b"\x1b[5n")

            self.assertEqual(terminal.pending_output(), len(b"\x1b[0n"))
            self.assertEqual(terminal.dropped_pty_responses(), 1)

    def test_full_reverse_queue_does_not_stop_pty_reading(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.input(b"x" * self.protocol_high_water)
            terminal.script_pty_reads(b"visible", ("error", errno.EAGAIN))

            self.assertFalse(
                terminal.service_pty(readable=True, writable=True)
            )
            self.assertTrue(terminal.screen_text().startswith("visible"))
            self.assertEqual(terminal.pending_output(), self.protocol_high_water)


if __name__ == "__main__":
    unittest.main()
