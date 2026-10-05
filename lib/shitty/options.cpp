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

#include "options.h"

#include "toml.h"
#include "brand.h"
#include "darts.h"
#include "quick_geometry.h"
#include "terminal_colors.h"

#include <lib/vterm/num.h>
#include <lib/vterm/fatal.h>

#include <std/ios/sys.h>
#include <std/alg/xchg.h>
#include <std/str/view.h>
#include <std/sym/s_map.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/ios/fs_utils.h>
#include <std/mem/obj_pool.h>

#include <wchar.h>
#include <stdlib.h>
#include <string.h>

using namespace stl;

extern "C" char** environ;

namespace {

    enum class OptionKind {
        NoArg,
        SepArg,
        SkipLine
    };

    struct OptionDesc {
        const char* option;
        OptionKind parseType;
        const char* implValue;
        const char* hardDefault;
        const char* helpDescr;
        // Options that only make sense on a command line stay out of the
        // config file.
        bool cliOnly = false;
        // What to name when refusing a spelling that carries no value.
        // The hard default is always legal and reads as a fair example
        // for most options - -bg #000, -saveLines 500 - but where the
        // default is also the inert mode it answers "how do I turn this
        // on?" with "off". Spelling out the whole set instead stays
        // neutral between -opt and +opt, and says exactly what the value
        // parser says when it refuses an unknown name.
        const char* valueNames = nullptr;
    };

    struct ResourceDesc {
        const char* resource;
        const char* hardDefault;
        const char* helpDescr;
        // Working options that stay out of every help listing.
        bool hidden = false;
    };

    static const OptionDesc optionsTable[] = {

        {"altScroll", OptionKind::NoArg, "true", "false", "Alternate scroll mode"},
        {"autoCopy", OptionKind::NoArg, "true", "false", "Sync primary to clipboard"},
        {"backgroundBlur", OptionKind::SepArg, nullptr, "glass", "What to put behind a translucent background: off, blur or glass; glass falls back to blur where the system has none, and none of them does anything while backgroundOpacity is 100", false, "off, blur or glass"},
        {"backgroundOpacity", OptionKind::SepArg, nullptr, "60", "Opacity of the terminal background, 0..100; 100 is opaque, and only the background goes translucent - text, cursor, selection and the pane divider stay solid"},
        {"bg", OptionKind::SepArg, nullptr, "#000", "Background color"},
        {"boldColors", OptionKind::NoArg, "true", "false", "Brighten bold text's palette colors"},
        {"border", OptionKind::SepArg, nullptr, "2", "Border width in pixels"},
        {"config", OptionKind::SepArg, nullptr, nullptr, "Path to the TOML config file", true},
        {"colorScheme", OptionKind::SepArg, nullptr, "Catppuccin Mocha", "Named terminal color scheme"},
        {"cr", OptionKind::SepArg, nullptr, nullptr, "Cursor color"},
        {"cursorKeepSelectionFg", OptionKind::NoArg, "true", "false", "Keep selected or reverse-video text color under the block cursor"},
        {"debug", OptionKind::SepArg, nullptr, nullptr, "Append window, font and grid diagnostics to this file", true},
        {"directory", OptionKind::SepArg, nullptr, nullptr, "Working directory for the shell; ~ and a leading ~/ mean the home directory. Unset, the shell inherits the launcher's directory, except a launcher's / - what launchd hands a bundled app - which becomes the home directory. A new tab starts where the active tab's foreground process is"},
        {"dump", OptionKind::SepArg, nullptr, nullptr, "Dump raw PTY input to file"},
        {"fg", OptionKind::SepArg, nullptr, "#fff", "Foreground color"},
        {"font", OptionKind::SepArg, nullptr, "monospace", "Font to use; repeat for fallbacks"},
        {"fontsize", OptionKind::SepArg, nullptr, "15", "Font size"},
        {"fullscreen", OptionKind::NoArg, "true", "false", "Start with the window fullscreen"},
        {"soft", OptionKind::SepArg, nullptr, "-1", "Unhinted subpixel rendering; 0..100 scales the stem darkening"},
        {"geometry", OptionKind::SepArg, nullptr, "80x24", "Terminal size in chars"},
        {"kittyCtrlBaseLayout", OptionKind::NoArg, "true", "false", "Report the ASCII base key as the Kitty primary under Ctrl"},
        {"vulkanInfo", OptionKind::NoArg, "true", "false", "Print Vulkan information", true},
        {"vulkanBlit", OptionKind::NoArg, "true", "false", "Present through the offscreen blit path", true},
        {"help", OptionKind::NoArg, "true", "false", "Print usage listing and quit", true},
        {"listres", OptionKind::NoArg, "true", "false", "Print advanced option listing and quit", true},
        {"listColorSchemes", OptionKind::NoArg, "true", "false", "Print terminal color scheme names and quit", true},
        {"printConfig", OptionKind::NoArg, "true", "false", "Print a config file carrying every option at its default, and quit; redirect it into the config path to start from the shipped configuration", true},
        {"login", OptionKind::NoArg, "true", "false", "Start shell as a login shell"},
        {"maximized", OptionKind::NoArg, "true", "false", "Start with the window maximized"},
        {"naturalEditing", OptionKind::NoArg, "true", "true", "Bind the macOS natural text editing chords"},
        {"promptEditor", OptionKind::NoArg, "true", "true", "Edit the command line at a shell prompt like a text field: click to place the cursor, select and delete, Cmd+A to select the command; needs the shell integration"},
        {"shellIntegration", OptionKind::NoArg, "true", "true", "Load the terminal's zsh integration into zsh shells, which tells the terminal where each prompt's command line is"},
        {"no-decorations", OptionKind::NoArg, "true", "false", "Disable window decorations"},
        {"optical", OptionKind::NoArg, "true", "false", "Optically space simple Latin and Cyrillic runs"},
        {"quick", OptionKind::NoArg, "true", "false", "Run as a quick-terminal window, hidden at startup and toggled by quickHotkey"},
        {"quickHotkey", OptionKind::SepArg, nullptr, "ctrl+grave", "Chord that toggles the quick-terminal window"},
        {"quickGeometry", OptionKind::SepArg, nullptr, "90%x75%+5%+10%", "Quick-terminal window size and position: <W>x<H>+<X>+<Y>, each pixels or a percent of the screen's usable area"},
        {"quickCompanion", OptionKind::SepArg, nullptr, nullptr, "Path to a config file for a quick-terminal companion process this one spawns and manages; ignored when quick is true"},
        {"quickCornerRadius", OptionKind::SepArg, nullptr, "12", "Quick-terminal window corner radius in points; 0 disables rounding"},
        {"quickRememberFrame", OptionKind::NoArg, "true", "true", "Remember the quick-terminal window's manually set position and size across shows"},
        {"quickFullscreenHotkey", OptionKind::SepArg, nullptr, nullptr, "Chord that toggles quick-terminal window fullscreen; empty disables it"},
        {"tabBar", OptionKind::SepArg, nullptr, "sidebar", "Where the tab bar lives: top or sidebar"},
        {"sidebarColor", OptionKind::SepArg, nullptr, nullptr, "Color of the sidebar tab list; defaults to a shade off the terminal background, and every other shade in the panel follows it. A color far from the background necessarily covers more of what shows through a translucent window"},
        {"sidebarTabTint", OptionKind::SepArg, nullptr, "65", "How opaque the active tab's glass pill is, 0..100 on the same scale as backgroundOpacity; 100 is the terminal background flat, 0 is clear glass with the desktop straight through. Only -backgroundBlur glass draws that pill, so this does nothing under blur or off"},
        {"sidebarWidth", OptionKind::SepArg, nullptr, "220", "Width of the sidebar tab list in points"},
        {"layeredWindow", OptionKind::NoArg, "true", "true", "Draw the terminal as a rounded panel laid over the window's own surface, the sidebar tab list on the surface beneath it. macOS and Wayland, with -tabBar sidebar; the panel takes -bg and -backgroundOpacity, the surface -sidebarColor and -sidebarOpacity"},
        {"sidebarOpacity", OptionKind::SepArg, nullptr, "55", "Opacity of the window surface under the terminal panel, 0..100, on the same scale as backgroundOpacity; only -layeredWindow has that surface"},
        {"sidebarTabColor", OptionKind::SepArg, nullptr, nullptr, "Color of the active tab's highlight on the -layeredWindow surface; defaults to the terminal's foreground"},
        {"sidebarTabOpacity", OptionKind::SepArg, nullptr, "16", "Opacity of the active tab's highlight on the -layeredWindow surface, 0..100; the hovered row takes half of it"},
        {"panelGap", OptionKind::SepArg, nullptr, "8", "Space between the terminal panel and the window's edges, in points, 0..100; only -layeredWindow has a panel"},
        {"panelRadius", OptionKind::SepArg, nullptr, "12", "Corner radius of the terminal panel in points, 0..100; only -layeredWindow has a panel"},
        {"bookmarksFile", OptionKind::SepArg, nullptr, nullptr, "File of [[bookmark]] tables the sidebar lists above the tabs and pins into; defaults to bookmarks.toml beside the config file"},
        {"cloneDirectory", OptionKind::SepArg, nullptr, "~/Projects", "Where the command palette's Clone Repository puts a repository, in a folder of its name"},
        {"teleportLogin", OptionKind::SepArg, nullptr, nullptr, "Login the command palette puts before a Teleport host (tsh ssh LOGIN@host); unset, tsh picks"},
        {"autoHideChrome", OptionKind::NoArg, "true", "true", "Hide the titlebar chrome and reveal it on mouse hover"},
        {"panes", OptionKind::NoArg, "true", "true", "Allow splitting a tab's terminal into multiple panes"},
        {"tabs", OptionKind::NoArg, "true", "true", "Allow more than one tab in a window; off, a window is a single shell, with no tab chords and no tab list"},
        {"paneDividerColor", OptionKind::SepArg, nullptr, "#00cd00", "Color of the seam between panes. Needs -border above 0 to have anywhere to paint"},
        {"paneDividerWidth", OptionKind::SepArg, nullptr, "1", "Thickness of the seam between panes, in pixels; painted into the air the panes' own borders leave, so it is clamped to twice -border and invisible when -border is 0"},
        {"remap", OptionKind::SepArg, nullptr, nullptr, "Rewrite a key chord, from=to; repeat for more"},
        {"rv", OptionKind::NoArg, "true", "false", "Reverse video"},
        {"saveLines", OptionKind::SepArg, nullptr, "50000", "Lines of scrollback history"},
        {"shell", OptionKind::SepArg, nullptr, nullptr, "Shell program to run"},
        {"showWraps", OptionKind::NoArg, "true", "false", "Show wrap marks at right margin"},
        {"title", OptionKind::SepArg, nullptr, nullptr, "Window title"},
        {"titleFallback", OptionKind::SepArg, nullptr, "process", "Title when the app sets none: process or none"},
        {"transparentTitlebar", OptionKind::NoArg, "true", "true", "Make the titlebar's color match the terminal background"},
        {"unicodeWidths", OptionKind::SepArg, nullptr, "0", "Unicode version for character widths; 0 matches the system libc"},
        {"uriScheme", OptionKind::SepArg, nullptr, nullptr, "Open a plain URI with this scheme; repeat for more, default http https file mailto gemini"},
        {"verbose", OptionKind::NoArg, "true", "false", "Output info messages"},
        {"version", OptionKind::NoArg, "true", "false", "Print version and quit", true},
        {"e", OptionKind::SkipLine, nullptr, nullptr, "Command line to run", true},
    };

