/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "chrome_canvas.h"

#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class Buffer;
}

// Text for the window's chrome, set the way the system sets it: a
// proportional face, shaped by HarfBuzz and drawn by FreeType onto a
// ChromeCanvas - not the terminal's grid font snapped to cells. Linux only,
// and only with FreeType and HarfBuzz; elsewhere open*() fails and the
// chrome draws no text.
class UiText {
public:
    UiText();
    ~UiText();
    UiText(const UiText&) = delete;
    UiText& operator=(const UiText&) = delete;

    bool openFile(const char* path, i32 index, float pixelSize);
    bool openMemory(const void* data, size_t size, i32 index, float pixelSize);
    bool ready() const;

    float ascent() const;
    float descent() const;
    // The advance of the whole string.
    float measure(stl::StringView utf8);
    // Draws with the baseline at `baseline`. Past `maxWidth` the string is
    // cut at a character and ends in an ellipsis. Returns the width drawn.
    float draw(ChromeCanvas& canvas, float x, float baseline, stl::StringView utf8, ChromeColor color, float maxWidth);

private:
    struct Impl;
    Impl* impl_;
};

// The desktop's interface face, `sans-serif` as fontconfig resolves it, in
// the regular or the semibold weight. False without fontconfig.
bool findUiFont(bool bold, stl::Buffer& path, i32& index);
