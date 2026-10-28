/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "cell_extra_store.h"

#include <lib/vterm/fatal.h>
#include <lib/vterm/listener.h>

#include <std/lib/list.h>
#include <std/str/view.h>
#include <std/sym/i_map.h>
#include <std/alg/xchg.h>
#include <std/alg/minmax.h>
#include <std/dbg/assert.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>

#include <new>
#include <string.h>

using namespace stl;

namespace {
    struct HyperlinkHandle final: IntrusiveNode {
        StringView identity;
        StringView payload;
        u32 displayId = 0;
        u32 identityNext = 0;

        HyperlinkHandle(StringView identity_, StringView payload_, u32 displayId_) noexcept;
    };

    struct CellExtra {
        IntrusiveList* hyperlinks = nullptr;
        StringView graphemeBytes;
        const u8* sixelPixels = nullptr;
        const u8* sixelPalette = nullptr;
        CellColor underlineColor = CellColor::defaultForeground();
    };

    static_assert(__is_trivially_copyable(CellExtra), "CellExtra must remain trivially copyable");

    // Every allocation is rounded to this, so records and handles stay
    // naturally aligned wherever they land in a chunk.
    constexpr size_t extraAlignment = 8;

    // Every arena is built from buffers of this one size, so a buffer any
    // arena hands back fits any arena that asks next. Nothing a cell can
    // hold comes near it - a palette is 768 bytes, a cluster at most 256.
    constexpr size_t bufferBytes = 128 * 1024;

    // The store's free list of buffers, shared by its three spaces and
    // every generation of each: an arena takes buffers here and returns
    // them when it is released, so after warm-up a collection moves no
    // memory to or from the system at all. Memory peaks once, at the
    // largest live-plus-garbage moment, and stays at that.
    struct BufferList {
        explicit BufferList(ObjPool& pool);

        Buffer* take();
        void give(Buffer* buffer);

        ObjPool& pool_;
        Vector<Buffer*> free_;
    };

    // One generation of one space: a bump allocator over buffers taken
    // from the list. A hit is a pointer bump; exhausting the window takes
    // the next buffer. A request larger than a buffer gets one of its own
    // size, freed rather than pooled on release - it would never fit a
    // later arena's needs. The byte budget is advisory here: a feed that
    // cannot reach a safe point keeps writing, and the arena keeps
    // taking buffers.
    struct Arena {
        explicit Arena(BufferList& buffers);

        u8* allocate(size_t bytes);
        void release() noexcept;
        void xchg(Arena& other) noexcept;
        size_t used() const noexcept;

        BufferList& buffers_;
        Vector<Buffer*> taken_;
        Vector<Buffer*> oversized_;
        u8* cursor_ = nullptr;
        u8* end_ = nullptr;
        size_t used_ = 0;

        u8* allocateSlow(size_t bytes);
    };

    // The garbage a space may accumulate per byte of live content before
    // it collects. A collection copies the live set once, so this is the
    // one tradeoff a copying collector has: 1 would bound memory at
    // twice the live set and spend one byte of copying per byte written;
    // 4 holds five times the live set and spends a quarter of that -
    // measured as the point where a clustered-text flood stops paying
    // for its collections.
    constexpr size_t garbagePerLive = 4;

    // One kind of extra content with its own byte budget. Content lives
    // in the current arena; a collection fills a fresh arena with the
    // live part and releases the old one back to the list. The budget
    // is the garbage a collection's cost amortizes - garbagePerLive
    // times the live set after the last turn - and never less than a
    // floor, below which no collection is worth its fixed cost. Twice
    // the budget is the hard limit a feed may not ride past a safe
    // point.
    struct ExtraSpace {
        explicit ExtraSpace(BufferList& buffers);

        // cap keeps the budget inside an external limit (the ref space
        // for records).
        void configure(size_t floor, size_t cap);
        u8* allocate(size_t bytes);
        // During a collection: the destination arena, fresh at begin().
        void begin();
        u8* allocateNext(size_t bytes);
        void turn();
        bool overBudget() const noexcept;
        bool overHardLimit() const noexcept;
        size_t budget() const noexcept;

        Arena current_;
        Arena next_;
        size_t floor_ = 4096;
        size_t cap_ = ~(size_t)(0);
        size_t budget_ = 4096;
    };

    struct CellExtraStoreImpl final: public CellExtraStore {
        CellExtraStoreImpl(VtCellExtras& extras, ObjPool& pool);

