/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "brand.h"

#include <lib/vterm/color.h>
#include <lib/vterm/fatal.h>

#include <std/str/builder.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>

#include <stdlib.h>

using namespace stl;

namespace {
    struct GenericBrand final: public Brand {
        StringView displayName() const override;
        StringView executableName() const override;
        StringView identifier() const override;
        StringView fontSizeEnvironment() const override;
        StringView versionEnvironment() const override;
        StringView iconData() const override;
        StringView exampleConfig() const override;
    };
}

StringView GenericBrand::displayName() const {
    return StringView(u8"Terminal");
}

StringView GenericBrand::executableName() const {
    return StringView(u8"terminal");
}

StringView GenericBrand::identifier() const {
    return StringView(u8"terminal");
}

StringView GenericBrand::fontSizeEnvironment() const {
    return StringView(u8"TERMINAL_FONT_SIZE");
}

StringView GenericBrand::versionEnvironment() const {
    return StringView(u8"TERMINAL_VERSION");
}

StringView GenericBrand::iconData() const {
    return StringView();
}

StringView GenericBrand::exampleConfig() const {
    return StringView();
}

const char* Brand::defaultFor(StringView) const {
    return nullptr;
}

const char* Brand::identifierCString() const {
    return (const char*)(identifier().data());
}

void Brand::configureVersionEnvironment() const {
    const StringView environment = versionEnvironment();
    if (setenv((const char*)(environment.data()), SHITTY_VERSION, 1) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"setenv ") << environment);
    }
}

Brand* Brand::generic() {
    static GenericBrand brand;
    return &brand;
}
