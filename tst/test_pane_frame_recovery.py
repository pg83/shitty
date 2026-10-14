# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""What the answer to a refused frame costs, and when it is paid (R3).

tst/test_pane_alt_screen.py (T2) pins the symptom: after one pane of a
split changes the identity of its Screen, the window keeps presenting.
It says nothing about *how*, and it cannot - a window that answered every
frame by repainting every pane would pass all nine of those tests while
turning a terminal into a machine that redraws the screen on every
keystroke.

So this file measures the frames instead of counting them. LAST_UPDATE
reports the cells and the row spans of the last update() a backend
accepted, for the pane it drew last (render_reference.cpp, updateOnce),
and that number separates the two shapes of frame T3 has to keep apart:

  * an ordinary frame carries the rows that were damaged and no others -
    one row for one line printed, and *no* rows at all for a pane that
    was not written to, which hands over its retained form;
  * the frame that answers a refusal carries every row of every pane,
    because exposeAll() has just damaged both screens of each of them
    (vterm.cpp) - which is the one thing that can make the frame the
    backend refused come back different.

Both are asserted, and the pair is the point. Reading only the second
would pass on a window that exposed everything always; reading only the
first would pass on a window that never recovered at all.

The measurements are stats of the last *accepted* update and not a
clock: REPAINT is one requested frame dispatched and answered, and a
frame that is refused twice leaves the numbers where the last accepted
frame left them.
"""

import unittest
from contextlib import contextmanager

from harness import Shitty


COLUMNS = 40
ROWS = 10

# Two panes side by side, so a pane's grid is half the window's columns
# less the divider. Read off the terminal rather than computed here: the
# split's arithmetic is pane_layout's business and not this file's.
ENTER_ALTERNATE = b"\033[?1049h\033[H\033[2JALTERNATE"
LEAVE_ALTERNATE = b"\033[?1049l"


@contextmanager
def settled_panes(count=2, columns=COLUMNS, rows=ROWS):
    """A window of `count` panes whose geometry has stopped moving.

    The split damages both panes whole and the window's geometry settles
    over the first few frames, speaking for every pane while it does
    (application_ut.cpp says the same thing at the other end). Frames,
    not seconds: there is nothing here that a loaded machine could make
    too fast.
    """
    with Shitty(columns=columns, rows=rows, extra_arguments=("-panes",)) as terminal:
        for _ in range(count - 1):
            terminal.split("V")
        for _ in range(4):
            terminal.repaint()
        if terminal.pane_count() != count:
            raise AssertionError(f"the window has {terminal.pane_count()} panes, not {count}")
        yield terminal


def pane_grid(terminal, index):
    """(columns, rows) of pane index, read off that pane's own pty."""
    return terminal.winsize_pane(index)


def visible_rows(text):
    """The non-blank rows of a pane's screen, stripped of their padding."""
    return [row.rstrip() for row in text.split("\n") if row.strip()]


class DamageOnlyBetweenScreenChangesTest(unittest.TestCase):
    """T3's fourth acceptance criterion, measured rather than read.

    "exposeAll() is not called on the successful path" is a statement
    about frames nobody refused, and the two cases below are the two
    shapes such a frame comes in: a pane that spoke a little, and a pane
    that did not speak at all.
    """

    def test_one_printed_line_costs_the_frame_one_row(self):
        with settled_panes() as terminal:
            columns, rows = pane_grid(terminal, 1)
            self.assertGreater(rows, 1)

            # The last pane of the frame is the one LAST_UPDATE answers
            # for, so the line goes there. One line printed at the home
            # cell damages the row it landed on and nothing else.
            terminal.write_to_pane(1, b"ONE\r\n")

            self.assertEqual(terminal.last_update(), (columns, 1))
            self.assertEqual(terminal.last_update_rows(), (0,))

    def test_a_pane_that_said_nothing_is_not_drawn_at_all(self):
        with settled_panes() as terminal:
            # Only the *first* pane speaks. The second one hands over its
            # retained form, which carries no damage by construction
            # (vterm.cpp, retainedOutput), and the frame draws none of
            # its rows - a zero that only holds while nothing exposed it.
            terminal.write_to_pane(0, b"ONLY THE FIRST\r\n")

            self.assertEqual(terminal.last_update(), (0, 0))
            self.assertEqual(terminal.last_update_rows(), ())

    def test_the_frames_after_a_recovery_are_ordinary_again(self):
        # The exposure is an answer to one refusal and not a mode the
        # window falls into: a fix that armed a "draw everything" flag
        # and never cleared it would pass every test in
        # test_pane_alt_screen.py and fail this one.
        with settled_panes() as terminal:
            columns, _ = pane_grid(terminal, 1)

            terminal.write_to_pane(0, ENTER_ALTERNATE)
            terminal.repaint()

            terminal.write_to_pane(1, b"AFTER\r\n")
            self.assertEqual(terminal.last_update(), (columns, 1))

            terminal.write_to_pane(0, b"MORE\r\n")
            self.assertEqual(terminal.last_update(), (0, 0))