        CellExtraView view(const TerminalCell& cell) const noexcept override;
        CellColor underlineColor(const TerminalCell& cell) const noexcept override;
        GraphemeView grapheme(const TerminalCell& cell) const noexcept override;
        GraphemeView grapheme(u32 ref) const noexcept override;
        StringView hyperlink(const TerminalCell& cell) const noexcept override;
        u32 hyperlinkDisplayId(const TerminalCell& cell) const noexcept override;

        u32 getOrCreateHyperlink(StringView identity, StringView payload, u32 displayId) override;
        u32 findHyperlink(StringView identity) const noexcept override;
        size_t hyperlinkCount() const noexcept override;

        void setUnderlineColor(TerminalCell& cell, CellColor color) override;
        void setGrapheme(TerminalCell& cell, const u32* codepoints, size_t count) override;
        void clearGrapheme(TerminalCell& cell) override;
        const u8* internSixelPalette(const u8* palette) override;
        void setSixel(TerminalCell& cell, const u8* pixels, const u8* palette) override;
        void setHyperlink(TerminalCell& cell, u32 hyperlinkRef) override;
        void clearHyperlink(TerminalCell& cell) override;
        void clearExtra(TerminalCell& cell, CellColor underlineColor) override;

        size_t slotBudget() const noexcept override;
        bool shouldCollect() const noexcept override;
        bool hardLimitExceeded() const noexcept override;
        void collect(Vector<TerminalCell*>& cells, u32* const* roots, size_t rootCount) override;

        VtCellExtras& extras_;
        Vector<CellExtra*> slots_;
        // OSC 8 identity lookup only. This is not a CellExtra content index:
        // append() always creates a new dense ref.
        Vector<u32> hyperlinkBuckets_;
        size_t hyperlinkCount_ = 0;
        BufferList buffers_;
        // Records and grapheme codepoints churn together, one record per
        // clustered cell written; hyperlinks and sixel patches live as
        // long as their page or image stays on screen. Three spaces, so a
        // text flood collects its own churn without copying an image.
        ExtraSpace text_;
        ExtraSpace links_;
        ExtraSpace images_;

        const CellExtra* get(u32 ref) const noexcept;
        void finishCollection() noexcept;

        static GraphemeView graphemeOf(const CellExtra& extra) noexcept;
        static HyperlinkHandle* hyperlinkOf(CellExtra& extra) noexcept;
        static const HyperlinkHandle* hyperlinkOf(const CellExtra& extra) noexcept;

        u32 append(const CellExtra& extra);
        u32 appendRecord(CellExtra* extra);
        void indexHyperlink(u32 ref);
        void rehashHyperlinks(size_t capacity);
        StringView copyBytes(ExtraSpace& space, StringView value);
        StringView copyBytesNext(ExtraSpace& space, StringView value);
        IntrusiveList* makeHyperlinkList(ExtraSpace& space, bool next);
        HyperlinkHandle* makeHyperlinkHandle(ExtraSpace& space, bool next, StringView identity, StringView payload, u32 displayId);
    };

    static bool equalBytes(StringView left, StringView right) noexcept {
        return left.length() == right.length() && (left.empty() || memcmp(left.data(), right.data(), left.length()) == 0);
    }

    static size_t alignUp(size_t bytes) noexcept {
        return (bytes + extraAlignment - 1) & ~(extraAlignment - 1);
    }

    // The 24-bit extraRef space caps the record budget: every record costs
    // at least sizeof(CellExtra) of text space, so a text space at its
    // hard limit - twice the budget - still holds fewer records than
    // the ref space.
    static constexpr size_t slotBudgetCeiling = (TerminalCell::maxExtraRef + 1) / 2;
    static constexpr size_t textBudgetCap = slotBudgetCeiling * sizeof(CellExtra) / 2;
}

HyperlinkHandle::HyperlinkHandle(StringView identity_, StringView payload_, u32 displayId_) noexcept
    : identity(identity_)
    , payload(payload_)
    , displayId(displayId_)
{
}

BufferList::BufferList(ObjPool& pool)
    : pool_(pool)
{
}

Buffer* BufferList::take() {
    if (free_.length() != 0) {
        Buffer* const buffer = free_[free_.length() - 1];
        free_.popBack();
        return buffer;
    }
    Buffer* const buffer = pool_.make<Buffer>();
    buffer->grow(bufferBytes);
    return buffer;
}