    static const ResourceDesc resourceTable[] = {

        {"altSendsEscape", "true", "Encode Alt key as ESC prefix"},
        {"modifyOtherKeys", "1", "Key modifier encoding level; 0..2"},
        {"allowOsc52Read", "false", "Allow applications to read clipboard via OSC 52"},
        {"allowWindowOps", "false", "Allow applications to manipulate and query the window"},
        {"osc52Select", "primary", "Selection used by OSC 52 selector s: primary or clipboard"},
        {"color0", nullptr, "Palette color 0"},
        {"color1", nullptr, "Palette color 1"},
        {"color2", nullptr, "Palette color 2"},
        {"color3", nullptr, "Palette color 3"},
        {"color4", nullptr, "Palette color 4"},
        {"color5", nullptr, "Palette color 5"},
        {"color6", nullptr, "Palette color 6"},
        {"color7", nullptr, "Palette color 7"},
        {"color8", nullptr, "Palette color 8"},
        {"color9", nullptr, "Palette color 9"},
        {"color10", nullptr, "Palette color 10"},
        {"color11", nullptr, "Palette color 11"},
        {"color12", nullptr, "Palette color 12"},
        {"color13", nullptr, "Palette color 13"},
        {"color14", nullptr, "Palette color 14"},
        {"color15", nullptr, "Palette color 15"},
    };

    // Everything the parser stores - scalar values and list entries alike
    // - is interned into the pool the Options instance itself lives in,
    // so the parsed result owns nothing separately and dies with its
    // pool.
    struct OptionsParser final: public Options {
        OptionsParser(ObjPool& owner, Brand& brand, char** argv, int argc, OptionsLoad load, bool desktopLaunch);

        void initialize(int* argc, char** argv);
        void handlePrintOpts();
        void parse();
        void loadConfigFile();
        void loadConfigFrom(StringView path, bool required, int depth);
        bool get(const char* name, StringView& out, OptionSource* src = nullptr);
        const OptionDesc* findOption(const char* prefix);
        bool isAdvancedOption(StringView name) const;
        bool isConfigurableOption(StringView name) const;
        void getBorder(u16& outBorder);
        void getBackgroundOpacity(u16& outOpacity);
        void getPaneDividerWidth(u16& outWidth);
        void getSaveLines(u16& outSaveLines);
        void getQuickCornerRadius(u16& outRadius);
        void getSidebarTabTint(u8& outTint);
        void getSidebarWidth(u16& outWidth);
        void getPercent(const char* name, u8& outPercent);
        void getPoints(const char* name, u16& outPoints);
        void getUnicodeWidths(UnicodeWidths& outWidths);
        void getFontsize(u8& outFontsize);
        void getSoft(i8& outSoft);
        void getGeometry(u16& outCols, u16& outRows);
        void getQuickGeometry(plt::QuickGeometry& outGeometry);
        void printVersion() const;
        void printConfig() const;
        void printUsage() const;
        void printResources() const;
        void printColorSchemes() const;
        bool getBool(const char* name, bool defaultValue = false);
        void getBackdropMode(const char* name, BackdropMode& out);
        void getColor(const char* name, Color& outColor);
        int getInteger(const char* name, int min, int max);
        Vector<StringView>* configList(StringView name);

        ObjPool& pool;
        Brand& brand;
        Darts* optionTrie = nullptr;
        Darts* resourceTrie = nullptr;
        SymbolMap<StringView> commandLine;
        SymbolMap<StringView> configFile;
        Vector<StringView> configFonts;
        Vector<SymbolFontSpan> configSymbolFonts;
        Vector<PaletteApp> configApps;
        Vector<PaletteEnv> configEnvs;
        Vector<StringView> configRemaps;
        Vector<StringView> configUriSchemes;
        OptionsLoad load;
        // The launch classification the caller captured at startup; a
        // reload receives the same value it was born with.
        bool desktopLaunch;
        bool configSyntaxError = false;
    };
}

// The list-shaped options; everything else in the config file is a
// scalar.
Vector<StringView>* OptionsParser::configList(StringView name) {
    if (name == StringView(u8"font")) {
        return &configFonts;
    }
    if (name == StringView(u8"remap")) {
        return &configRemaps;
    }
    if (name == StringView(u8"uriScheme")) {
        return &configUriSchemes;
    }
    return nullptr;
}

namespace {

    static void writeSpaces(ZeroCopyOutput& output, size_t count) {
        static constexpr u8 spaces[] = u8"                                ";
        while (count != 0) {
            const size_t chunk = count < sizeof(spaces) - 1 ? count : sizeof(spaces) - 1;
            output.write(spaces, chunk);
            count -= chunk;
        }
    }
}

const OptionDesc* OptionsParser::findOption(const char* prefix) {
    if (StringView(prefix) == StringView(u8"v")) {
        prefix = "version";
    }

    const i32 resolved = optionTrie->resolve(StringView(prefix));
    if (resolved == Darts::ambiguous) {
        raiseError(StringView(u8"ambiguous option: "), StringView(prefix));
    }
    if (resolved < 0) {
        return nullptr;
    }
    return &optionsTable[resolved];
}

bool OptionsParser::isAdvancedOption(StringView name) const {
    return resourceTrie->find(name) >= 0;
}

stl::StringView backdropModeName(BackdropMode mode) {
    switch (mode) {
        case BackdropMode::Blur:
            return StringView(u8"blur");
        case BackdropMode::Glass:
            return StringView(u8"glass");
        case BackdropMode::Off:
            break;
    }
    return StringView(u8"off");
}

bool OptionsParser::isConfigurableOption(StringView name) const {
    const i32 option = optionTrie->find(name);
    if (option >= 0) {
        return !optionsTable[option].cliOnly;
    }
    return isAdvancedOption(name);
}

namespace {

    // Fills configFile/configFonts from the SAX events of one TOML
    // document. A config problem must not keep the terminal from starting:
    // everything suspicious is a warning on stderr and the entry is
    // ignored, so no callback ever aborts the parse.
    struct ConfigSink: public TomlSink {
        // Which value the next scalar inside a [[symbolFont]] table fills.
        enum class SymbolKey : u8 {
            None,
            Font,
            First,
            Last,
        };

