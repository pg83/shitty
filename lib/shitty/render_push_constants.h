/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

// The block render.comp reads, in its declaration order. One definition
// for both GPU backends, because they compile the same shader source -
// spirv-cross hands Metal what Vulkan gets - and so they cannot disagree
// about this block without one of them reading garbage.
//
// R9-4, the invariant this whole arrangement rests on: **every member is
// a four-byte scalar**. That is the narrow reason one C++ struct can
// serve two different APIs at all - on a block of nothing but four-byte
// scalars, std140, std430 and scalar layouts agree, so Vulkan's push
// constants and Metal's setBytes see the same bytes at the same offsets.
// Add a vec2, a vec4, an array or a nested struct and they stop agreeing:
// std140 rounds an array's elements up to sixteen bytes and aligns a vec4
// to sixteen, and this definition would then be right for at most one
// backend.
//
// The assertion below guards it only in part. sizeof() alone would not:
// thirty scalars and one eight-byte member weigh the same 128 bytes and
// lay out differently. alignof() closes most of that gap - an eight-byte
// scalar or a vec2 would pull the struct's alignment past four - but no
// assertion here catches an array of u32, so the rule above is the thing
// to keep, and these two only make the common ways of breaking it loud.
//
// R9-1 is why it lives here rather than once per backend. It used to be
// declared twice, with a sizeof() assertion each, and the two were meant
// to guard exactly this. Adding four fields updated one assertion and
// left the other naming the old size: the guard was itself the thing
// that drifted, and it drifted silently because only one of the two
// backends compiles on the machine the change was written on. One
// definition and one assertion cannot do that.
struct GpuPushConstants {
    u32 glyphWidth;
    u32 glyphHeight;
    float boxDrawingStroke;
    u32 columns;
    u32 rows;
    // The far edge of the pane being drawn, not of the surface: the
    // first pixel column and row this dispatch may not touch.
    u32 outputWidth;
    u32 outputHeight;
    // Where cell 0,0 starts on the output - the pane's rectangle plus
    // the content insets.
    u32 originX;
    u32 originY;
    u32 cursorColor;
    i32 cursorX;
    i32 cursorY;
    u32 cursorStyle;
    u32 screenReverseVideo;
    i32 selectionLeft;
    i32 selectionTop;
    i32 selectionRight;
    i32 selectionBottom;
    // Two flags, rectangularSelectionBit and cursorKeepSelectionFgBit
    // below. The second used to be a field of its own upstream; here the
    // block is already at the 128 bytes R9-2 is about, so it rides in
    // the word the selection's shape already had.
    u32 selectionFlags;
    u32 showWraps;
    u32 selectionForeground;
    u32 selectionBackground;
    u32 selectionColorMask;
    u32 blinkVisible;
    u32 cursorBlink;
    u32 hoveredHyperlink;
    u32 hoveredLinkBegin;
    u32 hoveredLinkEnd;
    u32 updateCount;
    // F9: the pane's near edge. outputWidth/outputHeight above are its
    // far edge and originX/originY are where its grid starts inside it;
    // what was missing was where the pane itself starts, without which
    // the padding between the two cannot be named.
    u32 paneLeft;
    u32 paneTop;
    // The colour the fill pass paints, packed in the low 24 bits - and
    // the fill-pass flag in bit 24.
    //
    // R9-2 is why the flag lives here instead of in a field of its own.
    // Vulkan guarantees maxPushConstantsSize of only 128 bytes, and
    // devices are entitled to offer exactly that; a field of its own put
    // this block at 132 and over the floor. The flag is one bit and a
    // packed colour has a whole spare byte above it, so it costs nothing
    // to carry it there - and unpackColor() reads three 8-bit fields, so
    // the bit cannot leak into any channel.
    u32 paneBackgroundAndFill;
};

// Exactly the floor Vulkan guarantees. Not a coincidence and not slack
// to spend: a field added here without one removed puts the block over
// the smallest maxPushConstantsSize the specification allows, on devices
// that are within their rights to offer no more.
// The fill-pass flag, in the byte a packed 24-bit colour leaves spare.
//
// This is the one place the bit number is decided. render.comp reads it
// through @FILL_PASS_BIT@, which generate_render_shaders.py fills in from
// this line - so moving the flag moves both sides at once. It was written
// twice once (R9-3), and that version compiled cleanly while the shader
// went on reading the abandoned bit: the pane fill would have stopped
// happening, silently, and no sizeof() assertion knows about bit numbers.
constexpr u32 fillPassBit = 1u << 24;

