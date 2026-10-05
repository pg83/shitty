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

#include <plt/window.h>

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

// What -backgroundBlur asks the window to put behind a translucent
// background. Off creates no backdrop at all; Blur is the frosted
// NSVisualEffectView this terminal has had since T10; Glass is the
// macOS 26 system glass, which falls back to Blur where the system has
// none.
//
// The numeric values are not a contract - the test-mode dump prints the
// name for exactly that reason - so a mode may be inserted anywhere.
enum class BackdropMode: u8 {
    Off,
    Blur,
    Glass
};

// The spelling the config, the command line and the test-mode dump all
// share. One table, so a mode cannot be accepted under a name nothing
// ever prints back.
stl::StringView backdropModeName(BackdropMode mode);

// One [[symbolFont]] config entry: inside [first, last] the named font
// is consulted before the regular fallback chain. Entries are tried in
// document order; a font that does not cover the cluster falls through
// to the next matching entry and then to the ordinary chain.
struct SymbolFontSpan {
    u32 first = 0;
    u32 last = 0;
    stl::StringView font;
};

// A program the command palette opens by name ([[app]] in the config):
// run by the shell, in `directory` when one is given.
struct PaletteApp {
    stl::StringView name;
    stl::StringView command;
    stl::StringView directory;
};

// A named set of variables the command palette opens a tab with, or
// exports into the current one ([[env]]): "K=V" lines.
struct PaletteEnv {
    stl::StringView name;
    stl::StringView variables;
};

