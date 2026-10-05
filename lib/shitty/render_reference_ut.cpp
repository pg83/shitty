/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render.h"
#include "options.h"
#include "composer.h"
#include "font_pack.h"
#include "span_shaper.h"
#include "render_blend.h"
#include "font_embedded.h"
#include "font_resolver.h"
#include "grid_geometry.h"
#include "render_reference.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/screen.h>
#include <lib/vterm/cell_extra_store.h>

#if defined(HAVE_METAL_RENDERER)
    #include "render_metal.h"
#endif

#include <plt/platform_headless.h>

#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/tst/ut.h>

using namespace stl;

namespace {
    struct FakeFontpack final: public Fontpack {
        u16 getPx() const override;
        u16 getPy() const override;
        float boxDrawingStroke() const override;
        bool hasBold() const override;
        bool hasItalic() const override;
        bool hasBoldItalic() const override;
        Font* resolveFace(const u32* codepoints, size_t count) override;
        void adoptFaceFor(const FontFaceMiss& miss) override;
        Font* styledFace(Font* face, FontStyle style) const override;
    };

    static void configure(Composer& composer, FakeFontpack& fonts, u16 columns, u16 rows, u16 glyphWidth, u16 glyphHeight) {
        composer.fonts = &fonts;
        composer.geometry.setCellPixelSize(glyphWidth, glyphHeight);
        composer.extras.replace(CellExtraStore::create(composer.extras, *composer.pool, (size_t)(columns)*rows));
        const Insets insets = composer.contentInsets();
        composer.resize((u16)(gridPixelWidth(columns, insets, glyphWidth)), (u16)(gridPixelHeight(rows, insets, glyphHeight)));
        composer.shaper = SpanShaper::create(composer, *composer.pool);
    }

    static Color pixel(const ReferenceImage& image, u16 x, u16 y) {
        const size_t index = 3 * ((size_t)(y)*image.width + x);
        return {
            image.pixels[index],
            image.pixels[index + 1],
            image.pixels[index + 2],
        };
    }

    static Color cellPixel(const ReferenceImage& image, u16 x, u16 y) {
        return pixel(image, x, y);
    }

    // The *set* is the assertion, and that is the whole trick: a cell
    // holding a space has no glyph ink, so every decoration in it is
    // drawn flat, with no antialiasing to smear an intermediate value.
    // The set of colours a cell box holds is therefore exactly the
    // background plus the inks used, and any mark that started following
    // -backgroundOpacity introduces a fourth colour - without this test
    // having to know where a single pixel of it lives. Three mutations
    // survived the first round because the tests knew pixel coordinates
    // and only for the three marks they had coordinates for.
    static void distinctColors(const ReferenceImage& image, u16 x0, u16 y0, u16 width, u16 height, Vector<Color>& out) {
        out.clear();
        for (u16 y = y0; y < y0 + height; ++y) {
            for (u16 x = x0; x < x0 + width; ++x) {
                const Color here = cellPixel(image, x, y);
                bool seen = false;
                for (const Color& already : out) {
                    seen = seen || already == here;
                }
                if (!seen) {
                    out.pushBack(here);
                }
            }
        }
    }

    static bool holds(const Vector<Color>& colors, Color wanted) {
        for (const Color& color : colors) {
            if (color == wanted) {
                return true;
            }
        }
        return false;
    }
    static TerminalCell coloredCell(Color foreground, Color background) {
        TerminalCell cell{};
        cell.setForeground(CellColor::direct(foreground));
        cell.setBackground(CellColor::direct(background));
        return cell;
    }

    // T10/R10-test. Every pixel of one cell that the opaque render put
    // something other than the plain background into is a *mark* - glyph
    // ink, an underline, a cursor, a seam. "Only the background goes
    // translucent" is precisely the claim that those pixels do not move
    // when the option moves, so this walks the cell across two renders
    // of one frame and reports both halves of the answer.
    //
    // `marks` is the half that matters as much as the disagreements: a
    // cell where nothing was drawn has nothing to disagree about and
    // would pass an equality check by being empty. Three of the
    // mutations that reached this file survived a suite that never
    // counted what it was comparing.
    struct MarkAgreement {
        u32 marks = 0;
        u32 disagreements = 0;
    };

    template <typename Opaque, typename Translucent>
    static MarkAgreement compareMarks(const Opaque& opaque, const Translucent& translucent, u16 originX, u16 originY, u16 width, u16 height, Color plain) {
        MarkAgreement agreement;
        for (u16 y = 0; y < height; ++y) {
            for (u16 x = 0; x < width; ++x) {
                const u16 sampleX = (u16)(originX + x);
                const u16 sampleY = (u16)(originY + y);
                const Color solid = opaque(sampleX, sampleY);
                if (solid == plain) {
                    continue;
                }
                ++agreement.marks;
                if (!(translucent(sampleX, sampleY) == solid)) {
                    ++agreement.disagreements;
                }
            }
        }
        return agreement;
    }

    struct ReferenceFixture {
        explicit ReferenceFixture(Composer& composer) {
            const size_t bytes = (size_t)(composer.geometry.pixelWidth) * composer.geometry.pixelHeight * 3;
            while (pixels.length() < bytes) {
                pixels.pushBack(0);
            }
            target.pixels = pixels.mutData();
            target.length = pixels.length();
            target.width = composer.geometry.pixelWidth;
            target.height = composer.geometry.pixelHeight;
            target.stride = composer.geometry.pixelWidth * 3;
            renderer = ReferenceRenderer::create(
                composer,
                *rendererPool,
                {
                    .backend = plt::RenderBackend::Headless,
                    .connection = nullptr,
                    .window = &target,
                }
            );
        }

        ReferenceFixture* operator->() {
            return this;
        }

        ReferenceImage render(const TerminalUpdate& update) {
            return renderer->update(update) ? renderer->image() : ReferenceImage{};
        }

        Vector<u8> pixels;
        plt::HeadlessRenderTarget target;
        stl::ObjPool::Ref rendererPool = stl::ObjPool::fromMemory();
        ReferenceRenderer* renderer;
    };

    // A2: what a split hands the renderer - a second terminal on the
    // same composer, with its own screen, its own colours and its own
    // strip arena. ScreenFixture below is one of these plus the composer
    // they share.
    static void writeTextTo(Screen& screen, u16 row, u16 column, const char* text, const TerminalCell& attrs) {
        for (size_t index = 0; text[index] != 0; ++index) {
            const u32 codepoint = (u32)(u8)(text[index]);
            screen.writeGrapheme(row, (u16)(column + index), &codepoint, 1, false, attrs, 0, 0, attrs);
        }
    }

    // A6-3: the frame a pane really sends - the rows that changed and
    // no others. captureFrom() below exposes the screen first, which
    // makes every frame a full one; a renderer that begins a pane from
    // its neighbour's retained cells cannot be caught by a full frame,
    // because every cell of it is overwritten anyway.
    //
    // A9: the grid comes off the screen that produced the cells, not off
    // the composer - a pane is no longer the window.
    static TerminalUpdate captureDamagedFrom(Screen& screen, const TerminalColors& colors, Vector<TerminalRow>& rows) {
        const ScreenInfo info = screen.info();
        rows.grow((size_t)(info.rows));
        const ScreenFrame frame = screen.captureFrame(rows.mutData());
        TerminalUpdate update;
        update.rows = rows.data();
        update.rowCount = frame.damagedRows;
        update.gridColumns = info.columns;
        update.gridRows = info.rows;
        update.colors = &colors;
        update.shapes = &screen;
        return update;
    }

    static TerminalUpdate captureFrom(Composer&, Screen& screen, const TerminalColors& colors, Vector<TerminalRow>& rows) {
        screen.expose();
        return captureDamagedFrom(screen, colors, rows);
    }

    // A9: the grid of a hand-built update, which every test below that
    // does not go through a Screen has to state for itself - a renderer
    // refuses a frame that leaves it at zero.
    static void gridOf(TerminalUpdate& update, u16 columns, u16 rows) {
        update.gridColumns = columns;
        update.gridRows = rows;
    }

    struct ScreenFixture {
        // A1: `border` is the user option, which contentInsets() turns
        // into the reserve on all four sides; zero keeps the grid in the
        // surface corner, as every other test here expects.
        // `topReserve` is a chrome reserve in logical points on the top
        // edge alone - what T6's auto-hiding title bar strip claims -
        // and the only way to build a Composer here whose insets are not
        // the same number on all four sides.
        explicit ScreenFixture(u16 columns, u16 rows, u16 border = 0, u16 topReserve = 0);

        void writeText(u16 row, u16 column, const char* text, const TerminalCell& attrs);
        TerminalUpdate capture();

        // Declared before `pool` on purpose: members die in reverse
        // declaration order, and the pool's Composer keeps `&options`
        // while its Screen keeps `&colors`. Put the pool first and both
        // referents are gone before the objects that point at them.
        Options options;
        TerminalColors colors;
        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer* composer = nullptr;
        Screen* screen = nullptr;
        Vector<TerminalRow> rows;
    };

    // A renderer that knows one terminal and nothing about panes: the
    // shadow mirror in test_mode.cpp is one of these, and so is anything
    // written against Renderer before A2. What keeps it correct is the
    // default update() in render.h, which forwards a one-pane frame and
    // refuses a wider one instead of drawing the first pane of it.
    struct SingleTerminalRenderer final: Renderer {
        // Overriding one overload hides the other; production reaches
        // this class through a Renderer*, and so do the tests below.
        using Renderer::update;

        bool update(const TerminalUpdate&) override {
            ++terminalUpdates;
            return true;
        }

        bool repaint() override {
            return false;
        }

        int terminalUpdates = 0;
    };

    static bool cellHasInk(const ReferenceImage& image, u16 glyphWidth, u16 glyphHeight, u16 cell, Color background) {
        for (u16 y = 0; y < glyphHeight; ++y) {
            for (u16 x = 0; x < glyphWidth; ++x) {
                if (!(cellPixel(image, (u16)(cell * glyphWidth + x), y) == background)) {
                    return true;
                }
            }
        }
        return false;
    }
}

ScreenFixture::ScreenFixture(u16 columns, u16 rows, u16 border, u16 topReserve) {
    colors.defaultForeground = {1, 2, 3};
    colors.defaultBackground = {4, 5, 6};
    composer = pool->make<Composer>(pool.mutPtr());
    options.border = border;
    composer->setOptions(&options);
    // Before the glyph size and the resize below: a reserve claimed
    // before there is a grid is simply remembered, and the first resize
    // counts the grid out of it.
    composer->setChromeReserve(ChromeSide::Top, topReserve);
    // Only the embedded resolver: the tests must not depend on system
    // fonts.
    while (!composer->fontResolvers.empty()) {
        composer->fontResolvers.popFront();
    }
    composer->fontResolvers.pushBack(createEmbeddedFontResolver(*composer));
    composer->fonts = Fontpack::create(*composer, *pool, nullptr, 0, nullptr, 0, 16);
    composer->geometry.setCellPixelSize(composer->fonts->getPx(), composer->fonts->getPy());
    composer->extras.replace(CellExtraStore::create(composer->extras, *composer->pool, (size_t)(columns)*rows));
    const Insets insets = composer->contentInsets();
    composer->resize((u16)(gridPixelWidth(columns, insets, composer->geometry.cellPixelWidth)), (u16)(gridPixelHeight(rows, insets, composer->geometry.cellPixelHeight)));
    composer->shaper = SpanShaper::create(*composer, *pool);
    screen = Screen::createPrimary(composer->extras, *pool, columns, rows, &colors, 8);
}

void ScreenFixture::writeText(u16 row, u16 column, const char* text, const TerminalCell& attrs) {
    writeTextTo(*screen, row, column, text, attrs);
}

TerminalUpdate ScreenFixture::capture() {
    return captureFrom(*composer, *screen, colors, rows);
}

u16 FakeFontpack::getPx() const {
    return 0;
}

u16 FakeFontpack::getPy() const {
    return 0;
}

float FakeFontpack::boxDrawingStroke() const {
    return 0.0f;
}

bool FakeFontpack::hasBold() const {
    return true;
}

bool FakeFontpack::hasItalic() const {
    return true;
}

bool FakeFontpack::hasBoldItalic() const {
    return true;
}

Font* FakeFontpack::styledFace(Font* face, FontStyle) const {
    return face;
}

Font* FakeFontpack::resolveFace(const u32*, size_t) {
    return nullptr;
}

void FakeFontpack::adoptFaceFor(const FontFaceMiss&) {
}

