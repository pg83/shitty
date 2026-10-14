# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""Public adaptations of all 75 iTerm2 LineBuffer cases."""

import unittest

from harness import ShittyWithTabs as Shitty, put_rows


PORTED_CASES = (
    "testBasic",
    "testBasic_Wraps",
    "testCopyOnWrite_ModifySecond",
    "testCopyOnWrite_ModifyFirst",
    "testCopyOnWrite_ModifyBoth",
    "testCopyOnWrite_CopyOfCopy",
    "testCopyOnWrite_ClientKeepsOwnerAliveUntilWriteToSecond",
    "testCopyOnWrite_ClientKeepsOwnerAliveUntilWriteToFirst",
    "testCopyOnWrite_Pop",
    "testCopyOnWrite_Truncate",
    "testDropExcessLinesAdjustsCursorXWhenItsRawLineIsPartiallyTrimmed",
    "testConvertPositionMultiBlock",
    "testCoordinateForPosition_extendsToEOL_yOffset0_extendsRight",
    "testCoordinateForPosition_extendsToEOL_yOffset0_notExtendsRight",
    "testCoordinateForPosition_extendsToEOL_yOffsetPositive_extendsRight",
    "testCoordinateForPosition_extendsToEOL_yOffsetPositive_notExtendsRight",
    "testRoundTrip_singleLine_noWrap",
    "testRoundTrip_pastEOL_collapsesToEndOfContent",
    "testRoundTrip_pastEOL_extendsRight_clampsToWidthMinus1",
    "testRoundTrip_wrappedHardEOLLine",
    "testRoundTrip_softEOL",
    "testRoundTrip_emptyLinesInMiddle",
    "testRoundTrip_multiBlock_threeBlocks",
    "testRoundTrip_multiBlock",
    "testRoundTrip_widthOne",
    "testCrossWidth_wideToNarrow",
    "testCrossWidth_narrowToWide",
    "testRoundTrip_lastPositionPastEOL_extendsRight",
    "testRoundTrip_lastPositionPastEOL_noExtendsRight",
    "testRoundTrip_origin",
    "testRoundTrip_lineLengthEqualsWidth",
    "testRoundTrip_exhaustiveMixed",
    "testCrossWidth_pastEOL_collapsesToEndOfContent",
    "testRoundTrip_positionStableAcrossForceSeal",
    "testFirstAndLastPosition_mapToBoundaryCoords",
    "testBlockContaining_singleBlock_middleOfContent",
    "testBlockContaining_singleBlock_atEnd",
    "testBlockContaining_twoBlocks_atBoundary_secondLonger",
    "testBlockContaining_threeBlocks_atInnerBoundary",
    "testBlockContaining_threeBlocks_atInnerBoundary_nextBlockShorter",
    "testBlockContaining_threeBlocks_middleOfMiddleBlock",
    "testBlockContaining_positionZero",
    "testBlockContaining_yOffsetZero_atBoundary",
    "testBlockContaining_yOffsetPositive_atBoundary_noEmpties",
    "testBlockContaining_singleBlock_withTrailingEmpty_atEnd_yOffsetZero",
    "testBlockContaining_singleBlock_withTrailingEmpty_atEnd_yOffsetOne",
    "testBlockContaining_twoBlocks_secondAllEmpty_atBoundary",
    "testPositionForStartOfLastLineBeforePosition_atInnerBoundary",
    "testPositionForStartOfLastLineBeforePosition_yOffsetSpansEmptyBlock",
    "testPrepareToSearchFor_startingAtInnerBoundary_findsInThirdBlock",
    "testRightPromptBug",
    "testNumberOfRawLinesInRange",
    "testNumberOfRawLinesInRange_EmptyLines",
    "testNumberOfRawLinesInRange_BlockBoundaries",
    "testNumberOfRawLinesInRange_SingleWrappedLine",
    "testNumberOfRawLinesInRange_PartialRawLines",
    "testNumberOfRawLinesInRange_SoftEOL",
    "testNumberOfRawLinesInRange_DoubleWidthCharacters",
    "testNumberOfRawLinesInRange_EmptyRange",
    "testNumberOfRawLinesInRange_VeryLongRawLines",
    "testNumberOfRawLinesInRange_MultipleBlocks",
    "testNumberOfRawLinesInRange_BufferLimits",
    "testNumberOfRawLinesInRange_VaryingLengths",
    "testMultiLineSearchSpanningBlocks",
    "testMultiLineSearchWithinSingleBlock",
    "testMultiLineSearchWithinSingleBlockBackwards",
    "testMultiLineSearchWithinSingleBlockBackwardsFindsLastMatch",
    "testMultiLineSearchSpanningBlocksBackwards",
    "testMultiLineSearchPartialFirstLineSpanningBlocks",
    "testMultiLineSearchBackwardPartialFirstLineSpanningBlocks",
    "testMultiLineSearchBackwardFirstQueryLineInPriorBlock",
    "testMultiLineSearchSpanningThreeBlocks",
    "testMultiLineSearchSpanningBlocksSetsIncludesPartialLastLine",
    "testForwardSearchExcludesMatchAtStopAtPosition",
    "testBackwardSearchIncludesMatchAtStopAtPosition",
)


