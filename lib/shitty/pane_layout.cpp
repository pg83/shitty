/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pane_layout.h"

#include <std/alg/minmax.h>
#include <std/dbg/assert.h>

using namespace stl;

namespace {
    // Which axis a side travels along, and which way.
    struct SideAxis {
        SplitDirection direction = SplitDirection::Vertical;
        bool towardsFar = false;
    };

    SideAxis axisOf(PaneSide side) {
        switch (side) {
            case PaneSide::Left:
                return {SplitDirection::Vertical, false};
            case PaneSide::Right:
                return {SplitDirection::Vertical, true};
            case PaneSide::Up:
                return {SplitDirection::Horizontal, false};
            default:
                return {SplitDirection::Horizontal, true};
        }
    }
}

PixelRect contentBox(const Composer& composer) {
    // Saturating: a reserve wider than the window is a reserve claimed
    // before the first resize (render.h, surfacePane()), and an unsigned
    // difference would wrap it into an enormous box rather than an empty
    // one.
    const Insets chrome = composer.chromeInsets();
    const u32 horizontal = (u32)(chrome.left) + chrome.right;
    const u32 vertical = (u32)(chrome.top) + chrome.bottom;
    return {
        .x = 0,
        .y = 0,
        .width = (u16)(composer.geometry.pixelWidth > horizontal ? composer.geometry.pixelWidth - horizontal : 0),
        .height = (u16)(composer.geometry.pixelHeight > vertical ? composer.geometry.pixelHeight - vertical : 0),
    };
}

VtGeometry paneGeometry(const Composer& composer, const PixelRect& area) {
    // A10, the two transformations, one after the other and each exactly
    // once. The chrome moved the pane: the rectangle was cut out of the
    // content box, so putting the reserve back on turns it into a
    // position on the surface. The border shrinks the pane: it is the
    // air this pane keeps inside its own rectangle, and it is what the
    // backend adds back when it places the grid there (render.h,
    // surfacePane()).
    //
    // Deliberately not gridColumns()/gridRows(): those take the
    // *window's* insets out, and the chrome's share is already out of
    // the box these rectangles divide - asking them here would spend it
    // twice.
    //
    // With one pane the area is the whole content box, so chrome and
    // border come off in exactly the two steps Composer::resize() does
    // in one, and a window of one pane keeps the grid it always had.
    //
    // Saturating for the same reason contentBox() is: a border wider
    // than the pane must leave an empty grid rather than wrap into a
    // huge one.
    const Insets chrome = composer.chromeInsets();
    const Insets border = composer.paneInsets();
    const u32 horizontal = (u32)(border.left) + border.right;
    const u32 vertical = (u32)(border.top) + border.bottom;
    const u32 width = area.width > horizontal ? area.width - horizontal : 0;
    const u32 height = area.height > vertical ? area.height - vertical : 0;
    return {
        .columns = (u16)(max<u32>(1, width / max<u32>(1, composer.geometry.cellPixelWidth))),
        .rows = (u16)(max<u32>(1, height / max<u32>(1, composer.geometry.cellPixelHeight))),
        .insets = vtInsets(border),
        .originX = (i32)(chrome.left) + area.x,
        .originY = (i32)(chrome.top) + area.y,
        // T10: the same box before the division, which is what the
        // pointer clamps want - the grid rounds a partial cell away and
        // they do not.
        .width = (i32)(width),
        .height = (i32)(height),
    };
}

// A8: one terminal, the whole window - the only place outside Composer
// itself that is allowed to read the window's grid and call the answer a
// pane.
VtGeometry windowPane(const Composer& composer) {
    // Not a formula of its own: the pane that fills the window is the
    // pane occupying the whole content box, and saying it that way is
    // what keeps it from drifting from the panes that do not. It counts
    // out the same grid Composer::resize() did, because it divides the
    // same content box by the same glyph.
    return paneGeometry(composer, contentBox(composer));
}

bool PaneTree::isLeaf(u32 node) const {
    return nodes[node].near == noNode;
}

