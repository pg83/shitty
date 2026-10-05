/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

#include <sys/types.h>

struct PtySize {
    u32 columns = 0;
    u32 rows = 0;
    u32 pixelWidth = 0;
    u32 pixelHeight = 0;
};

// One child and its pseudoterminal. The handle is a pool-owned duplex
// resource: dropping its owner hangs up the child and closes the master.
// Reading and writing are scheduler-aware blocking stream operations; the
// client owns every coroutine which performs them.
//
// engage() reroutes the handle through the factory's eternal drain
// thread: the kernel is drained into blocks off the parser thread, and
// acquire() hands the parser a chain of whole blocks in place of a byte
// stream - everything buffered so far, in arrival order, or null at
// EOF. The views stay valid until the caller gives the same chain back
// with release(); one chain may be out at a time. After engage() the
// stream input() must not be used; output() keeps its interface and
// queues blocks onto the same thread. Dropping an engaged handle's
// owner performs a brief blocking handshake with the drain before the
// master closes.
struct PtyHandle {
    struct Chunk {
        virtual void* data() = 0;
        virtual size_t length() = 0;
        virtual Chunk* next() = 0;

        stl::StringView chunk() {
            return stl::StringView((const u8*)(data()), length());
        }
    };

    // The child behind this handle, so an owner which reaps it waits for
    // that child and not for whatever else the process has forked. Handles
    // with no child of their own - the test doubles - keep the default.
    virtual pid_t childPid() {
        return -1;
    }

    virtual void resize(const PtySize& size) = 0;
    virtual void engage() = 0;

    // len can be capped
    virtual Chunk* allocate(size_t len) = 0;
    virtual void send(Chunk* chunk, size_t len) = 0;

    virtual Chunk* acquire() = 0;
    virtual void release(Chunk* chunks) = 0;

    // The foreground process group of the pty's slave side, or 0 when
    // there is none to report - a scripted or replying pty, a dead
    // child, a kernel that answers with nothing. Cheap enough to poll.
    virtual pid_t foregroundProcessGroup() = 0;
};
