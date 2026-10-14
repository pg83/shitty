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
    struct ExtraChangeListener final: public CellExtraClient {
        explicit ExtraChangeListener(Composer& composer);

        void extrasCollected() override;

        Composer& composer;
        CellExtraStore* observed = nullptr;
        size_t calls = 0;
    };

    // A client that owns one cell and counts the collections it was asked
    // about, which is how a test tells "the walk reached me" from "the
    // walk happened".
    struct CountingClient final: public CellExtraClient {
        void collectExtras(Vector<TerminalCell*>& cells, Vector<u32*>& roots) override {
            ++collections;
            for (TerminalCell* cell : owned) {
                cells.pushBack(cell);
            }
            for (u32* root : ownedRoots) {
                roots.pushBack(root);
            }
        }

        void own(TerminalCell* cell) {
            owned.pushBack(cell);
        }

        void ownRoot(u32* root) {
            ownedRoots.pushBack(root);
        }

        Vector<TerminalCell*> owned;
        Vector<u32*> ownedRoots;
        size_t collections = 0;
    };

    static bool equal(StringView left, StringView right) {
        return left == right;
    }

    static CellExtraStore* createStore(Composer& composer, size_t cellCount) {
        CellExtraStore* const store = CellExtraStore::create(composer.extras, *composer.pool, cellCount);
        composer.extras.replace(store);
        return store;
    }
}

ExtraChangeListener::ExtraChangeListener(Composer& composer_)
    : composer(composer_)
{
}

void ExtraChangeListener::extrasCollected() {
    observed = composer.extras.store;
    ++calls;
}

