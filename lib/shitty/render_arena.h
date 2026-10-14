/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

#include <stddef.h>

// A3: the device-side mirror of one glyph strip arena.
//
// There is one SpanShaper per window (Composer::shaper), so there is one
// arena per plane for the whole window, and the strip offsets every pane
// hands out are already offsets into it. A pane therefore owns no range
// of its own and needs no base: the mirror tracks the window's single
// arena and nothing else.
//
// The arena is append-only inside a generation and dies whole when the
// generation moves (a font change, or the shaper outgrowing its budget
// and resetting). That is the entire contract this mirror encodes: send
// the tail while the generation holds, send everything when it moves.
//
// This is where the arena mirroring got smaller, and it is worth saying
// why, because the shape it replaced was deliberate and the reasoning
// that made it deliberate no longer holds. It used to key a range per
// pane on the pair "pane identity + generation", because generations
// came from a per-screen scheme that was one race away from handing two
// screens the same number, and a renderer that trusted the number alone
// would leave one pane drawn from another's bytes. With one shaper there
// is one arena, one generation and nothing to confuse it with.
//
// So the assumption did not disappear, it moved, and the new one is
// narrower and unguarded by types: **one shaper per window**. Give a
// window two of them - a second shaper for an overlay, a per-pane shaper
// for a pane with its own font - and this mirror is wrong again, in the
// same silent way: two arenas, two generation sequences, one mirror
// reading "nothing moved" for bytes that are somewhere else. Whoever
// adds the second shaper owns this file.
//
// The related assumption underneath it - that row identities come from
// one process-wide sequence rather than one per screen, so that the
// window's single shaper cannot cache two different rows under one key -
// is guarded by a test: Screen::RowIdentitiesNeverRepeatAcrossScreens in
// screen_ut.cpp.

// The half-open run of the arena the device still owes: copy [from, to)
// to the same place in the device buffer. Empty (from == to) when the
// mirror is already current.
struct ArenaCopy {
    size_t from = 0;
    size_t to = 0;
};

struct ArenaMirror {
    // One call per frame, per plane. `generation` is the shaper
    // generation the frame's strips were assigned in, `used` how much of
    // the arena is filled in that plane's unit - bytes for the mask
    // plane, u32 pixels for the color plane. The mirror only adds and
    // compares, so it does not care which; the caller keeps one mirror
    // per plane and stays in that plane's unit throughout.
    //
    // The plan assumes the copy it names is then made. A caller that
    // cannot make it (an allocation failed, the buffer was replaced and
    // lost its contents) calls reset() so the next frame starts from
    // nothing rather than from a mirror that was never written.
    ArenaCopy plan(u32 generation, size_t used);

    // Forget the mirror: the device buffer no longer holds what the last
    // plan said it holds.
    void reset();

    u32 generation = 0;
    // How much of the arena the device holds. Zero is "nothing", which
    // is why reset() needs no flag of its own: a mirror that owes from
    // zero owes the whole arena, whatever the generation says.
    size_t uploaded = 0;
};

inline void ArenaMirror::reset() {
    generation = 0;
    uploaded = 0;
}

inline ArenaCopy ArenaMirror::plan(u32 frameGeneration, size_t used) {
    // A generation that moved means every strip in the arena moved, so
    // last frame's tail names bytes that are no longer there.
    //
    // `uploaded > used` cannot happen inside a generation - the arena
    // only grows until it dies whole - so it means a generation went by
    // unseen, and the whole arena is owed. Sending it is one compare's
    // worth of insurance against a copy that reads past the end.
    const size_t from = generation == frameGeneration && uploaded <= used ? uploaded : 0;
    generation = frameGeneration;
    uploaded = used;
    return {from, used};
}
