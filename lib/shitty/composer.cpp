/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "composer.h"

#include "brand.h"
#include "options.h"
#include "session.h"
#include "font_face.h"
#include "font_pack.h"
#include "font_path.h"
#include "font_optical.h"
#include "glyph_cache.h"
#include "input_router.h"
#include "font_coretext.h"
#include "font_embedded.h"
#include "font_freetype.h"
#include "font_renderer.h"
#include "font_resolver.h"
#include "grid_geometry.h"
#include "input_bindings.h"
#include "font_fontconfig.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/vt_host.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/dbg/assert.h>
#include <std/mem/small_obj_allocator.h>

#include <stdio.h>
#include <stdint.h>
#include <plt/window.h>
#include <plt/platform.h>

using namespace stl;

Composer::Composer(ObjPool* pool_)
    : Composer(pool_, *Brand::generic())
{
}

Composer::Composer(ObjPool* pool_, Brand& brand_)
    : pool(pool_)
    , brand(&brand_)
{
    Options* const defaults = pool->make<Options>();
    defaults->vt.brandName = brand->displayName();
    setOptions(defaults);
    extras.store = CellExtraStore::create(extras, *pool, 0);
    smallObjects = SmallObjAllocator::create(pool);
    glyphs = createGlyphCache(*pool);
    input = createInputRouter(*this);
    inputBindings = InputBindings::create(*this);
    inputHandlers.pushBack(inputBindings);
    // The terminal actions belong to the window, not to a terminal: they
    // are claimed once here so any number of terminals can contribute a
    // listener to them, and so the lists outlive every terminal that does.
    inputBindings->add(InputActions::Copy, &copyListeners);
    inputBindings->add(InputActions::Paste, &pasteListeners);
    inputBindings->add(InputActions::PastePrimary, &pastePrimaryListeners);
    inputBindings->add(InputActions::PageUp, &pageUpListeners);
    inputBindings->add(InputActions::PageDown, &pageDownListeners);
    inputBindings->add(InputActions::NewTab, &newTabListeners);
    inputBindings->add(InputActions::CloseTab, &closeTabListeners);
    // The splits go in on 'd', which no other row claims on either
    // modifier set, so their position here settles nothing - but the
    // position is still theirs on purpose: find() answers with the first
    // match, and a later row for cmd+d would be unreachable rather than
    // ambiguous.
    inputBindings->add(InputActions::SplitVertical, &splitVerticalListeners);
    inputBindings->add(InputActions::SplitHorizontal, &splitHorizontalListeners);
    inputBindings->add(InputActions::FocusPaneLeft, &focusPaneLeftListeners);
    inputBindings->add(InputActions::FocusPaneDown, &focusPaneDownListeners);
    inputBindings->add(InputActions::FocusPaneUp, &focusPaneUpListeners);
    inputBindings->add(InputActions::FocusPaneRight, &focusPaneRightListeners);
    // Before PrevTab/NextTab: chords match in registration order, so the
    // -naturalEditing rows for cmd+arrows win over the tab walk exactly
    // while the preset holds.
    inputBindings->add(InputActions::LineStart, &lineStartListeners);
    inputBindings->add(InputActions::LineEnd, &lineEndListeners);
    inputBindings->add(InputActions::PrevTab, &prevTabListeners);
    inputBindings->add(InputActions::NextTab, &nextTabListeners);
    for (unsigned at = 0; at < 9; ++at) {
        inputBindings->add((InputActions)((unsigned)(InputActions::SelectTab1) + at), &selectTabListeners[at]);
    }
    inputBindings->add(InputActions::Clear, &clearListeners);
    inputBindings->add(InputActions::SelectCommandLine, &selectCommandLineListeners);
    inputBindings->add(InputActions::UndoCommandLine, &undoCommandLineListeners);
    inputBindings->add(InputActions::RedoCommandLine, &redoCommandLineListeners);
    inputBindings->add(InputActions::ToggleSidebar, &toggleSidebarListeners);
    inputBindings->add(InputActions::CommandPalette, &commandPaletteListeners);
    inputBindings->add(InputActions::WordLeft, &wordLeftListeners);
    inputBindings->add(InputActions::WordRight, &wordRightListeners);
    inputBindings->add(InputActions::KillLine, &killLineListeners);
    inputBindings->add(InputActions::EraseWord, &eraseWordListeners);
    if (FontResolver* const resolver = createCoreTextFontResolver(*this)) {
        fontResolvers.pushBack(resolver);
    }
    if (FontResolver* const resolver = createFontconfigResolver(*this)) {
        fontResolvers.pushBack(resolver);
    }
    if (FontResolver* const resolver = createPathFontResolver(*this)) {
        fontResolvers.pushBack(resolver);
    }
    if (FontResolver* const resolver = createEmbeddedFontResolver(*this)) {
        fontResolvers.pushBack(resolver);
    }
    installFontRenderers();
}