STD_TEST_SUITE(CellExtraStore) {
    STD_TEST(FactoryDoesNotReplaceComposerStore) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* const original = composer.extras.store;
        CellExtraStore* const store = CellExtraStore::create(composer.extras, *composer.pool, 1);

        STD_INSIST(store != nullptr);
        STD_INSIST(composer.extras.store == original);
    }

    STD_TEST(KeepsInlineUnderlineColorWithoutExtra) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer, 1);
        TerminalCell cell{};
        const CellColor color = CellColor::direct({17, 34, 51});

        store.setUnderlineColor(cell, color);

        STD_INSIST(!cell.hasExtra());
        STD_INSIST(store.underlineColor(cell) == color);
    }

    STD_TEST(CombinesAndClearsIndependentExtraValues) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer, 1);
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
        CellExtraStore& store = *createStore(composer, 1);
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
        CellExtraStore* store = createStore(composer, 2);
        TerminalCell cell{};
        const u32 grapheme[] = {0x1f469, 0x200d, 0x1f4bb};
        const u32 hyperlink = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 19);
        store->setGrapheme(cell, grapheme, 3);
        store->setHyperlink(cell, hyperlink);
        // R7: the cell reaches the collection the way production reaches
        // it - through a registered client - and not as an argument. A
        // collection that trusted its argument is the defect itself.
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        owner.own(&cell);
        Vector<TerminalCell*> cells;
        CellExtraStore* const previous = store;
        const size_t notificationsBefore = listener.calls;

        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(store != previous);
        STD_INSIST(owner.collections == 1);
        STD_INSIST(listener.calls == notificationsBefore + 1);
        STD_INSIST(listener.observed == store);
        STD_INSIST(cell.hasExtra());
        STD_INSIST(store->grapheme(cell).size() == 3);
        STD_INSIST(store->grapheme(cell)[2] == grapheme[2]);
        STD_INSIST(equal(store->hyperlink(cell), StringView(u8"https://live.test")));
        STD_INSIST(store->hyperlinkDisplayId(cell) == 19);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) != 0);
    }

    // R7-test: the store is one per window, but collect() is handed only
    // the roots of the terminal that started it. A cell belonging to
    // another pane or a background tab is neither migrated nor told its
    // ref moved, and the pool its ref pointed into is deleted at the end
    // of collect(). This reproduces that with two cells standing in for
    // two terminals - the mechanism is the same one VtermImpl::
    // collectCellExtras() reaches, which walks frame_pri/frame_alt of
    // its own terminal only.
    STD_TEST(CollectionLeavesAnotherTerminalsCellsBehind) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 4);
        // "mine" belongs to the terminal running the collection; "theirs"
        // to another pane of the same window.
        TerminalCell mine{};
        TerminalCell theirs{};
        const u32 mineGrapheme[] = {'a', 0x0301};
        const u32 theirsGrapheme[] = {'b', 0x0302, 0x0303};
        store->setGrapheme(mine, mineGrapheme, 2);
        store->setGrapheme(theirs, theirsGrapheme, 3);
        const u32 theirsRefBefore = theirs.extraRef();
        STD_INSIST(store->grapheme(theirs).size() == 3);
        // The other pane, standing where a second terminal stands: it is
        // registered, so the collection asks it too.
        CountingClient other;
        composer.extras.changedListeners.pushBack(&other);
        other.own(&theirs);

        // Only this terminal's cells are collected.
        Vector<TerminalCell*> cells;
        cells.pushBack(&mine);
        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        // What the other pane's cell should still read back as.
        STD_INSIST(theirs.extraRef() == theirsRefBefore);
        STD_INSIST(store->grapheme(theirs).size() == 3);
        STD_INSIST(store->grapheme(theirs)[0] == 'b');
        STD_INSIST(store->grapheme(theirs)[2] == 0x0303);
    }

    // The negative control for the test above: when the collection is
    // handed every live cell - which is what happens when the window has
    // exactly one terminal - the same two cells survive it intact. So the
    // failure above is about who was passed in, not about collect().
    STD_TEST(CollectionKeepsBothCellsWhenBothArePassedIn) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 4);
        TerminalCell mine{};
        TerminalCell theirs{};
        const u32 mineGrapheme[] = {'a', 0x0301};
        const u32 theirsGrapheme[] = {'b', 0x0302, 0x0303};
        store->setGrapheme(mine, mineGrapheme, 2);
        store->setGrapheme(theirs, theirsGrapheme, 3);

        Vector<TerminalCell*> cells;
        cells.pushBack(&mine);
        cells.pushBack(&theirs);
        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(store->grapheme(theirs).size() == 3);
        STD_INSIST(store->grapheme(theirs)[0] == 'b');
        STD_INSIST(store->grapheme(theirs)[2] == 0x0303);
        STD_INSIST(store->grapheme(mine).size() == 2);
    }

    // R7. The client list is what a collection walks, so a registration
    // that outlived its owner turns the next collection into a walk over
    // freed memory - this defect from the other side. Unlinking is
    // therefore the destructor's, exactly as Listener does it, and this
    // is the test that says so.
    //
    // The assertion is on the list itself and not on what a walk reads,
    // deliberately: reading a dead client is undefined, so a test that
    // waited for the crash would pass or fail by luck. Front and back
    // both being the survivor is the same statement with a defined
    // answer.
    STD_TEST(ACollectionDoesNotWalkAClientThatDied) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 2);

        CountingClient survivor;
        composer.extras.changedListeners.pushBack(&survivor);
        {
            CountingClient dying;
            composer.extras.changedListeners.pushBack(&dying);
            STD_INSIST(composer.extras.changedListeners.mutBack() == &dying);
        }
        STD_INSIST(!composer.extras.changedListeners.empty());
        STD_INSIST(composer.extras.changedListeners.mutFront() == &survivor);
        STD_INSIST(composer.extras.changedListeners.mutBack() == &survivor);

        // And the collection that follows is a real one: the survivor is
        // the only source of the cell, so a walk that never reached it
        // would lose the grapheme rather than quietly agree.
        TerminalCell cell{};
        const u32 grapheme[] = {'q', 0x0301};
        store->setGrapheme(cell, grapheme, 2);
        survivor.own(&cell);

        Vector<TerminalCell*> nothing;
        store->collect(nothing, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(survivor.collections == 1);
        STD_INSIST(store->grapheme(cell).size() == 2);
        STD_INSIST(store->grapheme(cell)[1] == 0x0301);
    }

    STD_TEST(CollectionDropsUnreachableHyperlinks) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 1);
        const u32 live = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 1);
        store->getOrCreateHyperlink(StringView(u8"dead"), StringView(u8"https://dead.test"), 2);
        TerminalCell cell{};
        store->setHyperlink(cell, live);
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        owner.own(&cell);
        Vector<TerminalCell*> cells;

        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(owner.collections == 1);
        STD_INSIST(store->hyperlinkCount() == 1);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) != 0);
        STD_INSIST(store->findHyperlink(StringView(u8"dead")) == 0);
    }

    STD_TEST(CollectionPreservesDistinctRefs) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 2);
        TerminalCell first{};
        TerminalCell second{};
        const u32 grapheme[] = {'x', 0x0301};
        store->setGrapheme(first, grapheme, 2);
        store->setGrapheme(second, grapheme, 2);
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        owner.own(&first);
        owner.own(&second);
        Vector<TerminalCell*> cells;

        STD_INSIST(first.extraRef() != second.extraRef());

        store->collect(cells, nullptr, 0);

        STD_INSIST(owner.collections == 1);
        STD_INSIST(first.extraRef() != second.extraRef());
    }

    STD_TEST(CollectionRewritesNonCellRoots) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 1);
        store->getOrCreateHyperlink(StringView(u8"dead"), StringView(u8"https://dead.test"), 1);
        u32 root = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 2);
        // A non-cell ref reaches the collection through its owner too:
        // Vterm's active hyperlink is exactly such a root, and it belongs
        // to no screen.
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        owner.ownRoot(&root);
        Vector<TerminalCell*> cells;

        store->collect(cells, nullptr, 0);
        store = composer.extras.store;

        STD_INSIST(owner.collections == 1);

        TerminalCell cell{};
        store->setHyperlink(cell, root);
        STD_INSIST(root == 1);
        STD_INSIST(store->findHyperlink(StringView(u8"dead")) == 0);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) == root);
        STD_INSIST(store->hyperlink(cell) == StringView(u8"https://live.test"));
    }

    // T5.3. The two doors into collect() are the caller's roots argument
    // and the client walk, and a caller that is also a client goes
    // through both with the same pointer. A cell survives that - a
    // second setExtraRef() under a relocation table is idempotent - but a
    // root does not: the second rewrite indexes the table with the ref
    // the first one just wrote, which is a slot of the *new* store read
    // out of the *old* store's table. The live hyperlink comes back as
    // ref 0, which is how a terminal loses the OSC 8 it had open.
    //
    // The "dead" hyperlink is the fixture and not decoration: it is what
    // makes the live ref move (2 in the old store, 1 in the new one). If
    // the live ref relocated to itself, the doubled rewrite would land
    // on the same answer and this test would pass with the defect in
    // place.
    STD_TEST(ARootReachedThroughBothDoorsIsRewrittenOnce) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 1);
        store->getOrCreateHyperlink(StringView(u8"dead"), StringView(u8"https://dead.test"), 1);
        u32 root = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 2);
        STD_INSIST(root == 2);
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        owner.ownRoot(&root);
        // And the same pointer once more, the way a terminal that starts
        // the collection and is registered for it hands its own roots
        // over.
        u32* const alsoRoots[] = {&root};
        Vector<TerminalCell*> cells;

        store->collect(cells, alsoRoots, 1);
        store = composer.extras.store;

        STD_INSIST(owner.collections == 1);
        STD_INSIST(root == 1);
        STD_INSIST(store->findHyperlink(StringView(u8"dead")) == 0);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) == root);
        TerminalCell cell{};
        store->setHyperlink(cell, root);
        STD_INSIST(store->hyperlink(cell) == StringView(u8"https://live.test"));
    }

    // The negative control for the test above. The same fixture, the
    // same relocation, the same argument - only the client no longer
    // owns the root, so it arrives once. This is what says the failure
    // above is about the duplicate and not about the roots argument
    // being passed at all: no other test in this file passes a non-null
    // roots array, so without this one a red would not distinguish the
    // two.
    STD_TEST(ARootReachedOnlyThroughTheArgumentIsRewrittenToo) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore* store = createStore(composer, 1);
        store->getOrCreateHyperlink(StringView(u8"dead"), StringView(u8"https://dead.test"), 1);
        u32 root = store->getOrCreateHyperlink(StringView(u8"live"), StringView(u8"https://live.test"), 2);
        STD_INSIST(root == 2);
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        u32* const roots[] = {&root};
        Vector<TerminalCell*> cells;

        store->collect(cells, roots, 1);
        store = composer.extras.store;

        STD_INSIST(owner.collections == 1);
        STD_INSIST(root == 1);
        STD_INSIST(store->findHyperlink(StringView(u8"live")) == root);
    }

    STD_TEST(SixelPatchSharesPaletteAcrossCells) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer, 2);
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
        CellExtraStore& store = *createStore(composer, 1);
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
        CellExtraStore& store = *createStore(composer, 1);
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
        CellExtraStore* store = createStore(composer, 2);
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
        CountingClient owner;
        composer.extras.changedListeners.pushBack(&owner);
        owner.own(&first);
        owner.own(&second);
        Vector<TerminalCell*> cells;

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
        // 200 columns x 64k saveLines: the linear x10 budget would pass the
        // 24-bit extraRef space and append() would throw before collection.
        CellExtraStore& store = *createStore(composer, (size_t)(200) * 65'536);
        STD_INSIST(store.slotBudget() * 2 <= (size_t)(TerminalCell::maxExtraRef) + 1);
    }

    STD_TEST(ReportsAllocationPressure) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CellExtraStore& store = *createStore(composer, 1);
        TerminalCell cell{};

        for (u32 codepoint = 1; codepoint <= 17; ++codepoint) {
            store.setGrapheme(cell, &codepoint, 1);
        }

        STD_INSIST(store.shouldCollect());
        STD_INSIST(!store.hardLimitExceeded());

        for (u32 codepoint = 18; codepoint <= 33; ++codepoint) {
            store.setGrapheme(cell, &codepoint, 1);
        }

        STD_INSIST(store.hardLimitExceeded());
    }
}
