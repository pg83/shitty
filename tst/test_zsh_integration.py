# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The zsh integration of the command-line editor, under a real zsh.

lib/shitty/shell/zsh is what the terminal writes out and points ZDOTDIR at
(shell_integration.cpp). Here it is run as a login shell would find it, in a
pty, with a .zshrc of the user's own: the user's files must still be read,
ZDOTDIR must be the user's again, the line must be reported as it is typed,
and a line sent back through the widget must become zsh's line - escapes and
all. Skipped where there is no zsh.
"""

import os
import pty
import re
import select
import shutil
import signal
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "lib" / "shitty" / "shell" / "zsh"
ZSH = shutil.which("zsh")
REPORT = re.compile(rb"\x1b\]7701;c=(\d+);([^\x07]*)\x07")


class ZshSession(unittest.TestCase):
    """zsh in a pty, with the integration and a .zshrc of the user's own."""

    ZSHRC = 'PS1="P> "\nexport RC_READ=yes\n'

    def setUp(self):
        self.directory = tempfile.mkdtemp()
        self.integration = Path(self.directory) / "integration"
        self.home = Path(self.directory) / "home"
        self.integration.mkdir()
        self.home.mkdir()
        for name in (".zshenv", "integration.zsh"):
            shutil.copy(SCRIPTS / name, self.integration / name)
        (self.home / ".zshrc").write_text(self.ZSHRC)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            environment = {
                "HOME": str(self.home),
                "ZDOTDIR": str(self.integration),
                "TERM": "xterm-256color",
                # ZLE counts $CURSOR in characters only in a multibyte locale.
                "LANG": "C.UTF-8",
                "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            }
            os.execve(ZSH, ["-zsh"], environment)

    def tearDown(self):
        # Whatever a test left on the line, the shell goes: a typed `exit`
        # could land in the middle of it.
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass
        os.close(self.fd)
        shutil.rmtree(self.directory, ignore_errors=True)

    def read_until(self, pattern, timeout=10.0):
        """Everything zsh writes until `pattern` shows in it."""
        output = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                output += chunk
                if re.search(pattern, output):
                    return output
        self.fail(f"never saw {pattern!r} in {output!r}")


@unittest.skipIf(ZSH is None, "no zsh on this machine")
class ZshIntegrationTest(ZshSession):

    def test_the_users_files_are_read_and_zdotdir_is_theirs_again(self):
        self.read_until(rb"\x1b\]133;B\x07")
        os.write(self.fd, b'echo "rc=$RC_READ zdotdir=${ZDOTDIR-unset}"\r')
        # Read to the command's end: the line as typed says the same words.
        output = self.read_until(rb"\x1b\]133;D;0\x07")
        self.assertIn(b"rc=yes zdotdir=unset", output)

    def test_the_line_is_reported_and_marked_as_it_is_typed(self):
        start = self.read_until(rb"\x1b\]7701;c=0;\x07")
        # The prompt's start and the input's, in that order, before the
        # empty line's report.
        self.assertLess(start.index(b"\x1b]133;A\x07"), start.index(b"\x1b]133;B\x07"))
        os.write(self.fd, b"ls a\\b")
        output = self.read_until(rb"c=6;ls a\\x5cb\x07")
        reports = REPORT.findall(output)
        self.assertEqual(reports[-1], (b"6", b"ls a\\x5cb"))

    def test_a_line_sent_back_becomes_zshs_line(self):
        self.read_until(rb"\x1b\]133;B\x07")
        os.write(self.fd, "echo hello world".encode())
        self.read_until(rb"c=16;echo hello world\x07")
        # What the terminal sends for "delete `hello `": the whole new line,
        # a backslash and a non-ASCII character in it, the cursor after the
        # deletion.
        line = "echo \\x5cw\u00e9rld".encode()
        os.write(self.fd, b"\x1b[7701~5:" + line + b"\x07")
        output = self.read_until(rb"c=5;echo \\x5cw\xc3\xa9rld\x07")
        self.assertEqual(REPORT.findall(output)[-1], (b"5", "echo \\x5cw\u00e9rld".encode()))
        # Run as `print -r`, which prints the backslash as it is.
        os.write(self.fd, b"\x01print -r -- \x05\r")
        output = self.read_until(rb"\x1b\]133;D;0\x07")
        self.assertIn("echo \\w\u00e9rld".encode(), output)
        self.assertIn(b"\x1b]133;C\x07", output)


@unittest.skipIf(ZSH is None, "no zsh on this machine")
class ZshWithTheLineInitHookTakenTest(ZshSession):
    """A plugin that does `zle -N zle-line-init` at its own first prompt,
    after the integration hooked in, takes that hook away from it. The input's
    start must still be marked, where the prompt ends, before the line's
    first report."""

    ZSHRC = (
        'PS1="P> "\nexport RC_READ=yes\n'
        "_mine() { builtin printf MINE >\"$TTY\" }\n"
        "_take() { zle -N zle-line-init _mine }\n"
        "precmd_functions+=(_take)\n"
    )

    def test_the_input_is_marked_without_line_init(self):
        start = self.read_until(rb"MINE")
        # Premise: the hook is the plugin's - the integration's line-init
        # would have reported the empty line.
        self.assertNotIn(b"\x1b]7701;", start)
        os.write(self.fd, b"e")
        output = start + self.read_until(rb"c=1;e\x07")
        mark = output.index(b"\x1b]133;B\x07")
        self.assertLess(output.index(b"P> "), mark)
        self.assertLess(mark, output.index(b"\x1b]7701;c=1;e\x07"))

    def test_the_line_is_still_reported(self):
        self.read_until(rb"MINE")
        os.write(self.fd, b"ls a\\b")
        output = self.read_until(rb"c=6;ls a\\x5cb\x07")
        self.assertEqual(REPORT.findall(output)[-1], (b"6", b"ls a\\x5cb"))



@unittest.skipIf(ZSH is None, "no zsh on this machine")
class ZshInViModeTest(ZshSession):
    """Under `bindkey -v` ctrl+_ is no undo at all; the terminal's undo and
    redo still reach zsh's own, through the keys the integration binds."""

    ZSHRC = 'PS1="P> "\nbindkey -v\n'

    def set_line(self):
        self.read_until(rb"\x1b\]133;B\x07")
        os.write(self.fd, b"echo hello world")
        self.read_until(rb"c=16;echo hello world\x07")
        os.write(self.fd, b"\x1b[7701~5:echo \x07")
        self.read_until(rb"c=5;echo \x07")

    def test_ctrl_underscore_is_no_undo_here(self):
        # The premise: what the terminal sent before would not undo.
        self.set_line()
        # In viins it goes into the line as a character of its own.
        os.write(self.fd, b"\x1f")
        output = self.read_until(rb"\x1b\]7701;c=6;[^\x07]*\x07")
        self.assertEqual(REPORT.findall(output)[-1], (b"6", b"echo \\x1F"))

    def test_undo_and_redo_reach_zsh(self):
        self.set_line()
        os.write(self.fd, b"\x1b[7703~")
        output = self.read_until(rb"c=16;echo hello world\x07")
        self.assertEqual(REPORT.findall(output)[-1], (b"16", b"echo hello world"))
        os.write(self.fd, b"\x1b[7702~")
        output = self.read_until(rb"c=5;echo \x07")
        self.assertEqual(REPORT.findall(output)[-1], (b"5", b"echo "))

if __name__ == "__main__":
    unittest.main()