void BufferList::give(Buffer* buffer) {
    free_.pushBack(buffer);
}

Arena::Arena(BufferList& buffers)
    : buffers_(buffers)
{
}

u8* Arena::allocate(size_t bytes) {
    bytes = alignUp(bytes);
    if ((size_t)(end_ - cursor_) < bytes) {
        return allocateSlow(bytes);
    }
    u8* const result = cursor_;
    cursor_ += bytes;
    used_ += bytes;
    return result;
}

u8* Arena::allocateSlow(size_t bytes) {
    used_ += bytes;
    if (bytes > bufferBytes) {
        Buffer* const buffer = buffers_.pool_.make<Buffer>();
        buffer->grow(bytes);
        oversized_.pushBack(buffer);
        return (u8*)(buffer->mutData());
    }
    // A buffer's whole capacity is the window; its own cursor stays at
    // zero and is never consulted.
    Buffer* const buffer = buffers_.take();
    taken_.pushBack(buffer);
    cursor_ = (u8*)(buffer->mutData());
    end_ = cursor_ + buffer->capacity();
    u8* const result = cursor_;
    cursor_ += bytes;
    return result;
}

void Arena::release() noexcept {
    for (Buffer* buffer : taken_) {
        buffers_.give(buffer);
    }
    taken_.clear();
    for (Buffer* buffer : oversized_) {
        // Nothing was ever appended, so this shrinks to an empty buffer
        // and frees the oversized storage.
        buffer->shrinkToFit();
    }
    oversized_.clear();
    cursor_ = nullptr;
    end_ = nullptr;
    used_ = 0;
}

void Arena::xchg(Arena& other) noexcept {
    taken_.xchg(other.taken_);
    oversized_.xchg(other.oversized_);
    stl::xchg(cursor_, other.cursor_);
    stl::xchg(end_, other.end_);
    stl::xchg(used_, other.used_);
}

size_t Arena::used() const noexcept {
    return used_;
}

ExtraSpace::ExtraSpace(BufferList& buffers)
    : current_(buffers)
    , next_(buffers)
{
}

void ExtraSpace::configure(size_t floor, size_t cap) {
    floor_ = floor;
    cap_ = cap;
    budget_ = min(max(floor_, current_.used() * (1 + garbagePerLive)), cap_);
}

u8* ExtraSpace::allocate(size_t bytes) {
    return current_.allocate(bytes);
}

void ExtraSpace::begin() {
    next_.release();
}

u8* ExtraSpace::allocateNext(size_t bytes) {
    return next_.allocate(bytes);
}

void ExtraSpace::turn() {
    current_.release();
    current_.xchg(next_);
    budget_ = min(max(floor_, current_.used() * (1 + garbagePerLive)), cap_);
}

bool ExtraSpace::overBudget() const noexcept {
    return current_.used() > budget_;
}

bool ExtraSpace::overHardLimit() const noexcept {
    return current_.used() > budget_ * 2;
}

size_t ExtraSpace::budget() const noexcept {
    return budget_;
}

CellExtraStoreImpl::CellExtraStoreImpl(VtCellExtras& extras, ObjPool& pool)
    : extras_(extras)
    , buffers_(pool)
    , text_(buffers_)
    , links_(buffers_)
    , images_(buffers_)
{
    slots_.pushBack(nullptr);
    rehashHyperlinks(16);
    // The floors are what an empty store may accumulate before its first
    // collection: a modest page of clustered text, a few links, one
    // screenful of sixel patches. Nothing here scales with the grid or
    // the scrollback - a collection walks only rows that may hold
    // extras and copies only live content, so its cost, and with it the
    // garbage worth amortizing it over, follows the live set alone.
    text_.configure(1024 * 1024, textBudgetCap);
    links_.configure(256 * 1024, ~(size_t)(0));
    images_.configure(4 * 1024 * 1024, ~(size_t)(0));
}

size_t CellExtraStoreImpl::hyperlinkCount() const noexcept {
    return hyperlinkCount_;
}

const CellExtra* CellExtraStoreImpl::get(u32 ref) const noexcept {
    if (ref == 0 || ref >= slots_.length()) {
        return nullptr;
    }
    return slots_[ref];
}

GraphemeView CellExtraStoreImpl::graphemeOf(const CellExtra& extra) noexcept {
    return {
        reinterpret_cast<const u32*>(extra.graphemeBytes.data()),
        static_cast<u32>(extra.graphemeBytes.length() / sizeof(u32)),
    };
}

