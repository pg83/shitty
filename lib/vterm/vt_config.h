/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "color.h"
#include "ansi_palette.h"
#include "unicode_width.h"

#include <std/str/view.h>
#include <std/sys/types.h>

// The semantic configuration of the VT core: every knob the terminal
// state machine reads. The embedder owns and fills it - in the shitty
// binaries Options carries one and the parser writes it - and the core
// only reads. Strings live in the owner's pool, NUL terminated.
struct VtConfig {
    u8 modifyOtherKeys = 0;
    u16 saveLines = 0;
    // The width emulation resolved from -unicodeWidths; parsing probes
    // the system libc when the option asks to match it.
    UnicodeWidths widths{0};
    stl::StringView title;
    stl::StringView dump;
    // The product name the terminal reports (XTVERSION) and prefixes
    // its diagnostics with.
    stl::StringView brandName;
    Color bg{};
    Color cr{};
    Color fg{};
    AnsiPalette palette{};
    bool altScrollMode = false;
    // The command-line editor at a shell prompt (prompt_editor.h).
    bool promptEditor = false;
    bool altSendsEscape = false;
    bool autoCopyMode = false;
    bool allowOsc52Read = false;
    bool allowWindowOps = false;
    bool osc52SelectClipboard = false;
    bool boldColors = false;
    bool kittyCtrlBaseLayout = false;
    bool verbose = false;
};

// The mount point of a reloadable configuration. The embedder swaps the
// snapshot behind the pointer and delivers configChanged(); the core
// reads through the slot on every access, so a swap is one pointer
// store away from taking effect.
struct VtConfigSlot {
    const VtConfig* config = nullptr;
};