        OptionsParser& options;
        const char* path;
        Buffer pending;
        Vector<StringView>* pendingList;
        bool pendingKnown;
        bool skippingTable;
        int arrayDepth;
        int inlineDepth;
        SymbolFontSpan symbolEntry;
        SymbolKey symbolPending;
        bool symbolOpen;
        bool symbolSeen;
        bool symbolFirstSet;
        bool symbolLastSet;
        bool symbolBroken;
        // An [[app]] or [[env]] table being read: which, its fields, the
        // key the next scalar is for, and whether this file has had one
        // (its first drops the imported ones, as a list option does).
        enum class Entry : u8 {
            None,
            App,
            Env,
        };
        Entry entryOpen = Entry::None;
        bool entryBroken = false;
        bool appSeen = false;
        bool envSeen = false;
        Buffer entryKey;
        PaletteApp app;
        StringView envName;
        Buffer envVariables;

        ConfigSink(OptionsParser& options, const char* path);

        bool tomlTable(const stl::StringView* segments, size_t count, bool array) override;
        bool tomlKey(const stl::StringView* segments, size_t count) override;
        bool tomlScalar(TomlType type, stl::StringView text) override;
        bool tomlArrayBegin() override;
        bool tomlArrayEnd() override;
        bool tomlInlineTableBegin() override;
        bool tomlInlineTableEnd() override;
        void tomlError(size_t line, stl::StringView message) override;

        // Validates and commits the open [[symbolFont]] entry; called on
        // the next table header and once after the document ends.
        void finishSymbolEntry();
        // The same for the open [[app]] or [[env]] entry.
        void finishEntry();

        void warn(const char* what, StringView name);
    };
}

ConfigSink::ConfigSink(OptionsParser& options_, const char* path)
    : options(options_)
    , path(path)
    , pendingList(nullptr)
    , pendingKnown(false)
    , skippingTable(false)
    , arrayDepth(0)
    , inlineDepth(0)
    , symbolPending(SymbolKey::None)
    , symbolOpen(false)
    , symbolSeen(false)
    , symbolFirstSet(false)
    , symbolLastSet(false)
    , symbolBroken(false)
{
}

void ConfigSink::warn(const char* what, StringView name) {
    const StringView identifier = options.brand.identifier();
    if (name.empty()) {
        sysE << identifier << StringView(u8": ") << StringView(path) << StringView(u8": ") << StringView(what) << endL;
    } else {
        sysE << identifier << StringView(u8": ") << StringView(path) << StringView(u8": ") << StringView(what) << StringView(u8": ") << name << endL;
    }
}

void ConfigSink::finishSymbolEntry() {
    if (!symbolOpen) {
        return;
    }
    const SymbolFontSpan entry = symbolEntry;
    const bool broken = symbolBroken;
    const bool firstSet = symbolFirstSet;
    const bool lastSet = symbolLastSet;
    symbolEntry = SymbolFontSpan();
    symbolPending = SymbolKey::None;
    symbolFirstSet = false;
    symbolLastSet = false;
    symbolBroken = false;
    if (broken) {
        // The offending key or value warned already; a half-understood
        // entry must not silently claim a codepoint range.
        return;
    }
    if (entry.font.empty()) {
        warn("symbolFont entry without a font", StringView());
        return;
    }
    if (firstSet != lastSet) {
        warn("symbolFont needs both first and last (or neither)", entry.font);
        return;
    }
    if (!firstSet) {
        // No explicit range: the three Private Use Areas, where patched
        // pictogram fonts live by construction.
        options.configSymbolFonts.pushBack({0xE000, 0xF8FF, entry.font});
        options.configSymbolFonts.pushBack({0xF0000, 0xFFFFD, entry.font});
        options.configSymbolFonts.pushBack({0x100000, 0x10FFFD, entry.font});
        return;
    }
    if (entry.first > entry.last) {
        warn("symbolFont range has first above last", entry.font);
        return;
    }
    options.configSymbolFonts.pushBack(entry);
}

void ConfigSink::finishEntry() {
    const Entry entry = entryOpen;
    const bool broken = entryBroken;
    entryOpen = Entry::None;
    entryBroken = false;
    entryKey.reset();
    if (entry == Entry::App) {
        const PaletteApp made = app;
        app = PaletteApp();
        if (broken) {
            return;
        }
        if (made.name.empty() || made.command.empty()) {
            warn("app needs a name and a command", made.name);
            return;
        }
        options.configApps.pushBack(made);
    } else if (entry == Entry::Env) {
        const StringView name = envName;
        envName = StringView();
        const StringView variables = options.pool.intern(StringView(envVariables));
        envVariables.reset();
        if (broken) {
            return;
        }
        if (name.empty()) {
            warn("env needs a name", StringView());
            return;
        }
        options.configEnvs.pushBack(PaletteEnv{name, variables});
    }
}

bool ConfigSink::tomlTable(const StringView* segments, size_t count, bool array) {
    finishSymbolEntry();
    finishEntry();
    symbolOpen = false;
    if (count == 1 && array && (segments[0] == StringView(u8"app") || segments[0] == StringView(u8"env"))) {
        const bool isApp = segments[0] == StringView(u8"app");
        bool& seen = isApp ? appSeen : envSeen;
        if (!seen) {
            seen = true;
            if (isApp) {
                options.configApps.clear();
            } else {
                options.configEnvs.clear();
            }
        }
        entryOpen = isApp ? Entry::App : Entry::Env;
        skippingTable = false;
        return true;
    }
    if (count == 1 && array && segments[0] == StringView(u8"symbolFont")) {
        if (!symbolSeen) {
            // This file speaks for the whole set: its first entry drops
            // whatever the imports accumulated, matching how a list
            // option replaces the imported list wholesale.
            symbolSeen = true;
            options.configSymbolFonts.clear();
        }
        symbolOpen = true;
        return true;
    }
    if (count == 1 && !array && segments[0] == StringView(u8"symbolFont")) {
        warn("symbolFont is an array of tables, write [[symbolFont]]", StringView());
        skippingTable = true;
        return true;
    }
    if (!skippingTable) {
        warn("options are plain keys, tables are ignored", StringView());
    }
    skippingTable = true;
    return true;
}

bool ConfigSink::tomlKey(const StringView* segments, size_t count) {
    if (inlineDepth != 0) {
        return true;
    }
    if (entryOpen != Entry::None) {
        entryKey.reset();
        if (count != 1) {
            warn(entryOpen == Entry::App ? "app keys are plain keys" : "env keys are plain keys", segments[0]);
            entryBroken = true;
        } else {
            entryKey.append(segments[0].data(), segments[0].length());
        }
        return true;
    }
    if (symbolOpen) {
        symbolPending = SymbolKey::None;
        if (count != 1) {
            warn("symbolFont keys are plain keys", segments[0]);
            symbolBroken = true;
        } else if (segments[0] == StringView(u8"font")) {
            symbolPending = SymbolKey::Font;
        } else if (segments[0] == StringView(u8"first")) {
            symbolPending = SymbolKey::First;
        } else if (segments[0] == StringView(u8"last")) {
            symbolPending = SymbolKey::Last;
        } else {
            warn("unknown symbolFont key", segments[0]);
            symbolBroken = true;
        }
        return true;
    }
    pending.reset();
    pendingKnown = false;
    if (skippingTable) {
        return true;
    }
    if (count != 1) {
        warn("dotted keys are not options", segments[0]);
        return true;
    }
    pending.append(segments[0].data(), segments[0].length());
    if (StringView(pending) == StringView(u8"import")) {
        // Consumed by the import pass before this sink runs.
        return true;
    }
    pendingKnown = options.isConfigurableOption(StringView(pending));
    if (!pendingKnown) {
        warn("unknown option", StringView(pending));
    }
    return true;
}

namespace {