const HyperlinkHandle* CellExtraStoreImpl::hyperlinkOf(const CellExtra& extra) noexcept {
    if (extra.hyperlinks == nullptr) {
        return nullptr;
    }
    return static_cast<const HyperlinkHandle*>(extra.hyperlinks->front());
}

HyperlinkHandle* CellExtraStoreImpl::hyperlinkOf(CellExtra& extra) noexcept {
    if (extra.hyperlinks == nullptr) {
        return nullptr;
    }
    return static_cast<HyperlinkHandle*>(extra.hyperlinks->mutFront());
}

StringView CellExtraStoreImpl::copyBytes(ExtraSpace& space, StringView value) {
    if (value.empty()) {
        return {};
    }
    u8* const bytes = space.allocate(value.length());
    memcpy(bytes, value.data(), value.length());
    return StringView(bytes, value.length());
}

StringView CellExtraStoreImpl::copyBytesNext(ExtraSpace& space, StringView value) {
    if (value.empty()) {
        return {};
    }
    u8* const bytes = space.allocateNext(value.length());
    memcpy(bytes, value.data(), value.length());
    return StringView(bytes, value.length());
}

IntrusiveList* CellExtraStoreImpl::makeHyperlinkList(ExtraSpace& space, bool next) {
    u8* const bytes = next ? space.allocateNext(sizeof(IntrusiveList)) : space.allocate(sizeof(IntrusiveList));
    return new (bytes) IntrusiveList;
}

HyperlinkHandle* CellExtraStoreImpl::makeHyperlinkHandle(ExtraSpace& space, bool next, StringView identity, StringView payload, u32 displayId) {
    u8* const bytes = next ? space.allocateNext(sizeof(HyperlinkHandle)) : space.allocate(sizeof(HyperlinkHandle));
    return new (bytes) HyperlinkHandle(identity, payload, displayId);
}

void CellExtraStoreImpl::rehashHyperlinks(size_t capacity) {
    Vector<u32> replacement(capacity);
    replacement.zero(capacity);
    for (u32 head : hyperlinkBuckets_) {
        for (u32 ref = head; ref != 0;) {
            CellExtra* extra = slots_[ref];
            HyperlinkHandle* hyperlink = hyperlinkOf(*extra);
            const u32 next = hyperlink->identityNext;
            const size_t bucket = hyperlink->identity.hash64() % capacity;
            hyperlink->identityNext = replacement[bucket];
            replacement.mut(bucket) = ref;
            ref = next;
        }
    }
    hyperlinkBuckets_.xchg(replacement);
}

void CellExtraStoreImpl::indexHyperlink(u32 ref) {
    if ((hyperlinkCount_ + 1) * 10 >= hyperlinkBuckets_.length() * 7) {
        rehashHyperlinks(hyperlinkBuckets_.length() * 2);
    }
    ++hyperlinkCount_;
    HyperlinkHandle* hyperlink = hyperlinkOf(*slots_[ref]);
    const size_t bucket = hyperlink->identity.hash64() % hyperlinkBuckets_.length();
    hyperlink->identityNext = hyperlinkBuckets_[bucket];
    hyperlinkBuckets_.mut(bucket) = ref;
}

u32 CellExtraStoreImpl::appendRecord(CellExtra* extra) {
    if (slots_.length() > TerminalCell::maxExtraRef) {
        raiseError(StringView(u8"cell extra store slot budget exhausted"));
    }
    const u32 ref = static_cast<u32>(slots_.length());
    slots_.pushBack(extra);
    return ref;
}

u32 CellExtraStoreImpl::append(const CellExtra& value) {
    STD_ASSERT(!value.graphemeBytes.empty() || hyperlinkOf(value) != nullptr || value.sixelPixels != nullptr);
    auto* extra = new (text_.allocate(sizeof(CellExtra))) CellExtra(value);
    return appendRecord(extra);
}

CellColor CellExtraStoreImpl::underlineColor(const TerminalCell& cell) const noexcept {
    if (!cell.hasExtra()) {
        return cell.inlineUnderlineColor();
    }
    return get(cell.extraRef())->underlineColor;
}

