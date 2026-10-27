/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

#include <plt/input.h>

// Signed, zero-based coordinates. Text pixels are relative to the first
// cell; positions outside the text remain outside for selection dragging.
// DEC locator pixels are supplied independently by the presenting client
// to preserve its coordinate convention (xterm uses window pixels).
struct VtPointerPosition {
    int pixelX = 0;
    int pixelY = 0;
    int locatorPixelX = 0;
    int locatorPixelY = 0;
};

struct VtPointerMotion {
    VtPointerPosition position;
    u16 modifiers = 0;
};

struct VtPointerButton {
    VtPointerPosition position;
    plt::PointerButton button = plt::PointerButton::Primary;
    bool pressed = false;
    u16 modifiers = 0;
    double time = 0;
};

struct VtScroll {
    VtPointerPosition position;
    double x = 0;
    double y = 0;
    u16 modifiers = 0;
    plt::ScrollPhase phase = plt::ScrollPhase::None;
    bool precise = false;
    bool momentum = false;
    double time = 0;
    // Backing pixels per native logical unit. Smooth deltas are normalized
    // into cell-sized steps against the current cell metrics.
    double contentScale = 1.0;
};
