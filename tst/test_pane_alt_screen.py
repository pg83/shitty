# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""One pane changes screens while its neighbour says nothing (T2).

The window stops presenting frames - for good - the moment one pane of a
split changes the identity of its Screen. A TUI does that with its first
escape sequence, so the report this came in as was "a TUI will not start
in the new pane": it starts, it runs, it is the window that stands still.

The chain:

  * `\\033[?1049h` swaps the pane's `cf` from `frame_pri` to `frame_alt`
    (vterm.cpp:3722), and `update.shapes` is that pointer (vterm.cpp:2743);
  * the renderer reads a changed `shapes` as a reshaped *frame* and then
    demands every row of every pane in it (render_reference.cpp:842, and
    render_metal.mm:1113 word for word);
  * the neighbour that was not written to hands over `retainedOutput()`,
    which zeroes `damagedRows` by construction (vterm.cpp:2757) - so it
    owes rows it cannot pay;
  * the frame is refused, and application.cpp:601-604 answers a refusal
    by asking for another frame *without changing anything*, so the next
    one is refused for the same reason, and the one after it, forever.

What is pinned here is the last step and not the first: a refusal is a
fair answer to a frame that is genuinely incomplete, and the renderer is
free to give it. What no window may do is give it again to the frame it
asked for itself. So every assertion below is of the shape "the frame
settles no later than the second presentTerminal()", which is what
ApplicationImpl owes whatever the renderer thinks of the first one.

Nothing here waits for time. REPAINT is one requested frame dispatched to
ApplicationImpl and answered with the bool presentTerminal() returned, so
the oracle is a frame that landed or did not - a count, not a clock.
These tests are red until T3.
"""

import unittest
from contextlib import contextmanager

from harness import Shitty


# The escape a full-screen program opens with, plus enough output for the
# alternate screen to be worth looking at. The three parts are one write
# so that the pane speaks exactly once: two writes would be two frames,
# and the second one would arrive after the shapes had already moved.
ENTER_ALTERNATE = b"\033[?1049h\033[H\033[2JALTERNATE"

# ?47h is the older way in (parser.cpp:1245 -> setAlternateScreen(true,
# false)): the same Screen swap reached through another sequence and
# another branch, with none of 1049's clearing or cursor saving.
ENTER_ALTERNATE_LEGACY = b"\033[?47h"


def rendered_rows(text):
    """The non-blank rows of a pane's screen, stripped of their padding."""
    return [row.rstrip() for row in text.split("\n") if row.strip()]


def expect_frame(terminal, after):
    """Dispatch one more frame; complain in words if it is refused.

    REPAINT asks the window for a frame and reports what
    ApplicationImpl::frame() answered, so a refusal arrives here as an
    error rather than as a test that waits. The message matters: the
    symptom on a real desktop is a window that has simply stopped, with
    nothing in the log, and a bare timeout in CI would describe it no
    better than the bug report did.
    """
    try:
        terminal.repaint()
    except RuntimeError as refused:
        raise AssertionError(
            f"the frame never settled after {after}: the renderer refused "
            f"it again on a frame the window asked for itself ({refused}). "
            "A refusal answered by requestFrame() and nothing else comes "
            "back the same way every time, so this window is stalled and "
            "not slow - it will not present again while the split lives."
        ) from None


def split_in_two(terminal):
    """Two panes, both drawn, both silent - the state the defect needs.

    A frame that lands consumes the output of every pane that spoke
    (application.cpp), so the panes fall silent on their own once a frame
    goes through; the split itself damages both of them, through
    applyLayout() -> paneResized(), and that first frame carries all of
    it. The extra frames are for the window geometry, which settles over
    the first few and speaks for every pane again while it does.

    Three is a count of frames and not an interval - there is nothing
    here that could be too fast on a loaded machine.
    """
    terminal.split("V")
    for _ in range(3):
        expect_frame(terminal, "the split")


@contextmanager
def two_panes_speaking(columns=40, rows=10):
    """A window of two settled panes, each with a word of its own."""
    with Shitty(columns=columns, rows=rows, extra_arguments=("-panes",)) as terminal:
        split_in_two(terminal)
        terminal.write_to_pane(0, b"FIRST\r\n")
        terminal.write_to_pane(1, b"SECOND\r\n")
        expect_frame(terminal, "both panes writing a line")
        yield terminal