STD_TEST_SUITE(ReferenceRenderer) {
    STD_TEST(RejectsMismatchedCellCount) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        FakeFontpack fonts;
        configure(composer, fonts, 1, 1, 1, 1);
        ReferenceFixture renderer(composer);
        TerminalColors colors;
        TerminalUpdate update;
        update.colors = &colors;
        // The grid is stated, so what this test refuses is still the
        // missing rows and not the missing grid.
        gridOf(update, 1, 1);

        const ReferenceImage image = renderer->render(update);

        STD_INSIST(image.pixels == nullptr);
        STD_INSIST(image.length == 0);
    }

    // A1: cell 0,0 starts at the content insets, not at the surface
    // corner. Production still asks for a uniform border, so what this
    // pins is the origin being applied, once, on both axes - dropping it,
    // halving it or doubling it moves the cell off these pixels.
    STD_TEST(PlacesTheGridAtTheContentInsets) {
        constexpr u16 border = 6;
        ScreenFixture fx(2, 1, border);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct({255, 0, 0}));
        attrs.setBackground(CellColor::direct({0, 0, 255}));
        // A space carries the cell's background and no ink, so the whole
        // cell is one color to look for.
        fx.writeText(0, 0, " ", attrs);
        ReferenceFixture renderer(*fx.composer);

        const ReferenceImage image = renderer->render(fx.capture());

        STD_INSIST(image.pixels != nullptr);
        const Insets insets = fx.composer->contentInsets();
        STD_INSIST(insets.left == border && insets.top == border);
        STD_INSIST((cellPixel(image, insets.left, insets.top) == Color{0, 0, 255}));
        STD_INSIST((cellPixel(image, (u16)(insets.left + fx.composer->geometry.cellPixelWidth - 1), (u16)(insets.top + fx.composer->geometry.cellPixelHeight - 1)) == Color{0, 0, 255}));
        // The pixel one step back on either axis is outside every cell.
        STD_INSIST((!(cellPixel(image, (u16)(insets.left - 1), insets.top) == Color{0, 0, 255})));
        STD_INSIST((!(cellPixel(image, insets.left, (u16)(insets.top - 1)) == Color{0, 0, 255})));
    }

    // R4-test, debt of wave 3, item 2 - the one the whole wave was
    // sequenced around. The reference renderer reads contentInsets()
    // itself and pairs `left` with x and `top` with y; until T6 put a
    // reserve on the top edge those two were the same number, so
    // transposing them (mutant M14 of T4, reproduced as R7 by R3-test)
    // survived every test in the tree and the entire `build test -k`
    // graph. With the strip reserved they name different pixels, and the
    // transposed origin puts cell 0,0 where nothing may draw.
    STD_TEST(PlacesTheGridAtInsetsThatDifferOnEveryAxis) {
        constexpr u16 border = 6;
        constexpr u16 strip = 32;
        ScreenFixture fx(8, 3, border, strip);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct({255, 0, 0}));
        attrs.setBackground(CellColor::direct({0, 0, 255}));
        // A space carries the cell's background and no ink, so the whole
        // cell is one color to look for.
        fx.writeText(0, 0, " ", attrs);
        ReferenceFixture renderer(*fx.composer);

        const ReferenceImage image = renderer->render(fx.capture());

        STD_INSIST(image.pixels != nullptr);
        const Insets insets = fx.composer->contentInsets();
        STD_INSIST(insets.left == border);
        STD_INSIST(insets.bottom == border);
        STD_INSIST(insets.right == border);
        STD_INSIST(insets.top == border + strip);
        // The premise of the whole test: the two coordinates of the
        // origin are different numbers now.
        STD_INSIST(insets.top != insets.left);

        // Cell 0,0 begins at (left, top) and fills its glyph box.
        STD_INSIST((cellPixel(image, insets.left, insets.top) == Color{0, 0, 255}));
        STD_INSIST((cellPixel(image, (u16)(insets.left + fx.composer->geometry.cellPixelWidth - 1), (u16)(insets.top + fx.composer->geometry.cellPixelHeight - 1)) == Color{0, 0, 255}));
        // The pixel one step back on either axis is outside every cell,
        // and the two axes are checked separately: this is what tells a
        // dropped `top` from a dropped `left`.
        STD_INSIST((!(cellPixel(image, (u16)(insets.left - 1), insets.top) == Color{0, 0, 255})));
        STD_INSIST((!(cellPixel(image, insets.left, (u16)(insets.top - 1)) == Color{0, 0, 255})));

        // And the transposed origin - x taken from `top`, y from `left`,
        // which is M14 exactly - names a pixel inside this surface that
        // belongs to no cell at all.
        STD_INSIST(insets.top < image.width);
        STD_INSIST(insets.left < image.height);
        STD_INSIST((!(cellPixel(image, insets.top, insets.left) == Color{0, 0, 255})));
        STD_INSIST((!(cellPixel(image, (u16)(insets.top + fx.composer->geometry.cellPixelWidth - 1), (u16)(insets.left + fx.composer->geometry.cellPixelHeight - 1)) == Color{0, 0, 255})));
    }

    STD_TEST(ScreenStripsBlendInkOverBackground) {
        ScreenFixture fx(4, 1);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct({255, 0, 0}));
        attrs.setBackground(CellColor::direct({0, 0, 255}));
        fx.writeText(0, 0, "a", attrs);
        ReferenceFixture renderer(*fx.composer);

        const ReferenceImage image = renderer->render(fx.capture());

        STD_INSIST(image.pixels != nullptr);
        const u16 width = fx.composer->geometry.cellPixelWidth;
        const u16 height = fx.composer->geometry.cellPixelHeight;
        // The glyph cell blends ink toward the foreground; a solid-core
        // pixel is nearly pure red.
        bool solid = false;
        bool background = false;
        for (u16 y = 0; y < height; ++y) {
            for (u16 x = 0; x < width; ++x) {
                const Color pixel = cellPixel(image, x, y);
                solid = solid || (pixel.red > 200 && pixel.blue < 60);
                background = background || pixel == Color{0, 0, 255};
            }
        }
        STD_INSIST(solid);
        STD_INSIST(background);
        // The blank neighbour renders the default background exactly.
        for (u16 y = 0; y < height; ++y) {
            for (u16 x = 0; x < width; ++x) {
                STD_INSIST((cellPixel(image, (u16)(width + x), y) == Color{4, 5, 6}));
            }
        }
    }

    STD_TEST(InverseAndScreenReverseCancelEachOther) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        FakeFontpack fonts;
        configure(composer, fonts, 1, 1, 1, 1);
        ReferenceFixture renderer(composer);
        TerminalColors colors;
        TerminalCell cell = coloredCell({255, 0, 0}, {0, 0, 255});
        cell.inverse = true;
        TerminalRow row{&cell, 0, 0};
        TerminalUpdate update;
        update.rows = &row;
        update.rowCount = 1;
        update.colors = &colors;
        gridOf(update, 1, 1);

        // No strips reach this renderer, so the cell paints its
        // background: the inverted foreground, then the original
        // background when screen reverse cancels the inversion.
        ReferenceImage image = renderer->render(update);
        STD_INSIST((cellPixel(image, 0, 0) == Color{255, 0, 0}));

        update.screenReverse = true;
        image = renderer->render(update);
        STD_INSIST((cellPixel(image, 0, 0) == Color{0, 0, 255}));
    }

    STD_TEST(AppliesSparseUpdatesToRetainedCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        FakeFontpack fonts;
        configure(composer, fonts, 2, 1, 1, 1);
        ReferenceFixture renderer(composer);
        TerminalColors colors;
        TerminalCell initial[2]{};
        initial[0].setBackground(CellColor::direct({10, 20, 30}));
        initial[1].setBackground(CellColor::direct({40, 50, 60}));
        TerminalRow row{initial, 0, 0};
        TerminalUpdate update;
        update.rows = &row;
        update.rowCount = 1;
        update.colors = &colors;
        gridOf(update, 2, 1);

        ReferenceImage image = renderer->render(update);
        STD_INSIST((cellPixel(image, 0, 0) == Color{10, 20, 30}));
        STD_INSIST((cellPixel(image, 1, 0) == Color{40, 50, 60}));

        // A frame with no damaged rows leaves the retained cells alone.
        initial[1].setBackground(CellColor::direct({70, 80, 90}));
        image = renderer->render(update);
        STD_INSIST((cellPixel(image, 1, 0) == Color{70, 80, 90}));
        update.rowCount = 0;
        initial[1].setBackground(CellColor::direct({40, 50, 60}));
        image = renderer->render(update);

        STD_INSIST((cellPixel(image, 0, 0) == Color{10, 20, 30}));
        STD_INSIST((cellPixel(image, 1, 0) == Color{70, 80, 90}));
    }

    STD_TEST(AppliesSelectionColors) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        FakeFontpack fonts;
        configure(composer, fonts, 1, 1, 2, 1);
        ReferenceFixture renderer(composer);
        TerminalColors colors;
        TerminalCell cell = coloredCell({255, 0, 0}, {0, 0, 255});
        TerminalRow row{&cell, 0, 0};
        TerminalUpdate update;
        update.rows = &row;
        update.rowCount = 1;
        update.colors = &colors;
        gridOf(update, 1, 1);
        update.snappedSelection = Rect(0, 0);
        update.selectionColorMask = 3;
        update.selectionForeground = {1, 2, 3};
        update.selectionBackground = {4, 5, 6};

        const ReferenceImage image = renderer->render(update);

        STD_INSIST((cellPixel(image, 0, 0) == Color{4, 5, 6}));
        STD_INSIST((cellPixel(image, 1, 0) == Color{4, 5, 6}));
    }

    STD_TEST(CursorKeepsSelectedForegroundWhenEnabled) {
        ScreenFixture fx(1, 1);
        TerminalCell attrs = coloredCell({255, 0, 0}, {0, 0, 255});
        // An overline gives exact foreground pixels without glyph antialiasing.
        attrs.overline = true;
        fx.writeText(0, 0, " ", attrs);
        Options options;
        fx.composer->setOptions(&options);
        ReferenceFixture renderer(*fx.composer);
        TerminalUpdate update = fx.capture();
        update.snappedSelection = Rect(0, 0);
        update.cursor.color = {0, 255, 0};
        update.cursor.style = TerminalCursor::Style::filled_block;
        const auto checkColors = [&](Color foreground) {
            const ReferenceImage image = renderer->render(update);
            STD_INSIST(image.pixels != nullptr);
            STD_INSIST(cellPixel(image, 0, 0) == foreground);
            STD_INSIST(cellPixel(image, 0, 1) == update.cursor.color);
        };

        checkColors({255, 0, 0});
        options.cursorKeepSelectionFg = true;
        checkColors({0, 0, 255});
        update.snappedSelection = {};
        checkColors({0, 0, 255});
    }

    STD_TEST(CursorKeepsReverseVideoForegroundWhenEnabled) {
        ScreenFixture fx(1, 1);
        TerminalCell attrs = coloredCell({255, 0, 0}, {0, 0, 255});
        attrs.inverse = true;
        attrs.overline = true;
        fx.writeText(0, 0, " ", attrs);
        Options options;
        fx.composer->setOptions(&options);
        ReferenceFixture renderer(*fx.composer);
        TerminalUpdate update = fx.capture();
        update.cursor.color = {0, 255, 0};
        update.cursor.style = TerminalCursor::Style::filled_block;
        const auto checkColors = [&](Color foreground) {
            const ReferenceImage image = renderer->render(update);
            STD_INSIST(image.pixels != nullptr);
            STD_INSIST(cellPixel(image, 0, 0) == foreground);
            STD_INSIST(cellPixel(image, 0, 1) == update.cursor.color);
        };

        checkColors({255, 0, 0});
        options.cursorKeepSelectionFg = true;
        checkColors({0, 0, 255});
        attrs.inverse = false;
        fx.writeText(0, 0, " ", attrs);
        update = fx.capture();
        update.cursor.color = {0, 255, 0};
        update.cursor.style = TerminalCursor::Style::filled_block;
        checkColors({0, 0, 255});
    }

    STD_TEST(SelectionOfWideContinuationHighlightsWholeGlyph) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        FakeFontpack fonts;
        configure(composer, fonts, 2, 1, 1, 1);
        ReferenceFixture renderer(composer);
        TerminalColors colors;
        TerminalCell cells[2]{
            coloredCell({255, 0, 0}, {0, 0, 255}),
            coloredCell({255, 0, 0}, {0, 0, 255}),
        };
        cells[0].dwidth = true;
        cells[1].dwidth_cont = true;
        TerminalRow row{cells, 0, 0};
        TerminalUpdate update;
        update.rows = &row;
        update.rowCount = 1;
        update.colors = &colors;
        gridOf(update, 2, 1);
        update.snappedSelection = Rect(1, 0, 2, 0);
        update.snappedSelection.rectangular = true;
        update.selectionColorMask = 2;
        update.selectionBackground = {4, 5, 6};

        const ReferenceImage image = renderer->render(update);

        STD_INSIST((cellPixel(image, 0, 0) == Color{4, 5, 6}));
        STD_INSIST((cellPixel(image, 1, 0) == Color{4, 5, 6}));
    }

    STD_TEST(GraphemeClusterRendersInkFromStrips) {
        ScreenFixture fx(2, 1);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::defaultForeground());
        attrs.setBackground(CellColor::defaultBackground());
        const u32 codepoints[] = {'a', 0x0301};
        fx.screen->writeGrapheme(0, 0, codepoints, 2, false, attrs, 0, 0, attrs);
        ReferenceFixture renderer(*fx.composer);

        const ReferenceImage image = renderer->render(fx.capture());

        STD_INSIST(image.pixels != nullptr);
        STD_INSIST(cellHasInk(image, fx.composer->geometry.cellPixelWidth, fx.composer->geometry.cellPixelHeight, 0, {4, 5, 6}));
    }

    STD_TEST(ColorStripCompositesOverBackground) {
        ScreenFixture fx(4, 1);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::defaultForeground());
        attrs.setBackground(CellColor::defaultBackground());
        const u32 emoji = 0x1f600;
        fx.screen->writeGrapheme(0, 0, &emoji, 1, true, attrs, 0, 0, attrs);
        ReferenceFixture renderer(*fx.composer);

        const ReferenceImage image = renderer->render(fx.capture());

        STD_INSIST(image.pixels != nullptr);
        const u16 width = fx.composer->geometry.cellPixelWidth;
        const u16 height = fx.composer->geometry.cellPixelHeight;
        size_t chromatic = 0;
        for (u16 y = 0; y < height; ++y) {
            for (u16 x = 0; x < 2 * width; ++x) {
                const Color pixel = cellPixel(image, x, y);
                const u8 high = pixel.red > pixel.green ? pixel.red : pixel.green;
                const u8 low = pixel.red < pixel.green ? pixel.red : pixel.green;
                chromatic += (high > pixel.blue ? high : pixel.blue) != (low < pixel.blue ? low : pixel.blue);
            }
        }
        STD_INSIST(chromatic > 16);
    }

    STD_TEST(PreeditOverlayCoversUnderlyingStrips) {
        ScreenFixture fx(4, 1);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::defaultForeground());
        attrs.setBackground(CellColor::defaultBackground());
        fx.writeText(0, 0, "ab", attrs);
        ReferenceFixture renderer(*fx.composer);

        ReferenceImage image = renderer->render(fx.capture());
        STD_INSIST(image.pixels != nullptr);
        const u16 width = fx.composer->geometry.cellPixelWidth;
        const u16 height = fx.composer->geometry.cellPixelHeight;
        STD_INSIST(cellHasInk(image, width, height, 0, {4, 5, 6}));

        // A blank preedit window over the text hides its strips: the
        // covered cells fall back to their plain background.
        TerminalUpdate update = fx.capture();
        TerminalCell preedit[2]{attrs, attrs};
        update.overlayCells = preedit;
        update.overlayRow = 0;
        update.overlayColumn = 0;
        update.overlayCount = 2;

        image = renderer->render(update);
        STD_INSIST(image.pixels != nullptr);
        STD_INSIST(!cellHasInk(image, width, height, 0, {4, 5, 6}));
        STD_INSIST(!cellHasInk(image, width, height, 1, {4, 5, 6}));
    }

    // A2. Two panes, one frame, two rectangles - and each pane owning
    // its rectangle is what all four assertions below are about.
    //
    // The surface holds a 4x3 grid with a border on every side; the two
    // panes take a row each from the top, which leaves the bottom row of
    // the surface belonging to no pane at all. Each pane draws the whole
    // three-row grid of its own terminal into its one-row rectangle.
    // What the panes must not share:
    //
    //   - their clear. A pane's padding carries its own terminal's
    //     background, so a clear that took the whole target would paint
    //     the first pane over with the second one's colour.
    //   - their origin. The lower pane's cell 0,0 lands at its own
    //     rectangle plus the insets, not at the surface corner.
    //   - their pixels. The rows that do not fit a pane would land on
    //     the pane below it and on the strip below them both; the clip
    //     to the pane rectangle is what stops them.
    STD_TEST(DrawsTwoPanesInTheirOwnRectangles) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 3, border);
        auto* const other = fx.pool->make<TerminalColors>();
        // The pool outlives the screen that keeps a pointer to these.
        other->defaultForeground = {1, 2, 3};
        other->defaultBackground = {0, 255, 0};
        Screen* const second = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 3, other, 8);
        Vector<TerminalRow> secondRows;

        TerminalCell first{};
        first.setForeground(CellColor::direct({255, 0, 0}));
        first.setBackground(CellColor::direct({255, 0, 0}));
        TerminalCell spilled{};
        spilled.setForeground(CellColor::direct({255, 255, 0}));
        spilled.setBackground(CellColor::direct({255, 255, 0}));
        TerminalCell lower{};
        lower.setForeground(CellColor::direct({255, 255, 255}));
        lower.setBackground(CellColor::direct({255, 255, 255}));
        // A space carries the cell's background and no ink, so each cell
        // is one colour to look for.
        fx.writeText(0, 0, " ", first);
        writeTextTo(*second, 0, 0, " ", lower);
        // The row that does not fit either pane, in the pane that draws
        // last: nothing may overwrite it afterwards, so where it lands is
        // where it stays.
        writeTextTo(*second, 1, 0, " ", spilled);

        ReferenceFixture renderer(*fx.composer);
        const u16 paneHeight = (u16)(border + fx.composer->geometry.cellPixelHeight);
        STD_INSIST(fx.composer->geometry.pixelHeight > 2 * paneHeight);
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, fx.composer->geometry.pixelWidth, paneHeight}, fx.capture()},
            {PixelRect{0, paneHeight, fx.composer->geometry.pixelWidth, paneHeight}, captureFrom(*fx.composer, *second, *other, secondRows)},
        };

        STD_INSIST(renderer.renderer->update(panes, 2));
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);

        // Each pane cleared its own rectangle with its own background.
        STD_INSIST((cellPixel(image, 0, 0) == Color{4, 5, 6}));
        STD_INSIST((cellPixel(image, 0, paneHeight) == Color{0, 255, 0}));
        // Each pane put cell 0,0 at its own rectangle plus the insets.
        STD_INSIST((cellPixel(image, border, border) == Color{255, 0, 0}));
        STD_INSIST((cellPixel(image, border, (u16)(paneHeight + border)) == Color{255, 255, 255}));
        // The row that did not fit its pane is nowhere on the surface,
        // and the strip no pane claimed was never written at all - the
        // fixture hands over a zeroed target.
        for (u16 y = 0; y < image.height; ++y) {
            for (u16 x = 0; x < image.width; ++x) {
                STD_INSIST((!(cellPixel(image, x, y) == Color{255, 255, 0})));
                if (y >= 2 * paneHeight) {
                    STD_INSIST((cellPixel(image, x, y) == Color{0, 0, 0}));
                }
            }
        }
    }

    // The same frame with the panes handed over in the other order. A
    // pane is placed by its rectangle and not by its turn, so the image
    // must come out the same - which is what tells a renderer that reads
    // the list from a renderer that draws whatever it was given last
    // wherever the previous pane was.
    STD_TEST(PaneOrderDoesNotMovePanes) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 2, border);
        auto* const other = fx.pool->make<TerminalColors>();
        other->defaultForeground = {1, 2, 3};
        other->defaultBackground = {0, 255, 0};
        Screen* const second = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, other, 8);
        Vector<TerminalRow> secondRows;

        TerminalCell top{};
        top.setForeground(CellColor::direct({255, 0, 0}));
        top.setBackground(CellColor::direct({255, 0, 0}));
        TerminalCell bottom{};
        bottom.setForeground(CellColor::direct({255, 255, 255}));
        bottom.setBackground(CellColor::direct({255, 255, 255}));
        fx.writeText(0, 0, " ", top);
        writeTextTo(*second, 0, 0, " ", bottom);

        ReferenceFixture renderer(*fx.composer);
        const u16 half = (u16)(fx.composer->geometry.pixelHeight / 2);
        const PaneUpdate panes[2] = {
            {PixelRect{0, half, fx.composer->geometry.pixelWidth, half}, captureFrom(*fx.composer, *second, *other, secondRows)},
            {PixelRect{0, 0, fx.composer->geometry.pixelWidth, half}, fx.capture()},
        };

        STD_INSIST(renderer.renderer->update(panes, 2));
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);

        STD_INSIST((cellPixel(image, 0, 0) == Color{4, 5, 6}));
        STD_INSIST((cellPixel(image, 0, half) == Color{0, 255, 0}));
        STD_INSIST((cellPixel(image, border, border) == Color{255, 0, 0}));
        STD_INSIST((cellPixel(image, border, (u16)(half + border)) == Color{255, 255, 255}));
    }

    // The pane-less update() is the one-pane frame spelled shorter: the
    // pane it builds is the whole surface, so the padding it clears and
    // the origin it draws at are the surface's own.
    STD_TEST(ThePaneLessUpdateFillsTheSurface) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 2, border);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct({255, 0, 0}));
        attrs.setBackground(CellColor::direct({0, 0, 255}));
        fx.writeText(1, 3, " ", attrs);
        ReferenceFixture renderer(*fx.composer);

        const ReferenceImage image = renderer->render(fx.capture());

        STD_INSIST(image.pixels != nullptr);
        // The last cell of the last row, which only a pane the size of
        // the surface reaches.
        const u16 x = (u16)(border + 3 * fx.composer->geometry.cellPixelWidth);
        const u16 y = (u16)(border + fx.composer->geometry.cellPixelHeight);
        STD_INSIST((cellPixel(image, x, y) == Color{0, 0, 255}));
        STD_INSIST((cellPixel(image, (u16)(image.width - 1), (u16)(image.height - 1)) == Color{4, 5, 6}));
    }

    // A frame with no panes in it is not a frame: nothing was drawn, so
    // nothing may be presented, and the caller has to be told - it asks
    // for another frame when it is (application.cpp:511).
    STD_TEST(AnEmptyFrameDrawsNothingAndSaysSo) {
        ScreenFixture fx(4, 2);
        ReferenceFixture renderer(*fx.composer);

        STD_INSIST(!renderer.renderer->update((const PaneUpdate*)(nullptr), 0));
    }

    // A pane whose rectangle has fallen off the surface - a resize
    // caught mid-flight - draws nothing at all. Its clip extents
    // saturate to zero, because an extent computed by unsigned
    // subtraction would come out enormous and clip nothing: the pane
    // would then write its cells past the right edge of the rows it was
    // given, which is the neighbouring row's pixels.
    STD_TEST(APaneOffTheSurfaceDrawsNothing) {
        ScreenFixture fx(4, 2);
        fx.writeText(0, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        ReferenceFixture renderer(*fx.composer);
        const PaneUpdate panes[1] = {
            {PixelRect{(u16)(fx.composer->geometry.pixelWidth + 4), 0, fx.composer->geometry.cellPixelWidth, fx.composer->geometry.cellPixelHeight}, fx.capture()},
        };

        STD_INSIST(renderer.renderer->update(panes, 1));
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);

        // The fixture hands over a zeroed target, so every pixel that is
        // still zero is a pixel nobody wrote.
        for (u16 y = 0; y < image.height; ++y) {
            for (u16 x = 0; x < image.width; ++x) {
                STD_INSIST((cellPixel(image, x, y) == Color{0, 0, 0}));
            }
        }
    }

    // A9. Two panes of two different grids in one frame, each drawn with
    // its own. Before A9 the size came from the composer, so both panes
    // walked the window's grid: the narrow one would read its rows past
    // their end and draw six columns where it has three.
    //
    // The window is wide enough that the right pane's rectangle holds
    // twice its own grid, which is what makes the assertion possible at
    // all - column 3 of that pane is inside its rectangle, so a cell
    // drawn there is a cell drawn, not a cell clipped.
    STD_TEST(DrawsTwoPanesOfDifferentGrids) {
        constexpr u16 border = 3;
        constexpr u16 wideColumns = 6;
        constexpr u16 narrowColumns = 3;
        ScreenFixture fx(16, 2, border);
        auto* const wideColors = fx.pool->make<TerminalColors>();
        wideColors->defaultForeground = {1, 2, 3};
        wideColors->defaultBackground = {0, 0, 128};
        auto* const narrowColors = fx.pool->make<TerminalColors>();
        narrowColors->defaultForeground = {1, 2, 3};
        narrowColors->defaultBackground = {0, 128, 0};
        Screen* const wide = Screen::createPrimary(fx.composer->extras, *fx.pool, wideColumns, 2, wideColors, 8);
        Screen* const narrow = Screen::createPrimary(fx.composer->extras, *fx.pool, narrowColumns, 2, narrowColors, 8);
        Vector<TerminalRow> wideRows;
        Vector<TerminalRow> narrowRows;

        // A space carries the cell's background and no ink, so every
        // cell is one colour to look for. Both grids are filled whole,
        // so nothing a pane draws outside its own columns can be a cell
        // that happens to be blank.
        for (u16 column = 0; column < wideColumns; ++column) {
            writeTextTo(*wide, 0, column, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
            writeTextTo(*wide, 1, column, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        }
        for (u16 column = 0; column < narrowColumns; ++column) {
            writeTextTo(*narrow, 0, column, " ", coloredCell({255, 255, 255}, {255, 255, 255}));
            writeTextTo(*narrow, 1, column, " ", coloredCell({255, 255, 255}, {255, 255, 255}));
        }

        ReferenceFixture renderer(*fx.composer);
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        const u16 half = (u16)(fx.composer->geometry.pixelWidth / 2);
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, half, fx.composer->geometry.pixelHeight}, captureFrom(*fx.composer, *wide, *wideColors, wideRows)},
            {PixelRect{half, 0, (u16)(fx.composer->geometry.pixelWidth - half), fx.composer->geometry.pixelHeight}, captureFrom(*fx.composer, *narrow, *narrowColors, narrowRows)},
        };
        // The premise: the right pane's rectangle has room for twice its
        // own columns, so column 3 of it is inside the clip.
        STD_INSIST(fx.composer->geometry.pixelWidth - half > border + 2 * narrowColumns * glyphWidth);

        STD_INSIST(renderer.renderer->update(panes, 2));
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);

        const u16 row = (u16)(border + glyphHeight / 2);
        // The wide pane drew all six of its columns, the last one
        // included.
        for (u16 column = 0; column < wideColumns; ++column) {
            const u16 x = (u16)(border + column * glyphWidth + glyphWidth / 2);
            STD_INSIST((cellPixel(image, x, row) == Color{255, 0, 0}));
        }
        // The narrow pane drew exactly three, and its fourth column is
        // its own padding - not a cell, and in particular not the wide
        // pane's colour read out of a row that has no fourth cell.
        for (u16 column = 0; column < narrowColumns; ++column) {
            const u16 x = (u16)(half + border + column * glyphWidth + glyphWidth / 2);
            STD_INSIST((cellPixel(image, x, row) == Color{255, 255, 255}));
        }
        for (u16 column = narrowColumns; column < wideColumns; ++column) {
            const u16 x = (u16)(half + border + column * glyphWidth + glyphWidth / 2);
            STD_INSIST((cellPixel(image, x, row) == Color{0, 128, 0}));
        }
        // Both rows of both panes, so a grid taken from the neighbour is
        // caught on the vertical axis too.
        const u16 secondRow = (u16)(border + glyphHeight + glyphHeight / 2);
        STD_INSIST((cellPixel(image, (u16)(border + glyphWidth / 2), secondRow) == Color{255, 0, 0}));
        STD_INSIST((cellPixel(image, (u16)(half + border + glyphWidth / 2), secondRow) == Color{255, 255, 255}));
    }

    // A6-3. The second pane sends only the row that changed - which is
    // the ordinary frame, TerminalUpdate::rows being the damaged rows -
    // and its undamaged row must still be its own.
    //
    // updateOnce() used to start every pane from `Vector<ReferenceCell>
    // next(cells_)`, the cells of the pane drawn before it, and then lay
    // the damaged rows on top. Pane 1's undamaged rows therefore showed
    // pane 0's content: a wrong frame, in the headless renderer the
    // parity tests of the next waves take for an oracle. No test could
    // catch it, because both pane tests captured through screen.expose()
    // and so sent full damage every time.
    STD_TEST(APartiallyDamagedPaneKeepsItsOwnRowsAndNotItsNeighbours) {
        constexpr u16 border = 3;
        // Six rows of window for two panes of two: half the surface has
        // to hold a whole two-row grid plus its border, or the row this
        // test is about falls outside the pane's clip and the assertion
        // stops being about the retain.
        ScreenFixture fx(4, 6, border);
        auto* const upperColors = fx.pool->make<TerminalColors>();
        upperColors->defaultForeground = {1, 2, 3};
        upperColors->defaultBackground = {0, 0, 128};
        auto* const lowerColors = fx.pool->make<TerminalColors>();
        lowerColors->defaultForeground = {1, 2, 3};
        lowerColors->defaultBackground = {0, 128, 0};
        Screen* const upper = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, upperColors, 8);
        Screen* const lower = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, lowerColors, 8);
        Vector<TerminalRow> upperRows;
        Vector<TerminalRow> lowerRows;

        writeTextTo(*upper, 0, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*upper, 1, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*lower, 0, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));
        writeTextTo(*lower, 1, 0, " ", coloredCell({0, 255, 255}, {0, 255, 255}));

        ReferenceFixture renderer(*fx.composer);
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        const u16 half = (u16)(fx.composer->geometry.pixelHeight / 2);
        {
            // The establishing frame: both panes whole, so both retains
            // hold their own pane's cells.
            const PaneUpdate panes[2] = {
                {PixelRect{0, 0, fx.composer->geometry.pixelWidth, half}, captureFrom(*fx.composer, *upper, *upperColors, upperRows)},
                {PixelRect{0, half, fx.composer->geometry.pixelWidth, half}, captureFrom(*fx.composer, *lower, *lowerColors, lowerRows)},
            };
            STD_INSIST(renderer.renderer->update(panes, 2));
        }
        upper->resetDamage();
        lower->resetDamage();

        // The upper pane repaints wholly, in a colour the lower pane has
        // never had; the lower pane damages its first row only.
        upper->expose();
        writeTextTo(*upper, 0, 0, " ", coloredCell({255, 0, 255}, {255, 0, 255}));
        writeTextTo(*upper, 1, 0, " ", coloredCell({255, 0, 255}, {255, 0, 255}));
        writeTextTo(*lower, 0, 0, " ", coloredCell({255, 255, 0}, {255, 255, 0}));

        const TerminalUpdate lowerUpdate = captureDamagedFrom(*lower, *lowerColors, lowerRows);
        // The premise of the whole test: the second pane's frame is
        // partial. A full one proves nothing here.
        STD_INSIST(lowerUpdate.rowCount == 1);
        STD_INSIST(lowerUpdate.rows[0].row == 0);
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, fx.composer->geometry.pixelWidth, half}, captureDamagedFrom(*upper, *upperColors, upperRows)},
            {PixelRect{0, half, fx.composer->geometry.pixelWidth, half}, lowerUpdate},
        };

        STD_INSIST(renderer.renderer->update(panes, 2));
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);

        const u16 x = (u16)(border + fx.composer->geometry.cellPixelWidth / 2);
        // The damaged row of each pane carries what that pane sent.
        STD_INSIST((cellPixel(image, x, (u16)(border + glyphHeight / 2)) == Color{255, 0, 255}));
        STD_INSIST((cellPixel(image, x, (u16)(half + border + glyphHeight / 2)) == Color{255, 255, 0}));
        // And the undamaged row of the lower pane is still the lower
        // pane's own - not the upper pane's, which is what a retain
        // shared between the two would have put there.
        const u16 undamaged = (u16)(half + border + glyphHeight + glyphHeight / 2);
        STD_INSIST((cellPixel(image, x, undamaged) == Color{0, 255, 255}));
        STD_INSIST((!(cellPixel(image, x, undamaged) == Color{255, 0, 255})));
    }

    // A9. Zero is a refused frame and never a window-sized default. The
    // grid states the width of TerminalRow::cells and the height row.row
    // indexes into, so an update that leaves it out is an update whose
    // cells have no length - and a renderer that filled it in from the
    // window would hide exactly the gap the field exists to mark.
    //
    // Both halves, and a control: the same frame with the grid stated is
    // accepted, so what the two refusals are about is the grid and not
    // the rest of the update.
    STD_TEST(AFrameWithoutAGridIsRefused) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        FakeFontpack fonts;
        configure(composer, fonts, 2, 1, 1, 1);
        ReferenceFixture renderer(composer);
        TerminalColors colors;
        TerminalCell cells[2]{
            coloredCell({255, 0, 0}, {255, 0, 0}),
            coloredCell({255, 0, 0}, {255, 0, 0}),
        };
        TerminalRow row{cells, 0, 0};
        TerminalUpdate update;
        update.rows = &row;
        update.rowCount = 1;
        update.colors = &colors;

        gridOf(update, 0, 1);
        STD_INSIST(!renderer.renderer->update(update));
        gridOf(update, 2, 0);
        STD_INSIST(!renderer.renderer->update(update));
        // The control: everything else about this frame is acceptable.
        gridOf(update, 2, 1);
        STD_INSIST(renderer.renderer->update(update));
    }

    // A6-4. The panes of a frame are matched to their retained cells by
    // identity - the Screen each one shapes through - and not by their
    // place in the list. A frame with as many panes in another order (a
    // neighbour closed and the tree rebalanced, two panes swapped on a
    // drag) is a frame in which nothing is where it was, so it is a
    // reshape: it needs every row of every pane, exactly as a resized
    // grid does, and a partial one is refused rather than drawn out of
    // the neighbour's cells.
    //
    // The control is the second half: the same swap, sent whole, is
    // accepted and puts each pane's own content in its own rectangle. So
    // the refusal above is about the order and not about the swap being
    // unrepresentable.
    STD_TEST(SwappedPanesAreAReshapeAndNotTheNeighboursRetain) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 6, border);
        auto* const firstColors = fx.pool->make<TerminalColors>();
        firstColors->defaultForeground = {1, 2, 3};
        firstColors->defaultBackground = {0, 0, 128};
        auto* const secondColors = fx.pool->make<TerminalColors>();
        secondColors->defaultForeground = {1, 2, 3};
        secondColors->defaultBackground = {0, 128, 0};
        Screen* const first = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, firstColors, 8);
        Screen* const second = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, secondColors, 8);
        Vector<TerminalRow> firstRows;
        Vector<TerminalRow> secondRows;
        writeTextTo(*first, 0, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*first, 1, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*second, 0, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));
        writeTextTo(*second, 1, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));

        ReferenceFixture renderer(*fx.composer);
        const u16 half = (u16)(fx.composer->geometry.pixelHeight / 2);
        const PixelRect top{0, 0, fx.composer->geometry.pixelWidth, half};
        const PixelRect bottom{0, half, fx.composer->geometry.pixelWidth, half};
        {
            const PaneUpdate panes[2] = {
                {top, captureFrom(*fx.composer, *first, *firstColors, firstRows)},
                {bottom, captureFrom(*fx.composer, *second, *secondColors, secondRows)},
            };
            STD_INSIST(renderer.renderer->update(panes, 2));
        }
        first->resetDamage();
        second->resetDamage();

        // The panes trade places, and each damages one row. Same count,
        // same shapes, different panes in each slot.
        writeTextTo(*first, 0, 0, " ", coloredCell({255, 0, 255}, {255, 0, 255}));
        writeTextTo(*second, 0, 0, " ", coloredCell({255, 255, 0}, {255, 255, 0}));
        {
            const PaneUpdate panes[2] = {
                {top, captureDamagedFrom(*second, *secondColors, secondRows)},
                {bottom, captureDamagedFrom(*first, *firstColors, firstRows)},
            };
            STD_INSIST(panes[0].update.rowCount == 1);
            STD_INSIST(panes[1].update.rowCount == 1);
            STD_INSIST(!renderer.renderer->update(panes, 2));
        }

        // The control: the same swap, sent whole.
        const PaneUpdate panes[2] = {
            {top, captureFrom(*fx.composer, *second, *secondColors, secondRows)},
            {bottom, captureFrom(*fx.composer, *first, *firstColors, firstRows)},
        };
        STD_INSIST(renderer.renderer->update(panes, 2));
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);
        const u16 x = (u16)(border + fx.composer->geometry.cellPixelWidth / 2);
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        // Each pane's own content, in the rectangle it was given now.
        STD_INSIST((cellPixel(image, x, (u16)(border + glyphHeight / 2)) == Color{255, 255, 0}));
        STD_INSIST((cellPixel(image, x, (u16)(border + glyphHeight + glyphHeight / 2)) == Color{255, 255, 255}));
        STD_INSIST((cellPixel(image, x, (u16)(half + border + glyphHeight / 2)) == Color{255, 0, 255}));
        STD_INSIST((cellPixel(image, x, (u16)(half + border + glyphHeight + glyphHeight / 2)) == Color{255, 0, 0}));
    }

    // The retained cells belong to the pane drawn last, so repaint()
    // redraws that pane, in that pane's rectangle. This is the contract
    // T9 inherits together with the debt it names ("repaint() of a
    // многопанельный frame redraws one pane"), written where it goes red
    // if either half of it stops holding: the pane that is retained, and
    // the rectangle it is retained with.
    STD_TEST(RepaintRedrawsThePaneDrawnLastInItsOwnRectangle) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 2, border);
        auto* const other = fx.pool->make<TerminalColors>();
        other->defaultForeground = {1, 2, 3};
        other->defaultBackground = {0, 255, 0};
        Screen* const second = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, other, 8);
        Vector<TerminalRow> secondRows;
        fx.writeText(0, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*second, 0, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));

        ReferenceFixture renderer(*fx.composer);
        const u16 half = (u16)(fx.composer->geometry.pixelHeight / 2);
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, fx.composer->geometry.pixelWidth, half}, fx.capture()},
            {PixelRect{0, half, fx.composer->geometry.pixelWidth, half}, captureFrom(*fx.composer, *second, *other, secondRows)},
        };
        STD_INSIST(renderer.renderer->update(panes, 2));

        // Scribble over the whole surface, then repaint: what comes back
        // is exactly what the repaint drew, and what is still the marker
        // is what it left alone.
        for (size_t index = 0; index < renderer.pixels.length(); ++index) {
            renderer.pixels.mut(index) = 9;
        }
        STD_INSIST(renderer.renderer->repaint());
        const ReferenceImage image = renderer.renderer->image();
        STD_INSIST(image.pixels != nullptr);

        // The lower pane - the one drawn last - is back, cleared with
        // its own background and with its cell 0,0 at its own rectangle.
        STD_INSIST((cellPixel(image, 0, half) == Color{0, 255, 0}));
        STD_INSIST((cellPixel(image, border, (u16)(half + border)) == Color{255, 255, 255}));
        // The upper pane was not redrawn, and the repaint did not spread
        // over its rectangle either.
        STD_INSIST((cellPixel(image, 0, 0) == Color{9, 9, 9}));
        STD_INSIST((cellPixel(image, border, border) == Color{9, 9, 9}));
    }
}

