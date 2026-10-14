/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/vt_config.h>
#include <lib/vterm/vt_geometry.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/lib/list.h>
#include <std/sys/types.h>
#include <std/mem/obj_pool.h>

namespace stl {
    class ObjPool;
    class Output;
    class SmallObjAllocator;
}

namespace plt {
    struct FiberMutex;
    struct Scheduler;
    struct InputSink;
    struct Platform;
    struct Window;
}

struct Fontpack;
struct Application;
struct Brand;
struct Config;
struct CellExtraStore;
struct Font;
struct FontFace;
struct FontMetrics;
struct FontRenderer;
struct GlyphCache;
struct InputBindings;
struct InputRemap;
struct Options;
struct Renderer;
struct SpanShaper;
struct SessionSet;
struct Pty;
struct LaunchCommand;
struct BookmarkShelf;
struct BookmarkProbe;
struct TerminalUpdate;
struct VtHost;
struct Vterm;
struct VtermTraceFactory;
struct FontRequest;

enum class FontKind : u8;

// A1: the layout-facing counterpart to the user's `border` option. `border`
// keeps meaning "air around the text"; Insets is what layout actually
// consumes - border plus whatever chrome (sidebar, titlebar strip) reserves
// on that side. Composer::contentInsets() is the only supported source of
// layout geometry; borderPixels() stays for reading the option itself.
//
// Units: every field is in backing (physical) pixels, the same unit as
// pixelWidth/pixelHeight and PixelRect below - NOT points, and NOT the
// logical points some Options fields (sidebarWidth, quickCornerRadius) are
// documented in. Any points-denominated option value MUST be multiplied by
// contentScale before it lands in an Insets field, exactly as
// borderPixels() already scales the border option. Skip that conversion and every reserve comes
// out half of what it should be on a 2x (Retina) display, which both
// misplaces the layout and misses the hit-test by the same factor.
struct Insets {
    u16 top = 0;
    u16 right = 0;
    u16 bottom = 0;
    u16 left = 0;
};

// A1/A10: the same four numbers in the core's own spelling. Field for
// field and by name, never positional: the two structs agree on order
// on purpose (T5.1's decision, section 2.1) and a positional copy would
// go on compiling the day one of them stops.
//
// It lives beside Insets rather than in pane_layout.h because it is not
// about layout: the core asks for the window's insets through
// VtHost::contentInsets() and the composer's adapter answers with this,
// which is a translation between two spellings of one number and has no
// pane in it at all.
inline VtInsets vtInsets(const Insets& insets) {
    return VtInsets{.top = insets.top, .right = insets.right, .bottom = insets.bottom, .left = insets.left};
}

// The side of the window one piece of chrome reserves, in Insets field
// order. Window chrome is not one feature but several, owned by
// different modules that never meet: the sidebar tab list takes the
// right edge (T5), the auto-hiding titlebar strip takes the top (T6).
// Naming the side is what lets each of them set its own reserve through
// Composer::setChromeReserve() without reading, merging, or clobbering
// the other's.
enum class ChromeSide : u8 {
    Top,
    Right,
    Bottom,
    Left,
    Count,
};

// A2: one pane's placement on the render target, in surface pixels. Deliberately
// distinct from the cell-grid `Rect` (rect.h) used for text selection - that
// type is Point-pairs in row/column space and means something else entirely.
struct PixelRect {
    u16 x = 0;
    u16 y = 0;
    u16 width = 0;
    u16 height = 0;
};

// A2: one pane's contribution to a single rendered frame. Renderer::update()
// takes a contiguous run of these instead of a beginFrame()/updatePane()/
// endFrame() state machine - one call, one frame, one present, and there is
// no way to leave the sequence unbalanced.
//
// Fixed here as the contract; T8 is the one who actually adds
//   virtual bool update(const PaneUpdate* panes, size_t count) = 0;
// to Renderer (render.h) and implements it in every backend. The plan and
// architecture docs write this as `stl::Span<const PaneUpdate>` - this
// codebase's stl has no Span type, and the established idiom for a
// read-only contiguous view is a pointer plus a count (see
// TomlSink::tomlKey, Darts::create), so the contract uses that instead.
// The existing `update(const TerminalUpdate&)` becomes a thin wrapper over
// a single-element array; nothing that calls it today changes.
struct PaneUpdate {
    PixelRect area;
    const TerminalUpdate& update;
};

// Application wiring. Components retain Composer itself and read dependencies
// here when needed; they do not cache aliases of these canonical fields.
// Event producers commit state here before notifying listeners.
struct Composer {
    explicit Composer(stl::ObjPool* pool);
    Composer(stl::ObjPool* pool, Brand& brand);