class PaneAlternateScreenTest(unittest.TestCase):
    def test_a_pane_entering_the_alternate_screen_lets_the_frame_settle(self):
        # The reported case, in its smallest form: one pane of two runs
        # something full-screen, the other sits at a prompt with nothing
        # to say. Today the window never presents again.
        with two_panes_speaking() as terminal:
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            expect_frame(terminal, "one pane entered the alternate screen")
            self.assertEqual(rendered_rows(terminal.screen_text_pane(0)), ["ALTERNATE"])
            # The pane that said nothing is still shown, and shown whole:
            # its retained form is what the frame carried for it, and a
            # window that dropped it would be as wrong as one that froze.
            self.assertEqual(rendered_rows(terminal.screen_text_pane(1)), ["SECOND"])

    def test_the_focused_pane_entering_the_alternate_screen_settles_too(self):
        # Which pane holds the focus is no part of the defect, and this
        # is the case that says so. The split leaves the focus on the new
        # pane, so the test above has the *silent* pane focused; here the
        # focus is moved onto the pane that speaks. That is the shape the
        # bug was reported in - the user splits, gets the new pane, and
        # starts the program in the pane they are looking at.
        #
        # Its own case because a window that answered the refusal by
        # repainting the focused pane alone would pass one of the two and
        # fail the other, and which one would be decided by where the
        # split put the focus. That is the mistake application.cpp:663-667
        # already makes on the renderer-rebuild path (plan, T3.3).
        with two_panes_speaking() as terminal:
            terminal.focus_pane(0)
            expect_frame(terminal, "moving the focus onto the first pane")
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            expect_frame(terminal, "the focused pane entered the alternate screen")
            self.assertEqual(rendered_rows(terminal.screen_text_pane(0)), ["ALTERNATE"])
            self.assertEqual(rendered_rows(terminal.screen_text_pane(1)), ["SECOND"])

    def test_a_pane_leaving_the_alternate_screen_lets_the_frame_settle(self):
        # The way out is the same swap in the other direction
        # (vterm.cpp:3756), so a program that somehow drew itself would
        # still stall the window on the way out. Built by entering the
        # alternate screen while the tab still has one pane - that frame
        # settles, there being no neighbour to owe rows - and splitting
        # after.
        with Shitty(columns=40, rows=10, extra_arguments=("-panes",)) as terminal:
            terminal.write_to_pane(0, b"PRIMARY\r\n")
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            expect_frame(terminal, "a lone pane entered the alternate screen")
            split_in_two(terminal)
            terminal.write_to_pane(1, b"SECOND\r\n")
            expect_frame(terminal, "the second pane writing a line")

            terminal.write_to_pane(0, b"\033[?1049l")
            expect_frame(terminal, "one pane left the alternate screen")
            # Back on the primary screen, with what was written there
            # before the program took over.
            self.assertEqual(rendered_rows(terminal.screen_text_pane(0)), ["PRIMARY"])
            self.assertEqual(rendered_rows(terminal.screen_text_pane(1)), ["SECOND"])

    def test_the_legacy_alternate_screen_sequence_stalls_the_window_too(self):
        # Not about 1049. ?47h reaches switchScreenBufferMode() through a
        # different parser branch and does none of 1049's extra work, and
        # the window stops just the same - because what the renderer
        # compares is the Screen pointer, and every route that moves it
        # is the same defect. A fix written against the 1049 sequence
        # rather than against the pointer leaves this red.
        with two_panes_speaking() as terminal:
            terminal.write_to_pane(0, ENTER_ALTERNATE_LEGACY)
            expect_frame(terminal, "one pane switched screens with ?47h")
            self.assertEqual(rendered_rows(terminal.screen_text_pane(1)), ["SECOND"])

    def test_rebuilding_the_alternate_screen_in_place_stalls_the_window_too(self):
        # The purest form of "the shapes moved": a pane already on the
        # alternate screen is sent ?1049h again, and switchScreenBufferMode
        # rebuilds frame_alt where it stands (vterm.cpp:3695). No screen
        # is entered and none is left - only the pointer changes - and
        # that alone is enough to freeze the window. Nothing about
        # primary-versus-alternate can explain this one.
        with Shitty(columns=40, rows=10, extra_arguments=("-panes",)) as terminal:
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            expect_frame(terminal, "a lone pane entered the alternate screen")
            split_in_two(terminal)
            terminal.write_to_pane(1, b"SECOND\r\n")
            expect_frame(terminal, "the second pane writing a line")

            terminal.write_to_pane(0, b"\033[?1049h\033[H\033[2JAGAIN")
            expect_frame(terminal, "the alternate screen was rebuilt in place")
            self.assertEqual(rendered_rows(terminal.screen_text_pane(0)), ["AGAIN"])
            self.assertEqual(rendered_rows(terminal.screen_text_pane(1)), ["SECOND"])

    def test_a_stalled_window_does_not_come_back_on_its_own(self):
        # The half of the defect that makes it a bug rather than a
        # dropped frame. consume() is not reached on a refusal, so the
        # pane offers the same output next time and the neighbour the
        # same nothing; panes_ in the renderer is not updated either, so
        # the shapes it compares against stay the old ones. Every frame
        # after the first is a copy of it. Five is arbitrary - one is
        # already too many - and any of them refusing is the failure.
        with two_panes_speaking() as terminal:
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            for attempt in range(5):
                expect_frame(terminal, f"the screen change, frame {attempt + 1}")

    def test_a_third_pane_makes_no_difference(self):
        # The count that matters is "more than one", not "two": any pane
        # with nothing to say owes the reshaped frame rows it has not
        # got. Kept as its own case because a fix that walked the panes
        # pairwise would pass the two-pane tests and fail here.
        with Shitty(columns=60, rows=12, extra_arguments=("-panes",)) as terminal:
            terminal.split("V")
            terminal.split("V")
            for _ in range(3):
                expect_frame(terminal, "the two splits")
            self.assertEqual(terminal.pane_count(), 3)
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            expect_frame(terminal, "one pane of three entered the alternate screen")