// A2. What Renderer itself promises a backend that never heard of panes.
// The default in render.h is the only thing standing between such a
// backend and a frame it would draw a fraction of; test_mode.cpp's
// shadow mirror is the one in the tree today.
STD_TEST_SUITE(RendererFrameContract) {
    STD_TEST(AOnePaneFrameReachesThePaneLessForm) {
        SingleTerminalRenderer renderer;
        Renderer& contract = renderer;
        const TerminalUpdate update{};
        const PaneUpdate panes[1] = {{PixelRect{0, 0, 80, 24}, update}};

        STD_INSIST(contract.update(panes, 1));
        STD_INSIST(renderer.terminalUpdates == 1);
    }

    STD_TEST(ATwoPaneFrameIsRefusedInsteadOfHalfDrawn) {
        SingleTerminalRenderer renderer;
        Renderer& contract = renderer;
        const TerminalUpdate first{};
        const TerminalUpdate second{};
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, 80, 12}, first},
            {PixelRect{0, 12, 80, 12}, second},
        };

        STD_INSIST(!contract.update(panes, 2));
        // Refused, not partly drawn: the pane-less form never ran, so no
        // half of this frame reached a drawable.
        STD_INSIST(renderer.terminalUpdates == 0);
    }

    STD_TEST(AnEmptyFrameIsRefused) {
        SingleTerminalRenderer renderer;
        Renderer& contract = renderer;

        STD_INSIST(!contract.update((const PaneUpdate*)(nullptr), 0));
        STD_INSIST(renderer.terminalUpdates == 0);
    }

    // F9. The seam, in pixels, on the renderer that is the oracle.
    //
    // The geometry of the band is settled in session_ut.cpp; what is not
    // settled anywhere else is that the band reaches the target at all.
    // setSeams() is a new door in the Renderer interface, and until this
    // test nothing on any path called it - the painting could have been
    // absent entirely and every other test would still be green.
    //
    // Read at both ends of the clamp on purpose: a band as wide as the
    // air must not put one pixel over either grid, which is the
    // assertion the clamp exists for.
    STD_TEST(TheSeamBandIsPaintedIntoTheAirAndNotOverEitherGrid) {
        constexpr u16 border = 3;
        constexpr u16 columns = 4;
        const Color ink{200, 30, 90};
        const Color background{0, 0, 40};
        const Color cellInk{250, 250, 250};
        ScreenFixture fx(24, 2, border);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = cellInk;
        colors->defaultBackground = background;

        Screen* screens[2];
        Vector<TerminalRow> paneRows[2];
        for (unsigned index = 0; index < 2; ++index) {
            screens[index] = Screen::createPrimary(fx.composer->extras, *fx.pool, columns, 2, colors, 8);
            const TerminalCell attrs = coloredCell(cellInk, Color{9, 9, 9});
            for (u16 row = 0; row < 2; ++row) {
                for (u16 column = 0; column < columns; ++column) {
                    writeTextTo(*screens[index], row, column, "W", attrs);
                }
            }
        }

        ReferenceFixture reference(*fx.composer);
        const u16 paneWidth = (u16)(fx.composer->geometry.pixelWidth / 2);
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, paneWidth, fx.composer->geometry.pixelHeight}, captureFrom(*fx.composer, *screens[0], *colors, paneRows[0])},
            {PixelRect{paneWidth, 0, (u16)(fx.composer->geometry.pixelWidth - paneWidth), fx.composer->geometry.pixelHeight}, captureFrom(*fx.composer, *screens[1], *colors, paneRows[1])},
        };

        // The air between the two grids, and the widest band that fits
        // in it - which is what SessionSet's clamp hands over when a
        // wider one is asked for.
        const u16 leftGridEnd = (u16)(paneWidth - border);
        const u16 air = 2 * border;
        const PixelRect band{leftGridEnd, 0, air, fx.composer->geometry.pixelHeight};
        reference.renderer->setSeams(&band, 1, ink);
        STD_INSIST(reference.renderer->update(panes, 2));
        const ReferenceImage image = reference.renderer->image();

        const u16 sampleRow = (u16)(fx.composer->geometry.pixelHeight / 2);
        // Every pixel of the band is the seam's colour, edge to edge.
        for (u16 x = band.x; x < band.x + band.width; ++x) {
            STD_INSIST(cellPixel(image, x, sampleRow) == ink);
        }
        // And neither grid lost a pixel to it. This is the half that
        // fails if a band is ever allowed past the air.
        STD_INSIST(!(cellPixel(image, (u16)(band.x - 1), sampleRow) == ink));
        STD_INSIST(!(cellPixel(image, (u16)(band.x + band.width), sampleRow) == ink));

        // A narrower band leaves the panes' own background showing on
        // both sides of it, rather than filling the air regardless.
        const PixelRect thin{(u16)(leftGridEnd + 2), 0, 2, fx.composer->geometry.pixelHeight};
        reference.renderer->setSeams(&thin, 1, ink);
        STD_INSIST(reference.renderer->update(panes, 2));
        const ReferenceImage thinImage = reference.renderer->image();
        STD_INSIST(cellPixel(thinImage, thin.x, sampleRow) == ink);
        STD_INSIST(cellPixel(thinImage, (u16)(thin.x + 1), sampleRow) == ink);
        STD_INSIST(cellPixel(thinImage, (u16)(thin.x - 1), sampleRow) == background);
        STD_INSIST(cellPixel(thinImage, (u16)(thin.x + thin.width), sampleRow) == background);

        // And no seams at all leaves the air as it was: the state the
        // window is in before anyone asks for a divider.
        reference.renderer->setSeams(nullptr, 0, ink);
        STD_INSIST(reference.renderer->update(panes, 2));
        const ReferenceImage bare = reference.renderer->image();
        for (u16 x = leftGridEnd; x < leftGridEnd + air; ++x) {
            STD_INSIST(cellPixel(bare, x, sampleRow) == background);
        }
    }

    // T10. Translucency, on the renderer that compiles on every
    // platform - the Metal suite below says the same thing about the
    // real shader, but only where Metal exists, and the thirteenth blind
    // instrument of this plan was a test that lived behind exactly that
    // guard.
    //
    // Two renders of one frame, at 100 and at 50, is the positive
    // control: a build that always halved would pass the second half
    // alone, and a build that never did would pass the first.
    STD_TEST(ATranslucentBackgroundIsPremultipliedAndTheSolidMarksStaySolid) {
        constexpr u16 border = 4;
        ScreenFixture fx(3, 1, border);
        const Color cellBackground{200, 100, 40};
        const Color paneBackground{80, 160, 240};
        const Color selectionBackground{0, 200, 0};
        const Color seamInk{255, 0, 255};
        fx.colors.defaultBackground = paneBackground;
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct({255, 255, 255}));
        attrs.setBackground(CellColor::direct(cellBackground));
        // Spaces: a cell with no ink is one colour all over, so a pixel
        // inside it is the cell's background and nothing else.
        fx.writeText(0, 0, "   ", attrs);

        TerminalUpdate update = fx.capture();
        // The last column is selected, and by colour rather than by
        // swap, so the expected value is a number this test names.
        update.snappedSelection = Rect(2, 0, 3, 0);
        update.snappedSelection.rectangular = true;
        update.selectionColorMask = 2;
        update.selectionBackground = selectionBackground;

        const Insets insets = fx.composer->contentInsets();
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 sampleY = (u16)(insets.top + fx.composer->geometry.cellPixelHeight / 2);
        const u16 firstCellX = (u16)(insets.left + glyphWidth / 2);
        const u16 selectedCellX = (u16)(insets.left + 2 * glyphWidth + glyphWidth / 2);
        // The seam sits in the air the border leaves, clear of every
        // cell, so what it proves is about the seam and not about a cell.
        const PixelRect seam{0, 0, 2, fx.composer->geometry.pixelHeight};
        STD_INSIST(border > seam.width);

        Color opaqueCell{};
        Color opaquePadding{};
        {
            fx.options.backgroundOpacity = 100;
            ReferenceFixture renderer(*fx.composer);
            renderer.renderer->setSeams(&seam, 1, seamInk);
            const ReferenceImage image = renderer->render(update);
            STD_INSIST(image.pixels != nullptr);
            opaqueCell = cellPixel(image, firstCellX, sampleY);
            opaquePadding = cellPixel(image, (u16)(insets.left - 1), sampleY);
            // The default draws exactly what it drew before this option
            // existed.
            STD_INSIST(opaqueCell == cellBackground);
            STD_INSIST(opaquePadding == paneBackground);
            STD_INSIST(cellPixel(image, selectedCellX, sampleY) == selectionBackground);
            STD_INSIST(cellPixel(image, 1, sampleY) == seamInk);
        }

        {
            fx.options.backgroundOpacity = 50;
            ReferenceFixture renderer(*fx.composer);
            renderer.renderer->setSeams(&seam, 1, seamInk);
            const ReferenceImage image = renderer->render(update);
            STD_INSIST(image.pixels != nullptr);

            // Multiplied down, not merely dimmed by some other factor:
            // these are the bytes premultiplication produces at alpha
            // 128, written out.
            STD_INSIST((cellPixel(image, firstCellX, sampleY) == Color{100, 50, 20}));
            STD_INSIST((cellPixel(image, (u16)(insets.left - 1), sampleY) == Color{40, 80, 120}));
            // And they are not what they were, which is what makes the
            // pair of renders a control rather than two restatements.
            STD_INSIST(!(cellPixel(image, firstCellX, sampleY) == opaqueCell));
            STD_INSIST(!(cellPixel(image, (u16)(insets.left - 1), sampleY) == opaquePadding));

            // The marks that stay solid. A selection you can see the
            // desktop through stops marking anything, and a pane divider
            // that fades stops dividing.
            STD_INSIST(cellPixel(image, selectedCellX, sampleY) == selectionBackground);
            STD_INSIST(cellPixel(image, 1, sampleY) == seamInk);
        }
    }

    // R10-test, the hole this closes. T10 declared thirteen marks solid
    // and tested three of them: the glyph pass, a selection, and the
    // seam on the reference renderer only. Underlines, the strike, the
    // overline, the wrap mark and every cursor shape were claimed and
    // never read, and mutations proved it - the set stayed green at full
    // strength with the block cursor taken out of the solid list.
    //
    // Colours are chosen so that no two of them, and no halving of any
    // of them, can be confused: each ink is a single saturated channel,
    // and the background is the only colour with more than one.
    STD_TEST(EveryMarkThatIsNotBackgroundStaysSolid) {
        constexpr u16 border = 2;
        const Color cellBackground{200, 100, 40};
        const Color premultiplied{100, 50, 20};
        const Color ink{255, 0, 0};
        const Color underlineInk{0, 255, 0};
        // All five underline styles: a straight line, the double, the
        // wavy one that also paints a second row, the dotted and the
        // dashed. Each takes a different branch to the same putPixel.
        const u8 styles[] = {1, 2, 3, 4, 5};
        for (u8 style : styles) {
            ScreenFixture fx(2, 1, border);
            fx.options.showWraps = true;
            fx.options.backgroundOpacity = 50;
            fx.colors.defaultBackground = cellBackground;
            TerminalCell attrs{};
            attrs.setForeground(CellColor::direct(ink));
            attrs.setBackground(CellColor::direct(cellBackground));
            // Inline, not through the extra store: a cell without an
            // extra carries its underline colour in its own payload, and
            // materialize() takes that branch when it differs from the
            // foreground - which is exactly why the two inks below are
            // different colours.
            attrs.setInlineUnderlineColor(CellColor::direct(underlineInk));
            attrs.underline_style = style;
            attrs.strike = true;
            attrs.overline = true;
            attrs.wrap = true;
            fx.writeText(0, 0, " ", attrs);
            ReferenceFixture renderer(*fx.composer);

            const ReferenceImage image = renderer->render(fx.capture());
            STD_INSIST(image.pixels != nullptr);

            const Insets insets = fx.composer->contentInsets();
            Vector<Color> colors;
            distinctColors(image, insets.left, insets.top, fx.composer->geometry.cellPixelWidth, fx.composer->geometry.cellPixelHeight, colors);

            // The background went down by half, and every mark did not.
            STD_INSIST(holds(colors, premultiplied));
            STD_INSIST(holds(colors, ink));
            STD_INSIST(holds(colors, underlineInk));
            // And nothing else is in the cell at all - which is what
            // catches a mark this test never had to locate. A halved
            // ink would be a fourth colour here.
            STD_INSIST(colors.length() == 3);
            // The premise, so the test cannot pass by the background
            // having been solid all along.
            STD_INSIST(!holds(colors, cellBackground));
        }
    }

    // The four cursor shapes, each drawn by its own loop in both
    // renderers. filled_block is the one that is not merely an ink:
    // it replaces the cell's background, and taking it out of the solid
    // list makes that background follow the option - which is mutation
    // MH, and it survived the whole set.
    STD_TEST(EveryCursorShapeStaysSolid) {
        constexpr u16 border = 2;
        const Color cellBackground{200, 100, 40};
        const Color premultiplied{100, 50, 20};
        const Color ink{255, 0, 0};
        const Color cursorInk{0, 0, 255};
        const TerminalCursor::Style shapes[] = {
            TerminalCursor::Style::filled_block,
            TerminalCursor::Style::hollow_block,
            TerminalCursor::Style::underline,
            TerminalCursor::Style::bar,
        };
        for (TerminalCursor::Style shape : shapes) {
            ScreenFixture fx(2, 1, border);
            fx.options.backgroundOpacity = 50;
            fx.colors.defaultBackground = cellBackground;
            TerminalCell attrs{};
            attrs.setForeground(CellColor::direct(ink));
            attrs.setBackground(CellColor::direct(cellBackground));
            fx.writeText(0, 0, " ", attrs);
            ReferenceFixture renderer(*fx.composer);

            TerminalUpdate update = fx.capture();
            update.cursor.style = shape;
            update.cursor.color = cursorInk;
            update.cursor.posX = 0;
            update.cursor.posY = 0;
            const ReferenceImage image = renderer->render(update);
            STD_INSIST(image.pixels != nullptr);

            const Insets insets = fx.composer->contentInsets();
            Vector<Color> colors;
            distinctColors(image, insets.left, insets.top, fx.composer->geometry.cellPixelWidth, fx.composer->geometry.cellPixelHeight, colors);

            STD_INSIST(holds(colors, cursorInk));
            if (shape == TerminalCursor::Style::filled_block) {
                // The cursor became the background, and it is solid: the
                // cell is one colour, the cursor's own.
                STD_INSIST(colors.length() == 1);
                STD_INSIST(!holds(colors, premultiplied));
                // Halved, it would be this - named so the assertion
                // above cannot be read as being about something else.
                STD_INSIST((!holds(colors, Color{0, 0, 128})));
            } else {
                // The cursor is drawn over a background that did fade.
                STD_INSIST(holds(colors, premultiplied));
                STD_INSIST(colors.length() == 2);
            }
        }
    }

    // T10, R10-test. The other half of the option's claim, and the half
    // that was guarded by nothing. "Only the background goes
    // translucent" names a list - glyphs, the four underline styles,
    // the strike, the overline, the wrap mark, all four cursor shapes,
    // the selection and the pane divider - and until this the list was
    // checked for the selection alone, plus the divider on this
    // renderer. Three one-token mutations proved the gap: storeSolidPixel()
    // made to follow the alpha, the filled block cursor dropped out of
    // solidCell, and the seam handed backgroundOpacity() instead of 100
    // each left the whole suite green at 942.
    //
    // The assertion is the property itself and not an arithmetic
    // restatement of it: one frame rendered at 100 and at 50, and every
    // mark pixel obliged to hold the same bytes in both. That needs no
    // expected colour, so it cannot drift out of step with the blending
    // the way a written-out number can, and it says nothing about
    // rounding - which is what leaves it true of the shader too.
    //
    // The padding sample below is the control that stops this passing by
    // the option being ignored: the background *must* move between the
    // two renders, or "the marks did not move" is a statement about a
    // build where nothing moved at all.
    STD_TEST(EverySolidMarkKeepsItsBytesWhateverTheBackgroundOpacity) {
        constexpr u16 border = 4;
        ScreenFixture fx(6, 1, border);
        const Color paneBackground{80, 160, 240};
        const Color ink{255, 255, 255};
        const Color cursorColor{255, 0, 0};
        const Color seamInk{255, 0, 255};
        // Distinct from both backgrounds: the shader inverts a cursor
        // whose colour equals the cell's, and a test riding that branch
        // would be asserting about a colour it did not choose.
        STD_INSIST(!(cursorColor == paneBackground));
        fx.colors.defaultBackground = paneBackground;
        fx.colors.defaultForeground = ink;
        // The wrap mark is drawn only when the option asks for it.
        fx.options.showWraps = true;

        // Spaces throughout, and every cell's background is the pane's:
        // a cell with no ink is one colour all over, so any pixel that
        // is *not* that colour is a mark this test is looking for.
        TerminalCell plain{};
        plain.setForeground(CellColor::direct(ink));
        plain.setBackground(CellColor::direct(paneBackground));

        TerminalCell underlined = plain;
        underlined.underline_style = 1;
        TerminalCell struck = plain;
        struck.strike = 1;
        TerminalCell overlined = plain;
        overlined.overline = 1;
        TerminalCell wrapped = plain;
        wrapped.wrap = 1;

        fx.writeText(0, 0, " ", underlined);
        fx.writeText(0, 1, " ", struck);
        fx.writeText(0, 2, " ", overlined);
        fx.writeText(0, 3, " ", wrapped);
        fx.writeText(0, 4, " ", plain);
        fx.writeText(0, 5, " ", plain);

        TerminalUpdate update = fx.capture();
        update.cursor.color = cursorColor;
        update.cursor.posX = 5;
        update.cursor.posY = 0;

        const Insets insets = fx.composer->contentInsets();
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        // In the air the border leaves, clear of every cell, so what it
        // holds is the seam and not a cell that happened to reach it.
        const PixelRect seam{0, 0, 2, fx.composer->geometry.pixelHeight};
        STD_INSIST(border > seam.width);
        const u16 seamY = (u16)(fx.composer->geometry.pixelHeight / 2);
        // Right of the seam and left of the first cell: pure pane fill,
        // which is the one thing here that has to change.
        const u16 padX = (u16)(insets.left - 1);
        STD_INSIST(padX >= seam.width);

        const TerminalCursor::Style shapes[] = {
            TerminalCursor::Style::filled_block,
            TerminalCursor::Style::hollow_block,
            TerminalCursor::Style::underline,
            TerminalCursor::Style::bar,
        };
        for (TerminalCursor::Style shape : shapes) {
            update.cursor.style = shape;

            // Two live fixtures, not two renders through one: the image
            // points into the fixture's own buffer, so a second render
            // on the same one would compare a picture with itself.
            fx.options.backgroundOpacity = 100;
            ReferenceFixture opaqueRenderer(*fx.composer);
            opaqueRenderer.renderer->setSeams(&seam, 1, seamInk);
            const ReferenceImage full = opaqueRenderer->render(update);
            STD_INSIST(full.pixels != nullptr);

            fx.options.backgroundOpacity = 50;
            ReferenceFixture halfRenderer(*fx.composer);
            halfRenderer.renderer->setSeams(&seam, 1, seamInk);
            const ReferenceImage half = halfRenderer->render(update);
            STD_INSIST(half.pixels != nullptr);

            // The control, first: without it every assertion below is
            // true of a build that ignores the option entirely.
            STD_INSIST(!(cellPixel(half, padX, (u16)(insets.top + glyphHeight / 2)) == cellPixel(full, padX, (u16)(insets.top + glyphHeight / 2))));

            const auto opaqueAt = [&](u16 x, u16 y) { return cellPixel(full, x, y); };
            const auto halfAt = [&](u16 x, u16 y) { return cellPixel(half, x, y); };
            const auto markIn = [&](u16 column) {
                return compareMarks(opaqueAt, halfAt, (u16)(insets.left + column * glyphWidth), insets.top, glyphWidth, glyphHeight, paneBackground);
            };

            const MarkAgreement underline = markIn(0);
            STD_INSIST(underline.marks > 0 && underline.disagreements == 0);
            const MarkAgreement strike = markIn(1);
            STD_INSIST(strike.marks > 0 && strike.disagreements == 0);
            const MarkAgreement overline = markIn(2);
            STD_INSIST(overline.marks > 0 && overline.disagreements == 0);
            const MarkAgreement wrapMark = markIn(3);
            STD_INSIST(wrapMark.marks > 0 && wrapMark.disagreements == 0);
            const MarkAgreement cursor = markIn(5);
            STD_INSIST(cursor.marks > 0 && cursor.disagreements == 0);

            // And the divider, which is solid for a reason of its own:
            // no storeSolidPixel() carries it, only the caller writing
            // 100 into the transparency field by hand.
            STD_INSIST(cellPixel(full, 1, seamY) == seamInk);
            STD_INSIST(cellPixel(half, 1, seamY) == seamInk);
        }
    }

    // R9-qa. The padding defect F9 found while looking for somewhere to
    // put the seam - a pane's own air wearing its neighbour's background
    // - is pinned in MetalPanes, and that suite only exists where Metal
    // does (HAVE_METAL_RENDERER). The same contract is render.h's, and
    // the reference renderer answers for it on every platform, so pin it
    // here too or CI never sees a regression in it.
    //
    // No seams are set: this is the contract that has to hold with the
    // divider switched off entirely, which is what keeps it a test of the
    // defect rather than of the work that uncovered it.
    STD_TEST(EachPanesPaddingCarriesItsOwnBackgroundWithNoSeamAtAll) {
        constexpr u16 border = 3;
        constexpr u16 columns = 4;
        const Color leftBackground{0, 0, 128};
        const Color rightBackground{128, 64, 0};
        // The premise: two panes that agreed about their background could
        // not tell the right answer from the wrong one at all.
        STD_INSIST(!(leftBackground == rightBackground));
        ScreenFixture fx(24, 2, border);

        auto* const leftColors = fx.pool->make<TerminalColors>();
        leftColors->defaultForeground = {250, 250, 250};
        leftColors->defaultBackground = leftBackground;
        auto* const rightColors = fx.pool->make<TerminalColors>();
        rightColors->defaultForeground = {250, 250, 250};
        rightColors->defaultBackground = rightBackground;
        TerminalColors* const paneColors[2] = {leftColors, rightColors};

        Screen* screens[2];
        Vector<TerminalRow> paneRows[2];
        for (unsigned index = 0; index < 2; ++index) {
            screens[index] = Screen::createPrimary(fx.composer->extras, *fx.pool, columns, 2, paneColors[index], 8);
        }

        ReferenceFixture reference(*fx.composer);
        const u16 paneWidth = (u16)(fx.composer->geometry.pixelWidth / 2);
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, paneWidth, fx.composer->geometry.pixelHeight}, captureFrom(*fx.composer, *screens[0], *leftColors, paneRows[0])},
            {PixelRect{paneWidth, 0, (u16)(fx.composer->geometry.pixelWidth - paneWidth), fx.composer->geometry.pixelHeight}, captureFrom(*fx.composer, *screens[1], *rightColors, paneRows[1])},
        };
        reference.renderer->setSeams(nullptr, 0, Color{0, 0, 0});
        STD_INSIST(reference.renderer->update(panes, 2));
        const ReferenceImage image = reference.renderer->image();

        // One pixel in from each pane's own left edge: still air, because
        // the grid starts a whole border further in.
        const u16 sampleRow = (u16)(fx.composer->geometry.pixelHeight / 2);
        STD_INSIST(border >= 2);
        // The left pane's padding was always right - it is the one a
        // per-frame clear happens to agree with. The positive control:
        // without it a build painting nothing would pass the next line.
        STD_INSIST(cellPixel(image, 1, sampleRow) == leftBackground);
        // And the half that was wrong.
        STD_INSIST(cellPixel(image, (u16)(paneWidth + 1), sampleRow) == rightBackground);
        STD_INSIST(!(cellPixel(image, (u16)(paneWidth + 1), sampleRow) == leftBackground));
    }
}

