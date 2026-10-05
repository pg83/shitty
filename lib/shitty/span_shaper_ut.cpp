/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "composer.h"
#include "font_pack.h"
#include "span_shaper.h"
#include "font_embedded.h"
#include "font_resolver.h"
#include "grid_geometry.h"

#include <lib/vterm/screen.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/terminal_types.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/tst/ut.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>

using namespace stl;

#if defined(HAVE_FREETYPE) && defined(HAVE_HARFBUZZ)
namespace {
    // A pictogram the embedded nerd font covers.
    static constexpr u32 shapeIcon = 0xe606;

    static void configureColors(TerminalColors& colors) {
        colors.defaultForeground = {1, 2, 3};
        colors.defaultBackground = {4, 5, 6};
    }

    static TerminalCell attributes() {
        TerminalCell cell{};
        cell.setForeground(CellColor::defaultForeground());
        cell.setBackground(CellColor::defaultBackground());
        return cell;
    }

    struct ShapeFixture {
        ShapeFixture();

        void writeText(Screen& screen, u16 row, u16 column, const char* text);
        size_t rowSpans(i32 viewRow, ScreenRowSpan* out);

        ObjPool::Ref pool = ObjPool::fromMemory();
        TerminalColors colors;
        Composer* composer = nullptr;
        Screen* screen = nullptr;
        SpanShaper* shaper = nullptr;
    };
}

ShapeFixture::ShapeFixture() {
    configureColors(colors);
    composer = pool->make<Composer>(pool.mutPtr());
    // Only the embedded resolver: the tests must not depend on system
    // fonts.
    while (!composer->fontResolvers.empty()) {
        composer->fontResolvers.popFront();
    }
    composer->fontResolvers.pushBack(createEmbeddedFontResolver(*composer));
    composer->fonts = Fontpack::create(*composer, *pool, nullptr, 0, nullptr, 0, 16);
    composer->geometry.setCellPixelSize(composer->fonts->getPx(), composer->fonts->getPy());
    // A1: the surface is the grid plus the content insets, never twice
    // the border option - the same formula every other fixture uses.
    const Insets insets = composer->contentInsets();
    composer->resize((u16)(gridPixelWidth(16, insets, composer->geometry.cellPixelWidth)), (u16)(gridPixelHeight(4, insets, composer->geometry.cellPixelHeight)));
    shaper = SpanShaper::create(*composer, *pool);
    composer->shaper = shaper;
    screen = Screen::createPrimary(composer->extras, *pool, 16, 4, &colors, 8);
}

void ShapeFixture::writeText(Screen& screen_, u16 row, u16 column, const char* text) {
    const TerminalCell attrs = attributes();
    for (size_t index = 0; text[index] != 0; ++index) {
        const u32 codepoint = (u32)(u8)(text[index]);
        screen_.writeGrapheme(row, (u16)(column + index), &codepoint, 1, false, attrs, 0, 0, attrs);
    }
}

size_t ShapeFixture::rowSpans(i32 viewRow, ScreenRowSpan* out) {
    const ScreenRowRef row = screen->viewRow(viewRow);
    return shaper->rowSpans(row.cells, screen->info().columns, row.id, out);
}

