/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "font_embedded.h"

#include "composer.h"
#include "font_coverage.h"
#include "font_data.h"
#include "font_face.h"
#include "font_resolver.h"

#include <std/mem/obj_pool.h>

using namespace stl;

namespace {
    // The bytes live in the binary, so a face outlives every consumer and
    // the count is a no-op.
    struct StaticFontFace final: public FontFace {
        StaticFontFace(const void* data, size_t size);

        u32 id() const noexcept override;
        const void* data() const override;
        size_t size() const override;
        i32 faceIndex() const override;

        void ref() noexcept override;
        i32 unref() noexcept override;
        i32 refCount() const noexcept override;

        const u32 id_ = nextFontFaceId();
        const void* data_;
        size_t size_;
    };

    struct EmbeddedFontResolverImpl final: public FontResolver {
        FontFace* resolve(const FontRequest& request) override;
        FontFace* resolveCluster(const u32* codepoints, size_t count, FontPlane plane) override;
    };
}

StaticFontFace::StaticFontFace(const void* data, size_t size)
    : data_(data)
    , size_(size)
{
}

u32 StaticFontFace::id() const noexcept {
    return id_;
}

const void* StaticFontFace::data() const {
    return data_;
}

size_t StaticFontFace::size() const {
    return size_;
}

i32 StaticFontFace::faceIndex() const {
    return 0;
}

void StaticFontFace::ref() noexcept {
}

i32 StaticFontFace::unref() noexcept {
    return 1;
}

i32 StaticFontFace::refCount() const noexcept {
    return 2;
}

namespace {
    static StaticFontFace embeddedMono(embeddedFontMono.data, embeddedFontMono.size);
    static StaticFontFace embeddedEmoji(embeddedFontEmoji.data, embeddedFontEmoji.size);
    static StaticFontFace embeddedEmojiText(embeddedFontEmojiText.data, embeddedFontEmojiText.size);
}

FontFace* EmbeddedFontResolverImpl::resolve(const FontRequest& request) {
    if (request.style != FontStyle::Regular) {
        return nullptr;
    }
    return &embeddedMono;
}

namespace {
    // Bits follow the generator's argument order.
    constexpr u8 coversEmoji = 0x1;
    constexpr u8 coversMono = 0x2;
    constexpr u8 coversEmojiText = 0x4;

    static u8 embeddedCoverage(u32 codepoint) {
        constexpr size_t count = sizeof(embeddedCoverageRanges) / sizeof(embeddedCoverageRanges[0]);
        size_t lo = 0;
        size_t hi = count;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (embeddedCoverageRanges[mid].last < codepoint) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == count || embeddedCoverageRanges[lo].first > codepoint) {
            return 0;
        }
        return embeddedCoverageRanges[lo].faces;
    }
}

// The generated coverage table answers without opening a face: which of
// the three embedded fonts, if any, serves the whole cluster. The color
// emoji leads unless the cluster explicitly asks for the text plane.
FontFace* EmbeddedFontResolverImpl::resolveCluster(const u32* codepoints, size_t count, FontPlane plane) {
    u8 mask = coversEmoji | coversMono | coversEmojiText;
    for (size_t index = 0; index < count && mask != 0; ++index) {
        mask &= embeddedCoverage(codepoints[index]);
    }
    if (mask == 0) {
        return nullptr;
    }
    if (plane == FontPlane::Mask) {
        if (mask & coversMono) {
            return &embeddedMono;
        }
        if (mask & coversEmojiText) {
            return &embeddedEmojiText;
        }
        return &embeddedEmoji;
    }
    if (mask & coversEmoji) {
        return &embeddedEmoji;
    }
    if (mask & coversMono) {
        return &embeddedMono;
    }
    return &embeddedEmojiText;
}

FontResolver* createEmbeddedFontResolver(Composer& composer) {
    return composer.pool->make<EmbeddedFontResolverImpl>();
}

void embeddedMonoFont(const void*& data, unsigned long& size) {
    data = embeddedFontMono.data;
    size = embeddedFontMono.size;
}