namespace {
    // The GUI's side of the core protocol: window requests forward to
    // the platform window, events fan into the composer's listener
    // lists - the session set and the application subscribe there.
    struct ComposerVtHost final: public VtHost {
        explicit ComposerVtHost(Composer& composer);

        plt::Clipboard* primary() override;
        plt::Clipboard* secondary() override;
        plt::WindowInfo info() override;
        void requestFrame() override;
        void requestResize(u32 width, u32 height) override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestIconify() override;
        void requestRestore() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestAttention() override;
        void requestPointerIcon(plt::PointerIcon icon) override;
        void requestOpenUri(stl::StringView uri) override;
        bool uriSchemeAllowed(stl::StringView scheme) override;
        void titleChanged(const VtermTitleChanged& event) override;
        void resized() override;
        VtInsets contentInsets() override;
        void surfaceResized(u32 width, u32 height) override;
        size_t cellCapacityExcept(const Vterm* except) override;

        Composer& composer;
    };

    static void walk(IntrusiveList& listeners, void* argument = nullptr) {
        for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen(argument);
        }
    }
}

ComposerVtHost::ComposerVtHost(Composer& composer_)
    : composer(composer_)
{
}

plt::Clipboard* ComposerVtHost::primary() {
    return composer.window->primary();
}

plt::Clipboard* ComposerVtHost::secondary() {
    return composer.window->secondary();
}

plt::WindowInfo ComposerVtHost::info() {
    return composer.window->info();
}

void ComposerVtHost::requestFrame() {
    composer.window->requestFrame();
}

void ComposerVtHost::requestResize(u32 width, u32 height) {
    composer.window->requestResize(width, height);
}

void ComposerVtHost::requestMaximized(bool maximized) {
    composer.window->requestMaximized(maximized);
}

void ComposerVtHost::requestFullscreen(bool fullscreen) {
    composer.window->requestFullscreen(fullscreen);
}

void ComposerVtHost::requestIconify() {
    composer.window->requestIconify();
}

void ComposerVtHost::requestRestore() {
    composer.window->requestRestore();
}

void ComposerVtHost::requestMove(i32 x, i32 y) {
    composer.window->requestMove(x, y);
}

void ComposerVtHost::requestFocus() {
    composer.window->requestFocus();
}

void ComposerVtHost::requestAttention() {
    composer.window->requestAttention();
}

void ComposerVtHost::requestPointerIcon(plt::PointerIcon icon) {
    composer.window->requestPointerIcon(icon);
}

void ComposerVtHost::requestOpenUri(StringView uri) {
    composer.window->requestOpenUri(uri);
}

bool ComposerVtHost::uriSchemeAllowed(StringView scheme) {
    return composer.opts->uriSchemeAllowed(scheme);
}

void ComposerVtHost::titleChanged(const VtermTitleChanged& event) {
    walk(composer.titleChangedListeners, (void*)(&event));
}

void ComposerVtHost::resized() {
    walk(composer.resizedListeners);
}

VtInsets ComposerVtHost::contentInsets() {
    // A1/A10: the composition, not its two parts. The core is handed
    // the sum and cannot tell the border from the chrome reserve, which
    // is what stops it having an opinion about either.
    return vtInsets(composer.contentInsets());
}

