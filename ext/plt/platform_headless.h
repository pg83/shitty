#pragma once

#include "platform.h"
#include "window.h"

namespace plt {
    enum class HeadlessPixelFormat : u8 {
        RGB8
    };

    struct HeadlessRenderTarget {
        u8* pixels = nullptr;
        size_t length = 0;
        u32 width = 0;
        u32 height = 0;
        u32 stride = 0;
        HeadlessPixelFormat format = HeadlessPixelFormat::RGB8;
    };

    struct HeadlessFrame {
        const u8* pixels = nullptr;
        size_t length = 0;
        u32 width = 0;
        u32 height = 0;
        u32 stride = 0;
        HeadlessPixelFormat format = HeadlessPixelFormat::RGB8;
        u64 generation = 0;
    };

    // The width/height pair of a sizing request, kept as a pair so a
    // test can tell a swapped one from a correct one. plt::Window takes
    // these as two scalars of the same type, so a call site that hands
    // them over the wrong way round compiles, runs, and - on every
    // backend but a recording one - is invisible: nothing in the tree
    // reads the request back. See ApplicationImpl::fontChanged(), whose
    // arithmetic was covered by grid_geometry_ut.cpp long before the
    // hand-off it feeds was.
    struct WindowSizeRequest {
        u32 width = 0;
        u32 height = 0;
        u64 count = 0;
    };

    struct WindowResizeUnitRequest {
        u32 width = 0;
        u32 height = 0;
        u32 baseWidth = 0;
        u32 baseHeight = 0;
        u64 count = 0;
    };

    // Where the input method is told to put its candidate window, in
    // backing pixels. Same reason as the two above and one more: the
    // origin is a point on the surface, so an x taken from the vertical
    // inset instead of the horizontal one is a plain transposition, and
    // it was invisible while the two insets were the same number. They
    // stopped being the same number when window chrome started
    // reserving one edge at a time.
    struct WindowTextInputRect {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
        u64 count = 0;
    };

    struct WindowHeadless: Window {
        // Dispatches one requested frame through WindowOptions::frame. The
        // callback is deliberately never made inline from requestFrame().
        virtual bool dispatchFrame() = 0;
        virtual bool framePending() const = 0;
        virtual void configure(const WindowInfo& info) = 0;
        virtual void failNextPresentation() = 0;
        virtual HeadlessFrame presentedFrame() const = 0;
        virtual void setClipboards(Clipboard& primary, Clipboard& secondary) = 0;

        // Requests recorded for tests instead of reaching a real desktop.
        virtual WindowSizeRequest requestedMinimumSize() const = 0;
        virtual WindowResizeUnitRequest requestedResizeUnit() const = 0;
        virtual WindowTextInputRect requestedTextInputRect() const = 0;
        virtual PointerIcon pointerIcon() const = 0;
        virtual stl::StringView openedUri() const = 0;
        virtual u64 openUriCount() const = 0;
        virtual stl::StringView title() const = 0;
    };

    Platform* createHeadlessPlatform(stl::ObjPool& owner);
}
