#pragma once

#include "clipboard.h"
#include "input.h"

#include <std/str/view.h>
#include <std/sys/types.h>

namespace plt {
    struct DropTarget;
    struct InputSink;

    enum class RenderBackend : u8 {
        Wayland,
        Cocoa,
        Headless
    };

    struct RenderContext {
        RenderBackend backend;
        void* connection;
        void* window;
    };

    // The union of the wp_cursor_shape_device_v1 shapes and the public
    // NSCursor cursors, collapsed where both platforms mean the same thing
    // (pointer covers pointingHandCursor, grab covers openHandCursor, and so
    // on). A backend without a native cursor for a value substitutes the
    // closest one it has.
    enum class PointerIcon : u8 {
        Default,
        ContextMenu,
        Help,
        Pointer,
        Progress,
        Wait,
        Cell,
        Crosshair,
        Text,
        VerticalText,
        Alias,
        Copy,
        Move,
        NoDrop,
        NotAllowed,
        Grab,
        Grabbing,
        ResizeEast,
        ResizeNorth,
        ResizeNorthEast,
        ResizeNorthWest,
        ResizeSouth,
        ResizeSouthEast,
        ResizeSouthWest,
        ResizeWest,
        ResizeEastWest,
        ResizeNorthSouth,
        ResizeNorthEastSouthWest,
        ResizeNorthWestSouthEast,
        ResizeColumn,
        ResizeRow,
        AllScroll,
        ZoomIn,
        ZoomOut,
        DndAsk,
        ResizeAll,
        DisappearingItem
    };

    // Where requestShowAt() places the window. TopOfActiveScreen is the
    // quick-terminal placement: a rect on the screen the pointer is
    // currently on, sized and positioned by WindowOptions::quickGeometry
    // (default full visibleFrame width, top-aligned, 40% of its height -
    // the placement's original, unconfigurable shape). The backend
    // resolves the rect against the actual screen.
    enum class ShowPlacement : u8 {
        Centered,
        TopOfActiveScreen
    };

    // One component of a quickGeometry spec ("<W>x<H>+<X>+<Y>"): either
    // an absolute pixel count or a percentage (0..100) of the target
    // screen's visibleFrame extent along that axis. Resolved against a
    // live screen only by the backend that implements
    // ShowPlacement::TopOfActiveScreen (Cocoa today); parsing and range
    // validation live in lib/shitty/quick_geometry.{h,cpp} so the
    // grammar is unit-testable without a live NSScreen.
    struct QuickGeometryDim {
        bool percent = false;
        u32 value = 0;
    };

    // Parsed quickGeometry option. width/height are the window's size;
    // x/y are its offset from the visibleFrame's top-left corner. The
    // defaults reproduce ShowPlacement::TopOfActiveScreen's original,
    // unconfigurable shape: full width, top-aligned, 40% height.
    struct QuickGeometry {
        QuickGeometryDim width{.percent = true, .value = 100};
        QuickGeometryDim height{.percent = true, .value = 40};
        QuickGeometryDim x{.percent = false, .value = 0};
        QuickGeometryDim y{.percent = false, .value = 0};
    };

    struct WindowInfo {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
        u32 screenPixelWidth = 0;
        u32 screenPixelHeight = 0;
        float contentScale = 1.0f;
        bool focused = false;
        bool iconified = false;
        bool maximized = false;
        bool fullscreen = false;
        bool tiled = false;
    };

    struct WindowEvents {
        virtual void close() = 0;
    };

    struct FrameCallback {
        // Returns true when a frame was submitted for presentation.
        virtual bool frame(const WindowInfo& info) = 0;
    };

    // What a backend is asked to put behind a window whose background is
    // translucent. None creates no backdrop at all; Blur is a frosted
    // pane of the desktop; Glass is the system's own glass material.
    //
    // Deliberately a separate type from lib/shitty's BackdropMode, even
    // though the two spell the same three modes today. The option says
    // what the user asked for and is parsed, dumped and documented as
    // such; this says what a backend is being asked to produce. The two
    // already disagree in one direction that matters: only the backend
    // knows whether the system it is running on has glass at all, so
    // Glass falling back to Blur happens here, below this line, and not
    // in the caller - the caller has no way to ask.
    enum class Backdrop : u8 {
        None,
        Blur,
        Glass
    };

    // Window chrome the application draws itself (WindowOptions::clientChrome):
    // what happens on it, and what the window around the content became.
    // Coordinates are logical, relative to the toplevel's top-left corner -
    // the outer margin included - whichever layer the pointer is over.
    struct ChromeEvent {
        enum class Kind : u8 {
            Enter,
            Leave,
            Motion,
            Press,
            Release,
            // The toplevel's size, scale or state changed: redraw.
            Changed
        };
        Kind kind = Kind::Changed;
        // 0 is the toplevel's own surface, 1 the overlay above the content,
        // 3 the menu above both. A Leave from layer 2 says the pointer went
        // over the content; a Press from layer 2, that it was pressed there
        // while the chrome held the keys (ChromeSink::capturesKeys()) - the
        // content gets the press too.
        u8 layer = 0;
        float x = 0;
        float y = 0;
        u32 button = 0;
        u32 clicks = 0;
    };

