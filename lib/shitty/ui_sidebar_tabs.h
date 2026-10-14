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

// macOS-only chrome: the vertical tab list down the window's left
// edge, shown when -sidebarTabs is on and toggled with cmd+b. Creates a
// self-contained pool object that projects the SessionSet tab model
// from sessionsChangedListeners onto an AppKit view over the content
// view, drives SessionSet from clicks, and reserves its own width out
// of the grid through Composer::setChromeReserve() so no text is ever
// drawn under it (A1). Nothing calls the object afterwards; off macOS
// nothing defines this.
void createSidebarTabsUi(stl::ObjPool& owner, Composer& composer);
