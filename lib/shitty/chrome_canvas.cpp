/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "chrome_canvas.h"

#include <math.h>
#include <string.h>

namespace {
    float clamp01(float value) {
        return value < 0 ? 0 : value > 1 ? 1 : value;
    }

    u8 toByte(float value) {
        return (u8)(clamp01(value) * 255.0f + 0.5f);
    }

    // The pixel rows and columns a shape reaches into, widened by `spread`
    // and clipped to the canvas.
    struct Span {
        u32 x0 = 0;
        u32 y0 = 0;
        u32 x1 = 0;
        u32 y1 = 0;
    };

    Span spanOf(float x, float y, float width, float height, float spread, u32 canvasWidth, u32 canvasHeight) {
        const float left = floorf(x - spread);
        const float top = floorf(y - spread);
        const float right = ceilf(x + width + spread);
        const float bottom = ceilf(y + height + spread);
        Span span;
        span.x0 = left < 0 ? 0 : (u32)(left);
        span.y0 = top < 0 ? 0 : (u32)(top);
        span.x1 = right < 0 ? 0 : right > (float)(canvasWidth) ? canvasWidth : (u32)(right);
        span.y1 = bottom < 0 ? 0 : bottom > (float)(canvasHeight) ? canvasHeight : (u32)(bottom);
        return span;
    }
}

float chromeRoundedRectDistance(float px, float py, float x, float y, float width, float height, float radius) {
    const float halfWidth = width * 0.5f;
    const float halfHeight = height * 0.5f;
    const float limit = halfWidth < halfHeight ? halfWidth : halfHeight;
    const float r = radius < 0 ? 0 : radius > limit ? limit : radius;
    const float qx = fabsf(px - (x + halfWidth)) - (halfWidth - r);
    const float qy = fabsf(py - (y + halfHeight)) - (halfHeight - r);
    const float outsideX = qx > 0 ? qx : 0;
    const float outsideY = qy > 0 ? qy : 0;
    const float inside = qx > qy ? qx : qy;
    return sqrtf(outsideX * outsideX + outsideY * outsideY) + (inside < 0 ? inside : 0) - r;
}

ChromeCanvas::ChromeCanvas(u32 width, u32 height) {
    resize(width, height);
}

void ChromeCanvas::resize(u32 width, u32 height) {
    width_ = width;
    height_ = height;
    pixels_.clear();
    pixels_.zero((size_t)(width) * height * 4);
}

void ChromeCanvas::clear() {
    if (!pixels_.empty()) {
        memset(pixels_.mutData(), 0, pixels_.length());
    }
}

void ChromeCanvas::blend(u32 x, u32 y, ChromeColor color, float coverage) {
    const float alpha = clamp01(color.a) * clamp01(coverage);
    if (alpha <= 0) {
        return;
    }
    u8* const p = pixels_.mutData() + ((size_t)(y) * width_ + x) * 4;
    const float keep = 1.0f - alpha;
    p[0] = toByte(color.b * alpha + p[0] / 255.0f * keep);
    p[1] = toByte(color.g * alpha + p[1] / 255.0f * keep);
    p[2] = toByte(color.r * alpha + p[2] / 255.0f * keep);
    p[3] = toByte(alpha + p[3] / 255.0f * keep);
}

void ChromeCanvas::fillRoundedRect(float x, float y, float width, float height, float radius, ChromeColor color) {
    const Span span = spanOf(x, y, width, height, 1, width_, height_);
    for (u32 row = span.y0; row < span.y1; ++row) {
        for (u32 column = span.x0; column < span.x1; ++column) {
            const float distance = chromeRoundedRectDistance(column + 0.5f, row + 0.5f, x, y, width, height, radius);
            blend(column, row, color, 0.5f - distance);
        }
    }
}

void ChromeCanvas::strokeRoundedRect(float x, float y, float width, float height, float radius, float thickness, ChromeColor color) {
    const Span span = spanOf(x, y, width, height, 1, width_, height_);
    for (u32 row = span.y0; row < span.y1; ++row) {
        for (u32 column = span.x0; column < span.x1; ++column) {
            const float distance = chromeRoundedRectDistance(column + 0.5f, row + 0.5f, x, y, width, height, radius);
            // Inside the outer edge and outside the inner one.
            const float outer = clamp01(0.5f - distance);
            const float inner = clamp01(0.5f - (distance + thickness));
            blend(column, row, color, outer - inner);
        }
    }
}

void ChromeCanvas::fillCircle(float centreX, float centreY, float radius, ChromeColor color) {
    fillRoundedRect(centreX - radius, centreY - radius, radius * 2, radius * 2, radius, color);
}

void ChromeCanvas::shadow(float x, float y, float width, float height, float radius, float blur, ChromeColor color) {
    const float spread = blur < 1 ? 1 : blur;
    const Span span = spanOf(x, y, width, height, spread, width_, height_);
    for (u32 row = span.y0; row < span.y1; ++row) {
        for (u32 column = span.x0; column < span.x1; ++column) {
            const float distance = chromeRoundedRectDistance(column + 0.5f, row + 0.5f, x, y, width, height, radius);
            if (distance >= spread) {
                continue;
            }
            // A smoothstep from the edge out to `blur`: close to what a
            // Gaussian of the shape looks like, at a fraction of the cost.
            float t = distance <= 0 ? 0 : distance / spread;
            t = 1.0f - t;
            blend(column, row, color, t * t * (3.0f - 2.0f * t));
        }
    }
}

void ChromeCanvas::punchRoundedRect(float x, float y, float width, float height, float radius) {
    const Span span = spanOf(x, y, width, height, 1, width_, height_);
    for (u32 row = span.y0; row < span.y1; ++row) {
        for (u32 column = span.x0; column < span.x1; ++column) {
            const float distance = chromeRoundedRectDistance(column + 0.5f, row + 0.5f, x, y, width, height, radius);
            const float keep = 1.0f - clamp01(0.5f - distance);
            if (keep >= 1) {
                continue;
            }
            u8* const p = pixels_.mutData() + ((size_t)(row) * width_ + column) * 4;
            for (u32 channel = 0; channel < 4; ++channel) {
                p[channel] = toByte(p[channel] / 255.0f * keep);
            }
        }
    }
}

void ChromeCanvas::blendMask(i32 x, i32 y, const u8* mask, u32 width, u32 height, u32 stride, ChromeColor color, i32 clipRight) {
    const i32 right = clipRight < (i32)(width_) ? clipRight : (i32)(width_);
    for (u32 row = 0; row < height; ++row) {
        const i32 targetY = y + (i32)(row);
        if (targetY < 0 || targetY >= (i32)(height_)) {
            continue;
        }
        for (u32 column = 0; column < width; ++column) {
            const i32 targetX = x + (i32)(column);
            if (targetX < 0 || targetX >= right) {
                continue;
            }
            const u8 coverage = mask[(size_t)(row) * stride + column];
            if (coverage != 0) {
                blend((u32)(targetX), (u32)(targetY), color, coverage / 255.0f);
            }
        }
    }
}
