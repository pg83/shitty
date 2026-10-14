/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/color.h>

#include <std/sys/types.h>

// T10. The arithmetic a translucent background is composed with, in one
// place because three of them have to agree about it: the reference
// renderer computes it here, render.comp computes the same thing in
// floats, and the two clear paths (render_metal.mm, render_vk.cpp)
// premultiply the frame's clear colour with the helpers below.
//
// **Everything here is premultiplied alpha**, and that is the whole
// point of the file. Core Animation reads a layer's contents as
// premultiplied and so does every compositor these backends present to;
// a colour written straight, with the alpha merely attached, is
// interpreted as though it had already been multiplied down - so the
// background reads back brighter than it should exactly where it is most
// transparent, and the edge pixels of every glyph, which are part
// foreground and part background, wear a halo of that error. Light text
// on a dark background gets a dark rim, dark text on a light one gets a
// light rim, and the brighter the desktop behind the window, the more it
// shows. It is one multiplication, and it belongs in the same expression
// as the alpha it pairs with, which is why both come out of one call
// below rather than being computed apart and assembled by the caller.

// The 0..100 the option is spelled in, as the 0..255 alpha the blending
// wants. 100 has to land on exactly 255 - it is the default, and every
// helper here has to be the identity at it, or a fork whose upstream has
// no such option would start drawing differently for everyone.
inline constexpr u8 backgroundAlphaFromPercent(u16 percent) {
    return (u8)(((u32)(percent < 100 ? percent : 100) * 255 + 50) / 100);
}

// One channel multiplied down by an alpha. Exact at 255: 255*a + 127
// divided by 255 is a for every a, so an opaque background survives this
// byte for byte.
inline constexpr u8 premultiplyChannel(u8 channel, u8 alpha) {
    return (u8)(((u32)(channel) * (u32)(alpha) + 127) / 255);
}

inline constexpr Color premultiply(Color color, u8 alpha) {
    return {
        premultiplyChannel(color.red, alpha),
        premultiplyChannel(color.green, alpha),
        premultiplyChannel(color.blue, alpha),
    };
}

// The coverage-weighted mix the reference renderer has always used,
// lifted here so the blend below and its callers share one rounding.
inline constexpr u8 mixChannel(u8 foreground, u8 background, u8 coverage) {
    return (u8)(((u32)(foreground) * (u32)(coverage) + (u32)(background) * (u32)(255 - coverage) + 127) / 255);
}

struct BlendedPixel {
    // Already multiplied by alpha below. Not the colour you would show a
    // user: at alpha 0 it is black whatever the background was.
    Color color;
    u8 alpha;
};

// An opaque foreground covering `coverage` of a background that is only
// `backgroundAlpha` opaque - the "over" composite, in premultiplied
// form. The foreground's own alpha is 255 and does not appear as a
// factor; that is why the colour term reads fg*c + bg*a*(1-c) and the
// alpha term reads 1*c + a*(1-c).
//
// At backgroundAlpha 255 this is the plain mix it replaced, byte for
// byte, with alpha 255 - which is what keeps the default free.
inline constexpr BlendedPixel blendOverBackground(Color foreground, Color background, u8 coverage, u8 backgroundAlpha) {
    const Color premultiplied = premultiply(background, backgroundAlpha);
    return {
        {
            mixChannel(foreground.red, premultiplied.red, coverage),
            mixChannel(foreground.green, premultiplied.green, coverage),
            mixChannel(foreground.blue, premultiplied.blue, coverage),
        },
        mixChannel(255, backgroundAlpha, coverage),
    };
}
