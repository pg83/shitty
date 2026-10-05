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
#include <std/str/builder.h>
#include <std/sys/throw.h>
#include <std/tst/ut.h>

#include <cstring>
#include <stdlib.h>
#include <unistd.h>

using namespace stl;

namespace {

    // Creates a temp config file with the given TOML body and returns its
    // path, still owned by the StringBuilder so the caller can hand its
    // cStr() straight to argv. Mirrors the mkstemp() pattern already used
    // by std/sys/mem_fd.cpp. The caller must unlink() the path when done.
    void writeTempConfig(StringBuilder& path, StringView text) {
        const char* directory = getenv("TMPDIR");
        path << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/options_ut_config.XXXXXX");
        const int fd = mkstemp(path.cStr());
        STD_INSIST(fd >= 0);
        const ssize_t written = ::write(fd, text.data(), text.length());
        STD_INSIST(written == (ssize_t)(text.length()));
        ::close(fd);
    }

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

    // The command palette's tables: an app's name, command and dir; an
    // env's name, every other key a variable. An entry without what it
    // needs, or with a value that is not a string, is dropped whole, and
    // a symbolFont table after them still reads.
    STD_TEST(PaletteAppAndEnvTablesParseFromConfig) {
        auto pool = ObjPool::fromMemory();
        Buffer config = writeTempConfig(StringView(
            u8"cloneDirectory = \"~/src\"\n"
            u8"\n"
            u8"[[app]]\n"
            u8"name = \"lazygit\"\n"
            u8"command = \"lazygit\"\n"
            u8"dir = \"~/p\"\n"
            u8"\n"
            u8"[[app]]\n"
            u8"name = \"no command\"\n"
            u8"\n"
            u8"[[env]]\n"
            u8"name = \"prod\"\n"
            u8"AWS_PROFILE = \"prod\"\n"
            u8"KUBECONFIG = \"~/.kube/eu\"\n"
            u8"\n"
            u8"[[env]]\n"
            u8"name = \"bad\"\n"
            u8"N = 5\n"
            u8"\n"
            u8"[[symbolFont]]\n"
            u8"font = \"after\"\n"
        ));
        char program[] = "st";
        char configOption[] = "-config";
        char* argv[] = {program, configOption, config.cStr(), nullptr};

        Options* const options = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(options->paletteApps.length() == 1);
        STD_INSIST(options->paletteApps[0].name == StringView(u8"lazygit") && options->paletteApps[0].command == StringView(u8"lazygit") && options->paletteApps[0].directory == StringView(u8"~/p"));
        STD_INSIST(options->paletteEnvs.length() == 1);
        STD_INSIST(options->paletteEnvs[0].name == StringView(u8"prod"));
        STD_INSIST(options->paletteEnvs[0].variables == StringView(u8"AWS_PROFILE=prod\nKUBECONFIG=~/.kube/eu\n"));
        STD_INSIST(options->symbolFonts.length() == 3 && options->symbolFonts[0].font == StringView(u8"after"));
        STD_INSIST(options->cloneDirectory == StringView(u8"~/src"));
        STD_INSIST(options->teleportLogin.empty());
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

    // T8 renamed this from ...DefaultToDisabled: two of the three are
    // still off by default and the titlebar is not.
    // A brand's own default stands in for the table's, and still gives
    // way to the config file and the command line like any default.
    STD_TEST(ABrandsOwnDefaultSitsUnderTheConfigAndTheCommandLine) {
        struct BareBrand final: public Brand {
            StringView displayName() const override {
                return Brand::generic()->displayName();
            }
            StringView executableName() const override {
                return Brand::generic()->executableName();
            }
            StringView identifier() const override {
                return Brand::generic()->identifier();
            }
            StringView fontSizeEnvironment() const override {
                return Brand::generic()->fontSizeEnvironment();
            }
            StringView versionEnvironment() const override {
                return Brand::generic()->versionEnvironment();
            }
            StringView iconData() const override {
                return StringView();
            }
            StringView exampleConfig() const override {
                return StringView();
            }
            const char* defaultFor(StringView option) const override {
                return option == StringView(u8"panes") ? "false" : nullptr;
            }
        } bare;

        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        // Each create() gets its own argv: the parser compacts it in place.
        char* forTable[] = {program, config, emptyConfig, nullptr};
        char* forBrand[] = {program, config, emptyConfig, nullptr};

        // Premise: the table's default is the other value.
        Options* const table = Options::create(*pool, *Brand::generic(), forTable, 3);
        STD_INSIST(table->panes);

        Options* const own = Options::create(*pool, bare, forBrand, 3);
        STD_INSIST(!own->panes);
        // Only the option the brand names moves.
        STD_INSIST(own->tabs == table->tabs);

        Buffer file = writeTempConfig(StringView(u8"panes = true\n"));
        char* fromFile[] = {program, config, file.cStr(), nullptr};
        STD_INSIST(Options::create(*pool, bare, fromFile, 3)->panes);

        char panesFlag[] = "-panes";
        char* fromLine[] = {program, config, emptyConfig, panesFlag, nullptr};
        STD_INSIST(Options::create(*pool, bare, fromLine, 4)->panes);
        unlink(file.cStr());
    }

    STD_TEST(QuickTheTransparentTitlebarAndTheHotkeyTakeTheirDefaults) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char* argv[] = {program, config, emptyConfig, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(!opts->quick);
        // T8: on by default now, and the two assertions above and below
        // it are what keeps this from reading as "every flag is true".
        STD_INSIST(opts->transparentTitlebar);
        STD_INSIST(opts->quickHotkey == StringView(u8"ctrl+grave"));
    }

    STD_TEST(CommandLineTogglesQuickAndTransparentTitlebar) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char quickFlag[] = "-quick";
        char titlebarFlag[] = "-transparentTitlebar";
        char* argv[] = {program, config, emptyConfig, quickFlag, titlebarFlag, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5);

        STD_INSIST(opts->quick);
        STD_INSIST(opts->transparentTitlebar);
        // Untouched by these two flags; still the hard default.
        STD_INSIST(opts->quickHotkey == StringView(u8"ctrl+grave"));
    }