#if defined(HAVE_METAL_RENDERER)

// A9, A6-4 on the GPU backend. Until this, the two-pane frame was
// checked by nothing machine-readable anywhere: the reference renderer
// only ever checked itself, tst/test_gpu_parity.py sends the Metal
// backend one pane, and the Vulkan backend refuses a wider frame
// outright (R6-arch, A6-6). The backend draws into a texture without a
// layer for exactly this reason - it is what the harness's shadow
// renderer already does (test_mode.cpp:2133) - and captureOutput()
// reads it back, so the assertions below are about pixels Metal placed.
//
// Asserted against the pane's own arithmetic rather than against the
// reference renderer's image: the two backends clear differently on a
// multi-pane frame (Metal clears the drawable once with the first
// pane's background, the reference clears per rectangle), which R6-arch
// records as an expected divergence that must not be resolved by
// bending the reference. Everything inside a pane's cells is a fair
// reading, and that is what these read.
namespace {
    struct MetalFixture {
        explicit MetalFixture(Composer& composer) {
            renderer = createMetalRenderer(composer, *pool, {plt::RenderBackend::Headless, nullptr, nullptr});
        }

        Color pixel(u16 x, u16 y) const {
            const auto* const bytes = (const u8*)(rgb.data());
            const size_t index = 3 * ((size_t)(y)*width + x);
            return {bytes[index], bytes[index + 1], bytes[index + 2]};
        }

