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

// The window around the terminal on Wayland, drawn by this program: the
// window's surface, the tab list on it with the window's buttons, and the
// terminal as a rounded panel over it - the macOS window, as near as a
// compositor lets a client come. Needs the window created with
// WindowOptions::clientChrome and a backend that could give it; without
// one it does nothing. Linux only (build.py).
void createWaylandChrome(stl::ObjPool& owner, Composer& composer);