// The bits of selectionFlags. Decimal, because generate_render_shaders.py
// reads them off these lines for the shader, for R9-3's reason.
constexpr u32 rectangularSelectionBit = 1;
// The option cursorKeepSelectionFg: a block cursor over a selected or
// reverse-video cell keeps that cell's foreground.
constexpr u32 cursorKeepSelectionFgBit = 2;

// T10. The background's *transparency*, in percent, in the seven bits
// left above the flag - the last of the spare byte a packed 24-bit
// colour leaves, and the only room this block has. R9-2 says why there
// is no field to be had: this struct sits exactly on the 128 bytes
// Vulkan guarantees, and one more field puts it over on devices that
// are within their rights to offer no more.
//
// **Transparency and not opacity**, which is the whole of the encoding
// decision. Both would fit; only one of them makes the all-zero field
// mean what this terminal did before the option existed. The seam
// between two panes is painted by the same fill pass as a pane, out of
// a `PushConstants band{}` that is zero-initialised and fills in three
// fields by hand - so under an opacity encoding, forgetting the fourth
// makes the divider fully transparent, silently, in exactly the frames
// nobody looks at twice. Under this one it stays solid. The default
// value of a bit field is not a detail here; it is the failure mode.
//
// render.comp reads the shift and the width through
// @BACKGROUND_TRANSPARENCY_SHIFT@ and @BACKGROUND_TRANSPARENCY_BITS@,
// filled in by generate_render_shaders.py from these two lines, for the
// reason R9-3 records: a bit number written twice compiles either way
// and leaves the shader reading a field nobody sets.
constexpr u32 backgroundTransparencyShift = 25;
constexpr u32 backgroundTransparencyBits = 7;

// The colour a fill pass paints, with the background transparency the
// 0..100 opacity option implies. One function so the three call sites
// cannot each derive the complement their own way.
constexpr u32 packPaneBackground(u32 packedColor, u32 opacityPercent) {
    const u32 opacity = opacityPercent < 100u ? opacityPercent : 100u;
    return (packedColor & 0xffffffu) | ((100u - opacity) << backgroundTransparencyShift);
}

// The panel's rounded corners on a window this program draws itself
// (ui_wayland_chrome.cpp): the fill pass run over a corner's square,
// masking what lies outside the arc to transparent and leaving the inside
// alone. Marked by `updateCount`, which the fill pass does not read and
// which no real frame could carry - a fill copies the whole block, so a
// marker in any field a frame does set would turn every fill into a
// corner. The corner's radius in pixels rides in `glyphWidth`, and which
// corner in `glyphHeight`: 0 the bottom-left, 1 the bottom-right.
//
// Decimal, because generate_render_shaders.py reads it off this line for
// the shader (@CORNER_PASS_MARKER@) - one definition, for R9-3's reason.
constexpr u32 cornerPassMarker = 3735928559;

// The three ways the packing above can be wrong, each of which compiles.
static_assert(((1u << backgroundTransparencyBits) - 1u) >= 100u, "the transparency field must hold 0..100");
static_assert(backgroundTransparencyShift + backgroundTransparencyBits <= 32u, "the transparency field must fit the word");
static_assert((((1u << backgroundTransparencyBits) - 1u) << backgroundTransparencyShift & (0xffffffu | fillPassBit)) == 0u, "the transparency field must not reach the packed colour or the fill-pass flag");
// And the property the encoding exists for: an untouched field is the
// opaque background this terminal drew before the option existed.
static_assert(packPaneBackground(0x123456u, 100u) == 0x123456u, "an opaque background must leave the field zero");

static_assert(sizeof(GpuPushConstants) == 128, "GPU push constant block must fit the 128-byte floor Vulkan guarantees");
// And nothing in it is wider than four bytes: see R9-4 above for why the
// one definition is only valid while that holds.
static_assert(alignof(GpuPushConstants) == 4, "GPU push constant block must be four-byte scalars only, or the two APIs lay it out differently");
