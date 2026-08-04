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

#include <std/ios/sys.h>
#include <std/str/view.h>

#include <stdlib.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace stl;

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
    };

    struct ResourceDesc {
        const char* resource;
        const char* hardDefault;
        const char* helpDescr;
    };

    const std::vector<OptionDesc> optionsTable = {

        {"altScroll", OptionKind::NoArg, "true", "false", "Alternate scroll mode"},
        {"autoCopy", OptionKind::NoArg, "true", "false", "Sync primary to clipboard"},
        {"bg", OptionKind::SepArg, nullptr, "#000", "Background color"},
        {"boldColors", OptionKind::NoArg, "true", "true", "Enable bright for bold"},
        {"border", OptionKind::SepArg, nullptr, "2", "Border width in pixels"},
        {"cr", OptionKind::SepArg, nullptr, nullptr, "Cursor color"},
        {"dump", OptionKind::SepArg, nullptr, nullptr, "Dump raw PTY input to file"},
        {"fg", OptionKind::SepArg, nullptr, "#fff", "Foreground color"},
        {"font", OptionKind::SepArg, nullptr, "monospace", "Font to use; repeat for fallbacks"},
        {"fontsize", OptionKind::SepArg, nullptr, "16", "Font size"},
        {"geometry", OptionKind::SepArg, nullptr, "80x24", "Terminal size in chars"},
        {"kittyCtrlBaseLayout", OptionKind::NoArg, "true", "false", "Kitty Ctrl base-layout compatibility mode (fix Codex)"},
        {"vulkanInfo", OptionKind::NoArg, "true", "false", "Print Vulkan information"},
        {"help", OptionKind::NoArg, "true", "false", "Print usage listing and quit"},
        {"listres", OptionKind::NoArg, "true", "false", "Print advanced option listing and quit"},
        {"login", OptionKind::NoArg, "true", "false", "Start shell as a login shell"},
        {"rv", OptionKind::NoArg, "true", "false", "Reverse video"},
        {"saveLines", OptionKind::SepArg, nullptr, "500", "Lines of scrollback history"},
        {"shell", OptionKind::SepArg, nullptr, nullptr, "Shell program to run"},
        {"showWraps", OptionKind::NoArg, "true", "false", "Show wrap marks at right margin"},
        {"title", OptionKind::SepArg, nullptr, "Shitty", "Window title"},
        {"verbose", OptionKind::NoArg, "true", "false", "Output info messages"},
        {"version", OptionKind::NoArg, "true", "false", "Print version and quit"},
        {"e", OptionKind::SkipLine, nullptr, nullptr, "Command line to run"},
    };

    const std::vector<ResourceDesc> resourceTable = {

        {"altSendsEscape", "true", "Encode Alt key as ESC prefix"},
        {"modifyOtherKeys", "1", "Key modifier encoding level; 0..2"},
        {"allowOsc52Read", "false", "Allow applications to read clipboard via OSC 52"},
        {"allowWindowOps", "false", "Allow applications to manipulate and query the window"},
        {"osc52Select", "primary", "Selection used by OSC 52 selector s: primary or clipboard"},
        {"color0", "#000000", "Palette color 0"},
        {"color1", "#cd0000", "Palette color 1"},
        {"color2", "#00cd00", "Palette color 2"},
        {"color3", "#cdcd00", "Palette color 3"},
        {"color4", "#0000ee", "Palette color 4"},
        {"color5", "#cd00cd", "Palette color 5"},
        {"color6", "#00cdcd", "Palette color 6"},
        {"color7", "#e5e5e5", "Palette color 7"},
        {"color8", "#7f7f7f", "Palette color 8"},
        {"color9", "#ff0000", "Palette color 9"},
        {"color10", "#00ff00", "Palette color 10"},
        {"color11", "#ffff00", "Palette color 11"},
        {"color12", "#5c5cff", "Palette color 12"},
        {"color13", "#ff00ff", "Palette color 13"},
        {"color14", "#00ffff", "Palette color 14"},
        {"color15", "#ffffff", "Palette color 15"},
    };

    std::map<std::string, std::string> commandLine;
    std::vector<std::string> fontArguments;
    std::vector<const char*> fontPointers;

    void writeSpaces(ZeroCopyOutput& output, size_t count) {
        static constexpr u8 spaces[] = u8"                                ";
        while (count != 0) {
            const size_t chunk = count < sizeof(spaces) - 1 ? count : sizeof(spaces) - 1;
            output.write(spaces, chunk);
            count -= chunk;
        }
    }

    const OptionDesc* findOption(const char* prefix) {
        if (strcmp(prefix, "v") == 0) {
            prefix = "version";
        }

        const OptionDesc* found = nullptr;
        const size_t n = strlen(prefix);

        for (const auto& option : optionsTable) {
            if (strncmp(option.option, prefix, n) != 0) {
                continue;
            }

            if (strlen(option.option) == n) {
                return &option;
            }
            if (found != nullptr) {
                throw std::runtime_error(std::string("ambiguous option: ") + prefix);
            }
            found = &option;
        }
        return found;
    }

    bool isAdvancedOption(const char* name) {
        for (const auto& resource : resourceTable) {
            if (strcmp(resource.resource, name) == 0) {
                return true;
            }
        }
        return false;
    }

    const char* get(const char* name, const char* fallback = nullptr, OptionSource* src = nullptr) {
        auto withSource = [=](const OptionSource source, const char* value) {
            if (src != nullptr) {
                *src = source;
            }
            return value;
        };

        const auto parsed = commandLine.find(name);
        if (parsed != commandLine.end()) {
            return withSource(OptionSource::CmdLine, parsed->second.c_str());
        }

        for (const auto& option : optionsTable) {
            if (strcmp(option.option, name) == 0 && option.hardDefault != nullptr) {
                return withSource(OptionSource::HardDefault, option.hardDefault);
            }
        }

        for (const auto& resource : resourceTable) {
            if (strcmp(resource.resource, name) == 0 && resource.hardDefault != nullptr) {
                return withSource(OptionSource::HardDefault, resource.hardDefault);
            }
        }

        return withSource(OptionSource::NONE, fallback);
    }

    void getBorder(u16& outBorder) {
        const char* option = get("border");
        std::stringstream input(option);
        int border;
        input >> border;
        const bool invalid = input.fail();
        input >> std::ws;
        if (invalid || !input.eof() || border < 0 || border > 3000) {
            throw std::runtime_error("-border: expected unsigned, max. 3000");
        }
        outBorder = border;
    }

    void getSaveLines(u16& outSaveLines) {
        const char* option = get("saveLines");
        std::stringstream input(option);
        int lines;
        input >> lines;
        const bool invalid = input.fail();
        input >> std::ws;
        if (invalid || !input.eof() || lines < 0 || lines > 50000) {
            throw std::runtime_error("-saveLines: expected unsigned, max. 50000");
        }
        outSaveLines = lines;
    }

    void getFontsize(u8& outFontsize) {
        const char* option = nullptr;
        const auto parsed = commandLine.find("fontsize");
        if (parsed != commandLine.end()) {
            option = parsed->second.c_str();
        } else if ((option = getenv("SHITTY_FONT_SIZE")) == nullptr) {
            option = get("fontsize");
        }
        std::stringstream input(option);
        int size;
        input >> size;
        const bool invalid = input.fail();
        input >> std::ws;
        if (invalid || !input.eof() || size < 1 || size > 255) {
            throw std::runtime_error("-fontsize/SHITTY_FONT_SIZE: expected integer within 1..255");
        }
        outFontsize = size;
    }

    void getGeometry(u16& outCols, u16& outRows) {
        const char* option = get("geometry");
        std::stringstream input(option);
        int cols;
        int rows;
        char separator;
        input >> cols >> separator >> rows;
        const bool invalid = input.fail();
        input >> std::ws;
        if (invalid || !input.eof() || separator != 'x' || cols < 1 || cols > UINT16_MAX || rows < 1 || rows > UINT16_MAX) {
            throw std::runtime_error("-geometry: expected format <COLS>x<ROWS>");
        }
        outCols = cols;
        outRows = rows;
    }

    u8 convHexDigit(const char* name, const char ch) {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }

        throw std::runtime_error(std::string("-") + name + ": illegal hex digit; expected hex RGB color");
    }

    void convColor(const char* name, const char* option, Color& outColor) {
        const char* value = option[0] == '#' ? option + 1 : option;
        switch (strlen(value)) {
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
                throw std::runtime_error(std::string("-") + name + ": expected hex RGB color");
        }
    }

}