    // -quickHotkey has no chord grammar of its own yet - T1 only checks
    // non-emptiness (options.cpp), the grammar itself is T3's job (plan
    // W3/T3). This documents that contract: garbage is accepted, not
    // rejected, at this layer.
    STD_TEST(CommandLineQuickHotkeyAcceptsAnyNonEmptyText) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char hotkeyFlag[] = "-quickHotkey";
        char hotkeyValue[] = "not a real chord!!";
        char* argv[] = {program, config, emptyConfig, hotkeyFlag, hotkeyValue, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5);

        STD_INSIST(opts->quickHotkey == StringView(u8"not a real chord!!"));
    }

    STD_TEST(EmptyCommandLineQuickHotkeyIsRejected) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char hotkeyFlag[] = "-quickHotkey";
        char hotkeyValue[] = "";
        char* argv[] = {program, config, emptyConfig, hotkeyFlag, hotkeyValue, nullptr};

        bool threw = false;
        try {
            // Reload: Startup would call exit() on this error and take
            // the whole test binary down with it.
            Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
        } catch (Exception& error) {
            threw = true;
            STD_INSIST(error.description().search(StringView(u8"expected a non-empty chord")) != nullptr);
        }
        STD_INSIST(threw);
    }

    // V3: where the tab bar lives is a named placement. T8 moved the
    // default from the title bar to the sidebar; both names are still
    // spelled out here, so the placement that is *not* the default stays
    // covered - it is the one nothing else in the suite exercises now.
    STD_TEST(TabBarPlacementDefaultsToTheSidebarAndTakesTwoNames) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char flag[] = "-tabBar";

        {
            char* argv[] = {program, config, emptyConfig, nullptr};
            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);
            STD_INSIST(opts->sidebarTabs);
        }
        {
            char value[] = "top";
            char* argv[] = {program, config, emptyConfig, flag, value, nullptr};
            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5);
            STD_INSIST(!opts->sidebarTabs);
        }
        {
            char value[] = "sidebar";
            char* argv[] = {program, config, emptyConfig, flag, value, nullptr};
            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5);
            STD_INSIST(opts->sidebarTabs);
        }
    }

    // And a name that is neither is refused rather than quietly read as
    // one of them - a misspelt placement that silently meant "top" would
    // look exactly like the feature not working.
    STD_TEST(AnUnknownTabBarPlacementIsRejected) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char flag[] = "-tabBar";
        char value[] = "left";
        char* argv[] = {program, config, emptyConfig, flag, value, nullptr};

        bool threw = false;
        try {
            // Reload: Startup would call exit() on this error and take
            // the whole test binary down with it.
            Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
        } catch (Exception& error) {
            threw = true;
            STD_INSIST(error.description().search(StringView(u8"expected top or sidebar")) != nullptr);
        }
        STD_INSIST(threw);
    }

    STD_TEST(ConfigFileSetsQuickTransparentTitlebarAndQuickHotkey) {
        auto pool = ObjPool::fromMemory();
        StringBuilder path;
        writeTempConfig(path, StringView(u8"quick = true\ntransparentTitlebar = true\nquickHotkey = \"cmd+space\"\n"));

        char program[] = "st";
        char configFlag[] = "-config";
        char* argv[] = {program, configFlag, path.cStr(), nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(opts->quick);
        STD_INSIST(opts->transparentTitlebar);
        STD_INSIST(opts->quickHotkey == StringView(u8"cmd+space"));

        ::unlink(path.cStr());
    }

    // Priority order lives in OptionsParser::get() (options.cpp): the
    // command line is checked before the config file. '+quick' is an
    // explicit "false" on the command line and must beat a configured
    // "true".
    STD_TEST(CommandLineQuickBeatsConfiguredQuick) {
        auto pool = ObjPool::fromMemory();
        StringBuilder path;
        writeTempConfig(path, StringView(u8"quick = true\n"));

        char program[] = "st";
        char configFlag[] = "-config";
        char plusQuick[] = "+quick";
        char* argv[] = {program, configFlag, path.cStr(), plusQuick, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 4);

        STD_INSIST(!opts->quick);

        ::unlink(path.cStr());
    }

    // getBool() (options.cpp) only accepts the literal strings "true"
    // and "false"; a config value can carry anything else, unlike the
    // command line's NoArg flags which can only ever produce those two.
    STD_TEST(GarbageBooleanValueInConfigIsRejected) {
        auto pool = ObjPool::fromMemory();
        StringBuilder path;
        writeTempConfig(path, StringView(u8"quick = \"nope\"\n"));

        char program[] = "st";
        char configFlag[] = "-config";
        char* argv[] = {program, configFlag, path.cStr(), nullptr};

        bool threw = false;
        try {
            Options::create(*pool, *Brand::generic(), argv, 3, OptionsLoad::Reload);
        } catch (Exception& error) {
            threw = true;
            STD_INSIST(error.description().search(StringView(u8"-quick: expected true or false")) != nullptr);
        }
        STD_INSIST(threw);

        ::unlink(path.cStr());
    }

    // F9. The divider's two options. The width has a hard default in the
    // table; the colour deliberately has none, so that an unset one is
    // the scheme's rather than a constant - the same shape cr already
    // uses to default to fg.
    // T8. Fifteen defaults changed at once, and before this test nine of
    // them had no observer in this suite at all - the four that did are
    // the four that reddened. One test that names every one of them, so
    // a value edited in optionsTable without anyone meaning to is a red
    // that says which.
    //
    // Asserted against literals rather than against optionsTable, on
    // purpose: reading the table back would agree with itself whatever
    // it said, which is the whole failure this is here to stop.
    STD_TEST(TheFifteenDefaultsTaskEightChose) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char* argv[] = {program, config, emptyConfig, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

        // The premise: an Options that never ran the parser carries the
        // inert value of each of these, not the product default. If the
        // two ever coincided this test would pass on an instance the
        // parser had not touched.
        Options* const unparsed = pool->make<Options>();
        STD_INSIST(unparsed->backgroundOpacity != opts->backgroundOpacity);
        STD_INSIST(unparsed->sidebarTabs != opts->sidebarTabs);
        STD_INSIST(unparsed->panes != opts->panes);

        STD_INSIST(opts->fontsize == 15);
        STD_INSIST(opts->vt.saveLines == 50000);
        STD_INSIST(opts->naturalEditing);
        STD_INSIST(opts->sidebarTabs);
        STD_INSIST(opts->panes);
        STD_INSIST(opts->autoHideChrome);
        STD_INSIST(opts->transparentTitlebar);
        STD_INSIST(opts->quickRememberFrame);
        STD_INSIST(opts->backgroundBlur == BackdropMode::Glass);
        STD_INSIST(opts->backgroundOpacity == 60);
        STD_INSIST(opts->quickCornerRadius == 12);
        STD_INSIST(opts->paneDividerColor.red == 0x00 && opts->paneDividerColor.green == 0xcd && opts->paneDividerColor.blue == 0x00);
        STD_INSIST(opts->quickGeometry.width.percent && opts->quickGeometry.width.value == 90);
        STD_INSIST(opts->quickGeometry.height.percent && opts->quickGeometry.height.value == 75);
        STD_INSIST(opts->quickGeometry.x.percent && opts->quickGeometry.x.value == 5);
        STD_INSIST(opts->quickGeometry.y.percent && opts->quickGeometry.y.value == 10);
        // colorScheme, by what it puts in the palette rather than by its
        // name: the name is not stored, and the colours are the whole of
        // what choosing a scheme does. Catppuccin Mocha's background.
        STD_INSIST(opts->vt.bg.red == 0x1e && opts->vt.bg.green == 0x1e && opts->vt.bg.blue == 0x2e);
        STD_INSIST(opts->vt.fg.red == 0xcd && opts->vt.fg.green == 0xd6 && opts->vt.fg.blue == 0xf4);
        // uriScheme, the one list among them. mailto and gemini are the
        // two T8 added; nosuch is the control that keeps this from
        // passing on a trie that allows everything.
        STD_INSIST(opts->uriSchemes.length() == 5);
        STD_INSIST(opts->uriSchemeAllowed(StringView(u8"http")));
        STD_INSIST(opts->uriSchemeAllowed(StringView(u8"https")));
        STD_INSIST(opts->uriSchemeAllowed(StringView(u8"file")));
        STD_INSIST(opts->uriSchemeAllowed(StringView(u8"mailto")));
        STD_INSIST(opts->uriSchemeAllowed(StringView(u8"gemini")));
        STD_INSIST(!opts->uriSchemeAllowed(StringView(u8"nosuch")));
    }

    STD_TEST(TheDividerDefaultsToOnePixelOfItsOwnGreen) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char* argv[] = {program, config, emptyConfig, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

        // One pixel: the smallest thing that is still a line. Zero was
        // the old A10 default and is what left nothing to see.
        STD_INSIST(opts->paneDividerWidth == 1);
        // T8: constant, not derived. The premise first - the seam's
        // colour and the palette entry it used to be taken from have to
        // differ at all, or "it is not the palette's" would
        // pass on a tree where the option had quietly gone back to
        // deriving itself and the scheme happened to be green.
        const bool differsFromBrightBlack = opts->paneDividerColor.red != opts->vt.palette[8].red || opts->paneDividerColor.green != opts->vt.palette[8].green || opts->paneDividerColor.blue != opts->vt.palette[8].blue;
        STD_INSIST(differsFromBrightBlack);
        STD_INSIST(opts->paneDividerColor.red == 0x00);
        STD_INSIST(opts->paneDividerColor.green == 0xcd);
        STD_INSIST(opts->paneDividerColor.blue == 0x00);
        // And it is not simply the background, which is the answer a
        // seam that stayed invisible would give.
        const bool sameAsBackground = opts->paneDividerColor.red == opts->vt.bg.red && opts->paneDividerColor.green == opts->vt.bg.green && opts->paneDividerColor.blue == opts->vt.bg.blue;
        STD_INSIST(!sameAsBackground);
    }

    STD_TEST(TheCommandLineSetsTheDividerWidthAndColour) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char widthFlag[] = "-paneDividerWidth";
        char width[] = "6";
        char colourFlag[] = "-paneDividerColor";
        char colour[] = "#ff0000";
        char* argv[] = {program, config, emptyConfig, widthFlag, width, colourFlag, colour, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 7);

        STD_INSIST(opts->paneDividerWidth == 6);
        STD_INSIST(opts->paneDividerColor.red == 255);
        STD_INSIST(opts->paneDividerColor.green == 0);
        STD_INSIST(opts->paneDividerColor.blue == 0);
        // The given colour beat the scheme's, which is the whole point of
        // the option and the half a default-only test cannot see.
        STD_INSIST(opts->paneDividerColor.red != opts->vt.palette[8].red || opts->paneDividerColor.green != opts->vt.palette[8].green || opts->paneDividerColor.blue != opts->vt.palette[8].blue);
    }

    // T10, retargeted by T8. The pair used to promise that an untouched
    // config drew the solid window upstream draws; it now promises the
    // translucent one this fork ships. Both halves are asserted, because
    // either alone is satisfiable by an accident: an opacity below 100
    // with no backdrop shows the desktop raw, and a backdrop at 100 is
    // the case the startup warning exists for.
    STD_TEST(TranslucencyDefaultsToGlassOverASixtyPercentBackground) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char* argv[] = {program, config, emptyConfig, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(opts->backgroundOpacity == 60);
        STD_INSIST(opts->backgroundBlur == BackdropMode::Glass);
    }

    STD_TEST(TranslucencyComesFromTheConfigAndTheCommandLine) {
        auto pool = ObjPool::fromMemory();
        StringBuilder path;
        writeTempConfig(path, StringView(u8"backgroundOpacity = 40\nbackgroundBlur = true\n"));

        {
            char program[] = "st";
            char configFlag[] = "-config";
            char* argv[] = {program, configFlag, path.cStr(), nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

            STD_INSIST(opts->backgroundOpacity == 40);
            STD_INSIST(opts->backgroundBlur == BackdropMode::Blur);
        }

        {
            // The command line beats the file, and '-backgroundBlur
            // off' is an explicit Off that has to beat a configured
            // true. It is also the replacement for '+backgroundBlur',
            // which SepArg refuses outright now that the option carries
            // a value.
            char program[] = "st";
            char configFlag[] = "-config";
            char opacityFlag[] = "-backgroundOpacity";
            char opacity[] = "75";
            char blurFlag[] = "-backgroundBlur";
            char blurValue[] = "off";
            char* argv[] = {program, configFlag, path.cStr(), opacityFlag, opacity, blurFlag, blurValue, nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 7);

            STD_INSIST(opts->backgroundOpacity == 75);
            STD_INSIST(opts->backgroundBlur == BackdropMode::Off);
        }

        ::unlink(path.cStr());
    }

    // R1-test. Three places went into wave 1 with no observer at all,
    // and T1 named them itself. Two are reachable without a process and
    // live here: the round trip between the spellings the parser
    // accepts and the name the dump prints, and the form hint both
    // SepArg refusals grew. The third - the "(default: off)" tail
    // printUsage() started printing when the option changed kind -
    // needs the help text and lives in tst/test_options.py.
    STD_TEST(EveryBackdropSpellingParsesAndTheNamesPrintBackUnchanged) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char modeFlag[] = "-backgroundBlur";

        struct Spelling {
            const char* text;
            BackdropMode mode;
        };
        const Spelling canonical[] = {
            {"off", BackdropMode::Off},
            {"blur", BackdropMode::Blur},
            {"glass", BackdropMode::Glass},
        };

        // The premise, stated before anything is read back. A parser
        // that answered Off to every value, or a backdropModeName()
        // collapsed onto one string, would satisfy a round trip that
        // never checked the three ends were distinct to begin with -
        // and this is exactly the shape that has passed on nothing
        // eight times in this repository.
        for (size_t outer = 0; outer < 3; ++outer) {
            for (size_t inner = outer + 1; inner < 3; ++inner) {
                STD_INSIST(StringView(canonical[outer].text) != StringView(canonical[inner].text));
                STD_INSIST(canonical[outer].mode != canonical[inner].mode);
                STD_INSIST(backdropModeName(canonical[outer].mode) != backdropModeName(canonical[inner].mode));
            }
        }

        for (const Spelling& probe : canonical) {
            char value[16];
            ::strcpy(value, probe.text);
            char* argv[] = {program, config, emptyConfig, modeFlag, value, nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);

            STD_INSIST(opts->backgroundBlur == probe.mode);
            // One table for both directions. The dump prints this name,
            // the config comment offers it to the user as something to
            // type, and -help offers one of them as an example: a name
            // the parser would refuse is a lie in three places at once.
            STD_INSIST(backdropModeName(opts->backgroundBlur) == StringView(probe.text));
        }

        // The compatibility half, and the whole reason the option could
        // change kind at all: a config written while this was a flag
        // still means what it meant.
        const Spelling aliases[] = {
            {"false", BackdropMode::Off},
            {"true", BackdropMode::Blur},
        };
        for (const Spelling& probe : aliases) {
            char value[16];
            ::strcpy(value, probe.text);
            char* argv[] = {program, config, emptyConfig, modeFlag, value, nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);

            STD_INSIST(opts->backgroundBlur == probe.mode);
        }

        // And from a file, quoted, which is how both example configs
        // spell it now. A bare true is a TOML boolean and a quoted
        // "glass" is a TOML string; only the second shape carries the
        // mode this option was widened for.
        {
            StringBuilder path;
            writeTempConfig(path, StringView(u8"backgroundBlur = \"glass\"\n"));
            char configFlag[] = "-config";
            char* argv[] = {program, configFlag, path.cStr(), nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

            STD_INSIST(opts->backgroundBlur == BackdropMode::Glass);
            ::unlink(path.cStr());
        }
    }

    STD_TEST(AnUnknownBackdropModeIsRejectedAndTheThreeNamesAreOffered) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char modeFlag[] = "-backgroundBlur";

        // 'on' and '1' are what a hand reaching for the old flag types;
        // the empty string is what an unquoted config key can deliver.
        // None of them may be guessed at - a mode silently read as Off
        // is the quiet substitution the loud refusal exists to prevent.
        const char* const rejected[] = {"on", "1", "yes", "blurred", ""};
        for (const char* value : rejected) {
            char text[16];
            ::strcpy(text, value);
            char* argv[] = {program, config, emptyConfig, modeFlag, text, nullptr};
            bool threw = false;
            try {
                Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
            } catch (Exception& error) {
                threw = true;
                const StringView message = error.description();
                STD_INSIST(message.search(StringView(u8"-backgroundBlur")) != nullptr);
                // The refusal has to name the way out, not merely say
                // no: all three legal spellings, in the message itself.
                STD_INSIST(message.search(backdropModeName(BackdropMode::Off)) != nullptr);
                STD_INSIST(message.search(backdropModeName(BackdropMode::Blur)) != nullptr);
                STD_INSIST(message.search(backdropModeName(BackdropMode::Glass)) != nullptr);
            }
            STD_INSIST(threw);
        }
        // The control is the test above: the three canonical spellings
        // and the two aliases are accepted there, so this one cannot be
        // satisfied by a parser that rejects everything.
    }

    STD_TEST(TheBackdropOptionSpelledAsAFlagIsToldWhatShapeToUse) {
        // Unobserved before this test, and named as such by T1. The
        // option takes a value now, so every finger and every config
        // that still spells it as a flag lands in one of these two
        // refusals; both carry a tail naming the shape. The only
        // assertions that existed - tst/test_options.py's 'missing
        // value' and "'+' is invalid here" - are on the base complaint
        // and survive dropping the tail whole.
        //
        // F1b turned that tail from one example into the whole set.
        // The example was the option's hard default, and this option's
        // hard default is 'off' - the single value that answers "how do
        // I switch this on?" with "you don't". The set is neutral
        // between the two refusals, '+backgroundBlur' having genuinely
        // meant off, and it is the same set the parser recites when it
        // refuses a name outside it: the last block below is what holds
        // the two messages to one story.
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char modeFlag[] = "-backgroundBlur";

        // The premise, before any of it is leaned on: three searches for
        // three names prove nothing while the three names might be one
        // string. R1-test's M6 - Glass printing back as "blur" - is that
        // collapse exactly, and it must fail here rather than pass.
        const StringView names[] = {
            backdropModeName(BackdropMode::Off),
            backdropModeName(BackdropMode::Blur),
            backdropModeName(BackdropMode::Glass),
        };
        STD_INSIST(names[0] != names[1]);
        STD_INSIST(names[1] != names[2]);
        STD_INSIST(names[0] != names[2]);

        // The set as the hint spells it, carried out of the first
        // refusal by copy - the Exception owning the message is gone by
        // the time the second one is read.
        char offered[64];
        size_t offeredLength = 0;

        struct Refusal {
            const char* argument;
            const char* complaint;
        };
        const Refusal refusals[] = {
            {"-backgroundBlur", ": missing value"},
            {"+backgroundBlur", ": '+' is invalid here"},
        };
        for (const Refusal& probe : refusals) {
            char argument[32];
            ::strcpy(argument, probe.argument);
            char* argv[] = {program, config, emptyConfig, argument, nullptr};
            bool threw = false;
            try {
                Options::create(*pool, *Brand::generic(), argv, 4, OptionsLoad::Reload);
            } catch (Exception& error) {
                threw = true;
                const StringView message = error.description();
                // The half that already had an observer.
                STD_INSIST(message.search(StringView(probe.complaint)) != nullptr);
                const StringView lead(u8"; -backgroundBlur takes a value: ");
                const u8* const at = message.search(lead);
                STD_INSIST(at != nullptr);

                const StringView set(at + lead.length(), message.end());
                STD_INSIST(!set.empty());

                // Every mode is named. Naming only the default is the
                // shape this test exists to keep out: a reader who wants
                // the backdrop on must not be handed 'off' alone.
                for (const StringView& name : names) {
                    STD_INSIST(set.search(name) != nullptr);
                }

                // And what is named is what the parser takes. Read back
                // out of the message, fed in, printed back - so the
                // offer cannot rot into advice that fails.
                for (const StringView& name : names) {
                    char value[16];
                    STD_INSIST(name.length() < sizeof(value));
                    ::memcpy(value, name.data(), name.length());
                    value[name.length()] = '\0';
                    char* accepted[] = {program, config, emptyConfig, modeFlag, value, nullptr};

                    Options* const opts = Options::create(*pool, *Brand::generic(), accepted, 5, OptionsLoad::Reload);

                    STD_INSIST(backdropModeName(opts->backgroundBlur) == name);
                }

                // Both refusals offer one and the same set.
                if (offeredLength == 0) {
                    STD_INSIST(set.length() < sizeof(offered));
                    ::memcpy(offered, set.data(), set.length());
                    offered[set.length()] = '\0';
                    offeredLength = set.length();
                } else {
                    STD_INSIST(set == StringView(offered));
                }
            }
            STD_INSIST(threw);
        }
        STD_INSIST(offeredLength != 0);

        // The third message a hand reaching for the old flag can raise,
        // and the one the other two must not contradict: the parser's
        // own refusal of a name outside the set has to recite that same
        // set, word for word. Two texts drifting apart is how a user
        // ends up told two different things about one option.
        {
            char unknown[] = "sparkle";
            char* argv[] = {program, config, emptyConfig, modeFlag, unknown, nullptr};
            bool threw = false;
            try {
                Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
            } catch (Exception& error) {
                threw = true;
                STD_INSIST(error.description().search(StringView(offered)) != nullptr);
            }
            STD_INSIST(threw);
        }
    }

    // Both ends of the range and a non-number. 101 is the interesting
    // one: 0..100 is a percentage, and a ceiling of 65535 would let a
    // u16 through that the shader packs into seven bits.
    STD_TEST(AnOpacityOutsideTheRangeIsRejected) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char opacityFlag[] = "-backgroundOpacity";

        const char* const rejected[] = {"101", "-1", "half"};
        for (const char* value : rejected) {
            char opacity[16];
            ::strcpy(opacity, value);
            char* argv[] = {program, config, emptyConfig, opacityFlag, opacity, nullptr};
            bool threw = false;
            try {
                Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
            } catch (Exception& error) {
                threw = true;
                STD_INSIST(error.description().search(StringView(u8"-backgroundOpacity")) != nullptr);
            }
            STD_INSIST(threw);
        }

        // And the ends themselves are accepted, or the test above would
        // pass just as well against an option that rejects everything.
        const char* const accepted[] = {"0", "100"};
        for (const char* value : accepted) {
            char opacity[16];
            ::strcpy(opacity, value);
            char* argv[] = {program, config, emptyConfig, opacityFlag, opacity, nullptr};
            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
            STD_INSIST(opts->backgroundOpacity == (value[0] == '0' ? 0 : 100));
        }
    }

    // C10. The absence is the interesting half: an unset sidebarColor
    // is not a colour, it is "keep deriving the panel from bg and fg",
    // and that is what keeps a fork whose upstream has no such option
    // drawing what it always drew.
    STD_TEST(TheSidebarColourIsUnsetUntilSomebodySetsIt) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char* argv[] = {program, config, emptyConfig, nullptr};

        Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(!opts->sidebarColorSet);
    }

    STD_TEST(TheSidebarColourComesFromTheConfigAndTheCommandLine) {
        auto pool = ObjPool::fromMemory();
        StringBuilder path;
        writeTempConfig(path, StringView(u8"sidebarColor = \"#204060\"\n"));

        {
            char program[] = "st";
            char configFlag[] = "-config";
            char* argv[] = {program, configFlag, path.cStr(), nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 3);

            STD_INSIST(opts->sidebarColorSet);
            STD_INSIST(opts->sidebarColor.red == 0x20);
            STD_INSIST(opts->sidebarColor.green == 0x40);
            STD_INSIST(opts->sidebarColor.blue == 0x60);
        }

        {
            // The command line beats the file. Without this half, a
            // parser that read the config and ignored argv would pass.
            char program[] = "st";
            char configFlag[] = "-config";
            char colourFlag[] = "-sidebarColor";
            char colour[] = "#0a0b0c";
            char* argv[] = {program, configFlag, path.cStr(), colourFlag, colour, nullptr};

            Options* const opts = Options::create(*pool, *Brand::generic(), argv, 5);

            STD_INSIST(opts->sidebarColorSet);
            STD_INSIST(opts->sidebarColor.red == 0x0a);
            STD_INSIST(opts->sidebarColor.green == 0x0b);
            STD_INSIST(opts->sidebarColor.blue == 0x0c);
        }

        ::unlink(path.cStr());
    }

    STD_TEST(AGarbageSidebarColourIsRejected) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char colourFlag[] = "-sidebarColor";
        char colour[] = "not-a-colour";
        char* argv[] = {program, config, emptyConfig, colourFlag, colour, nullptr};

        bool threw = false;
        try {
            Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
        } catch (Exception& error) {
            threw = true;
            // Named, so the user is told which option was wrong.
            STD_INSIST(error.description().search(StringView(u8"sidebarColor")) != nullptr);
        }
        STD_INSIST(threw);
    }

    STD_TEST(AGarbageDividerWidthIsRejected) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char widthFlag[] = "-paneDividerWidth";
        char width[] = "wide";
        char* argv[] = {program, config, emptyConfig, widthFlag, width, nullptr};

        bool threw = false;
        try {
            // Reload, for the reason the hotkey test above gives: on this
            // error Startup calls exit() and takes the test binary with
            // it.
            Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
        } catch (Exception& error) {
            threw = true;
            STD_INSIST(error.description().search(StringView(u8"-paneDividerWidth")) != nullptr);
        }
        STD_INSIST(threw);
    }

    // R9-qa. The width test above rejects a non-number; these are the
    // two neighbours it does not reach - a number past the ceiling, and
    // a colour that is not one. Both go through different code (the
    // range check, and convColor) and neither was asserted.
    STD_TEST(ADividerWidthPastTheCeilingAndAGarbageColourAreBothRejected) {
        auto pool = ObjPool::fromMemory();
        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";

        {
            char widthFlag[] = "-paneDividerWidth";
            char width[] = "5000";
            char* argv[] = {program, config, emptyConfig, widthFlag, width, nullptr};
            bool threw = false;
            try {
                Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
            } catch (Exception& error) {
                threw = true;
                STD_INSIST(error.description().search(StringView(u8"-paneDividerWidth")) != nullptr);
            }
            STD_INSIST(threw);
        }

        {
            char colourFlag[] = "-paneDividerColor";
            char colour[] = "not-a-colour";
            char* argv[] = {program, config, emptyConfig, colourFlag, colour, nullptr};
            bool threw = false;
            try {
                Options::create(*pool, *Brand::generic(), argv, 5, OptionsLoad::Reload);
            } catch (Exception& error) {
                threw = true;
                // Named, so the user is told which option was wrong and
                // not merely that something was.
                STD_INSIST(error.description().search(StringView(u8"paneDividerColor")) != nullptr);
            }
            STD_INSIST(threw);
        }
    }

    // F6, R6-sec V1. Options::uriSchemeAllowed() answers "no" on an
    // instance whose trie was never built, instead of dereferencing the
    // null it used to. The branch is real and reachable - Composer's
    // constructor puts exactly such an Options in its slot before any
    // configuration is read (composer.cpp) - and an fprintf placed in
    // it did not fire once across all 963 tests. Nothing executed it,
    // so returning true from it, or going back to the dereference,
    // would both have passed green.
    //
    // What is guarded is a policy about the world outside this process:
    // which schemes a link may be opened with. An instance that has not
    // been told the policy has to refuse, not permit.
    STD_TEST(AnOptionsNobodyParsedPermitsNoUriSchemeAndAParsedOnePermitsHttp) {
        auto pool = ObjPool::fromMemory();

        // Built the way Composer builds its placeholder: make<Options>()
        // and no create(), which is the only way to hold one of these.
        Options* const unparsed = pool->make<Options>();
        STD_INSIST(unparsed->uriSchemeTrie == nullptr);

        // http, and not some scheme nobody allows: a parsed Options
        // permits it, which is what makes the answer below a statement
        // about *this* instance rather than about the scheme. The
        // assertion pair at the end is the fixture's own proof - without
        // the parsed half, "false" here would hold just as well for a
        // uriSchemeAllowed() that had been rewritten to refuse
        // everything, and this test would be guarding nothing.
        STD_INSIST(!unparsed->uriSchemeAllowed(StringView(u8"http")));
        STD_INSIST(!unparsed->uriSchemeAllowed(StringView(u8"https")));
        STD_INSIST(!unparsed->uriSchemeAllowed(StringView(u8"file")));
        // Case folding is on the far side of the null check, so this is
        // the same branch and not a second one - asserted so a fix that
        // moved the check below the fold is caught here rather than by a
        // crash somewhere else.
        STD_INSIST(!unparsed->uriSchemeAllowed(StringView(u8"HTTP")));
        // And the empty scheme, which is what a malformed URI offers.
        STD_INSIST(!unparsed->uriSchemeAllowed(StringView(u8"")));

        char program[] = "st";
        char config[] = "-config";
        char emptyConfig[] = "/dev/null";
        char* argv[] = {program, config, emptyConfig, nullptr};
        Options* const parsed = Options::create(*pool, *Brand::generic(), argv, 3);

        STD_INSIST(parsed->uriSchemeTrie != nullptr);
        STD_INSIST(parsed->uriSchemeAllowed(StringView(u8"http")));
        STD_INSIST(parsed->uriSchemeAllowed(StringView(u8"HTTP")));
        STD_INSIST(!parsed->uriSchemeAllowed(StringView(u8"nosuch")));
    }
}
