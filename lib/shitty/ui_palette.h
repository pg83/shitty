/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

namespace stl {
    class ObjPool;
}

struct Composer;

// macOS-only: the command palette (palette_session.h) on Cmd+K, a panel
// over the middle of the terminal with a field that takes the keys while
// it is up. Off macOS nothing defines this; pt on Wayland draws its own
// in ui_wayland_chrome.cpp.
void createPaletteUi(stl::ObjPool& owner, Composer& composer);