u32 PaneTree::allocate() {
    if (free_ != noNode) {
        const u32 node = free_;
        free_ = nodes[node].parent;
        nodes.mut(node) = Node{};
        return node;
    }
    nodes.pushBack(Node{});
    return (u32)(nodes.length() - 1);
}

void PaneTree::release(u32 node) {
    // The pane id is cleared along with the links: find() walks the whole
    // vector, free slots included, and a stale id there would answer for
    // a pane that is gone.
    nodes.mut(node) = Node{};
    nodes.mut(node).parent = free_;
    free_ = node;
}

u32 PaneTree::find(u64 pane) const {
    if (pane == 0) {
        return noNode;
    }
    for (size_t at = 0; at < nodes.length(); ++at) {
        if (nodes[at].pane == pane) {
            return (u32)(at);
        }
    }
    return noNode;
}

u32 PaneTree::firstLeaf(u32 node) const {
    while (node != noNode && !isLeaf(node)) {
        node = nodes[node].near;
    }
    return node;
}

void PaneTree::plant(u64 pane) {
    STD_ASSERT(root == noNode && pane != 0);
    const u32 node = allocate();
    nodes.mut(node).pane = pane;
    root = node;
    focused_ = node;
    count_ = 1;
}

bool PaneTree::split(SplitDirection direction, u64 pane) {
    STD_ASSERT(pane != 0);
    if (focused_ == noNode) {
        return false;
    }
    // The focused leaf becomes the split itself and its pane moves into a
    // fresh near child. Turning the node inside out this way leaves the
    // parent link - and with it every index anyone else is holding -
    // alone; hanging a new split under the parent would first have to
    // work out which child the leaf was.
    const u32 divided = focused_;
    const u32 near = allocate();
    const u32 far = allocate();
    nodes.mut(near).pane = nodes[divided].pane;
    nodes.mut(near).parent = divided;
    nodes.mut(far).pane = pane;
    nodes.mut(far).parent = divided;
    Node& split_ = nodes.mut(divided);
    split_.pane = 0;
    split_.near = near;
    split_.far = far;
    split_.share = shareScale / 2;
    split_.direction = direction;
    focused_ = far;
    ++count_;
    return true;
}

bool PaneTree::close(u64 pane) {
    const u32 node = find(pane);
    if (node == noNode) {
        return count_ != 0;
    }
    if (node == root) {
        release(node);
        root = noNode;
        focused_ = noNode;
        count_ = 0;
        return false;
    }
    const u32 parent = nodes[node].parent;
    const u32 sibling = nodes[parent].near == node ? nodes[parent].far : nodes[parent].near;
    // Whether the focus sits inside the subtree that is going. Asked
    // before anything moves: afterwards the links that would answer it
    // are gone.
    bool focusLost = false;
    for (u32 at = focused_; at != noNode; at = nodes[at].parent) {
        if (at == node) {
            focusLost = true;
            break;
        }
    }
    // The sibling subtree takes the split's place by moving into the
    // split's own node, for the same reason split() turns a leaf inside
    // out: the grandparent's link keeps pointing at a live node.
    const Node adopted = nodes[sibling];
    Node& into = nodes.mut(parent);
    into.pane = adopted.pane;
    into.near = adopted.near;
    into.far = adopted.far;
    into.share = adopted.share;
    into.direction = adopted.direction;
    if (adopted.near != noNode) {
        nodes.mut(adopted.near).parent = parent;
        nodes.mut(adopted.far).parent = parent;
    }
    if (focused_ == sibling) {
        focused_ = parent;
    }
    release(node);
    release(sibling);
    if (focusLost) {
        focused_ = firstLeaf(parent);
    }
    --count_;
    return true;
}

u64 PaneTree::focused() const {
    return focused_ == noNode ? 0 : nodes[focused_].pane;
}

void PaneTree::focus(u64 pane) {
    const u32 node = find(pane);
    if (node == noNode) {
        return;
    }
    focused_ = node;
}