class ScreenChangeCostsOneWholeFrameTest(unittest.TestCase):
    """The other half: the frame that answers the refusal, and its price.

    R1 named the price out loud - one lost frame and a full redraw of
    every pane on every screen switch - and made T4 measure it. What is
    pinned here is that the price is the one that was agreed: every row
    of the quiet pane, once, on the frame the change arrived in.
    """

    def test_a_screen_change_hands_the_quiet_pane_over_whole(self):
        with settled_panes() as terminal:
            columns, rows = pane_grid(terminal, 1)

            # Pane 1 is silent through all of this; pane 0 is the one
            # that changes screens. Before the change the frames cost
            # pane 1 nothing at all, which is the number the one below is
            # read against.
            terminal.write_to_pane(0, b"BEFORE\r\n")
            self.assertEqual(terminal.last_update(), (0, 0))

            terminal.write_to_pane(0, ENTER_ALTERNATE)

            # And now it costs every row of it: exposeAll() damaged both
            # of its screens, so what it hands over is a full frame
            # instead of the retained form that owed rows it had not got.
            self.assertEqual(terminal.last_update(), (columns * rows, rows))
            self.assertEqual(terminal.last_update_rows(), tuple(range(rows)))

    def test_leaving_the_alternate_screen_costs_the_same(self):
        with settled_panes() as terminal:
            columns, rows = pane_grid(terminal, 1)

            terminal.write_to_pane(0, ENTER_ALTERNATE)
            terminal.repaint()
            terminal.write_to_pane(1, b"QUIET\r\n")
            self.assertEqual(terminal.last_update(), (columns, 1))

            terminal.write_to_pane(0, LEAVE_ALTERNATE)
            self.assertEqual(terminal.last_update(), (columns * rows, rows))

    def test_the_frame_lands_on_the_callback_it_was_asked_for(self):
        # T3's third acceptance criterion in its strict form. The window
        # is handed one frame callback and answers it with a frame that
        # landed - not with a request for another one it would answer
        # next time. REPAINT reports what ApplicationImpl::frame()
        # returned, so a window that deferred the recovery by a frame
        # would raise on the first call and pass on the second.
        with settled_panes() as terminal:
            columns, rows = pane_grid(terminal, 1)

            # Something drawn since the split settled, so the numbers
            # below have to move to be the numbers of this frame. Without
            # it a refused frame would leave the split's own full-frame
            # figures standing and the assertion would read them.
            terminal.write_to_pane(1, b"MARK\r\n")
            self.assertEqual(terminal.last_update(), (columns, 1))

            terminal.write_to_pane(0, ENTER_ALTERNATE)
            terminal.repaint()

            self.assertEqual(terminal.last_update(), (columns * rows, rows))
            # The quiet pane was handed over whole, not replaced: what it
            # was showing is what it still shows.
            self.assertEqual(visible_rows(terminal.screen_text_pane(1)), ["MARK"])
            self.assertEqual(visible_rows(terminal.screen_text_pane(0)), ["ALTERNATE"])


class EveryQuietPaneIsHandedOverTest(unittest.TestCase):
    def test_a_third_pane_is_handed_over_too(self):
        # The count that matters is "all of them". A window that exposed
        # the pane that changed and the one next to it would settle the
        # frame with two panes and stall with three - and LAST_UPDATE
        # answers for the *last* pane of the frame, which is the one such
        # a fix would leave out.
        with settled_panes(count=3, columns=60, rows=12) as terminal:
            columns, rows = pane_grid(terminal, 2)

            terminal.write_to_pane(0, b"HEAD\r\n")
            self.assertEqual(terminal.last_update(), (0, 0))

            terminal.write_to_pane(0, ENTER_ALTERNATE)
            self.assertEqual(terminal.last_update(), (columns * rows, rows))


if __name__ == "__main__":
    unittest.main()