void ComposerVtHost::surfaceResized(u32 width, u32 height) {
    // The clamp is here and not in the core: u16 is the width of
    // Composer::geometry's fields, i.e. this embedder's own form of
    // storage, and VtHost speaks in u32 like requestResize() beside it.
    const u32 limit = UINT16_MAX;
    composer.resize((u16)(min(width, limit)), (u16)(min(height, limit)));
}

size_t ComposerVtHost::cellCapacityExcept(const Vterm* except) {
    // A11: the null check belongs where SessionSet is a concept at all.
    // A Composer without one is a headless adapter, and there the
    // asking terminal is the only pane there is - so the sum over
    // everybody else is zero rather than absent.
    if (composer.sessions == nullptr) {
        return 0;
    }
    return composer.sessions->cellCapacityExcept(except);
}

void Composer::installVtHost() {
    host = pool->make<ComposerVtHost>(*this);
    // Unit fixtures install the adapter without a platform; anything
    // that spawns pty fibers brings one.
    scheduler = platform != nullptr ? platform->scheduler() : nullptr;
}

void Composer::installFontRenderers() {
    while (!fontRenderers.empty()) {
        fontRenderers.popFront();
    }
    if (FontRenderer* renderer = createCoreTextFontRenderer(*this)) {
        if (opts->optical) {
            renderer = createOpticalFontRenderer(*this, renderer);
        }
        fontRenderers.pushBack(renderer);
    }
    if (FontRenderer* renderer = createFreeTypeFontRenderer(*this)) {
        if (opts->optical) {
            renderer = createOpticalFontRenderer(*this, renderer);
        }
        fontRenderers.pushBack(renderer);
    }
}

