"""Terminals behind one window.

Terminal-bound harness commands (write, read_input, screen state...)
address the session the window shows: switch by chord or NEW_SESSION,
then poke. write_to/read_input_of reach a background session the way
its own shell would.
"""

import time
import unittest

from harness import ShittyWithTabs as Shitty


class TabTest(unittest.TestCase):
    def test_a_fresh_window_has_one_session_and_shows_it(self):
        with Shitty(columns=8, rows=2) as terminal:
            self.assertEqual(terminal.session_state(), (1, 0))

    def test_opening_a_session_shows_it_and_keeps_the_first(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            self.assertEqual(terminal.session_state(), (2, 1))
            terminal.new_session()
            self.assertEqual(terminal.session_state(), (3, 2))

    def test_closing_a_session_activates_a_neighbour(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            terminal.close_session(2)
            count, active = terminal.session_state()
            self.assertEqual(count, 2)
            self.assertLess(active, count)

    def test_closing_back_to_one_leaves_a_session_showing(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.close_session(1)
            self.assertEqual(terminal.session_state(), (1, 0))

    def test_chords_cycle_sessions_with_wraparound(self):
        # The real keyboard route: InputBindings claims the chord ahead of
        # the session set, exactly like a user's fingers would.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.new_session()
            self.assertEqual(terminal.session_state(), (3, 2))
            terminal.chord_next_tab()
            self.assertEqual(terminal.session_state(), (3, 0))
            terminal.chord_next_tab()
            self.assertEqual(terminal.session_state(), (3, 1))
            terminal.chord_prev_tab()
            self.assertEqual(terminal.session_state(), (3, 0))
            terminal.chord_prev_tab()
            self.assertEqual(terminal.session_state(), (3, 2))

    def test_close_chord_closes_the_active_session(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_close_tab()
            self.assertEqual(terminal.session_state(), (1, 0))

    def test_chords_do_not_leak_into_any_shell(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_next_tab()
            terminal.chord_prev_tab()
            self.assertEqual(terminal.read_input_of(0), b"")
            self.assertEqual(terminal.read_input_of(1), b"")

    def test_clear_chord_reaches_only_the_active_shell(self):
        # The platform clear chord sends the shell its own Ctrl+L, routed
        # like every other action: to the session the window shows.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.chord_clear()
            self.assertEqual(terminal.read_input_of(0), b"")
            self.assertEqual(terminal.read_input_of(1), b"\x0c")

    def test_commands_follow_the_active_session(self):
        # write() feeds whichever session is shown; each screen keeps its
        # own content across switches.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"AB")
            terminal.new_session()
            terminal.write(b"XY")
            self.assertIn("XY", terminal.screen_text())
            self.assertNotIn("AB", terminal.screen_text())
            terminal.chord_prev_tab()
            terminal.present()
            self.assertIn("AB", terminal.screen_text())
            self.assertNotIn("XY", terminal.screen_text())

    def test_feeding_a_background_session_stays_off_the_shown_screen(self):
        # A background shell keeps talking while another tab is up: its
        # bytes must neither show through nor get lost.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"AB")
            terminal.new_session()
            terminal.write_to(0, b"XY")
            self.assertNotIn("XY", terminal.screen_text())
            terminal.close_session(1)
            terminal.present()
            self.assertIn("ABXY", terminal.screen_text())

    def test_typing_reaches_only_the_active_sessions_shell(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.frontend_text_event("x")
            self.assertEqual(terminal.read_input_of(0), b"")
            self.assertEqual(terminal.read_input_of(1), b"x")
            terminal.close_session(1)
            terminal.frontend_text_event("y")
            self.assertEqual(terminal.read_input_of(0), b"y")

    def test_background_responses_go_to_their_own_shell(self):
        # A background session's parser answers its own shell: the cursor
        # report lands in its pty, never in the one the window shows.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.read_input_of(0)
            terminal.read_input_of(1)
            terminal.write_to(0, b"\x1b[6n")
            self.assertEqual(terminal.read_input_of(0), b"\x1b[1;1R")
            self.assertEqual(terminal.read_input_of(1), b"")

    def test_bracketed_paste_is_per_session(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?2004h")
            terminal.read_input_of(0)
            terminal.new_session()
            terminal.paste(b"hi")
            self.assertEqual(terminal.read_input_of(1), b"hi")
            self.assertEqual(terminal.read_input_of(0), b"")
            terminal.chord_prev_tab()
            terminal.paste(b"yo")
            self.assertEqual(
                terminal.read_input_of(0), b"\x1b[200~yo\x1b[201~"
            )
            self.assertEqual(terminal.read_input_of(1), b"")

    def test_mouse_reporting_stays_with_the_session_that_enabled_it(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[?1000h")
            terminal.read_input_of(0)
            terminal.new_session()
            terminal.button(1, True)
            terminal.button(1, False)
            self.assertEqual(terminal.read_input_of(0), b"")
            self.assertEqual(terminal.read_input_of(1), b"")
            terminal.chord_prev_tab()
            terminal.button(1, True)
            terminal.button(1, False)
            self.assertNotEqual(terminal.read_input_of(0), b"")
            self.assertEqual(terminal.read_input_of(1), b"")

    def test_kitty_keyboard_mode_is_per_session(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[>1u")
            self.assertEqual(terminal.state()[3], 1)
            terminal.new_session()
            self.assertEqual(terminal.state()[3], 0)
            terminal.chord_prev_tab()
            self.assertEqual(terminal.state()[3], 1)

    def test_alt_screen_survives_the_background(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"base")
            terminal.write(b"\x1b[?1049h\x1b[HALT")
            terminal.new_session()
            terminal.write(b"other")
            terminal.chord_prev_tab()
            terminal.present()
            self.assertIn("ALT", terminal.screen_text())
            terminal.write(b"\x1b[?1049l")
            terminal.present()
            self.assertIn("base", terminal.screen_text())

    def test_background_sessions_track_the_window_resize(self):
        # A resize lands on every session, background ones included; a
        # terminal that only resized on activation would come back with
        # the old grid.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.resize(6, 2)
            self.assertEqual(terminal.winsize(), (6, 2))
            terminal.close_session(1)
            terminal.present()
            self.assertEqual(terminal.winsize(), (6, 2))
            rows = terminal.screen_text().splitlines()
            self.assertTrue(rows)
            for row in rows:
                self.assertLessEqual(len(row), 6)

    def test_selection_survives_the_background(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"ABCDEF")
            terminal.select_start(0, 0)
            terminal.select_extend(3, 0)
            terminal.select_finish()
            before = terminal.selection_state()
            terminal.new_session()
            terminal.write(b"noise")
            terminal.chord_prev_tab()
            self.assertEqual(terminal.selection_state(), before)

    def test_scrollback_position_survives_the_background(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"one\r\ntwo\r\nthree\r\nfour")
            terminal.present()
            bottom = terminal.screen_text()
            terminal.page_up()
            terminal.present()
            shown = terminal.screen_text()
            self.assertNotEqual(shown, bottom)
            terminal.new_session()
            terminal.write(b"noise")
            terminal.chord_prev_tab()
            terminal.present()
            self.assertEqual(terminal.screen_text(), shown)

    def test_focus_state_replays_into_the_activated_session(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.focus(True)
            terminal.write(b"\x1b[?1004h")
            terminal.read_input_of(0)
            terminal.new_session()
            # Going background is a focus-out for the shell left behind.
            self.assertEqual(terminal.read_input_of(0), b"\x1b[O")
            terminal.focus(False)
            terminal.chord_prev_tab()
            # Shown again while the window itself is unfocused: activation
            # must not invent focus the session does not have.
            self.assertEqual(terminal.read_input_of(0), b"")
            terminal.focus(True)
            self.assertEqual(terminal.read_input_of(0), b"\x1b[I")
            # And a switch under a focused window is out on leave, in on
            # return - exactly once each.
            terminal.chord_next_tab()
            self.assertEqual(terminal.read_input_of(0), b"\x1b[O")
            terminal.chord_prev_tab()
            self.assertEqual(terminal.read_input_of(0), b"\x1b[I")

    def test_title_follows_the_active_session(self):
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b]2;first\x07")
            self.assertEqual(terminal.window_title(), "first")
            terminal.new_session()
            terminal.write(b"\x1b]2;second\x07")
            self.assertEqual(terminal.window_title(), "[2/2] second")
            # A background title change stays the background's until it
            # is shown; it must not clobber the window under another
            # session's index.
            terminal.write_to(0, b"\x1b]2;sneaky\x07")
            self.assertEqual(terminal.window_title(), "[2/2] second")
            terminal.chord_prev_tab()
            self.assertEqual(terminal.window_title(), "[1/2] sneaky")

    def test_osc52_from_the_background_sets_the_clipboard(self):
        # Deliberate: a background shell may still own the clipboard,
        # exactly like it may ring the bell - pin the policy so it only
        # changes on purpose. "Ym9v" is "boo".
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.write_to(0, b"\x1b]52;c;Ym9v\x1b\\")
            self.assertEqual(terminal.get_selection(primary=False), b"boo")
            self.assertEqual(terminal.read_input_of(1), b"")

    def test_close_with_a_stalled_transaction_stays_coherent(self):
        # The paste transaction parks on scripted backpressure; closing
        # its session must neither crash nor drop the arena from under
        # the suspended fiber, and the survivor keeps working.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.new_session()
            terminal.script_pty_writes(1)
            terminal.paste(b"0123456789")
            terminal.close_session(1)
            self.assertEqual(terminal.session_state(), (1, 0))
            terminal.write(b"OK")
            terminal.present()
            self.assertIn("OK", terminal.screen_text())

    def test_closing_a_blinking_background_session_is_clean(self):
        # The first session arms its blink deadline, goes to the
        # background and is closed with the timer still pending; later
        # blink ticks must find a coherent world.
        with Shitty(columns=8, rows=2) as terminal:
            terminal.write(b"\x1b[5mAB\x1b[0m")
            terminal.new_session()
            terminal.close_session(0)
            self.assertEqual(terminal.session_state(), (1, 0))
            terminal.blink_tick()
            self.assertEqual(terminal.session_state(), (1, 0))

    def test_a_closed_sessions_arena_is_reaped_and_reused(self):
        # A session opened and closed leaves a grave; after a presented
        # frame the reaper drops its arena, and the window keeps opening
        # fresh sessions over the reclaimed ground.
        with Shitty(columns=8, rows=2) as terminal:
            for _ in range(3):
                terminal.new_session()
                terminal.close_session(1)
                terminal.present()
                # The reaper polls its graves; give the deadline a chance
                # to pass before the next command turns the loop.
                time.sleep(0.05)
                self.assertEqual(terminal.session_state(), (1, 0))

    def test_a_storm_of_sessions_stays_consistent(self):
        with Shitty(columns=8, rows=2) as terminal:
            for round_index in range(5):
                terminal.new_session()
                terminal.write(b"S%d" % (round_index + 1))
                terminal.new_session()
                terminal.chord_prev_tab()
                self.assertIn(
                    "S%d" % (round_index + 1), terminal.screen_text()
                )
                terminal.chord_next_tab()
                terminal.close_session(terminal.session_state()[1])
            count, active = terminal.session_state()
            self.assertEqual(count, 6)
            self.assertLess(active, count)


if __name__ == "__main__":
    unittest.main()
