/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pane_layout.h"

#include <std/lib/vector.h>
#include <std/tst/ut.h>

using namespace stl;

namespace {
    // Pane ids are opaque to the tree, so the tests name them by hand.
    // They are deliberately not 1, 2, 3 in tree order: an implementation
    // that answered with an index instead of the id it was given would
    // pass on consecutive ids and fail here.
    constexpr u64 first = 11;
    constexpr u64 second = 22;
    constexpr u64 third = 33;
    constexpr u64 fourth = 44;

    Vector<u64> panesOf(const PaneTree& tree) {
        Vector<u64> out;
        tree.panes(out);
        return out;
    }

    Vector<PanePlacement> layoutOf(const PaneTree& tree, const PixelRect& area, u16 divider = 0) {
        Vector<PanePlacement> out;
        tree.layout(area, divider, out);
        return out;
    }

    const PanePlacement* placementOf(const Vector<PanePlacement>& placements, u64 pane) {
        for (const PanePlacement& placement : placements) {
            if (placement.pane == pane) {
                return &placement;
            }
        }
        return nullptr;
    }

    // A tab of four panes in two columns, so that every side of every
    // pane has a neighbour and no two neighbours share a split:
    //
    //   +--------+--------+
    //   | first  | second |
    //   +--------+--------+
    //   | fourth | third  |
    //   +--------+--------+
    PaneTree quartered() {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        tree.split(SplitDirection::Horizontal, third);
        tree.focus(first);
        tree.split(SplitDirection::Horizontal, fourth);
        return tree;
    }
}

