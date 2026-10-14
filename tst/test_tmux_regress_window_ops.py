# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""Public session adaptations of applicable current tmux window-ops."""

import unittest

from harness import ShittyWithTabs as Shitty


PORTED_CASES = (
    ("regress/window-ops.sh:new-session-count", "test_fresh_session_count"),
    ("regress/window-ops.sh:new-session-active", "test_fresh_session_active"),
    (
        "regress/window-ops.sh:new-window-first-count",
        "test_first_new_session_count",
    ),
    (
        "regress/window-ops.sh:new-window-first-active",
        "test_first_new_session_active",
    ),
    (
        "regress/window-ops.sh:new-window-second-count",
        "test_second_new_session_count",
    ),
    (
        "regress/window-ops.sh:new-window-second-active",
        "test_second_new_session_active",
    ),
    (
        "regress/window-ops.sh:replace-close-count",
        "test_replace_close_count",
    ),
    (
        "regress/window-ops.sh:replace-close-active",
        "test_replace_close_active",
    ),
    (
        "regress/window-ops.sh:replace-reopen-count",
        "test_replace_reopen_count",
    ),
    (
        "regress/window-ops.sh:replace-reopen-active",
        "test_replace_reopen_active",
    ),
    (
        "regress/window-ops.sh:kill-current-count",
        "test_kill_current_count",
    ),
    (
        "regress/window-ops.sh:kill-current-previous",
        "test_kill_current_activates_previous",
    ),
    (
        "regress/window-ops.sh:kill-all-others",
        "test_close_all_others_leaves_one",
    ),
    ("regress/window-ops.sh:select-next-first", "test_select_next_first"),
    ("regress/window-ops.sh:select-next-second", "test_select_next_second"),
    ("regress/window-ops.sh:select-next-wrap", "test_select_next_wrap"),
    (
        "regress/window-ops.sh:select-previous-wrap",
        "test_select_previous_wrap",
    ),
    (
        "regress/window-ops.sh:selection-keeps-count",
        "test_selection_keeps_count",
    ),
    (
        "regress/window-ops.sh:selection-shows-window-content",
        "test_selection_shows_session_content",
    ),
    (
        "regress/window-ops.sh:selection-shows-window-name",
        "test_selection_shows_session_title",
    ),
)


class TmuxRegressWindowOpsTest(unittest.TestCase):
    def test_upstream_inventory_has_20_distinct_executable_cases(self):
        self.assertEqual(len(PORTED_CASES), 20)
        self.assertEqual(len({source for source, _ in PORTED_CASES}), 20)
        self.assertEqual(len({test for _, test in PORTED_CASES}), 20)
        for _, test in PORTED_CASES:
            self.assertTrue(callable(getattr(self, test)))

    def test_fresh_session_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            self.assertEqual(terminal.session_state()[0], 1)

    def test_fresh_session_active(self):
        with Shitty(columns=8, rows=2) as terminal:
            self.assertEqual(terminal.session_state()[1], 0)

    def test_first_new_session_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            self.assertEqual(terminal.session_state()[0], 2)

    def test_first_new_session_active(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            self.assertEqual(terminal.session_state()[1], 1)

    def test_second_new_session_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            self.assertEqual(terminal.session_state()[0], 3)

    def test_second_new_session_active(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            self.assertEqual(terminal.session_state()[1], 2)

    def test_replace_close_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_close_tab()
            self.assertEqual(terminal.session_state()[0], 1)

    def test_replace_close_active(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_close_tab()
            self.assertEqual(terminal.session_state()[1], 0)

    def test_replace_reopen_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_close_tab()
            terminal.new_session()
            self.assertEqual(terminal.session_state()[0], 2)

    def test_replace_reopen_active(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_close_tab()
            terminal.new_session()
            self.assertEqual(terminal.session_state()[1], 1)

    def test_kill_current_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_close_tab()
            self.assertEqual(terminal.session_state()[0], 2)

    def test_kill_current_activates_previous(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_close_tab()
            self.assertEqual(terminal.session_state()[1], 1)

    def test_close_all_others_leaves_one(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_close_tab()
            terminal.chord_close_tab()
            self.assertEqual(terminal.session_state(), (1, 0))

    def test_select_next_first(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_next_tab()
            terminal.chord_next_tab()
            self.assertEqual(terminal.session_state()[1], 1)

    def test_select_next_second(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_next_tab()
            terminal.chord_next_tab()
            terminal.chord_next_tab()
            self.assertEqual(terminal.session_state()[1], 2)

    def test_select_next_wrap(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_next_tab()
            self.assertEqual(terminal.session_state()[1], 0)

    def test_select_previous_wrap(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.chord_next_tab()
            terminal.chord_prev_tab()
            self.assertEqual(terminal.session_state()[1], 2)

    def test_selection_keeps_count(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            for _ in range(7):
                terminal.chord_next_tab()
                terminal.chord_prev_tab()
            self.assertEqual(terminal.session_state()[0], 3)

    def test_selection_shows_session_content(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"first")
            terminal.new_session()
            terminal.write(b"second")
            terminal.chord_prev_tab()
            terminal.present()
            self.assertIn("first", terminal.screen_text())
            self.assertNotIn("second", terminal.screen_text())

    def test_selection_shows_session_title(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b]2;first\x07")
            terminal.new_session()
            terminal.write(b"\x1b]2;second\x07")
            terminal.chord_prev_tab()
            self.assertEqual(terminal.window_title(), "[1/2] first")


if __name__ == "__main__":
    unittest.main()