u32 PaneTree::descend(u32 node, SplitDirection direction, bool towardsFar) const {
    while (!isLeaf(node)) {
        // Along the axis being crossed, take the side the neighbour is
        // entered from; across it, the near child, which is the top or
        // the left one.
        const bool far = nodes[node].direction == direction && towardsFar;
        node = far ? nodes[node].far : nodes[node].near;
    }
    return node;
}

bool PaneTree::focusNeighbour(PaneSide side) {
    if (focused_ == noNode) {
        return false;
    }
    const SideAxis axis = axisOf(side);
    for (u32 at = focused_; nodes[at].parent != noNode; at = nodes[at].parent) {
        const u32 parent = nodes[at].parent;
        if (nodes[parent].direction != axis.direction) {
            continue;
        }
        const bool fromNear = nodes[parent].near == at;
        if (fromNear != axis.towardsFar) {
            continue;
        }
        const u32 target = axis.towardsFar ? nodes[parent].far : nodes[parent].near;
        focused_ = descend(target, axis.direction, !axis.towardsFar);
        return true;
    }
    return false;
}

size_t PaneTree::count() const {
    return count_;
}

bool PaneTree::empty() const {
    return count_ == 0;
}

bool PaneTree::holds(u64 pane) const {
    return find(pane) != noNode;
}

void PaneTree::collect(u32 node, Vector<u64>& out) const {
    if (isLeaf(node)) {
        out.pushBack(nodes[node].pane);
        return;
    }
    collect(nodes[node].near, out);
    collect(nodes[node].far, out);
}

void PaneTree::panes(Vector<u64>& out) const {
    if (root == noNode) {
        return;
    }
    collect(root, out);
}

void PaneTree::place(u32 node, const PixelRect& area, u16 divider, Vector<PanePlacement>& panes_, Vector<PaneDivider>* dividers) const {
    if (isLeaf(node)) {
        panes_.pushBack({nodes[node].pane, area});
        return;
    }
    const bool vertical = nodes[node].direction == SplitDirection::Vertical;
    const u32 extent = vertical ? area.width : area.height;
    const u32 gap = min<u32>(divider, extent);
    const u32 usable = extent - gap;
    const u64 scaled = (u64)(usable) * nodes[node].share;
    const u32 nearExtent = (u32)(scaled / shareScale);
    // The remainder of the division lands here, which is what keeps the
    // two panes and the gap adding back up to the area they came from.
    const u32 farExtent = usable - nearExtent;
    PixelRect nearArea = area;
    PixelRect farArea = area;
    if (vertical) {
        nearArea.width = (u16)(nearExtent);
        farArea.x = (u16)(area.x + nearExtent + gap);
        farArea.width = (u16)(farExtent);
    } else {
        nearArea.height = (u16)(nearExtent);
        farArea.y = (u16)(area.y + nearExtent + gap);
        farArea.height = (u16)(farExtent);
    }
    if (dividers != nullptr) {
        PixelRect bar = area;
        if (vertical) {
            bar.x = (u16)(area.x + nearExtent);
            bar.width = (u16)(gap);
        } else {
            bar.y = (u16)(area.y + nearExtent);
            bar.height = (u16)(gap);
        }
        dividers->pushBack({node, nodes[node].direction, bar, area});
    }
    place(nodes[node].near, nearArea, divider, panes_, dividers);
    place(nodes[node].far, farArea, divider, panes_, dividers);
}

void PaneTree::layout(const PixelRect& area, u16 divider, Vector<PanePlacement>& panes_, Vector<PaneDivider>* dividers) const {
    if (root == noNode) {
        return;
    }
    place(root, area, divider, panes_, dividers);
}

void PaneTree::setShare(u32 split, u32 share_) {
    STD_ASSERT(split < nodes.length() && !isLeaf(split));
    nodes.mut(split).share = min<u32>(max<u32>(share_, 1), shareScale - 1);
}

u32 PaneTree::share(u32 split) const {
    STD_ASSERT(split < nodes.length() && !isLeaf(split));
    return nodes[split].share;
}