STD_TEST_SUITE(PaneLayout) {
    STD_TEST(PlantsOnePaneAndFocusesIt) {
        PaneTree tree;
        tree.plant(first);

        STD_INSIST(tree.count() == 1);
        STD_INSIST(!tree.empty());
        STD_INSIST(tree.focused() == first);
        STD_INSIST(tree.holds(first));
        STD_INSIST(!tree.holds(second));
    }

    STD_TEST(SplittingKeepsBothPanesAndFocusesTheNewOne) {
        PaneTree tree;
        tree.plant(first);
        STD_INSIST(tree.split(SplitDirection::Vertical, second));

        STD_INSIST(tree.count() == 2);
        // The pane that was divided is still there. An implementation
        // that replaced it instead of dividing it would keep count() at
        // one and lose this.
        STD_INSIST(tree.holds(first));
        STD_INSIST(tree.holds(second));
        // The new pane takes the focus, which is what makes a second
        // cmd+d divide the half just made rather than the original.
        STD_INSIST(tree.focused() == second);

        const Vector<u64> order = panesOf(tree);
        STD_INSIST(order.length() == 2);
        STD_INSIST(order[0] == first);
        STD_INSIST(order[1] == second);
    }

    STD_TEST(SplittingAnEmptyTreeDoesNothing) {
        PaneTree tree;

        STD_INSIST(!tree.split(SplitDirection::Vertical, first));
        STD_INSIST(tree.count() == 0);
        STD_INSIST(tree.focused() == 0);
        STD_INSIST(panesOf(tree).empty());
    }

    STD_TEST(ClosingAPaneGivesItsSpaceToTheSibling) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        STD_INSIST(tree.close(second));

        STD_INSIST(tree.count() == 1);
        STD_INSIST(tree.focused() == first);
        // The split node collapsed with the pane: what is left is a whole
        // window's worth, not half of one.
        const Vector<PanePlacement> placed = layoutOf(tree, {.x = 0, .y = 0, .width = 800, .height = 600});
        STD_INSIST(placed.length() == 1);
        STD_INSIST(placed[0].area.width == 800);
        STD_INSIST(placed[0].area.height == 600);
    }

    STD_TEST(ClosingTheLastPaneEmptiesTheTree) {
        PaneTree tree;
        tree.plant(first);

        // False is how the tab learns it has to go.
        STD_INSIST(!tree.close(first));
        STD_INSIST(tree.count() == 0);
        STD_INSIST(tree.empty());
        STD_INSIST(tree.focused() == 0);
        STD_INSIST(!tree.holds(first));
        // And it can be planted again: nothing about the closed node
        // survived to answer for a pane that is gone.
        tree.plant(second);
        STD_INSIST(tree.focused() == second);
        STD_INSIST(!tree.holds(first));
    }

    STD_TEST(ClosingAPaneThatIsNotThereChangesNothing) {
        PaneTree tree;
        tree.plant(first);

        STD_INSIST(tree.close(second));
        STD_INSIST(tree.count() == 1);
        STD_INSIST(tree.focused() == first);
    }

    STD_TEST(ClosingTheFocusedPaneMovesTheFocusIntoTheSurvivingSubtree) {
        PaneTree tree = quartered();
        // `fourth` has the focus; its sibling is the leaf `first`.
        STD_INSIST(tree.focused() == fourth);
        STD_INSIST(tree.close(fourth));

        STD_INSIST(tree.focused() == first);
        STD_INSIST(tree.count() == 3);
    }

    STD_TEST(ClosingAPaneWhoseSiblingIsASplitKeepsTheWholeSubtree) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        tree.split(SplitDirection::Horizontal, third);
        // `first` is now a leaf whose sibling is the split holding
        // `second` above `third`. Collapsing has to adopt that whole
        // subtree, children, direction and share alike - not just its
        // first leaf.
        STD_INSIST(tree.close(first));

        STD_INSIST(tree.count() == 2);
        STD_INSIST(tree.holds(second));
        STD_INSIST(tree.holds(third));

        const Vector<u64> order = panesOf(tree);
        STD_INSIST(order.length() == 2);
        STD_INSIST(order[0] == second);
        STD_INSIST(order[1] == third);

        // And they are still stacked, across the full width: an adoption
        // that dropped the far child would leave one pane, and one that
        // kept the outgoing split's own direction would put them side by
        // side.
        const Vector<PanePlacement> placed = layoutOf(tree, {.x = 0, .y = 0, .width = 800, .height = 600});
        STD_INSIST(placed.length() == 2);
        STD_INSIST(placementOf(placed, second)->area.width == 800);
        STD_INSIST(placementOf(placed, third)->area.width == 800);
        STD_INSIST(placementOf(placed, second)->area.height == 300);
        STD_INSIST(placementOf(placed, third)->area.y == 300);
    }

    STD_TEST(ClosingLeavesTheSurvivingSubtreeReachableFromItsNewParent) {
        PaneTree tree = quartered();
        // Empties the left column, so that `first`'s sibling becomes the
        // split holding the right column rather than a leaf.
        STD_INSIST(tree.close(fourth));
        STD_INSIST(tree.close(first));

        STD_INSIST(tree.count() == 2);
        const Vector<u64> order = panesOf(tree);
        STD_INSIST(order.length() == 2);
        STD_INSIST(order[0] == second);
        STD_INSIST(order[1] == third);
        // Neighbour walks go through parent links, so a subtree adopted
        // without re-parenting its children answers here.
        tree.focus(third);
        STD_INSIST(tree.focusNeighbour(PaneSide::Up));
        STD_INSIST(tree.focused() == second);
    }

    STD_TEST(ClosingEveryPaneOneByOneEndsEmpty) {
        PaneTree tree = quartered();

        STD_INSIST(tree.close(second));
        STD_INSIST(tree.close(third));
        STD_INSIST(tree.close(fourth));
        STD_INSIST(!tree.close(first));
        STD_INSIST(tree.empty());
    }

    STD_TEST(RepeatedSplittingAndClosingReusesNodes) {
        PaneTree tree;
        tree.plant(first);
        for (unsigned round = 0; round < 64; ++round) {
            STD_INSIST(tree.split(SplitDirection::Vertical, second));
            STD_INSIST(tree.close(second));
        }

        STD_INSIST(tree.count() == 1);
        // Three nodes ever exist here - the split and its two children -
        // so a free list that did not hand slots back would leave 129.
        STD_INSIST(tree.nodes.length() == 3);
    }

    STD_TEST(SharesHalveWithEachDivisionOfTheSamePane) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        tree.split(SplitDirection::Vertical, third);
        tree.split(SplitDirection::Vertical, fourth);

        // Each cmd+d halves the pane that has the focus, so the four
        // panes end up 1/2, 1/4, 1/8, 1/8 of the width.
        const Vector<PanePlacement> placed = layoutOf(tree, {.x = 0, .y = 0, .width = 800, .height = 100});
        STD_INSIST(placed.length() == 4);
        STD_INSIST(placementOf(placed, first)->area.width == 400);
        STD_INSIST(placementOf(placed, second)->area.width == 200);
        STD_INSIST(placementOf(placed, third)->area.width == 100);
        STD_INSIST(placementOf(placed, fourth)->area.width == 100);
        // An implementation that split the *tree* in half rather than the
        // focused pane would give every pane 200.
        STD_INSIST(placementOf(placed, first)->area.width != 200);
    }

    STD_TEST(TilesTheAreaWithoutGapOrOverlap) {
        PaneTree tree = quartered();
        const PixelRect area{.x = 7, .y = 13, .width = 800, .height = 600};
        const Vector<PanePlacement> placed = layoutOf(tree, area);

        STD_INSIST(placed.length() == 4);
        u32 covered = 0;
        for (const PanePlacement& placement : placed) {
            covered += (u32)(placement.area.width) * placement.area.height;
            // Nothing sticks out of the area it was given.
            STD_INSIST(placement.area.x >= area.x);
            STD_INSIST(placement.area.y >= area.y);
            STD_INSIST((u32)(placement.area.x) + placement.area.width <= (u32)(area.x) + area.width);
            STD_INSIST((u32)(placement.area.y) + placement.area.height <= (u32)(area.y) + area.height);
        }
        STD_INSIST(covered == (u32)(area.width) * area.height);
    }

    STD_TEST(TheFarPaneTakesTheRemainderOfTheDivision) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        // An odd width cannot halve evenly. The odd pixel has to land
        // somewhere, and it lands on the far side.
        const Vector<PanePlacement> placed = layoutOf(tree, {.x = 0, .y = 0, .width = 101, .height = 50});

        STD_INSIST(placementOf(placed, first)->area.width == 50);
        STD_INSIST(placementOf(placed, second)->area.width == 51);
        STD_INSIST(placementOf(placed, second)->area.x == 50);
        // Rounding the near pane up instead would give 51 and 50, and
        // rounding both would give 51 and 51 - two pixels more than there
        // are.
        STD_INSIST(placementOf(placed, first)->area.width + placementOf(placed, second)->area.width == 101);
    }

    STD_TEST(SplittingHorizontallyDividesTheHeight) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Horizontal, second);
        const Vector<PanePlacement> placed = layoutOf(tree, {.x = 0, .y = 0, .width = 800, .height = 601});

        STD_INSIST(placementOf(placed, first)->area.height == 300);
        STD_INSIST(placementOf(placed, second)->area.height == 301);
        STD_INSIST(placementOf(placed, second)->area.y == 300);
        // The width is untouched: a direction that divided the wrong axis
        // would halve this instead.
        STD_INSIST(placementOf(placed, first)->area.width == 800);
        STD_INSIST(placementOf(placed, second)->area.width == 800);
    }

    STD_TEST(TheDividerComesOutOfTheAreaAndNotOutOfOnePane) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);

        Vector<PanePlacement> placed;
        Vector<PaneDivider> dividers;
        tree.layout({.x = 0, .y = 0, .width = 800, .height = 600}, 10, placed, &dividers);

        STD_INSIST(placementOf(placed, first)->area.width == 395);
        STD_INSIST(placementOf(placed, second)->area.width == 395);
        STD_INSIST(placementOf(placed, second)->area.x == 405);
        STD_INSIST(dividers.length() == 1);
        STD_INSIST(dividers[0].area.x == 395);
        STD_INSIST(dividers[0].area.width == 10);
        STD_INSIST(dividers[0].area.height == 600);
        STD_INSIST(dividers[0].direction == SplitDirection::Vertical);
        // Taking the gap out of one pane rather than out of the area
        // would leave 400 and 390.
        STD_INSIST(placementOf(placed, first)->area.width == placementOf(placed, second)->area.width);
    }

    // T10: the seams are reported whatever the gap is. This test used to
    // be NoDividersAreReportedWhenTheGapIsZero and asserted the opposite,
    // on the reading that a divider is a bar to be drawn. A10's default is
    // that the air between two panes is made by their own borders and the
    // gap stays zero, and every product call to layout() passes zero, so
    // under the old rule the only layout the window ever asks for would
    // report nothing to grab - and dragging a divider, which is the whole
    // point of reporting them, would be impossible.
    // A seam of zero width is still a line the pointer can be near.
    STD_TEST(TheSeamsAreReportedEvenWhenTheGapIsZero) {
        PaneTree tree = quartered();

        Vector<PanePlacement> placed;
        Vector<PaneDivider> dividers;
        tree.layout({.x = 0, .y = 0, .width = 800, .height = 600}, 0, placed, &dividers);

        STD_INSIST(placed.length() == 4);
        // Three splits hold four panes, however they nest.
        STD_INSIST(dividers.length() == 3);
        for (const PaneDivider& divider : dividers) {
            const bool vertical = divider.direction == SplitDirection::Vertical;
            STD_INSIST((vertical ? divider.area.width : divider.area.height) == 0);
            // The seam lies inside the box it divides, and spans it
            // completely across the axis being cut.
            STD_INSIST(vertical ? divider.area.height == divider.box.height : divider.area.width == divider.box.width);
            STD_INSIST(divider.area.x >= divider.box.x && divider.area.y >= divider.box.y);
        }
    }

    // R8-qa. The other half of the rule T10 rewrote: reporting a seam
    // whatever the gap is must not turn into reporting one where there is
    // no split. An unsplit tab is what a window holds until the user asks
    // for a second pane, and dividerAt() widens every reported seam into
    // a grab strip a cell wide - so a seam reported here would silently
    // take a column of clicks away from the text of a window that has no
    // divider at all.
    STD_TEST(AnUnsplitTreeReportsNoSeamAtAll) {
        PaneTree tree;
        tree.plant(first);

        Vector<PanePlacement> placed;
        Vector<PaneDivider> dividers;
        tree.layout({.x = 0, .y = 0, .width = 800, .height = 600}, 0, placed, &dividers);

        STD_INSIST(placed.length() == 1);
        STD_INSIST(dividers.empty());
    }

    // The box is the split's own rectangle and not the whole window's:
    // dragging the inner divider of a nested split has to count its share
    // out of the half it divides. A9's quartered tree gives a top-level
    // split over the whole 800 and two inner ones over 400 apiece.
    STD_TEST(TheDividerCarriesTheRectangleItsShareIsCountedIn) {
        PaneTree tree = quartered();

        Vector<PanePlacement> placed;
        Vector<PaneDivider> dividers;
        tree.layout({.x = 0, .y = 0, .width = 800, .height = 600}, 0, placed, &dividers);

        size_t whole = 0;
        size_t halves = 0;
        for (const PaneDivider& divider : dividers) {
            whole += divider.box.width == 800 && divider.box.height == 600 ? 1 : 0;
            halves += divider.box.width == 400 && divider.box.height == 600 ? 1 : 0;
        }
        STD_INSIST(whole == 1);
        STD_INSIST(halves == 2);
    }

    STD_TEST(MovingADividerMovesThePanesItSeparates) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);

        Vector<PanePlacement> placed;
        Vector<PaneDivider> dividers;
        // The handle comes back with the divider the pointer would grab.
        tree.layout({.x = 0, .y = 0, .width = 800, .height = 600}, 4, placed, &dividers);
        STD_INSIST(dividers.length() == 1);

        tree.setShare(dividers[0].split, PaneTree::shareScale / 4);
        STD_INSIST(tree.share(dividers[0].split) == PaneTree::shareScale / 4);

        const Vector<PanePlacement> moved = layoutOf(tree, {.x = 0, .y = 0, .width = 800, .height = 600});
        STD_INSIST(placementOf(moved, first)->area.width == 200);
        STD_INSIST(placementOf(moved, second)->area.width == 600);
    }

    STD_TEST(ADividerIsNeverPushedOntoAPane) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);

        // Both ends of the range leave the other side something.
        tree.setShare(0, 0);
        STD_INSIST(tree.share(0) == 1);
        tree.setShare(0, PaneTree::shareScale * 4);
        STD_INSIST(tree.share(0) == PaneTree::shareScale - 1);
    }

    STD_TEST(ADividerWiderThanTheAreaLeavesNoNegativePane) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        const Vector<PanePlacement> placed = layoutOf(tree, {.x = 0, .y = 0, .width = 4, .height = 600}, 40);

        STD_INSIST(placed.length() == 2);
        STD_INSIST(placed[0].area.width == 0);
        STD_INSIST(placed[1].area.width == 0);
        // Unsigned arithmetic that let the gap run past the extent would
        // hand back widths near 65535 here rather than zero.
        STD_INSIST(placed[1].area.x <= 4);
    }

    STD_TEST(TheNeighbourAcrossASplitTakesTheFocus) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        STD_INSIST(tree.focused() == second);

        STD_INSIST(tree.focusNeighbour(PaneSide::Left));
        STD_INSIST(tree.focused() == first);
        STD_INSIST(tree.focusNeighbour(PaneSide::Right));
        STD_INSIST(tree.focused() == second);
        // There is nothing above or below a vertical split, and nothing
        // to the right of the rightmost pane.
        STD_INSIST(!tree.focusNeighbour(PaneSide::Right));
        STD_INSIST(!tree.focusNeighbour(PaneSide::Up));
        STD_INSIST(!tree.focusNeighbour(PaneSide::Down));
        STD_INSIST(tree.focused() == second);
    }

    STD_TEST(TheNeighbourIsFoundAcrossMoreThanOneSplit) {
        PaneTree tree = quartered();
        // `fourth` is bottom left. Its right-hand neighbour lives under a
        // split two levels up, and the leaf entered there is the top one.
        tree.focus(fourth);
        STD_INSIST(tree.focusNeighbour(PaneSide::Right));
        STD_INSIST(tree.focused() == second);

        tree.focus(third);
        STD_INSIST(tree.focusNeighbour(PaneSide::Up));
        STD_INSIST(tree.focused() == second);
        STD_INSIST(tree.focusNeighbour(PaneSide::Left));
        STD_INSIST(tree.focused() == first);
        STD_INSIST(tree.focusNeighbour(PaneSide::Down));
        STD_INSIST(tree.focused() == fourth);
    }

    STD_TEST(TheNeighbourEnteredIsTheOneAgainstTheDivider) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);
        tree.split(SplitDirection::Vertical, third);
        // first | second | third, with `second` and `third` under one
        // split of their own.
        tree.focus(first);
        STD_INSIST(tree.focusNeighbour(PaneSide::Right));
        // The pane against the divider crossed, not the far end of the
        // subtree behind it.
        STD_INSIST(tree.focused() == second);

        tree.focus(third);
        STD_INSIST(tree.focusNeighbour(PaneSide::Left));
        STD_INSIST(tree.focused() == second);
        STD_INSIST(tree.focusNeighbour(PaneSide::Left));
        STD_INSIST(tree.focused() == first);
    }

    STD_TEST(FocusingAPaneThatIsNotThereLeavesTheFocusAlone) {
        PaneTree tree;
        tree.plant(first);
        tree.split(SplitDirection::Vertical, second);

        tree.focus(third);
        STD_INSIST(tree.focused() == second);
        tree.focus(first);
        STD_INSIST(tree.focused() == first);
    }

    STD_TEST(AnEmptyTreeLaysOutNothing) {
        PaneTree tree;

        STD_INSIST(layoutOf(tree, {.x = 0, .y = 0, .width = 800, .height = 600}).empty());
        STD_INSIST(!tree.focusNeighbour(PaneSide::Left));
    }
}
