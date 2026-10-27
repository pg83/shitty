# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import errno
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from harness import TEST_PLATFORM, Shitty, run_startup_failure


RELEASE = 0
PRESS = 1
CAPS_LOCK = 16
NUM_LOCK = 32

KEY_ESCAPE = 256
KEY_F1 = 290
KEY_F6 = 295
KEY_KP_0 = 320
KEY_KP_5 = 325
KEY_KP_EQUAL = 336


UPSTREAM_CASES = (
    "Terminal.BlinkingCursor",
    "Terminal.IME.CursorVisibleDuringComposition",
    "Terminal.ModifierKeysDoNotScrollViewport",
    "Terminal.localPathAtMousePosition",
    "Terminal.AutoScrollOnUpdate",
    "Terminal.DECCARA",
    "Terminal.CaptureScreenBuffer",
    "Terminal.RIS",
    "Terminal.RIS.keepsFrozenModesAppliedToInputGenerator",
    "Terminal.RIS.resetsPassiveMouseTracking",
    "Terminal.mouse coordinate modes are one mutually-exclusive setting",
    "Terminal.forceRedraw keeps the cell size",
    "Terminal.clampedTotalPageSize",
    "Terminal.DECCOLM.doesNotDoubleCountStatusLine",
    "Terminal.DECSCPP.doesNotDoubleCountStatusLine",
    "Terminal.DECCOLM.resizesGridSynchronously",
    "Terminal.RIS.resetsDECCOLM",
    "Terminal.DECNCSM",
    "Terminal.SynchronizedOutput",
    "Terminal.XTPUSHCOLORS_and_XTPOPCOLORS",
    "Terminal.DECAC",
    "Terminal.DECATC",
    "Terminal.DECSTGLT",
    "Terminal.DECATC.doesNotRecolorTheStatusLine",
    "Terminal.UnderlineStyleClearing",
    "Terminal.selection_does_not_pad_wide_characters",
    "Terminal.a_one_column_selection_still_breaks_lines",
    "Terminal.selection_keeps_leading_blank_lines",
    "Terminal.CurlyUnderline",
    "Terminal.TextSelection",
    "Terminal.TextSelection_wrapped_line",
    "Terminal.ParsingBuffer",
    "Terminal.TrivialLineBufferIntegrity",
    "Terminal.BoxDrawingCharacters",
    "Terminal.smoothScrollExtraLines.zero_when_no_offset",
    "Terminal.smoothScrollExtraLines.one_when_offset_nonzero",
    "Terminal.screenTransitionProgress.no_transition_returns_1",
    "Terminal.cursorAnimationProgress.no_animation_returns_1",
    "Terminal.applySmoothScrollPixelDelta.accumulates_subline_offset",
    "Terminal.applySmoothScrollPixelDelta.converts_full_cell_to_scroll",
    "Terminal.applySmoothScrollPixelDelta.clamps_at_top_of_history",
    "Terminal.applySmoothScrollPixelDelta.returns_disabled_on_alternate_screen",
    "Terminal.onBufferScrolled.preserves_viewport_with_pixel_offset",
    "Terminal.momentumScroll.starts_on_end_with_velocity",
    "Terminal.momentumScroll.velocity_computation_is_correct",
    "Terminal.momentumScroll.no_start_below_threshold",
    "Terminal.momentumScroll.no_start_with_single_sample",
    "Terminal.momentumScroll.decelerates_over_ticks",
    "Terminal.momentumScroll.stops_at_min_velocity",
    "Terminal.momentumScroll.cancelled_by_begin",
    "Terminal.wheelGlide.single_notch_glides_over_frames",
    "Terminal.wheelGlide.rapid_notches_accumulate",
    "Terminal.wheelGlide.direction_reversal",
    "Terminal.wheelGlide.clamps_at_history_top",
    "Terminal.wheelGlide.inactive_on_alt_screen",
    "Terminal.wheelGlide.cancelled_by_reset",
    "Terminal.wheelGlide.gated_on_smoothScrolling_only",
    "Terminal.wheelGlide.nextRender_schedules_while_active",
    "Terminal.wheelGlide.opposing_notches_do_not_spin_forever",
    "Terminal.wheelGlide.reports_result_for_caller_fallthrough",
    "Terminal.momentumScroll.stray_update_cancels_active_glide",
    "Terminal.resizeScreen.minimal_one_by_one",
    "Terminal.resizeScreen.minimal_one_by_one.with_status_line",
    "Terminal.momentumScroll.cancelled_by_resize",
    "Terminal.momentumScroll.disabled_when_setting_off",
    "Terminal.momentumScroll.disabled_when_smooth_scrolling_off",
    "Terminal.momentumScroll.noPhase_never_triggers_momentum",
    "Terminal.momentumScroll.nextRender_schedules_during_active",
    "Terminal.momentumScroll.repeated_gestures_work_independently",
    "Terminal.momentumScroll.rapid_repeated_gestures",
    "Terminal.momentumScroll.scroll_position_advances_correctly",
    "Terminal.momentumScroll.cancelled_by_alternate_screen",
    "Terminal.cursorMotionAnimation.starts_on_position_change",
    "Terminal.cursorMotionAnimation.chains_midanimation",
    "Terminal.screenTransition.activates_on_screen_switch",
    "Terminal.screenTransition.fades_out_blends_to_background",
    "Terminal.screenTransition.fadeout_cell_colors_blend_toward_background",
    "Terminal.screenTransition.finalizes_after_duration",
    "Terminal.screenTransition.reaches_fade_in_phase",
    "Terminal.CancelSelection_no_selection",
    "Terminal.CancelSelection_with_selection",
    "Terminal.CancelSelection_double_clear",
    "Terminal.ShiftClickExtendSelection",
    "Terminal.ScrollWhileSelecting",
    "Terminal.PerformAutoScroll",
    "Terminal.PassiveMouseTracking_Selection",
    "Terminal.KittyKeyRelease.sendKeyEvent",
    "Terminal.KittyKeyRelease.sendCharEvent",
    "Terminal.KittyKeyRelease.NoOutputWithoutFlag",
    "Terminal.KittyKeyRelease.RepeatStillWorks",
    "Terminal.TopAnchoredRegion.PartialScrollKeepsViewportFixed",
    "Terminal.TopAnchoredRegion.PartialScrollDoesNotMoveNormalModeCursor",
    "Terminal.TopAnchoredRegion.ScrollCountMatchesScrolledLines",
    "Terminal.Cursorline.trivialLineUnderCursorIsHighlighted",
    "Terminal.Cursorline.notShownInInsertMode",
    "Terminal.Cursorline.nonCursorTrivialLineNotHighlighted",
    "Terminal.YankHighlight.trivialLineIsHighlighted",
    "Terminal.GraphemeCluster.selectionDoesNotShiftLayout",
    "Terminal.GraphemeCluster.aWideCellOnTheLastColumnKeepsItsWidth",
    "Terminal.GraphemeCluster.batchedFallbackDoesNotInheritTheCursor",
    "Terminal.GraphemeCluster.aClusterLeavesTheBatchedPath",
    "Terminal.Wheel.AltScreen.NoProtocol.emits_cursor_keys",
    "Terminal.Wheel.AltScreen.AppCursorKeys.emits_SS3",
    "Terminal.Wheel.AltScreen.DECSET1007.emits_cursor_keys",
    "Terminal.Wheel.AltScreen.AppTracking.passes_through_as_SGR",
    "Terminal.Wheel.PrimaryScreen.NoProtocol.local_scroll",
    "Terminal.Wheel.AltScreen.ShiftBypass.not_handled",
    "Terminal.Wheel.AltScreen.ViNormalMode.no_cursor_keys",
    "Terminal.Wheel.AltScreen.ScrollMultiplier.repeats_cursor_keys",
    "Terminal.hint_mode_accepts_labels_while_lock_keys_are_latched",
    "Terminal.DECUDK_fires_while_lock_keys_are_latched",
    "Terminal.numpad_digit_keeps_NumLock_for_the_input_generator",
    "Terminal.no_key_encoding_depends_on_lock_modifiers",
    "Terminal.kitty_keyboard_protocol_reports_lock_modifiers",
    "Terminal.win32_input_mode_reports_unicode_for_escape_and_numpad",
    "Terminal.selectAll",
    "Terminal.selectAll.completesInInsertMode",
    "Terminal.DECMode.numberMappingRoundTrips",
    "Terminal.TextSelection_drag_into_blank_stops_at_the_pointer",
    "Terminal.TextSelection_multiline_drag_still_takes_the_first_line_whole",
    "Terminal.selection_of_a_trivial_line_survives_a_scrolled_viewport",
    "Terminal.passive mouse tracking declines the event so the UI may act on it",
    "TraceHandler.an_APC_body_waits_its_turn_like_every_other_sequence",
    "Terminal.focus.events_reach_the_pty_only_under_DECMode_1004",
    "Terminal.contains is exclusive on both axes",
    "Terminal.flushInput drops pending input on a fatal PTY write error",
    "Terminal.IME queries answered under the state lock survive concurrent output and resize",
    "Terminal.hint_mode_matches_a_url_wrapped_across_rows",
    "Terminal.hint_mode_visible_scope_ignores_scrollback",
    "Terminal.hint_mode_scrollback_scope_finds_history",
    "Terminal.hint_mode_scrollback_labels_survive_scrolling",
    "Terminal.hint_mode_visible_scope_scans_again_on_scroll",
    "Terminal.hint_mode_overlay_draws_labels_into_the_render_buffer",
    "Terminal.hint_mode_overlay_reaches_a_line_without_the_cursor",
    "Terminal.hint_mode_overlay_highlights_a_wrapped_match_on_both_rows",
    "Terminal.hint_mode_dispatches_each_action",
    "Terminal.hint_mode_validates_and_resolves_paths_against_the_working_directory",
    "Terminal.hint_mode_extends_the_scan_past_the_viewport_to_finish_a_wrapped_line",
    "Terminal.hint_mode_maps_columns_past_a_wide_character",
    "Terminal.hint_mode_survives_a_negative_scrollback_limit",
    "Terminal.hint_mode_matches_track_content_scrolling",
    "Terminal.hint_mode_overlay_wraps_a_label_past_the_row_edge",
    "Terminal reports the identity its settings named",
    "a terminal constructed below VT525 narrows its sequence table too",
)


def write_numbered_history(terminal, count=16):
    terminal.write(
        b"".join(f"{line:02d}\r\n".encode() for line in range(count))
    )


def precise_scroll_result(*deltas):
    with Shitty(
        columns=8, rows=4, save_lines=32, glyph_px=4, glyph_py=8
    ) as terminal:
        write_numbered_history(terminal)
        time = 0.01
        for delta in deltas:
            terminal.scroll(
                0, delta, phase="update", precise=True, time=time
            )
            time += 0.01
        return terminal.snapshot(), terminal.reference_image()


def image_region(image, left, top, right, bottom):
    width, height, pixels = image
    if not (0 <= left <= right <= width and 0 <= top <= bottom <= height):
        raise ValueError("image region is outside the image")
    result = bytearray()
    for y in range(top, bottom):
        begin = 3 * (y * width + left)
        end = 3 * (y * width + right)
        result.extend(pixels[begin:end])
    return bytes(result)


def drawn_layout(snapshot, row):
    result = []
    for column in range(snapshot.columns):
        cell = snapshot.cell(column, row)
        if cell.double_width_continuation or cell.char == " ":
            continue
        codepoints = cell.grapheme or (ord(cell.char),)
        result.append((column, codepoints, 2 if cell.double_width else 1))
    return result


def double_click(terminal, column, row=0, time=1.0):
    x = column + 2
    y = row + 2
    terminal.button(0, True, x=x, y=y, time=time)
    terminal.button(0, False, x=x, y=y, time=time + 0.01)
    terminal.button(0, True, x=x, y=y, time=time + 0.1)
    return terminal.button(0, False, x=x, y=y, time=time + 0.11)