    struct ChromeSink {
        virtual void chrome(const ChromeEvent& event) = 0;
        // While this says yes, the window's keys come here instead of to
        // the content: presses and repeats as keys, what they type as text
        // (none while Control or Super is held). Releases go nowhere.
        virtual bool capturesKeys() {
            return false;
        }
        virtual void chromeKey(const KeyInput& key) {
            (void)(key);
        }
        virtual void chromeText(u32 codepoint) {
            (void)(codepoint);
        }
    };

    struct ChromeState {
        // The toplevel surface in logical units, outer margin included, and
        // in the pixels a buffer for it must have.
        u32 width = 0;
        u32 height = 0;
        u32 pixelWidth = 0;
        u32 pixelHeight = 0;
        float scale = 1.0f;
        bool focused = false;
        bool maximized = false;
        bool fullscreen = false;
        bool tiled = false;
    };

    // Resize edges for WindowChrome::startResize, combinable.
    enum : u32 {
        ChromeEdgeTop = 1,
        ChromeEdgeBottom = 2,
        ChromeEdgeLeft = 4,
        ChromeEdgeRight = 8
    };

    // The window around the content, drawn by the application. The content -
    // everything Window::info() and the renderer know about - sits inside the
    // toplevel at the insets; the toplevel is the content plus the insets,
    // and the outer margin (part of each inset) is outside the window as the
    // shell sees it, room for a shadow and for resize handles. Pictures are
    // premultiplied 32-bit pixels, B G R A in memory.
    struct WindowChrome {
        virtual void setSink(ChromeSink* sink) = 0;
        virtual void setInsets(u32 left, u32 top, u32 right, u32 bottom, u32 margin) = 0;
        virtual ChromeState state() const = 0;
        // Layer 0 covers the whole toplevel; layer 1, the overlay, the
        // rectangle setOverlay() gave it, over the content; layer 3, the
        // menu, the rectangle setMenu() gave it, over everything.
        virtual void present(u8 layer, const u8* pixels, u32 pixelWidth, u32 pixelHeight) = 0;
        virtual void setOverlay(bool shown, i32 x, i32 y, u32 width, u32 height) = 0;
        virtual void setMenu(bool shown, i32 x, i32 y, u32 width, u32 height) = 0;
        virtual void setCursor(PointerIcon icon) = 0;
        virtual void startMove() = 0;
        virtual void startResize(u32 edges) = 0;
    };

    struct WindowOptions {
        stl::StringView appId = {};
        stl::StringView title = {};
        u32 width = 800;
        u32 height = 600;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        bool decorations = true;
        // The application draws the window around its content itself
        // (Window::chrome()). Wayland only, and only where the compositor
        // offers subsurfaces and shared memory; elsewhere chrome() stays
        // null and the window is what it always was.
        bool clientChrome = false;
        // The titlebar's color matches the background the window is
        // created with, instead of the system chrome color. Cocoa-only;
        // Wayland has no titlebar of its own to recolor.
        bool transparentTitlebar = false;
        // The quick-terminal window: floats above other windows and
        // fullscreen spaces, and starts hidden - the caller shows it
        // itself through requestShowAt() once a hotkey fires. Cocoa-only
        // today; Wayland has no global hotkey path to trigger it.
        bool quick = false;
        // The rect requestShowAt(TopOfActiveScreen) resolves against the
        // active screen. Only consumed where TopOfActiveScreen actually
        // computes a placement (Cocoa today); ignored elsewhere, same as
        // quick above.
        QuickGeometry quickGeometry{};
        // Corner radius of the quick-terminal window's content layer, in
        // points; 0 keeps square corners. Cocoa-only, applied at window
        // creation time - Wayland has no equivalent compositor hook for an
        // undecorated window and ignores it, same as transparentTitlebar.
        u16 quickCornerRadius = 0;
        // How opaque the terminal background is, 0..100; 100 keeps the
        // solid window. Below 100 the window frame and the content layer
        // are made transparent at creation time so the alpha the
        // renderer writes reaches the compositor at all. Cocoa-only,
        // same scope as transparentTitlebar: Wayland needs a
        // composite-alpha swapchain of its own for this and ignores it.
        //
        // Creation time and not later on purpose. This is the one place
        // the decision is taken, and the renderer reads it back off the
        // live layer rather than off the option, so the two cannot
        // disagree about a window that already exists.
        u16 backgroundOpacity = 100;
        // What to put behind whatever shows through a translucent
        // background. Ignored while backgroundOpacity is 100 - nothing
        // would be visible through an opaque background, and an
        // invisible backdrop costs the compositor a pass per frame.
        // Cocoa-only.
        Backdrop backdrop = Backdrop::None;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        // Null leaves the window rejecting every drag.
        DropTarget* drop = nullptr;
        // Encoded image bytes (PNG) for the application icon; empty keeps
        // the platform default. Cocoa sets the Dock icon from it, Wayland
        // has no icon protocol and ignores it.
        stl::StringView icon = {};
        // The human-visible application name. Cocoa pushes it to Launch
        // Services so the menu bar of an unbundled binary shows it
        // instead of argv[0]; Wayland ignores it (appId serves the
        // shell). The Cmd-Tab switcher is beyond reach: its label comes
        // from the application bundle, which a bare executable lacks.
        stl::StringView appName = {};
    };

