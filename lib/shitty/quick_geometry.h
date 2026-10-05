/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <plt/window.h>

#include <std/str/view.h>

// Parses the quickGeometry option's "<W>x<H>+<X>+<Y>" grammar into a
// plt::QuickGeometry - pure parsing and range validation, no platform
// dependency, so it is reachable from a plain unit test the way
// quick_hotkey_chord.h's parseQuickHotkey is. options.cpp's
// OptionsParser::getQuickGeometry is the only production caller; it
// turns a false return into the startup diagnostic.
//
// Each of W, H, X, Y is either a bare unsigned integer (pixels) or the
// same followed by '%' (a percentage of the target screen's visibleFrame
// extent along that axis, resolved only once a real screen is known -
// see WindowImpl::topOfActiveScreenFrame in platform_cocoa.mm). W and H
// additionally reject zero: a window with no width or height would be
// invisible, which is never a useful outcome for an option whose whole
// point is to be seen. X and Y accept zero - it is the default offset.
// A percent past 100, a pixel count past UINT16_MAX (no real screen
// approaches it, and it keeps the bound consistent with -geometry's
// COLSxROWS), a missing 'x' or '+' separator, an empty component, or any
// stray byte all fail the same way: false, out left untouched.
//
// There is no negative sign in the grammar at all, not even for X/Y: a
// leading '-' is simply not a digit, so it fails to parse like any other
// garbage rather than being accepted and rejected by a separate range
// check.
bool parseQuickGeometry(stl::StringView text, plt::QuickGeometry& out);
