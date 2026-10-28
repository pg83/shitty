/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "glyph_cache.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/sym/i_map.h>

using namespace stl;

namespace {

    // The memo earns its keep on text that never repeats: every distinct
    // glyph must rasterize once, so the budget holds the largest set one
    // script can show at once - the ideographs and Hangul syllables of
    // the BMP, 32k glyphs - at a typical retina glyph of 32x32 coverage
    // bytes. Bigger glyphs fit fewer before overflow, which drops the
    // whole arena, semi-space style: entries are epoch-checked, never
    // removed one by one.
    constexpr size_t repertoireGlyphs = 32768;
    constexpr size_t typicalGlyphBytes = 32 * 32;
    constexpr size_t pixelBudget = repertoireGlyphs * typicalGlyphBytes;

    struct Entry {
        u32 offset;
        u32 epoch;
        u16 width;
        u16 height;
        i16 left;
        i16 top;
    };

    struct GlyphCacheImpl final: public GlyphCache {
        explicit GlyphCacheImpl(ObjPool& pool);

        u32 makeNamespace() override;
        bool find(u64 key, GlyphStrike& strike) override;
        void insert(u64 key, const GlyphStrike& strike) override;
        void clear() override;

        IntMap<Entry>* entries_;
        Buffer pixels_;
        u32 epoch_ = 0;
        u32 namespaces_ = 0;
    };

}

GlyphCacheImpl::GlyphCacheImpl(ObjPool& pool)
    : entries_(pool.make<IntMap<Entry>>(&pool))
{
}

u32 GlyphCacheImpl::makeNamespace() {
    return ++namespaces_;
}

bool GlyphCacheImpl::find(u64 key, GlyphStrike& strike) {
    const Entry* const entry = entries_->find(key);
    if (entry == nullptr || entry->epoch != epoch_) {
        return false;
    }
    strike.data = (const u8*)(pixels_.data()) + entry->offset;
    strike.width = entry->width;
    strike.height = entry->height;
    strike.left = entry->left;
    strike.top = entry->top;
    return true;
}

void GlyphCacheImpl::insert(u64 key, const GlyphStrike& strike) {
    const size_t bytes = (size_t)(strike.width) * strike.height;
    if (bytes > pixelBudget) {
        return;
    }
    if (pixels_.used() + bytes > pixelBudget) {
        clear();
    }
    const size_t offset = pixels_.used();
    pixels_.grow(offset + bytes);
    pixels_.seekAbsolute(offset + bytes);
    __builtin_memcpy((u8*)(pixels_.mutData()) + offset, strike.data, bytes);
    entries_->insert(key, (u32)(offset), epoch_, strike.width, strike.height, strike.left, strike.top);
}

void GlyphCacheImpl::clear() {
    pixels_.reset();
    ++epoch_;
}

GlyphCache* createGlyphCache(ObjPool& pool) {
    return pool.make<GlyphCacheImpl>(pool);
}
