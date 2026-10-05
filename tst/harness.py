# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import select
import socket
import subprocess
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHITTY = Path(os.environ.get("SHITTY_TEST_BINARY", ROOT / "st"))
PRETTY = Path(os.environ.get("SHITTY_PRETTY_TEST_BINARY", ROOT / "pt_test"))

TEST_PLATFORM = os.environ.get("SHITTY_TEST_PLATFORM")
if TEST_PLATFORM is None:
    TEST_PLATFORM = "cocoa" if sys.platform == "darwin" else "wayland"


def run_startup_failure(font_size_env=None, extra_arguments=(), extra_environment=None):
    parent, child = socket.socketpair()
    environment = os.environ.copy()
    # Keep the developer's real ~/.config/shitty out of every test.
    environment["XDG_CONFIG_HOME"] = "/nonexistent"
    if extra_environment is not None:
        environment.update(extra_environment)
    if font_size_env is None:
        environment.pop("SHITTY_FONT_SIZE", None)
    else:
        environment["SHITTY_FONT_SIZE"] = str(font_size_env)
    try:
        return subprocess.run(
            [
                str(SHITTY),
                "--test-fd",
                str(child.fileno()),
                *map(str, extra_arguments),
            ],
            pass_fds=(child.fileno(),),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
    finally:
        child.close()
        parent.close()


def put_rows(*values):
    return b"".join(
        f"\x1b[{row};1H".encode() + value
        for row, value in enumerate(values, 1)
    )


@dataclass
class Cell:
    char: str
    double_width: bool
    double_width_continuation: bool
    bold: bool
    italic: bool
    underline: bool
    underline_style: int
    faint: bool
    blink: bool
    conceal: bool
    strike: bool
    overline: bool
    inverse: bool
    wrapped: bool
    foreground: tuple[int, int, int]
    background: tuple[int, int, int]
    underline_color: tuple[int, int, int]
    hyperlink: int
    semantic: int
    protected: bool
    line_attribute: int
    drawn: bool
    foreground_index: int = -2
    background_index: int = -2
    underline_index: int = -2
    grapheme: tuple[int, ...] = ()


@dataclass
class Snapshot:
    columns: int
    rows: int
    cursor_x: int
    cursor_y: int
    cursor_style: int
    view_offset: int
    refresh_count: int
    selection: tuple[int, int, int, int]
    rectangular_selection: bool
    cells: list[Cell]
    lines: list[str]

    def cell(self, column, row):
        if not (0 <= column < self.columns and 0 <= row < self.rows):
            raise IndexError("cell coordinate is outside the page")
        return self.cells[row * self.columns + column]


@dataclass
class RenderState:
    screen_reverse: bool
    blink_visible: bool
    cursor_blink: bool
    selection_mask: int
    selection_foreground: tuple[int, int, int]
    selection_background: tuple[int, int, int]
    grapheme_cells: int
    grapheme_codepoints: int


@dataclass
class PenState:
    bold: bool
    faint: bool
    italic: bool
    underline: bool
    underline_style: int
    blink: bool
    conceal: bool
    strike: bool
    inverse: bool
    foreground: tuple[int, int, int]
    background: tuple[int, int, int]
    foreground_index: int
    background_index: int


VGA_PIN = (
    "-fg", "#ffffff", "-bg", "#000000",
    "-color0", "#000000", "-color1", "#aa0000", "-color2", "#00aa00",
    "-color3", "#aa5500", "-color4", "#0000aa", "-color5", "#aa00aa",
    "-color6", "#00aaaa", "-color7", "#aaaaaa", "-color8", "#555555",
    "-color9", "#ff5555", "-color10", "#55ff55", "-color11", "#ffff55",
    "-color12", "#5555ff", "-color13", "#ff55ff", "-color14", "#55ffff",
    "-color15", "#ffffff",
)

# How long a single command may wait for its reply. A terminal that dies
# hangs up and every read fails at once, but one that spins - the tab
# walk CLOSE_SESSION used to do is the worked example - stays connected
# and silent, and a wait with no bound on it turns that into a CI job
# hanging until the runner's own timeout rather than a red test. Well
# past anything a healthy command takes: the whole pane suite answers in
# under a second.
#
# The bound belongs on the read, through select() below, and NOT on the
# socket: settimeout() puts the fd into non-blocking mode, and a
# non-blocking send() stops at whatever fits in the socket's send buffer
# (8 KiB for a macOS socketpair) instead of queueing the whole message.
# The one send() per command that makefile("rwb", buffering=0) does then
# truncates every command longer than that, mid-hex, and the terminal
# answers the mangled line with "invalid hex input" - see
# test_harness_reply_timeout.py, which pins this down.
REPLY_TIMEOUT = 10


def ShittyWithTabs(*args, extra_arguments=(), **kwargs):
    """A Shitty whose window holds more than one tab.

    On Linux st is one shell to a window unless -tabs says otherwise
    (bin/st/main.cpp, defaultFor): the tab chords stay the program's and
    a second session cannot be opened. Tests about several sessions ask
    for tabs out loud rather than lean on a platform's default."""
    return Shitty(*args, extra_arguments=("-tabs", *extra_arguments), **kwargs)


class Shitty:
    def __init__(
        self, columns=80, rows=24, save_lines=500,
        glyph_px=1, glyph_py=1,
        font_size_env=None, extra_arguments=(), extra_environment=None,
        binary=None, pin_vga=True, pin_border=True, pin_natural_editing=True,
        capture_stderr=False,
    ):
        parent, child = socket.socketpair()
        self.socket = parent
        self.stream = parent.makefile("rwb", buffering=0)
        self._receive_buffer = bytearray()
        child_environment = os.environ.copy()
        # Keep the developer's real ~/.config/shitty out of every test;
        # config tests override this through extra_environment.
        child_environment["XDG_CONFIG_HOME"] = "/nonexistent"
        child_environment["SHITTY_TEST_GLYPH"] = f"{glyph_px}x{glyph_py}"
        if font_size_env is None:
            child_environment.pop("SHITTY_FONT_SIZE", None)
        else:
            child_environment["SHITTY_FONT_SIZE"] = str(font_size_env)
        if extra_environment is not None:
            child_environment.update(extra_environment)
        self.process = subprocess.Popen(
            [
                str(SHITTY if binary is None else binary),
                "--test-fd",
                str(child.fileno()),
                "-geometry",
                f"{columns}x{rows}",
                "-saveLines",
                str(save_lines),
                # Pixel coordinates and image fixtures use a two-pixel
                # border on every platform. Config tests can opt out;
                # explicit border arguments below still take precedence.
                *(("-border", "2") if pin_border else ()),
                # Pin white-on-black and the sixteen palette slots
                # to plain VGA with explicit color options, so the
                # default scheme cannot shift color assertions. Scheme
                # tests opt out with pin_vga=False.
                *(VGA_PIN if pin_vga else ()),
                # T8. naturalEditing is on by default now, and it is an
                # input-layer preset: it claims the Option and Command
                # chords before anything reaches the pty. Every keyboard
                # conformance suite here asks what bytes a chord
                # produces, so it is pinned off for the reason the
                # palette is pinned to VGA - an ambient default must not
                # be the one answering. Thirteen tests across seven
                # files reddened on the default alone. Tests about the
                # preset itself opt out with pin_natural_editing=False.
                *(("+naturalEditing",) if pin_natural_editing else ()),
                *map(str, extra_arguments),
            ],
            pass_fds=(child.fileno(),),
            env=child_environment,
            # F10: off by default so every existing test keeps seeing the
            # terminal's diagnostics on the runner's own stderr. A test
            # that asks for the pipe must read it only after close(),
            # through stderr_text() below: nobody drains it while the
            # process runs, so a child that filled the pipe would block.
            # Startup warnings - the only thing this exists for - are
            # written before READY and fit many times over.
            stderr=subprocess.PIPE if capture_stderr else None,
        )
        self._glyph_px = glyph_px
        self._glyph_py = glyph_py
        self._stderr = None
        child.close()
        self._window_info = {
            "x": 10,
            "y": 20,
            "pixel_width": columns * glyph_px + 4,
            "pixel_height": rows * glyph_py + 4,
            "screen_width": 1920,
            "screen_height": 1080,
            "iconified": False,
            "maximized": False,
            "fullscreen": False,
            "tiled": False,
        }
        if self._readline() != "READY":
            raise RuntimeError("shitty test mode did not become ready")

    def close(self):
        if self.process.poll() is None:
            try:
                self.command("QUIT")
            finally:
                try:
                    if self.process.stderr is not None:
                        # communicate() drains the pipe and reaps the
                        # child; wait() alone can deadlock against a full
                        # pipe.
                        self._stderr = self.process.communicate(timeout=5)[1]
                    else:
                        self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    # QUIT never landed, which is the failure the test
                    # above is already reporting. Leaving the terminal
                    # running would hand it to every test after this one -
                    # a spinning one keeps a core busy for the rest of the
                    # suite - so it goes down here either way.
                    self.process.kill()
                    self.process.wait()
        self.stream.close()
        self.socket.close()

    def stderr_text(self):
        """Everything the terminal wrote to stderr, valid after close().

        Requires capture_stderr=True at construction; without it the
        child's stderr is the runner's and there is nothing to return.
        """
        if self._stderr is None:
            raise RuntimeError("construct with capture_stderr=True to read stderr")
        return self._stderr

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _readline(self):
        while True:
            newline = self._receive_buffer.find(b"\n")
            if newline >= 0:
                line = bytes(self._receive_buffer[:newline])
                del self._receive_buffer[:newline + 1]
                return line.decode("ascii")
            # The bound goes here rather than on the socket, which has to
            # stay blocking for writes to arrive whole; see REPLY_TIMEOUT.
            if not select.select([self.socket], (), (), REPLY_TIMEOUT)[0]:
                raise RuntimeError(f"shitty gave no reply within {REPLY_TIMEOUT}s")
            chunk = self.socket.recv(64 * 1024)
            if not chunk:
                raise RuntimeError(f"shitty exited with {self.process.poll()}")
            self._receive_buffer.extend(chunk)

    def command(self, command):
        self.stream.write(command.encode("ascii") + b"\n")
        response = self._readline()
        if response.startswith("ERR "):
            raise RuntimeError(response[4:])
        if response != "OK":
            raise RuntimeError(f"unexpected response: {response}")

    def private_mode(self, mode):
        self.stream.write(f"PRIVATE_MODE {mode}\n".encode("ascii"))
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid private mode response")
        return bool(int(response[1]))

    def ansi_mode(self, mode):
        self.stream.write(f"ANSI_MODE {mode}\n".encode("ascii"))
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid ansi mode response")
        return bool(int(response[1]))

    def options(self):
        self.stream.write(b"OPTIONS\n")
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid options response")
        result = {}
        for field in response[1:]:
            name, value = field.split("=", 1)
            # Most fields are numbers, but not all of them: background_blur
            # prints the mode's name, because the numeric value of the
            # enum behind it is deliberately not a contract. int() applied
            # to every field would turn that into a ValueError in every
            # caller of options(), not only in the ones that read it.
            try:
                result[name] = int(value)
            except ValueError:
                result[name] = value
        return result

    def argv(self):
        encoded = self._read_hex_response("ARGV")
        return [os.fsdecode(value) for value in encoded.split(b"\0")]

    def launch_command(self):
        fields = self._read_hex_response("LAUNCH_COMMAND").split(b"\0")
        return os.fsdecode(fields[0]), [os.fsdecode(value) for value in fields[1:]]

    def load_font(self, *families):
        request = b"\0".join(os.fsencode(family) for family in families)
        self.stream.write(b"FONT_LOAD " + request.hex().encode() + b"\n")
        response = self._readline().split()
        if len(response) != 6 or response[0] != "OK":
            raise RuntimeError("invalid font load response")
        values = tuple(map(int, response[1:]))
        return dict(zip(
            ("px", "py", "bold", "italic", "bold_italic"),
            values,
        ))

    def render_image(self, *families):
        request = b"\0".join(os.fsencode(family) for family in families)
        self.stream.write(b"RENDER_IMAGE " + request.hex().encode() + b"\n")
        response = self._readline().split()
        if len(response) != 4 or response[0] != "OK":
            raise RuntimeError("invalid render image response")
        width = int(response[1])
        height = int(response[2])
        pixels = bytes.fromhex(response[3])
        if len(pixels) != width * height * 3:
            raise RuntimeError("invalid render image size")
        return width, height, pixels

    def write(self, output):
        self.command("WRITE " + output.hex())

    def measure_widths(self, *outputs):
        if not outputs:
            raise ValueError("empty width measurement")
        request = " ".join(output.hex() for output in outputs)
        replies = self._read_hex_response("MEASURE_WIDTHS " + request)
        positions = []
        offset = 0
        while offset < len(replies):
            if replies[offset : offset + 2] != b"\x1b[":
                raise RuntimeError("invalid cursor position response")
            end = replies.find(b"R", offset + 2)
            if end < 0:
                raise RuntimeError("truncated cursor position response")
            fields = replies[offset + 2 : end].split(b";")
            if len(fields) != 2:
                raise RuntimeError("invalid cursor position fields")
            row, column = map(int, fields)
            positions.append((column - 1, row - 1))
            offset = end + 1
        if len(positions) != len(outputs):
            raise RuntimeError("missing cursor position response")
        return positions

    def input(self, data):
        self.command("INPUT " + data.hex())

    def user_input(self, data):
        self.command("USER_INPUT " + data.hex())

    def hard_reset(self):
        self.command("HARD_RESET")

    def spawn(self, *arguments):
        encoded = b"\0".join(os.fsencode(argument) for argument in arguments)
        self.command("SPAWN " + encoded.hex())

    def pump(self):
        self.command("PUMP")

    def read_pty(self):
        self.stream.write(b"READ_PTY\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid PTY read response")
        return bool(int(response[1]))

    def read_child_output(self):
        return self._read_hex_response("READ_CHILD_OUTPUT")

    def script_pty_reads(self, *outcomes):
        tokens = []
        for outcome in outcomes:
            if isinstance(outcome, bytes):
                tokens.append("d" + outcome.hex())
            elif outcome == "eof":
                tokens.append("z")
            elif (
                isinstance(outcome, tuple)
                and len(outcome) == 2
                and outcome[0] == "error"
            ):
                tokens.append("e" + str(outcome[1]))
            else:
                raise ValueError(f"invalid PTY read outcome: {outcome!r}")
        if not tokens:
            raise ValueError("empty PTY read script")
        self.command("PTY_READ_SCRIPT " + " ".join(tokens))

    def script_pty_repeat(self, byte, count, eof=False):
        if not 0 <= byte <= 255 or count <= 0:
            raise ValueError("invalid repeated PTY input")
        self.command(f"PTY_READ_REPEAT {byte} {count} {int(eof)}")

    def pending_scripted_pty_read_bytes(self):
        self.stream.write(b"PENDING_SCRIPTED_PTY_READ_BYTES\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid scripted PTY byte count")
        return int(response[1])

    def wait_read_pty(self):
        self.command("WAIT_READ_PTY")

    def _read_image_reply(self, command):
        self.stream.write(command + b"\n")
        response = self._readline().split()
        if len(response) != 4 or response[0] != "OK":
            raise RuntimeError("invalid image response")
        width = int(response[1])
        height = int(response[2])
        pixels = bytes.fromhex(response[3])
        if len(pixels) != width * height * 3:
            raise RuntimeError("invalid image size")
        return width, height, pixels

    def reference_image(self):
        return self._read_image_reply(b"REFERENCE_IMAGE")

    def vulkan_image(self):
        return self._read_image_reply(b"VULKAN_IMAGE")

    def vulkan_shadow(self):
        self.stream.write(b"VULKAN_SHADOW\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid vulkan shadow response")
        return bool(int(response[1]))

    def shape_generation(self):
        self.stream.write(b"SHAPE_GENERATION\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid shape generation response")
        return int(response[1])

    def fail_next_present(self):
        self.command("FAIL_NEXT_PRESENT")

    def fail_next_font_change(self):
        self.command("FAIL_NEXT_FONT_CHANGE")

    def present(self):
        self.command("PRESENT")

    def repaint(self):
        self.command("REPAINT")

    def gpu_attribute_masks(self):
        self.stream.write(b"GPU_ATTRIBUTE_MASKS\n")
        response = self._readline().split()
        if len(response) != 3 or response[0] != "OK":
            raise RuntimeError("invalid GPU attribute response")
        return tuple(map(int, response[1:]))

    def child_status(self):
        self.stream.write(b"CHILD_STATUS\n")
        response = self._readline().split()
        if len(response) != 3 or response[0] != "OK":
            raise RuntimeError("invalid child status response")
        return None if bool(int(response[1])) else int(response[2])

    def poll_child(self):
        self.stream.write(b"POLL_CHILD\n")
        response = self._readline().split(" ", 3)
        if len(response) != 4 or response[0] != "OK":
            raise RuntimeError("invalid child poll response")
        status = None if bool(int(response[1])) else int(response[2])
        return status, bytes.fromhex(response[3]).decode("ascii")

    def wait_child(self, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            status, screen = self.poll_child()
            if status is not None:
                return status, screen
            if time.monotonic() >= deadline:
                raise TimeoutError("child did not exit")
            time.sleep(0.005)

    def write_chunks(self, *chunks):
        for chunk in chunks:
            self.write(chunk)

    def feed_chunks(self, *chunks):
        if not chunks or any(not chunk for chunk in chunks):
            raise ValueError("PTY chunks must be non-empty")
        self.command(
            "WRITE_CHUNKS " + " ".join(chunk.hex() for chunk in chunks)
        )

    def page_up(self):
        self.command("PAGE_UP")

    def page_down(self):
        self.command("PAGE_DOWN")

    def wheel_up(self, count=1):
        for _ in range(count):
            self.command("WHEEL_UP")

    def wheel_down(self, count=1):
        for _ in range(count):
            self.command("WHEEL_DOWN")

    def scroll(
        self, x, y, modifiers=0, pixel_x=2, pixel_y=2,
        phase="none", precise=False, momentum=False, time=0,
    ):
        phases = {"none": 0, "begin": 1, "update": 2, "end": 3,
                  "cancel": 4}
        if phase not in phases:
            raise ValueError(f"invalid scroll phase: {phase}")
        self.command(
            f"SCROLL {x!r} {y!r} {modifiers} {pixel_x} {pixel_y} "
            f"{phases[phase]} {int(precise)} {int(momentum)} {time!r}"
        )

    def pointer(self, x, y, modifiers=0, scale_x=1, scale_y=1):
        self.command(
            f"POINTER {x!r} {y!r} {modifiers} {scale_x!r} {scale_y!r}"
        )

    def button(
        self, button, pressed, x=2, y=2, modifiers=0, time=0,
        scale_x=1, scale_y=1,
    ):
        return self._read_hex_response(
            f"BUTTON {button} {int(pressed)} {x!r} {y!r} "
            f"{modifiers} {time!r} {scale_x!r} {scale_y!r}"
        )

    def grapheme_breaks(self, *codepoints):
        encoded = " ".join(f"{codepoint:X}" for codepoint in codepoints)
        self.stream.write(f"GRAPHEME_BREAKS {encoded}\n".encode("ascii"))
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid grapheme break response")
        return tuple(value == "1" for value in response[1])

    def resize(self, columns, rows):
        self.command(f"RESIZE {columns} {rows}")
        self._window_info["pixel_width"] = columns * self._glyph_px + 4
        self._window_info["pixel_height"] = rows * self._glyph_py + 4

    def resize_pixels(self, width, height):
        self.command(f"RESIZE_PIXELS {width} {height}")
        self._window_info["pixel_width"] = width
        self._window_info["pixel_height"] = height

    def window_info(self, **values):
        unknown = values.keys() - self._window_info.keys()
        if unknown:
            raise ValueError(f"unknown window info fields: {sorted(unknown)}")
        self._window_info.update(values)
        info = self._window_info
        self.command(
            "WINDOW_INFO "
            f"{info['x']} {info['y']} "
            f"{info['pixel_width']} {info['pixel_height']} "
            f"{info['screen_width']} {info['screen_height']} "
            f"{int(info['iconified'])} {int(info['maximized'])} "
            f"{int(info['fullscreen'])} {int(info['tiled'])}"
        )

    def new_session(self):
        """Open another terminal behind this window, and show it."""
        self.command("NEW_SESSION")

    def close_session(self, index):
        """Close the session at index; a neighbour becomes active."""
        self.command(f"CLOSE_SESSION {index}")

    def split(self, direction="V"):
        """Divide the focused pane, as the split chord does.

        Needs the terminal started with extra_arguments=("-panes",): the
        option is off by default, and without it SessionSet declines the
        split and the command answers ERR rather than moving the pane
        count.
        """
        self.command(f"SPLIT {direction}")

    def _pane_numbers(self, command, count):
        """`count` numbers from a pane query, or the terminal's own ERR.

        A pane index nothing answers to is a failed command and not a
        dead terminal, so the message it comes back with is worth
        keeping: "no such pane" reads better than a shape complaint.
        """
        self.stream.write(command.encode("ascii") + b"\n")
        response = self._readline().split()
        if response and response[0] == "ERR":
            raise RuntimeError(" ".join(response[1:]))
        if len(response) != count + 1 or response[0] != "OK":
            raise RuntimeError(f"invalid response to {command}")
        return tuple(map(int, response[1:]))

    def pane_count(self):
        """How many panes the active tab shows."""
        return self._pane_numbers("PANE_COUNT", 1)[0]

    def focus_pane(self, index):
        """Give pane index the input focus."""
        self.command(f"FOCUS_PANE {index}")

    def winsize_pane(self, index):
        """(columns, rows) of pane index, read off that pane's own pty."""
        return self._pane_numbers(f"WINSIZE_PANE {index}", 2)

    def screen_text_pane(self, index):
        """Pane index's visible screen, in the shape screen_text() uses.

        Read from that pane's own screen rather than from the renderer,
        which retains the cells of whichever pane it drew last and so has
        one answer for the whole window.
        """
        return self._read_hex_response(f"SCREEN_TEXT_PANE {index}").decode("ascii")

    def write_to_pane(self, index, output):
        """Pane index's shell produced bytes: they parse into that pane's
        terminal alone, focused or not."""
        self.command(f"PTY_WRITE_PANE {index} " + output.hex())

    def write_to(self, index, output):
        """Session index's shell produced bytes: they parse into that
        session's terminal whether or not it is the one shown."""
        self.command(f"WRITE_SESSION {index} " + output.hex())

    def read_input_of(self, index):
        """Everything written into session index's pty since last read."""
        return self._read_hex_response(f"READ_INPUT_SESSION {index}")

    def window_title(self):
        return self._read_hex_response("WINDOW_TITLE").decode("utf-8")

    def _chord(self, character, modifiers):
        self.frontend_key_event(ord(character), 1, modifiers=modifiers)
        self.frontend_key_event(ord(character), 0, modifiers=modifiers)

    def chord_next_tab(self):
        """The next-tab chord, routed like real keyboard input."""
        self._chord("]", 8 | 1 if TEST_PLATFORM == "cocoa" else 2 | 1)

    def chord_prev_tab(self):
        self._chord("[", 8 | 1 if TEST_PLATFORM == "cocoa" else 2 | 1)

    def chord_close_tab(self):
        self._chord("W", 8 if TEST_PLATFORM == "cocoa" else 2 | 1)

    def chord_clear(self):
        self._chord("L", 8 if TEST_PLATFORM == "cocoa" else 2 | 1)

    def _font_chord(self, character, text, modifiers):
        """One font-size chord, as this platform binds it.

        Cmd+key on macOS against a Ctrl form elsewhere
        (input_bindings.cpp), and elsewhere the binding also wants the
        text event the layout produces under that chord - '+' under
        Ctrl+Shift+=, not '='. A test that spells one platform's pair by
        hand passes on that platform and silently does nothing on the
        other: the font never changes, and what the test then measures is
        a terminal that ignored it.
        """
        if TEST_PLATFORM == "cocoa":
            self._chord(character, 8)
            return
        self.frontend_key_event(ord(character), 1, modifiers=modifiers)
        self.frontend_text_event(text, modifiers=modifiers)
        self.frontend_key_event(ord(character), 0, modifiers=modifiers)

    def chord_font_increase(self):
        self._font_chord("=", "+", 2 | 1)

    def chord_font_decrease(self):
        self._font_chord("-", "-", 2)

    def chord_font_reset(self):
        self._font_chord("0", "0", 2)

    def session_state(self):
        """(session count, active index) for the window's terminals."""
        self.stream.write(b"SESSION_STATE\n")
        count, active = self._readline().split()
        return int(count), int(active)

    def winsize(self):
        self.stream.write(b"WINSIZE\n")
        response = self._readline().split()
        if len(response) != 3 or response[0] != "OK":
            raise RuntimeError("invalid winsize response")
        return tuple(map(int, response[1:]))

    def winsize_full(self):
        self.stream.write(b"WINSIZE_FULL\n")
        response = self._readline().split()
        if len(response) != 5 or response[0] != "OK":
            raise RuntimeError("invalid full winsize response")
        return tuple(map(int, response[1:]))

    def font_state(self):
        self.stream.write(b"FONT_STATE\n")
        response = self._readline().split()
        if len(response) != 10 or response[0] != "OK":
            raise RuntimeError("invalid font state response")
        return tuple(map(int, response[1:]))

    def last_update(self):
        self.stream.write(b"LAST_UPDATE\n")
        response = self._readline().split()
        if len(response) != 3 or response[0] != "OK":
            raise RuntimeError("invalid last update response")
        return tuple(map(int, response[1:]))

    def last_update_rows(self):
        self.stream.write(b"LAST_UPDATE_ROWS\n")
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid last update rows response")
        return tuple(map(int, response[1:]))

    def frontend_content_scale(
        self,
        x_numerator,
        x_denominator,
        y_numerator=None,
        y_denominator=None,
    ):
        if y_numerator is None:
            y_numerator = x_numerator
        if y_denominator is None:
            y_denominator = x_denominator
        self.command(
            f"FRONTEND_SCALE {x_numerator} {x_denominator} "
            f"{y_numerator} {y_denominator}"
        )

    def rectangle_origin(self):
        self.stream.write(b"RECTANGLE_ORIGIN\n")
        response = self._readline().split()
        if len(response) != 5 or response[0] != "OK":
            raise RuntimeError("invalid rectangle origin response")
        return tuple(map(int, response[1:]))

    def key(self, name, modifiers=0):
        self.command(f"KEY {name} {modifiers}")

    def char(self, character, modifiers=0):
        if isinstance(character, str):
            character = ord(character)
        self.command(f"CHAR {character} {modifiers}")

    def control_character(self, character, shifted=False):
        if isinstance(character, str):
            character = ord(character)
        self.stream.write(
            f"CONTROL_CHARACTER {character} {int(shifted)}\n".encode()
        )
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid control character response")
        return int(response[1])

    def frontend_control(self, character, shifted=False, alt=False):
        if isinstance(character, str):
            character = ord(character)
        self.command(
            f"FRONTEND_CONTROL {character} {int(shifted)} {int(alt)}"
        )

    def frontend_key_event(self, key, action, scancode=0, modifiers=0):
        self.command(
            f"FRONTEND_KEY_EVENT {key} {scancode} {action} {modifiers}"
        )

    def layout_key(self, key, layout, base, modifiers=0, action=1, shifted=0):
        if isinstance(key, str):
            key = ord(key)
        if isinstance(layout, str):
            layout = ord(layout)
        if isinstance(base, str):
            base = ord(base)
        if isinstance(shifted, str):
            shifted = ord(shifted)
        self.command(
            f"FRONTEND_LAYOUT_KEY {key} {action} {modifiers} "
            f"{layout} {shifted} {base}"
        )

    def frontend_text_event(self, character, modifiers=0):
        if isinstance(character, str):
            character = ord(character)
        self.command(f"FRONTEND_TEXT_EVENT {character} {modifiers}")

    def kitty_key(self, key, shifted=0, base=0, modifiers=0, event=1):
        self.command(
            f"KITTY_KEY {key} {shifted} {base} {modifiers} {event}"
        )

    def kitty_special(self, name, modifiers=0, event=1):
        self.command(f"KITTY_SPECIAL {name} {modifiers} {event}")

    def paste(self, data):
        self.command("PASTE " + data.hex())

    def drop(self, data):
        self.command("DROP " + data.hex())

    def drop_path(self, data):
        self.command("DROP_PATH " + data.hex())

    def paste_clipboard(self, primary=False):
        self.stream.write(
            f"PASTE_CLIPBOARD {int(primary)}\n".encode("ascii")
        )
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid clipboard paste response")
        self.command("FLUSH_OUTPUT")
        return response[1] == "1"

    def focus(self, focused):
        self.command(f"FOCUS {int(focused)}")

    def pointer_presence(self, present):
        self.command(f"POINTER_PRESENCE {int(present)}")

    def highlight_release(self, end_x, end_y, mouse_x, mouse_y):
        self.command(
            f"HIGHLIGHT_RELEASE {end_x} {end_y} {mouse_x} {mouse_y}"
        )

    def locator_position(self, column, row, pixel_x, pixel_y, buttons=0):
        self.command(
            f"LOCATOR_POSITION {column} {row} {pixel_x} {pixel_y} {buttons}"
        )

    def locator_button(self, button, pressed):
        self.command(f"LOCATOR_BUTTON {button} {int(pressed)}")

    def sync_timeout(self):
        self.command("SYNC_TIMEOUT")

    def blink_tick(self):
        self.command("BLINK_TICK")

    def selection_autoscroll_tick(self):
        self.command("SELECTION_AUTOSCROLL_TICK")

    def select_start(self, column, row, cycle=False):
        self.command(f"SELECT_START {column} {row} {int(cycle)}")

    def select_extend(self, column, row, cycle=False):
        self.command(f"SELECT_EXTEND {column} {row} {int(cycle)}")

    def select_update(self, column, row):
        self.command(f"SELECT_UPDATE {column} {row}")

    def select_rectangular(self):
        self.command("SELECT_RECTANGULAR")

    def selection_state(self):
        self.stream.write(b"SELECTION_STATE\n")
        response = self._readline().split()
        if len(response) != 11 or response[0] != "OK":
            raise RuntimeError("invalid selection state response")
        values = tuple(map(int, response[1:]))
        return {
            "raw": values[:4],
            "raw_rectangular": bool(values[4]),
            "snapped": values[5:9],
            "snapped_rectangular": bool(values[9]),
        }

    def _read_hex_response(self, command):
        self.stream.write(command.encode("ascii") + b"\n")
        response = self._readline().split(" ", 1)
        if response[0] == "ERR":
            # What the terminal refused, said in its own words - the way
            # command() reports it. A bad pane index arrives here.
            raise RuntimeError(response[1] if len(response) == 2 else "")
        if response[0] != "OK":
            raise RuntimeError(f"invalid response to {command}")
        return bytes.fromhex(response[1]) if len(response) == 2 else b""

    def select_finish(self):
        return self._read_hex_response("SELECT_FINISH")

    def select_clear(self):
        self.command("SELECT_CLEAR")

    def has_selection(self):
        self.stream.write(b"HAS_SELECTION\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid selection availability response")
        return bool(int(response[1]))

    def hyperlink(self, column, row):
        return self.hyperlink_bytes(column, row).decode()

    def hyperlink_bytes(self, column, row):
        return self._read_hex_response(f"HYPERLINK {column} {row}")

    def hyperlink_count(self):
        self.stream.write(b"HYPERLINK_COUNT\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid hyperlink count response")
        return int(response[1])

    def desktop_state(self):
        self.stream.write(b"DESKTOP_STATE\n")
        response = self._readline().split()
        if len(response) != 7 or response[0] != "OK":
            raise RuntimeError("invalid desktop state response")
        return {
            "icon": int(response[1]),
            "open_count": int(response[2]),
            "hovered_hyperlink": int(response[3]),
            "hovered_link_begin": int(response[4]),
            "hovered_link_end": int(response[5]),
            "opened_uri": (
                b"" if response[6] == "-" else bytes.fromhex(response[6])
            ),
        }

    def read_actions(self):
        return self._read_hex_response("READ_ACTIONS").decode().splitlines()

    def state(self):
        self.stream.write(b"STATE\n")
        response = self._readline().split()
        if len(response) != 5 or response[0] != "OK":
            raise RuntimeError("invalid state response")
        return tuple(map(int, response[1:]))

    def protocol_state(self):
        self.stream.write(b"PROTOCOL_STATE\n")
        response = self._readline().split()
        if len(response) != 6 or response[0] != "OK":
            raise RuntimeError("invalid protocol state response")
        return tuple(map(int, response[1:]))

    def cursor_state(self):
        self.stream.write(b"CURSOR_STATE\n")
        response = self._readline().split()
        if len(response) != 4 or response[0] != "OK":
            raise RuntimeError("invalid cursor state response")
        return tuple(map(int, response[1:]))

    def cursor_pending_wrap(self):
        self.stream.write(b"CURSOR_PENDING_WRAP\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid cursor pending-wrap response")
        return bool(int(response[1]))

    def cursor_at_prompt(self):
        self.stream.write(b"CURSOR_AT_PROMPT\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid cursor-at-prompt response")
        return bool(int(response[1]))

    def row_semantic(self, row):
        self.stream.write(f"ROW_SEMANTIC {row}\n".encode("ascii"))
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid row-semantic response")
        return int(response[1])

    def semantic_click(self):
        self.stream.write(b"SEMANTIC_CLICK\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid semantic-click response")
        return int(response[1])

    def conformance_state(self):
        self.stream.write(b"CONFORMANCE_STATE\n")
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid conformance state response")
        result = {}
        for field in response[1:]:
            name, value = field.split("=", 1)
            result[name] = value if name == "screen" else bool(int(value))
        return result

    def pen_state(self):
        self.stream.write(b"PEN_STATE\n")
        response = self._readline().split()
        if len(response) != 10 or response[0] != "OK":
            raise RuntimeError("invalid pen state response")
        values = tuple(map(int, response[1:]))
        flags = values[0]
        return PenState(
            bool(flags & 4),
            bool(flags & 128),
            bool(flags & 8),
            bool(flags & 16),
            (flags >> 12) & 7,
            bool(flags & 256),
            bool(flags & 512),
            bool(flags & 1024),
            bool(flags & 32),
            values[1:4],
            values[4:7],
            values[7],
            values[8],
        )

    def utf8_push(self, payload):
        self.stream.write(b"UTF8_PUSH " + payload.hex().encode("ascii") + b"\n")
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid UTF-8 decoder response")
        return tuple(int(value, 16) for value in response[1:])

    def utf8_flush(self):
        self.stream.write(b"UTF8_FLUSH\n")
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid UTF-8 decoder flush response")
        return tuple(int(value, 16) for value in response[1:])

    def utf8_reset(self):
        self.command("UTF8_RESET")

    def codepoint_widths(self, *codepoints):
        if not codepoints:
            raise ValueError("empty codepoint width request")
        request = " ".join(f"{codepoint:x}" for codepoint in codepoints)
        self.stream.write(
            ("CODEPOINT_WIDTHS " + request + "\n").encode("ascii")
        )
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid codepoint width response")
        return tuple(map(int, response[1:]))

    def parser_trace_on(self):
        self.command("PARSER_TRACE_ON")

    def parser_trace_clear(self):
        self.command("PARSER_TRACE_CLEAR")

    def parser_trace(self):
        self.stream.write(b"READ_PARSER_TRACE\n")
        response = self._readline().split(maxsplit=1)
        if not response or response[0] != "OK":
            raise RuntimeError("invalid parser trace response")
        payload = bytes.fromhex(response[1]).decode("ascii") if len(response) > 1 else ""
        result = []
        for line in payload.splitlines():
            event, encoded = line.split(" ", 1)
            result.append((event, bytes.fromhex(encoded)))
        return result

    def render_state(self):
        self.stream.write(b"RENDER_STATE\n")
        response = self._readline().split()
        if len(response) != 13 or response[0] != "OK":
            raise RuntimeError("invalid renderer state response")
        values = tuple(map(int, response[1:]))
        return RenderState(
            bool(values[0]),
            bool(values[1]),
            bool(values[2]),
            values[3],
            values[4:7],
            values[7:10],
            values[10],
            values[11],
        )

    def charset_state(self):
        self.stream.write(b"CHARSET_STATE\n")
        response = self._readline().split()
        if len(response) != 5 or response[0] != "OK":
            raise RuntimeError("invalid charset state response")
        return tuple(map(int, response[1:]))

    def mouse_encode(
        self,
        encoding,
        event,
        modifiers,
        motion_button,
        button,
        column,
        row,
    ):
        return self._read_hex_response(
            "MOUSE_ENCODE "
            f"{encoding} {event} {modifiers} {motion_button} "
            f"{button} {column} {row}"
        )

    def set_primary_selection(self, content, auto_copy=False):
        self.command(f"SET_PRIMARY {int(auto_copy)} {content.hex()}")

    def set_system_clipboard(self, content):
        self.command("SET_SYSTEM " + content.hex())

    def set_clipboard_chunk(self, size):
        self.command(f"SET_CLIPBOARD_CHUNK {size}")

    def get_selection(self, primary):
        return self._read_hex_response(f"GET_SELECTION {int(primary)}")

    def current_cwd(self):
        return self._read_hex_response("GET_CWD")

    def osc7_cwd(self, argument):
        return self._read_hex_response("OSC7_CWD " + argument.hex())

    def read_input(self):
        return self._read_hex_response("READ_INPUT")

    def read_all_input(self):
        output = bytearray(self.read_input())
        while True:
            finished = self.flush_output_result()
            output.extend(self.read_input())
            if finished:
                return bytes(output)

    def preedit(self, text, cursor_begin=-1, cursor_end=-1):
        encoded = text.encode() if isinstance(text, str) else text
        payload = encoded.hex() if encoded else "-"
        self.command(f"PREEDIT {payload} {cursor_begin} {cursor_end}")

    def presented_pixel(self, x, y):
        self.stream.write(f"PRESENTED_PIXEL {x} {y}\n".encode("ascii"))
        response = self._readline().split()
        if len(response) != 4 or response[0] != "OK":
            raise RuntimeError("invalid presented pixel response")
        return tuple(int(value) for value in response[1:])

    def screen_text(self):
        return self._read_hex_response("SCREEN_TEXT").decode("ascii")

    def all_text(self):
        encoded = self._read_hex_response("ALL_TEXT")
        if not encoded.endswith(b"\0"):
            raise RuntimeError("invalid all text response")
        return tuple(
            line.decode("utf-8")
            for line in encoded[:-1].split(b"\0")
        )

    def flush_output(self):
        self.command("FLUSH_OUTPUT")

    def flush_output_result(self):
        self.stream.write(b"FLUSH_OUTPUT_RESULT\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid PTY flush response")
        return bool(int(response[1]))

    def script_pty_writes(self, *outcomes):
        tokens = []
        for outcome in outcomes:
            if isinstance(outcome, int) and outcome > 0:
                tokens.append("n" + str(outcome))
            elif (
                isinstance(outcome, tuple)
                and len(outcome) == 2
                and outcome[0] == "error"
            ):
                tokens.append("e" + str(outcome[1]))
            else:
                raise ValueError(f"invalid PTY write outcome: {outcome!r}")
        if not tokens:
            raise ValueError("empty PTY write script")
        self.command("PTY_WRITE_SCRIPT " + " ".join(tokens))

    def read_written_pty(self):
        return self._read_hex_response("READ_WRITTEN_PTY")

    def service_pty(self, readable=False, writable=False):
        self.stream.write(
            f"SERVICE_PTY {int(readable)} {int(writable)}\n".encode()
        )
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid PTY service response")
        return bool(int(response[1]))

    def snapshot(self):
        return self._snapshot("SNAPSHOT", False)

    def model_snapshot(self):
        return self._snapshot("MODEL_SNAPSHOT", True)

    def model_digest(self):
        self.stream.write(b"MODEL_DIGEST\n")
        response = self._readline().split()
        if len(response) != 3 or response[0] != "OK":
            raise RuntimeError("invalid model digest response")
        return tuple(int(value, 16) for value in response[1:])

    def tab_stop(self, column):
        self.stream.write(f"TAB_STOP {column}\n".encode())
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid tab stop response")
        return bool(int(response[1]))

    def tab_stops(self, columns=None):
        if columns is None:
            columns = self.snapshot().columns
        self.stream.write(f"TAB_STOPS {columns}\n".encode())
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid tab stops response")
        return tuple(value == "1" for value in response[1])

    def set_wrapped(self, row):
        self.command(f"SET_WRAPPED {row}")

    def scrollback_state(self):
        self.stream.write(b"SCROLLBACK_STATE\n")
        response = self._readline().split()
        if len(response) != 5 or response[0] != "OK":
            raise RuntimeError("invalid scrollback state response")
        return tuple(map(int, response[1:]))

    def _snapshot(self, command, detailed):
        self.stream.write(command.encode("ascii") + b"\n")
        response = self._readline().split(" ", 13)
        if len(response) != 14 or response[0] != "OK":
            raise RuntimeError("invalid snapshot response")
        (
            columns,
            rows,
            cursor_x,
            cursor_y,
            style,
            offset,
            refresh_count,
            selection_tl_x,
            selection_tl_y,
            selection_br_x,
            selection_br_y,
            rectangular_selection,
        ) = map(
            int, response[1:13]
        )
        encoded_cells = response[13]
        record_size = 82 if detailed else 50
        cells = []
        offset_in_cells = 0
        for _ in range(columns * rows):
            if offset_in_cells + record_size > len(encoded_cells):
                raise RuntimeError("invalid snapshot cell count")
            record = encoded_cells[
                offset_in_cells : offset_in_cells + record_size
            ]
            flags = int(record[8:16], 16)
            foreground_index = -2
            background_index = -2
            underline_index = -2
            grapheme = ()
            if detailed:
                def signed(field):
                    value = int(field, 16)
                    return value - (1 << 32) if value & (1 << 31) else value

                foreground_index = signed(record[50:58])
                background_index = signed(record[58:66])
                underline_index = signed(record[66:74])
                grapheme_count = int(record[74:82], 16)
                grapheme_end = offset_in_cells + record_size + 8 * grapheme_count
                if grapheme_end > len(encoded_cells):
                    raise RuntimeError("invalid snapshot grapheme count")
                grapheme = tuple(
                    int(encoded_cells[index : index + 8], 16)
                    for index in range(
                        offset_in_cells + record_size, grapheme_end, 8
                    )
                )
                offset_in_cells = grapheme_end
            else:
                offset_in_cells += record_size
            cells.append(
                Cell(
                    char=chr(int(record[0:8], 16)),
                    double_width=bool(flags & 1),
                    double_width_continuation=bool(flags & 2),
                    bold=bool(flags & 4),
                    italic=bool(flags & 8),
                    underline=bool(flags & 16),
                    underline_style=(flags >> 12) & 7,
                    faint=bool(flags & 128),
                    blink=bool(flags & 256),
                    conceal=bool(flags & 512),
                    strike=bool(flags & 1024),
                    overline=bool(flags & 2048),
                    inverse=bool(flags & 32),
                    wrapped=bool(flags & 64),
                    foreground=tuple(
                        int(record[k : k + 2], 16) for k in (16, 18, 20)
                    ),
                    background=tuple(
                        int(record[k : k + 2], 16) for k in (22, 24, 26)
                    ),
                    underline_color=tuple(
                        int(record[k : k + 2], 16) for k in (28, 30, 32)
                    ),
                    hyperlink=int(record[34:42], 16),
                    semantic=int(record[42:50], 16),
                    protected=bool(flags & 32768),
                    line_attribute=(flags >> 16) & 3,
                    drawn=bool(flags & (1 << 18)),
                    foreground_index=foreground_index,
                    background_index=background_index,
                    underline_index=underline_index,
                    grapheme=grapheme,
                )
            )
        if offset_in_cells != len(encoded_cells):
            raise RuntimeError("invalid snapshot cell count")
        text = "".join(cell.char for cell in cells)
        lines = [
            text[row * columns : (row + 1) * columns]
            for row in range(rows)
        ]
        return Snapshot(
            columns,
            rows,
            cursor_x,
            cursor_y,
            style,
            offset,
            refresh_count,
            (selection_tl_x, selection_tl_y, selection_br_x, selection_br_y),
            bool(rectangular_selection),
            cells,
            lines,
        )
