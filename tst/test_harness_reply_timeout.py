# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The harness' bound on a reply, and the way it must not be written.

Two halves of one invariant.  The harness will not wait forever for a
terminal that has stopped answering, and the way it declines to wait
must not cost it the integrity of what it sends: a command goes out in
a single send(), so the socket has to stay blocking for the kernel to
queue the whole line.  socket.settimeout() breaks exactly that - it
puts the fd into non-blocking mode, where send() stops at the send
buffer - and it broke it here once already, silently, for every command
longer than 8 KiB.
"""

import os
import signal
import socket
import time
import unittest
from unittest import mock

import harness
from harness import Shitty


def send_buffer_bytes():
    """What one send() into a socketpair is guaranteed to take."""
    parent, child = socket.socketpair()
    try:
        return parent.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
    finally:
        parent.close()
        child.close()


class HarnessReplyTimeoutTest(unittest.TestCase):
    def test_command_socket_stays_blocking(self):
        """The invariant itself, stated where a reader will find it.

        A timeout on the socket is the one implementation of the bound
        that cannot work, so it is worth failing on directly rather than
        only through the truncation it causes.
        """
        with Shitty(columns=8, rows=2) as terminal:
            self.assertIsNone(terminal.socket.gettimeout())

    def test_command_far_larger_than_the_send_buffer_arrives_whole(self):
        """And the consequence, end to end through the real protocol.

        OSC 777 comes back through READ_ACTIONS as the exact bytes the
        dispatcher saw, so a command line several times the send buffer
        is compared byte for byte against what was written.  Under a
        socket timeout this does not merely fail: the line is cut
        mid-hex and the terminal answers "invalid hex input".
        """
        size = 3 * send_buffer_bytes()
        payload = ("0123456789abcdef" * (size // 16 + 1))[:size].encode("ascii")
        with Shitty(columns=8, rows=2) as terminal:
            # WRITE carries this hex-encoded, so the line the harness
            # sends is twice the payload again: six send buffers.
            terminal.write(b"\x1b]777;" + payload + b"\x1b\\")
            self.assertEqual(
                terminal.read_actions(),
                ["OSC 777 " + payload.hex()],
            )

    @unittest.skipIf(os.name != "posix", "needs SIGSTOP to freeze the terminal")
    def test_a_terminal_that_stops_answering_fails_rather_than_hangs(self):
        """The bound, proven against a terminal that really is silent.

        SIGSTOP is the cheap stand-in for the spinning terminal the
        bound exists for: connected, never hanging up, never replying.
        The timeout is turned down for the test - the point is that the
        wait ends and says so, not how long ten seconds is.
        """
        with Shitty(columns=8, rows=2) as terminal:
            terminal.process.send_signal(signal.SIGSTOP)
            try:
                started = time.monotonic()
                with mock.patch.object(harness, "REPLY_TIMEOUT", 0.3):
                    with self.assertRaises(RuntimeError) as caught:
                        terminal.state()
                elapsed = time.monotonic() - started
            finally:
                # Not SIGCONT: the frozen terminal still owes a reply,
                # and letting it answer late would desync every command
                # after this one, close()'s QUIT included.
                terminal.process.kill()
                terminal.process.wait()
        self.assertIn("no reply within", str(caught.exception))
        self.assertLess(elapsed, 5)


if __name__ == "__main__":
    unittest.main()
