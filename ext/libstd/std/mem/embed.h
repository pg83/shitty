#pragma once

#include <std/typ/support.h>

namespace stl {
    template <typename T>
    struct Embed {
        T t;

        // stl:: on purpose - see the note on ObjPool::makeImpl. Embed is the
        // other half of that path: ObjPool::make() wraps T in one of these,
        // so an argument from namespace std reaches ADL here too.
        template <typename... A>
        Embed(A&&... a)
            : t(stl::forward<A>(a)...)
        {
        }
    };
}
