/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class StringBuilder;
}

// A6: the quick-terminal window's manually set position and size, as last
// observed by the caller (T3), entirely in points - x/y the window
// frame's origin, width/height the content view's size.
//
// Points rather than plt::WindowInfo's own mix of points and backing
// pixels on purpose: a backing pixel means a different amount of screen
// on every display, so a frame saved on a 2x laptop panel came back
// doubled on a 1x external monitor, was persisted that way on the next
// hide, and halved again on the way back - the user's placement
// destroyed one show at a time (R2-qa round 2, B4). A point means the
// same thing everywhere, and the arithmetic disappears with the units.
//
// This module still does not resolve or clamp a frame against a live
// screen; quickFrameTarget() below is where that happens, and it takes
// the screen from its caller. A frame with an off-screen or stale
// position/size is not this module's problem to catch, and
// loadQuickFrame() accepts one exactly as stored.
struct QuickFrame {
    i32 x = 0;
    i32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

// A rectangle in points: a screen's usable area (Cocoa's
// NSScreen.visibleFrame - a secondary display's origin is nowhere near
// zero, which is the whole point of carrying it) or the window frame
// resolved against one.
struct QuickFrameRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// Resolves a saved frame into the window frame to apply - titlebar
// included - in points:
// `visible` is the usable area of the screen it is being restored on,
// `titlebarHeight` the chrome above the content (0 where there is none,
// e.g. the portable fallback which has no window chrome to ask about).
//
// The whole frame - titlebar included - is what gets clamped into
// `visible`, not the content alone: clamping the content and only then
// adding the titlebar let a window at the bottom edge stick its titlebar
// out above the visible area (R2-qa round 2, Z2).
//
// Pure and screen-agnostic on purpose. This is the one implementation of
// the clamp, shared by the Cocoa path (ui_quick_hotkey.mm, which
// resolves `visible` from the NSScreen the frame was saved on) and the
// portable fallback (application.cpp): the previous two independent
// implementations left the regression coverage standing on the one that
// does not run on macOS (R2-qa round 2, I8).
QuickFrameRect quickFrameTarget(const QuickFrame& frame, const QuickFrameRect& visible, double titlebarHeight);

// Area, in square points, of the part of `a` that lies inside `b`; zero
// when they do not overlap at all.
double quickFrameOverlap(const QuickFrameRect& a, const QuickFrameRect& b);

// True when every part of `frame` lies on one of `screens` - their whole
// frames, Cocoa's NSScreen.frame rather than visibleFrame, so a window
// reaching across the menu-bar strip of a neighbouring display still
// counts as being where the user put it.
//
// This is the question quickFrameTarget() must not be asked when the
// answer is yes: a frame spanning two displays has an origin on one of
// them, and clamping it into that one screen drags the whole window off
// the other - a placement made with a single mouse drag, replaced by a
// computed one and then persisted (R2-qa round 3, B7). Only a frame that
// is partly nowhere needs a screen picked for it at all.
//
// Screens are taken to be non-overlapping, which is what macOS lays out
// for every arrangement except mirroring; mirrored displays report the
// very same rect twice, and an exact duplicate is skipped rather than
// counted again.
bool quickFrameFitsScreens(const QuickFrameRect& frame, const QuickFrameRect* screens, size_t count);

// Whether hiding the quick window should persist `live`, the frame it
// currently has: `computed` says the last show did not restore the saved
// frame verbatim but resolved one against the attached screens
// (quickFrameFitsScreens said no), and `computedFrame` is what it put on
// screen.
//
// False for exactly one case - the window is still, to the point, the
// frame this code computed. That is an adaptation, not a placement, and
// writing it back replaces the user's real one permanently (R2-qa round
// 2, B4; round 3, B7).
//
// Everything else saves, and the frame comparison is what makes that
// true rather than the flag alone. A flag that only a later restore
// could clear stayed set through the show that had no saved frame to
// restore, so deleting the state file - the documented way to start over
// - left the option dead until the process was restarted (R2-qa round 3,
// B6). Once the window is not where this code put it, whoever moved it
// was the user.
bool quickFrameShouldSave(bool computed, const QuickFrameRect& computedFrame, const QuickFrameRect& live);

// Re-expresses one window extent in points-per-pixel terms: `extent` was
// read back from a window living at `restoredScale`, and the composer is
// still carrying `composerScale`. Result clamped to what Composer::resize
// takes, and a non-positive `restoredScale` treated as 1 rather than
// dividing by it.
//
// Restoring a saved frame can move the window to a display whose content
// scale differs from the one requestShowAt() just sized the grid for. The
// scale change that follows regrids the font and resizes the window to
// the grid of the screen it is no longer on, pulling it off the frame
// just restored - measured on a 1x monitor plus a 2x panel, a restored
// 1000x500 came back 980x490 (F2c's report). Re-deriving the grid from
// the window as it now is makes that resize reproduce the restored frame
// instead of replacing it.
//
// Here rather than inline in its one caller (applySavedQuickFrame,
// application.cpp) because that caller sits in a branch no headless test
// can reach, which left this arithmetic covered by nothing at all - a
// mutation setting the ratio to 1 killed no test (R2-test, I13).
u32 quickFrameRegridExtent(u32 extent, float restoredScale, float composerScale);

// Builds the default frame store path from the main config file path
// (Options::configPath): alongside it, same directory, with its
// extension (the last '.' in the file name, if any) replaced by
// "-quick-frame" - e.g. ~/.config/shitty/shitty.toml becomes
// ~/.config/shitty/shitty-quick-frame. Deriving it from configPath
// instead of re-walking XDG_CONFIG_HOME/HOME keeps this module free of
// any Brand dependency, and guarantees agreement with whichever config
// file this process actually resolved, -config override included.
//
// False - out untouched - when configPath is empty: OptionsParser
// leaves it that way only when no HOME and no XDG_CONFIG_HOME are set,
// and there is nowhere sane to put a state file either, same as it
// already means no persistence for the main config.
bool defaultQuickFramePath(stl::StringView configPath, stl::StringBuilder& out);

// Reads the saved frame from `path`. False - out untouched - when the
// file is missing, unreadable, or does not parse completely (any of the
// four fields absent or out of shape): a corrupt or absent state file is
// not an error, the caller treats it exactly like "no saved frame yet"
// and falls back to quickGeometry (A6).
//
// Spaces around the "=" and around either side of a line are ignored on
// both the key and the value - this is a file people edit by hand. Tabs
// are not: nothing writes them, and stl::StringView::stripSpace() is
// about the space character.
//
// The keys carry their unit ("x-points" and friends), which is also how
// a file written by the previous generation - "x"/"width", sizes in
// backing pixels - is retired: none of its keys are recognized, so it
// parses incompletely and is treated as absent, exactly as above. There
// is deliberately no migration; the units a pixel-sized frame was
// written in are not recoverable without the display it came from
// (R2-qa round 2, B4).
bool loadQuickFrame(stl::StringView path, QuickFrame& out);

// Writes `frame` to `path` atomically: a temporary file in the same
// directory (named after this process's pid, so two live processes
// racing on the same path - always true under quickCompanion - never
// collide with each other), then rename() over the target. A reader
// therefore never observes a half-written file, and a failure at any
// point before the rename leaves the previous frame (or nothing) at
// `path` exactly as it was.
//
// Returns false when the write could not be completed at all - the
// parent directory is missing, unwritable, or some other I/O error -
// and cleans up its own temporary file first. Persisting a frame is
// best-effort: the caller logs and moves on rather than failing startup
// over it.
bool saveQuickFrame(stl::StringView path, const QuickFrame& frame);