void Composer::setContentScale(float scale) {
    STD_ASSERT(scale > 0.0f);
    if (contentScale == scale) {
        return;
    }
    contentScale = scale;
    for (IntrusiveNode* node = contentScaleChangedListeners.mutFront(); node != contentScaleChangedListeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

void Composer::setOptions(const Options* options) {
    opts = options;
    vtConfig.config = &options->vt;
}

float Composer::boxDrawingStroke() const {
    if (fonts != nullptr) {
        const float measured = fonts->boxDrawingStroke();
        if (measured > 0.0f) {
            return measured;
        }
    }
    const u16 shortSide = geometry.cellPixelWidth < geometry.cellPixelHeight ? geometry.cellPixelWidth : geometry.cellPixelHeight;
    const float fallback = (float)(shortSide) / 12.0f;
    return fallback > 1.0f ? fallback : 1.0f;
}

u16 Composer::scaledPixels(u16 points) const {
    const float scaled = points * contentScale;
    if (!(scaled > 0)) {
        return 0;
    }
    // Saturation only, and out of reach of every legal option value at
    // every scale a display has: both options that reach here are
    // validated in points and capped at 3000 of them, so this bites at
    // scale 10 and never before. A ceiling of 3000 *pixels* used to bite
    // at scale 2, where it froze the reserve while the chrome drawn from
    // the same option kept growing (R4-qa, Q1). Half of u16 is what
    // leaves room for the border this is summed with in contentInsets().
    if (scaled >= 30000) {
        return 30000;
    }
    return (u16)(scaled + 0.5f);
}

u16 Composer::borderPixels() const {
    // opts->border, and nothing beside it. VtState::baseBorder used to
    // hold a copy; M6c dissolved VtState and the copy went with it, so
    // the published snapshot is now read where it is published.
    // Upstream precomputed the same number into a scalar
    // VtGeometry::borderPixels; T5.1 replaced that field with four-sided
    // VtInsets and left the conversion here, where A1 puts it. The core
    // is handed the four pixel counts and never the option or the
    // scale, so there is no second place either could be scaled in.
    return scaledPixels(opts->border);
}

namespace {
    // Where the panel's edge stands on one side, in points: past the
    // chrome when there is some, `gap` clear of the window edge when
    // there is none. Summed in u32 and clamped for the reason
    // scaledPixels() saturates: a reserve is validated in points up to
    // 3000, and the sum has to stay a u16 for the scale to apply to.
    u16 panelEdge(u16 reserve, u16 gap, u16 pad) {
        const u32 edge = (u32)(max<u16>(reserve, gap)) + pad;
        return (u16)(min<u32>(edge, 30000));
    }
}

Insets Composer::chromeEdges(u16 gap, u16 pad, u16 header) const {
    return Insets{
        scaledPixels(panelEdge(chromeReserves[(unsigned)(ChromeSide::Top)], gap, (u16)(min<u32>((u32)(pad) + header, 30000)))),
        scaledPixels(panelEdge(chromeReserves[(unsigned)(ChromeSide::Right)], gap, pad)),
        scaledPixels(panelEdge(chromeReserves[(unsigned)(ChromeSide::Bottom)], gap, pad)),
        scaledPixels(panelEdge(chromeReserves[(unsigned)(ChromeSide::Left)], gap, pad)),
    };
}

Insets Composer::chromeInsets() const {
    // The layered window moves every side by the same rule, so the panes,
    // the pointer and the grid count follow it without knowing it exists;
    // off, the pad and the gap are zero and each side is its reserve.
    return panelLayer ? chromeEdges(panelGap, panelPad, panelHeader) : chromeEdges(0, 0, 0);
}

bool Composer::panelLayered() const {
    return panelLayer;
}

PixelRect Composer::panelRect() const {
    const u16 width = geometry.pixelWidth;
    const u16 height = geometry.pixelHeight;
    if (!panelLayer) {
        return PixelRect{0, 0, width, height};
    }
    // The chrome edge without the pad: the pad is air inside the panel,
    // and the panel is what this names.
    const Insets edges = chromeEdges(panelGap, 0, 0);
    const u16 top = edges.top;
    const u16 right = edges.right;
    const u16 bottom = edges.bottom;
    const u16 left = edges.left;
    // A window narrower than its own chrome has no panel rather than one
    // of negative size.
    if ((u32)(left) + right >= width || (u32)(top) + bottom >= height) {
        return PixelRect{left, top, 0, 0};
    }
    return PixelRect{left, top, (u16)(width - left - right), (u16)(height - top - bottom)};
}

void Composer::setPanelLayer(bool on, u16 gapPoints, u16 padPoints, u16 headerPoints) {
    if (panelLayer == on && panelGap == gapPoints && panelPad == padPoints && panelHeader == headerPoints) {
        return;
    }
    panelLayer = on;
    panelGap = gapPoints;
    panelPad = padPoints;
    panelHeader = headerPoints;
    // Re-counted exactly as a changed reserve is, and for the same
    // reason: the content box moved under a surface that did not.
    if (geometry.pixelWidth != 0 && geometry.pixelHeight != 0 && geometry.cellPixelWidth != 0 && geometry.cellPixelHeight != 0) {
        resize(geometry.pixelWidth, geometry.pixelHeight);
    }
}

Insets Composer::paneInsets() const {
    // One call, four sides: the border is symmetric by definition, and
    // reading it once is also what keeps the four sides consistent if a
    // reload changes the option between two of them.
    const u16 border = borderPixels();
    return Insets{border, border, border, border};
}

Insets Composer::contentInsets() const {
    // A10: the sum, and nothing else - no third reading of either
    // option, so the composition cannot drift from the two parts it is
    // made of.
    const Insets chrome = chromeInsets();
    const Insets pane = paneInsets();
    return Insets{
        (u16)(pane.top + chrome.top),
        (u16)(pane.right + chrome.right),
        (u16)(pane.bottom + chrome.bottom),
        (u16)(pane.left + chrome.left),
    };
}

u16 Composer::chromeReserve(ChromeSide side) const {
    STD_ASSERT(side != ChromeSide::Count);
    return chromeReserves[(unsigned)(side)];
}

void Composer::setChromeReserve(ChromeSide side, u16 points) {
    STD_ASSERT(side != ChromeSide::Count);
    u16& stored = chromeReserves[(unsigned)(side)];
    if (stored == points) {
        return;
    }
    stored = points;
    // The content box just changed under a surface that did not, so the
    // grid has to be counted out of it again - this is the whole of what
    // makes cmd+b widen the terminal and send the shell its new size
    // (A7). resize() itself decides whether anything actually moved and
    // stays silent when it did not. Before the first font and the first
    // surface there is no grid to count: showWindow()'s own resize()
    // picks the reserve up when it runs.
    if (geometry.pixelWidth != 0 && geometry.pixelHeight != 0 && geometry.cellPixelWidth != 0 && geometry.cellPixelHeight != 0) {
        resize(geometry.pixelWidth, geometry.pixelHeight);
    }
}

void Composer::resize(u16 pixelWidth_, u16 pixelHeight_) {
    STD_ASSERT(geometry.cellPixelWidth != 0);
    STD_ASSERT(geometry.cellPixelHeight != 0);

    const Insets insets = contentInsets();
    const u16 columns_ = (u16)(gridColumns(pixelWidth_, insets, geometry.cellPixelWidth));
    const u16 rows_ = (u16)(gridRows(pixelHeight_, insets, geometry.cellPixelHeight));

    if (geometry.columns == columns_ && geometry.rows == rows_ && geometry.pixelWidth == pixelWidth_ && geometry.pixelHeight == pixelHeight_) {
        return;
    }

    // F4, Q2. Every re-count of the grid passes through here, which is
    // why the trace does too. It used to live in
    // ApplicationImpl::updateWindowInfo(), i.e. on the platform's own
    // callback, so a grid changed by setChromeReserve() - cmd+b, a
    // reload, the title bar strip appearing - printed nothing at all:
    // R4-qa moved the grid three times with cmd+b and read zero
    // `window:` lines, which made T6's "no resize events on hover"
    // criterion unable to fail. The full-screen transition bugs the
    // trace was written for are still visible in it: what a platform
    // delivers reaches this function unchanged.
    if (vtConfig.config->verbose && (geometry.columns != columns_ || geometry.rows != rows_)) {
        fprintf(stderr, "%s: window: %ux%u px, grid %ux%u -> %ux%u, scale %.2f\n", brand->identifierCString(), (unsigned)(pixelWidth_), (unsigned)(pixelHeight_), (unsigned)(geometry.columns), (unsigned)(geometry.rows), (unsigned)(columns_), (unsigned)(rows_), (double)(contentScale));
    }

    geometry.columns = columns_;
    geometry.rows = rows_;
    geometry.pixelWidth = pixelWidth_;
    geometry.pixelHeight = pixelHeight_;

    for (IntrusiveNode* node = resizedListeners.mutFront(); node != resizedListeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

Font* Composer::loadFont(ObjPool& owner, const FontRequest& request, FontMetrics& metrics) {
    for (IntrusiveNode* node = fontResolvers.mutFront(); node != fontResolvers.mutEnd();) {
        FontResolver* const resolver = static_cast<FontResolver*>(node);
        node = node->next;
        FontFace* const resolved = resolver->resolve(request);
        if (resolved != nullptr) {
            return renderFace(owner, resolved, request.pixels, request.kind, metrics);
        }
    }
    return nullptr;
}

Font* Composer::renderFace(ObjPool& owner, FontFace* face, u16 pixels, FontKind kind, FontMetrics& metrics) {
    const IntrusivePtr<FontFace> adopted(face);
    for (IntrusiveNode* node = fontRenderers.mutFront(); node != fontRenderers.mutEnd();) {
        FontRenderer* const renderer = static_cast<FontRenderer*>(node);
        node = node->next;
        try {
            Font* const font = renderer->render(owner, adopted, pixels, kind, metrics);
            if (font != nullptr) {
                return font;
            }
        } catch (Exception&) {
            // A renderer that cannot open or fit the face passes it on.
        }
    }
    return nullptr;
}