    // A TOML integer as a Unicode codepoint: decimal or the 0x/0o/0b
    // forms, underscores already stripped by the parser, capped at
    // U+10FFFF. Negative values and overflow fail.
    static bool parseCodepoint(StringView text, u32& out) {
        size_t at = 0;
        if (at < text.length() && text[at] == '+') {
            at += 1;
        }
        u64 base = 10;
        if (text.length() >= at + 2 && text[at] == '0') {
            const char kind = text[at + 1];
            if (kind == 'x') {
                base = 16;
                at += 2;
            } else if (kind == 'o') {
                base = 8;
                at += 2;
            } else if (kind == 'b') {
                base = 2;
                at += 2;
            }
        }
        if (at == text.length()) {
            return false;
        }
        u64 value = 0;
        for (; at < text.length(); ++at) {
            const char digit = text[at];
            u64 numeral;
            if (digit >= '0' && digit <= '9') {
                numeral = (u64)(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                numeral = (u64)(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                numeral = (u64)(digit - 'A' + 10);
            } else {
                return false;
            }
            if (numeral >= base) {
                return false;
            }
            value = value * base + numeral;
            if (value > 0x10FFFF) {
                return false;
            }
        }
        out = (u32)(value);
        return true;
    }
}

bool ConfigSink::tomlScalar(TomlType type, StringView text) {
    if (inlineDepth != 0) {
        return true;
    }
    if (entryOpen != Entry::None) {
        const StringView key(entryKey);
        if (arrayDepth != 0 || type != TomlType::String) {
            if (!entryBroken) {
                warn(entryOpen == Entry::App ? "app values are strings" : "env values are strings", key);
            }
            entryBroken = true;
            return true;
        }
        if (entryOpen == Entry::App) {
            if (key == StringView(u8"name")) {
                app.name = options.pool.intern(text);
            } else if (key == StringView(u8"command")) {
                app.command = options.pool.intern(text);
            } else if (key == StringView(u8"dir")) {
                app.directory = options.pool.intern(text);
            } else {
                warn("unknown app key", key);
                entryBroken = true;
            }
        } else if (key == StringView(u8"name")) {
            envName = options.pool.intern(text);
        } else if (!key.empty()) {
            // Every other key is a variable of the set.
            envVariables.append(key.data(), key.length());
            envVariables.append("=", 1);
            envVariables.append(text.data(), text.length());
            envVariables.append("\n", 1);
        }
        entryKey.reset();
        return true;
    }
    if (symbolOpen) {
        if (arrayDepth != 0) {
            if (!symbolBroken) {
                warn("symbolFont values are scalars", text);
            }
            symbolBroken = true;
            return true;
        }
        if (symbolPending == SymbolKey::Font) {
            if (type != TomlType::String) {
                warn("symbolFont font must be a string", text);
                symbolBroken = true;
            } else {
                symbolEntry.font = options.pool.intern(text);
            }
        } else if (symbolPending != SymbolKey::None) {
            u32 codepoint = 0;
            if (type != TomlType::Integer || !parseCodepoint(text, codepoint)) {
                warn("symbolFont first/last must be codepoint integers", text);
                symbolBroken = true;
            } else if (symbolPending == SymbolKey::First) {
                symbolEntry.first = codepoint;
                symbolFirstSet = true;
            } else {
                symbolEntry.last = codepoint;
                symbolLastSet = true;
            }
        }
        symbolPending = SymbolKey::None;
        return true;
    }
    if (arrayDepth != 0) {
        if (pendingList == nullptr) {
            return true;
        }
        if (type != TomlType::String) {
            warn("list entries must be strings", text);
            return true;
        }
        pendingList->pushBack(options.pool.intern(text));
        return true;
    }
    if (!pendingKnown) {
        return true;
    }
    if (Vector<StringView>* const list = options.configList(StringView(pending))) {
        list->clear();
        list->pushBack(options.pool.intern(text));
        return true;
    }
    options.configFile.insert(StringView(pending), options.pool.intern(text));
    return true;
}

bool ConfigSink::tomlArrayBegin() {
    if (entryOpen != Entry::None) {
        if (arrayDepth == 0 && inlineDepth == 0 && !entryBroken) {
            warn("app and env values are strings, not lists", StringView(entryKey));
            entryBroken = true;
        }
        arrayDepth += 1;
        return true;
    }
    if (symbolOpen) {
        if (inlineDepth == 0 && arrayDepth == 0 && !symbolBroken) {
            warn("symbolFont values are scalars, not lists", StringView());
            symbolBroken = true;
        }
        arrayDepth += 1;
        return true;
    }
    if (inlineDepth == 0 && arrayDepth == 0) {
        pendingList = pendingKnown ? options.configList(StringView(pending)) : nullptr;
        if (pendingList != nullptr) {
            pendingList->clear();
        } else if (pendingKnown) {
            warn("this option does not take a list", StringView(pending));
        }
    }
    arrayDepth += 1;
    return true;
}

bool ConfigSink::tomlArrayEnd() {
    arrayDepth -= 1;
    if (arrayDepth == 0) {
        pendingList = nullptr;
    }
    return true;
}

bool ConfigSink::tomlInlineTableBegin() {
    if (entryOpen != Entry::None) {
        if (inlineDepth == 0 && !entryBroken) {
            warn("app and env values are strings, not tables", StringView(entryKey));
            entryBroken = true;
        }
        inlineDepth += 1;
        return true;
    }
    if (symbolOpen) {
        if (inlineDepth == 0 && !symbolBroken) {
            warn("symbolFont values are scalars, not tables", StringView());
            symbolBroken = true;
        }
    } else if (inlineDepth == 0 && pendingKnown) {
        warn("no option takes a table", StringView(pending));
        pendingKnown = false;
    }
    inlineDepth += 1;
    return true;
}

bool ConfigSink::tomlInlineTableEnd() {
    inlineDepth -= 1;
    return true;
}

void ConfigSink::tomlError(size_t line, StringView message) {
    options.configSyntaxError = true;
    const StringView identifier = options.brand.identifier();
    sysE << identifier << StringView(u8": ") << StringView(path) << StringView(u8":") << line << StringView(u8": ") << message << StringView(u8"; ignoring the rest of the file") << endL;
}

namespace {

    // The first pass over a config file: collects the import list and
    // nothing else. Quiet on purpose - the second pass reports every
    // problem once.
    struct ImportScanSink final: public TomlSink {
        ObjPool& pool;
        Vector<StringView>& imports;
        int arrayDepth;
        int inlineDepth;
        bool pendingImport;

        ImportScanSink(ObjPool& pool, Vector<StringView>& imports);

        bool tomlTable(const stl::StringView* path, size_t count, bool array) override;
        bool tomlKey(const stl::StringView* path, size_t count) override;
        bool tomlScalar(TomlType type, stl::StringView text) override;
        bool tomlArrayBegin() override;
        bool tomlArrayEnd() override;
        bool tomlInlineTableBegin() override;
        bool tomlInlineTableEnd() override;
        void tomlError(size_t line, stl::StringView message) override;
    };
}

ImportScanSink::ImportScanSink(ObjPool& pool_, Vector<StringView>& imports_)
    : pool(pool_)
    , imports(imports_)
    , arrayDepth(0)
    , inlineDepth(0)
    , pendingImport(false)
{
}

bool ImportScanSink::tomlTable(const StringView*, size_t, bool) {
    pendingImport = false;
    return true;
}

bool ImportScanSink::tomlKey(const StringView* segments, size_t count) {
    if (inlineDepth != 0) {
        return true;
    }
    pendingImport = count == 1 && segments[0] == StringView(u8"import");
    return true;
}

bool ImportScanSink::tomlScalar(TomlType type, StringView text) {
    if (pendingImport && inlineDepth == 0 && type == TomlType::String) {
        imports.pushBack(pool.intern(text));
    }
    return true;
}

bool ImportScanSink::tomlArrayBegin() {
    arrayDepth += 1;
    return true;
}

bool ImportScanSink::tomlArrayEnd() {
    arrayDepth -= 1;
    if (arrayDepth == 0) {
        pendingImport = false;
    }
    return true;
}

bool ImportScanSink::tomlInlineTableBegin() {
    inlineDepth += 1;
    pendingImport = false;
    return true;
}

bool ImportScanSink::tomlInlineTableEnd() {
    inlineDepth -= 1;
    return true;
}

void ImportScanSink::tomlError(size_t, StringView) {
}

namespace {

    // Expands ${NAME} from the process environment anywhere in the config
    // text before parsing. Deliberately simple: one pass over the whole
    // environment, replacing every occurrence of each variable. Appending
    // the substituted value verbatim to the output keeps a
    // self-referential variable from looping forever.
    static void substituteEnvironment(Buffer& text) {
        for (char** entry = environ; *entry != nullptr; ++entry) {
            StringView name;
            StringView value;
            if (!StringView(*entry).split('=', name, value) || name.empty()) {
                continue;
            }
            StringBuilder token;
            token << StringView(u8"${") << name << StringView(u8"}");
            const StringView needle(token);
            const u8* base = (const u8*)(text.data());
            const size_t used = text.used();
            Buffer replaced;
            bool changed = false;
            size_t at = 0;
            while (at + needle.length() <= used) {
                if (memcmp(base + at, needle.data(), needle.length()) == 0) {
                    replaced.append(value.data(), value.length());
                    at += needle.length();
                    changed = true;
                } else {
                    replaced.append(base + at, 1);
                    at += 1;
                }
            }
            if (changed) {
                replaced.append(base + at, used - at);
                text.xchg(replaced);
            }
        }
    }
}

void OptionsParser::loadConfigFile() {
    StringBuilder path;
    bool required = false;
    if (const StringView* chosen = commandLine.find(StringView(u8"config"))) {
        path << *chosen;
        required = true;
    } else {
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (xdg != nullptr && xdg[0] != '\0') {
            path << StringView(xdg) << StringView(u8"/") << brand.identifier() << StringView(u8"/") << brand.identifier() << StringView(u8".toml");
        } else {
            const char* home = getenv("HOME");
            if (home == nullptr || home[0] == '\0') {
                return;
            }
            path << StringView(home) << StringView(u8"/.config/") << brand.identifier() << StringView(u8"/") << brand.identifier() << StringView(u8".toml");
        }
    }
    // Recorded whether or not the file below actually exists:
    // quick_companion.cpp's self-reference guard needs the path this
    // process would read from even when -config named a file that is
    // not there yet, and loadConfigFrom() below either loads it or
    // raises.
    configPath = pool.intern(StringView(path));
    loadConfigFrom(StringView(path), required, 0);
}

namespace {
    // Resolves an import entry against the importing file: ~/ goes to
    // the home directory, a relative path to the importing file's
    // directory.
    static void resolveImportPath(StringBuilder& resolved, StringView value, StringView importer) {
        if (value.length() >= 2 && value[0] == '~' && value[1] == '/') {
            const char* home = getenv("HOME");
            if (home != nullptr && home[0] != '\0') {
                resolved << StringView(home) << StringView(value.data() + 1, value.length() - 1);
                return;
            }
        }
        if (!value.empty() && value[0] == '/') {
            resolved << value;
            return;
        }
        size_t slash = 0;
        for (size_t at = 0; at < importer.length(); ++at) {
            if (importer[at] == '/') {
                slash = at + 1;
            }
        }
        resolved << StringView(importer.data(), slash) << value;
    }
}

void OptionsParser::loadConfigFrom(StringView path, bool required, int depth) {
    if (depth > 8) {
        raiseError(StringView(u8"config imports nest deeper than 8 files: "), path);
    }
    Buffer filename{path};
    Buffer text;
    try {
        readFileContent(filename, text);
    } catch (Exception&) {
        if (depth > 0) {
            raiseError(StringView(u8"config import: cannot open "), path);
        }
        if (required) {
            raiseError(StringView(u8"-config: cannot open "), path);
        }
        return;
    }
    substituteEnvironment(text);
    // Imports first, in order, so a later import and then the file's
    // own keys each override what came before them.
    Vector<StringView> imports;
    {
        ImportScanSink scan(pool, imports);
        parseToml(StringView(text), scan);
    }
    for (size_t at = 0; at < imports.length(); ++at) {
        StringBuilder resolved;
        resolveImportPath(resolved, imports[at], path);
        loadConfigFrom(StringView(resolved), true, depth + 1);
    }
    ConfigSink sink(*this, filename.cStr());
    parseToml(StringView(text), sink);
    // The parser has no document-end event; the last [[symbolFont]]
    // entry is still open here.
    sink.finishSymbolEntry();
    sink.finishEntry();
    if (load == OptionsLoad::Reload && configSyntaxError) {
        raiseError(StringView(u8"config reload: invalid TOML in "), path);
    }
}

bool OptionsParser::get(const char* name, StringView& out, OptionSource* src) {
    const auto withSource = [&](const OptionSource source, StringView value) {
        if (src != nullptr) {
            *src = source;
        }
        out = value;
        return source != OptionSource::NONE;
    };

    if (const StringView* parsed = commandLine.find(StringView(name))) {
        return withSource(OptionSource::CmdLine, *parsed);
    }

    if (const StringView* configured = configFile.find(StringView(name))) {
        return withSource(OptionSource::Config, *configured);
    }

    const i32 option = optionTrie->find(StringView(name));
    if (option >= 0 && StringView(name) == StringView(u8"title")) {
        return withSource(OptionSource::HardDefault, brand.displayName());
    }
    if (option >= 0) {
        if (const char* own = brand.defaultFor(StringView(name))) {
            return withSource(OptionSource::HardDefault, StringView(own));
        }
    }
    if (option >= 0 && optionsTable[option].hardDefault != nullptr) {
        return withSource(OptionSource::HardDefault, StringView(optionsTable[option].hardDefault));
    }

    const i32 resource = resourceTrie->find(StringView(name));
    if (resource >= 0 && resourceTable[resource].hardDefault != nullptr) {
        return withSource(OptionSource::HardDefault, StringView(resourceTable[resource].hardDefault));
    }

    return withSource(OptionSource::NONE, StringView());
}

namespace {

    // Whitespace around the number and a sign pass, anything else fails.
    static bool parseNumber(StringView text, long& out) {
        i64 parsed = 0;
        if (!parseI64(text.stripSpace(), parsed)) {
            return false;
        }
        out = (long)(parsed);
        return true;
    }
}

void OptionsParser::getBorder(u16& outBorder) {
    StringView value;
    long border = 0;
    if (!get("border", value) || !parseNumber(value, border) || border < 0 || border > 3000) {
        raiseError(StringView(u8"-border: expected unsigned, max. 3000"));
    }
    outBorder = (u16)(border);
}

void OptionsParser::getBackgroundOpacity(u16& outOpacity) {
    StringView value;
    long opacity = 0;
    if (!get("backgroundOpacity", value) || !parseNumber(value, opacity) || opacity < 0 || opacity > 100) {
        raiseError(StringView(u8"-backgroundOpacity: expected 0..100"));
    }
    outOpacity = (u16)(opacity);
}

void OptionsParser::getPaneDividerWidth(u16& outWidth) {
    StringView value;
    long width = 0;
    if (!get("paneDividerWidth", value) || !parseNumber(value, width) || width < 0 || width > 3000) {
        raiseError(StringView(u8"-paneDividerWidth: expected unsigned, max. 3000"));
    }
    outWidth = (u16)(width);
}

void OptionsParser::getSaveLines(u16& outSaveLines) {
    StringView value;
    long lines = 0;
    if (!get("saveLines", value) || !parseNumber(value, lines) || lines < 0 || lines > 50000) {
        raiseError(StringView(u8"-saveLines: expected unsigned, max. 50000"));
    }
    outSaveLines = (u16)(lines);
}

void OptionsParser::getQuickCornerRadius(u16& outRadius) {
    StringView value;
    long radius = 0;
    if (!get("quickCornerRadius", value) || !parseNumber(value, radius) || radius < 0 || radius > 1000) {
        raiseError(StringView(u8"-quickCornerRadius: expected unsigned, max. 1000"));
    }
    outRadius = (u16)(radius);
}

void OptionsParser::getSidebarTabTint(u8& outTint) {
    StringView value;
    long tint = 0;
    if (!get("sidebarTabTint", value) || !parseNumber(value, tint) || tint < 0 || tint > 100) {
        raiseError(StringView(u8"-sidebarTabTint: expected 0..100"));
    }
    outTint = (u8)(tint);
}

void OptionsParser::getSidebarWidth(u16& outWidth) {
    StringView value;
    long width = 0;
    if (!get("sidebarWidth", value) || !parseNumber(value, width) || width < 1 || width > 3000) {
        raiseError(StringView(u8"-sidebarWidth: expected 1..3000"));
    }
    outWidth = (u16)(width);
}

// The two shapes the layered window's options come in. Named by the
// option they are asked for, so the error says which one was wrong.
void OptionsParser::getPercent(const char* name, u8& outPercent) {
    StringView value;
    long percent = 0;
    if (!get(name, value) || !parseNumber(value, percent) || percent < 0 || percent > 100) {
        raiseError(StringView(u8"-"), StringView(name), StringView(u8": expected 0..100"));
    }
    outPercent = (u8)(percent);
}

void OptionsParser::getPoints(const char* name, u16& outPoints) {
    StringView value;
    long points = 0;
    if (!get(name, value) || !parseNumber(value, points) || points < 0 || points > 100) {
        raiseError(StringView(u8"-"), StringView(name), StringView(u8": expected 0..100"));
    }
    outPoints = (u16)(points);
}

void OptionsParser::getUnicodeWidths(UnicodeWidths& outWidths) {
    StringView value;
    long version = 0;
    if (!get("unicodeWidths", value) || !parseNumber(value, version) || version < 0 || version > 99) {
        raiseError(StringView(u8"-unicodeWidths: expected a Unicode major version, 0 to match the system"));
    }
    if (version == 0) {
        // Match this system's libc: the shells at the pty's far end
        // measure their lines with its wcwidth, and agreeing with it
        // keeps their cursor math on our cells. Probe the two
        // reclassification watersheds - the Unicode 9 emoji batch and
        // the 15.1 trigram batch; a libc that knows the newest one gets
        // the full tables.
        if (wcwidth((wchar_t)(0x2632)) != 2) {
            version = wcwidth((wchar_t)(0x231a)) == 2 ? 15 : 8;
        }
    }
    outWidths = UnicodeWidths((u32)(version));
}

void OptionsParser::getFontsize(u8& outFontsize) {
    StringView value;
    if (const StringView* argument = commandLine.find(StringView(u8"fontsize"))) {
        value = *argument;
    } else if (const char* env = getenv((const char*)(brand.fontSizeEnvironment().data()))) {
        value = StringView(env);
    } else {
        get("fontsize", value);
    }
    long size = 0;
    if (!parseNumber(value, size) || size < 1 || size > 255) {
        StringBuilder message;
        message << StringView(u8"-fontsize/") << brand.fontSizeEnvironment() << StringView(u8": expected integer within 1..255");
        raiseError(StringView(message));
    }
    outFontsize = (u8)(size);
}

void OptionsParser::getSoft(i8& outSoft) {
    StringView value;
    long soft = 0;
    if (!get("soft", value) || !parseNumber(value, soft) || soft < -1 || soft > 100) {
        raiseError(StringView(u8"-soft: expected 0..100"));
    }
    outSoft = (i8)(soft);
}

void OptionsParser::getGeometry(u16& outCols, u16& outRows) {
    StringView value;
    get("geometry", value);
    StringView colsText;
    StringView rowsText;
    long cols = 0;
    long rows = 0;
    const bool valid = value.split('x', colsText, rowsText) && parseNumber(colsText, cols) && parseNumber(rowsText, rows);
    if (!valid || cols < 1 || cols > UINT16_MAX || rows < 1 || rows > UINT16_MAX) {
        raiseError(StringView(u8"-geometry: expected format <COLS>x<ROWS>"));
    }
    outCols = (u16)(cols);
    outRows = (u16)(rows);
}

void OptionsParser::getQuickGeometry(plt::QuickGeometry& outGeometry) {
    StringView value;
    get("quickGeometry", value);
    if (!parseQuickGeometry(value, outGeometry)) {
        raiseError(StringView(u8"-quickGeometry: expected format <W>x<H>+<X>+<Y>, each a positive pixel count or a percent 0..100 (X/Y may be 0)"));
    }
}

namespace {

    static u8 convHexDigit(const char* name, const char ch) {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }

        raiseError(StringView(u8"-"), StringView(name), StringView(u8": illegal hex digit; expected hex RGB color"));
    }

    static void convColor(const char* name, StringView option, Color& outColor) {
        const char* value = (const char*)(option.data());
        size_t length = option.length();
        if (length != 0 && value[0] == '#') {
            ++value;
            --length;
        }
        switch (length) {
            case 3:
                outColor.red = 17 * convHexDigit(name, value[0]);
                outColor.green = 17 * convHexDigit(name, value[1]);
                outColor.blue = 17 * convHexDigit(name, value[2]);
                break;
            case 6:
                outColor.red = (convHexDigit(name, value[0]) << 4) + convHexDigit(name, value[1]);
                outColor.green = (convHexDigit(name, value[2]) << 4) + convHexDigit(name, value[3]);
                outColor.blue = (convHexDigit(name, value[4]) << 4) + convHexDigit(name, value[5]);
                break;
            default:
                raiseError(StringView(u8"-"), StringView(name), StringView(u8": expected hex RGB color"));
        }
    }

    [[noreturn]] static void reportStartupError(StringView message) {
        sysO << StringView(u8"Error: ") << message << StringView(u8"!\nTry -help for usage options.") << endL;
        exit(-1);
    }

}

OptionsParser::OptionsParser(ObjPool& owner, Brand& brand_, char** argv, int argc, OptionsLoad load_, bool desktopLaunch_)
    : pool(owner)
    , brand(brand_)
    , commandLine(&owner)
    , configFile(&owner)
    , load(load_)
    , desktopLaunch(desktopLaunch_)
{
    {
        Vector<StringView> names;
        for (const auto& option : optionsTable) {
            names.pushBack(StringView(option.option));
        }
        optionTrie = Darts::create(owner, names.data(), names.length());
        names.clear();
        for (const auto& resource : resourceTable) {
            names.pushBack(StringView(resource.resource));
        }
        resourceTrie = Darts::create(owner, names.data(), names.length());
    }
    vt.brandName = brand.displayName();
    initialize(&argc, argv);
    parse();
    if (vt.verbose) {
        printVersion();
    }
}

Options* Options::create(ObjPool& pool, Brand& brand, char** argv, int argc, OptionsLoad load, bool desktopLaunch) {
    return pool.make<OptionsParser>(pool, brand, argv, argc, load, desktopLaunch);
}

void OptionsParser::initialize(int* argc, char** argv) {
    int output = 1;

    for (int input = 1; input < *argc; ++input) {
        const char* argument = argv[input];
        if ((argument[0] != '-' && argument[0] != '+') || argument[1] == '\0') {
            argv[output++] = argv[input];
            continue;
        }

        const bool enabled = argument[0] == '-';
        const char* name = argument + 1;

        if (StringView(name) == StringView(u8"e")) {
            while (input < *argc) {
                argv[output++] = argv[input++];
            }
            break;
        }

        const OptionDesc* option = findOption(name);
        if (option == nullptr) {
            if (!isAdvancedOption(StringView(name))) {
                raiseError(StringView(u8"unknown option: "), StringView(argument));
            }

            if (input + 1 >= *argc) {
                raiseError(StringView(argument), StringView(u8": missing value"));
            }
            commandLine.insert(StringView(name), pool.intern(StringView(argv[++input])));
            continue;
        }

        switch (option->parseType) {
            case OptionKind::NoArg:
                commandLine.insert(StringView(option->option), StringView(enabled ? option->implValue : "false"));
                break;
            case OptionKind::SepArg: {
                // A form hint and not merely a complaint. -backgroundBlur
                // grew a value, and every config and every finger that
                // still spells it as a flag arrives in one of these two
                // branches; naming the shape - and a value worth typing -
                // is what turns the refusal into an instruction. An option
                // that spells its set out takes that; for the rest the
                // hard default is the example, being always legal.
                const auto valueHint = [option](StringBuilder& hint) {
                    hint << StringView(u8"; -") << StringView(option->option) << StringView(u8" takes a value");
                    if (option->valueNames != nullptr) {
                        hint << StringView(u8": ") << StringView(option->valueNames);
                    } else if (option->hardDefault != nullptr) {
                        hint << StringView(u8", as in -") << StringView(option->option) << StringView(u8" ") << StringView(option->hardDefault);
                    }
                };
                if (!enabled) {
                    StringBuilder hint;
                    valueHint(hint);
                    raiseError(StringView(argument), StringView(u8": '+' is invalid here"), StringView(hint));
                }
                if (input + 1 >= *argc) {
                    StringBuilder hint;
                    valueHint(hint);
                    raiseError(StringView(argument), StringView(u8": missing value"), StringView(hint));
                }
                const StringView value = pool.intern(StringView(argv[++input]));
                commandLine.insert(StringView(option->option), value);
                if (StringView(option->option) == StringView(u8"font")) {
                    fontnames.pushBack(value);
                }
                if (StringView(option->option) == StringView(u8"remap")) {
                    remaps.pushBack(value);
                }
                if (StringView(option->option) == StringView(u8"uriScheme")) {
                    uriSchemes.pushBack(value);
                }
                break;
            }
            case OptionKind::SkipLine:
                break;
        }
    }

    *argc = output;
    argv[output] = nullptr;

    try {
        loadConfigFile();
    } catch (Exception& error) {
        if (load == OptionsLoad::Startup) {
            reportStartupError(error.description());
        }
        throw;
    }
}

bool OptionsParser::getBool(const char* name, bool defaultValue) {
    StringView option;
    if (!get(name, option)) {
        return defaultValue;
    }
    if (option == StringView(u8"true")) {
        return true;
    }
    if (option == StringView(u8"false")) {
        return false;
    }
    raiseError(StringView(u8"-"), StringView(name), StringView(u8": expected true or false"));
}

void OptionsParser::getBackdropMode(const char* name, BackdropMode& out) {
    StringView option;
    // There is no "no value" case to answer here. When neither the command
    // line nor the config names this option, get() hands back the table's
    // hard default, and this option's is the legal name "off" - so it
    // always comes back with something to read. The early exit that used
    // to stand here could not run. Should the hard default ever go null,
    // the empty name falls to the refusal at the end of this function
    // instead of passing quietly as Off, which is the louder of the two.
    (void)(get(name, option));
    // 'true' and 'false' are what every config written while this was a
    // flag still carries. Dropping them would turn a working config into
    // a refusal to start - the one outcome the field's own comment block
    // exists to avoid - so they stay as aliases of the two modes a flag
    // could express.
    if (option == StringView(u8"off") || option == StringView(u8"false")) {
        out = BackdropMode::Off;
        return;
    }
    if (option == StringView(u8"blur") || option == StringView(u8"true")) {
        out = BackdropMode::Blur;
        return;
    }
    if (option == StringView(u8"glass")) {
        out = BackdropMode::Glass;
        return;
    }
    raiseError(StringView(u8"-"), StringView(name), StringView(u8": expected off, blur or glass"));
}

void OptionsParser::getColor(const char* name, Color& outColor) {
    StringView option;
    if (!get(name, option)) {
        raiseError(StringView(u8"-"), StringView(name), StringView(u8": missing value"));
    }
    convColor(name, option, outColor);
}

int OptionsParser::getInteger(const char* name, int min, int max) {
    StringView option;
    if (!get(name, option)) {
        return min;
    }

    long result = 0;
    if (!parseNumber(option, result)) {
        raiseError(StringView(u8"-"), StringView(name), StringView(u8": expected integer"));
    }
    return stl::min(stl::max((long)(min), result), (long)(max));
}

void OptionsParser::handlePrintOpts() {
    if (getBool("version")) {
        printVersion();
        exit(0);
    }
    if (getBool("help")) {
        printUsage();
        exit(0);
    }
    if (getBool("listres")) {
        printResources();
        exit(0);
    }
    if (getBool("listColorSchemes")) {
        printColorSchemes();
        exit(0);
    }
    if (getBool("printConfig")) {
        printConfig();
        exit(0);
    }
}

void OptionsParser::parse() {
    handlePrintOpts();
    try {
        getBorder(border);
        getBackgroundOpacity(backgroundOpacity);
        getPaneDividerWidth(paneDividerWidth);
        getSaveLines(vt.saveLines);
        getUnicodeWidths(vt.widths);
        if (fontnames.empty()) {
            fontnames.append(configFonts.data(), configFonts.length());
        }
        if (fontnames.empty()) {
            StringView fallback;
            get("font", fallback);
            fontnames.pushBack(fallback);
        }
        symbolFonts.append(configSymbolFonts.data(), configSymbolFonts.length());
        paletteApps.append(configApps.data(), configApps.length());
        paletteEnvs.append(configEnvs.data(), configEnvs.length());
        if (remaps.empty()) {
            remaps.append(configRemaps.data(), configRemaps.length());
        }
        if (uriSchemes.empty()) {
            uriSchemes.append(configUriSchemes.data(), configUriSchemes.length());
        }
        if (uriSchemes.empty()) {
            // Schemes with a handler on any sane desktop. A configured
            // list replaces this outright.
            //
            // The list lives here rather than in the table's hardDefault
            // column because this option is list-shaped: get() hands
            // back one scalar, and a hard default of "http https file
            // mailto gemini" would arrive as a single scheme with spaces
            // in it. The help text names the same five, and the
            // example configs carry them commented out - keep the three
            // in step.
            uriSchemes.pushBack(StringView(u8"http"));
            uriSchemes.pushBack(StringView(u8"https"));
            uriSchemes.pushBack(StringView(u8"file"));
            uriSchemes.pushBack(StringView(u8"mailto"));
            uriSchemes.pushBack(StringView(u8"gemini"));
        }
        {
            // The trie is queried with a lowercased probe, so fold the
            // configured spellings once here.
            Vector<StringView> folded;
            for (const StringView scheme : uriSchemes) {
                u8* bytes = (u8*)(pool.allocate(scheme.length() + 1));
                for (size_t index = 0; index < scheme.length(); ++index) {
                    const u8 byte = scheme[index];
                    bytes[index] = byte >= 'A' && byte <= 'Z' ? (u8)(byte + ('a' - 'A')) : byte;
                }
                bytes[scheme.length()] = '\0';
                folded.pushBack(StringView(bytes, scheme.length()));
            }
            uriSchemeTrie = Darts::create(pool, folded.data(), folded.length());
        }
        getFontsize(fontsize);
        getSoft(soft);
        getGeometry(nCols, nRows);
        optical = getBool("optical");
        vulkanInfo = getBool("vulkanInfo");
        vulkanBlit = getBool("vulkanBlit");
        if (!get("shell", shell)) {
            if (const char* env = getenv("SHELL")) {
                shell = pool.intern(StringView(env));
            }
        }
        if (shell.empty()) {
            shell = StringView(u8"bash");
        }
        // Any string is a path and empty is "no directory named", the
        // same shape as -shell above; whether it can be entered is the
        // child's finding, at spawn, and never fatal.
        get("directory", directory);
        // A path like -directory; unset, bookmarks.toml beside the
        // config (defaultBookmarksPath(), bookmarks.h).
        get("bookmarksFile", bookmarksFile);
        get("cloneDirectory", cloneDirectory);
        get("teleportLogin", teleportLogin);
        get("title", vt.title, &titleSource);
        StringView titleFallback;
        get("titleFallback", titleFallback);
        if (titleFallback == StringView(u8"process")) {
            titleFallbackProcess = true;
        } else if (titleFallback == StringView(u8"none")) {
            titleFallbackProcess = false;
        } else {
            raiseError(StringView(u8"-titleFallback: expected process or none"));
        }
        get("dump", vt.dump);
        get("debug", debugTrace);
        OptionSource schemeSource = OptionSource::NONE;
        StringView schemeName;
        get("colorScheme", schemeName, &schemeSource);
        const TerminalColorScheme* scheme = TerminalColorScheme::find(schemeName);
        if (scheme == nullptr) {
            raiseError(StringView(u8"-colorScheme: unknown scheme: "), schemeName, StringView(u8"; use -listColorSchemes"));
        }
        vt.fg = scheme->foregroundColor();
        vt.bg = scheme->backgroundColor();
        vt.palette = scheme->ansiPalette();
        auto applyColorOption = [&](const char* name, Color& color) {
            OptionSource source = OptionSource::NONE;
            StringView value;
            get(name, value, &source);
            if ((int)(source) >= (int)(schemeSource) && source > OptionSource::HardDefault) {
                getColor(name, color);
            }
        };
        applyColorOption("fg", vt.fg);
        applyColorOption("bg", vt.bg);
        static const char* const paletteNames[] = {
            "color0",
            "color1",
            "color2",
            "color3",
            "color4",
            "color5",
            "color6",
            "color7",
            "color8",
            "color9",
            "color10",
            "color11",
            "color12",
            "color13",
            "color14",
            "color15",
        };
        for (size_t index = 0; index < 16; ++index) {
            applyColorOption(paletteNames[index], vt.palette[index]);
        }
        rv = getBool("rv");
        if (rv) {
            xchg(vt.fg, vt.bg);
        }
        StringView cursor;
        if (get("cr", cursor)) {
            convColor("cr", cursor, vt.cr);
        } else {
            vt.cr = vt.fg;
        }
        // T8. Not cr's shape any more: the seam carries a hard default
        // of its own, so it is the same colour under every scheme rather
        // than the palette's bright black under each. A seam is a
        // deliberate accent - it has to be found by the eye and aimed at
        // by the mouse - and a per-scheme grey is exactly the thing that
        // disappears into some of them.
        //
        // An explicit value still wins, and needs no branch to do so:
        // get() reaches the table's hard default only after the command
        // line and the config file have both missed.
        StringView divider;
        (void)(get("paneDividerColor", divider));
        convColor("paneDividerColor", divider, paneDividerColor);
        // C10. Same shape as the divider above, with one difference that
        // decides the whole option: there is no default to resolve here.
        //
        // The panel's own default is six percent of the foreground mixed
        // into the background, and that mix is done by AppKit, in sRGB,
        // by NSColor. Computing it here in integers would land a byte or
        // two off it - which is to say the default would change, on a
        // fork whose upstream has no such option at all. So the flag
        // says whether anyone asked, and an unasked sidebar takes the
        // path it took before this option existed, unchanged.
        StringView sidebar;
        sidebarColorSet = get("sidebarColor", sidebar);
        if (sidebarColorSet) {
            convColor("sidebarColor", sidebar, sidebarColor);
        }
        vt.altScrollMode = getBool("altScroll");
        naturalEditing = getBool("naturalEditing");
        vt.promptEditor = getBool("promptEditor");
        shellIntegration = getBool("shellIntegration");
        vt.altSendsEscape = getBool("altSendsEscape");
        vt.autoCopyMode = getBool("autoCopy");
        vt.allowOsc52Read = getBool("allowOsc52Read");
        vt.allowWindowOps = getBool("allowWindowOps");
        StringView osc52Select;
        get("osc52Select", osc52Select);
        if (osc52Select != StringView(u8"primary") && osc52Select != StringView(u8"clipboard")) {
            raiseError(StringView(u8"-osc52Select: expected primary or clipboard"));
        }
        vt.osc52SelectClipboard = osc52Select == StringView(u8"clipboard");
        vt.boldColors = getBool("boldColors");
        vt.kittyCtrlBaseLayout = getBool("kittyCtrlBaseLayout");
        noDecorations = getBool("no-decorations");
        // A desktop launch has no shell environment to inherit, so the
        // profile files that build PATH (Homebrew's lives in .zprofile) only
        // run for a login shell. An explicit login setting still wins.
        StringView loginOption;
        OptionSource loginSource = OptionSource::NONE;
        get("login", loginOption, &loginSource);
        login = getBool("login") || (loginSource == OptionSource::HardDefault && desktopLaunch);
        maximized = getBool("maximized");
        fullscreen = getBool("fullscreen");
        quick = getBool("quick");
        {
            StringView hotkey;
            get("quickHotkey", hotkey);
            if (hotkey.empty()) {
                raiseError(StringView(u8"-quickHotkey: expected a non-empty chord"));
            }
            quickHotkey = hotkey;
        }
        getQuickGeometry(quickGeometry);
        // No format to validate here - any non-empty string is a path -
        // and no error on empty: that is simply "no companion", the
        // same shape as -dump/-shell/-title above.
        get("quickCompanion", quickCompanion);
        getQuickCornerRadius(quickCornerRadius);
        quickRememberFrame = getBool("quickRememberFrame");
        // Same shape as quickCompanion above: any string is a chord, empty
        // means disabled, chord grammar is validated where it is parsed.
        get("quickFullscreenHotkey", quickFullscreenHotkey);
        // Two named placements rather than a boolean feature switch: the
        // question a reader has is "where do the tabs live", and "top"
        // is an answer where "false" was a riddle. Same shape as
        // osc52Select above - the names are checked here and stored as
        // the one bit the chrome modules actually branch on.
        StringView tabBar;
        get("tabBar", tabBar);
        if (tabBar != StringView(u8"top") && tabBar != StringView(u8"sidebar")) {
            raiseError(StringView(u8"-tabBar: expected top or sidebar"));
        }
        tabs = getBool("tabs");
        // No tabs, no list of them: the sidebar, its chord and the window
        // chrome drawn around it all hang off this one bit.
        sidebarTabs = tabs && tabBar == StringView(u8"sidebar");
        getSidebarWidth(sidebarWidth);
        getSidebarTabTint(sidebarTabTint);
        layeredWindow = getBool("layeredWindow");
        getPercent("sidebarOpacity", sidebarOpacity);
        // Same shape as sidebarColor above: unset is an absence, and the
        // highlight then follows the foreground through a theme change.
        StringView tabColor;
        sidebarTabColorSet = get("sidebarTabColor", tabColor);
        if (sidebarTabColorSet) {
            convColor("sidebarTabColor", tabColor, sidebarTabColor);
        }
        getPercent("sidebarTabOpacity", sidebarTabOpacity);
        getPoints("panelGap", panelGap);
        getPoints("panelRadius", panelRadius);
        autoHideChrome = getBool("autoHideChrome");
        panes = getBool("panes");
        showWraps = getBool("showWraps");
        cursorKeepSelectionFg = getBool("cursorKeepSelectionFg");
        vt.verbose = getBool("verbose");
        getBackdropMode("backgroundBlur", backgroundBlur);
        // F10. An opaque background hides the backdrop completely,
        // whichever mode asked for it, so none is created - the same
        // shape README.md
        // gives for the quick-window options that do not apply. What
        // the acceptance found missing was not the behaviour but the
        // silence: the user turns the backdrop on, sees nothing change,
        // and has nothing to go on. One line, and it names the option to
        // reach for rather than merely reporting that something was
        // ignored.
        //
        // A warning and not an error, for the reason backgroundOpacity's
        // own comment gives: that option is reloadable, and a config
        // legal at one of its values and fatal at another turns a
        // one-line edit into a refusal to start.
        if (backgroundBlur != BackdropMode::Off && backgroundOpacity == 100) {
            sysE << brand.identifier() << StringView(u8": -backgroundBlur has nothing to show while -backgroundOpacity is 100; lower -backgroundOpacity to let the desktop show through") << endL;
        }
        transparentTitlebar = getBool("transparentTitlebar");
        vt.modifyOtherKeys = getInteger("modifyOtherKeys", 0, 2);
    } catch (Exception& error) {
        if (load == OptionsLoad::Startup) {
            reportStartupError(error.description());
        }
        throw;
    }
}

void OptionsParser::printVersion() const {
    sysO << brand.displayName() << StringView(u8" " SHITTY_VERSION "\nCopyright (C) 2026 ") << brand.displayName() << StringView(u8" team") << endL;
}

// T8. The brand's own example config, embedded at build time and
// written back out unchanged.
//
// Generating this from optionsTable instead was the other candidate and
// is the wrong one, because the table is not where the effective
// defaults are. bg and fg carry hard defaults of "#000" and "#fff" that
// nothing ever uses - a named colorScheme outranks them, and one is
// always in force; cr and the sixteen palette slots have no hard default
// at all and follow the scheme too; uriScheme is a list, which the
// column cannot hold. A generator reading the table would print four of
// those wrong and say nothing.
//
// The example config, by contrast, is the artifact that already has to
// be right: tst/test_config.py starts the terminal on it, compares it
// against -help, and holds the two brands' copies to each other. Making
// it the source means -printConfig cannot drift from what ships, and
// that a file the user writes with it is the file the tests exercise.
void OptionsParser::printConfig() const {
    // Empty only for the generic brand, which ships no config file and
    // is never the one running a command line. Writing nothing is then
    // the honest answer rather than someone else's brand.
    const StringView config = brand.exampleConfig();
    OutBuf output(stdoutStream());
    output << config;
}

void OptionsParser::printUsage() const {
    printVersion();
    OutBuf output(stdoutStream());
    output << StringView(u8"Usage:\n  ") << brand.executableName() << StringView(u8" [-option ...] [shell]\n\nOptions:\n");
    size_t maxWidth = 0;
    for (const auto& option : optionsTable) {
        maxWidth = max(maxWidth, StringView(option.option).length());
    }
    for (const auto& option : optionsTable) {
        const StringView name(option.option);
        output << StringView(u8"  -") << name;
        writeSpaces(output, maxWidth + 3 - name.length());
        output << StringView(option.helpDescr);
        StringView hardDefault;
        if (name == StringView(u8"title")) {
            hardDefault = brand.displayName();
        } else if (const char* own = brand.defaultFor(name)) {
            hardDefault = StringView(own);
        } else if (option.hardDefault != nullptr) {
            hardDefault = StringView(option.hardDefault);
        }
        // T8. Boolean options print their default too, which they did
        // not before. The old silence carried no information while every
        // NoArg default was false; now that five of them are true, "is
        // -panes already on?" is a question the listing has to answer,
        // and +panes is the spelling that turns it off.
        //
        // The exception is the NoArg options that are actions rather
        // than settings - -help, -version, -listres and their kin. They
        // are cliOnly, never appear in a config file, and "(default:
        // false)" beside -help would be noise about a switch nobody
        // holds.
        const bool actionFlag = option.parseType == OptionKind::NoArg && option.cliOnly;
        if (!hardDefault.empty() && !actionFlag) {
            output << StringView(u8" (default: ") << hardDefault << StringView(u8")");
        }
        output << endL;
    }
    output << endL;
}

void OptionsParser::printResources() const {
    printVersion();
    OutBuf output(stdoutStream());
    output << StringView(u8"Advanced options:\n");
    size_t maxWidth = 0;
    for (const auto& resource : resourceTable) {
        if (resource.hidden) {
            continue;
        }
        maxWidth = max(maxWidth, StringView(resource.resource).length());
    }
    for (const auto& resource : resourceTable) {
        if (resource.hidden) {
            continue;
        }
        const StringView name(resource.resource);
        output << StringView(u8"  -") << name;
        writeSpaces(output, maxWidth + 3 - name.length());
        output << StringView(resource.helpDescr);
        if (resource.hardDefault != nullptr) {
            output << StringView(u8" (default: ") << StringView(resource.hardDefault) << StringView(u8")");
        }
        output << endL;
    }
    output << endL;
}

void OptionsParser::printColorSchemes() const {
    OutBuf output(stdoutStream());
    for (size_t index = 0; index < TerminalColorScheme::builtinCount(); ++index) {
        output << StringView(TerminalColorScheme::builtins()[index].name) << endL;
    }
    for (size_t index = 0; index < TerminalColorScheme::count(); ++index) {
        output << StringView(TerminalColorScheme::all()[index].name) << endL;
    }
}

bool Options::uriSchemeAllowed(StringView scheme) const {
    if (uriSchemeTrie == nullptr) {
        return false;
    }
    u8 folded[128];
    if (scheme.length() > sizeof(folded)) {
        return false;
    }
    for (size_t index = 0; index < scheme.length(); ++index) {
        const u8 byte = scheme[index];
        folded[index] = byte >= 'A' && byte <= 'Z' ? (u8)(byte + ('a' - 'A')) : byte;
    }
    return uriSchemeTrie->find(StringView(folded, scheme.length())) != Darts::missing;
}