        bool capture() {
            return renderer != nullptr && renderer->captureOutput(rgb, width, height);
        }

        ObjPool::Ref pool = ObjPool::fromMemory();
        Renderer* renderer = nullptr;
        Buffer rgb;
        u32 width = 0;
        u32 height = 0;
    };

    // Ink inside one cell box of one pane, read back off the texture
    // Metal wrote. A cell whose strip was taken away holds nothing but
    // its own background; a cell that kept one holds glyph pixels that
    // differ from it. That binary is the whole instrument that
    // AnOverlayLandsOnTheStripsOfItsOwnPane below reads, and it is the
    // same one PreeditOverlayCoversUnderlyingStrips uses on the
    // reference renderer.
    static bool paneCellHasInk(const MetalFixture& metal, const Composer& composer, u16 originX, u16 originY, u16 row, u16 column, Color background) {
        const u16 glyphWidth = composer.geometry.cellPixelWidth;
        const u16 glyphHeight = composer.geometry.cellPixelHeight;
        for (u16 y = 0; y < glyphHeight; ++y) {
            for (u16 x = 0; x < glyphWidth; ++x) {
                const u16 pixelX = (u16)(originX + column * glyphWidth + x);
                const u16 pixelY = (u16)(originY + row * glyphHeight + y);
                if (!(metal.pixel(pixelX, pixelY) == background)) {
                    return true;
                }
            }
        }
        return false;
    }
}