Options opts;

void Options::initialize(int* argc, char** argv) {
    int output = 1;

    for (int input = 1; input < *argc; ++input) {
        const char* argument = argv[input];
        if ((argument[0] != '-' && argument[0] != '+') || argument[1] == '\0') {
            argv[output++] = argv[input];
            continue;
        }

        const bool enabled = argument[0] == '-';
        const char* name = argument + 1;

        if (strcmp(name, "e") == 0) {
            while (input < *argc) {
                argv[output++] = argv[input++];
            }
            break;
        }

        const OptionDesc* option = findOption(name);
        if (option == nullptr) {
            if (!isAdvancedOption(name)) {
                throw std::runtime_error(std::string("unknown option: ") + argument);
            }

            if (input + 1 >= *argc) {
                throw std::runtime_error(std::string(argument) + ": missing value");
            }
            commandLine[name] = argv[++input];
            continue;
        }

        switch (option->parseType) {
            case OptionKind::NoArg:
                commandLine[option->option] = enabled ? option->implValue : "false";
                break;
            case OptionKind::SepArg:
                if (!enabled) {
                    throw std::runtime_error(std::string(argument) + ": '+' is invalid here");
                }
                if (input + 1 >= *argc) {
                    throw std::runtime_error(std::string(argument) + ": missing value");
                }
                commandLine[option->option] = argv[++input];
                if (strcmp(option->option, "font") == 0) {
                    fontArguments.push_back(argv[input]);
                }
                break;
            case OptionKind::SkipLine:
                break;
        }
    }

    *argc = output;
    argv[output] = nullptr;
}