CellExtraView CellExtraStoreImpl::view(const TerminalCell& cell) const noexcept {
    if (!cell.hasExtra()) {
        return {
            .underlineColor = cell.inlineUnderlineColor(),
        };
    }
    const CellExtra* const extra = get(cell.extraRef());
    const HyperlinkHandle* const hyperlink = hyperlinkOf(*extra);
    return {
        .underlineColor = extra->underlineColor,
        .grapheme = graphemeOf(*extra),
        .hyperlinkDisplayId = hyperlink == nullptr ? 0 : hyperlink->displayId,
        .sixelPixels = extra->sixelPixels,
        .sixelPalette = extra->sixelPalette,
    };
}

GraphemeView CellExtraStoreImpl::grapheme(const TerminalCell& cell) const noexcept {
    return cell.hasExtra() ? grapheme(cell.extraRef()) : GraphemeView{};
}

GraphemeView CellExtraStoreImpl::grapheme(u32 ref) const noexcept {
    const CellExtra* extra = get(ref);
    return extra == nullptr ? GraphemeView{} : graphemeOf(*extra);
}

StringView CellExtraStoreImpl::hyperlink(const TerminalCell& cell) const noexcept {
    if (!cell.hasExtra()) {
        return {};
    }
    const HyperlinkHandle* handle = hyperlinkOf(*get(cell.extraRef()));
    return handle == nullptr ? StringView{} : handle->payload;
}

u32 CellExtraStoreImpl::hyperlinkDisplayId(const TerminalCell& cell) const noexcept {
    if (!cell.hasExtra()) {
        return 0;
    }
    const HyperlinkHandle* handle = hyperlinkOf(*get(cell.extraRef()));
    return handle == nullptr ? 0 : handle->displayId;
}

u32 CellExtraStoreImpl::findHyperlink(StringView identity) const noexcept {
    if (identity.empty()) {
        return 0;
    }
    const size_t bucket = identity.hash64() % hyperlinkBuckets_.length();
    for (u32 ref = hyperlinkBuckets_[bucket]; ref != 0;) {
        const HyperlinkHandle* handle = hyperlinkOf(*slots_[ref]);
        if (equalBytes(handle->identity, identity)) {
            return ref;
        }
        ref = handle->identityNext;
    }
    return 0;
}

u32 CellExtraStoreImpl::getOrCreateHyperlink(StringView identity, StringView payload, u32 displayId) {
    if (const u32 existing = findHyperlink(identity); existing != 0) {
        return existing;
    }

    CellExtra extra;
    extra.hyperlinks = makeHyperlinkList(links_, false);
    const StringView storedIdentity = copyBytes(links_, identity);
    const StringView storedPayload = copyBytes(links_, payload);
    extra.hyperlinks->pushBack(makeHyperlinkHandle(links_, false, storedIdentity, storedPayload, displayId));

    const u32 ref = append(extra);
    indexHyperlink(ref);
    return ref;
}

void CellExtraStoreImpl::setUnderlineColor(TerminalCell& cell, CellColor color) {
    if (!cell.hasExtra()) {
        cell.setInlineUnderlineColor(color);
        return;
    }

    CellExtra copy = *get(cell.extraRef());
    copy.underlineColor = color;
    cell.setExtraRef(append(copy));
}

void CellExtraStoreImpl::setGrapheme(TerminalCell& cell, const u32* codepoints, size_t count) {
    if (count == 0) {
        clearGrapheme(cell);
        return;
    }

    CellExtra copy;
    if (cell.hasExtra()) {
        copy = *get(cell.extraRef());
    } else {
        copy.underlineColor = cell.inlineUnderlineColor();
    }
    copy.graphemeBytes = copyBytes(text_, StringView(reinterpret_cast<const u8*>(codepoints), count * sizeof(u32)));
    copy.sixelPixels = nullptr;
    copy.sixelPalette = nullptr;
    cell.setExtraRef(append(copy));
}

void CellExtraStoreImpl::clearGrapheme(TerminalCell& cell) {
    if (!cell.hasExtra()) {
        return;
    }

    const CellExtra& current = *get(cell.extraRef());
    if (current.graphemeBytes.empty()) {
        return;
    }
    if (hyperlinkOf(current) == nullptr && current.sixelPixels == nullptr) {
        cell.setInlineUnderlineColor(current.underlineColor);
        return;
    }

    CellExtra copy = current;
    copy.graphemeBytes = {};
    cell.setExtraRef(append(copy));
}

const u8* CellExtraStoreImpl::internSixelPalette(const u8* palette) {
    return copyBytes(images_, StringView(palette, SixelPatch::paletteBytes)).data();
}

