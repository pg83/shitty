/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "options.h"

#include "brand.h"

#include <std/sys/fd.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/tst/ut.h>

#include <cstring>
#include <stdlib.h>
#include <unistd.h>

using namespace stl;

namespace {
    // Writes text to a fresh temp file and hands back its path.
    static Buffer writeTempConfig(StringView text) {
        const char* tmp = getenv("TMPDIR");
        Buffer path;
        if (tmp != nullptr && tmp[0] != '\0') {
            path.append(tmp, strlen(tmp));
        } else {
            path.append("/tmp", 4);
        }
        const StringView suffix = StringView(u8"/options-ut-XXXXXX");
        path.append(suffix.data(), suffix.length());
        const int raw = mkstemp(path.cStr());
        STD_INSIST(raw >= 0);
        FD file(raw);
        file.write(text.data(), text.length());
        close(raw);
        return path;
    }
}

STD_TEST_SUITE(Options) {
    STD_TEST(CursorKeepSelectionFgFlag) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char enabled[] = "-cursorKeepSelectionFg";
        char disabled[] = "+cursorKeepSelectionFg";
        char* defaults[] = {program, nullptr};
        char* on[] = {program, enabled, nullptr};
        char* off[] = {program, enabled, disabled, nullptr};
        STD_INSIST(!Options::create(*pool, *Brand::generic(), defaults, 1)->cursorKeepSelectionFg);
        STD_INSIST(Options::create(*pool, *Brand::generic(), on, 2)->cursorKeepSelectionFg);
        STD_INSIST(!Options::create(*pool, *Brand::generic(), off, 3)->cursorKeepSelectionFg);
    }

    STD_TEST(SymbolFontTablesParseFromConfig) {
        auto pool = ObjPool::fromMemory();
        Buffer config = writeTempConfig(StringView(
            u8"font = [\"main\"]\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"pictograms\"\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"arrows\"\n"
            u8"first = 0x2190\n"
            u8"last = 0x21FF\n"
        ));
        char program[] = "st";
        char configOption[] = "-config";
        char* argv[] = {program, configOption, config.cStr(), nullptr};

        Options* const options = Options::create(*pool, *Brand::generic(), argv, 3);

        // A rangeless entry claims the three Private Use Areas.
        STD_INSIST(options->symbolFonts.length() == 4);
        STD_INSIST(options->symbolFonts[0].font == StringView(u8"pictograms"));
        STD_INSIST(options->symbolFonts[0].first == 0xE000);
        STD_INSIST(options->symbolFonts[0].last == 0xF8FF);
        STD_INSIST(options->symbolFonts[1].first == 0xF0000);
        STD_INSIST(options->symbolFonts[1].last == 0xFFFFD);
        STD_INSIST(options->symbolFonts[2].first == 0x100000);
        STD_INSIST(options->symbolFonts[2].last == 0x10FFFD);
        STD_INSIST(options->symbolFonts[3].font == StringView(u8"arrows"));
        STD_INSIST(options->symbolFonts[3].first == 0x2190);
        STD_INSIST(options->symbolFonts[3].last == 0x21FF);
        unlink(config.cStr());
    }

    STD_TEST(SymbolFontDropsHalfUnderstoodEntries) {
        auto pool = ObjPool::fromMemory();
        Buffer config = writeTempConfig(StringView(
            u8"[[symbolFont]]\n"
            u8"font = \"half\"\n"
            u8"first = 0xE000\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"inverted\"\n"
            u8"first = 10\n"
            u8"last = 5\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"glyph = \"typo\"\n"
            u8"font = \"typoed\"\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = 5\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"first = 65\n"
            u8"last = 90\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"good\"\n"
            u8"first = 65\n"
            u8"last = 90\n"
        ));
        char program[] = "st";
        char configOption[] = "-config";
        char* argv[] = {program, configOption, config.cStr(), nullptr};

        Options* const options = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(options->symbolFonts.length() == 1);
        STD_INSIST(options->symbolFonts[0].font == StringView(u8"good"));
        STD_INSIST(options->symbolFonts[0].first == 65);
        STD_INSIST(options->symbolFonts[0].last == 90);
        unlink(config.cStr());
    }

    // Every base parseCodepoint accepts, and every malformed shape the
    // sink warns about without aborting the rest of the config.
    STD_TEST(SymbolFontCodepointBasesAndMalformedShapes) {
        auto pool = ObjPool::fromMemory();
        Buffer config = writeTempConfig(StringView(
            u8"[[symbolFont]]\n"
            u8"font = \"octal\"\n"
            u8"first = 0o100\n"
            u8"last = 0o101\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"binary\"\n"
            u8"first = 0b1000001\n"
            u8"last = 0b1000010\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"plus\"\n"
            u8"first = +65\n"
            u8"last = +90\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"upper\"\n"
            u8"first = 0xE000\n"
            u8"last = 0xE0FF\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"grouped\"\n"
            u8"first = 0xE_000\n"
            u8"last = 70\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"overflowing\"\n"
            u8"first = 9999999999\n"
            u8"last = 70\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"listed\"\n"
            u8"first = [65]\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = { name = \"tabled\" }\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"dotted.key = 1\n"
            u8"font = \"dotted\"\n"
            u8"\n"
            u8"[symbolFont]\n"
            u8"font = \"single-table\"\n"
        ));
        char program[] = "st";
        char configOption[] = "-config";
        char* argv[] = {program, configOption, config.cStr(), nullptr};

        Options* const options = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(options->symbolFonts.length() == 4);
        STD_INSIST(options->symbolFonts[0].font == StringView(u8"octal"));
        STD_INSIST(options->symbolFonts[0].first == 0100);
        STD_INSIST(options->symbolFonts[0].last == 0101);
        STD_INSIST(options->symbolFonts[1].font == StringView(u8"binary"));
        STD_INSIST(options->symbolFonts[1].first == 65);
        STD_INSIST(options->symbolFonts[1].last == 66);
        STD_INSIST(options->symbolFonts[2].font == StringView(u8"plus"));
        STD_INSIST(options->symbolFonts[2].first == 65);
        STD_INSIST(options->symbolFonts[2].last == 90);
        STD_INSIST(options->symbolFonts[3].font == StringView(u8"upper"));
        STD_INSIST(options->symbolFonts[3].first == 0xE000);
        STD_INSIST(options->symbolFonts[3].last == 0xE0FF);
        unlink(config.cStr());
    }

    STD_TEST(CreatedInstancesOwnTheirParsedLists) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char font[] = "-font";
        char firstName[] = "first-font";
        char secondName[] = "second-font";
        char* firstArgv[] = {program, config, emptyConfig, font, firstName, nullptr};
        char* secondArgv[] = {program, config, emptyConfig, font, secondName, nullptr};

        Options* const first = Options::create(*pool, *Brand::generic(), firstArgv, 5);
        Options* const second = Options::create(*pool, *Brand::generic(), secondArgv, 5);

        STD_INSIST(first->fontnames.length() == 1);
        STD_INSIST(second->fontnames.length() == 1);
        STD_INSIST(first->fontnames[0] == StringView(u8"first-font"));
        STD_INSIST(second->fontnames[0] == StringView(u8"second-font"));
        STD_INSIST(firstArgv[1] == nullptr);
        STD_INSIST(secondArgv[1] == nullptr);
    }
}