    struct Window {
        virtual void requestShow() = 0;
        // Hides the window without destroying it (orderOut: on Cocoa);
        // the counterpart requestShow()/requestShowAt() bring it back.
        virtual void requestHide() = 0;
        // Like requestShow(), but places the window per placement
        // instead of always centering it.
        virtual void requestShowAt(ShowPlacement placement) = 0;
        virtual void requestClose() = 0;
        virtual void requestFrame() = 0;

        virtual void requestTitle(stl::StringView title) = 0;
        virtual void requestAttention() = 0;
        virtual void requestRestore() = 0;
        virtual void requestIconify() = 0;
        virtual void requestMove(i32 x, i32 y) = 0;
        virtual void requestFocus() = 0;
        virtual void requestMaximized(bool maximized) = 0;
        virtual void requestFullscreen(bool fullscreen) = 0;
        // Rounds (radius > 0) or squares (radius == 0) the window's own
        // corners, in points - not the one-shot construction-time
        // WindowOptions::quickCornerRadius, but a live toggle: the quick
        // window's geometric fullscreen chord (ui_quick_hotkey.mm) calls
        // this to square the corners before expanding over the whole
        // screen and restore them after collapsing back, so a
        // "fullscreen" window never shows desktop through masked
        // corners. Cocoa-only, same scope as quickCornerRadius itself -
        // Wayland and the headless backend accept the call and do
        // nothing.
        virtual void requestCornerRadius(u16 radius) = 0;
        virtual void requestResize(u32 width, u32 height) = 0;
        virtual void requestMinimumSize(u32 width, u32 height) = 0;
        virtual void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) = 0;

        // The primary selection. On macOS it maps to the Find pasteboard: the
        // platform has no primary selection, and the Find pasteboard is the
        // closest persistent per-application slot. Reads may therefore observe
        // search-field text.
        virtual Clipboard* primary() = 0;
        // The regular clipboard.
        virtual Clipboard* secondary() = 0;
        virtual void requestPointerIcon(PointerIcon icon) = 0;
        // Opens uri with the desktop's default handler for its scheme. The
        // launch is fire-and-forget: failures surface only in the desktop
        // environment, never back to the caller.
        virtual void requestOpenUri(stl::StringView uri) = 0;
        // Caret rectangle in surface pixels. Input methods position their
        // candidate window next to it (text-input-v3 cursor rectangle on
        // Wayland, firstRectForCharacterRange on macOS).
        virtual void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) = 0;

        virtual WindowInfo info() const = 0;
        // True between a requestShow()/requestShowAt() and the next
        // requestHide() (or the window never having been shown). The
        // caller that owns show/hide state - toggling the quick-terminal
        // window on a hotkey, for instance - needs this to stay correct
        // even when something other than that caller hides the window,
        // such as hide-on-resign-key.
        virtual bool visible() const = 0;
        // True while the user is interactively resizing the window; a
        // renderer presents transaction-synchronously then and stays
        // asynchronous otherwise.
        virtual bool inLiveResize() const = 0;
        // The escape hatch, for whatever this contract deliberately has
        // no entry for. Add a method here (like requestCornerRadius())
        // when every backend has a meaningful answer, even a no-op one -
        // the backend gets to decide what "meaningful" means for it.
        // Reach through .window instead (bridged to the platform's
        // native handle - NSWindow on Cocoa, wl_surface on Wayland; see
        // ui_csd_tabs.mm and ui_quick_hotkey.mm) when the contract has
        // no reasonable cross-platform shape at all: native
        // -toggleFullScreen: does not do what a quick window needs, and
        // nothing here offers position+size as one atomic operation the
        // way -setFrame: does. Getting this wrong has a real cost, not
        // just a style one: a split move+resize shows a frame in between.
        //
        // Whenever you do reach through: check .backend before the
        // bridge cast, never .window against null. Every backend hands
        // back a non-null .window - the headless one points it at its
        // own render target - so a null check passes on all of them and
        // the cast then yields a pointer that bridges fine and dies on
        // the first message sent to it. Both times this rule was left
        // implicit it cost a SIGSEGV in the same wave (headless
        // regression tests, then a probe test in R2-qa round 2, B5).
        virtual RenderContext renderContext() const = 0;
        // The window's own chrome, when WindowOptions::clientChrome asked
        // for it and the backend could give it; null otherwise.
        virtual WindowChrome* chrome() {
            return nullptr;
        }
    };
}