bool Options::getBool(const char* name, bool defaultValue) {
    const char* option = get(name);
    if (option == nullptr) {
        return defaultValue;
    }
    if (strcmp(option, "true") == 0) {
        return true;
    }
    if (strcmp(option, "false") == 0) {
        return false;
    }
    throw std::runtime_error(std::string("-") + name + ": expected true or false");
}

void Options::getColor(const char* name, Color& outColor) {
    const char* option = get(name);
    if (option == nullptr) {
        throw std::runtime_error(std::string("-") + name + ": missing value");
    }
    convColor(name, option, outColor);
}

int Options::getInteger(const char* name, int min, int max) {
    const char* option = get(name);
    if (option == nullptr) {
        return min;
    }

    std::stringstream input(option);
    int result;
    input >> result;
    const bool invalid = input.fail();
    input >> std::ws;
    if (invalid || !input.eof()) {
        throw std::runtime_error(std::string("-") + name + ": expected integer");
    }
    return std::min(std::max(min, result), max);
}

void Options::handlePrintOpts() {
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
}

void Options::parse() {
    handlePrintOpts();
    try {
        getBorder(border);
        getSaveLines(saveLines);
        if (fontArguments.empty()) {
            fontArguments.push_back(get("font"));
        }
        fontPointers.clear();
        for (const std::string& name : fontArguments) {
            fontPointers.push_back(name.c_str());
        }
        fontnames = fontPointers.data();
        fontnameCount = fontPointers.size();
        getFontsize(fontsize);
        getGeometry(nCols, nRows);
        vulkanInfo = getBool("vulkanInfo");
        shell = get("shell", getenv("SHELL"));
        if (shell == nullptr) {
            shell = "bash";
        }
        title = get("title", nullptr, &titleSource);
        dump = get("dump");
        getColor("fg", fg);
        getColor("bg", bg);
        rv = getBool("rv");
        if (rv) {
            std::swap(fg, bg);
        }
        if (get("cr") != nullptr) {
            getColor("cr", cr);
        } else {
            cr = fg;
        }
        altScrollMode = getBool("altScroll");
        altSendsEscape = getBool("altSendsEscape");
        autoCopyMode = getBool("autoCopy");
        allowOsc52Read = getBool("allowOsc52Read");
        allowWindowOps = getBool("allowWindowOps");
        const std::string osc52Select = get("osc52Select");
        if (osc52Select != "primary" && osc52Select != "clipboard") {
            throw std::runtime_error("-osc52Select: expected primary or clipboard");
        }
        osc52SelectClipboard = osc52Select == "clipboard";
        boldColors = getBool("boldColors");
        kittyCtrlBaseLayout = getBool("kittyCtrlBaseLayout");
        login = getBool("login");
        showWraps = getBool("showWraps");
        verbose = getBool("verbose");
        modifyOtherKeys = getInteger("modifyOtherKeys", 0, 2);
    } catch (const std::exception& error) {
        sysO << StringView(u8"Error: ") << StringView(error.what()) << StringView(u8"!\nTry -help for usage options.") << endL;
        exit(-1);
    }
}

void Options::printVersion() const {
    sysO << StringView(u8"Shitty " SHITTY_VERSION "\nCopyright (C) 2026 Shitty team") << endL;
}

void Options::printUsage() const {
    printVersion();
    OutBuf output(stdoutStream());
    output << StringView(u8"Usage:\n  st [-option ...] [shell]\n\nOptions:\n");
    size_t maxWidth = 0;
    for (const auto& option : optionsTable) {
        maxWidth = std::max(maxWidth, strlen(option.option));
    }
    for (const auto& option : optionsTable) {
        output << StringView(u8"  -") << StringView(option.option);
        writeSpaces(output, maxWidth + 3 - strlen(option.option));
        output << StringView(option.helpDescr);
        if (option.hardDefault != nullptr && option.parseType != OptionKind::NoArg) {
            output << StringView(u8" (default: ") << StringView(option.hardDefault) << StringView(u8")");
        }
        output << endL;
    }
    output << endL;
}

void Options::printResources() const {
    printVersion();
    OutBuf output(stdoutStream());
    output << StringView(u8"Advanced options:\n");
    size_t maxWidth = 0;
    for (const auto& resource : resourceTable) {
        maxWidth = std::max(maxWidth, strlen(resource.resource));
    }
    for (const auto& resource : resourceTable) {
        output << StringView(u8"  -") << StringView(resource.resource);
        writeSpaces(output, maxWidth + 3 - strlen(resource.resource));
        output << StringView(resource.helpDescr);
        if (resource.hardDefault != nullptr) {
            output << StringView(u8" (default: ") << StringView(resource.hardDefault) << StringView(u8")");
        }
        output << endL;
    }
    output << endL;
}
