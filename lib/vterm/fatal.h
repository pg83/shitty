/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/str/builder.h>

// The application's throwable error: a message assembled at the raise
// site, caught as stl::Exception by the top-level handlers. This is what
// std::runtime_error used to be here.
struct FatalError final: public stl::Exception {
    explicit FatalError(stl::Buffer&& text) noexcept;
    ~FatalError() noexcept override;

    stl::ExceptionKind kind() const noexcept override;
    stl::StringView description() override;

    stl::Buffer text;
};

[[noreturn]] void raiseFatal(stl::Buffer&& text);

// raiseError(StringView(u8"-border: expected unsigned, max. "), limit) -
// every part streams through StringBuilder, so anything Outable works.
//
// Wrap literals in StringView. A bare u8"..." deduces as char8_t[N], and
// stl::output is declared for every type but defined only for the named
// ones (std/ios/outable.h), so the array instantiation survives the
// compiler and dies in the linker - on whichever platform happens to
// build the caller.
template <typename... Part>
[[noreturn]] void raiseError(const Part&... part) {
    stl::StringBuilder text;
    (text << ... << part);
    raiseFatal(static_cast<stl::Buffer&&>(text));
}
