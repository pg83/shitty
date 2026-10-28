/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "composer.h"

#include <lib/vterm/listener.h>
#include <lib/vterm/cell_extra_store.h>

#include <std/tst/ut.h>
#include <std/mem/obj_pool.h>

#include <cstring>

using namespace stl;

namespace {
    struct ExtraChangeListener final: public Listener {
        explicit ExtraChangeListener(Composer& composer);

        void onListen(void*) override;

        Composer& composer;
        CellExtraStore* observed = nullptr;
        size_t calls = 0;
    };

    static bool equal(StringView left, StringView right) {
        return left == right;
    }

    static CellExtraStore* createStore(Composer& composer) {
        CellExtraStore* const store = CellExtraStore::create(composer.extras, *composer.pool);
        composer.extras.replace(store);
        return store;
    }
}

ExtraChangeListener::ExtraChangeListener(Composer& composer_)
    : composer(composer_)
{
}

void ExtraChangeListener::onListen(void*) {
    observed = composer.extras.store;
    ++calls;
}

STD_TEST_SUITE(CellExtraStore) {
    STD_TEST(FactoryDoesNotReplaceComposerStore) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* const original = composer.extras.store;
        CellExtraStore* const store = CellExtraStore::create(composer.extras, *composer.pool);

        STD_INSIST(store != nullptr);
        STD_INSIST(composer.extras.store == original);
    }

    STD_TEST(KeepsInlineUnderlineColorWithoutExtra) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        TerminalCell cell{};
        const CellColor color = CellColor::direct({17, 34, 51});

        store.setUnderlineColor(cell, color);

        STD_INSIST(!cell.hasExtra());
        STD_INSIST(store.underlineColor(cell) == color);
    }

    STD_TEST(CombinesAndClearsIndependentExtraValues) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        TerminalCell cell{};
        const CellColor underline = CellColor::indexed(42);
        const u32 grapheme[] = {'a', 0x0301};
        const u32 hyperlink = store.getOrCreateHyperlink(StringView(u8"id"), StringView(u8"https://example.test"), 73);

        store.setUnderlineColor(cell, underline);
        store.setGrapheme(cell, grapheme, 2);
        store.setHyperlink(cell, hyperlink);

        const GraphemeView stored = store.grapheme(cell);
        STD_INSIST(cell.hasExtra());
        STD_INSIST(store.underlineColor(cell) == underline);
        STD_INSIST(stored.size() == 2);
        STD_INSIST(stored[0] == grapheme[0]);
        STD_INSIST(stored[1] == grapheme[1]);
        STD_INSIST(equal(store.hyperlink(cell), StringView(u8"https://example.test")));
        STD_INSIST(store.hyperlinkDisplayId(cell) == 73);

        store.clearGrapheme(cell);

        STD_INSIST(store.grapheme(cell).empty());
        STD_INSIST(equal(store.hyperlink(cell), StringView(u8"https://example.test")));
        STD_INSIST(store.underlineColor(cell) == underline);

        store.clearHyperlink(cell);

        STD_INSIST(!cell.hasExtra());
        STD_INSIST(store.hyperlink(cell).empty());
        STD_INSIST(store.underlineColor(cell) == underline);
    }

    STD_TEST(ReusesHyperlinkIdentity) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        const u32 first = store.getOrCreateHyperlink(StringView(u8"same"), StringView(u8"first"), 1);
        const u32 second = store.getOrCreateHyperlink(StringView(u8"same"), StringView(u8"second"), 2);
        TerminalCell cell{};
        store.setHyperlink(cell, second);

        STD_INSIST(first == second);
        STD_INSIST(store.findHyperlink(StringView(u8"same")) == first);
        STD_INSIST(store.hyperlinkCount() == 1);
        STD_INSIST(equal(store.hyperlink(cell), StringView(u8"first")));
        STD_INSIST(store.hyperlinkDisplayId(cell) == 1);
    }

    STD_TEST(CollectionRebuildsStoreAndPreservesLiveValues) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        ExtraChangeListener listener(composer);
        composer.extras.changedListeners.pushBack(&listener);
        CellExtraStore* store = createStore(composer);
        TerminalCell cell{};
        const u32 grapheme[] = {0x1f469, 0x200d, 0x1f4bb};
        const u32 hyperlink = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 19);
        store->setGrapheme(cell, grapheme, 3);
        store->setHyperlink(cell, hyperlink);
        Vector<TerminalCell*> cells;
        cells.pushBack(&cell);
        const size_t notificationsBefore = listener.calls;

        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(listener.calls == notificationsBefore + 1);
        STD_INSIST(listener.observed == store);
        STD_INSIST(cell.hasExtra());
        STD_INSIST(store->grapheme(cell).size() == 3);
        STD_INSIST(store->grapheme(cell)[2] == grapheme[2]);
        STD_INSIST(equal(store->hyperlink(cell), StringView(u8"https://live.test")));
        STD_INSIST(store->hyperlinkDisplayId(cell) == 19);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) != 0);
    }

    STD_TEST(CollectionDropsUnreachableHyperlinks) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer);
        const u32 live = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 1);
        store->getOrCreateHyperlink(StringView(u8"dead"), StringView(u8"https://dead.test"), 2);
        TerminalCell cell{};
        store->setHyperlink(cell, live);
        Vector<TerminalCell*> cells;
        cells.pushBack(&cell);

        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(store->hyperlinkCount() == 1);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) != 0);
        STD_INSIST(store->findHyperlink(StringView(u8"dead")) == 0);
    }

    STD_TEST(EqualGraphemesShareOneRecord) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer);
        TerminalCell first{};
        TerminalCell second{};
        TerminalCell third{};
        const u32 grapheme[] = {'x', 0x0301};
        store->setGrapheme(first, grapheme, 2);
        store->setGrapheme(second, grapheme, 2);
        third.setInlineUnderlineColor(CellColor::indexed(3));
        store->setGrapheme(third, grapheme, 2);

        // The same cluster under the same underline color is one record;
        // another underline color is another record.
        STD_INSIST(first.extraRef() == second.extraRef());
        STD_INSIST(third.extraRef() != first.extraRef());
        STD_INSIST(store->underlineColor(third) == CellColor::indexed(3));

        // Records are copy-on-write: changing one cell leaves the other
        // on the shared record.
        store->setUnderlineColor(second, CellColor::indexed(5));
        STD_INSIST(second.extraRef() != first.extraRef());
        STD_INSIST(store->grapheme(first).size() == 2);
        STD_INSIST(store->underlineColor(first) == CellColor::defaultForeground());
        STD_INSIST(store->underlineColor(second) == CellColor::indexed(5));

        // Collection keeps the sharing, and the record found after it is
        // the relocated one, not a stale ref from before.
        Vector<TerminalCell*> cells;
        cells.pushBack(&first);
        cells.pushBack(&second);
        cells.pushBack(&third);
        store->collect(cells, nullptr, 0);
        TerminalCell fourth{};
        store->setGrapheme(fourth, grapheme, 2);
        STD_INSIST(fourth.extraRef() == first.extraRef());
        STD_INSIST(store->grapheme(fourth).size() == 2);
        STD_INSIST(store->grapheme(fourth)[1] == 0x0301);
    }

    STD_TEST(CollectionRewritesNonCellRoots) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer);
        store->getOrCreateHyperlink(StringView(u8"dead"), StringView(u8"https://dead.test"), 1);
        u32 root = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 2);
        u32* roots[] = {&root};
        Vector<TerminalCell*> cells;

        store->collect(cells, roots, 1);
        store = composer.extras.store;

        TerminalCell cell{};
        store->setHyperlink(cell, root);
        STD_INSIST(root == 1);
        STD_INSIST(store->findHyperlink(StringView(u8"dead")) == 0);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) == root);
        STD_INSIST(store->hyperlink(cell) == StringView(u8"https://live.test"));
    }

    STD_TEST(SixelPatchSharesPaletteAcrossCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        u8 pixels[SixelPatch::pixelCount];
        u8 palette[SixelPatch::paletteBytes];
        for (size_t index = 0; index < sizeof(pixels); ++index) {
            pixels[index] = (u8)(index);
        }
        for (size_t index = 0; index < sizeof(palette); ++index) {
            palette[index] = (u8)(index * 7);
        }
        const u8* interned = store.internSixelPalette(palette);
        TerminalCell first{};
        TerminalCell second{};

        store.setSixel(first, pixels, interned);
        store.setSixel(second, pixels, interned);

        const CellExtraView firstView = store.view(first);
        const CellExtraView secondView = store.view(second);
        STD_INSIST(first.hasExtra());
        STD_INSIST(memcmp(firstView.sixelPixels, pixels, sizeof(pixels)) == 0);
        STD_INSIST(memcmp(firstView.sixelPalette, palette, sizeof(palette)) == 0);
        STD_INSIST(firstView.sixelPalette == secondView.sixelPalette);
        STD_INSIST(firstView.sixelPixels != pixels);
    }

    STD_TEST(SixelAndGraphemeDisplaceEachOther) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        u8 pixels[SixelPatch::pixelCount] = {1};
        u8 palette[SixelPatch::paletteBytes] = {2};
        const u8* interned = store.internSixelPalette(palette);
        const u32 grapheme[] = {'x', 0x0301};
        TerminalCell cell{};

        store.setGrapheme(cell, grapheme, 2);
        store.setSixel(cell, pixels, interned);
        STD_INSIST(store.grapheme(cell).empty());
        STD_INSIST(store.view(cell).sixelPixels != nullptr);

        store.setGrapheme(cell, grapheme, 2);
        STD_INSIST(store.view(cell).sixelPixels == nullptr);
        STD_INSIST(store.grapheme(cell).size() == 2);
    }

    STD_TEST(ClearHyperlinkKeepsSixel) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        u8 pixels[SixelPatch::pixelCount] = {3};
        u8 palette[SixelPatch::paletteBytes] = {4};
        const u8* interned = store.internSixelPalette(palette);
        const u32 hyperlink = store.getOrCreateHyperlink(StringView(u8"id"), StringView(u8"https://example.test"), 5);
        TerminalCell cell{};

        store.setSixel(cell, pixels, interned);
        store.setHyperlink(cell, hyperlink);
        STD_INSIST(store.view(cell).sixelPixels != nullptr);
        STD_INSIST(!store.hyperlink(cell).empty());

        store.clearHyperlink(cell);
        STD_INSIST(cell.hasExtra());
        STD_INSIST(store.hyperlink(cell).empty());
        STD_INSIST(store.view(cell).sixelPixels != nullptr);
    }

    STD_TEST(CollectionRelocatesOnePaletteCopyPerImage) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer);
        u8 pixels[SixelPatch::pixelCount];
        u8 palette[SixelPatch::paletteBytes];
        for (size_t index = 0; index < sizeof(pixels); ++index) {
            pixels[index] = (u8)(255 - index);
        }
        for (size_t index = 0; index < sizeof(palette); ++index) {
            palette[index] = (u8)(index * 3);
        }
        const u8* interned = store->internSixelPalette(palette);
        TerminalCell first{};
        TerminalCell second{};
        store->setSixel(first, pixels, interned);
        store->setSixel(second, pixels, interned);
        Vector<TerminalCell*> cells;
        cells.pushBack(&first);
        cells.pushBack(&second);

        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        const CellExtraView firstView = store->view(first);
        const CellExtraView secondView = store->view(second);
        STD_INSIST(memcmp(firstView.sixelPixels, pixels, sizeof(pixels)) == 0);
        STD_INSIST(memcmp(firstView.sixelPalette, palette, sizeof(palette)) == 0);
        STD_INSIST(firstView.sixelPalette == secondView.sixelPalette);
        STD_INSIST(firstView.sixelPalette != interned);
    }

    STD_TEST(SlotBudgetStaysInsideRefSpace) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        // Whatever the live set grows to, the record budget stays inside
        // the 24-bit extraRef space: append() must never throw before a
        // collection had its chance.
        CellExtraStore& store = *createStore(composer);
        STD_INSIST(store.slotBudget() * 2 <= (size_t)(TerminalCell::maxExtraRef) + 1);
    }

    STD_TEST(ReportsAllocationPressure) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        TerminalCell cell{};

        // Rewriting one cell leaves a trail of dead records: the byte
        // budget asks for a collection first, and only well past it the
        // hard limit; a collection of the one live cell clears both.
        u32 codepoint = 1;
        size_t writes = 0;
        while (!store.shouldCollect() && writes < 1'000'000) {
            store.setGrapheme(cell, &codepoint, 1);
            ++codepoint;
            ++writes;
        }
        STD_INSIST(store.shouldCollect());
        STD_INSIST(!store.hardLimitExceeded());
        const size_t writesToBudget = writes;

        while (!store.hardLimitExceeded() && writes < 1'000'000) {
            store.setGrapheme(cell, &codepoint, 1);
            ++codepoint;
            ++writes;
        }
        STD_INSIST(store.hardLimitExceeded());
        STD_INSIST(writes >= writesToBudget * 2 - 1);

        Vector<TerminalCell*> cells;
        cells.pushBack(&cell);
        store.collect(cells, nullptr, 0);

        STD_INSIST(!store.shouldCollect());
        STD_INSIST(!store.hardLimitExceeded());
        STD_INSIST(store.grapheme(cell).size() == 1);
        STD_INSIST(store.grapheme(cell)[0] == codepoint - 1);
    }

    STD_TEST(TextCollectionLeavesImagesInPlace) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer);
        u8 pixels[SixelPatch::pixelCount] = {9};
        u8 palette[SixelPatch::paletteBytes] = {8};
        const u8* interned = store.internSixelPalette(palette);
        TerminalCell image{};
        store.setSixel(image, pixels, interned);
        const u8* const pixelsBefore = store.view(image).sixelPixels;
        TerminalCell text{};
        u32 codepoint = 1;
        while (!store.shouldCollect()) {
            store.setGrapheme(text, &codepoint, 1);
            ++codepoint;
        }
        Vector<TerminalCell*> cells;
        cells.pushBack(&image);
        cells.pushBack(&text);

        // The text space asked for this collection; the image space did
        // not, so the patch and its palette stay where they are while
        // the records move.
        store.collect(cells, nullptr, 0);

        STD_INSIST(!store.shouldCollect());
        STD_INSIST(store.view(image).sixelPixels == pixelsBefore);
        STD_INSIST(store.view(image).sixelPalette == interned);
        STD_INSIST(memcmp(store.view(image).sixelPixels, pixels, sizeof(pixels)) == 0);
        STD_INSIST(store.grapheme(text).size() == 1);
    }
}
