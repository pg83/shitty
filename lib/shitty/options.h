/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */
/* part of this file is part of Zutty.
 * Copyright (C) 2020 Tom Szilagyi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * See the file LICENSE.GPL3 for the full license.
 */

#pragma once

#include <lib/vterm/vt_config.h>
#include <lib/vterm/ansi_palette.h>

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/vector.h>

namespace stl {
    class ObjPool;
}

struct Darts;
struct Brand;

enum class OptionSource {
    NONE,
    HardDefault,
    Config,
    CmdLine
};

enum class OptionsLoad {
    Startup,
    Reload
};

// One [[symbolFont]] config entry: inside [first, last] the named font
// is consulted before the regular fallback chain. Entries are tried in
// document order; a font that does not cover the cluster falls through
// to the next matching entry and then to the ordinary chain.
struct SymbolFontSpan {
    u32 first = 0;
    u32 last = 0;
    stl::StringView font;
};

// Every string lives in the ObjPool the instance was created in, NUL
// terminated, so a view's data() doubles as a C string for the libc
// calls that need one.
struct Options {
    // The semantic knobs of the VT core live in the embedded VtConfig;
    // everything else here is the interactive shell around it.
    VtConfig vt;
    u8 fontsize = 0;
    // -1: classic hinted grid rendering. 0..100: unhinted rendering with
    // subpixel glyph placement, the value scaling the stem darkening.
    i8 soft = -1;
    u16 border = 0;
    u16 nCols = 0;
    u16 nRows = 0;
    stl::Vector<stl::StringView> fontnames;
    // TOML-only ([[symbolFont]] tables); there is no command-line form.
    stl::Vector<SymbolFontSpan> symbolFonts;
    stl::Vector<stl::StringView> remaps;
    stl::Vector<stl::StringView> uriSchemes;
    // The lowercased spellings of uriSchemes, interned as a trie at
    // parse time; the host adapter answers scheme policy from it.
    const Darts* uriSchemeTrie = nullptr;
    stl::StringView shell;
    // -debug: append window/font/grid diagnostics to this file.
    stl::StringView debugTrace;
    OptionSource titleSource = OptionSource::NONE;
    bool vulkanInfo = false;
    // Skip the direct-storage swapchain even where the surface offers
    // it: the CI shadow renderer walks the blit fallback this way.
    bool vulkanBlit = false;
    bool login = false;
    bool maximized = false;
    // Fullscreen wins over maximized when both are set: it is the
    // stronger request, and the window manager would otherwise
    // resolve the pair for us differently on every platform.
    bool fullscreen = false;
    // The macOS natural-text-editing preset: Option word gestures and
    // Command line gestures as chords, at the price of the reserved
    // Command arrows.
    bool naturalEditing = false;
    bool noDecorations = false;
    // -titleFallback process: the active terminal's title follows the
    // pty's foreground process name whenever the name changes and no
    // fresher application title replaces it.
    bool titleFallbackProcess = false;
    bool optical = false;
    bool showWraps = false;
    bool cursorKeepSelectionFg = false;
    bool rv = false;

    static Options* create(stl::ObjPool& pool, Brand& brand, char** argv, int argc, OptionsLoad load = OptionsLoad::Startup);

    // Case-folds the scheme and answers from uriSchemeTrie; false until
    // the trie exists, so an unparsed instance allows nothing.
    bool uriSchemeAllowed(stl::StringView scheme) const;
};
