# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import socket
import subprocess
import os
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHITTY = Path(os.environ.get("SHITTY_TEST_BINARY", ROOT / "st"))


def run_startup_failure(font_size_env=None, extra_arguments=()):
    parent, child = socket.socketpair()
    environment = os.environ.copy()
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


class Shitty:
    def __init__(
        self, columns=80, rows=24, save_lines=500,
        glyph_px=1, glyph_py=1,
        font_size_env=None, extra_arguments=(),
    ):
        parent, child = socket.socketpair()
        self.socket = parent
        self.stream = parent.makefile("rwb", buffering=0)
        self._receive_buffer = bytearray()
        child_environment = os.environ.copy()
        child_environment["SHITTY_TEST_GLYPH"] = f"{glyph_px}x{glyph_py}"
        if font_size_env is None:
            child_environment.pop("SHITTY_FONT_SIZE", None)
        else:
            child_environment["SHITTY_FONT_SIZE"] = str(font_size_env)
        self.process = subprocess.Popen(
            [
                str(SHITTY),
                "--test-fd",
                str(child.fileno()),
                "-geometry",
                f"{columns}x{rows}",
                "-saveLines",
                str(save_lines),
                *map(str, extra_arguments),
            ],
            pass_fds=(child.fileno(),),
            env=child_environment,
        )
        self._glyph_px = glyph_px
        self._glyph_py = glyph_py
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
        }
        if self._readline() != "READY":
            raise RuntimeError("shitty test mode did not become ready")

    def close(self):
        if self.process.poll() is None:
            try:
                self.command("QUIT")
            finally:
                self.process.wait(timeout=5)
        self.stream.close()
        self.socket.close()

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

    def options(self):
        self.stream.write(b"OPTIONS\n")
        response = self._readline().split()
        if not response or response[0] != "OK":
            raise RuntimeError("invalid options response")
        result = {}
        for field in response[1:]:
            name, value = field.split("=", 1)
            result[name] = int(value)
        return result

    def argv(self):
        encoded = self._read_hex_response("ARGV")
        return [os.fsdecode(value) for value in encoded.split(b"\0")]

    def launch_command(self):
        fields = self._read_hex_response("LAUNCH_COMMAND").split(b"\0")
        return os.fsdecode(fields[0]), [os.fsdecode(value) for value in fields[1:]]

    def resolve_fontconfig(self, family):
        encoded = self._read_hex_response(
            "FONTCONFIG_RESOLVE " + os.fsencode(family).hex()
        ).split(b"\0")
        if len(encoded) != 8:
            raise RuntimeError("invalid fontconfig response")
        result = {}
        for offset, name in enumerate(
            ("regular", "bold", "italic", "bold_italic")
        ):
            result[name] = os.fsdecode(encoded[2 * offset])
            result[name + "_index"] = int(encoded[2 * offset + 1])
        return result

    def font_face_metrics(self, filename, index, codepoint):
        request = b"\0".join(
            (
                os.fsencode(filename),
                str(index).encode(),
                str(codepoint).encode(),
            )
        )
        self.stream.write(
            b"FONT_FACE_METRICS " + request.hex().encode() + b"\n"
        )
        response = self._readline().split()
        if len(response) != 8 or response[0] != "OK":
            raise RuntimeError("invalid font face metrics response")
        return dict(zip(
            (
                "valid", "face_index", "has_glyph", "advance",
                "max_advance", "py", "baseline",
            ),
            map(int, response[1:]),
        ))

    def font_glyph_metrics(self, filename, index, codepoint):
        request = b"\0".join(
            (
                os.fsencode(filename),
                str(index).encode(),
                str(codepoint).encode(),
            )
        )
        self.stream.write(
            b"FONT_GLYPH_METRICS " + request.hex().encode() + b"\n"
        )
        response = self._readline().split()
        if len(response) != 9 or response[0] != "OK":
            raise RuntimeError("invalid font glyph metrics response")
        return dict(zip(
            (
                "px", "py", "baseline", "color", "length", "nonzero",
                "first_row", "last_row",
            ),
            map(int, response[1:]),
        ))

    def load_overlay(
        self, primary_filename, primary_index, overlay_filename, overlay_index
    ):
        request = b"\0".join(
            (
                os.fsencode(primary_filename),
                str(primary_index).encode(),
                os.fsencode(overlay_filename),
                str(overlay_index).encode(),
            )
        )
        self.stream.write(b"FONT_OVERLAY " + request.hex().encode() + b"\n")
        response = self._readline().split()
        if len(response) != 5 or response[0] != "OK":
            raise RuntimeError("invalid font overlay response")
        values = tuple(map(int, response[1:]))
        return dict(zip(("compatible", "px", "py", "baseline"), values))

    def load_font(self, family, double_width):
        request = b"\0".join(map(os.fsencode, (family, double_width)))
        self.stream.write(b"FONT_LOAD " + request.hex().encode() + b"\n")
        response = self._readline().split()
        if len(response) != 9 or response[0] != "OK":
            raise RuntimeError("invalid font load response")
        values = tuple(map(int, response[1:]))
        return dict(zip(
            (
                "px", "py", "bold", "italic", "bold_italic", "double_width",
                "regular_face_index", "double_width_face_index",
            ),
            values,
        ))

    def render_image(self, family, double_width):
        request = b"\0".join(map(os.fsencode, (family, double_width)))
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

    def fail_next_present(self):
        self.command("FAIL_NEXT_PRESENT")

    def present(self):
        self.command("PRESENT")

    def gpu_attribute_masks(self):
        self.stream.write(b"GPU_ATTRIBUTE_MASKS\n")
        response = self._readline().split()
        if len(response) != 4 or response[0] != "OK":
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

    def scroll(self, x, y, modifiers=0, pixel_x=2, pixel_y=2):
        self.command(
            f"SCROLL {x!r} {y!r} {modifiers} {pixel_x} {pixel_y}"
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
            f"{int(info['fullscreen'])}"
        )

    def winsize(self):
        self.stream.write(b"WINSIZE\n")
        response = self._readline().split()
        if len(response) != 3 or response[0] != "OK":
            raise RuntimeError("invalid winsize response")
        return tuple(map(int, response[1:]))

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

    def focus(self, focused):
        self.command(f"FOCUS {int(focused)}")

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

    def select_start(self, column, row):
        self.command(f"SELECT_START {column} {row}")

    def select_update(self, column, row):
        self.command(f"SELECT_UPDATE {column} {row}")

    def select_rectangular(self):
        self.command("SELECT_RECTANGULAR")

    def _read_hex_response(self, command):
        self.stream.write(command.encode("ascii") + b"\n")
        response = self._readline().split(" ", 1)
        if response[0] != "OK":
            raise RuntimeError(f"invalid response to {command}")
        return bytes.fromhex(response[1]) if len(response) == 2 else b""

    def select_finish(self):
        return self._read_hex_response("SELECT_FINISH")

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

    def read_actions(self):
        return self._read_hex_response("READ_ACTIONS").decode().splitlines()

    def read_printer(self):
        return self._read_hex_response("READ_PRINTER")

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

    def osc52(self, argument):
        self.stream.write(b"OSC52 " + argument.hex().encode() + b"\n")
        response = self._readline().split()
        if len(response) not in (5, 6) or response[0] != "OK":
            raise RuntimeError("invalid OSC 52 response")
        fields = tuple(bool(int(value)) for value in response[1:5])
        content = bytes.fromhex(response[5]) if len(response) == 6 else b""
        return fields + (content,)

    def osc52_reply(self, content, selector=b""):
        return self._read_hex_response(
            "OSC52_REPLY " + selector.hex() + " " + content.hex()
        )

    def osc52_policy(
        self,
        argument,
        allow_read=False,
        select_clipboard=False,
        primary=b"",
        clipboard=b"",
    ):
        payload = b"\0".join((argument, primary, clipboard))
        return self._read_hex_response(
            f"OSC52_POLICY {int(allow_read)} {int(select_clipboard)} "
            + payload.hex()
        )

    def set_primary_selection(self, content, auto_copy=False):
        self.command(f"SET_PRIMARY {int(auto_copy)} {content.hex()}")

    def set_system_clipboard(self, content):
        self.command("SET_SYSTEM " + content.hex())

    def get_selection(self, primary):
        return self._read_hex_response(f"GET_SELECTION {int(primary)}")

    def apply_clipboard_osc52(self, argument):
        self.command("APPLY_CLIPBOARD_OSC52 " + argument.hex())

    def osc7_cwd(self, argument):
        return self._read_hex_response("OSC7_CWD " + argument.hex())

    def read_input(self):
        return self._read_hex_response("READ_INPUT")

    def screen_text(self):
        return self._read_hex_response("SCREEN_TEXT").decode("ascii")

    def pending_output(self):
        self.stream.write(b"PENDING_OUTPUT\n")
        response = self._readline().split()
        if len(response) != 2 or response[0] != "OK":
            raise RuntimeError("invalid pending output response")
        return int(response[1])

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
