/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

#include <stddef.h>

namespace stl {
    class ObjPool;
    class Output;
}

namespace plt {
    struct Platform;
    struct Window;
}

struct Composer;
struct VtCellExtras;
struct VtGeometry;
struct VtHost;
struct Vterm;
struct VtermTraceFactory;

struct VtermHeadless {
    virtual void feed(const u8* data, size_t len) = 0;
    // The one terminal this host built and feeds; the host owns it for
    // the process lifetime, there is no session set to ask.
    virtual Vterm* terminal() = 0;
    // The headless platform and window the host runs on, for embedders
    // that share its loop or its chrome - a test driving a real pty or
    // a session set next to the terminal.
    virtual plt::Platform* platform() = 0;
    virtual plt::Window* window() = 0;
    // The embedding pieces the host built around its terminal, for a
    // test that grows a second terminal against the same window.
    virtual VtHost* host() = 0;
    virtual VtGeometry& geometry() = 0;
    virtual VtCellExtras& extras() = 0;

    // ptyCapture observes what the terminal writes toward its child;
    // null discards it.
    //
    // The Composer, and not the pool-plus-config upstream takes:
    // create() counts the headless grid out of contentInsets(), builds
    // the terminal's PaneGeometry from it, and borrows the composer's
    // own host adapter.
    //
    // T5.9 meant to take upstream's file whole and could not: its body
    // calls a nine-argument Vterm::create and Vterm::windowResized(),
    // and our core has neither - create() takes a Composer, and a
    // resize arrives as paneResized(). Upstream's file compiles only
    // against upstream's Vterm, so taking it whole takes the pane
    // architecture out with it (A1/A8).
    //
    // The file lives in lib/shitty and not, as upstream's does, in
    // lib/vterm. It is an embedder's adapter - it builds a platform, a
    // window and the Composer's host - and while it sat in the core it
    // was three quarters of the boundary allowance and the one reason
    // an archive globbed out of lib/vterm could not be linked with
    // --no-undefined (`./build so`). The price is here rather than in a
    // report: bin/core_perf and bin/main_fuzz name the new path, and
    // core_perf was byte-for-byte upstream until they did.
    static VtermHeadless* create(Composer& composer, VtermTraceFactory* traceFactory, stl::Output* ptyCapture = nullptr);
};