void CellExtraStoreImpl::setSixel(TerminalCell& cell, const u8* pixels, const u8* palette) {
    STD_ASSERT(pixels != nullptr && palette != nullptr);

    CellExtra copy;
    if (cell.hasExtra()) {
        copy = *get(cell.extraRef());
    } else {
        copy.underlineColor = cell.inlineUnderlineColor();
    }
    copy.graphemeBytes = {};
    copy.sixelPixels = copyBytes(images_, StringView(pixels, SixelPatch::pixelCount)).data();
    copy.sixelPalette = palette;
    cell.setExtraRef(append(copy));
}

void CellExtraStoreImpl::setHyperlink(TerminalCell& cell, u32 hyperlinkRef) {
    if (hyperlinkRef == 0) {
        clearHyperlink(cell);
        return;
    }
    const CellExtra& hyperlinkExtra = *get(hyperlinkRef);
    if (hyperlinkOf(hyperlinkExtra) == nullptr) {
        clearHyperlink(cell);
        return;
    }

    CellExtra copy;
    if (cell.hasExtra()) {
        const CellExtra& current = *get(cell.extraRef());
        if (current.hyperlinks == hyperlinkExtra.hyperlinks) {
            return;
        }
        copy = current;
    } else {
        copy.underlineColor = cell.inlineUnderlineColor();
        if (hyperlinkExtra.graphemeBytes.empty() && hyperlinkExtra.underlineColor == copy.underlineColor) {
            cell.setExtraRef(hyperlinkRef);
            return;
        }
    }
    copy.hyperlinks = hyperlinkExtra.hyperlinks;
    cell.setExtraRef(append(copy));
}

void CellExtraStoreImpl::clearHyperlink(TerminalCell& cell) {
    if (!cell.hasExtra()) {
        return;
    }

    const CellExtra& current = *get(cell.extraRef());
    if (hyperlinkOf(current) == nullptr) {
        return;
    }
    if (current.graphemeBytes.empty() && current.sixelPixels == nullptr) {
        cell.setInlineUnderlineColor(current.underlineColor);
        return;
    }

    CellExtra copy = current;
    copy.hyperlinks = nullptr;
    cell.setExtraRef(append(copy));
}

void CellExtraStoreImpl::clearExtra(TerminalCell& cell, CellColor underlineColor_) {
    cell.setInlineUnderlineColor(underlineColor_);
}

size_t CellExtraStoreImpl::slotBudget() const noexcept {
    return min(text_.budget() / sizeof(CellExtra), slotBudgetCeiling);
}

bool CellExtraStoreImpl::shouldCollect() const noexcept {
    return text_.overBudget() || links_.overBudget() || images_.overBudget();
}

bool CellExtraStoreImpl::hardLimitExceeded() const noexcept {
    return text_.overHardLimit() || links_.overHardLimit() || images_.overHardLimit();
}

