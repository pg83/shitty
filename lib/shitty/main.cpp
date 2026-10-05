/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "brand.h"
#include "composer.h"
#include "application.h"
#include "vt_headless.h"

#include <lib/vterm/fatal.h>

#ifdef SHITTY_HEAP_PROFILE
    #include "heap_profile.h"
#endif

#include <std/ios/in_fd.h>
#include <std/ios/sys.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/fd.h>
#include <std/sys/crt.h>
#include <std/sys/fs.h>
#include <std/sys/throw.h>

#include <string.h>
#include <exception>

#include <fcntl.h>

using namespace stl;

namespace {
    static constexpr size_t perfFeedChunk = 8 * 1024;

    struct PerfFile {
        size_t pathOffset;
    };

    static void writeRepeated(ZeroCopyOutput& output, u8 byte, size_t count) {
        if (count == 0) {
            return;
        }
        u8* const bytes = static_cast<u8*>(output.imbue(count).ptr);
        memset(bytes, byte, count);
        output.commit(count);
    }

    static void writeTenths(ZeroCopyOutput& output, double value) {
        const u64 tenths = (u64)(value * 10 + 0.5);
        output << tenths / 10 << StringView(u8".") << tenths % 10;
    }

    static void showPerfProgress(size_t done, size_t total, size_t bytes, u64 startedUs) {
        constexpr size_t width = 40;
        const size_t filled = total == 0 ? width : done * width / total;
        const double elapsed = (double)(monotonicNowUs() - startedUs) / 1e6;
        const double mib = bytes / (1024.0 * 1024.0);
        const double mibPerSecond = elapsed > 0 ? mib / elapsed : 0;
        OutBuf output(stderrStream());
        output << StringView(u8"\r[");
        writeRepeated(output, u8'#', filled);
        writeRepeated(output, u8' ', width - filled);
        output << StringView(u8"] ") << (u64)(done) << StringView(u8"/") << (u64)(total) << StringView(u8" ");
        writeTenths(output, mib);
        output << StringView(u8" MiB, ");
        writeTenths(output, mibPerSecond);
        output << StringView(u8" MiB/s") << flsH;
    }

    static int runPerf(Brand& brand, int argc, char* argv[]) {
        if (argc < 3) {
            StringBuilder message;
            message << StringView(u8"usage: ") << brand.executableName() << StringView(u8" perf DIRECTORY...");
            raiseError(StringView(message));
        }

        Buffer paths;
        Vector<PerfFile> files;
        for (int index = 2; index < argc; ++index) {
            const StringView directory(argv[index]);
            listDir(directory, [&](const TPathInfo& entry) {
                if (!entry.isDir) {
                    const PerfFile file{.pathOffset = paths.used()};
                    paths.append(directory.data(), directory.length());
                    if (directory.empty() || directory.back() != '/') {
                        const u8 slash = '/';
                        paths.append(&slash, 1);
                    }
                    paths.append(entry.item.data(), entry.item.length());
                    const u8 zero = 0;
                    paths.append(&zero, 1);
                    files.pushBack(file);
                }
            });
        }

        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr(), brand);
        VtermHeadless* vterm = VtermHeadless::create(composer, nullptr);
        Buffer data;
        size_t bytes = 0;
        const u64 started = monotonicNowUs();
        showPerfProgress(0, files.length(), bytes, started);
        for (size_t index = 0; index < files.length(); ++index) {
            const char* path = (const char*)paths.data() + files[index].pathOffset;
            const int rawFd = open(path, O_RDONLY);
            if (rawFd < 0) {
                Errno().raise(StringBuilder() << StringView(u8"cannot open ") << StringView(path));
            }

            ScopedFD fd(rawFd);
            data.reset();
            FDInput(fd).readAll(data);
            const u8* input = (const u8*)data.data();
            size_t remaining = data.used();
            while (remaining != 0) {
                const size_t length = remaining < perfFeedChunk ? remaining : perfFeedChunk;
                vterm->feed(input, length);
                input += length;
                remaining -= length;
            }
            bytes += data.used();
            if ((index + 1) % 256 == 0 || index + 1 == files.length()) {
                showPerfProgress(index + 1, files.length(), bytes, started);
            }
        }
        sysE << endL;
#ifdef SHITTY_HEAP_PROFILE
        dumpHeapProfile();
#endif
        return 0;
    }
}

int runMain(Brand& brand, int argc, char* argv[]) {
    int status = 1;
    try {
#ifdef SHITTY_HEAP_PROFILE
        initializeHeapProfile();
#endif
        if (argc > 1 && StringView(argv[1]) == StringView(u8"perf")) {
            status = runPerf(brand, argc, argv);
        } else {
            ObjPool::Ref pool = ObjPool::fromMemory();
            Composer& composer = *pool->make<Composer>(pool.mutPtr(), brand);
            composer.application = Application::create(composer);
            status = composer.application->run(argc, argv);
#ifdef SHITTY_HEAP_PROFILE
            dumpHeapProfile();
#endif
        }
    } catch (Exception& error) {
        const StringView message = error.description();
        sysE << StringView(u8"Error: ") << message << endL;
    }
    return status;
}