def unwrapped_line_count(snapshot, location, length):
    """Count logical lines intersecting a range of physical rows."""
    if length == 0:
        return 0
    if location < 0 or length < 0 or location + length > snapshot.rows:
        raise ValueError("physical row range is outside the snapshot")
    return 1 + sum(
        not snapshot.cell(snapshot.columns - 1, row).wrapped
        for row in range(location, location + length - 1)
    )


def hard_lines(*lines):
    return b"\r\n".join(line.encode() for line in lines)


def find_text(terminal, query, **options):
    """Call the host text-search adapter once Shitty grows that operation."""
    operation = getattr(terminal, "find_text", None)
    if operation is None:
        raise AssertionError(
            "terminal-buffer search is supported by the implementation "
            "consensus but Shitty has no host search operation"
        )
    return operation(query, **options)


class ITerm2LineBufferTest(unittest.TestCase):
    def test_upstream_inventory_has_all_75_distinct_cases(self):
        self.assertEqual(len(PORTED_CASES), 75)
        self.assertEqual(len(set(PORTED_CASES)), 75)

    def test_basic_keeps_two_hard_lines_in_order(self):
        with Shitty(columns=80, rows=3, save_lines=4) as terminal:
            terminal.write(b"Hello world\r\nGoodbye cruel world\r\n")
            snapshot = terminal.model_snapshot()
            self.assertEqual(
                tuple(line.rstrip() for line in snapshot.lines),
                ("Hello world", "Goodbye cruel world", ""),
            )
            self.assertFalse(snapshot.cell(79, 0).wrapped)
            self.assertFalse(snapshot.cell(79, 1).wrapped)

    def test_basic_wraps_into_the_eight_expected_physical_rows(self):
        with Shitty(columns=4, rows=8, save_lines=0) as terminal:
            terminal.write(b"Hello world\r\nGoodbye cruel world")
            snapshot = terminal.model_snapshot()
            self.assertEqual(
                snapshot.lines,
                ["Hell", "o wo", "rld ", "Good", "bye ", "crue", "l wo", "rld "],
            )
            self.assertEqual(
                [snapshot.cell(3, row).wrapped for row in range(8)],
                [True, True, False, True, True, True, True, False],
            )

    def test_modify_second_replay_leaves_the_first_session_unchanged(self):
        with Shitty(columns=24, rows=3, save_lines=4) as terminal:
            terminal.write(b"Hello world")
            terminal.new_session()
            terminal.write(b"Hello world\r\nGoodbye cruel world")
            self.assertEqual(
                terminal.all_text()[:2],
                ("Hello world", "Goodbye cruel world"),
            )
            terminal.chord_prev_tab()
            self.assertEqual(terminal.all_text()[0], "Hello world")
            self.assertEqual(tuple(filter(None, terminal.all_text())), ("Hello world",))

    def test_modify_first_replay_leaves_the_second_session_unchanged(self):
        with Shitty(columns=24, rows=3, save_lines=4) as terminal:
            terminal.write(b"Hello world")
            terminal.new_session()
            terminal.write(b"Hello world")
            terminal.write_to(0, b"\r\nGoodbye cruel world")
            self.assertEqual(tuple(filter(None, terminal.all_text())), ("Hello world",))
            terminal.chord_prev_tab()
            self.assertEqual(
                terminal.all_text()[:2],
                ("Hello world", "Goodbye cruel world"),
            )

    def test_modify_both_replays_diverge_without_cross_session_aliasing(self):
        with Shitty(columns=24, rows=3, save_lines=4) as terminal:
            terminal.write(b"Hello world")
            terminal.new_session()
            terminal.write(b"Hello world\r\nHello world")
            terminal.write_to(0, b"\r\nGoodbye cruel world")
            self.assertEqual(terminal.all_text()[:2], ("Hello world", "Hello world"))
            terminal.chord_prev_tab()
            self.assertEqual(
                terminal.all_text()[:2],
                ("Hello world", "Goodbye cruel world"),
            )

    def test_copy_of_copy_replays_three_independently_diverging_states(self):
        with Shitty(columns=24, rows=3, save_lines=4) as terminal:
            terminal.write(b"Hello world")
            terminal.new_session()
            terminal.write(b"Hello world\r\nGoodbye cruel world")
            terminal.new_session()
            terminal.write(b"Hello world\r\nI like traffic lights")

            expected = (
                ("Hello world", "I like traffic lights"),
                ("Hello world", "Goodbye cruel world"),
                ("Hello world",),
            )
            for lines in expected:
                self.assertEqual(tuple(filter(None, terminal.all_text())), lines)
                terminal.chord_prev_tab()

    def test_surviving_second_session_remains_writable_after_first_is_destroyed(self):
        with Shitty(columns=24, rows=2, save_lines=4) as terminal:
            terminal.write(b"Hello world")
            terminal.new_session()
            terminal.write(b"Hello world")
            terminal.close_session(0)
            self.assertEqual(terminal.session_state(), (1, 0))
            terminal.write(b"\r\nGoodbye cruel world")
            self.assertEqual(
                terminal.all_text(),
                ("Hello world", "Goodbye cruel world"),
            )

    def test_surviving_first_session_remains_writable_after_second_is_destroyed(self):
        with Shitty(columns=24, rows=2, save_lines=4) as terminal:
            terminal.write(b"Hello world")
            terminal.new_session()
            terminal.write(b"Hello world")
            terminal.close_session(1)
            self.assertEqual(terminal.session_state(), (1, 0))
            terminal.write(b"\r\nGoodbye cruel world")
            self.assertEqual(
                terminal.all_text(),
                ("Hello world", "Goodbye cruel world"),
            )

    def test_height_growth_pops_the_newest_history_row_without_mutating_snapshot(self):
        with Shitty(columns=24, rows=1, save_lines=4) as terminal:
            terminal.write(b"Hello world\r\nGoodbye cruel world\r\n")
            before = terminal.model_snapshot()
            self.assertEqual(terminal.scrollback_state()[0], 2)

            terminal.resize(24, 2)
            after = terminal.model_snapshot()
            self.assertEqual(before.lines, [" " * 24])
            self.assertEqual(
                tuple(line.rstrip() for line in after.lines),
                ("Goodbye cruel world", ""),
            )
            self.assertEqual(terminal.scrollback_state()[0], 1)

    def test_bounded_truncate_changes_only_the_overflowing_session(self):
        with Shitty(columns=24, rows=1, save_lines=2) as terminal:
            terminal.write(b"Hello world\r\nGoodbye cruel world\r\n")
            terminal.new_session()
            terminal.write(b"Hello world\r\nGoodbye cruel world\r\n")
            terminal.write(b"third\r\nfourth\r\n")
            self.assertEqual(terminal.scrollback_state()[0], 2)
            self.assertEqual(terminal.all_text(), ("third", "fourth", ""))

            terminal.chord_prev_tab()
            self.assertEqual(terminal.scrollback_state()[0], 2)
            self.assertEqual(
                terminal.all_text(),
                ("Hello world", "Goodbye cruel world", ""),
            )

    def test_partial_history_trim_keeps_cursor_on_the_surviving_wrapped_suffix(self):
        wide = "界".encode()
        with Shitty(
            columns=4,
            rows=2,
            save_lines=2,
            extra_arguments=("-unicodeWidths", "17"),
        ) as terminal:
            terminal.write(b"ABCDEFGH\r\nIJK" + wide + b"NOP")
            terminal.write(b"\x1b[1;1H")
            terminal.resize(4, 4)
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines, ["EFGH", "IJK ", "界 NO", "P   "])
            self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (0, 2))
            self.assertTrue(snapshot.cell(0, 2).double_width)
            self.assertTrue(snapshot.cell(1, 2).double_width_continuation)

    def test_coordinates_remain_addressable_across_multiple_storage_pages(self):
        payload = b"A" * 10_000
        with Shitty(columns=80, rows=4, save_lines=128) as terminal:
            terminal.write(payload + b"\r\nSecond")
            self.assertEqual("".join(terminal.all_text()).rstrip(), payload.decode() + "Second")

            terminal.wheel_up(10_000)
            terminal.select_start(0, 0)
            terminal.select_update(3, 0)
            self.assertEqual(terminal.select_finish(), b"AAA")
            terminal.wheel_down(10_000)
            terminal.select_start(0, 3)
            terminal.select_update(6, 3)
            self.assertEqual(terminal.select_finish(), b"Second")

    def test_selection_end_past_hard_eol_extends_to_the_right_edge_semantically(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcde\r\nxyz")
            terminal.select_start(0, 0)
            terminal.select_update(10, 0)
            self.assertEqual(terminal.select_finish(), b"abcde")

    def test_selection_start_past_hard_eol_collapses_to_natural_content_end(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcde\r\nxyz")
            terminal.select_start(7, 0)
            terminal.select_update(3, 1)
            self.assertEqual(terminal.select_finish(), b"\nxyz")

    def test_selection_end_on_a_later_empty_row_extends_to_its_right_edge(self):
        with Shitty(columns=10, rows=5, save_lines=0) as terminal:
            terminal.write(b"abcde\r\n\r\n\r\n\r\nxyz")
            terminal.select_start(0, 0)
            terminal.select_update(10, 2)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 10, 2))

    def test_selection_start_on_a_later_empty_row_uses_column_zero(self):
        with Shitty(columns=10, rows=5, save_lines=0) as terminal:
            terminal.write(b"abcde\r\n\r\n\r\n\r\nxyz")
            terminal.select_start(7, 2)
            terminal.select_update(3, 4)
            self.assertEqual(terminal.select_finish(), b"\n\nxyz")

    def test_content_coordinates_round_trip_one_cell_at_a_time(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcde\r\nxyz")
            for row, text in enumerate((b"abcde", b"xyz")):
                for column, byte in enumerate(text):
                    terminal.select_start(column, row)
                    terminal.select_update(column + 1, row)
                    self.assertEqual(terminal.select_finish(), bytes((byte,)))

    def test_past_eol_selection_start_loses_the_original_blank_column(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcde\r\nxyz")
            results = []
            for column in (6, 7, 9):
                terminal.select_start(column, 0)
                terminal.select_update(1, 1)
                results.append(terminal.select_finish())
            self.assertEqual(results, [b"\nx"] * 3)

    def test_past_eol_selection_end_clamps_to_the_same_last_grid_column(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcde\r\nxyz")
            results = []
            for column in (6, 7, 9, 10):
                terminal.select_start(0, 0)
                terminal.select_update(column, 0)
                results.append(terminal.select_finish())
            self.assertEqual(results, [b"abcde"] * 4)

    def test_wrapped_hard_line_coordinates_cross_every_soft_boundary(self):
        with Shitty(columns=5, rows=4, save_lines=0) as terminal:
            terminal.write(b"Hello world\r\n")
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines[:3], ["Hello", " worl", "d    "])
            self.assertEqual(
                [snapshot.cell(4, row).wrapped for row in range(3)],
                [True, True, False],
            )
            terminal.select_start(0, 0)
            terminal.select_update(1, 2)
            self.assertEqual(terminal.select_finish(), b"Hello world")

    def test_soft_eol_appends_form_one_wrapped_logical_line(self):
        with Shitty(columns=5, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcdefghij")
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines, ["abcde", "fghij"])
            self.assertTrue(snapshot.cell(4, 0).wrapped)
            self.assertFalse(snapshot.cell(4, 1).wrapped)
            terminal.select_start(0, 0)
            terminal.select_update(5, 1)
            self.assertEqual(terminal.select_finish(), b"abcdefghij")

    def test_empty_hard_lines_in_the_middle_keep_distinct_coordinates(self):
        with Shitty(columns=10, rows=4, save_lines=0) as terminal:
            terminal.write(put_rows(b"abc", b"", b"", b"xyz"))
            self.assertEqual(terminal.all_text(), ("abc", "", "", "xyz"))
            for row in (1, 2):
                terminal.select_start(0, row)
                terminal.select_update(1, row)
                self.assertEqual(terminal.select_finish(), b"")
            terminal.select_start(0, 3)
            terminal.select_update(3, 3)
            self.assertEqual(terminal.select_finish(), b"xyz")

    def test_three_successive_storage_epochs_keep_every_old_coordinate_live(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first")
            first = terminal.model_snapshot()
            terminal.write(b"\r\nsecond")
            second = terminal.model_snapshot()
            terminal.write(b"\r\nthird")

            self.assertEqual(first.lines, ["first     ", "          ", "          "])
            self.assertEqual(second.lines[:2], ["first     ", "second    "])
            self.assertEqual(terminal.all_text(), ("first", "second", "third"))
            for row, word in enumerate((b"first", b"second", b"third")):
                terminal.select_start(0, row)
                terminal.select_update(len(word), row)
                self.assertEqual(terminal.select_finish(), word)

    def test_two_storage_epochs_join_at_one_public_hard_line_boundary(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"first")
            before = terminal.model_snapshot()
            terminal.write(b"\r\nsecond")
            terminal.select_start(0, 0)
            terminal.select_update(6, 1)
            self.assertEqual(terminal.select_finish(), b"first\nsecond")
            self.assertEqual(before.lines[0], "first     ")

    def test_width_one_gives_each_character_its_own_wrapped_row(self):
        with Shitty(columns=1, rows=3, save_lines=0) as terminal:
            terminal.write(b"abc")
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines, ["a", "b", "c"])
            self.assertEqual(
                [snapshot.cell(0, row).wrapped for row in range(3)],
                [True, True, False],
            )

    def test_cursor_position_maps_from_wide_to_narrow_reflow(self):
        with Shitty(columns=20, rows=3, save_lines=4) as terminal:
            terminal.write(b"abcdefghij\x1b[1;7H")
            self.assertEqual(
                (terminal.snapshot().cursor_x, terminal.snapshot().cursor_y),
                (6, 0),
            )
            terminal.resize(4, 3)
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines, ["abcd", "efgh", "ij  "])
            self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (2, 1))

    def test_cursor_position_maps_from_narrow_to_wide_reflow(self):
        with Shitty(columns=4, rows=3, save_lines=4) as terminal:
            terminal.write(b"abcdefghij\x1b[2;3H")
            self.assertEqual(
                (terminal.snapshot().cursor_x, terminal.snapshot().cursor_y),
                (2, 1),
            )
            terminal.resize(20, 3)
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines[0], "abcdefghij" + " " * 10)
            self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (6, 0))

    def test_last_line_end_can_extend_to_the_right_grid_boundary(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc\r\nxyz")
            terminal.select_start(0, 1)
            terminal.select_update(10, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 1, 10, 1))
            self.assertEqual(terminal.select_finish(), b"xyz")

    def test_last_line_end_can_remain_at_the_natural_content_boundary(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc\r\nxyz")
            terminal.select_start(0, 1)
            terminal.select_update(3, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 1, 3, 1))
            self.assertEqual(terminal.select_finish(), b"xyz")

    def test_origin_round_trips_as_the_first_content_cell(self):
        with Shitty(columns=10, rows=1, save_lines=0) as terminal:
            terminal.write(b"abc")
            terminal.select_start(0, 0)
            terminal.select_update(1, 0)
            self.assertEqual(terminal.select_finish(), b"a")

    def test_exact_width_hard_line_has_no_phantom_continuation_row(self):
        with Shitty(columns=5, rows=2, save_lines=0) as terminal:
            terminal.write(b"abcde\r\nxyz")
            snapshot = terminal.model_snapshot()
            self.assertEqual(snapshot.lines, ["abcde", "xyz  "])
            self.assertFalse(snapshot.cell(4, 0).wrapped)
            for column, byte in enumerate(b"abcde"):
                terminal.select_start(column, 0)
                terminal.select_update(column + 1, 0)
                self.assertEqual(terminal.select_finish(), bytes((byte,)))

    def test_mixed_hard_soft_and_empty_rows_have_exhaustive_cell_coordinates(self):
        logical = ("Hell", "o wo", "rld", "ABC", "", "1234", "5678", "z")
        with Shitty(columns=4, rows=8, save_lines=0) as terminal:
            terminal.write(b"Hello world\r\nABC\r\n\r\n12345678\r\nz")
            self.assertEqual(terminal.all_text(), logical)
            for row, text in enumerate(logical):
                for column, char in enumerate(text.encode()):
                    terminal.select_start(column, row)
                    terminal.select_update(column + 1, row)
                    self.assertEqual(terminal.select_finish(), bytes((char,)))

    @unittest.expectedFailure
    def test_past_eol_anchor_uses_iterm2_collapse_policy_across_widths(self):
        with Shitty(columns=20, rows=5, save_lines=4) as terminal:
            terminal.write(b"abcdefghij\r\nzzz\x1b[1;16H")
            terminal.resize(4, 5)
            snapshot = terminal.model_snapshot()
            self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (2, 2))
            self.assertEqual(terminal.all_text(), ("abcd", "efgh", "ij", "zzz", ""))

    def test_selection_position_stays_live_after_later_lines_are_appended(self):
        with Shitty(columns=10, rows=2, save_lines=4) as terminal:
            terminal.write(b"abcde")
            terminal.select_start(2, 0)
            terminal.select_update(3, 0)
            terminal.write(b"\r\nxyz")
            self.assertEqual(terminal.select_finish(), b"c")

    def test_first_and_last_public_boundaries_cover_the_whole_buffer(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc\r\nxyz")
            terminal.select_start(0, 0)
            terminal.select_update(3, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 3, 1))
            self.assertEqual(terminal.select_finish(), b"abc\nxyz")

    def test_single_line_middle_position_selects_the_expected_cell(self):
        with Shitty(columns=10, rows=1, save_lines=0) as terminal:
            terminal.write(b"abcde")
            terminal.select_start(2, 0)
            terminal.select_update(3, 0)
            self.assertEqual(terminal.select_finish(), b"c")

    def test_single_line_end_position_stays_on_that_hard_line(self):
        with Shitty(columns=10, rows=1, save_lines=0) as terminal:
            terminal.write(b"abcde")
            terminal.select_start(0, 0)
            terminal.select_update(5, 0)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 5, 0))
            self.assertEqual(terminal.select_finish(), b"abcde")

    def test_two_line_boundary_belongs_to_the_previous_hard_line_end(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(b"Hello world\r\nGoodbye cruel world")
            terminal.select_start(0, 0)
            terminal.select_update(11, 0)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 11, 0))
            self.assertEqual(terminal.select_finish(), b"Hello world")

    def test_three_line_inner_boundary_keeps_the_middle_line_endpoint(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first\r\nsecond\r\nthird")
            terminal.select_start(0, 1)
            terminal.select_update(6, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 1, 6, 1))
            self.assertEqual(terminal.select_finish(), b"second")

    def test_inner_boundary_is_stable_when_the_following_line_is_shorter(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"aaaa\r\nbbbbbbb\r\ncc")
            terminal.select_start(0, 1)
            terminal.select_update(7, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 1, 7, 1))
            self.assertEqual(terminal.select_finish(), b"bbbbbbb")

    def test_middle_position_in_the_middle_storage_epoch_selects_that_cell(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first\r\nsecond\r\nthird")
            terminal.select_start(3, 1)
            terminal.select_update(4, 1)
            self.assertEqual(terminal.select_finish(), b"o")

    def test_zero_position_is_the_first_cell_of_the_first_hard_line(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc\r\nxyz")
            terminal.select_start(0, 0)
            terminal.select_update(1, 0)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 1, 0))
            self.assertEqual(terminal.select_finish(), b"a")

    def test_zero_vertical_offset_keeps_an_inner_boundary_on_the_prior_line(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first\r\nsecond\r\nthird")
            terminal.select_start(0, 1)
            terminal.select_update(6, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 1, 6, 1))
            self.assertEqual(terminal.select_finish(), b"second")

    def test_positive_vertical_offset_advances_from_boundary_to_the_next_row(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first\r\nsecond\r\nthird")
            terminal.select_start(6, 1)
            terminal.select_update(1, 2)
            self.assertEqual(terminal.select_finish(), b"\nt")

    def test_trailing_empty_line_does_not_move_the_content_end_boundary(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc\r\n")
            self.assertEqual(terminal.all_text(), ("abc", ""))
            terminal.select_start(0, 0)
            terminal.select_update(3, 0)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 3, 0))
            self.assertEqual(terminal.select_finish(), b"abc")

    def test_positive_vertical_offset_addresses_the_trailing_empty_line(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc\r\n")
            terminal.select_start(0, 1)
            terminal.select_update(1, 1)
            self.assertEqual(terminal.snapshot().selection, (0, 1, 1, 1))
            self.assertEqual(terminal.select_finish(), b"")

    def test_later_empty_epoch_does_not_steal_the_prior_content_boundary(self):
        with Shitty(columns=10, rows=2, save_lines=0) as terminal:
            terminal.write(b"abc")
            terminal.write(b"\r\n")
            terminal.select_start(0, 0)
            terminal.select_update(3, 0)
            self.assertEqual(terminal.snapshot().selection, (0, 0, 3, 0))
            self.assertEqual(terminal.select_finish(), b"abc")

    def test_line_before_an_inner_boundary_is_the_complete_previous_line(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first\r\nsecond\r\nthird")
            terminal.select_start(0, 1)
            terminal.select_update(6, 1)
            self.assertEqual(terminal.select_finish(), b"second")

    def test_line_before_content_after_an_empty_epoch_is_the_empty_line(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"abc\r\n\r\nxyz")
            self.assertEqual(terminal.all_text(), ("abc", "", "xyz"))
            terminal.select_start(0, 1)
            terminal.select_update(1, 1)
            self.assertEqual(terminal.select_finish(), b"")

    @unittest.expectedFailure
    def test_search_from_an_inner_boundary_finds_the_next_line(self):
        with Shitty(columns=10, rows=3, save_lines=0) as terminal:
            terminal.write(b"first\r\nsecond\r\nxthird")
            self.assertEqual(terminal.all_text(), ("first", "second", "xthird"))
            result = find_text(
                terminal,
                "xthird",
                start=(6, 1),
                backwards=False,
                multiline=False,
            )
            self.assertEqual(result["starts"], ((0, 2),))

    def test_right_prompt_coordinate_survives_a_one_column_shrink(self):
        prompt = b"Prompt>                                                     [abcdefgh]"
        with Shitty(columns=133, rows=3, save_lines=0) as terminal:
            terminal.write(put_rows(b"Blah", prompt, b"Hello world") + b"\x1b[2;71H")
            self.assertEqual((terminal.snapshot().cursor_x, terminal.snapshot().cursor_y), (70, 1))
            terminal.resize(132, 3)
            snapshot = terminal.model_snapshot()
            self.assertEqual((snapshot.cursor_x, snapshot.cursor_y), (70, 1))
            self.assertEqual(
                tuple(line.rstrip() for line in snapshot.lines),
                ("Blah", prompt.decode(), "Hello world"),
            )

    def test_raw_line_count_for_general_physical_ranges(self):
        lines = (
            "Now is the time for all good men to come to the aid of their party",
            "Twas brillig and the slithy toves did gyre and gimble in the wabe",
            "The quick brown fox jumps over the lazy dog.",
            "Every seasoned coder knows the value of clear, concise logic.",
            "Bright stars shimmer quietly above the sleeping valley.",
            "Careful planning prevents needless problems down the line.",
            "The diligent student reviewed each chapter before the exam.",
            "Silence settled across the room as the verdict was read.",
            "Persistent effort turns small advantages into real progress.",
            "The old clock chimed softly as midnight approached.",
            "A well-written test suite guards against subtle regressions.",
            "Steady rain fell while the city continued its hurried pace.",
        )
        with Shitty(columns=30, rows=27, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertEqual(unwrapped_line_count(snapshot, 7, 13), 7)
            self.assertEqual(unwrapped_line_count(snapshot, 0, 27), 12)
            self.assertEqual(unwrapped_line_count(snapshot, 1, 4), 2)

    def test_raw_line_count_includes_each_empty_hard_line(self):
        lines = (
            "Now is the time for all good men to come to the aid of their party",
            "", "", "",
            "Bright stars shimmer quietly above the sleeping valley.",
            "", "", "",
            "Persistent effort turns small advantages into real progress.",
            "The old clock chimed softly as midnight approached.",
            "A well-written test suite guards against subtle regressions.",
            "Steady rain fell while the city continued its hurried pace.",
        )
        with Shitty(columns=30, rows=19, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertEqual(unwrapped_line_count(snapshot, 4, 6), 5)
            self.assertEqual(unwrapped_line_count(snapshot, 0, 18), 12)
            self.assertEqual(unwrapped_line_count(snapshot, 1, 4), 3)

    def test_raw_line_count_at_the_first_physical_boundary(self):
        lines = (
            "Now is the time for all good men to come to the aid of their party",
            "Twas brillig and the slithy toves did gyre and gimble in the wabe",
            "The quick brown fox jumps over the lazy dog.",
            "Every seasoned coder knows the value of clear, concise logic.",
        )
        with Shitty(columns=30, rows=11, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            self.assertEqual(unwrapped_line_count(terminal.model_snapshot(), 0, 3), 1)

    def test_each_single_physical_row_intersects_one_raw_line(self):
        lines = (
            "Now is the time for all good men to come to the aid of their party",
            "Short line",
            "Another line",
        )
        with Shitty(columns=30, rows=5, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertEqual(
                [unwrapped_line_count(snapshot, row, 1) for row in range(4)],
                [1, 1, 1, 1],
            )

    def test_partial_physical_ranges_count_every_intersected_raw_line(self):
        lines = (
            "Now is the time for all good men to come to the aid of their party",
            "Twas brillig and the slithy toves did gyre and gimble in the wabe",
            "Short",
        )
        with Shitty(columns=30, rows=7, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertEqual(unwrapped_line_count(snapshot, 1, 3), 2)
            self.assertEqual(unwrapped_line_count(snapshot, 3, 2), 1)

    def test_soft_eol_segments_count_as_one_raw_line(self):
        with Shitty(columns=30, rows=4, save_lines=0) as terminal:
            terminal.write(b"x" * 100 + b"Short line")
            snapshot = terminal.model_snapshot()
            self.assertEqual(unwrapped_line_count(snapshot, 0, 3), 1)
            self.assertEqual(
                [snapshot.cell(29, row).wrapped for row in range(3)],
                [True, True, True],
            )

    def test_double_width_ranges_count_logical_lines_not_storage_cells(self):
        lines = (
            "日本語の文字列がとても長くなりますので複数行に分かれます",
            "Another line with 中文字符",
            "Regular ASCII text",
        )
        with Shitty(
            columns=30,
            rows=6,
            save_lines=0,
            extra_arguments=("-unicodeWidths", "17"),
        ) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertTrue(snapshot.cell(0, 0).double_width)
            self.assertTrue(snapshot.cell(1, 0).double_width_continuation)
            self.assertEqual(unwrapped_line_count(snapshot, 0, 3), 2)

    def test_empty_physical_range_contains_no_raw_lines(self):
        with Shitty(columns=30, rows=3, save_lines=0) as terminal:
            terminal.write(hard_lines("Line 1", "Line 2", "Line 3"))
            self.assertEqual(unwrapped_line_count(terminal.model_snapshot(), 0, 0), 0)
        with Shitty(columns=30, rows=1, save_lines=0) as terminal:
            self.assertEqual(unwrapped_line_count(terminal.model_snapshot(), 0, 0), 0)

    def test_ranges_inside_a_very_long_raw_line_still_count_one(self):
        with Shitty(columns=30, rows=18, save_lines=0) as terminal:
            terminal.write(hard_lines("abcdefghij" * 50, "Short"))
            snapshot = terminal.model_snapshot()
            self.assertEqual(unwrapped_line_count(snapshot, 5, 5), 1)
            self.assertEqual(unwrapped_line_count(snapshot, 0, 17), 1)

    def test_raw_line_ranges_cross_many_ring_storage_positions(self):
        lines = tuple(
            f"Line {index} with some additional text to fill space"
            for index in range(20)
        )
        with Shitty(columns=30, rows=40, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertGreaterEqual(unwrapped_line_count(snapshot, 10, 10), 5)
            used_rows = snapshot.cursor_y + 1
            self.assertGreater(used_rows, 20)
            self.assertGreaterEqual(
                unwrapped_line_count(snapshot, 5, used_rows - 10),
                10,
            )

    def test_raw_line_ranges_at_both_buffer_limits(self):
        lines = (
            "First line with enough text to wrap around the width limit",
            "Middle line",
            "Last line with enough text to wrap around the width limit too",
        )
        with Shitty(columns=30, rows=7, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            used_rows = snapshot.cursor_y + 1
            self.assertEqual(unwrapped_line_count(snapshot, 0, 1), 1)
            self.assertEqual(unwrapped_line_count(snapshot, used_rows - 1, 1), 1)
            self.assertEqual(unwrapped_line_count(snapshot, 0, used_rows), 3)

    def test_raw_line_ranges_for_varying_logical_lengths(self):
        lines = (
            "x",
            "This line is exactly 30 chars",
            "This line is slightly over 30 characters in length",
            "This is a very long line that will wrap multiple times because it contains a lot of text",
        )
        with Shitty(columns=30, rows=8, save_lines=0) as terminal:
            terminal.write(hard_lines(*lines))
            snapshot = terminal.model_snapshot()
            self.assertEqual(unwrapped_line_count(snapshot, 0, 1), 1)
            self.assertEqual(unwrapped_line_count(snapshot, 1, 1), 1)
            self.assertEqual(unwrapped_line_count(snapshot, 2, 2), 1)
            self.assertEqual(unwrapped_line_count(snapshot, 4, 3), 1)

    @unittest.expectedFailure
    def test_multiline_search_spans_storage_boundaries_forward(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(hard_lines("first line", "second line"))
            self.assertEqual(terminal.all_text(), ("first line", "second line"))
            result = find_text(
                terminal,
                "first line\nsecond",
                start=(0, 0),
                backwards=False,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((0, 0),))

    @unittest.expectedFailure
    def test_multiline_search_finds_a_match_within_one_storage_extent(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(hard_lines("first line", "second line"))
            self.assertEqual(terminal.all_text(), ("first line", "second line"))
            result = find_text(
                terminal,
                "first line\nsecond",
                start=(0, 0),
                backwards=False,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((0, 0),))

    @unittest.expectedFailure
    def test_multiline_search_finds_a_prior_match_backwards(self):
        with Shitty(columns=80, rows=3, save_lines=0) as terminal:
            terminal.write(hard_lines("first line", "second line", "third line"))
            self.assertEqual(
                terminal.all_text(),
                ("first line", "second line", "third line"),
            )
            result = find_text(
                terminal,
                "second line\nthird",
                start=(10, 2),
                backwards=True,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((0, 1),))

    @unittest.expectedFailure
    def test_backward_multiline_search_returns_the_last_match(self):
        with Shitty(columns=80, rows=5, save_lines=0) as terminal:
            terminal.write(hard_lines("hello", "world", "hello", "world", "end"))
            self.assertEqual(
                terminal.all_text(),
                ("hello", "world", "hello", "world", "end"),
            )
            result = find_text(
                terminal,
                "hello\nworld",
                start=(3, 4),
                backwards=True,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((0, 2),))

    @unittest.expectedFailure
    def test_multiline_search_spans_storage_boundaries_backwards(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(hard_lines("first line", "second line"))
            self.assertEqual(terminal.all_text(), ("first line", "second line"))
            result = find_text(
                terminal,
                "first line\nsecond",
                start=(11, 1),
                backwards=True,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((0, 0),))

    @unittest.expectedFailure
    def test_multiline_search_accepts_a_partial_first_line_across_storage(self):
        with Shitty(columns=80, rows=5, save_lines=0) as terminal:
            terminal.write(hard_lines("18", "19", "20", "21", "22"))
            self.assertEqual(terminal.all_text(), ("18", "19", "20", "21", "22"))
            partial = find_text(
                terminal,
                "8\n19\n20\n21\n22",
                start=(0, 0),
                backwards=False,
                multiline=True,
            )
            self.assertEqual(partial["starts"], ((1, 0),))
            complete = find_text(
                terminal,
                "18\n19\n20\n21\n22",
                start=(0, 0),
                backwards=False,
                multiline=True,
            )
            self.assertEqual(complete["starts"], ((0, 0),))

    @unittest.expectedFailure
    def test_backward_multiline_search_accepts_a_partial_first_line(self):
        with Shitty(columns=80, rows=5, save_lines=0) as terminal:
            terminal.write(hard_lines("11", "12", "13", "14", "15"))
            self.assertEqual(terminal.all_text(), ("11", "12", "13", "14", "15"))
            result = find_text(
                terminal,
                "1\n12\n13\n14\n1",
                start=(2, 4),
                backwards=True,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((1, 0),))

    @unittest.expectedFailure
    def test_backward_multiline_search_continues_into_the_prior_storage_extent(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(hard_lines("11", "12"))
            self.assertEqual(terminal.all_text(), ("11", "12"))
            result = find_text(
                terminal,
                "1\n12",
                start=(2, 1),
                backwards=True,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((1, 0),))

    @unittest.expectedFailure
    def test_multiline_search_spans_three_storage_extents(self):
        with Shitty(columns=80, rows=3, save_lines=0) as terminal:
            terminal.write(hard_lines("alpha", "beta", "gamma"))
            self.assertEqual(terminal.all_text(), ("alpha", "beta", "gamma"))
            cases = (
                ("alpha\nbeta", (0, 0)),
                ("beta\ngamma", (0, 1)),
                ("alpha\nbeta\ngamma", (0, 0)),
            )
            for query, expected in cases:
                result = find_text(
                    terminal,
                    query,
                    start=(0, 0),
                    backwards=False,
                    multiline=True,
                )
                self.assertEqual(result["starts"], (expected,))

    @unittest.expectedFailure
    def test_cross_storage_match_reports_the_partial_last_line(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(b"first line\r\nsecond")
            self.assertEqual(terminal.all_text(), ("first line", "second"))
            result = find_text(
                terminal,
                "first line\nsecond",
                start=(0, 0),
                backwards=False,
                multiline=True,
            )
            self.assertEqual(result["starts"], ((0, 0),))
            self.assertTrue(result["includes_partial_last_line"])

    @unittest.expectedFailure
    def test_forward_search_excludes_a_match_at_the_stop_position(self):
        with Shitty(columns=80, rows=3, save_lines=0) as terminal:
            terminal.write(hard_lines("hello world", "hello there", "hello world again"))
            self.assertEqual(
                terminal.all_text(),
                ("hello world", "hello there", "hello world again"),
            )
            result = find_text(
                terminal,
                "hello world",
                start=(0, 1),
                stop=(0, 2),
                backwards=False,
                multiline=False,
            )
            self.assertEqual(result["starts"], ())

    @unittest.expectedFailure
    def test_backward_search_includes_a_match_at_the_stop_position(self):
        with Shitty(columns=80, rows=2, save_lines=0) as terminal:
            terminal.write(hard_lines("Hello world", "Goodbye cruel world"))
            self.assertEqual(terminal.all_text(), ("Hello world", "Goodbye cruel world"))
            result = find_text(
                terminal,
                "Hello",
                start=(19, 1),
                stop=(0, 0),
                backwards=True,
                multiline=False,
            )
            self.assertEqual(result["starts"], ((0, 0),))


if __name__ == "__main__":
    unittest.main()