class ContourTerminalTest(unittest.TestCase):
    def test_upstream_inventory_has_all_144_cases(self):
        self.assertEqual(len(UPSTREAM_CASES), 144)
        self.assertEqual(len(set(UPSTREAM_CASES)), 144)

    def test_blinking_cursor_advances_through_both_phases(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[1 q")
            self.assertTrue(terminal.render_state().cursor_blink)
            self.assertTrue(terminal.render_state().blink_visible)

            terminal.blink_tick()
            self.assertFalse(terminal.render_state().blink_visible)
            terminal.blink_tick()
            self.assertTrue(terminal.render_state().blink_visible)

    def test_ime_composition_renders_while_blink_phase_is_off(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[1 q")
            terminal.blink_tick()
            self.assertFalse(terminal.render_state().blink_visible)

            terminal.preedit("test", 0, 4)
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.lines[0], "test    ")
            self.assertEqual(
                [snapshot.cell(column, 0).inverse for column in range(4)],
                [True] * 4,
            )

    def test_reported_modifier_keys_preserve_the_scrolled_viewport(self):
        modifiers = (
            (280, 16),
            (282, 32),
            (340, 1),
            (341, 2),
            (342, 4),
            (343, 8),
            (344, 1),
            (345, 2),
            (346, 4),
            (347, 8),
        )
        for key, mask in modifiers:
            with self.subTest(key=key):
                with Shitty(columns=8, rows=3, save_lines=8) as terminal:
                    terminal.write(
                        b"one\r\ntwo\r\nthree\r\nfour\x1b[>8u"
                    )
                    terminal.page_up()

                    terminal.frontend_key_event(key, 1, modifiers=mask)

                    self.assertEqual(terminal.snapshot().view_offset, 1)
                    packet = terminal.read_input()
                    self.assertTrue(packet.startswith(b"\x1b["))
                    self.assertTrue(packet.endswith(b"u"))

        with Shitty(columns=8, rows=3, save_lines=8) as terminal:
            terminal.write(b"one\r\ntwo\r\nthree\r\nfour\x1b[>8u")
            terminal.page_up()

            terminal.frontend_key_event(257, 1)

            self.assertEqual(terminal.snapshot().view_offset, 0)
            self.assertEqual(terminal.read_input(), b"\x1b[13u")

    @unittest.expectedFailure
    def test_existing_local_paths_are_resolved_at_the_pointer(self):
        with TemporaryDirectory(prefix="shitty-contour-path-") as root_text:
            root = Path(root_text)
            nested = root / "nested"
            nested.mkdir()
            target = nested / "file.txt"
            target.write_text("test")
            short = root / "short~1"
            short.mkdir()
            short_target = short / "file.txt"
            short_target.write_text("test")

            with Shitty(columns=240, rows=4) as terminal:
                terminal.osc7_cwd(("file://" + root.as_posix()).encode())
                terminal.write(
                    b"open nested/file.txt now\r\n"
                    + b"open " + target.as_posix().encode() + b"\r\n"
                    + b"open " + short_target.as_posix().encode() + b"\r\n"
                    + b"open nested/missing.txt now"
                )

                self.assertEqual(
                    (
                        terminal.hyperlink(10, 0),
                        terminal.hyperlink(8, 1),
                        terminal.hyperlink(8, 2),
                        terminal.hyperlink(10, 3),
                    ),
                    (
                        target.as_posix(),
                        target.as_posix(),
                        short_target.as_posix(),
                        "",
                    ),
                )

    def test_output_preserves_viewport_and_typed_input_returns_to_bottom(self):
        for input_kind in ("key", "text"):
            with self.subTest(input_kind=input_kind):
                with Shitty(columns=8, rows=3, save_lines=8) as terminal:
                    terminal.write(b"one\r\ntwo\r\nthree\r\nfour")
                    terminal.page_up()
                    before = terminal.snapshot()

                    terminal.write(b"\r\nfive")
                    after_output = terminal.snapshot()
                    self.assertEqual(after_output.view_offset, 2)
                    self.assertEqual(after_output.lines, before.lines)

                    if input_kind == "key":
                        terminal.frontend_key_event(257, 1)
                    else:
                        terminal.frontend_text_event("a")
                    self.assertEqual(terminal.snapshot().view_offset, 0)

    def test_deccara_changes_only_the_requested_rectangle_attributes(self):
        with Shitty(columns=5, rows=5) as terminal:
            original = ["12345", "67890", "ABCDE", "abcde", "fghij"]
            terminal.write("\r\n".join(original).encode())
            terminal.write(
                b"\x1b[2*x"
                b"\x1b[2;3;4;5;1;4$r"
            )

            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.lines, original)
            for row in range(5):
                for column in range(5):
                    cell = snapshot.cell(column, row)
                    changed = 1 <= row <= 3 and 2 <= column <= 4
                    self.assertEqual(cell.bold, changed)
                    self.assertEqual(cell.underline, changed)

    def test_contour_private_capture_request_is_ignored(self):
        with Shitty(columns=5, rows=5, save_lines=20) as terminal:
            terminal.write(
                b"1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10"
            )
            before = terminal.snapshot()

            terminal.write(b"\x1b[>0;7,t")

            after = terminal.snapshot()
            self.assertEqual(after.lines, before.lines)
            self.assertEqual(after.view_offset, before.view_offset)
            self.assertEqual(terminal.read_input(), b"")

    def test_ris_resets_application_cursor_mode_and_its_encoder_state(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"text\x1b[?1h")
            terminal.frontend_key_event(265, 1)
            self.assertEqual(terminal.read_input(), b"\x1bOA")

            terminal.write(b"\x1bc")
            terminal.frontend_key_event(265, 1)

            self.assertEqual(terminal.snapshot().lines, [" " * 8] * 3)
            self.assertEqual(terminal.read_input(), b"\x1b[A")

    def test_contour_passive_mouse_mode_is_an_intentional_boundary(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"\x1b[?2029h\x1b[?2029$p")
            self.assertEqual(terminal.read_input(), b"\x1b[?2029;0$y")

            terminal.write(b"\x1bc\x1b[?2029$p")
            self.assertEqual(terminal.read_input(), b"\x1b[?2029;0$y")

    def test_mouse_coordinate_modes_are_mutually_exclusive(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"\x1b[?1006h")
            self.assertEqual(terminal.state()[1], 2)
            for reset in (1005, 1015, 1016):
                terminal.write(f"\x1b[?{reset}l".encode())
                self.assertEqual(terminal.state()[1], 2)

            terminal.write(b"\x1b[?1016h")
            self.assertEqual(terminal.state()[1], 4)
            terminal.write(b"\x1b[?1015h")
            self.assertEqual(terminal.state()[1], 3)
            terminal.write(b"\x1b[?1015l")
            self.assertEqual(terminal.state()[1], 0)

    def test_redraw_resize_round_trip_keeps_cell_pixel_size(self):
        with Shitty(
            columns=10, rows=5, glyph_px=9, glyph_py=18
        ) as terminal:
            self.assertEqual(terminal.winsize_full(), (10, 5, 90, 90))
            terminal.resize(11, 5)
            self.assertEqual(terminal.winsize_full(), (11, 5, 99, 90))
            terminal.resize(10, 5)
            self.assertEqual(terminal.winsize_full(), (10, 5, 90, 90))

    def test_pixel_resize_clamps_the_page_to_one_cell(self):
        with Shitty(
            columns=20, rows=5, glyph_px=9, glyph_py=18
        ) as terminal:
            terminal.resize_pixels(5, 5)
            snapshot = terminal.snapshot()
            self.assertEqual((snapshot.columns, snapshot.rows), (1, 1))
            self.assertEqual(terminal.winsize_full(), (1, 1, 9, 18))

    def test_deccolm_changes_columns_without_changing_rows(self):
        with Shitty(columns=80, rows=24) as terminal:
            terminal.write(b"\x1b[?40h\x1b[?3h")
            snapshot = terminal.snapshot()
            self.assertEqual((snapshot.columns, snapshot.rows), (132, 24))
            terminal.write(b"\x1b[?3l")
            snapshot = terminal.snapshot()
            self.assertEqual((snapshot.columns, snapshot.rows), (80, 24))

    @unittest.expectedFailure
    def test_decscpp_changes_columns_without_changing_rows(self):
        with Shitty(columns=80, rows=24) as terminal:
            terminal.write(b"\x1b[132$|")
            snapshot = terminal.snapshot()
            self.assertEqual((snapshot.columns, snapshot.rows), (132, 24))

    def test_deccolm_resizes_before_following_output_is_processed(self):
        with Shitty(columns=80, rows=24) as terminal:
            terminal.write(b"\x1b[?40h\x1b[?3h\x1b[1;132HX")
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.columns, 132)
            self.assertEqual(snapshot.cell(131, 0).char, "X")

    def test_ris_resets_deccolm_to_80_columns(self):
        with Shitty(columns=80, rows=24) as terminal:
            terminal.write(b"\x1b[?40h\x1b[?3h")
            self.assertEqual(terminal.snapshot().columns, 132)
            terminal.write(b"\x1bc")
            self.assertEqual(terminal.snapshot().columns, 80)

    def test_decncsm_controls_deccolm_screen_clearing(self):
        with Shitty(columns=80, rows=5) as terminal:
            terminal.write(b"\x1b[?40hHELLO\x1b[?3h")
            self.assertNotIn("HELLO", terminal.snapshot().lines[0])

        with Shitty(columns=80, rows=5) as terminal:
            terminal.write(
                b"\x1b[65;1\"p\x1b[?40h\x1b[?95hHELLO\x1b[?3h"
            )
            self.assertTrue(terminal.snapshot().lines[0].startswith("HELLO"))

    def test_synchronized_output_publishes_only_the_completed_frame(self):
        with Shitty(columns=8, rows=1) as terminal:
            terminal.write(b"\x00\x1b[?2026hHello ")
            self.assertEqual(terminal.snapshot().lines, [" " * 8])
            terminal.write(b"X")
            self.assertEqual(terminal.snapshot().lines, [" " * 8])
            terminal.write(b"\x1b[?2026l")
            self.assertEqual(terminal.snapshot().lines, ["Hello X "])

    @unittest.expectedFailure
    def test_xterm_color_stack_restores_the_pushed_palette(self):
        with Shitty(columns=4, rows=1) as terminal:
            terminal.write(
                b"\x1b]4;1;rgb:ff/00/00\x1b\\"
                b"\x1b[#P"
                b"\x1b]4;1;rgb:00/ff/00\x1b\\"
                b"\x1b[#Q"
                b"\x1b[31mX"
            )
            self.assertEqual(
                terminal.snapshot().cell(0, 0).foreground,
                (255, 0, 0),
            )

    def test_decac_assigns_and_resets_default_text_colors(self):
        with Shitty(columns=4, rows=1) as terminal:
            terminal.write(b"\x1b[1;2;5,|A")
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.cell(0, 0).foreground, (0, 170, 0))
            self.assertEqual(snapshot.cell(0, 0).background, (170, 0, 170))

            terminal.write(b"\x1b[1,|B")
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.cell(1, 0).foreground, (255, 255, 255))
            self.assertEqual(snapshot.cell(1, 0).background, (0, 0, 0))

    @unittest.expectedFailure
    def test_decatc_assigns_colors_to_the_bold_attribute(self):
        with Shitty(columns=4, rows=1) as terminal:
            terminal.write(b"\x1b[1){\x1b[1;2;5,}\x1b[1mX")
            cell = terminal.snapshot().cell(0, 0)
            self.assertTrue(cell.bold)
            self.assertEqual(cell.foreground, (0, 170, 0))
            self.assertEqual(cell.background, (170, 0, 170))

    @unittest.expectedFailure
    def test_decstglt_selects_the_alternate_color_table(self):
        with Shitty(columns=4, rows=1) as terminal:
            terminal.write(
                b"\x1b[1;2;5,}"
                b"\x1b[1){\x1b[1mA"
                b"\x1b[3){\x1b[1mB"
            )
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.cell(0, 0).foreground, (0, 170, 0))
            self.assertNotEqual(
                snapshot.cell(1, 0).foreground,
                (0, 170, 0),
            )

    def test_decatc_status_line_case_has_no_synthetic_screen(self):
        with Shitty(columns=20, rows=4) as terminal:
            terminal.write(b"AB\x1b[1){\x1b[0;2;5,}")
            snapshot = terminal.snapshot()
            self.assertEqual((snapshot.columns, snapshot.rows), (20, 4))
            self.assertTrue(snapshot.lines[0].startswith("AB"))

    def test_each_underline_style_replaces_the_previous_one(self):
        with Shitty(columns=12, rows=1) as terminal:
            terminal.write(
                b"\x1b[4:1mAB\x1b[21mCD\x1b[4:3mEF"
                b"\x1b[24mGH\x1b[4:2mIJ\x1b[mKL"
            )
            snapshot = terminal.snapshot()
            self.assertEqual(
                [snapshot.cell(column, 0).underline_style
                 for column in range(12)],
                [1, 1, 2, 2, 3, 3, 0, 0, 2, 2, 0, 0],
            )

    def test_selection_does_not_pad_wide_characters(self):
        with Shitty(columns=6, rows=1) as terminal:
            terminal.write("中ab".encode())
            terminal.select_start(0, 0)
            terminal.select_update(4, 0)
            self.assertEqual(terminal.select_finish(), "中ab".encode())

    def test_one_column_rectangular_selection_keeps_line_breaks(self):
        with Shitty(columns=4, rows=3) as terminal:
            terminal.write(b"a\r\nb\r\nc")
            terminal.select_start(0, 0)
            terminal.select_rectangular()
            terminal.select_update(1, 2)
            self.assertEqual(terminal.select_finish(), b"a\nb\nc")

    def test_linear_selection_keeps_leading_blank_lines(self):
        with Shitty(columns=6, rows=3) as terminal:
            terminal.write(b"\x1b[3;1Habc")
            terminal.select_start(0, 0)
            terminal.select_update(3, 2)
            self.assertEqual(terminal.select_finish(), b"\n\nabc")

    def test_curly_underline_is_not_italic_and_resets_cleanly(self):
        with Shitty(columns=4, rows=1) as terminal:
            terminal.write(b"\x1b[4:3mAB\x1b[mCD")
            snapshot = terminal.snapshot()
            self.assertEqual(
                [snapshot.cell(column, 0).underline_style
                 for column in range(4)],
                [3, 3, 0, 0],
            )
            self.assertEqual(
                [snapshot.cell(column, 0).italic for column in range(4)],
                [False] * 4,
            )

    def test_mouse_selection_drag_release_and_click_clear(self):
        with Shitty(
            columns=5, rows=5, glyph_px=2, glyph_py=2
        ) as terminal:
            terminal.write(
                b"12345\r\n67890\r\nABCDE\r\nabcde\r\nfghij"
            )
            terminal.pointer(5, 5)
            self.assertEqual(
                terminal.button(0, True, x=5, y=5, time=1.0), b""
            )
            terminal.pointer(8, 7)
            self.assertEqual(
                terminal.button(0, False, x=8, y=7, time=1.1),
                b"7890\nABC",
            )

            terminal.button(0, True, x=8, y=7, time=2.0)
            self.assertEqual(
                terminal.button(0, False, x=8, y=7, time=2.1), b""
            )

    def test_mouse_selection_joins_a_soft_wrapped_line(self):
        with Shitty(
            columns=5, rows=2, glyph_px=2, glyph_py=2
        ) as terminal:
            terminal.write(b"aaaaaaaaaa")
            terminal.pointer(5, 3)
            terminal.button(0, True, x=5, y=3, time=1.0)
            terminal.pointer(6, 5)
            self.assertEqual(
                terminal.button(0, False, x=6, y=5, time=1.1),
                b"aaaaaa",
            )

    def test_parser_buffer_survives_chunk_boundaries(self):
        with Shitty(columns=12, rows=2) as terminal:
            terminal.feed_chunks(
                b"Hello \xe2", b"\x94", b"\x82", b"\x1b[3", b"1mX"
            )
            snapshot = terminal.snapshot()
            self.assertTrue(snapshot.lines[0].startswith("Hello │X"))
            self.assertEqual(
                snapshot.cell(7, 0).foreground, (170, 0, 0)
            )

    def test_trivial_ascii_line_buffer_keeps_all_bytes(self):
        with Shitty(columns=20, rows=3) as terminal:
            terminal.write(b"ABCDEFGHIJ")
            self.assertEqual(
                terminal.snapshot().lines[0], "ABCDEFGHIJ" + " " * 10
            )

    def test_box_drawing_utf8_is_not_corrupted(self):
        with Shitty(columns=20, rows=5) as terminal:
            terminal.write("│── file\r\n├── dir".encode())
            snapshot = terminal.snapshot()
            self.assertTrue(snapshot.lines[0].startswith("│── file"))
            self.assertTrue(snapshot.lines[1].startswith("├── dir"))
            self.assertNotIn("�", "".join(snapshot.lines))

    def test_smooth_scroll_has_no_extra_render_row_at_zero_offset(self):
        snapshot, image = precise_scroll_result(0.0)
        self.assertEqual(snapshot.view_offset, 0)
        self.assertEqual(image, precise_scroll_result(0.0)[1])

    @unittest.expectedFailure
    def test_smooth_scroll_renders_an_extra_row_at_fractional_offset(self):
        zero, zero_image = precise_scroll_result(0.0)
        partial, partial_image = precise_scroll_result(0.25)
        self.assertEqual((zero.view_offset, partial.view_offset), (0, 0))
        self.assertNotEqual(partial_image, zero_image)

    def test_screen_without_transition_has_a_stable_complete_frame(self):
        with Shitty(columns=10, rows=4) as terminal:
            terminal.write(b"stable")
            before = terminal.snapshot()
            terminal.repaint()
            after = terminal.snapshot()
            self.assertEqual(after.lines, before.lines)
            self.assertEqual(
                (after.cursor_x, after.cursor_y, after.cursor_style),
                (before.cursor_x, before.cursor_y, before.cursor_style),
            )

    def test_cursor_without_animation_has_a_stable_complete_frame(self):
        with Shitty(columns=10, rows=4) as terminal:
            terminal.write(b"\x1b[2 qX")
            before = terminal.snapshot()
            terminal.blink_tick()
            after = terminal.snapshot()
            self.assertEqual(after.lines, before.lines)
            self.assertEqual(
                (after.cursor_x, after.cursor_y, after.cursor_style),
                (before.cursor_x, before.cursor_y, before.cursor_style),
            )

    @unittest.expectedFailure
    def test_precision_scroll_accumulates_visible_subline_offset(self):
        zero, zero_image = precise_scroll_result(0.0)
        quarter, quarter_image = precise_scroll_result(0.25)
        half, half_image = precise_scroll_result(0.25, 0.25)
        self.assertEqual(
            (zero.view_offset, quarter.view_offset, half.view_offset),
            (0, 0, 0),
        )
        self.assertNotEqual(quarter_image, zero_image)
        self.assertNotEqual(half_image, quarter_image)

    @unittest.expectedFailure
    def test_precision_scroll_keeps_remainder_after_full_cell(self):
        whole, whole_image = precise_scroll_result(1.0)
        partial, partial_image = precise_scroll_result(1.25)
        self.assertEqual((whole.view_offset, partial.view_offset), (1, 1))
        self.assertNotEqual(partial_image, whole_image)

    def test_precision_scroll_clears_fraction_at_history_boundary(self):
        with Shitty(
            columns=8, rows=4, save_lines=32, glyph_px=4, glyph_py=10
        ) as terminal:
            write_numbered_history(terminal)
            terminal.scroll(
                0, 20.5, phase="update", precise=True, time=0.01
            )
            top = terminal.snapshot().view_offset
            terminal.scroll(
                0, -1.25, phase="update", precise=True, time=0.02
            )
            self.assertEqual(terminal.snapshot().view_offset, top - 1)

    def test_precision_scroll_is_disabled_on_alternate_screen(self):
        with Shitty(
            columns=12, rows=4, save_lines=16, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(b"\x1b[?1049halternate")
            terminal.scroll(
                0, 0.5, phase="update", precise=True, time=0.01
            )
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.view_offset, 0)
            self.assertTrue(snapshot.lines[0].startswith("alternate"))

    @unittest.expectedFailure
    def test_output_preserves_viewport_with_fractional_pixel_offset(self):
        with Shitty(
            columns=8, rows=4, save_lines=32, glyph_px=4, glyph_py=8
        ) as terminal:
            write_numbered_history(terminal, 10)
            terminal.scroll(
                0, 0.25, phase="update", precise=True, time=0.01
            )
            before = terminal.reference_image()
            terminal.write(b"10\r\n11\r\n")
            after = terminal.snapshot()
            self.assertGreater(after.view_offset, 0)
            self.assertEqual(terminal.reference_image(), before)

    @unittest.expectedFailure
    def test_precision_gesture_end_starts_momentum_with_velocity(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 0, phase="begin", precise=True, time=0.0
            )
            for time in (0.01, 0.02, 0.03):
                terminal.scroll(
                    0, 0.75, phase="update", precise=True, time=time
                )
            terminal.scroll(
                0, 0, phase="end", precise=True, time=0.04
            )
            before = terminal.snapshot().view_offset
            terminal.blink_tick()
            self.assertGreater(terminal.snapshot().view_offset, before)

    def test_native_momentum_velocity_advances_the_viewport(self):
        with Shitty(columns=8, rows=4, save_lines=64, glyph_py=10) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 2.0, phase="begin", precise=True, momentum=True,
                time=0.05,
            )
            before = terminal.snapshot().view_offset
            terminal.scroll(
                0, 1.5, phase="update", precise=True, momentum=True,
                time=0.066,
            )
            self.assertGreater(terminal.snapshot().view_offset, before)

    def test_slow_precision_gesture_does_not_start_momentum(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            write_numbered_history(terminal)
            terminal.scroll(
                0, 0, phase="begin", precise=True, time=0.0
            )
            terminal.scroll(
                0, 0.02, phase="update", precise=True, time=0.1
            )
            terminal.scroll(
                0, 0.02, phase="update", precise=True, time=0.2
            )
            terminal.scroll(
                0, 0, phase="end", precise=True, time=0.3
            )
            terminal.blink_tick()
            self.assertEqual(terminal.snapshot().view_offset, 0)

    def test_single_native_momentum_sample_is_applied(self):
        with Shitty(columns=8, rows=4, save_lines=32, glyph_py=10) as terminal:
            write_numbered_history(terminal)
            terminal.scroll(
                0, 1.25, phase="begin", precise=True, momentum=True,
                time=0.01,
            )
            terminal.scroll(
                0, 0, phase="end", precise=True, momentum=True,
                time=0.02,
            )
            self.assertEqual(terminal.snapshot().view_offset, 1)

    def test_native_momentum_packets_decelerate_without_reversing(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            offsets = [terminal.snapshot().view_offset]
            deltas = (3.0, 2.0, 1.0, 0.5, 0.25, 0.125)
            for index, delta in enumerate(deltas):
                terminal.scroll(
                    0,
                    delta,
                    phase="begin" if index == 0 else "update",
                    precise=True,
                    momentum=True,
                    time=0.05 + index * 0.016,
                )
                offsets.append(terminal.snapshot().view_offset)
            advances = [
                current - previous
                for previous, current in zip(offsets, offsets[1:])
            ]
            self.assertEqual(advances, sorted(advances, reverse=True))
            self.assertGreater(advances[0], advances[-1])

    def test_native_momentum_end_stops_viewport_motion(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 2.0, phase="begin", precise=True, momentum=True,
                time=0.05,
            )
            terminal.scroll(
                0, 1.0, phase="update", precise=True, momentum=True,
                time=0.066,
            )
            terminal.scroll(
                0, 0, phase="end", precise=True, momentum=True,
                time=0.082,
            )
            stopped = terminal.snapshot().view_offset
            terminal.blink_tick()
            terminal.blink_tick()
            self.assertEqual(terminal.snapshot().view_offset, stopped)

    def test_new_physical_gesture_rejects_stale_native_momentum(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 2, phase="begin", precise=True, momentum=True,
                time=0.01,
            )
            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.02,
            )
            terminal.scroll(
                0, 0, phase="begin", precise=True, time=0.03
            )
            after_begin = terminal.snapshot().view_offset

            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.04,
            )
            self.assertEqual(terminal.snapshot().view_offset, after_begin)

    @unittest.expectedFailure
    def test_one_wheel_notch_glides_instead_of_jumping(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(0, 9, phase="none", precise=False, time=0.01)
            self.assertEqual(terminal.snapshot().view_offset, 0)

            terminal.blink_tick()
            first_frame = terminal.snapshot().view_offset
            self.assertGreater(first_frame, 0)
            self.assertLess(first_frame, 9)

    @unittest.expectedFailure
    def test_rapid_wheel_notches_accumulate_one_glide_target(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 40)
            terminal.scroll(0, 9, phase="none", precise=False, time=0.01)
            terminal.scroll(0, 9, phase="none", precise=False, time=0.018)
            self.assertEqual(terminal.snapshot().view_offset, 0)

            terminal.blink_tick()
            first_frame = terminal.snapshot().view_offset
            self.assertGreater(first_frame, 0)
            self.assertLess(first_frame, 18)

    @unittest.expectedFailure
    def test_wheel_glide_direction_reversal_keeps_signed_target(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 40)
            terminal.scroll(0, 9, phase="none", precise=False, time=0.01)
            terminal.blink_tick()
            before_reversal = terminal.snapshot().view_offset
            self.assertGreater(before_reversal, 0)
            self.assertLess(before_reversal, 9)

            terminal.scroll(0, -6, phase="none", precise=False, time=0.02)
            terminal.blink_tick()
            after_reversal = terminal.snapshot().view_offset
            self.assertGreater(after_reversal, 0)
            self.assertLess(after_reversal, 9)

    def test_wheel_glide_clamps_and_stays_at_history_top(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            write_numbered_history(terminal, 16)
            terminal.scroll(0, 100, phase="none", precise=False, time=0.01)
            top = terminal.snapshot().view_offset
            self.assertGreater(top, 0)

            terminal.blink_tick()
            terminal.blink_tick()
            self.assertEqual(terminal.snapshot().view_offset, top)

    def test_wheel_glide_falls_back_to_keys_on_alternate_screen(self):
        with Shitty(columns=8, rows=4, save_lines=16) as terminal:
            terminal.write(b"\x1b[?1007h\x1b[?1049halternate")
            terminal.scroll(0, 3, phase="none", precise=False, time=0.01)
            self.assertEqual(terminal.snapshot().view_offset, 0)
            self.assertEqual(terminal.read_input(), b"\x1b[A" * 3)

    def test_scrolling_to_bottom_cancels_wheel_continuation(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            write_numbered_history(terminal, 16)
            terminal.scroll(0, 6, phase="none", precise=False, time=0.01)
            for _ in range(4):
                terminal.page_down()
            self.assertEqual(terminal.snapshot().view_offset, 0)

            terminal.blink_tick()
            terminal.blink_tick()
            self.assertEqual(terminal.snapshot().view_offset, 0)

    @unittest.expectedFailure
    def test_wheel_glide_does_not_require_native_momentum_packets(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            write_numbered_history(terminal, 16)
            terminal.scroll(
                0, 6, phase="none", precise=False, momentum=False,
                time=0.01,
            )
            self.assertEqual(terminal.snapshot().view_offset, 0)
            terminal.blink_tick()
            self.assertGreater(terminal.snapshot().view_offset, 0)

    @unittest.expectedFailure
    def test_active_wheel_glide_requests_an_animation_frame(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 16)
            terminal.scroll(0, 6, phase="none", precise=False, time=0.01)
            before = terminal.snapshot().refresh_count

            terminal.blink_tick()
            self.assertGreater(terminal.snapshot().refresh_count, before)

    def test_exactly_opposing_wheel_notches_do_not_keep_animating(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 16)
            terminal.scroll(0, 6, phase="none", precise=False, time=0.01)
            terminal.scroll(0, -6, phase="none", precise=False, time=0.01)
            self.assertEqual(terminal.snapshot().view_offset, 0)
            before = terminal.snapshot().refresh_count

            terminal.blink_tick()
            terminal.blink_tick()
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.view_offset, 0)
            self.assertEqual(snapshot.refresh_count, before)

    def test_wheel_policy_falls_through_instead_of_swallowing_input(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            write_numbered_history(terminal, 16)
            terminal.scroll(0, 2, phase="none", precise=False, time=0.01)
            self.assertEqual(terminal.snapshot().view_offset, 2)

        with Shitty(columns=8, rows=4, save_lines=8) as terminal:
            terminal.write(b"\x1b[?1007h\x1b[?1049h")
            terminal.scroll(0, 2, phase="none", precise=False, time=0.01)
            self.assertEqual(terminal.read_input(), b"\x1b[A\x1b[A")

    @unittest.expectedFailure
    def test_stray_precision_update_cancels_active_wheel_glide(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(0, 9, phase="none", precise=False, time=0.01)
            terminal.blink_tick()
            before_update = terminal.snapshot().view_offset
            self.assertGreater(before_update, 0)
            self.assertLess(before_update, 9)

            terminal.scroll(0, 1, phase="update", precise=True, time=0.02)
            after_update = terminal.snapshot().view_offset
            terminal.blink_tick()
            self.assertEqual(terminal.snapshot().view_offset, after_update)

    def test_terminal_can_resize_to_one_cell_and_grow_back(self):
        with Shitty(columns=20, rows=10) as terminal:
            terminal.write(b"X")
            terminal.resize(1, 1)
            minimal = terminal.snapshot()
            self.assertEqual((minimal.columns, minimal.rows), (1, 1))

            terminal.resize(20, 10)
            grown = terminal.snapshot()
            self.assertEqual((grown.columns, grown.rows), (20, 10))

    @unittest.expectedFailure
    def test_one_cell_main_page_survives_a_dec_status_line(self):
        with Shitty(columns=20, rows=10) as terminal:
            terminal.write(b"\x1b[1$~")
            terminal.resize(1, 1)
            snapshot = terminal.snapshot()
            self.assertEqual((snapshot.columns, snapshot.rows), (1, 1))

            terminal.write(b"\x1bP$q$~\x1b\\")
            self.assertEqual(terminal.read_input(), b"\x1bP1$r1$~\x1b\\")

    def test_resize_rejects_stale_native_momentum_packets(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 2, phase="begin", precise=True, momentum=True,
                time=0.01,
            )
            terminal.resize(9, 5)
            after_resize = terminal.snapshot().view_offset

            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.02,
            )
            self.assertEqual(terminal.snapshot().view_offset, after_resize)

    def test_cancelled_physical_gesture_has_no_synthetic_momentum(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 16)
            terminal.scroll(
                0, 0, phase="begin", precise=True, momentum=False,
                time=0.01,
            )
            terminal.scroll(
                0, 0.75, phase="update", precise=True, momentum=False,
                time=0.02,
            )
            terminal.scroll(
                0, 0, phase="cancel", precise=True, momentum=False,
                time=0.03,
            )
            stopped = terminal.snapshot()

            terminal.blink_tick()
            after = terminal.snapshot()
            self.assertEqual(after.view_offset, stopped.view_offset)
            self.assertEqual(after.refresh_count, stopped.refresh_count)

    def test_legacy_wheel_path_is_immediate_and_has_no_continuation(self):
        with Shitty(columns=8, rows=4, save_lines=32) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 16)
            terminal.scroll(0, 3, phase="none", precise=False, time=0.01)
            immediate = terminal.snapshot()
            self.assertEqual(immediate.view_offset, 3)

            terminal.blink_tick()
            after = terminal.snapshot()
            self.assertEqual(after.view_offset, immediate.view_offset)
            self.assertEqual(after.refresh_count, immediate.refresh_count)

    def test_unphased_precision_input_never_arms_momentum(self):
        with Shitty(columns=8, rows=4, save_lines=32, glyph_py=10) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 16)
            for index in range(3):
                terminal.scroll(
                    0, 0.4, phase="none", precise=True,
                    time=0.01 * (index + 1),
                )
            immediate = terminal.snapshot()
            self.assertEqual(immediate.view_offset, 1)

            terminal.blink_tick()
            after = terminal.snapshot()
            self.assertEqual(after.view_offset, immediate.view_offset)
            self.assertEqual(after.refresh_count, immediate.refresh_count)

    @unittest.expectedFailure
    def test_active_synthetic_momentum_requests_animation_frames(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 0, phase="begin", precise=True, time=0.0
            )
            for index in range(3):
                terminal.scroll(
                    0, 0.75, phase="update", precise=True,
                    time=0.01 * (index + 1),
                )
            terminal.scroll(
                0, 0, phase="end", precise=True, time=0.04
            )
            before = terminal.snapshot().refresh_count

            terminal.blink_tick()
            self.assertGreater(terminal.snapshot().refresh_count, before)

    def test_repeated_native_momentum_gestures_are_independent(self):
        with Shitty(columns=8, rows=4, save_lines=64, glyph_py=10) as terminal:
            write_numbered_history(terminal, 32)
            terminal.scroll(
                0, 1, phase="begin", precise=True, momentum=True,
                time=0.01,
            )
            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.02,
            )
            terminal.scroll(
                0, 0, phase="end", precise=True, momentum=True,
                time=0.03,
            )
            first = terminal.snapshot().view_offset

            terminal.scroll(
                0, 0, phase="begin", precise=True, momentum=False,
                time=0.04,
            )
            terminal.scroll(
                0, 1, phase="begin", precise=True, momentum=True,
                time=0.05,
            )
            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.06,
            )
            terminal.scroll(
                0, 0, phase="end", precise=True, momentum=True,
                time=0.07,
            )
            self.assertGreater(terminal.snapshot().view_offset, first)

    @unittest.expectedFailure
    def test_rapid_physical_gestures_restart_synthetic_momentum(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 40)
            time = 0.0
            for _ in range(3):
                terminal.scroll(
                    0, 0, phase="begin", precise=True, time=time
                )
                for _ in range(3):
                    time += 0.01
                    terminal.scroll(
                        0, 0.75, phase="update", precise=True, time=time
                    )
                time += 0.01
                terminal.scroll(
                    0, 0, phase="end", precise=True, time=time
                )
                terminal.blink_tick()

            after_gestures = terminal.snapshot()
            terminal.blink_tick()
            after_continuation = terminal.snapshot()
            self.assertGreater(
                after_continuation.view_offset, after_gestures.view_offset
            )
            self.assertGreater(
                after_continuation.refresh_count,
                after_gestures.refresh_count,
            )

    @unittest.expectedFailure
    def test_synthetic_momentum_advances_and_eventually_settles(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            terminal.write(b"\x1b[2 q")
            write_numbered_history(terminal, 40)
            terminal.scroll(
                0, 0, phase="begin", precise=True, time=0.0
            )
            for index in range(3):
                terminal.scroll(
                    0, 0.75, phase="update", precise=True,
                    time=0.01 * (index + 1),
                )
            terminal.scroll(
                0, 0, phase="end", precise=True, time=0.04
            )
            at_release = terminal.snapshot().view_offset

            offsets = []
            for _ in range(64):
                terminal.blink_tick()
                offsets.append(terminal.snapshot().view_offset)

            self.assertGreater(max(offsets), at_release)
            self.assertEqual(len(set(offsets[-4:])), 1)

    @unittest.expectedFailure
    def test_alternate_screen_cancels_native_momentum_stream(self):
        with Shitty(columns=8, rows=4, save_lines=64) as terminal:
            write_numbered_history(terminal, 40)
            terminal.scroll(
                0, 1, phase="begin", precise=True, momentum=True,
                time=0.01,
            )
            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.02,
            )
            terminal.write(b"\x1b[?1049h\x1b[?1049l")
            after_switch = terminal.snapshot().view_offset

            terminal.scroll(
                0, 1, phase="update", precise=True, momentum=True,
                time=0.03,
            )
            self.assertEqual(terminal.snapshot().view_offset, after_switch)

    @unittest.expectedFailure
    def test_cursor_position_change_schedules_motion_frames(self):
        with Shitty(columns=12, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"\x1b[2 q\x1b[2;10H")
            after_move = terminal.snapshot().refresh_count

            terminal.blink_tick()
            self.assertGreater(
                terminal.snapshot().refresh_count, after_move
            )

    @unittest.expectedFailure
    def test_cursor_motion_retargets_while_animation_is_active(self):
        with Shitty(columns=12, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"\x1b[2 q\x1b[2;10H")
            terminal.blink_tick()
            first_frame = terminal.reference_image()

            terminal.write(b"\x1b[3;2H")
            after_retarget = terminal.snapshot().refresh_count
            terminal.blink_tick()
            self.assertGreater(
                terminal.snapshot().refresh_count, after_retarget
            )
            self.assertNotEqual(terminal.reference_image(), first_frame)

    @unittest.expectedFailure
    def test_screen_switch_schedules_transition_frames(self):
        with Shitty(columns=8, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"primary\x1b[2 q\x1b[?25l")
            terminal.write(b"\x1b[?1049hsecondary")
            after_switch = terminal.snapshot().refresh_count

            terminal.blink_tick()
            self.assertGreater(
                terminal.snapshot().refresh_count, after_switch
            )

    @unittest.expectedFailure
    def test_screen_transition_fadeout_changes_the_presented_frame(self):
        with Shitty(columns=8, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"\x1b[?25l\x1b[41m\x1b[2J")
            terminal.write(b"\x1b[0m\x1b[?1049hsecondary")
            first_frame = terminal.reference_image()
            after_switch = terminal.snapshot().refresh_count

            terminal.blink_tick()
            self.assertGreater(
                terminal.snapshot().refresh_count, after_switch
            )
            self.assertNotEqual(terminal.reference_image(), first_frame)

    @unittest.expectedFailure
    def test_screen_transition_fadeout_blends_cell_toward_background(self):
        with Shitty(columns=8, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"\x1b[?25l\x1b[41m\x1b[2J")
            terminal.write(b"\x1b[0m\x1b[?1049h")
            before = terminal.presented_pixel(2, 2)

            terminal.blink_tick()
            blended = terminal.presented_pixel(2, 2)
            self.assertNotEqual(blended, before)
            self.assertTrue(all(0 <= value <= 170 for value in blended))

    @unittest.expectedFailure
    def test_screen_transition_finalizes_and_stops_requesting_frames(self):
        with Shitty(columns=8, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"primary\x1b[2 q\x1b[?25l")
            terminal.write(b"\x1b[?1049hsecondary")
            after_switch = terminal.snapshot().refresh_count
            terminal.blink_tick()
            self.assertGreater(
                terminal.snapshot().refresh_count, after_switch
            )

            for _ in range(64):
                terminal.blink_tick()
            settled = terminal.snapshot().refresh_count
            terminal.blink_tick()
            self.assertEqual(terminal.snapshot().refresh_count, settled)

    @unittest.expectedFailure
    def test_screen_transition_reaches_a_distinct_fade_in_frame(self):
        with Shitty(columns=8, rows=3, glyph_px=4, glyph_py=8) as terminal:
            terminal.write(b"primary\x1b[2 q\x1b[?25l")
            terminal.write(b"\x1b[?1049hsecondary")
            fadeout_frame = terminal.reference_image()
            for _ in range(8):
                terminal.blink_tick()

            fadein_frame = terminal.reference_image()
            self.assertNotEqual(fadein_frame, fadeout_frame)
            after_fadein = terminal.snapshot().refresh_count
            terminal.blink_tick()
            self.assertGreater(
                terminal.snapshot().refresh_count, after_fadein
            )

    def test_cancel_selection_without_selection_is_a_safe_noop(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"abcdefgh")
            terminal.select_clear()
            self.assertFalse(terminal.has_selection())

    def test_cancel_selection_clears_an_existing_selection(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"abcdefgh")
            terminal.button(0, True, x=2, y=2, time=1)
            terminal.pointer(6, 2)
            self.assertEqual(
                terminal.button(0, False, x=6, y=2, time=1.01),
                b"abcd",
            )

            terminal.select_clear()
            self.assertFalse(terminal.has_selection())

    def test_cancel_selection_can_be_repeated(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(b"abcdefgh")
            terminal.button(0, True, x=2, y=2, time=1)
            terminal.pointer(6, 2)
            terminal.button(0, False, x=6, y=2, time=1.01)

            for _ in range(2):
                terminal.select_clear()
                self.assertFalse(terminal.has_selection())

    def test_shift_click_extends_the_nearest_selection_endpoint(self):
        for extension_x, expected in ((2, b"abcdef"), (10, b"defgh")):
            with self.subTest(extension_x=extension_x):
                with Shitty(columns=12, rows=3) as terminal:
                    terminal.write(b"abcdefghijkl")
                    terminal.button(0, True, x=5, y=2, time=1)
                    terminal.pointer(8, 2)
                    self.assertEqual(
                        terminal.button(
                            0, False, x=8, y=2, time=1.01
                        ),
                        b"def",
                    )

                    terminal.button(
                        0, True, x=extension_x, y=2,
                        modifiers=1, time=2,
                    )
                    self.assertEqual(
                        terminal.button(
                            0, False, x=extension_x, y=2,
                            modifiers=1, time=2.01,
                        ),
                        expected,
                    )

    def test_scroll_while_selecting_updates_only_an_active_drag(self):
        with Shitty(columns=8, rows=4, save_lines=20) as terminal:
            write_numbered_history(terminal, 10)
            terminal.button(0, True, x=4, y=5)
            terminal.pointer(x=2, y=2)
            before = terminal.snapshot().selection

            terminal.scroll(0, 1)
            self.assertNotEqual(terminal.snapshot().selection, before)
            selected = terminal.button(0, False, x=2, y=2)
            self.assertNotEqual(selected, b"")

            terminal.scroll(0, 1)
            self.assertEqual(terminal.select_finish(), selected)

        with Shitty(columns=8, rows=4, save_lines=20) as terminal:
            write_numbered_history(terminal, 10)
            terminal.scroll(0, 1)
            self.assertFalse(terminal.has_selection())

    def test_autoscroll_requires_an_active_selection_and_clamps(self):
        with Shitty(columns=8, rows=4, save_lines=20) as terminal:
            write_numbered_history(terminal, 10)
            terminal.button(0, True, x=4, y=5)
            terminal.pointer(x=2, y=2)

            terminal.selection_autoscroll_tick()
            self.assertGreater(terminal.snapshot().view_offset, 0)
            for _ in range(64):
                terminal.selection_autoscroll_tick()
            history_limit = terminal.snapshot().view_offset
            terminal.selection_autoscroll_tick()
            self.assertEqual(
                terminal.snapshot().view_offset, history_limit
            )
            selected = terminal.button(0, False, x=2, y=2)
            self.assertNotEqual(selected, b"")

            completed = terminal.snapshot()
            terminal.selection_autoscroll_tick()
            self.assertEqual(
                terminal.snapshot().view_offset, completed.view_offset
            )
            self.assertEqual(
                terminal.snapshot().selection, completed.selection
            )

    def test_contour_passive_mouse_mode_remains_an_explicit_boundary(self):
        with Shitty(columns=8, rows=3) as terminal:
            terminal.write(
                b"abcdefgh\x1b[?1000h\x1b[?1006h"
                b"\x1b[?2029h\x1b[?2029$p"
            )
            self.assertEqual(terminal.read_input(), b"\x1b[?2029;0$y")

            terminal.button(0, True, x=2, y=2, modifiers=1, time=1)
            terminal.pointer(6, 2, modifiers=1)
            self.assertEqual(
                terminal.button(
                    0, False, x=6, y=2, modifiers=1, time=1.01
                ),
                b"abcd",
            )
            self.assertEqual(terminal.read_input(), b"")

    def test_kitty_functional_key_release_is_reported_when_requested(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>3u")
            terminal.kitty_special("UP", event=1)
            terminal.kitty_special("UP", event=3)
            self.assertEqual(
                terminal.read_input(), b"\x1b[A\x1b[1;1:3A"
            )

    def test_kitty_character_release_is_reported_when_requested(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>3u")
            terminal.kitty_key(ord("a"), modifiers=4, event=1)
            terminal.kitty_key(ord("a"), modifiers=4, event=3)
            self.assertEqual(
                terminal.read_input(), b"\x1b[97;5u\x1b[97;5:3u"
            )

    def test_kitty_release_without_event_type_flag_has_no_output(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>1u")
            terminal.kitty_special("UP", event=3)
            terminal.kitty_key(ord("a"), modifiers=4, event=3)
            self.assertEqual(terminal.read_input(), b"")

    def test_kitty_functional_key_repeat_keeps_its_event_type(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>3u")
            terminal.kitty_special("UP", event=2)
            self.assertEqual(terminal.read_input(), b"\x1b[1;1:2A")

    @unittest.expectedFailure
    def test_top_anchored_partial_su_keeps_scrolled_viewport_fixed(self):
        with Shitty(columns=8, rows=6, save_lines=20) as terminal:
            terminal.write(
                b"h1\r\nh2\r\nh3\r\nh4\r\nh5\r\nh6\r\nh7\r\nh8\r\n"
            )
            terminal.scroll(0, 3)
            before = terminal.snapshot()
            self.assertEqual(before.view_offset, 3)

            terminal.write(b"\x1b[1;3r\x1b[3;1H\x1b[S")

            after = terminal.snapshot()
            self.assertEqual(after.view_offset, before.view_offset)
            self.assertEqual(after.lines, before.lines)

    def test_partial_ind_keeps_frontend_anchor_outside_region_fixed(self):
        with Shitty(columns=8, rows=6, save_lines=20) as terminal:
            terminal.write(b"r1\r\nr2\r\nr3\r\nr4\r\nr5\r\nr6")
            terminal.select_start(0, 5)
            terminal.select_update(2, 5)
            before = terminal.selection_state()

            terminal.write(b"\x1b[1;3r\x1b[3;1H\x1bD")

            self.assertEqual(terminal.selection_state(), before)

    @unittest.expectedFailure
    def test_full_history_partial_ind_has_no_viewport_count_drift(self):
        with Shitty(columns=8, rows=4, save_lines=2) as terminal:
            terminal.write(b"a\r\nb\r\nc\r\nd\r\ne\r\nf\r\n")
            terminal.scroll(0, 1)
            before = terminal.snapshot()
            self.assertEqual(before.view_offset, 1)

            terminal.write(b"\x1b[1;2r\x1b[2;1H\x1bD")

            self.assertEqual(terminal.snapshot().view_offset, before.view_offset)

    @unittest.expectedFailure
    def test_public_cursor_guide_highlights_a_plain_cursor_line(self):
        with Shitty(
            columns=10, rows=4, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(
                b"plain0\r\nplain1\r\nplain2"
                b"\x1b[2;1H"
                b"\x1b]1337;HighlightCursorLine=1\x1b\\"
            )
            self.assertNotEqual(
                terminal.presented_pixel(36, 14),
                (0, 0, 0),
            )

    def test_disabled_cursor_guide_does_not_tint_insert_mode_lines(self):
        with Shitty(
            columns=10, rows=4, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(
                b"plain0\r\nplain1\r\nplain2"
                b"\x1b[2;1H"
                b"\x1b]1337;HighlightCursorLine=0\x1b\\"
            )
            for y in (6, 14, 22):
                self.assertEqual(
                    terminal.presented_pixel(36, y),
                    (0, 0, 0),
                )

    @unittest.expectedFailure
    def test_cursor_guide_tints_only_the_cursor_plain_line(self):
        with Shitty(
            columns=10, rows=4, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(
                b"plain0\r\nplain1"
                b"\x1b[1;1H"
                b"\x1b]1337;HighlightCursorLine=1\x1b\\"
            )
            self.assertNotEqual(
                terminal.presented_pixel(36, 6),
                (0, 0, 0),
            )
            self.assertEqual(
                terminal.presented_pixel(36, 14),
                (0, 0, 0),
            )

    def test_selection_highlights_a_plain_line_without_tinting_its_sibling(self):
        with Shitty(
            columns=10, rows=4, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(
                b"\x1b]17;#00aa00\x1b\\"
                b"plain0\r\nplain1\r\nplain2\x1b[?25l"
            )
            terminal.select_start(0, 1)
            terminal.select_update(9, 1)

            self.assertEqual(
                terminal.presented_pixel(35, 14),
                (0, 170, 0),
            )
            self.assertEqual(
                terminal.presented_pixel(35, 22),
                (0, 0, 0),
            )

    @unittest.expectedFailure
    def test_selection_does_not_change_grapheme_render_layout(self):
        cases = (
            (
                "thumbs up and skin tone",
                "[\U0001f44d\U0001f3fb]",
                [
                    (0, (0x5B,), 1),
                    (1, (0x1F44D, 0x1F3FB), 2),
                    (3, (0x5D,), 1),
                ],
            ),
            (
                "rainbow flag",
                "[\U0001f3f3\ufe0f\u200d\U0001f308]",
                [
                    (0, (0x5B,), 1),
                    (1, (0x1F3F3, 0xFE0F, 0x200D, 0x1F308), 2),
                    (3, (0x5D,), 1),
                ],
            ),
            (
                "eye and VS16",
                "[\U0001f441\ufe0f]",
                [
                    (0, (0x5B,), 1),
                    (1, (0x1F441, 0xFE0F), 2),
                    (3, (0x5D,), 1),
                ],
            ),
            (
                "plain text",
                "abc",
                [
                    (0, (0x61,), 1),
                    (1, (0x62,), 1),
                    (2, (0x63,), 1),
                ],
            ),
            (
                "alpha and combining acute",
                "\u03b1\u0301x",
                [
                    (0, (0x03B1, 0x0301), 1),
                    (1, (0x78,), 1),
                ],
            ),
        )
        mismatches = []
        for name, text, expected_layout in cases:
            with Shitty(
                columns=20, rows=3, glyph_px=4, glyph_py=8
            ) as terminal:
                terminal.write(
                    b"\x1b]17;#000000\x1b\\"
                    b"\x1b]19;#ffffff\x1b\\"
                    b"\x1b[?25l"
                    + text.encode()
                    + b"\r\n"
                )
                layout = drawn_layout(terminal.model_snapshot(), 0)
                before = image_region(
                    terminal.reference_image(), 2, 2, 82, 10
                )

                terminal.select_start(0, 0)
                terminal.select_update(19, 0)
                terminal.select_finish()
                after = image_region(
                    terminal.reference_image(), 2, 2, 82, 10
                )

                if layout != expected_layout or after != before:
                    mismatches.append((name, layout, after == before))

        self.assertEqual(mismatches, [])

    def test_wide_grapheme_at_last_column_wraps_without_losing_width(self):
        with Shitty(columns=5, rows=3) as terminal:
            terminal.write("\x1b[1;5H\u4e16\x1b[3;1H".encode())
            snapshot = terminal.snapshot()

            self.assertTrue(
                any(snapshot.cell(column, 0).wrapped for column in range(5))
            )
            self.assertEqual(snapshot.cell(0, 1).char, "\u4e16")
            self.assertTrue(snapshot.cell(0, 1).double_width)
            self.assertTrue(snapshot.cell(1, 1).double_width_continuation)

            terminal.select_start(0, 1)
            terminal.select_update(2, 1)
            self.assertEqual(terminal.select_finish(), "\u4e16".encode())

    def test_selected_plain_line_does_not_inherit_wide_cursor_colors(self):
        def first_three_cells_after(last_cell):
            with Shitty(
                columns=5, rows=3, glyph_px=4, glyph_py=8
            ) as terminal:
                terminal.write(
                    b"\x1b[2 q\x1b[2;1Hxyz"
                    b"\x1b[1;1H\x1b[31mab\x1b[m"
                    + (
                        b"\x1b[1;4H"
                        if last_cell == "\u4e16"
                        else b"\x1b[1;5H"
                    )
                    + last_cell.encode()
                )
                terminal.select_start(3, 1)
                terminal.select_update(4, 1)
                return image_region(
                    terminal.reference_image(), 2, 10, 14, 18
                )

        self.assertEqual(
            first_three_cells_after("\u4e16"),
            first_three_cells_after("Z"),
        )

    def test_multi_codepoint_cluster_remains_one_public_model_cell(self):
        with Shitty(columns=20, rows=3) as terminal:
            terminal.write("[\U0001f441\ufe0f]\r\n\u03b1\u0301x".encode())
            snapshot = terminal.model_snapshot()

            self.assertEqual(snapshot.cell(1, 0).grapheme, (0x1F441, 0xFE0F))
            self.assertTrue(snapshot.cell(1, 0).double_width)
            self.assertTrue(snapshot.cell(2, 0).double_width_continuation)
            self.assertEqual(snapshot.cell(0, 1).grapheme, (0x03B1, 0x0301))
            self.assertFalse(snapshot.cell(0, 1).double_width)
            self.assertEqual(snapshot.cell(1, 1).char, "x")

    def test_alternate_screen_wheel_emits_normal_cursor_keys(self):
        with Shitty(
            columns=20, rows=5, extra_arguments=("-altScroll",)
        ) as terminal:
            terminal.write(b"\x1b[?1049h")
            terminal.scroll(0, -1)
            terminal.scroll(0, 1)
            self.assertEqual(terminal.read_input(), b"\x1b[B\x1b[A")

    def test_alternate_screen_wheel_honors_application_cursor_mode(self):
        with Shitty(
            columns=20, rows=5, extra_arguments=("-altScroll",)
        ) as terminal:
            terminal.write(b"\x1b[?1049h\x1b[?1h")
            terminal.scroll(0, -1)
            self.assertEqual(terminal.read_input(), b"\x1bOB")

    def test_decset_1007_enables_alternate_scroll_without_changing_decckm(self):
        with Shitty(columns=20, rows=5) as terminal:
            terminal.write(b"\x1b[?1049h\x1b[?1007h")
            terminal.scroll(0, -1)
            self.assertEqual(terminal.read_input(), b"\x1b[B")

    def test_mouse_tracking_takes_priority_over_alternate_scroll(self):
        with Shitty(
            columns=20, rows=5, extra_arguments=("-altScroll",)
        ) as terminal:
            terminal.write(
                b"\x1b[?1049h\x1b[?1002h\x1b[?1006h"
            )
            terminal.scroll(0, -1)
            self.assertEqual(terminal.read_input(), b"\x1b[<65;1;1M")

    def test_primary_screen_wheel_scrolls_locally_without_child_input(self):
        with Shitty(columns=8, rows=4, save_lines=20) as terminal:
            write_numbered_history(terminal, 10)
            terminal.scroll(0, 1)
            self.assertEqual(terminal.snapshot().view_offset, 1)
            self.assertEqual(terminal.read_input(), b"")

    @unittest.expectedFailure
    def test_shift_bypasses_alternate_scroll_cursor_injection(self):
        with Shitty(
            columns=20, rows=5, extra_arguments=("-altScroll",)
        ) as terminal:
            terminal.write(b"\x1b[?1049h")
            terminal.scroll(0, -1, modifiers=1)
            self.assertEqual(terminal.read_input(), b"")

    @unittest.expectedFailure
    def test_active_frontend_selection_suppresses_alternate_scroll_keys(self):
        with Shitty(
            columns=20, rows=5, extra_arguments=("-altScroll",)
        ) as terminal:
            terminal.write(b"\x1b[?1049habcdef")
            terminal.select_start(0, 0)
            terminal.select_update(3, 0)
            terminal.scroll(0, -1)
            self.assertEqual(terminal.read_input(), b"")

    def test_multi_notch_alternate_scroll_repeats_cursor_keys(self):
        with Shitty(
            columns=20, rows=5, extra_arguments=("-altScroll",)
        ) as terminal:
            terminal.write(b"\x1b[?1049h")
            terminal.scroll(0, -3)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[B\x1b[B\x1b[B",
            )

    def test_lock_keys_do_not_block_selected_url_copy_action(self):
        url = b"https://example.com"
        copy_modifiers = 8 if TEST_PLATFORM == "cocoa" else 1 | 2
        for locks in (CAPS_LOCK, NUM_LOCK, CAPS_LOCK | NUM_LOCK):
            with self.subTest(locks=locks):
                with Shitty(columns=40, rows=3) as terminal:
                    terminal.write(b"visit " + url + b" now")
                    self.assertEqual(double_click(terminal, 12), url)
                    terminal.set_system_clipboard(b"old")

                    modifiers = copy_modifiers | locks
                    terminal.frontend_key_event(
                        ord("C"), PRESS, modifiers=modifiers
                    )
                    terminal.frontend_key_event(
                        ord("C"), RELEASE, modifiers=modifiers
                    )

                    self.assertEqual(
                        terminal.get_selection(primary=False), url
                    )
                    self.assertEqual(terminal.read_input(), b"")

    def test_decudk_fires_while_lock_keys_are_latched(self):
        for locks in (CAPS_LOCK, NUM_LOCK, CAPS_LOCK | NUM_LOCK):
            with self.subTest(locks=locks):
                with Shitty(columns=20, rows=3) as terminal:
                    terminal.write(
                        b"\x1bP0;1|17/48656C6C6F\x1b\\"
                    )
                    terminal.frontend_key_event(
                        KEY_F6, PRESS, modifiers=locks
                    )
                    terminal.frontend_key_event(
                        KEY_F6, RELEASE, modifiers=locks
                    )
                    self.assertEqual(terminal.read_input(), b"Hello")

    def test_application_keypad_keeps_numlock_for_digit_selection(self):
        with Shitty(columns=20, rows=3) as terminal:
            terminal.write(b"\x1b=")
            terminal.frontend_key_event(
                KEY_KP_5, PRESS, modifiers=NUM_LOCK
            )
            self.assertEqual(terminal.read_input(), b"5")

    def test_legacy_key_encoding_is_invariant_under_lock_modifiers(self):
        locks = (CAPS_LOCK, NUM_LOCK, CAPS_LOCK | NUM_LOCK)
        with Shitty(columns=20, rows=3) as terminal:
            def encode_key(key, modifiers):
                terminal.frontend_key_event(
                    key, PRESS, modifiers=modifiers
                )
                terminal.frontend_key_event(
                    key, RELEASE, modifiers=modifiers
                )
                return terminal.read_input()

            for key in (*range(KEY_F1, KEY_F1 + 25),
                        *range(KEY_KP_0, KEY_KP_EQUAL + 1)):
                baseline = encode_key(key, 0)
                for latched in locks:
                    with self.subTest(key=key, locks=latched):
                        self.assertEqual(
                            encode_key(key, latched), baseline
                        )

            def encode_character(codepoint, modifiers):
                character = chr(codepoint)
                key = ord(character.upper()) if character.isalpha() else codepoint
                terminal.frontend_key_event(
                    key, PRESS, modifiers=modifiers
                )
                terminal.frontend_text_event(
                    codepoint, modifiers=modifiers
                )
                terminal.frontend_key_event(
                    key, RELEASE, modifiers=modifiers
                )
                return terminal.read_input()

            for modify_other_keys in (False, True):
                if modify_other_keys:
                    terminal.write(b"\x1b[>4;2m")
                for codepoint in range(0x20, 0x7F):
                    baseline = encode_character(codepoint, 0)
                    for latched in locks:
                        with self.subTest(
                            codepoint=codepoint,
                            locks=latched,
                            modify_other_keys=modify_other_keys,
                        ):
                            self.assertEqual(
                                encode_character(codepoint, latched),
                                baseline,
                            )

    def test_kitty_keyboard_protocol_reports_lock_modifiers(self):
        expected = {
            CAPS_LOCK: b"\x1b[97;65u",
            NUM_LOCK: b"\x1b[97;129u",
            CAPS_LOCK | NUM_LOCK: b"\x1b[97;193u",
        }
        with Shitty(columns=20, rows=3) as terminal:
            terminal.write(b"\x1b[>9u")
            for locks, packet in expected.items():
                with self.subTest(locks=locks):
                    terminal.frontend_key_event(
                        ord("A"), PRESS, modifiers=locks
                    )
                    terminal.frontend_text_event("a", modifiers=locks)
                    terminal.frontend_key_event(
                        ord("A"), RELEASE, modifiers=locks
                    )
                    self.assertEqual(terminal.read_input(), packet)

    @unittest.expectedFailure
    def test_win32_input_mode_reports_escape_and_numpad_unicode(self):
        with Shitty(columns=20, rows=3) as terminal:
            terminal.write(b"\x1b[?9001h")
            terminal.frontend_key_event(KEY_ESCAPE, PRESS)
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[27;0;27;1;0;1_",
            )

            terminal.frontend_key_event(
                KEY_KP_5, PRESS, modifiers=NUM_LOCK
            )
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[101;0;53;1;32;1_",
            )

    def test_full_page_drag_can_select_visible_page_and_scrollback(self):
        with Shitty(columns=10, rows=3, save_lines=5) as terminal:
            terminal.write(
                b"hist1\r\nhist2\r\npage1\r\npage2\r\npage3"
            )
            terminal.scroll(0, 100)
            self.assertGreater(terminal.snapshot().view_offset, 0)
            terminal.select_start(0, 0)
            terminal.scroll(0, -100)
            terminal.select_update(9, 2)
            selected = terminal.select_finish()

            self.assertTrue(terminal.has_selection())
            for line in (b"hist1", b"hist2", b"page1", b"page3"):
                self.assertIn(line, selected)

    def test_full_page_drag_is_complete_in_insert_mode(self):
        with Shitty(columns=10, rows=3) as terminal:
            terminal.write(b"one\r\ntwo\r\nthree")
            terminal.select_start(0, 0)
            terminal.select_update(9, 2)
            selected = terminal.select_finish()
            completed = terminal.selection_state()

            self.assertTrue(terminal.has_selection())
            self.assertIn(b"one", selected)
            self.assertIn(b"three", selected)
            terminal.pointer(5, 3)
            self.assertEqual(terminal.selection_state(), completed)

    def test_private_mode_set_reset_and_report_share_one_number_map(self):
        mutable_modes = (
            1, 3, 4, 5, 6, 7, 8, 9, 12, 25, 40, 41, 42, 45,
            47, 66, 67, 69, 1000, 1001, 1002, 1003, 1004,
            1005, 1006, 1007, 1015, 1016, 1034, 1036, 1039, 1045,
            1047, 1049, 2004, 2026, 2027, 2031, 2048, 5522,
        )
        for mode in mutable_modes:
            with self.subTest(mode=mode):
                with Shitty(columns=10, rows=6) as terminal:
                    if mode == 3:
                        terminal.write(b"\x1b[?40h")
                        terminal.read_input()
                    terminal.write(f"\x1b[?{mode}h".encode())
                    terminal.read_input()
                    terminal.write(f"\x1b[?{mode}$p".encode())
                    self.assertEqual(
                        terminal.read_input(),
                        f"\x1b[?{mode};1$y".encode(),
                    )
                    terminal.write(f"\x1b[?{mode}l".encode())
                    terminal.read_input()
                    terminal.write(f"\x1b[?{mode}$p".encode())
                    self.assertEqual(
                        terminal.read_input(),
                        f"\x1b[?{mode};2$y".encode(),
                    )

        with Shitty(columns=10, rows=6) as terminal:
            terminal.write(b"\x1b[?2$p\x1b[?38$p\x1b[?44$p")
            self.assertEqual(
                terminal.read_input(),
                b"\x1b[?2;1$y\x1b[?38;0$y\x1b[?44;0$y",
            )

    def test_single_line_selection_into_blank_stops_at_pointer(self):
        with Shitty(columns=20, rows=4) as terminal:
            terminal.write(b"abc")
            terminal.select_start(0, 0)
            terminal.select_update(6, 0)

            self.assertEqual(
                terminal.selection_state()["raw"], (0, 0, 6, 0)
            )
            self.assertEqual(terminal.select_finish(), b"abc")

    def test_multiline_selection_takes_remainder_of_first_line(self):
        with Shitty(columns=20, rows=4) as terminal:
            terminal.write(b"abc\r\ndef")
            terminal.select_start(0, 0)
            terminal.select_update(2, 1)
            self.assertEqual(terminal.select_finish(), b"abc\nde")

    def test_scrolled_selected_plain_line_uses_cell_rendering(self):
        with Shitty(
            columns=20, rows=4, save_lines=30, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(
                b"\x1b]17;#00aa00\x1b\\\x1b[?25l"
                + b"".join(
                    f"line{index}\r\n".encode()
                    for index in range(20)
                )
            )
            terminal.scroll(0, 9)
            self.assertEqual(terminal.snapshot().view_offset, 9)
            terminal.select_start(0, 0)
            terminal.select_update(3, 0)

            self.assertEqual(terminal.presented_pixel(13, 6), (0, 170, 0))
            self.assertEqual(terminal.presented_pixel(13, 14), (0, 0, 0))

    @unittest.expectedFailure
    def test_passive_mouse_tracking_reports_without_claiming_selection(self):
        with Shitty(columns=20, rows=4) as terminal:
            terminal.write(
                b"abcdef\x1b[?1000h\x1b[?1006h\x1b[?2029h"
            )
            terminal.button(0, True, x=2, y=2, time=1.0)
            selected = terminal.button(
                0, False, x=6, y=2, time=1.1
            )

            self.assertEqual(
                terminal.read_input(),
                b"\x1b[<0;1;1M\x1b[<0;5;1m",
            )
            self.assertEqual(selected, b"abcd")

    @unittest.expectedFailure
    def test_trace_buffers_apc_behind_the_preceding_sequence(self):
        apc = b"Ga=T,f=32,s=2,v=2;AAAA"
        with Shitty(columns=8, rows=4) as terminal:
            terminal.parser_trace_on()
            before = terminal.snapshot()
            terminal.write(b"\x1b[2;2H\x1b_" + apc + b"\x1b\\")

            self.assertEqual(
                terminal.parser_trace(),
                [("csi", b"2;2H"), ("apc", apc)],
            )
            after = terminal.snapshot()
            self.assertEqual(
                (after.cursor_x, after.cursor_y),
                (before.cursor_x, before.cursor_y),
            )

    def test_focus_packets_are_gated_by_private_mode_1004(self):
        with Shitty(columns=10, rows=3) as terminal:
            terminal.focus(False)
            terminal.focus(True)
            self.assertEqual(terminal.read_input(), b"")

            terminal.write(b"\x1b[?1004h")
            terminal.focus(False)
            terminal.focus(True)
            self.assertEqual(
                terminal.read_input(), b"\x1b[O\x1b[I"
            )

    def test_snapshot_page_bounds_are_exclusive_on_both_axes(self):
        with Shitty(columns=5, rows=3) as terminal:
            snapshot = terminal.snapshot()
            self.assertEqual(snapshot.cell(0, 0).char, " ")
            self.assertEqual(snapshot.cell(4, 2).char, " ")
            for column, row in ((5, 0), (0, 3), (-1, 0), (0, -1)):
                with self.subTest(column=column, row=row):
                    with self.assertRaises(IndexError):
                        snapshot.cell(column, row)

    def test_fatal_pty_write_drops_pending_input_but_eagain_retries(self):
        with Shitty(columns=10, rows=3) as terminal:
            terminal.script_pty_writes(("error", errno.EAGAIN))
            terminal.input(b"hello")
            self.assertEqual(terminal.read_written_pty(), b"")
            terminal.script_pty_writes(5)
            self.assertTrue(terminal.flush_output_result())
            self.assertEqual(terminal.read_written_pty(), b"hello")

        with Shitty(columns=10, rows=3) as terminal:
            terminal.script_pty_writes(("error", errno.EPIPE))
            terminal.input(b"hello")
            self.assertEqual(terminal.read_written_pty(), b"")
            terminal.script_pty_writes(5)
            self.assertTrue(terminal.flush_output_result())
            self.assertEqual(terminal.read_written_pty(), b"")

    def test_ime_queries_survive_serialized_output_and_resize(self):
        with Shitty(columns=80, rows=24, save_lines=100) as terminal:
            sizes = ((20, 6), (80, 24))
            for round_number in range(200):
                terminal.preedit(
                    "composition \u4e16\u754c",
                    cursor_begin=4,
                    cursor_end=8,
                )
                terminal.write(
                    "wide \u4e16\u754c and combining \u1b26\u1b38 e\u0301\r\n".encode()
                )
                if round_number % 25 == 24:
                    terminal.resize(*sizes[(round_number // 25) % 2])
                    snapshot = terminal.model_snapshot()
                    self.assertLess(snapshot.cursor_x, snapshot.columns)
                    self.assertLess(snapshot.cursor_y, snapshot.rows)
                    snapshot.cell(snapshot.cursor_x, snapshot.cursor_y)
                    self.assertEqual(
                        len(snapshot.lines[snapshot.cursor_y]),
                        snapshot.columns,
                    )

            terminal.preedit(b"")
            snapshot = terminal.model_snapshot()
            self.assertGreater(snapshot.rows, 0)
            self.assertGreater(snapshot.columns, 0)
            self.assertLess(snapshot.cursor_x, snapshot.columns)
            self.assertLess(snapshot.cursor_y, snapshot.rows)

    def test_semantic_url_selection_crosses_a_soft_wrap(self):
        url = b"https://example.com/wrapped"
        with Shitty(columns=20, rows=5, save_lines=10) as terminal:
            terminal.write(url)
            snapshot = terminal.snapshot()
            self.assertTrue(
                any(snapshot.cell(column, 0).wrapped for column in range(20))
            )
            self.assertEqual(double_click(terminal, 3, row=1), url)

    def test_semantic_url_selection_only_sees_the_current_viewport(self):
        url = b"https://scrolled-away.example"
        with Shitty(columns=40, rows=3, save_lines=20) as terminal:
            terminal.write(url + b"\r\n\r\n\r\n\r\n\r\n")
            self.assertFalse(
                any(
                    "https://scrolled-away.example" in line
                    for line in terminal.snapshot().lines
                )
            )
            self.assertNotEqual(double_click(terminal, 10, row=0), url)

            terminal.select_clear()
            terminal.scroll(0, 100)
            snapshot = terminal.snapshot()
            url_row = next(
                row for row, line in enumerate(snapshot.lines)
                if "https://scrolled-away.example" in line
            )
            self.assertEqual(double_click(terminal, 10, row=url_row), url)

    def test_scrollback_uri_discovery_finds_a_history_row(self):
        url = b"https://scrolled-away.example"
        with Shitty(columns=40, rows=3, save_lines=20) as terminal:
            terminal.write(url + b"\r\n\r\n\r\n\r\n\r\n")
            self.assertNotIn(url.decode(), terminal.snapshot().lines)

            terminal.scroll(0, 100)
            snapshot = terminal.snapshot()
            row = next(
                index
                for index, line in enumerate(snapshot.lines)
                if url.decode() in line
            )

            self.assertGreater(snapshot.view_offset, 0)
            self.assertEqual(terminal.hyperlink(10, row), url.decode())

    def test_selected_scrollback_uri_survives_viewport_scrolling(self):
        url = b"https://one.example"
        with Shitty(columns=40, rows=3, save_lines=20) as terminal:
            terminal.write(
                url
                + b"\r\nhttps://two.example\r\n\r\n\r\n\r\n\r\n"
            )
            terminal.scroll(0, 100)
            snapshot = terminal.snapshot()
            row = next(
                index
                for index, line in enumerate(snapshot.lines)
                if url.decode() in line
            )
            self.assertEqual(double_click(terminal, 10, row=row), url)
            before = terminal.selection_state()["snapped"]

            terminal.scroll(0, -1)
            after = terminal.selection_state()["snapped"]

            self.assertEqual(after[0::2], before[0::2])
            self.assertEqual(after[1], before[1] - 1)
            self.assertEqual(after[3], before[3] - 1)
            self.assertEqual(terminal.select_finish(), url)

    def test_visible_uri_discovery_reacts_to_viewport_scrolling(self):
        url = b"https://scrolled-away.example"
        with Shitty(columns=40, rows=3, save_lines=20) as terminal:
            terminal.write(url + b"\r\n\r\n\r\n\r\n\r\n")
            terminal.pointer(12, 2, modifiers=2)
            self.assertEqual(terminal.desktop_state()["icon"], 0)

            terminal.scroll(0, 100)
            snapshot = terminal.snapshot()
            row = next(
                index
                for index, line in enumerate(snapshot.lines)
                if url.decode() in line
            )
            terminal.pointer(12, 2 + row, modifiers=2)
            state = terminal.desktop_state()

            self.assertEqual(state["icon"], 1)
            self.assertEqual(
                (state["hovered_link_begin"], state["hovered_link_end"]),
                (0, len(url)),
            )

    def test_semantic_uri_overlay_reaches_the_render_buffer(self):
        url = b"https://example.com"
        with Shitty(
            columns=40, rows=3, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(b"visit " + url + b" now")
            before = image_region(
                terminal.reference_image(), 26, 2, 30, 10
            )

            terminal.select_start(6, 0)
            terminal.select_extend(6, 0, cycle=True)
            selected = terminal.select_finish()
            after = image_region(
                terminal.reference_image(), 26, 2, 30, 10
            )

            self.assertEqual(selected, url)
            self.assertEqual(terminal.model_snapshot().cell(6, 0).char, "h")
            self.assertNotEqual(after, before)

    def test_semantic_uri_overlay_reaches_a_non_cursor_line(self):
        url = b"https://example.com"
        with Shitty(
            columns=40, rows=3, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(b"visit " + url + b" now\r\ncursor here")
            before = image_region(
                terminal.reference_image(), 26, 2, 30, 10
            )

            terminal.select_start(6, 0)
            terminal.select_extend(6, 0, cycle=True)
            selected = terminal.select_finish()
            after = image_region(
                terminal.reference_image(), 26, 2, 30, 10
            )

            self.assertEqual(selected, url)
            self.assertEqual(terminal.snapshot().cursor_y, 1)
            self.assertNotEqual(after, before)

    def test_semantic_uri_overlay_covers_both_wrapped_rows(self):
        url = b"https://example.com/wrapped"
        with Shitty(
            columns=20, rows=3, glyph_px=4, glyph_py=8
        ) as terminal:
            terminal.write(url)
            terminal.select_start(3, 0)
            terminal.select_extend(3, 0, cycle=True)

            self.assertEqual(terminal.select_finish(), url)
            self.assertEqual(
                terminal.selection_state()["snapped"], (0, 0, 7, 1)
            )
            first_row = terminal.presented_pixel(80, 6)
            second_row = terminal.presented_pixel(4, 14)
            outside = terminal.presented_pixel(36, 14)
            self.assertEqual(first_row, second_row)
            self.assertNotEqual(second_row, outside)

    def test_semantic_uri_frontend_dispatches_all_public_actions(self):
        url = b"https://example.com"
        copy_modifiers = 8 if TEST_PLATFORM == "cocoa" else 1 | 2

        with self.subTest(action="select"):
            with Shitty(columns=40, rows=3) as terminal:
                terminal.write(b"go " + url + b" now")
                self.assertEqual(double_click(terminal, 10), url)
                self.assertTrue(terminal.has_selection())

        with self.subTest(action="copy"):
            with Shitty(columns=40, rows=3) as terminal:
                terminal.write(b"go " + url + b" now")
                self.assertEqual(double_click(terminal, 10), url)
                terminal.frontend_key_event(
                    ord("C"), PRESS, modifiers=copy_modifiers
                )
                terminal.frontend_key_event(
                    ord("C"), RELEASE, modifiers=copy_modifiers
                )
                self.assertEqual(terminal.get_selection(False), url)

        with self.subTest(action="open"):
            with Shitty(columns=40, rows=3) as terminal:
                terminal.write(b"go " + url + b" now")
                terminal.button(
                    0, True, x=12, y=2, modifiers=2, time=1.0
                )
                terminal.button(
                    0, False, x=12, y=2, modifiers=2, time=1.1
                )
                state = terminal.desktop_state()
                self.assertEqual(state["open_count"], 1)
                self.assertEqual(state["opened_uri"], url)

        with self.subTest(action="paste"):
            with Shitty(columns=40, rows=3) as terminal:
                terminal.set_system_clipboard(url)
                self.assertTrue(terminal.paste_clipboard())
                self.assertEqual(terminal.read_input(), url)

        with self.subTest(action="copy_and_paste"):
            with Shitty(columns=40, rows=3) as terminal:
                terminal.write(b"go " + url + b" now")
                self.assertEqual(double_click(terminal, 10), url)
                terminal.frontend_key_event(
                    ord("C"), PRESS, modifiers=copy_modifiers
                )
                terminal.frontend_key_event(
                    ord("C"), RELEASE, modifiers=copy_modifiers
                )
                self.assertTrue(terminal.paste_clipboard())
                self.assertEqual(terminal.read_input(), url)

    @unittest.expectedFailure
    def test_bare_path_hints_are_validated_and_resolved_against_osc7(self):
        with TemporaryDirectory(prefix="shitty-contour-path-") as root_text:
            root = Path(root_text)
            target = root / "Makefile"
            target.write_text("all:\n")
            expected = target.as_posix()

            with Shitty(columns=60, rows=4) as terminal:
                terminal.osc7_cwd(("file://" + root.as_posix()).encode())
                terminal.write(
                    b"edit Makefile please\r\n"
                    b"also Makefile again\r\n"
                    b"but Nonexistent is absent"
                )

                self.assertEqual(
                    (
                        terminal.hyperlink(7, 0),
                        terminal.hyperlink(7, 1),
                        terminal.hyperlink(8, 2),
                    ),
                    (expected, expected, ""),
                )

    def test_uri_scan_finishes_a_wrapped_line_at_viewport_edges(self):
        url = b"https://example.com/wrapped"
        with Shitty(columns=20, rows=1, save_lines=10) as terminal:
            terminal.write(url + b"\r\n\r\n\r\n")
            terminal.scroll(0, 100)

            self.assertEqual(terminal.snapshot().lines[0], url[:20].decode())
            self.assertEqual(terminal.hyperlink(3, 0), url.decode())

            terminal.scroll(0, -1)
            self.assertTrue(
                terminal.snapshot().lines[0].startswith("wrapped")
            )
            self.assertEqual(terminal.hyperlink(3, 0), url.decode())

    def test_uri_columns_account_for_a_preceding_wide_character(self):
        url = b"https://example.com"
        with Shitty(columns=40, rows=3) as terminal:
            terminal.write("\u4e2d ".encode() + url)
            snapshot = terminal.model_snapshot()
            self.assertTrue(snapshot.cell(0, 0).double_width)
            self.assertTrue(snapshot.cell(1, 0).double_width_continuation)

            terminal.pointer(10, 2, modifiers=2)
            state = terminal.desktop_state()
            self.assertEqual(terminal.hyperlink(8, 0), url.decode())
            self.assertEqual(
                (state["hovered_link_begin"], state["hovered_link_end"]),
                (3, 3 + len(url)),
            )

    def test_negative_scrollback_limit_is_rejected_without_a_crash(self):
        result = run_startup_failure(
            extra_arguments=("-saveLines", "-100")
        )
        self.assertEqual(result.returncode, 255)

    def test_selected_uri_tracks_its_content_into_scrollback(self):
        url = b"https://tracked.example"
        with Shitty(columns=40, rows=3, save_lines=50) as terminal:
            terminal.write(url + b"\r\n")
            terminal.select_start(10, 0)
            terminal.select_extend(10, 0, cycle=True)
            self.assertEqual(terminal.select_finish(), url)
            before = terminal.selection_state()["snapped"]

            terminal.write(b"more\r\nlines\r\nhere\r\n")
            after = terminal.selection_state()["snapped"]

            self.assertEqual(terminal.select_finish(), url)
            self.assertEqual(after[0::2], before[0::2])
            self.assertLess(after[1], before[1])
            self.assertLess(after[3], before[3])

    def test_uri_overlay_wraps_when_a_match_starts_at_the_last_column(self):
        url = b"https://edge.example"
        with Shitty(columns=20, rows=30, save_lines=50) as terminal:
            for index in range(26):
                terminal.write(f"https://s{index}.io\r\n".encode())
            terminal.write(b" " * 19 + url)

            self.assertEqual(terminal.hyperlink(19, 26), url.decode())
            terminal.select_start(19, 26)
            terminal.select_extend(19, 26, cycle=True)

            self.assertEqual(terminal.select_finish(), url)
            self.assertEqual(
                terminal.selection_state()["snapped"],
                (19, 26, 19, 27),
            )

    def test_vt340_operating_identity_enables_vt300_mode_reports(self):
        with Shitty(columns=20, rows=5) as terminal:
            terminal.write(b"\x1b[63;1\"p\x1b[4$p")
            self.assertEqual(
                terminal.read_input(), b"\x1b[4;2$y"
            )

    def test_vt220_level_excludes_decfra_but_keeps_decscl(self):
        fill = b"\x1b[90;1;1;1;1$x"
        with Shitty(columns=5, rows=2) as terminal:
            terminal.write(b"\x1b[62;1\"pA" + fill)
            self.assertEqual(terminal.snapshot().lines[0], "A    ")

            terminal.write(b"\x1b[65;1\"pA" + fill)
            self.assertEqual(terminal.snapshot().lines[0], "Z    ")


if __name__ == "__main__":
    unittest.main()