// Every string lives in the ObjPool the instance was created in, NUL
// terminated, so a view's data() doubles as a C string for the libc
// calls that need one.
//
// The member initializers below are NOT the product's defaults, and are
// not meant to be. Production never reads one: OptionsParser::parse()
// assigns every field before an Options instance leaves
// Options::create(), and the only other instance in the tree -
// Composer's placeholder (composer.cpp) - is replaced by the parsed one
// at startup. What they are is the inert value of each knob, which is
// what a unit-test composer wants to start from: border 0, fontsize 0,
// saveLines 0, an opaque background, no sidebar, no panes.
//
// The product's defaults live in one place, optionsTable's hardDefault
// column in options.cpp, and the example configs carry the same values
// in a form a reader can copy. T8 changed fifteen of them without
// touching a line here, and that is the shape to keep: a field whose
// initializer is a *chosen* constant rather than an inert one -
// sidebarTabTint's 65, and tabs' true (the inert value for a window that
// behaves as it did before the option) - is the one that has to be kept in
// step with the table by hand.
struct Options {
    // The semantic knobs of the VT core live in the embedded VtConfig;
    // everything else here is the interactive shell around it.
    VtConfig vt;
    u8 fontsize = 0;
    // -1: classic hinted grid rendering. 0..100: unhinted rendering with
    // subpixel glyph placement, the value scaling the stem darkening.
    i8 soft = -1;
    // How much of the desktop shows through the terminal background, as a
    // percentage of opaque: 100 keeps today's solid window, 0 leaves the
    // background invisible. Only the *background* follows it - glyphs,
    // the cursor, a selection and the pane divider stay solid, because a
    // terminal whose letters are see-through is unreadable.
    //
    // Read once, at startup, on both sides of the decision: the Cocoa
    // window is made transparent at creation time from this value
    // (platform_cocoa.mm), and the renderer will not write alpha into a
    // layer that was created opaque. A reload that raises or lowers it
    // within a window that started translucent takes effect; one that
    // asks an opaque window to become translucent needs a restart. The
    // alternative - letting the renderer act on a reload the window
    // cannot follow - paints the background darker instead of
    // see-through, which is a wrong picture rather than an unchanged one.
    u16 backgroundOpacity = 100;
    u16 border = 0;
    // The seam between two panes, in pixels, and what colour it is.
    // A10's default used to be a zero gap - panes touching, their own
    // borders making the air between them - which left nothing to see
    // and nothing to aim at. One pixel is the smallest thing that is
    // still a line; the grab strip is a separate number and does not
    // follow this one (see SessionSetImpl::dividerGrab).
    u16 paneDividerWidth = 1;
    u16 nCols = 0;
    u16 nRows = 0;
    // Quick-terminal window corner radius, in points; 0 disables rounding.
    // Parsed and range-checked here; threaded into plt::WindowOptions and
    // consumed by the Cocoa window layer by T3, which is the only reader.
    u16 quickCornerRadius = 0;
    // Width of the sidebar tab list, in points, when -tabBar is sidebar.
    // Reserved into Composer::contentInsets().left by ui_sidebar_tabs.mm.
    u16 sidebarWidth = 0;
    // How opaque the active tab's glass pill is, 0..100, on the same
    // scale as backgroundOpacity: 100 is the terminal background flat,
    // 0 is untinted glass with the desktop straight through. Only
    // -backgroundBlur glass has a pill to tint; the other two backdrops
    // paint the active row and ignore this. Read by ui_sidebar_tabs.mm,
    // whose comment over applyPill() carries the measurements behind the
    // default.
    u8 sidebarTabTint = 65;
    // The layered window (ui_sidebar_tabs.mm): the surface's opacity
    // under the terminal panel, 0..100 on backgroundOpacity's scale, and
    // the panel's distance from the window edges and its corner radius,
    // both in points. The panel itself takes bg and backgroundOpacity,
    // the surface sidebarColor and this.
    u8 sidebarOpacity = 0;
    // The active tab's flat highlight on the layered surface: its opacity,
    // and (sidebarTabColor below, when sidebarTabColorSet) its colour.
    u8 sidebarTabOpacity = 0;
    u16 panelGap = 0;
    u16 panelRadius = 0;
    // bookmarks.toml, when -bookmarksFile names another; empty is the
    // default beside the config (bookmarks.h).
    stl::StringView bookmarksFile;
    // The command palette's: where clones go (~ expanded by the caller),
    // and the login before a Teleport host.
    stl::StringView cloneDirectory;
    stl::StringView teleportLogin;
    stl::Vector<stl::StringView> fontnames;
    // TOML-only ([[symbolFont]] tables); there is no command-line form.
    stl::Vector<SymbolFontSpan> symbolFonts;
    // TOML-only too ([[app]] and [[env]] tables), for the command palette.
    stl::Vector<PaletteApp> paletteApps;
    stl::Vector<PaletteEnv> paletteEnvs;
    stl::Vector<stl::StringView> remaps;
    stl::Vector<stl::StringView> uriSchemes;
    // The lowercased spellings of uriSchemes, interned as a trie at
    // parse time; the host adapter answers scheme policy from it.
    const Darts* uriSchemeTrie = nullptr;
    stl::StringView shell;
    // -directory, as written: `~` is expanded and the launcher's `/` is
    // second-guessed by launchDirectory() at startup, not here.
    stl::StringView directory;
    // -debug: append window/font/grid diagnostics to this file.
    stl::StringView debugTrace;
    // The chord that toggles the quick-terminal window; only parsed and
    // validated non-empty here, the chord grammar itself is T3's.
    stl::StringView quickHotkey;
    // Path to a config file for a quick-terminal companion process this
    // one spawns and manages; empty runs without one. Only stored here -
    // ~ expansion, realpath canonicalization, the self-reference guard,
    // and the fork/exec itself all live in quick_companion.cpp, which
    // needs argv0 and the filesystem the option parser does not have.
    stl::StringView quickCompanion;
    // The chord that toggles quick-terminal window fullscreen. Only parsed
    // and stored here, same as quickHotkey above; empty means disabled.
    // Chord grammar and registration are T3's, in the same module as
    // quickHotkey (ui_quick_hotkey.mm).
    stl::StringView quickFullscreenHotkey;
    // The main config file OptionsParser::loadConfigFile() resolved for
    // this process - set whether or not the file actually exists, empty
    // when no config path could even be computed (no -config, no HOME,
    // no XDG_CONFIG_HOME). quick_companion.cpp's self-reference guard
    // canonicalizes this and compares it against quickCompanion's own
    // target; it is otherwise unused.
    stl::StringView configPath;
    // The quick-terminal window's size and position, parsed from
    // -quickGeometry by lib/shitty/quick_geometry.cpp. The product
    // default is the table's, 90%x75%+5%+10% - an inset panel rather
    // than the full-width strip this started as.
    //
    // The member initializer below it is plt::QuickGeometry's own, and
    // is deliberately not that: it still reproduces
    // ShowPlacement::TopOfActiveScreen's original 100%x40%+0+0. Nothing
    // production ever reads it - OptionsParser::parse() calls
    // getQuickGeometry() unconditionally - and the struct lives across
    // the ext/plt boundary, so it is left where it is, the same way
    // border and fontsize leave theirs at zero.
    plt::QuickGeometry quickGeometry;
    OptionSource titleSource = OptionSource::NONE;
    // T8: a hard default of its own (#00cd00) rather than the scheme's
    // bright black it used to derive. A seam has to be found by the eye
    // and aimed at by the mouse, and a per-scheme grey is the thing that
    // disappears into some schemes. Filled by the parser on every path;
    // the zero here is never read.
    Color paneDividerColor{};
    // C10. The sidebar panel's colour, and the origin every other shade
    // in the panel is mixed from - a panel whose background is set by
    // hand and whose active-row highlight is still derived from the
    // terminal's can drift apart until neither reads.
    //
    // Unset is not a colour but an absence: the panel then keeps
    // deriving itself from bg and fg exactly as it did before this
    // option existed, because that derivation is AppKit's and cannot be
    // reproduced here byte for byte. sidebarColorSet is what says which.
    Color sidebarColor{};
    // The layered window's active-tab highlight; unset follows fg.
    Color sidebarTabColor{};
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
    // Whether zsh gets the terminal's integration (shell_integration.h).
    bool shellIntegration = false;
    bool noDecorations = false;
    // -titleFallback process: the active terminal's title follows the
    // pty's foreground process name whenever the name changes and no
    // fresher application title replaces it.
    bool titleFallbackProcess = false;
    bool optical = false;
    // Runs as a quick-terminal window: hidden at startup, shown and
    // hidden by the quickHotkey chord instead of the normal show-on-start.
    bool quick = false;
    // Persist the quick-terminal window's manually set position and size
    // across shows, via lib/shitty/quick_frame_store.{h,cpp} (T2). Parsed
    // here; the save/restore path itself is T3's.
    bool quickRememberFrame = false;
    // Where the tab bar lives, resolved from -tabBar: false is the
    // title bar, true - the default since T8 - is a vertical list down
    // the window's left edge reserving sidebarWidth out of the grid.
    // One placement or the other, never both - which is the whole of
    // what cmd+b used to get wrong by swapping between them (V3).
    bool sidebarTabs = false;
    // The terminal as a rounded panel over the window's own surface, the
    // tab list left on the surface underneath. Only a Cocoa window with
    // the sidebar shows it; everywhere else it is read and changes nothing.
    bool layeredWindow = false;
    // Hide the titlebar chrome and reveal it on mouse hover, without
    // changing the grid's row count (A7). Unused until T6.
    bool autoHideChrome = false;
    // Allow splitting a tab's terminal into multiple panes (cmd+d /
    // cmd+shift+d). Unused until T9/T10 build the pane tree.
    bool panes = false;
    // A window holds more than one shell: the tab chords are bound and the
    // tab bar (-tabBar) can show. Off, a window is one shell and nothing
    // else - the tab list with it. st's default on Linux (brand.h).
    // Starts true, unlike its neighbours: the option is newer than every
    // fixture built on these initializers, and each of them was written
    // against a window that has tabs.
    bool tabs = true;
    bool showWraps = false;
    // The titlebar's color matches the terminal background instead of
    // the system chrome color. Geometry is untouched: no
    // FullSizeContentView, the content area stays below the titlebar.
    // What to put behind whatever shows through the translucent
    // background: nothing, iTerm2's frosted blur, or system glass.
    // Meaningless while backgroundOpacity is 100 - an
    // opaque background covers the blurred backdrop completely - and in
    // that case the backdrop is simply never created rather than the
    // option being rejected: backgroundOpacity is reloadable, and a
    // config that is legal at one of its values and fatal at another
    // turns a one-line edit into a refusal to start. Cocoa-only.
    BackdropMode backgroundBlur = BackdropMode::Off;
    bool sidebarColorSet = false;
    bool sidebarTabColorSet = false;
    bool transparentTitlebar = false;
    bool cursorKeepSelectionFg = false;
    bool rv = false;

    // desktopLaunch is the launch classification captured once at startup
    // (composer.desktopLaunch); it feeds the login default and must be the
    // same value on every reload.
    static Options* create(stl::ObjPool& pool, Brand& brand, char** argv, int argc, OptionsLoad load = OptionsLoad::Startup, bool desktopLaunch = false);

    // Case-folds the scheme and answers from uriSchemeTrie; false until
    // the trie exists, so an unparsed instance allows nothing.
    bool uriSchemeAllowed(stl::StringView scheme) const;
};