    void setContentScale(float scale);
    // Builds the VtHost adapter over the platform window and installs it
    // with the scheduler; call once the window exists.
    void installVtHost();
    // Installs the platform font backends after command-line options have
    // selected any decorators around them.
    void installFontRenderers();
    // Publishes a parsed snapshot: the core's view (the config slot, the
    // precomputed border) follows the swap atomically.
    void setOptions(const Options* options);
    // A1/A10: the window's grid, counted out of contentInsets() - border
    // plus what the chrome reserves on each side - rather than out of a
    // symmetric border alone. That is why it stays here and is not
    // VtGeometry::resize(): the reserves are the embedder's, and a core
    // that counted the grid without them would put the sidebar back on
    // top of the text every time cmd+b widened the terminal. It commits
    // the same fields VtGeometry::resize() would have and walks
    // resizedListeners itself, which is what the host adapter's
    // resized() does upstream.
    void resize(u16 pixelWidth, u16 pixelHeight);
    // The user's `border` option in backing pixels. Upstream precomputed
    // the same number into a scalar VtGeometry::borderPixels; T5.1
    // replaced that field with four-sided VtInsets, and this stays here
    // for the same reason resize() does - A1 makes the points-to-pixels
    // conversion the embedder's, and it is the one border_pixels_guard
    // meters. The core is handed the result, per pane and per side,
    // through paneGeometry() (pane_layout.h); it never learns the option
    // or the scale, so there is no second place to scale them in.
    u16 borderPixels() const;
    // Logical points to backing pixels: the one conversion every
    // points-denominated length owes the layout (see the unit note on
    // Insets above). borderPixels() is its first caller and the chrome
    // reserves are its second, so the option and the reserve round and
    // clamp the same way rather than drifting apart by a pixel at some
    // fractional scale.
    //
    // The ceiling is arithmetic saturation and nothing else, and it is
    // deliberately far out of reach: the options that feed this are
    // validated in *points* (-border 0..3000, -sidebarWidth 1..3000)
    // while the ceiling counts backing pixels, so a ceiling anywhere
    // near the option maximum silently stops the reserve growing at
    // scale 2 while the chrome drawn from the same option keeps going.
    // That is exactly what the old 3000-pixel ceiling did: -sidebarWidth
    // 1500, 1600 and 2800 all left 24 columns on a 3456 px window while
    // the panel grew to 5600 px, putting 1300 pt of text under it
    // (R4-qa, Q1). An Insets field is a border plus one reserve, so half
    // of u16 apiece is what keeps their sum representable.
    u16 scaledPixels(u16 points) const;

    // A10: the two transformations A1 left composed. They look like one
    // as long as the window holds a single pane, and they are two the
    // moment it holds a split - so each of them has a name of its own,
    // and the composition keeps its name too, with the condition it is
    // true under written down.
    //
    // Window rectangle -> content box: what the chrome (the sidebar tab
    // list, the titlebar strip) took off the window and no pane may draw
    // in. The layout layer subtracts these before it cuts the box into
    // panes; it is the only step that knows about chrome at all.
    Insets chromeInsets() const;
    // Pane rectangle -> the pane's grid origin: the user's `border`, the
    // air around the text, on all four sides. A backend adds these to
    // the rectangle it was handed. It never adds a chrome reserve,
    // because the rectangle it was handed is already inside the content
    // box - charging the reserve again would push the right pane of a
    // vertical split in by the width of the sidebar a second time.
    Insets paneInsets() const;
    // A1: border (symmetric) plus chrome reserves (per side), in backing
    // pixels (see the unit note on Insets above) - which is to say the
    // composition of the two above, field for field.
    //
    // Correct exactly while the pane *is* the window: resize() counts
    // the window's own grid with it (that grid is what windowPane()
    // names), and so does everyone still measuring a terminal that fills
    // its window. Whoever must stop calling it is whoever still calls it
    // - the same standing invitation windowPane() and surfacePane()
    // carry.
    Insets contentInsets() const;
    // The chrome reserve on one side, in *logical points* - the unit
    // window chrome is naturally described in (a sidebar width option, a
    // titlebar strip height in points) and the one that survives the
    // window moving to a display of a different scale: contentInsets()
    // scales it on the way out, so nobody has to re-apply a reserve when
    // contentScale changes. Setting a side re-counts the grid out of the
    // new content box and publishes the resize, which is what makes
    // cmd+b widen the terminal and hand the shell its new size (A7).
    void setChromeReserve(ChromeSide side, u16 points);
    u16 chromeReserve(ChromeSide side) const;
    // The layered window (ui_sidebar_tabs.mm): the terminal drawn as a
    // panel of its own laid over the window's surface, the tab list left
    // on the surface underneath. Off until a real window turns it on, so
    // nothing headless and nothing off macOS ever sees it.
    //
    // The panel stands `gap` points clear of every window edge that no
    // chrome already holds - on a side with a reserve it starts where the
    // reserve ends, which is how the panel meets the sidebar with no gap
    // between them - and its text sits `pad` points inside it. Both go
    // into chromeInsets(), so the grid, the panes and the pointer all
    // follow without a second place knowing about the panel; setting it
    // re-counts the grid the way setChromeReserve() does.
    //
    // `header` is a band at the top of the panel that holds its own title
    // bar: the grid starts below it, and it stays part of the panel.
    void setPanelLayer(bool on, u16 gapPoints, u16 padPoints, u16 headerPoints);
    bool panelLayered() const;
    // The panel's rectangle on the surface, in backing pixels: the window
    // less max(reserve, gap) on each side. The whole surface while the
    // layer is off, which is the area the clear has always painted.
    PixelRect panelRect() const;
    float boxDrawingStroke() const;
    Font* loadFont(stl::ObjPool& owner, const FontRequest& request, FontMetrics& metrics);
    // Adopts a face fresh from a resolver and rasterizes it with the first
    // renderer in fontRenderers that succeeds; null when none does.
    Font* renderFace(stl::ObjPool& owner, FontFace* face, u16 pixels, FontKind kind, FontMetrics& metrics);

