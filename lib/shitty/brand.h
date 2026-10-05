/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>

struct Brand {
    virtual stl::StringView displayName() const = 0;
    virtual stl::StringView executableName() const = 0;
    virtual stl::StringView identifier() const = 0;
    virtual stl::StringView fontSizeEnvironment() const = 0;
    virtual stl::StringView versionEnvironment() const = 0;
    virtual stl::StringView iconData() const = 0;
    // The brand's own example config file, embedded verbatim at build
    // time, which -printConfig writes to stdout. Empty for generic():
    // the neutral brand ships no config file of its own, and embedding
    // either real one there would put that brand's name into both
    // binaries.
    virtual stl::StringView exampleConfig() const = 0;
    // A default of the brand's own for `option`, in place of the one the
    // options table carries, or null for the table's. The command line and
    // the config file override it like any default. On Linux st is the bare
    // window - no tabs, panes or decorations - and pt the one with the tab
    // list; everywhere else both brands take the table's defaults.
    virtual const char* defaultFor(stl::StringView option) const;

    const char* identifierCString() const;
    void configureVersionEnvironment() const;

    // Headless/unit-test composers use a neutral brand unless their adapter
    // supplies the production brand explicitly.
    static Brand* generic();
};

int runMain(Brand& brand, int argc, char* argv[]);
