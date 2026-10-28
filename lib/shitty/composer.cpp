/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "composer.h"

#include "brand.h"
#include "options.h"
#include "font_face.h"
#include "font_pack.h"
#include "font_path.h"
#include "glyph_cache.h"
#include "font_optical.h"
#include "input_router.h"
#include "font_coretext.h"
#include "font_embedded.h"
#include "font_freetype.h"
#include "font_renderer.h"
#include "font_resolver.h"
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

#include <plt/window.h>
#include <plt/platform.h>

using namespace stl;

namespace {
    static u16 scaledBorder(u16 base, float scale) {
        const float scaled = base * scale;
        if (!(scaled > 0)) {
            return 0;
        }
        if (scaled >= 3000) {
            return 3000;
        }
        return (u16)(scaled + 0.5f);
    }
}

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
    extras.store = CellExtraStore::create(extras, *pool);
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
        void requestResizeCells(u32 columns, u32 rows) override;
        VtGridSize gridSize(u32 pixelWidth, u32 pixelHeight) override;
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
    composer.resizeWindow(width, height);
}

void ComposerVtHost::requestResizeCells(u32 columns, u32 rows) {
    requestResize(2u * composer.layout.borderPixels + columns * composer.geometry.cellPixelWidth, 2u * composer.layout.borderPixels + rows * composer.geometry.cellPixelHeight);
}

VtGridSize ComposerVtHost::gridSize(u32 width, u32 height) {
    return composer.layout.gridSize(width, height, composer.geometry.cellPixelWidth, composer.geometry.cellPixelHeight);
}

void Composer::resizeWindow(u32 width, u32 height) {
    width = min(width, (u32)(UINT16_MAX));
    height = min(height, (u32)(UINT16_MAX));
    const VtGridSize grid = layout.gridSize(width, height, geometry.cellPixelWidth, geometry.cellPixelHeight);
    const bool moved = layout.resize(width, height, grid.columns * geometry.cellPixelWidth, grid.rows * geometry.cellPixelHeight);
    const bool resized = geometry.resizeCells(grid.columns, grid.rows, host);
    if (moved || resized) {
        walk(layoutChangedListeners);
    }
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
    layout.borderPixels = scaledBorder(opts->border, contentScale);
    for (IntrusiveNode* node = contentScaleChangedListeners.mutFront(); node != contentScaleChangedListeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

void Composer::setOptions(const Options* options) {
    opts = options;
    vtConfig.config = &options->vt;
    layout.borderPixels = scaledBorder(options->border, contentScale);
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