    // The embedding pieces of the VT core, handed to Vterm::create
    // explicitly: the grid geometry, the reloadable config slot, the
    // cell-extra slot, and the fiber machinery every terminal shares.
    //
    // geometry is the *window's*: its surface and the cell size the font
    // gives it, counted by resize() out of contentInsets(). Its insets
    // and its origin stay at zero, because a window is not a pane: a
    // pane's own grid, border and origin are a second VtGeometry, built
    // by paneGeometry() (pane_layout.h) and delivered through
    // Vterm::paneResized() (A8). Same type, different instances, and
    // neither is ever written from the other.
    VtGeometry geometry;
    VtConfigSlot vtConfig;
    VtCellExtras extras;
    stl::SmallObjAllocator* smallObjects = nullptr;
    plt::Scheduler* scheduler = nullptr;
    VtHost* host = nullptr;
    stl::ObjPool* pool = nullptr;
    Brand* brand = nullptr;
    // Owns the renderer and its listeners; dropped and rebuilt wholesale
    // when the renderer loses its surface.
    stl::ObjPool::Ref rendererPool = stl::ObjPool::fromMemory();
    Application* application = nullptr;
    Fontpack* fonts = nullptr;
    // The rasterized-glyph memo shared by every font backend; fonts key
    // it with private namespaces, so it never needs resetting on a font
    // change - stale strikes age out through the byte budget.
    GlyphCache* glyphs = nullptr;
    InputBindings* inputBindings = nullptr;
    // Chord rewriting; created after the options are parsed, so it stays
    // null for early events.
    InputRemap* inputRemap = nullptr;
    // Immutable runtime configuration. The constructor installs zeroed
    // defaults for headless adapters; Config publishes parsed snapshots.
    const Options* opts = nullptr;
    Config* config = nullptr;
    plt::InputSink* input = nullptr;
    Renderer* renderer = nullptr;
    // The render side of the cell grid: shapes rows into strips through
    // fonts. The model never touches it; created with the fontpack
    // machinery and null in purely headless embeddings.
    SpanShaper* shaper = nullptr;
    // Process-lifetime PTY factory and the immutable command each new
    // session launches. Individual handles never leave SessionSet.
    Pty* pty = nullptr;
    const LaunchCommand* launch = nullptr;
    // The user's shell alone, resolved at startup like launch: what a
    // bookmark's tab runs its command with. launch itself when that is the
    // shell; its own command when the window was started with -e.
    const LaunchCommand* shellLaunch = nullptr;
    VtermTraceFactory* vtermTraceFactory = nullptr;
    SessionSet* sessions = nullptr;
    // The bookmarks the sidebar lists (bookmarks.h); null in headless
    // embeddings that load none.
    BookmarkShelf* bookmarks = nullptr;
    // Whether their ssh hosts answer (bookmark_probe.h); null with them.
    BookmarkProbe* bookmarkProbe = nullptr;
    plt::Platform* platform = nullptr;
    plt::Window* window = nullptr;