class QuietNeighbourTest(unittest.TestCase):
    """The controls: what the window does today when it does present.

    These pass before T3 as well as after it. They are here because the
    red tests above only mean something if the pieces they blame are
    innocent on their own - a fix that settled the frame by, say, never
    reshaping again would turn the red ones green and these ones red.
    """

    def test_a_lone_pane_changing_screens_settles(self):
        # Why the report said "the new pane": before the split there is
        # no pane that can owe rows, so the same TUI in the same terminal
        # is fine. The defect is the split, not the screen change.
        with Shitty(columns=40, rows=10, extra_arguments=("-panes",)) as terminal:
            terminal.write_to_pane(0, b"FIRST\r\n")
            expect_frame(terminal, "a line in the only pane")
            self.assertEqual(terminal.pane_count(), 1)
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            expect_frame(terminal, "the only pane entered the alternate screen")
            self.assertEqual(rendered_rows(terminal.screen_text_pane(0)), ["ALTERNATE"])

    def test_a_frame_no_pane_is_silent_in_settles(self):
        # The other half of the same statement: with the neighbour
        # damaged too, the renderer's demand is met and the frame lands.
        # So the refusal is about the silent pane and not about the
        # screen change - which is why the fix is owed to the retry and
        # not to the check (plan, R1).
        #
        # It is also the escape hatch users find by accident - resize the
        # window, change the font, or let the other shell print a
        # screenful - and the measurement that proved the diagnosis: a
        # full repaint of the neighbour is the one thing that can make a
        # later frame differ from the one being refused, which is exactly
        # what exposeAll() is to do on the window's own initiative in T3.
        # A refused frame does not reach consume(), so pane 0 still
        # carries its screen change when pane 1 is damaged below: the two
        # arrive in one frame however many were refused before it.
        #
        # Deliberately says nothing about those. Whether any of them was
        # refused is the subject of the red tests above, and asserting a
        # refusal here would make this case fail on the fix rather than
        # on the defect.
        with two_panes_speaking() as terminal:
            terminal.write_to_pane(0, ENTER_ALTERNATE)
            terminal.write_to_pane(1, b"\033[H\033[2JAWAKE")
            expect_frame(terminal, "the quiet pane was damaged in full")
            self.assertEqual(rendered_rows(terminal.screen_text_pane(0)), ["ALTERNATE"])
            self.assertEqual(rendered_rows(terminal.screen_text_pane(1)), ["AWAKE"])


if __name__ == "__main__":
    unittest.main()