void CellExtraStoreImpl::collect(Vector<TerminalCell*>& cells, u32* const* roots, size_t rootCount) {
    STD_ASSERT(extras_.store == this);

    // A collection a budget asked for touches the spaces over budget; one
    // nobody asked for - a forced or explicit one - turns all three.
    const bool asked = shouldCollect();
    const bool text = !asked || text_.overBudget();
    const bool links = !asked || links_.overBudget();
    const bool images = !asked || images_.overBudget();

    // The dedup maps of this pass: one new list per shared hyperlink
    // list, one new block per shared palette. They die with the pass.
    ObjPool::Ref scratch = ObjPool::fromMemory();
    IntMap<IntrusiveList*> lists(scratch.mutPtr());
    IntMap<const u8*> palettes(scratch.mutPtr());

    // Turning the text space renumbers the refs: the old slot table is
    // read, a fresh one is built, and relocation maps old to new. The
    // other spaces patch records in place, and relocation then only
    // marks a record as done - two cells sharing a ref must not copy its
    // payload twice.
    Vector<CellExtra*> oldSlots;
    Vector<u32> relocation;
    if (text) {
        oldSlots.xchg(slots_);
        slots_.pushBack(nullptr);
        relocation.zero(oldSlots.length());
        text_.begin();
    } else {
        relocation.zero(slots_.length());
    }
    if (links) {
        links_.begin();
    }
    if (text || links) {
        // The identity index is rebuilt from the live set: the refs or the
        // handles move, and the dead ones must not be found again.
        hyperlinkBuckets_.clear();
        rehashHyperlinks(16);
        hyperlinkCount_ = 0;
    }
    if (images) {
        images_.begin();
    }

    const auto relocateList = [&](IntrusiveList* source) -> IntrusiveList* {
        const u64 key = (u64)(uintptr_t)(source);
        if (IntrusiveList** known = lists.find(key); known != nullptr) {
            return *known;
        }
        IntrusiveList* const fresh = makeHyperlinkList(links_, true);
        for (const IntrusiveNode* node = source->front(); node != source->end(); node = node->next) {
            const auto* handle = static_cast<const HyperlinkHandle*>(node);
            const StringView identity = copyBytesNext(links_, handle->identity);
            const StringView payload = copyBytesNext(links_, handle->payload);
            fresh->pushBack(makeHyperlinkHandle(links_, true, identity, payload, handle->displayId));
        }
        lists.insert(key, fresh);
        return fresh;
    };

    const auto migrate = [&](u32 oldRef) -> u32 {
        STD_ASSERT(oldRef != 0 && oldRef < relocation.length());
        if (relocation[oldRef] != 0) {
            return relocation[oldRef];
        }
        CellExtra* const record = text ? oldSlots[oldRef] : slots_[oldRef];
        CellExtra copy = *record;
        if (links && copy.hyperlinks != nullptr) {
            copy.hyperlinks = relocateList(copy.hyperlinks);
        }
        if (images && copy.sixelPixels != nullptr) {
            copy.sixelPixels = copyBytesNext(images_, StringView(copy.sixelPixels, SixelPatch::pixelCount)).data();
            const u64 key = (u64)(uintptr_t)(copy.sixelPalette);
            if (const u8** relocated = palettes.find(key); relocated != nullptr) {
                copy.sixelPalette = *relocated;
            } else {
                copy.sixelPalette = copyBytesNext(images_, StringView(copy.sixelPalette, SixelPatch::paletteBytes)).data();
                palettes.insert(key, copy.sixelPalette);
            }
        }
        u32 ref = oldRef;
        if (text) {
            copy.graphemeBytes = copyBytesNext(text_, copy.graphemeBytes);
            // Copying preserves ref identity: one destination slot per
            // distinct live source ref, irrespective of equal payloads
            // in other source slots.
            auto* const fresh = new (text_.allocateNext(sizeof(CellExtra))) CellExtra(copy);
            ref = appendRecord(fresh);
        } else {
            *record = copy;
        }
        if ((text || links) && copy.hyperlinks != nullptr) {
            const HyperlinkHandle& handle = *hyperlinkOf(copy);
            if (const u32 known = findHyperlink(handle.identity); known != 0) {
                STD_ASSERT(equalBytes(hyperlinkOf(*slots_[known])->payload, handle.payload));
                slots_[ref]->hyperlinks = slots_[known]->hyperlinks;
            } else {
                indexHyperlink(ref);
            }
        }
        relocation.mut(oldRef) = ref;
        return ref;
    };

    for (TerminalCell* cell : cells) {
        STD_ASSERT(cell->hasExtra());
        migrate(cell->extraRef());
    }
    for (size_t index = 0; index < rootCount; ++index) {
        STD_ASSERT(roots[index] != nullptr);
        migrate(*roots[index]);
    }
    if (text) {
        for (TerminalCell* cell : cells) {
            cell->setExtraRef(relocation[cell->extraRef()]);
        }
        for (size_t index = 0; index < rootCount; ++index) {
            *roots[index] = relocation[*roots[index]];
        }
        text_.turn();
    }
    if (links) {
        links_.turn();
    }
    if (images) {
        images_.turn();
    }
    finishCollection();
}

void CellExtraStoreImpl::finishCollection() noexcept {
    // The refs and every pointer handed out are void: the caches keyed
    // on them must hear it even though the store object itself stays.
    extras_.notifyChanged();
}

void VtCellExtras::replace(CellExtraStore* next) {
    if (store == next) {
        return;
    }
    store = next;
    notifyChanged();
}

void VtCellExtras::notifyChanged() {
    for (IntrusiveNode* node = changedListeners.mutFront(); node != changedListeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

CellExtraStore* CellExtraStore::create(VtCellExtras& extras, ObjPool& pool) {
    return pool.make<CellExtraStoreImpl>(extras, pool);
}