    u16 fontSize = 0;
    // Per-side chrome reserve in logical points, indexed by ChromeSide;
    // read through chromeReserve() and written through
    // setChromeReserve(), which is what keeps the grid in step with it.
    u16 chromeReserves[(unsigned)(ChromeSide::Count)]{};
    // Every side's chrome edge in backing pixels: the reserve, or `gap`
    // where the reserve is narrower, plus `pad`, plus `header` on top. The
    // one place the reserves are scaled, for chromeInsets() and
    // panelRect() alike.
    Insets chromeEdges(u16 gap, u16 pad, u16 header) const;
    // setPanelLayer()'s three, in logical points like the reserves.
    bool panelLayer = false;
    u16 panelGap = 0;
    u16 panelPad = 0;
    u16 panelHeader = 0;
    // A1: the points-to-pixels factor every reserve and the border option
    // owe the layout. It lived on VtState until M6c dissolved it; the
    // core has no use for a scale it never converts anything with.
    float contentScale = 1.0f;
    // The -debug trace file, or -1; debug_trace.cpp writes through it.
    int debugFd = -1;
    // Whether launchd started this process (a Finder, Dock or open(1)
    // launch on macOS). Captured once at startup: launchd adopts any
    // process whose parent exits, so asking again at a config reload
    // would re-classify a shell launch as a desktop one.
    bool desktopLaunch = false;

    // resize commits the core geometry before the host adapter walks
    // this list.
    stl::IntrusiveList resizedListeners;
    stl::IntrusiveList contentScaleChangedListeners;
    stl::IntrusiveList fontChangedListeners;
    // Vterms publish their own undecorated title through the host
    // adapter into this list. The session owner decides whether the
    // source is visible and how the window presents it.
    stl::IntrusiveList titleChangedListeners;
    stl::IntrusiveList configChangedListeners;
    stl::IntrusiveList fontIncListeners;
    stl::IntrusiveList fontDecListeners;
    stl::IntrusiveList fontResetListeners;
    // SessionSet commits its tab model - count, order, active index,
    // labels - and then walks this list; the window chrome projects the
    // model from here.
    stl::IntrusiveList sessionsChangedListeners;
    // Font resolvers are tried in registration order.
    stl::IntrusiveList fontResolvers;
    // Font renderers are tried in registration order; any renderer accepts
    // any resolver's face.
    stl::IntrusiveList fontRenderers;
    // Input producers call input; the router walks this list in registration
    // order and stops at the first handler which accepts the event.
    stl::IntrusiveList inputHandlers;
    // Terminal actions are claimed once for the window. Every terminal
    // pushes its own node here and unlinks it when it stops being the one
    // the window shows, so the action follows the active terminal without
    // the binding table ever being re-registered.
    stl::IntrusiveList copyListeners;
    stl::IntrusiveList pasteListeners;
    stl::IntrusiveList pastePrimaryListeners;
    stl::IntrusiveList pageUpListeners;
    stl::IntrusiveList pageDownListeners;
    stl::IntrusiveList newTabListeners;
    stl::IntrusiveList closeTabListeners;
    // A4: the two split chords. Claimed for the window like the tab
    // actions, and for the same reason - a split outlives the pane that
    // asked for it, and the pane that answers the next one is a
    // different terminal.
    stl::IntrusiveList splitVerticalListeners;
    stl::IntrusiveList splitHorizontalListeners;
    // The pane focus chords, one list per direction.
    stl::IntrusiveList focusPaneLeftListeners;
    stl::IntrusiveList focusPaneDownListeners;
    stl::IntrusiveList focusPaneUpListeners;
    stl::IntrusiveList focusPaneRightListeners;
    stl::IntrusiveList prevTabListeners;
    stl::IntrusiveList nextTabListeners;
    // cmd+b. Claimed for the window like the tab actions above, so the
    // sidebar module can be a fire-and-forget listener that outlives no
    // particular terminal - and so the chord is registered even where no
    // module answers it, which is every platform but macOS today.
    stl::IntrusiveList toggleSidebarListeners;
    stl::IntrusiveList commandPaletteListeners;
    // One list per direct-selection chord; index N serves SelectTab1+N.
    stl::IntrusiveList selectTabListeners[9];
    stl::IntrusiveList clearListeners;
    stl::IntrusiveList selectCommandLineListeners;
    stl::IntrusiveList undoCommandLineListeners;
    stl::IntrusiveList redoCommandLineListeners;
    stl::IntrusiveList wordLeftListeners;
    stl::IntrusiveList wordRightListeners;
    stl::IntrusiveList lineStartListeners;
    stl::IntrusiveList lineEndListeners;
    stl::IntrusiveList killLineListeners;
    stl::IntrusiveList eraseWordListeners;
};
