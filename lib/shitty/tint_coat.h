/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/color.h>

#include <std/sys/types.h>

// C10. The thinnest coat of paint that still shows a wanted colour when
// it is laid over a known backdrop.
//
// Why a terminal needs such a thing. The sidebar's panel is drawn on a
// layer that sits over the CAMetalLayer, and the renderer has already
// painted that strip: it is chrome reserve, outside every pane, so what
// lies under the panel is the frame clear - the terminal's own
// background, at the terminal's own alpha. Painting the panel's colour
// over that at the same alpha does not match the terminal, it stacks on
// it: alpha a over alpha a composites to 2a - a², which is 0.75 where
// the window is 0.5, and the panel reads as a more solid thing than the
// window it belongs to (S10 found this by arithmetic before a user
// could find it by looking).
//
// There is no coat that reaches exactly alpha a - the backdrop already
// supplies a, and anything laid over it only adds. So the question is
// not "how do I match" but "how little can I add and still land on the
// colour I was asked for", and that has an exact answer: push the coat's
// own colour as far toward the edge of the range as it will go, and the
// alpha needed to reach the target falls in proportion.
//
// The default is the case that shows it is the right generalisation and
// not a trick. The panel defaults to six percent of the foreground mixed
// into the background; asked for that colour over that background, this
// returns a coat of roughly the foreground at roughly six percent -
// which is what the sidebar painted by hand before this function
// existed.
//
// The other end is a property of the request, not of this code, and the
// option's documentation says so: a colour far from the terminal's
// background cannot be reached by a thin coat at all, so asking for one
// necessarily covers more of the desktop.

struct TintCoat {
    // The colour to paint. Pushed away from the target, toward whichever
    // end of the range the target sits on relative to the backdrop -
    // that is the whole of how the alpha is made small.
    Color color;
    // 0..255. Zero means the target already is the backdrop and there is
    // nothing to paint.
    u8 alpha;
};

// How much room a channel has to move in the direction it needs to go.
// Zero only when it does not need to move at all: a target channel can
// never be further from the backdrop than the range allows.
inline constexpr u32 tintHeadroom(u8 backdrop, u8 target) {
    return target >= backdrop ? (u32)(255 - backdrop) : (u32)(backdrop);
}

inline constexpr u32 tintDistance(u8 backdrop, u8 target) {
    return target >= backdrop ? (u32)(target - backdrop) : (u32)(backdrop - target);
}

inline constexpr TintCoat thinnestCoat(Color target, Color backdrop) {
    const u8 backdropChannels[3] = {backdrop.red, backdrop.green, backdrop.blue};
    const u8 targetChannels[3] = {target.red, target.green, target.blue};

    // The alpha is the largest any one channel demands: a coat thinner
    // than that cannot carry the channel that has the furthest to go,
    // whatever colour it is painted in.
    u32 alpha = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const u32 headroom = tintHeadroom(backdropChannels[channel], targetChannels[channel]);
        if (headroom == 0) {
            continue;
        }
        const u32 needed = (tintDistance(backdropChannels[channel], targetChannels[channel]) * 255 + headroom - 1) / headroom;
        alpha = needed > alpha ? needed : alpha;
    }
    if (alpha == 0) {
        // The target is the backdrop. Nothing to paint, and the colour
        // is arbitrary - the target itself is the least surprising one
        // for anyone who reads it in a debugger.
        return {target, 0};
    }
    if (alpha > 255) {
        alpha = 255;
    }

    // And the colour that, at that alpha, lands on the target: the
    // distance the channel has to cover, divided by how much of it the
    // coat actually carries.
    u8 coat[3] = {0, 0, 0};
    for (unsigned channel = 0; channel < 3; ++channel) {
        const u8 base = backdropChannels[channel];
        const u32 distance = tintDistance(base, targetChannels[channel]);
        const u32 moved = (distance * 255 + alpha / 2) / alpha;
        if (targetChannels[channel] >= base) {
            const u32 raised = (u32)(base) + moved;
            coat[channel] = (u8)(raised > 255 ? 255 : raised);
        } else {
            coat[channel] = (u8)(moved >= (u32)(base) ? 0 : (u32)(base) - moved);
        }
    }
    return {{coat[0], coat[1], coat[2]}, (u8)(alpha)};
}

// What a coat actually shows when laid over an opaque backdrop - the
// inverse of the above, and the only honest way to assert that the pair
// is right: a test that recomputed the coat with the coat's own formula
// would be a restatement.
inline constexpr Color coatOverOpaque(TintCoat coat, Color backdrop) {
    const u8 coatChannels[3] = {coat.color.red, coat.color.green, coat.color.blue};
    const u8 backdropChannels[3] = {backdrop.red, backdrop.green, backdrop.blue};
    u8 result[3] = {0, 0, 0};
    for (unsigned channel = 0; channel < 3; ++channel) {
        const u32 blended = (u32)(coatChannels[channel]) * coat.alpha + (u32)(backdropChannels[channel]) * (255u - coat.alpha) + 127u;
        result[channel] = (u8)(blended / 255u);
    }
    return {result[0], result[1], result[2]};
}