STD_TEST_SUITE(MetalPanes) {
    // Three panes, three grids, one frame. Two would not do: with two
    // panes the second one's cell offset is the first one's size under
    // any arithmetic that sums the panes before it, so an offset walk
    // that took every pane for the first one is indistinguishable. The
    // third pane is where "sum of the panes before me" and "my index
    // times the first pane's size" part company, and it is also the
    // second pane that has to bias its strips off a base of its own.
    //
    // The cells carry letters rather than blanks so the strips are read
    // as well as the cells: a pane that biased its neighbour's strips
    // moves ink, and ink is only visible if there is some.
    // T10, on the real shader. render.comp is the product here and the
    // reference renderer only mirrors it, so the premultiplication that
    // matters is the one Metal performs - and this reads it back off a
    // texture Metal wrote.
    //
    // Alpha itself is not observable through captureOutput(), which
    // hands back RGB. That is enough for the hazard being pinned:
    // premultiplication lives in the *colour* channels, and a shader
    // that attached alpha without multiplying the colour down leaves
    // those channels at the full background value. The two answers are
    // 100 apart on the red channel below.
    STD_TEST(ATranslucentBackgroundReachesTheTextureMultipliedDown) {
        constexpr u16 border = 3;
        const Color paneBackground{200, 100, 40};
        // A different colour from the pane's, so the cell sample below
        // cannot be satisfied by the fill pass having reached it.
        const Color cellBackground{40, 200, 100};
        STD_INSIST(!(paneBackground == cellBackground));
        ScreenFixture fx(6, 2, border);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = {255, 255, 255};
        colors->defaultBackground = paneBackground;
        Screen* const screen = Screen::createPrimary(fx.composer->extras, *fx.pool, 6, 2, colors, 8);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct({255, 255, 255}));
        attrs.setBackground(CellColor::direct(cellBackground));
        for (u16 row = 0; row < 2; ++row) {
            for (u16 column = 0; column < 6; ++column) {
                writeTextTo(*screen, row, column, " ", attrs);
            }
        }

        // Inside the pane's own padding: filled by the fill pass, no
        // cell reaches it, so what it holds is the pane background and
        // nothing blended into it.
        const u16 sampleX = 1;
        const u16 sampleY = (u16)(fx.composer->geometry.pixelHeight / 2);
        STD_INSIST(border > sampleX);
        // And a second sample inside cell 0,0, which the *glyph* pass
        // writes. The two passes multiply the background down in
        // different expressions, and a test that reads only the fill
        // leaves the other one unpinned: with only the padding sampled,
        // a shader blending against a background it had not multiplied
        // survived this suite. A space carries the cell's background and
        // no ink, so coverage there is zero and the pixel is the
        // background term alone - which is exactly the term that differs.
        const Insets insets = fx.composer->contentInsets();
        const u16 cellX = (u16)(insets.left + fx.composer->geometry.cellPixelWidth / 2);
        const u16 cellY = (u16)(insets.top + fx.composer->geometry.cellPixelHeight / 2);
        // A third sample, in a selected cell. "Only the background goes
        // translucent" names a selection explicitly, and the shader
        // decides that in an expression of its own - the reference
        // renderer's copy of the rule being pinned says nothing about
        // this one. Column 4, selected by colour rather than by swap, so
        // the expected value is a number rather than a derivation.
        const Color selectionBackground{0, 200, 0};
        const u16 selectedX = (u16)(insets.left + 4 * fx.composer->geometry.cellPixelWidth + fx.composer->geometry.cellPixelWidth / 2);

        Color opaque{};
        Color opaqueCell{};
        Color opaqueSelection{};
        {
            fx.options.backgroundOpacity = 100;
            Vector<TerminalRow> rows;
            MetalFixture metal(*fx.composer);
            STD_INSIST(metal.renderer != nullptr);
            TerminalUpdate captured = captureFrom(*fx.composer, *screen, *colors, rows);
            captured.snappedSelection = Rect(4, 0, 5, 0);
            captured.snappedSelection.rectangular = true;
            captured.selectionColorMask = 2;
            captured.selectionBackground = selectionBackground;
            const PaneUpdate pane{
                PixelRect{0, 0, fx.composer->geometry.pixelWidth, fx.composer->geometry.pixelHeight},
                captured,
            };
            STD_INSIST(metal.renderer->update(&pane, 1));
            STD_INSIST(metal.capture());
            opaque = metal.pixel(sampleX, sampleY);
            opaqueCell = metal.pixel(cellX, cellY);
            opaqueSelection = metal.pixel(selectedX, cellY);
            // The control: the default is the picture this backend drew
            // before the option existed.
            STD_INSIST(opaque == paneBackground);
            STD_INSIST(opaqueCell == cellBackground);
            STD_INSIST(opaqueSelection == selectionBackground);
        }

        {
            fx.options.backgroundOpacity = 50;
            Vector<TerminalRow> rows;
            MetalFixture metal(*fx.composer);
            STD_INSIST(metal.renderer != nullptr);
            TerminalUpdate captured = captureFrom(*fx.composer, *screen, *colors, rows);
            captured.snappedSelection = Rect(4, 0, 5, 0);
            captured.snappedSelection.rectangular = true;
            captured.selectionColorMask = 2;
            captured.selectionBackground = selectionBackground;
            const PaneUpdate pane{
                PixelRect{0, 0, fx.composer->geometry.pixelWidth, fx.composer->geometry.pixelHeight},
                captured,
            };
            STD_INSIST(metal.renderer->update(&pane, 1));
            STD_INSIST(metal.capture());
            const Color half = metal.pixel(sampleX, sampleY);

            // Multiplied down. The shader works in floats, so it is
            // allowed to land a step either side of the reference
            // renderer's integer answer - but nowhere near the
            // un-multiplied value, which is what the second bound says.
            STD_INSIST(half.red >= 98 && half.red <= 102);
            STD_INSIST(half.green >= 48 && half.green <= 52);
            STD_INSIST(half.blue >= 18 && half.blue <= 22);
            STD_INSIST(!(half == opaque));

            // The glyph pass, on a cell of its own colour: {40, 200, 100}
            // multiplied down is {20, 100, 50}, and the un-multiplied
            // answer is the cell background itself.
            const Color halfCell = metal.pixel(cellX, cellY);
            STD_INSIST(halfCell.red >= 18 && halfCell.red <= 22);
            STD_INSIST(halfCell.green >= 98 && halfCell.green <= 102);
            STD_INSIST(halfCell.blue >= 48 && halfCell.blue <= 52);
            STD_INSIST(!(halfCell == opaqueCell));

            // And the selection, solid: a mark you can see the desktop
            // through stops marking anything. Multiplied down it would
            // be {0, 100, 0}, which is the value this assertion refuses.
            STD_INSIST(metal.pixel(selectedX, cellY) == selectionBackground);
            STD_INSIST(metal.pixel(selectedX, cellY) == opaqueSelection);
            // And the reference renderer's own answer for the same
            // colour, so the two implementations are pinned to each
            // other and not merely each to itself.
            STD_INSIST(premultiply(paneBackground, backgroundAlphaFromPercent(50)).red == 100);
        }
    }

    // R10-test, the shader half. Three of the thirteen marks T10 called
    // solid were pinned on the reference renderer alone, and one of them
    // - the seam - not on the shader at all: the translucency test above
    // renders one pane and never asked for a seam, so render_metal.mm's
    // band never ran under a translucent frame. Making the seam follow
    // the option survived the whole set.
    //
    // A one-pane frame with a seam set explicitly is enough: the band
    // loop runs over whatever setSeams() was given, independently of how
    // many panes there are, and the band lands in the border air where
    // no cell can reach it.
    STD_TEST(EveryMarkThatIsNotBackgroundStaysSolidOnTheShader) {
        constexpr u16 border = 3;
        const Color cellBackground{200, 100, 40};
        const Color ink{255, 0, 0};
        const Color seamInk{255, 0, 255};
        const Color cursorInk{0, 0, 255};
        ScreenFixture fx(4, 1, border);
        fx.options.backgroundOpacity = 50;
        fx.options.showWraps = true;
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = ink;
        colors->defaultBackground = cellBackground;
        Screen* const screen = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 1, colors, 8);
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct(ink));
        attrs.setBackground(CellColor::direct(cellBackground));
        attrs.underline_style = 1;
        attrs.strike = true;
        attrs.overline = true;
        attrs.wrap = true;
        for (u16 column = 0; column < 4; ++column) {
            writeTextTo(*screen, 0, column, " ", attrs);
        }

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        // In the left border air, clear of every cell.
        const PixelRect seam{0, 0, 2, fx.composer->geometry.pixelHeight};
        STD_INSIST(border > seam.width);
        metal.renderer->setSeams(&seam, 1, seamInk);
        Vector<TerminalRow> rows;
        TerminalUpdate captured = captureFrom(*fx.composer, *screen, *colors, rows);
        // The block cursor, which is not an ink but a background: taking
        // it out of the solid list is mutation MH, and only a sample
        // inside its cell can see that.
        captured.cursor.style = TerminalCursor::Style::filled_block;
        captured.cursor.color = cursorInk;
        captured.cursor.posX = 2;
        captured.cursor.posY = 0;
        const PaneUpdate pane{
            PixelRect{0, 0, fx.composer->geometry.pixelWidth, fx.composer->geometry.pixelHeight},
            captured,
        };
        STD_INSIST(metal.renderer->update(&pane, 1));
        STD_INSIST(metal.capture());

        const Insets insets = fx.composer->contentInsets();
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;

        // The seam, in the ink it was handed and not half of it.
        const u16 seamY = (u16)(fx.composer->geometry.pixelHeight / 2);
        STD_INSIST(metal.pixel(1, seamY) == seamInk);
        STD_INSIST((!(metal.pixel(1, seamY) == Color{128, 0, 128})));

        // The distinct colours of a decorated cell. The shader works in
        // floats, so the faded background is asserted by *absence* - it
        // is neither the full colour nor any of the inks - while the
        // inks themselves are exact, being written without a multiply.
        const auto colorsIn = [&metal, glyphWidth, glyphHeight](u16 x0, u16 y0, Vector<Color>& out) {
            out.clear();
            for (u16 y = y0; y < y0 + glyphHeight; ++y) {
                for (u16 x = x0; x < x0 + glyphWidth; ++x) {
                    const Color here = metal.pixel(x, y);
                    bool seen = false;
                    for (const Color& already : out) {
                        seen = seen || already == here;
                    }
                    if (!seen) {
                        out.pushBack(here);
                    }
                }
            }
        };

        Vector<Color> decorated;
        colorsIn(insets.left, insets.top, decorated);
        // Background and one ink - the underline defaults to the
        // foreground, and strike, overline and the wrap mark all use it.
        STD_INSIST(decorated.length() == 2);
        STD_INSIST(holds(decorated, ink));
        STD_INSIST(!holds(decorated, cellBackground));
        // Halved, the ink would be this, and it is what a decoration
        // that started following the option would leave here.
        STD_INSIST((!holds(decorated, Color{128, 0, 0})));

        Vector<Color> cursorCell;
        colorsIn((u16)(insets.left + 2 * glyphWidth), insets.top, cursorCell);
        // The block cursor replaced the background and stayed solid, so
        // its cell is one colour: the cursor's own. Its decorations are
        // drawn in what used to be the background.
        STD_INSIST(holds(cursorCell, cursorInk));
        STD_INSIST((!holds(cursorCell, Color{0, 0, 128})));
    }

    // T10, R10-test. The solid marks, on the shader that is the product.
    // The everywhere-compiled twin above says the same thing about the
    // reference renderer, and it has to: this one cannot run where Metal
    // does not exist. But it is the only instrument that can see
    // render.comp at all, and render.comp is where the rule lives -
    // three one-token mutations of it (storeSolidPixel() made to follow
    // the alpha; the filled block cursor dropped out of solidCell; the
    // seam handed backgroundOpacity() instead of 100) each left the
    // whole suite green before this test existed.
    //
    // One pane, not two, and the seam still arrives: render_metal.mm
    // walks setSeams() independently of how many panes the frame holds,
    // and the band lands in the border's air where no pane fill follows
    // it. A second pane would prove nothing more and would put the
    // divider's colour next to a neighbour that could account for it.
    STD_TEST(TheSolidMarksReachTheTextureUnfadedAtAnyOpacity) {
        constexpr u16 border = 4;
        ScreenFixture fx(6, 1, border);
        const Color paneBackground{80, 160, 240};
        const Color ink{255, 255, 255};
        const Color cursorColor{255, 0, 0};
        const Color seamInk{255, 0, 255};
        STD_INSIST(!(cursorColor == paneBackground));
        fx.colors.defaultBackground = paneBackground;
        fx.colors.defaultForeground = ink;
        fx.options.showWraps = true;

        TerminalCell plain{};
        plain.setForeground(CellColor::direct(ink));
        plain.setBackground(CellColor::direct(paneBackground));
        TerminalCell underlined = plain;
        underlined.underline_style = 1;
        TerminalCell struck = plain;
        struck.strike = 1;
        TerminalCell overlined = plain;
        overlined.overline = 1;
        TerminalCell wrapped = plain;
        wrapped.wrap = 1;

        fx.writeText(0, 0, " ", underlined);
        fx.writeText(0, 1, " ", struck);
        fx.writeText(0, 2, " ", overlined);
        fx.writeText(0, 3, " ", wrapped);
        fx.writeText(0, 4, " ", plain);
        fx.writeText(0, 5, " ", plain);

        TerminalUpdate captured = fx.capture();
        captured.cursor.color = cursorColor;
        captured.cursor.posX = 5;
        captured.cursor.posY = 0;

        const Insets insets = fx.composer->contentInsets();
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        const PixelRect seam{0, 0, 2, fx.composer->geometry.pixelHeight};
        STD_INSIST(border > seam.width);
        const u16 seamY = (u16)(fx.composer->geometry.pixelHeight / 2);
        const u16 padX = (u16)(insets.left - 1);
        STD_INSIST(padX >= seam.width);

        const TerminalCursor::Style shapes[] = {
            TerminalCursor::Style::filled_block,
            TerminalCursor::Style::hollow_block,
            TerminalCursor::Style::underline,
            TerminalCursor::Style::bar,
        };
        for (TerminalCursor::Style shape : shapes) {
            captured.cursor.style = shape;
            const PaneUpdate pane{
                PixelRect{0, 0, fx.composer->geometry.pixelWidth, fx.composer->geometry.pixelHeight},
                captured,
            };

            fx.options.backgroundOpacity = 100;
            MetalFixture opaqueMetal(*fx.composer);
            STD_INSIST(opaqueMetal.renderer != nullptr);
            opaqueMetal.renderer->setSeams(&seam, 1, seamInk);
            STD_INSIST(opaqueMetal.renderer->update(&pane, 1));
            STD_INSIST(opaqueMetal.capture());

            fx.options.backgroundOpacity = 50;
            MetalFixture halfMetal(*fx.composer);
            STD_INSIST(halfMetal.renderer != nullptr);
            halfMetal.renderer->setSeams(&seam, 1, seamInk);
            STD_INSIST(halfMetal.renderer->update(&pane, 1));
            STD_INSIST(halfMetal.capture());

            const auto opaqueAt = [&](u16 x, u16 y) { return opaqueMetal.pixel(x, y); };
            const auto halfAt = [&](u16 x, u16 y) { return halfMetal.pixel(x, y); };

            // The control: the background has to move, or every
            // agreement below is an agreement about a build in which
            // nothing moved.
            const u16 padY = (u16)(insets.top + glyphHeight / 2);
            STD_INSIST(!(halfAt(padX, padY) == opaqueAt(padX, padY)));

            const auto markIn = [&](u16 column) {
                return compareMarks(opaqueAt, halfAt, (u16)(insets.left + column * glyphWidth), insets.top, glyphWidth, glyphHeight, paneBackground);
            };
            const MarkAgreement underline = markIn(0);
            STD_INSIST(underline.marks > 0 && underline.disagreements == 0);
            const MarkAgreement strike = markIn(1);
            STD_INSIST(strike.marks > 0 && strike.disagreements == 0);
            const MarkAgreement overline = markIn(2);
            STD_INSIST(overline.marks > 0 && overline.disagreements == 0);
            const MarkAgreement wrapMark = markIn(3);
            STD_INSIST(wrapMark.marks > 0 && wrapMark.disagreements == 0);
            const MarkAgreement cursor = markIn(5);
            STD_INSIST(cursor.marks > 0 && cursor.disagreements == 0);

            // The divider, whose solidity no storeSolidPixel() carries:
            // it is the caller in this backend writing 100 into the
            // transparency field by hand, and that is a second copy of
            // the rule (render_vk.cpp holds the other).
            STD_INSIST(opaqueAt(1, seamY) == seamInk);
            STD_INSIST(halfAt(1, seamY) == seamInk);
        }
    }

    STD_TEST(DrawThreeGridsInOneFrame) {
        constexpr u16 border = 3;
        // R7-test. Without a chrome reserve this stand cannot tell
        // paneInsets() from contentInsets(): with nothing claimed off the
        // top they are the same four numbers, and the mutation that makes
        // Metal charge every pane for the window's chrome a second time
        // passed the whole suite. Deliberately *smaller than a glyph*, so
        // that it also exercises the edge assertions below - a reserve
        // taller than a cell would be caught by a sample anywhere inside
        // the cell box, and the defect this stand was blind to is exactly
        // the sub-cell one. The premise is asserted, not assumed.
        constexpr u16 topReserve = 5;
        constexpr u16 paneColumns[3] = {6, 3, 4};
        const Color paneInk[3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        ScreenFixture fx(24, 2, border, topReserve);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = {1, 2, 3};
        colors->defaultBackground = {0, 0, 128};
        Screen* screens[3];
        Vector<TerminalRow> paneRows[3];
        for (unsigned index = 0; index < 3; ++index) {
            screens[index] = Screen::createPrimary(fx.composer->extras, *fx.pool, paneColumns[index], 2, colors, 8);
            TerminalCell attrs{};
            attrs.setForeground(CellColor::direct(paneInk[index]));
            attrs.setBackground(CellColor::direct({8, 8, 8}));
            for (u16 row = 0; row < 2; ++row) {
                for (u16 column = 0; column < paneColumns[index]; ++column) {
                    writeTextTo(*screens[index], row, column, "W", attrs);
                }
            }
        }

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        const u16 paneWidth = (u16)(fx.composer->geometry.pixelWidth / 3);
        // A10: the chrome comes off the window before the rectangles are
        // cut, so the layout hands the backend rectangles that already
        // begin below it - which is what makes adding it again here a
        // double charge rather than an arrangement of the same pixels.
        const u16 chromeTop = fx.composer->chromeInsets().top;
        const u16 paneHeight = (u16)(fx.composer->geometry.pixelHeight - chromeTop);
        const PaneUpdate panes[3] = {
            {PixelRect{0, chromeTop, paneWidth, paneHeight}, captureFrom(*fx.composer, *screens[0], *colors, paneRows[0])},
            {PixelRect{paneWidth, chromeTop, paneWidth, paneHeight}, captureFrom(*fx.composer, *screens[1], *colors, paneRows[1])},
            {PixelRect{(u16)(2 * paneWidth), chromeTop, (u16)(fx.composer->geometry.pixelWidth - 2 * paneWidth), paneHeight}, captureFrom(*fx.composer, *screens[2], *colors, paneRows[2])},
        };
        // The premise: every pane's rectangle holds more columns than
        // its grid has, so a column past its grid is inside its clip.
        for (unsigned index = 0; index < 3; ++index) {
            STD_INSIST(paneWidth > border + (paneColumns[index] + 1) * glyphWidth);
        }
        // And the premises of the edge assertions: the reserve is real,
        // and it is smaller than a cell, so only an assertion that reads
        // the edge itself can see it move.
        STD_INSIST(chromeTop != 0);
        STD_INSIST(chromeTop < glyphHeight);

        STD_INSIST(metal.renderer->update(panes, 3));
        STD_INSIST(metal.capture());
        STD_INSIST(metal.width == fx.composer->geometry.pixelWidth);

        // The pane's own background is what the drawable was cleared
        // with, so the first pixel that is anything else is the first
        // pixel of the grid - the edge itself, read rather than sampled.
        // A sample taken inside a cell box cannot tell a grid placed a
        // few pixels too low from one placed right; these two scans can,
        // and that is the whole reason they are here.
        const auto firstRowOfGrid = [&metal, colors](u16 x, u16 limit) -> u16 {
            for (u16 y = 0; y < limit; ++y) {
                if (!(metal.pixel(x, y) == colors->defaultBackground)) {
                    return y;
                }
            }
            return limit;
        };
        const auto firstColumnOfGrid = [&metal, colors](u16 y, u16 from, u16 limit) -> u16 {
            for (u16 x = from; x < limit; ++x) {
                if (!(metal.pixel(x, y) == colors->defaultBackground)) {
                    return x;
                }
            }
            return limit;
        };

        for (unsigned index = 0; index < 3; ++index) {
            const u16 origin = (u16)(index * paneWidth + border);
            const u16 gridTop = (u16)(chromeTop + border);
            // Exactly here, to the pixel: the pane's rectangle plus the
            // border, and the chrome reserve counted once rather than
            // twice.
            STD_INSIST(firstRowOfGrid((u16)(origin + glyphWidth / 2), (u16)(metal.height)) == gridTop);
            STD_INSIST(firstColumnOfGrid((u16)(gridTop + glyphHeight / 2), (u16)(index * paneWidth), (u16)(metal.width)) == origin);
            for (u16 row = 0; row < 2; ++row) {
                const u16 top = (u16)(gridTop + row * glyphHeight);
                // Every cell of this pane's grid carries this pane's own
                // ink over this pane's own cell background.
                for (u16 column = 0; column < paneColumns[index]; ++column) {
                    bool inked = false;
                    bool background = false;
                    for (u16 y = 0; y < glyphHeight; ++y) {
                        for (u16 x = 0; x < glyphWidth; ++x) {
                            const Color pixel = metal.pixel((u16)(origin + column * glyphWidth + x), (u16)(top + y));
                            inked = inked || (pixel == paneInk[index]);
                            background = background || (pixel == Color{8, 8, 8});
                        }
                    }
                    STD_INSIST(inked);
                    STD_INSIST(background);
                }
                // And the column past its grid is padding: the pane's
                // default background, whole, with nothing drawn in it.
                for (u16 y = 0; y < glyphHeight; ++y) {
                    for (u16 x = 0; x < glyphWidth; ++x) {
                        STD_INSIST((metal.pixel((u16)(origin + paneColumns[index] * glyphWidth + x), (u16)(top + y)) == Color{0, 0, 128}));
                    }
                }
            }
        }
    }

    // R5-test. A3 after the pane arena ranges went: one shaper per
    // window means one arena, and shaping a later pane of a frame can
    // collect the arena that an earlier pane's strips already point
    // into. That is why assignFrameStrips() carries the generation loop
    // around the whole frame rather than around one pane - and T4.1
    // reported the path as unreachable, so the widening rested on
    // reading alone.
    //
    // It is reachable from here, and only from here. The arena budget is
    // three viewports of glyph coverage (arenaBudget() in
    // span_shaper.cpp), so a window this size collects after a couple of
    // dozen rows of text nothing has shaped before. Feeding two of the
    // three panes that text while the first one holds still puts the
    // collection in the middle of a frame, with the held pane's strips
    // assigned before it and nothing of the held pane's own driving it.
    //
    // Why not tst/test_gpu_parity.py, where the same three panes are
    // easy to build: the damage is one frame wide. The collection
    // empties the row cache, so the very next frame reshapes the held
    // pane and hands it good offsets - and every harness command drives
    // several frames before the texture can be read back, so the
    // corrupt one is always overwritten before anything sees it. Here
    // update() is one frame and capture() reads that frame.
    //
    // The held pane's own rectangle from the first frame is the oracle.
    // Its cells never change and its geometry never moves, so a correct
    // backend redraws it identically forever; a backend that let a
    // neighbour's collection move its strips draws it out of whatever
    // bytes landed at those offsets instead. Comparing it against the
    // reference renderer would not do: the two backends clear a
    // multi-pane frame differently on purpose (see the note above this
    // suite), and this compares Metal against Metal.
    STD_TEST(APaneKeepsItsInkWhenALaterPaneCollectsTheArena) {
        constexpr u16 paneColumns = 6;
        constexpr u16 paneRowCount = 2;
        constexpr unsigned paneCount = 3;
        ScreenFixture fx(paneCount * paneColumns, paneRowCount);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = {1, 2, 3};
        colors->defaultBackground = {0, 0, 128};
        Screen* screens[paneCount];
        Vector<TerminalRow> paneRows[paneCount];
        const Color paneInk[paneCount] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        for (unsigned index = 0; index < paneCount; ++index) {
            screens[index] = Screen::createPrimary(fx.composer->extras, *fx.pool, paneColumns, paneRowCount, colors, 8);
        }
        // The pane that holds still, in letters with ink to spare: a
        // strip read from the wrong place is only visible where there
        // was something to read.
        const TerminalCell held = coloredCell(paneInk[0], {8, 8, 8});
        writeTextTo(*screens[0], 0, 0, "MWQBHX", held);
        writeTextTo(*screens[0], 1, 0, "XHBQWM", held);

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        const u16 paneWidth = (u16)(fx.composer->geometry.pixelWidth / paneCount);
        const u16 paneHeight = fx.composer->geometry.pixelHeight;

        // A counter written out in base 36, so every (frame, pane, row)
        // is six characters nothing has shaped before. A generator that
        // varies one column runs out of strings before the arena runs
        // out of budget, and then this stand stops holding its subject
        // without saying so - which the collection assertion at the end
        // is there to catch.
        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        const auto drawFrame = [&](unsigned step) {
            for (unsigned index = 1; index < paneCount; ++index) {
                const TerminalCell attrs = coloredCell(paneInk[index], {8, 8, 8});
                for (u16 row = 0; row < paneRowCount; ++row) {
                    char text[paneColumns + 1] = {};
                    unsigned value = (step * paneCount + index) * paneRowCount + row;
                    for (u16 column = 0; column < paneColumns; ++column) {
                        text[column] = alphabet[value % 36];
                        value /= 36;
                    }
                    writeTextTo(*screens[index], row, 0, text, attrs);
                }
            }
            const PaneUpdate panes[paneCount] = {
                {PixelRect{0, 0, paneWidth, paneHeight}, captureFrom(*fx.composer, *screens[0], *colors, paneRows[0])},
                {PixelRect{paneWidth, 0, paneWidth, paneHeight}, captureFrom(*fx.composer, *screens[1], *colors, paneRows[1])},
                {PixelRect{(u16)(2 * paneWidth), 0, (u16)(fx.composer->geometry.pixelWidth - 2 * paneWidth), paneHeight}, captureFrom(*fx.composer, *screens[2], *colors, paneRows[2])},
            };
            return metal.renderer->update(panes, paneCount);
        };

        // The held pane's rectangle, as a correct backend draws it.
        const u16 heldWidth = (u16)(paneColumns * glyphWidth);
        const u16 heldHeight = (u16)(paneRowCount * glyphHeight);
        STD_INSIST(heldWidth <= paneWidth);
        STD_INSIST(drawFrame(0));
        STD_INSIST(metal.capture());
        Vector<Color> expected;
        for (u16 y = 0; y < heldHeight; ++y) {
            for (u16 x = 0; x < heldWidth; ++x) {
                expected.pushBack(metal.pixel(x, y));
            }
        }
        // The positive control: a rectangle of nothing but background
        // would compare equal to itself whatever the strips did.
        bool inked = false;
        for (const Color& color : expected) {
            inked = inked || color == paneInk[0];
        }
        STD_INSIST(inked);

        bool collected = false;
        for (unsigned step = 1; step < 60 && !collected; ++step) {
            const u32 before = fx.composer->shaper->spanGeneration();
            STD_INSIST(drawFrame(step));
            collected = fx.composer->shaper->spanGeneration() != before;
            STD_INSIST(metal.capture());
            size_t index = 0;
            for (u16 y = 0; y < heldHeight; ++y) {
                for (u16 x = 0; x < heldWidth; ++x) {
                    STD_INSIST(metal.pixel(x, y) == expected[index++]);
                }
            }
        }
        // Not a skip and not a silence: a run that never collected
        // compared sixty frames that do not hold the subject.
        STD_INSIST(collected);
    }

    // A9 on this backend, which nothing reached until now (R7-test,
    // MM4). "Zero in the grid is a refused frame and not a window-sized
    // default" is closed on the reference renderer by its own
    // AFrameWithoutAGridIsRefused; on Metal every path from unit_tests
    // hands the backend a real Screen, whose grid is never zero, so
    // deleting the refusal outright changed nothing in the suite and
    // llvm-cov listed the line as never executed.
    //
    // It takes two panes, and that is the whole reason this was hard to
    // reach. On a one-pane frame the per-pane refusal is shadowed by the
    // `cellCount == 0` check right below it - a single zeroed grid makes
    // the whole frame's cell count zero, so the frame is refused either
    // way and deleting the per-pane test changes nothing observable. It
    // is only when a healthy pane keeps the count non-zero that the
    // per-pane refusal is the only thing standing between the backend
    // and a walk over cells whose length it does not know.
    STD_TEST(AZeroGridInOnePaneRefusesTheWholeFrame) {
        constexpr u16 border = 3;
        ScreenFixture fx(8, 2, border);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = {1, 2, 3};
        colors->defaultBackground = {0, 0, 128};
        Screen* const healthy = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, colors, 8);
        Screen* const zeroed = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, colors, 8);
        writeTextTo(*healthy, 0, 0, "W", coloredCell({255, 0, 0}, {8, 8, 8}));
        writeTextTo(*zeroed, 0, 0, "W", coloredCell({0, 255, 0}, {8, 8, 8}));
        Vector<TerminalRow> healthyRows;
        Vector<TerminalRow> zeroedRows;

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 half = (u16)(fx.composer->geometry.pixelWidth / 2);
        const PixelRect left{0, 0, half, fx.composer->geometry.pixelHeight};
        const PixelRect right{half, 0, half, fx.composer->geometry.pixelHeight};
        TerminalUpdate healthyUpdate = captureFrom(*fx.composer, *healthy, *colors, healthyRows);
        TerminalUpdate zeroedUpdate = captureFrom(*fx.composer, *zeroed, *colors, zeroedRows);
        // The premise: a real Screen names its own grid, so the zero
        // below has to be put there by hand, and the healthy pane keeps
        // the frame's cell count away from zero.
        STD_INSIST(healthyUpdate.gridColumns == 4 && healthyUpdate.gridRows == 2);
        STD_INSIST(zeroedUpdate.gridColumns == 4 && zeroedUpdate.gridRows == 2);

        gridOf(zeroedUpdate, 0, 2);
        {
            const PaneUpdate panes[2] = {{left, healthyUpdate}, {right, zeroedUpdate}};
            STD_INSIST(!metal.renderer->update(panes, 2));
        }
        // The same, with the pane that has no grid drawn first: the
        // refusal is a property of the pane and not of its position.
        {
            const PaneUpdate panes[2] = {{left, zeroedUpdate}, {right, healthyUpdate}};
            STD_INSIST(!metal.renderer->update(panes, 2));
        }
        // The negative control, and the half without which the refusals
        // above prove nothing: the same two panes with the grid back are
        // accepted, so what was refused was the grid and not something
        // else this stand gets wrong.
        gridOf(zeroedUpdate, 4, 2);
        {
            const PaneUpdate panes[2] = {{left, healthyUpdate}, {right, zeroedUpdate}};
            STD_INSIST(metal.renderer->update(panes, 2));
        }
        // And it drew, rather than merely answering true: each pane's
        // cells carry a background of their own, so a pixel inside them
        // is something other than what the drawable was cleared with.
        STD_INSIST(metal.capture());
        const u16 y = (u16)(border + fx.composer->geometry.cellPixelHeight / 2);
        STD_INSIST(!(metal.pixel((u16)(border + fx.composer->geometry.cellPixelWidth / 2), y) == colors->defaultBackground));
        STD_INSIST(!(metal.pixel((u16)(half + border + fx.composer->geometry.cellPixelWidth / 2), y) == colors->defaultBackground));
    }

    // A6-4 where it was found. shapeChanged compared how many panes and
    // what shape, never which: a frame with the panes in another order
    // kept every retained grid where it was. The two panes here trade
    // places and damage one row each, which is a frame nothing in the
    // retain fits - so it is a reshape and is refused, exactly as a
    // resized grid is. The control is the same swap sent whole:
    // accepted, with each pane's content in its new rectangle.
    STD_TEST(SwappedPanesAreAReshape) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 6, border);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = {1, 2, 3};
        colors->defaultBackground = {0, 0, 128};
        Screen* const first = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, colors, 8);
        Screen* const second = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, colors, 8);
        Vector<TerminalRow> firstRows;
        Vector<TerminalRow> secondRows;
        writeTextTo(*first, 0, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*first, 1, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*second, 0, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));
        writeTextTo(*second, 1, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 half = (u16)(fx.composer->geometry.pixelHeight / 2);
        const PixelRect top{0, 0, fx.composer->geometry.pixelWidth, half};
        const PixelRect bottom{0, half, fx.composer->geometry.pixelWidth, half};
        {
            const PaneUpdate panes[2] = {
                {top, captureFrom(*fx.composer, *first, *colors, firstRows)},
                {bottom, captureFrom(*fx.composer, *second, *colors, secondRows)},
            };
            STD_INSIST(metal.renderer->update(panes, 2));
        }
        first->resetDamage();
        second->resetDamage();
        writeTextTo(*first, 0, 0, " ", coloredCell({255, 0, 255}, {255, 0, 255}));
        writeTextTo(*second, 0, 0, " ", coloredCell({255, 255, 0}, {255, 255, 0}));
        {
            const PaneUpdate panes[2] = {
                {top, captureDamagedFrom(*second, *colors, secondRows)},
                {bottom, captureDamagedFrom(*first, *colors, firstRows)},
            };
            STD_INSIST(panes[0].update.rowCount == 1);
            STD_INSIST(panes[1].update.rowCount == 1);
            STD_INSIST(!metal.renderer->update(panes, 2));
        }

        const PaneUpdate panes[2] = {
            {top, captureFrom(*fx.composer, *second, *colors, secondRows)},
            {bottom, captureFrom(*fx.composer, *first, *colors, firstRows)},
        };
        STD_INSIST(metal.renderer->update(panes, 2));
        STD_INSIST(metal.capture());
        const u16 x = (u16)(border + fx.composer->geometry.cellPixelWidth / 2);
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        STD_INSIST((metal.pixel(x, (u16)(border + glyphHeight / 2)) == Color{255, 255, 0}));
        STD_INSIST((metal.pixel(x, (u16)(border + glyphHeight + glyphHeight / 2)) == Color{255, 255, 255}));
        STD_INSIST((metal.pixel(x, (u16)(half + border + glyphHeight / 2)) == Color{255, 0, 255}));
        STD_INSIST((metal.pixel(x, (u16)(half + border + glyphHeight + glyphHeight / 2)) == Color{255, 0, 0}));
    }

    // A6-3's counterpart in Metal: the second pane sends one damaged row
    // and its undamaged row stays its own. Metal already kept a cell
    // range per pane, so this is the property A9's per-pane offsets had
    // to preserve while those offsets stopped being pane * one grid.
    STD_TEST(APartiallyDamagedPaneKeepsItsOwnRows) {
        constexpr u16 border = 3;
        ScreenFixture fx(4, 6, border);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = {1, 2, 3};
        colors->defaultBackground = {0, 0, 128};
        Screen* const upper = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, colors, 8);
        Screen* const lower = Screen::createPrimary(fx.composer->extras, *fx.pool, 4, 2, colors, 8);
        Vector<TerminalRow> upperRows;
        Vector<TerminalRow> lowerRows;
        writeTextTo(*upper, 0, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*upper, 1, 0, " ", coloredCell({255, 0, 0}, {255, 0, 0}));
        writeTextTo(*lower, 0, 0, " ", coloredCell({255, 255, 255}, {255, 255, 255}));
        writeTextTo(*lower, 1, 0, " ", coloredCell({0, 255, 255}, {0, 255, 255}));

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 half = (u16)(fx.composer->geometry.pixelHeight / 2);
        const PixelRect top{0, 0, fx.composer->geometry.pixelWidth, half};
        const PixelRect bottom{0, half, fx.composer->geometry.pixelWidth, half};
        {
            const PaneUpdate panes[2] = {
                {top, captureFrom(*fx.composer, *upper, *colors, upperRows)},
                {bottom, captureFrom(*fx.composer, *lower, *colors, lowerRows)},
            };
            STD_INSIST(metal.renderer->update(panes, 2));
        }
        upper->resetDamage();
        lower->resetDamage();
        upper->expose();
        writeTextTo(*upper, 0, 0, " ", coloredCell({255, 0, 255}, {255, 0, 255}));
        writeTextTo(*upper, 1, 0, " ", coloredCell({255, 0, 255}, {255, 0, 255}));
        writeTextTo(*lower, 0, 0, " ", coloredCell({255, 255, 0}, {255, 255, 0}));

        const TerminalUpdate lowerUpdate = captureDamagedFrom(*lower, *colors, lowerRows);
        STD_INSIST(lowerUpdate.rowCount == 1);
        const PaneUpdate panes[2] = {
            {top, captureDamagedFrom(*upper, *colors, upperRows)},
            {bottom, lowerUpdate},
        };
        STD_INSIST(metal.renderer->update(panes, 2));
        STD_INSIST(metal.capture());

        const u16 x = (u16)(border + fx.composer->geometry.cellPixelWidth / 2);
        const u16 glyphHeight = fx.composer->geometry.cellPixelHeight;
        STD_INSIST((metal.pixel(x, (u16)(border + glyphHeight / 2)) == Color{255, 0, 255}));
        STD_INSIST((metal.pixel(x, (u16)(half + border + glyphHeight / 2)) == Color{255, 255, 0}));
        const u16 undamaged = (u16)(half + border + glyphHeight + glyphHeight / 2);
        STD_INSIST((metal.pixel(x, undamaged) == Color{0, 255, 255}));
        STD_INSIST((!(metal.pixel(x, undamaged) == Color{255, 0, 255})));
    }

    // F9. Each pane's padding is that pane's own background, and not the
    // first pane's.
    //
    // A defect on its own, older than the divider work and reachable
    // without it: two panes whose shells set different OSC 11 already
    // drew wrong. render.h:65-68 states the contract - "the backend
    // clears that rectangle... the clear is per pane and not per frame,
    // which is what lets two panes with two different backgrounds share
    // one drawable" - and this backend did not keep it. It cleared the
    // drawable once with frame[0]'s background, and its own comment
    // deferred whose colour that should be to T9.
    //
    // Nothing could see it: DrawThreeGridsInOneFrame above gives all
    // three panes one TerminalColors, so the first pane's background and
    // every other pane's are the same colour by construction, and it
    // reads pixels against that single colour. Two different backgrounds
    // are the whole instrument here.
    //
    // The divider is switched off deliberately. This has to fail on a
    // build with no seam at all, or it would be reading the divider work
    // rather than the defect that outlived it.
    STD_TEST(EachPanesPaddingIsItsOwnBackgroundAndNotItsNeighbours) {
        constexpr u16 border = 3;
        constexpr u16 columns = 4;
        const Color leftBackground{0, 0, 128};
        const Color rightBackground{128, 64, 0};
        ScreenFixture fx(24, 2, border);
        fx.options.paneDividerWidth = 0;

        auto* const leftColors = fx.pool->make<TerminalColors>();
        leftColors->defaultForeground = {1, 2, 3};
        leftColors->defaultBackground = leftBackground;
        auto* const rightColors = fx.pool->make<TerminalColors>();
        rightColors->defaultForeground = {1, 2, 3};
        rightColors->defaultBackground = rightBackground;
        // The premise, asserted rather than assumed: a stand whose two
        // panes agreed about their background could not tell the two
        // answers apart at all.
        STD_INSIST(!(leftBackground == rightBackground));

        Screen* screens[2];
        Vector<TerminalRow> paneRows[2];
        TerminalColors* const paneColors[2] = {leftColors, rightColors};
        for (unsigned index = 0; index < 2; ++index) {
            screens[index] = Screen::createPrimary(fx.composer->extras, *fx.pool, columns, 2, paneColors[index], 8);
            TerminalCell attrs{};
            attrs.setForeground(CellColor::direct({255, 255, 255}));
            attrs.setBackground(CellColor::direct({8, 8, 8}));
            for (u16 row = 0; row < 2; ++row) {
                for (u16 column = 0; column < columns; ++column) {
                    writeTextTo(*screens[index], row, column, "W", attrs);
                }
            }
        }

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 paneWidth = (u16)(fx.composer->geometry.pixelWidth / 2);
        const u16 paneHeight = fx.composer->geometry.pixelHeight;
        const PaneUpdate panes[2] = {
            {PixelRect{0, 0, paneWidth, paneHeight}, captureFrom(*fx.composer, *screens[0], *leftColors, paneRows[0])},
            {PixelRect{paneWidth, 0, (u16)(fx.composer->geometry.pixelWidth - paneWidth), paneHeight}, captureFrom(*fx.composer, *screens[1], *rightColors, paneRows[1])},
        };
        STD_INSIST(metal.renderer->update(panes, 2));
        STD_INSIST(metal.capture());
        STD_INSIST(metal.width == fx.composer->geometry.pixelWidth);

        // A pixel inside each pane's padding: one pixel in from the
        // pane's own left edge is still air, because the grid starts a
        // whole border further in. Read at a row well inside the pane, so
        // that the top inset is not what is being sampled instead.
        const u16 sampleRow = (u16)(paneHeight / 2);
        STD_INSIST(border >= 2);
        const Color leftPadding = metal.pixel(1, sampleRow);
        const Color rightPadding = metal.pixel((u16)(paneWidth + 1), sampleRow);

        // The left pane's padding was always right - it is the pane the
        // drawable was cleared with. Asserted anyway, as the positive
        // control: without it a build that painted nothing at all would
        // satisfy the assertion that follows.
        STD_INSIST(leftPadding == leftBackground);
        // And the one that was wrong: the right pane's air wore the left
        // pane's colour.
        STD_INSIST(rightPadding == rightBackground);
        STD_INSIST(!(rightPadding == leftBackground));
    }

    // F7. A preedit overlay lands on the strips of the pane that sent
    // it, and the cell it lands at is a sum of three terms: the pane's
    // own cell offset into the frame's single cell array, the overlay's
    // row, and the overlay's column. M7b dropped the first term by hand
    // (`rowIndex` in overrideOverlayStrips computed without
    // `cellOffset`, so a preedit typed in pane 1 wrote its strips over
    // pane 0) and nothing anywhere went red: 967 unit tests, six tests
    // in tst/test_preedit.py, three in tst/test_gpu_parity.py including
    // both of the multi-pane ones.
    //
    // Why nothing saw it. The only stand in this file that ever set an
    // overlay was PreeditOverlayCoversUnderlyingStrips, built on
    // ScreenFixture(4, 1) - one pane, whose cell offset is zero, where
    // a dropped term and a present one are the same number.
    // test_preedit.py reads the model's snapshot and never reaches a
    // strip at all; test_gpu_parity.py draws several panes but never a
    // preedit; and the word "overlay" does not occur in this suite.
    // That is the eighth degenerate fixture of this merge and the same
    // shape as the other seven: the parameter that zeroes the thing
    // under test is the default, and every stand takes the default.
    //
    // So this one refuses the default on all three terms at once. The
    // overlay goes to the *second* pane, on a row that is not row zero,
    // at a column that is not column zero, and the row and the column
    // are different numbers - which is not decoration. Checking that a
    // sum reacts is not enough; its terms have to be distinguishable
    // from each other, or one wrong answer hides inside another's. So
    // the three addresses the three plausible answers give are computed
    // and asserted to be three disjoint runs of cells before anything
    // is drawn. A stand where two of them coincide cannot tell those
    // two mutations apart, which is how T5.2's IME anchor let a swapped
    // pair of axes hide inside a lost offset.
    //
    // The instrument is blank overlay cells over inked ones, exactly as
    // the reference renderer's twin does it: covered cells lose their
    // strips and fall back to their own background, every cell the
    // overlay did not cover keeps its ink. Absence at the right address
    // and presence at both wrong ones is what pins the sum.
    STD_TEST(AnOverlayLandsOnTheStripsOfItsOwnPane) {
        constexpr unsigned paneCount = 2;
        constexpr u16 paneColumns = 6;
        constexpr u16 paneRowCount = 3;
        // The pane the overlay belongs to. Setting this to 0 puts the
        // overlay's pane at cell offset zero and the stand goes blind -
        // the term the mutation drops would be nothing to drop. The
        // premise assertions below refuse that quietly-degenerate
        // fixture out loud rather than passing.
        constexpr unsigned overlayPane = 1;
        constexpr unsigned quietPane = paneCount - 1 - overlayPane;
        constexpr u16 overlayRow = 2;
        constexpr u16 overlayColumn = 1;
        constexpr u16 overlayCount = 2;
        const Color ink{255, 255, 255};
        const Color cellBackground{8, 8, 8};

        ScreenFixture fx((u16)(paneCount * paneColumns), paneRowCount);
        auto* const colors = fx.pool->make<TerminalColors>();
        colors->defaultForeground = ink;
        colors->defaultBackground = {0, 0, 128};
        TerminalCell attrs{};
        attrs.setForeground(CellColor::direct(ink));
        attrs.setBackground(CellColor::direct(cellBackground));

        Screen* screens[paneCount];
        Vector<TerminalRow> paneRows[paneCount];
        for (unsigned index = 0; index < paneCount; ++index) {
            screens[index] = Screen::createPrimary(fx.composer->extras, *fx.pool, paneColumns, paneRowCount, colors, 8);
            // Every cell of every pane inked: a strip written to the
            // wrong place only shows where there was something to
            // displace, and a strip taken from the wrong place only
            // shows where there was something to take.
            for (u16 row = 0; row < paneRowCount; ++row) {
                writeTextTo(*screens[index], row, 0, "MWQBHX", attrs);
            }
        }

        MetalFixture metal(*fx.composer);
        STD_INSIST(metal.renderer != nullptr);
        const u16 glyphWidth = fx.composer->geometry.cellPixelWidth;
        const u16 paneWidth = (u16)(paneColumns * glyphWidth);
        const u16 paneHeight = fx.composer->geometry.pixelHeight;
        // No border and no chrome reserve here, so a pane's cell grid
        // starts at its rectangle's corner and the pixel arithmetic
        // below is the whole mapping. Asserted, not assumed.
        const Insets insets = fx.composer->contentInsets();
        STD_INSIST(insets.left == 0 && insets.top == 0);
        STD_INSIST(fx.composer->geometry.pixelWidth == paneCount * paneWidth);

        // The three answers, in cells of the frame's single cell array,
        // computed the way render_metal.mm computes the one it uses.
        const size_t paneCells = (size_t)(paneColumns)*paneRowCount;
        const size_t rightAnswer = overlayPane * paneCells + (size_t)(overlayRow)*paneColumns + overlayColumn;
        const size_t lostOffset = (size_t)(overlayRow)*paneColumns + overlayColumn;
        const size_t swappedAxes = overlayPane * paneCells + (size_t)(overlayColumn)*paneColumns + overlayRow;
        // The axes have to be told apart from each other before they
        // can be told apart from the offset.
        STD_INSIST(overlayRow != 0);
        STD_INSIST(overlayColumn != 0);
        STD_INSIST(overlayRow != overlayColumn);
        // And the three runs, overlayCount cells wide each, have to be
        // disjoint - equal starts would be a stand that cannot see one
        // of the two mutations at all, and overlapping runs a stand
        // that reports the wrong one.
        STD_INSIST(lostOffset + overlayCount <= rightAnswer || rightAnswer + overlayCount <= lostOffset);
        STD_INSIST(swappedAxes + overlayCount <= rightAnswer || rightAnswer + overlayCount <= swappedAxes);
        STD_INSIST(lostOffset + overlayCount <= swappedAxes || swappedAxes + overlayCount <= lostOffset);

        const auto inked = [&](unsigned pane, u16 row, u16 column) {
            return paneCellHasInk(metal, *fx.composer, (u16)(pane * paneWidth), 0, row, column, cellBackground);
        };
        TerminalCell blank[overlayCount] = {attrs, attrs};
        const auto drawFrame = [&](TerminalCell* overlay) {
            TerminalUpdate updates[paneCount];
            for (unsigned index = 0; index < paneCount; ++index) {
                updates[index] = captureFrom(*fx.composer, *screens[index], *colors, paneRows[index]);
            }
            if (overlay != nullptr) {
                updates[overlayPane].overlayCells = overlay;
                updates[overlayPane].overlayRow = overlayRow;
                updates[overlayPane].overlayColumn = overlayColumn;
                updates[overlayPane].overlayCount = overlayCount;
            }
            const PaneUpdate panes[paneCount] = {
                {PixelRect{0, 0, paneWidth, paneHeight}, updates[0]},
                {PixelRect{paneWidth, 0, paneWidth, paneHeight}, updates[1]},
            };
            return metal.renderer->update(panes, paneCount) && metal.capture();
        };

        // The positive control, and the half without which every
        // absence below proves nothing: all three addresses hold ink
        // before the overlay arrives.
        STD_INSIST(drawFrame(nullptr));
        STD_INSIST(inked(overlayPane, overlayRow, overlayColumn));
        STD_INSIST(inked(overlayPane, overlayRow, (u16)(overlayColumn + 1)));
        STD_INSIST(inked(quietPane, overlayRow, overlayColumn));
        STD_INSIST(inked(quietPane, overlayRow, (u16)(overlayColumn + 1)));
        STD_INSIST(inked(overlayPane, overlayColumn, overlayRow));
        STD_INSIST(inked(overlayPane, overlayColumn, (u16)(overlayRow + 1)));

        STD_INSIST(drawFrame(blank));

        // The right address: the covered cells lost their strips.
        STD_INSIST(!inked(overlayPane, overlayRow, overlayColumn));
        STD_INSIST(!inked(overlayPane, overlayRow, (u16)(overlayColumn + 1)));
        // The address a lost pane offset gives - M7b's mutation M-c,
        // which the whole tree survived.
        STD_INSIST(inked(quietPane, overlayRow, overlayColumn));
        STD_INSIST(inked(quietPane, overlayRow, (u16)(overlayColumn + 1)));
        // The address a swapped pair of axes gives, which is a
        // different wrong answer and has to stay one.
        STD_INSIST(inked(overlayPane, overlayColumn, overlayRow));
        STD_INSIST(inked(overlayPane, overlayColumn, (u16)(overlayRow + 1)));
        // And the run stopped where it said it would, one cell short on
        // either side: a stand that only read the two covered cells
        // would pass on a backend that blanked the whole row.
        STD_INSIST(inked(overlayPane, overlayRow, (u16)(overlayColumn - 1)));
        STD_INSIST(inked(overlayPane, overlayRow, (u16)(overlayColumn + overlayCount)));
    }

}

#endif
