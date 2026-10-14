/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#if defined(__APPLE__)

    #include <std/str/view.h>
    #include <std/sys/types.h>

// Parses a quickHotkey chord ("mod+mod+...+key", e.g. "ctrl+grave") into
// a Carbon modifier mask and virtual keycode - just the string ->
// (modifiers, keyCode) grammar, no Carbon calls. Split out of
// ui_quick_hotkey.mm so this pure logic is reachable from a plain unit
// test without pulling in Objective-C or a live NSApplication. u32
// stands in for Carbon's UInt32 (itself an unsigned int) so this header
// does not need <Carbon/Carbon.h> - only quick_hotkey_chord.cpp does,
// for kVK_* and the modifier bit constants.
//
// Returns false - modifiers and keyCode left unwritten - when the string
// does not parse as a chord at all: an unknown modifier keyword, a
// trailing or empty key token, or a key name outside the table this file
// knows. A chord that parses but names no modifier at all (e.g. "space")
// is NOT rejected here: ui_quick_hotkey.mm's caller treats modifiers ==
// 0 as a separate, more specific failure - a bare key would grab that
// key system-wide, not just be an unusual chord - with its own
// diagnostic, so that case is deliberately left visible in the output
// rather than folded into this function's single pass/fail result.
bool parseQuickHotkey(stl::StringView text, u32& modifiers, u32& keyCode);

#endif
