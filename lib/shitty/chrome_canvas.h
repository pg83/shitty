/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/vector.h>
#include <std/sys/types.h>

// A colour as the chrome is drawn with it: 0..1 channels, alpha straight
// (not premultiplied) - the way options and palettes spell colours.
struct ChromeColor {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;
};

// The picture of the window around the terminal, drawn on the CPU: the few
// shapes a Mac window is made of - rounded rectangles, circles, soft
// shadows and text masks - anti-aliased by coverage, each composited over
// what is already there. Pixels are premultiplied, B G R A in memory: what
// a wl_shm ARGB8888 buffer holds on a little-endian machine.
//
// Coordinates are pixels, floating so a shape can sit between two of them;
// a pixel's centre is at +0.5. Nothing here knows about Wayland.
class ChromeCanvas {
public:
    ChromeCanvas() = default;
    ChromeCanvas(u32 width, u32 height);

    void resize(u32 width, u32 height);
    // Every pixel transparent.
    void clear();

    // A rectangle with circular corners of the given radius (clamped to
    // half the shorter side), composited over.
    void fillRoundedRect(float x, float y, float width, float height, float radius, ChromeColor color);
    // The band `thickness` wide just inside the rounded rectangle's edge.
    void strokeRoundedRect(float x, float y, float width, float height, float radius, float thickness, ChromeColor color);
    void fillCircle(float centreX, float centreY, float radius, ChromeColor color);
    // A soft shadow of a rounded rectangle: full inside, falling off over
    // `blur` pixels outside the edge.
    void shadow(float x, float y, float width, float height, float radius, float blur, ChromeColor color);
    // Makes the rounded rectangle's inside transparent again, anti-aliased
    // at its edge - where another surface shows through.
    void punchRoundedRect(float x, float y, float width, float height, float radius);
    // An 8-bit coverage mask composited in `color` at (x, y), clipped to
    // the canvas and to the column `clipRight`.
    void blendMask(i32 x, i32 y, const u8* mask, u32 width, u32 height, u32 stride, ChromeColor color, i32 clipRight);

    u32 width() const {
        return width_;
    }
    u32 height() const {
        return height_;
    }
    const u8* data() const {
        return pixels_.data();
    }
    // The pixel at (x, y) as premultiplied B G R A.
    const u8* pixel(u32 x, u32 y) const {
        return pixels_.data() + ((size_t)(y) * width_ + x) * 4;
    }

private:
    void blend(u32 x, u32 y, ChromeColor color, float coverage);

    u32 width_ = 0;
    u32 height_ = 0;
    stl::Vector<u8> pixels_;
};

// Signed distance from (px, py) to the rounded rectangle's edge: negative
// inside. Exposed for the tests and for hit testing the same shape.
float chromeRoundedRectDistance(float px, float py, float x, float y, float width, float height, float radius);
