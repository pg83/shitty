/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "vt_geometry.h"

#include <std/str/view.h>
#include <std/sys/types.h>

#include <plt/window.h>

#include <stddef.h>

struct Vterm;
struct VtermTitleChanged;

// Every way the terminal reaches past pure byte-stream semantics: the
// clipboards OSC 52 and the selection write, the window operations
// XTWINOPS drives, the frame request that publishes damage, the desktop
// actions of hyperlinks, the title publication, and the resize echo of
// an in-band grid change. The embedder implements it: the GUI adapter
// forwards to the platform window and fans events into its listener
// lists, the headless host emulates against its headless window.
struct VtHost {
    virtual plt::Clipboard* primary() = 0;
    virtual plt::Clipboard* secondary() = 0;
    virtual plt::WindowInfo info() = 0;
    virtual void requestFrame() = 0;
    virtual void requestResize(u32 width, u32 height) = 0;
    virtual void requestMaximized(bool maximized) = 0;
    virtual void requestFullscreen(bool fullscreen) = 0;
    virtual void requestIconify() = 0;
    virtual void requestRestore() = 0;
    virtual void requestMove(i32 x, i32 y) = 0;
    virtual void requestFocus() = 0;
    virtual void requestAttention() = 0;
    virtual void requestPointerIcon(plt::PointerIcon icon) = 0;
    virtual void requestOpenUri(stl::StringView uri) = 0;
    // Whether a detected plain-text URI with this scheme is actionable.
    // The scheme list is the embedder's policy; an explicit OSC 8 link
    // is authoritative and never asks.
    virtual bool uriSchemeAllowed(stl::StringView scheme) = 0;
    // A terminal published its undecorated title; the embedder decides
    // whether the source is visible and how a window presents it.
    virtual void titleChanged(const VtermTitleChanged& event) = 0;
    // The grid geometry moved under an in-band resize the core applied
    // itself; every terminal behind the window must hear it.
    virtual void resized() = 0;
    // A1/A10: what the window reserves on each side before any pane may
    // draw - the user's border plus whatever chrome (a sidebar, a
    // titlebar strip) took - in physical pixels. The core divides the
    // *window's* pixels by these when an application asks how much grid
    // fits in them (CSI 18t / 19t, CSI 8t); they are never a pane's,
    // whose own border arrives in VtGeometry::insets and carries no
    // chrome at all. Substituting one for the other compiles and
    // answers plausibly, which is why they come from different places.
    //
    // Asked rather than held: the reserve moves under cmd+b, a font
    // change and a display of another scale, and a core that kept a
    // copy would be a second place that knows how much is taken on the
    // left. Returned by value for the same reason - the embedder
    // composes it out of two other numbers and has nowhere to keep it.
    virtual VtInsets contentInsets() = 0;
    // The core applied an in-band resize (CSI 4t / CSI 8t / CSI 9t) and
    // has already asked the window for it through requestResize(). This
    // commits the same size on the embedder's own surface without
    // waiting for the platform to call back, which is what makes the
    // grid and the child's resize a consequence of the escape sequence
    // rather than of the next frame. Upstream's core writes its own
    // VtGeometry here; ours cannot, because counting the window's grid
    // needs contentInsets() and A1 leaves the points-to-pixels
    // conversion with the embedder.
    //
    // Not a duplicate of requestResize(): that one asks the platform,
    // and in the two embedders with no frame loop - the headless
    // adapter and the C facade - nothing else would ever reach the pane
    // or the child. Where a frame does come back it carries the size
    // the window manager actually granted, which is not the size that
    // was asked for whenever it refuses.
    virtual void surfaceResized(u32 width, u32 height) = 0;
    // A11: how many cells every live pane behind this window holds,
    // except one. The core sizes the shared cell-extra store by the sum
    // over the panes, and only the embedder has the list; the exception
    // is the caller, which adds its own count itself because it may not
    // be in the list yet at the moment it asks. An embedder with no
    // list at all - the headless adapter, the C facade - answers zero,
    // and then the caller is the only pane there is.
    virtual size_t cellCapacityExcept(const Vterm* except) = 0;
};
