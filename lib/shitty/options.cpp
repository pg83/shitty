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
    };

    struct ResourceDesc {
        const char* resource;
        const char* hardDefault;
        const char* helpDescr;
        // Working options that stay out of every help listing.
        bool hidden = false;
    };

    // macOS sinks the active tab and the terminal into one well whose rim
    // lives in the border: three points of material with the cut and the
    // lit edge, the lip on the terminal's own side, two points of room.
    #if defined(__APPLE__)
        static const char* const defaultBorder = "6";
    #else
        static const char* const defaultBorder = "2";
    #endif

    static const OptionDesc optionsTable[] = {

        {"altScroll", OptionKind::NoArg, "true", "false", "Alternate scroll mode"},
        {"autoCopy", OptionKind::NoArg, "true", "false", "Sync primary to clipboard"},
        {"bg", OptionKind::SepArg, nullptr, "#000", "Background color"},
        {"boldColors", OptionKind::NoArg, "true", "false", "Brighten bold text's palette colors"},
        {"border", OptionKind::SepArg, nullptr, defaultBorder, "Border width in pixels"},
        {"config", OptionKind::SepArg, nullptr, nullptr, "Path to the TOML config file", true},
        {"colorScheme", OptionKind::SepArg, nullptr, "default", "Named terminal color scheme"},
        {"cr", OptionKind::SepArg, nullptr, nullptr, "Cursor color"},
        {"cursorKeepSelectionFg", OptionKind::NoArg, "true", "false", "Keep selected or reverse-video text color under the block cursor"},
        {"debug", OptionKind::SepArg, nullptr, nullptr, "Append window, font and grid diagnostics to this file", true},
        {"dump", OptionKind::SepArg, nullptr, nullptr, "Dump raw PTY input to file"},
        {"fg", OptionKind::SepArg, nullptr, "#fff", "Foreground color"},
        {"font", OptionKind::SepArg, nullptr, "monospace", "Font to use; repeat for fallbacks"},
        {"fontsize", OptionKind::SepArg, nullptr, "16", "Font size"},
        {"fullscreen", OptionKind::NoArg, "true", "false", "Start with the window fullscreen"},
        {"soft", OptionKind::SepArg, nullptr, "-1", "Unhinted subpixel rendering; 0..100 scales the stem darkening"},
        {"geometry", OptionKind::SepArg, nullptr, "80x24", "Terminal size in chars"},
        {"kittyCtrlBaseLayout", OptionKind::NoArg, "true", "false", "Report the ASCII base key as the Kitty primary under Ctrl"},
        {"vulkanInfo", OptionKind::NoArg, "true", "false", "Print Vulkan information", true},
        {"vulkanBlit", OptionKind::NoArg, "true", "false", "Present through the offscreen blit path", true},
        {"help", OptionKind::NoArg, "true", "false", "Print usage listing and quit", true},
        {"listres", OptionKind::NoArg, "true", "false", "Print advanced option listing and quit", true},
        {"listColorSchemes", OptionKind::NoArg, "true", "false", "Print terminal color scheme names and quit", true},
        {"login", OptionKind::NoArg, "true", "false", "Start shell as a login shell"},
        {"maximized", OptionKind::NoArg, "true", "false", "Start with the window maximized"},
        {"naturalEditing", OptionKind::NoArg, "true", "false", "Bind the macOS natural text editing chords"},
        {"no-decorations", OptionKind::NoArg, "true", "false", "Disable window decorations"},
        {"optical", OptionKind::NoArg, "true", "false", "Optically space simple Latin and Cyrillic runs"},
        {"remap", OptionKind::SepArg, nullptr, nullptr, "Rewrite a key chord, from=to; repeat for more"},
        {"rv", OptionKind::NoArg, "true", "false", "Reverse video"},
        {"saveLines", OptionKind::SepArg, nullptr, "500", "Lines of scrollback history"},
        {"shell", OptionKind::SepArg, nullptr, nullptr, "Shell program to run"},
        {"showWraps", OptionKind::NoArg, "true", "false", "Show wrap marks at right margin"},
        {"title", OptionKind::SepArg, nullptr, nullptr, "Window title"},
        {"titleFallback", OptionKind::SepArg, nullptr, "process", "Title when the app sets none: process or none"},
        {"unicodeWidths", OptionKind::SepArg, nullptr, "0", "Unicode version for character widths; 0 matches the system libc"},
        {"uriScheme", OptionKind::SepArg, nullptr, nullptr, "Open a plain URI with this scheme; repeat for more, default http https file"},
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
        OptionsParser(ObjPool& owner, Brand& brand, char** argv, int argc, OptionsLoad load);

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
        void getSaveLines(u16& outSaveLines);
        void getUnicodeWidths(UnicodeWidths& outWidths);
        void getFontsize(u8& outFontsize);
        void getSoft(i8& outSoft);
        void getGeometry(u16& outCols, u16& outRows);
        void printVersion() const;
        void printUsage() const;
        void printResources() const;
        void printColorSchemes() const;
        bool getBool(const char* name, bool defaultValue = false);
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
        Vector<StringView> configRemaps;
        Vector<StringView> configUriSchemes;
        OptionsLoad load;
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

bool ConfigSink::tomlTable(const StringView* segments, size_t count, bool array) {
    finishSymbolEntry();
    symbolOpen = false;
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

void OptionsParser::getSaveLines(u16& outSaveLines) {
    StringView value;
    long lines = 0;
    if (!get("saveLines", value) || !parseNumber(value, lines) || lines < 0 || lines > 50000) {
        raiseError(StringView(u8"-saveLines: expected unsigned, max. 50000"));
    }
    outSaveLines = (u16)(lines);
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

OptionsParser::OptionsParser(ObjPool& owner, Brand& brand_, char** argv, int argc, OptionsLoad load_)
    : pool(owner)
    , brand(brand_)
    , commandLine(&owner)
    , configFile(&owner)
    , load(load_)
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

Options* Options::create(ObjPool& pool, Brand& brand, char** argv, int argc, OptionsLoad load) {
    return pool.make<OptionsParser>(pool, brand, argv, argc, load);
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
                if (!enabled) {
                    raiseError(StringView(argument), StringView(u8": '+' is invalid here"));
                }
                if (input + 1 >= *argc) {
                    raiseError(StringView(argument), StringView(u8": missing value"));
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
}

void OptionsParser::parse() {
    handlePrintOpts();
    try {
        getBorder(border);
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
        if (remaps.empty()) {
            remaps.append(configRemaps.data(), configRemaps.length());
        }
        if (uriSchemes.empty()) {
            uriSchemes.append(configUriSchemes.data(), configUriSchemes.length());
        }
        if (uriSchemes.empty()) {
            // The conservative default: schemes with a handler on any sane
            // desktop. A configured list replaces this outright.
            uriSchemes.pushBack(StringView(u8"http"));
            uriSchemes.pushBack(StringView(u8"https"));
            uriSchemes.pushBack(StringView(u8"file"));
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
        vt.altScrollMode = getBool("altScroll");
        naturalEditing = getBool("naturalEditing");
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
        login = getBool("login");
        maximized = getBool("maximized");
        fullscreen = getBool("fullscreen");
        showWraps = getBool("showWraps");
        cursorKeepSelectionFg = getBool("cursorKeepSelectionFg");
        vt.verbose = getBool("verbose");
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
        } else if (option.hardDefault != nullptr) {
            hardDefault = StringView(option.hardDefault);
        }
        if (!hardDefault.empty() && option.parseType != OptionKind::NoArg) {
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
