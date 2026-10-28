/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "glyph_cache.h"

#include <std/mem/obj_pool.h>
#include <std/tst/ut.h>

using namespace stl;

STD_TEST_SUITE(GlyphCache) {
    STD_TEST(RoundTripsAStrike) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        GlyphCache* const cache = createGlyphCache(*pool);
        const u8 pixels[6] = {1, 2, 3, 4, 5, 6};
        GlyphStrike strike;
        STD_INSIST(!cache->find(42, strike));
        cache->insert(42, {pixels, 3, 2, -1, 7});
        STD_INSIST(cache->find(42, strike));
        STD_INSIST(strike.width == 3);
        STD_INSIST(strike.height == 2);
        STD_INSIST(strike.left == -1);
        STD_INSIST(strike.top == 7);
        STD_INSIST(strike.data[0] == 1);
        STD_INSIST(strike.data[5] == 6);
        STD_INSIST(strike.data != pixels);
    }

    STD_TEST(ClearDropsEveryEntry) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        GlyphCache* const cache = createGlyphCache(*pool);
        const u8 pixels[1] = {9};
        cache->insert(1, {pixels, 1, 1, 0, 0});
        cache->clear();
        GlyphStrike strike;
        STD_INSIST(!cache->find(1, strike));
        cache->insert(1, {pixels, 1, 1, 0, 0});
        STD_INSIST(cache->find(1, strike));
    }

    STD_TEST(NamespacesAreUnique) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        GlyphCache* const cache = createGlyphCache(*pool);
        STD_INSIST(cache->makeNamespace() != cache->makeNamespace());
    }

    STD_TEST(BudgetOverflowRestartsTheArena) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        GlyphCache* const cache = createGlyphCache(*pool);
        // 64k per strike: 512 of them cross the 32 MiB budget.
        static u8 pixels[64 * 1024];
        pixels[0] = 17;
        for (u64 key = 0; key < 513; ++key) {
            cache->insert(key, {pixels, 256, 256, 0, 0});
        }
        GlyphStrike strike;
        STD_INSIST(!cache->find(0, strike));
        STD_INSIST(cache->find(512, strike));
        STD_INSIST(strike.data[0] == 17);
    }
}
