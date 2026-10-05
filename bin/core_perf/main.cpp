/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

// Feeds a corpus file through the complete headless VT core - parser,
// vterm and screen, with the frame consumed each time - and reports the
// throughput. Unlike parser_perf, which drives the FSM with no-op
// callbacks, this measures the work an embedder actually pays for.
//
// Usage: core_perf <corpus-path> [runs] [save-lines]
//
// save-lines matters: the default Options carries 0, so a run left at
// the default measures a terminal with no scrollback to retain.

#include "composer.h"
#include <lib/vterm/num.h>
#include "options.h"
#include <lib/shitty/vt_headless.h>

#include <std/ios/in_fd.h>
#include <std/ios/sys.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/sys/fd.h>

#include <fcntl.h>

using namespace stl;

namespace {
    // One PTY read's worth per feed, so the batching matches what a host
    // draining a pty would hand over.
    static constexpr size_t chunkBytes = 64 * 1024;

    static int usage() {
        sysE << StringView(u8"usage: core_perf <corpus-path> [runs] [save-lines]") << endL;
        return 2;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return usage();
    }
    i64 runs = 3;
    if (argc > 2 && (!parseI64(StringView(argv[2]), runs) || runs <= 0)) {
        return usage();
    }
    i64 saveLines = 0;
    if (argc > 3 && (!parseI64(StringView(argv[3]), saveLines) || saveLines < 0 || saveLines > 50000)) {
        return usage();
    }

    Buffer corpus;
    {
        const int rawFd = open(argv[1], O_RDONLY);
        if (rawFd < 0) {
            sysE << StringView(u8"core_perf: cannot read corpus") << endL;
            return 2;
        }
        ScopedFD fd(rawFd);
        FDInput(fd).readAll(corpus);
    }
    if (corpus.used() == 0) {
        sysE << StringView(u8"core_perf: cannot read corpus") << endL;
        return 2;
    }

    u64 best = 0;
    for (i64 run = 0; run < runs; ++run) {
        // A fresh terminal per run: scrollback accumulated by an earlier
        // run would otherwise pay for the next one's allocations.
        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer* const composer = pool->make<Composer>(pool.mutPtr());
        // Composer builds a default Options; swap in one carrying the
        // requested scrollback, since the default retains none.
        Options* const options = pool->make<Options>();
        options->vt.saveLines = (u16)(saveLines);
        composer->setOptions(options);
        VtermHeadless* const host = VtermHeadless::create(*composer, nullptr);

        const u64 started = monotonicNowUs();
        const u8* input = (const u8*)(corpus.data());
        size_t remaining = corpus.used();
        while (remaining != 0) {
            const size_t length = remaining < chunkBytes ? remaining : chunkBytes;
            host->feed(input, length);
            input += length;
            remaining -= length;
        }
        const u64 elapsed = monotonicNowUs() - started;
        if (run == 0 || elapsed < best) {
            best = elapsed;
        }
    }

    const double seconds = (double)(best) / 1e6;
    const double mebibytes = (double)(corpus.used()) / (double)(1 << 20);
    sysO << StringView(u8"core_perf ") << StringView(argv[1]) << StringView(u8": ") << (i64)(corpus.used()) << StringView(u8" bytes in ") << (i64)(best) << StringView(u8" us, ") << (i64)(mebibytes / seconds) << StringView(u8" MiB/s, save_lines=") << saveLines << endL;
    return 0;
}
