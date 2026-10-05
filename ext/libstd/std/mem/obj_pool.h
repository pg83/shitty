#pragma once

#include "new.h"
#include "embed.h"
#include "disposable.h"

#include <std/ptr/arc.h>
#include <std/sys/types.h>
#include <std/typ/intrin.h>
#include <std/ptr/intrusive.h>

namespace stl {
    class StringView;

    class ObjPool: public ARC {
        template <size_t Size, size_t Align>
        void* allocFor() {
            if constexpr (Align > alignof(max_align_t)) {
                return allocateOverAligned(Size, Align);
            } else {
                return allocate(Size);
            }
        }

        // stl:: on purpose. An unqualified forward() here is subject to ADL,
        // and any argument from namespace std - make<T>(someStdString) -
        // drags std::forward in as a second, equally good candidate. libc++
        // spells its own signature with the __remove_reference_t builtin,
        // which is enough for clang to prefer stl::forward and say nothing,
        // so macOS never sees it; libstdc++ writes the class trait and both
        // gcc and clang then call it ambiguous. Same for the two calls in
        // make() below.
        template <typename T, typename... A>
        T* makeImpl(A&&... a) {
            return new (allocFor<sizeof(T), alignof(T)>()) T(stl::forward<A>(a)...);
        }

    public:
        using Ref = IntrusivePtr<ObjPool>;

        virtual ~ObjPool() noexcept;

        virtual void* allocate(size_t len) = 0;
        virtual void submit(Disposable* d) noexcept = 0;

        StringView intern(StringView s);
        void* allocateOverAligned(size_t len, size_t align);

        // king of ownership
        template <typename T, typename... A>
        T* make(A&&... a) {
            struct Wrapper1: public Embed<T>, public Newable {
                using Embed<T>::Embed;
            };

            if constexpr (stdHasTrivialDestructor(T)) {
                static_assert(sizeof(Wrapper1) == sizeof(T));

                return &makeImpl<Wrapper1>(stl::forward<A>(a)...)->t;
            } else {
                struct Wrapper2: public Disposable, public Wrapper1 {
                    using Wrapper1::Wrapper1;
                };

                auto res = makeImpl<Wrapper2>(stl::forward<A>(a)...);

                submit(res);

                return &res->t;
            }
        }

        static Ref fromMemory() {
            return fromMemoryRaw();
        }

        // 2 MiB hugetlb-backed bump arena owned by `slave`; falls back to `slave` itself if MAP_HUGETLB is refused.
        static ObjPool* fromHugePages(ObjPool* slave);

        // Strict variant — returns a HugePool or throws if hugetlb pages are unavailable. No fallback to slave.
        static ObjPool* hugePages(ObjPool* slave);

        static ObjPool* create(ObjPool* pool);
        static ObjPool* fromMemoryRaw();
    };
}
