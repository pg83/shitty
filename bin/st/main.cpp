/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "brand.h"
#include "shitty_icon_data.h"
#include "shitty_config_data.h"

using namespace stl;

namespace {
    struct ShittyBrand final: public Brand {
        StringView displayName() const override;
        StringView executableName() const override;
        StringView identifier() const override;
        StringView fontSizeEnvironment() const override;
        StringView versionEnvironment() const override;
        StringView iconData() const override;
        StringView exampleConfig() const override;
        const char* defaultFor(StringView option) const override;
    };

    static Brand* createBrand();
}

StringView ShittyBrand::displayName() const {
    return StringView(u8"Shitty");
}

StringView ShittyBrand::executableName() const {
    return StringView(u8"st");
}

StringView ShittyBrand::identifier() const {
    return StringView(u8"shitty");
}

StringView ShittyBrand::fontSizeEnvironment() const {
    return StringView(u8"SHITTY_FONT_SIZE");
}

StringView ShittyBrand::versionEnvironment() const {
    return StringView(u8"SHITTY_VERSION");
}

StringView ShittyBrand::iconData() const {
    return StringView((const u8*)(shittyIcon.data), shittyIcon.size);
}

StringView ShittyBrand::exampleConfig() const {
    return StringView((const u8*)(shittyExampleConfig.data), shittyExampleConfig.size);
}

const char* ShittyBrand::defaultFor(StringView option) const {
#if defined(__linux__)
    // On Linux st is the plain terminal: one shell to a window, placed by
    // the (usually tiling) window manager, with no title bar, tab list or
    // panes, and every chord those would take left to the program inside.
    // pt is the one with the tab list and the window drawn around it.
    // All of it can be switched back on in the config file.
    if (option == StringView(u8"tabs") || option == StringView(u8"panes")) {
        return "false";
    }
    if (option == StringView(u8"no-decorations")) {
        return "true";
    }
#else
    (void)(option);
#endif
    return nullptr;
}

namespace {
    static Brand* createBrand() {
        static ShittyBrand brand;
        return &brand;
    }
}

int main(int argc, char* argv[]) {
    return runMain(*createBrand(), argc, argv);
}