STD_TEST_SUITE(SpanShaper) {
    STD_TEST(PlainTextIsOneSpanWithInk) {
        ShapeFixture fx;
        fx.writeText(*fx.screen, 0, 0, "abc");
        ScreenRowSpan spans[16];
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 1);
        // Three letters and the blanks behind them, up to the capture
        // limit: the canvas for whatever ink crosses the last cell.
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 8);
        STD_INSIST(!spans[0].color && !spans[0].missing);
        const u16 width = fx.composer->fonts->getPx();
        const u16 height = fx.composer->fonts->getPy();
        const size_t stride = (size_t)(spans[0].end - spans[0].begin) * width;
        const u8* const arena = fx.shaper->spanMask();
        bool ink = false;
        for (u16 row = 0; row < height && !ink; ++row) {
            for (size_t x = 0; x < stride && !ink; ++x) {
                ink = arena[spans[0].offset + (size_t)(row)*stride + x] > 64;
            }
        }
        STD_INSIST(ink);
    }

    STD_TEST(BlanksSplitSpansAndDedupRepeats) {
        ShapeFixture fx;
        fx.writeText(*fx.screen, 0, 0, "ab ab ab");
        ScreenRowSpan spans[16];
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 3);
        // In running text the single blank between words is all there is
        // to take: interior words stay "ab " and intern the same strip.
        // The last word of the row takes the full capture behind it.
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 3);
        STD_INSIST(spans[1].begin == 3 && spans[1].end == 6);
        STD_INSIST(spans[0].offset == spans[1].offset);
        STD_INSIST(spans[2].begin == 6 && spans[2].end == 13);
    }

    STD_TEST(MutationReshapesTheRow) {
        ShapeFixture fx;
        fx.writeText(*fx.screen, 0, 0, "ab");
        ScreenRowSpan spans[16];
        STD_INSIST(fx.rowSpans(0, spans) == 1);
        fx.writeText(*fx.screen, 0, 2, "c");
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 1);
        STD_INSIST(spans[0].end == 8);
    }

    STD_TEST(IconCapturesOneBlank) {
        ShapeFixture fx;
        const TerminalCell attrs = attributes();
        const u32 icon = shapeIcon;
        fx.screen->writeGrapheme(0, 0, &icon, 1, false, attrs, 0, 0, attrs);
        fx.writeText(*fx.screen, 0, 2, "x");
        ScreenRowSpan spans[16];
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 2);
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 2);
        STD_INSIST(spans[1].begin == 2 && spans[1].end == 8);
    }

    STD_TEST(AdjacentIconsCaptureTheBlanksBehindThem) {
        // Two icons are one span, and it takes the blanks behind it like
        // any other - the pictogram rule is the general rule now.
        ShapeFixture fx;
        const TerminalCell attrs = attributes();
        const u32 icon = shapeIcon;
        fx.screen->writeGrapheme(0, 0, &icon, 1, false, attrs, 0, 0, attrs);
        fx.screen->writeGrapheme(0, 1, &icon, 1, false, attrs, 0, 0, attrs);
        ScreenRowSpan spans[16];
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 1);
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 7);
    }

    STD_TEST(ADifferentlyPaintedBlankIsNotCaptured) {
        // The strip is a mask: ink crossing into the captured cell takes
        // that cell's colors, so a blank that would paint it differently
        // stays outside the span. Shaping attributes are another matter -
        // see BoldBlankIsStillCaptured.
        ShapeFixture fx;
        TerminalCell attrs = attributes();
        const u32 letter = 'w';
        fx.screen->writeGrapheme(0, 0, &letter, 1, false, attrs, 0, 0, attrs);
        TerminalCell other = attrs;
        other.setForeground(CellColor::indexed(1));
        const u32 blank = ' ';
        fx.screen->writeGrapheme(0, 1, &blank, 1, false, other, 0, 0, other);
        ScreenRowSpan spans[16];
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 1);
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 1);
    }

    STD_TEST(BoldBlankIsStillCaptured) {
        // A blank shapes to nothing, so the attributes that only pick a
        // face cannot keep it out of the span: the ink of the run before
        // it lands in it either way.
        ShapeFixture fx;
        TerminalCell attrs = attributes();
        const u32 letter = 'w';
        fx.screen->writeGrapheme(0, 0, &letter, 1, false, attrs, 0, 0, attrs);
        TerminalCell bolder = attrs;
        bolder.bold = 1;
        const u32 blank = ' ';
        fx.screen->writeGrapheme(0, 1, &blank, 1, false, bolder, 0, 0, bolder);
        ScreenRowSpan spans[16];
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 1);
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 6);
    }

    STD_TEST(BlankRowHasNoSpans) {
        ShapeFixture fx;
        ScreenRowSpan spans[16];
        STD_INSIST(fx.rowSpans(0, spans) == 0);
    }

    STD_TEST(ExtrasStoreReplacementKeepsStrips) {
        ShapeFixture fx;
        fx.writeText(*fx.screen, 0, 0, "abc");
        ScreenRowSpan spans[16];
        STD_INSIST(fx.rowSpans(0, spans) == 1);
        const u32 offset = spans[0].offset;
        const size_t used = fx.shaper->spanMaskUsed();
        // Replacing the store voids the raw-bytes cache level. Mutate the
        // row so it reshapes: the strip must come back through the
        // materialized level without growing the arena.
        fx.composer->extras.replace(CellExtraStore::create(fx.composer->extras, *fx.composer->pool, 64));
        fx.writeText(*fx.screen, 0, 0, "abc");
        STD_INSIST(fx.rowSpans(0, spans) == 1);
        STD_INSIST(spans[0].offset == offset);
        STD_INSIST(fx.shaper->spanMaskUsed() == used);
    }

    STD_TEST(FontChangeResetsStrips) {
        ShapeFixture fx;
        fx.writeText(*fx.screen, 0, 0, "abc");
        ScreenRowSpan spans[16];
        STD_INSIST(fx.rowSpans(0, spans) == 1);
        STD_INSIST(fx.shaper->spanMaskUsed() != 0);
        for (IntrusiveNode* node = fx.composer->fontChangedListeners.mutFront(); node != fx.composer->fontChangedListeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }
        STD_INSIST(fx.shaper->spanMaskUsed() == 0);
        STD_INSIST(fx.rowSpans(0, spans) == 1);
        STD_INSIST(fx.shaper->spanMaskUsed() != 0);
    }

    STD_TEST(ArenaOverflowResetsWithinBudget) {
        ShapeFixture fx;
        ScreenRowSpan spans[16];
        const size_t budget = 3u * 16 * 4 * fx.composer->fonts->getPx() * fx.composer->fonts->getPy();
        const u32 before = fx.shaper->spanGeneration();
        // Churn one row with unique content: every reshape appends a new
        // strip, the previous becomes garbage. The arena must stay under
        // its budget by resetting, and the final content must render
        // correctly into the fresh arenas.
        char text[8];
        for (u32 round = 0; round < 400; ++round) {
            text[0] = (char)('a' + round % 26);
            text[1] = (char)('a' + (round / 26) % 26);
            text[2] = (char)('0' + round % 10);
            text[3] = 0;
            fx.writeText(*fx.screen, 1, 0, text);
            STD_INSIST(fx.rowSpans(1, spans) >= 1);
        }
        STD_INSIST(fx.shaper->spanMaskUsed() <= budget);
        STD_INSIST(fx.shaper->spanGeneration() > before);
        const size_t count = fx.rowSpans(1, spans);
        STD_INSIST(count == 1);
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 8);
        STD_INSIST(spans[0].offset + (size_t)(8) * fx.composer->fonts->getPx() * fx.composer->fonts->getPy() <= fx.shaper->spanMaskUsed());
    }

    STD_TEST(LigatureFormsAcrossCells) {
        ShapeFixture fx;
        Fontpack& fonts = *fx.composer->fonts;
        const u32 arrow[] = {'-', '>'};
        Font* const face = fonts.resolveFace(arrow, 1);
        STD_INSIST(face != nullptr && !face->colored());
        const u16 width = fonts.getPx();
        const u16 height = fonts.getPy();
        const size_t strip = (size_t)(2) * width * height;
        Buffer shaped;
        shaped.grow(strip);
        shaped.seekAbsolute(strip);
        shaped.zero(strip);
        face->render(arrow, 2, 2, shaped.mutData());

        // The same two codepoints rendered as isolated cells: the shaped
        // strip must differ - the arrow ligature replaced them.
        Buffer isolated;
        isolated.grow(strip);
        isolated.seekAbsolute(strip);
        isolated.zero(strip);
        Buffer glyph;
        glyph.grow((size_t)(width)*height);
        for (u16 cell = 0; cell < 2; ++cell) {
            glyph.seekAbsolute((size_t)(width)*height);
            glyph.zero((size_t)(width)*height);
            face->render(&arrow[cell], 1, 1, glyph.mutData());
            for (u16 row = 0; row < height; ++row) {
                __builtin_memcpy((u8*)(isolated.mutData()) + (size_t)(row) * 2 * width + (size_t)(cell)*width, (const u8*)(glyph.data()) + (size_t)(row)*width, width);
            }
        }
        size_t shapedInk = 0;
        size_t isolatedInk = 0;
        bool differ = false;
        for (size_t index = 0; index < strip; ++index) {
            shapedInk += ((const u8*)(shaped.data()))[index] > 8;
            isolatedInk += ((const u8*)(isolated.data()))[index] > 8;
            differ = differ || ((const u8*)(shaped.data()))[index] != ((const u8*)(isolated.data()))[index];
        }
        STD_INSIST(shapedInk != 0);
        STD_INSIST(isolatedInk != 0);
        STD_INSIST(differ);
    }

    STD_TEST(ScrolledRowKeepsItsShapeThroughHistory) {
        ShapeFixture fx;
        fx.writeText(*fx.screen, 0, 0, "abc");
        ScreenRowSpan spans[16];
        STD_INSIST(fx.rowSpans(0, spans) == 1);
        const TerminalCell attrs = attributes();
        fx.screen->scrollRows(0, 4, -1, attrs);
        // The shaped row is history now; its identity moved with it, so
        // scrolling the view back serves the cached spans.
        STD_INSIST(fx.screen->scrollView(1));
        const size_t count = fx.rowSpans(0, spans);
        STD_INSIST(count == 1);
        STD_INSIST(spans[0].begin == 0 && spans[0].end == 8);
    }
}
#endif
