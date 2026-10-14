/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_frame_store.h"

#include <lib/vterm/num.h>

#include <std/alg/minmax.h>
#include <std/ios/fs_utils.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/sys/fd.h>
#include <std/sys/throw.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

using namespace stl;

bool defaultQuickFramePath(StringView configPath, StringBuilder& out) {
    if (configPath.empty()) {
        return false;
    }

    // The extension is the substring after the last '.' that comes after
    // the last '/', so a dotted directory name in the path never gets
    // mistaken for one.
    size_t afterSlash = 0;
    for (size_t at = 0; at < configPath.length(); ++at) {
        if (configPath[at] == '/') {
            afterSlash = at + 1;
        }
    }
    size_t extensionAt = configPath.length();
    for (size_t at = afterSlash; at < configPath.length(); ++at) {
        if (configPath[at] == '.') {
            extensionAt = at;
        }
    }

    out << StringView(configPath.data(), extensionAt) << StringView(u8"-quick-frame");
    return true;
}

QuickFrameRect quickFrameTarget(const QuickFrame& frame, const QuickFrameRect& visible, double titlebarHeight) {
    const double titlebar = max(0.0, titlebarHeight);
    // A degenerate screen would otherwise produce a zero-sized window,
    // which is never a useful outcome; one point is the same floor
    // saveQuickFrame's own callers already respect.
    const double visibleWidth = max(1.0, visible.width);
    const double visibleHeight = max(1.0, visible.height);

    const double wantedWidth = max(1.0, (double)(frame.width));
    const double wantedHeight = max(1.0, (double)(frame.height)) + titlebar;

    QuickFrameRect target;
    target.width = min(wantedWidth, visibleWidth);
    target.height = min(wantedHeight, visibleHeight);
    // The bounds are computed from the clamped size, so they are never
    // below visible's own origin no matter how small the screen is.
    target.x = min(max((double)(frame.x), visible.x), visible.x + visibleWidth - target.width);
    target.y = min(max((double)(frame.y), visible.y), visible.y + visibleHeight - target.height);
    return target;
}

double quickFrameOverlap(const QuickFrameRect& a, const QuickFrameRect& b) {
    const double width = min(a.x + a.width, b.x + b.width) - max(a.x, b.x);
    const double height = min(a.y + a.height, b.y + b.height) - max(a.y, b.y);
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return width * height;
}

bool quickFrameFitsScreens(const QuickFrameRect& frame, const QuickFrameRect* screens, size_t count) {
    const double area = frame.width * frame.height;
    if (area <= 0) {
        return false;
    }

    double covered = 0;
    for (size_t at = 0; at < count; ++at) {
        bool duplicate = false;
        for (size_t earlier = 0; earlier < at; ++earlier) {
            // Mirroring is the one arrangement where two screens report
            // the same rect; counting it twice would call a window that
            // is half off the display fully covered.
            if (screens[earlier].x == screens[at].x && screens[earlier].y == screens[at].y && screens[earlier].width == screens[at].width && screens[earlier].height == screens[at].height) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            covered += quickFrameOverlap(frame, screens[at]);
        }
    }
    // Half a square point of slack, not exact equality: these areas are
    // products of CGFloats that came out of AppKit, and a frame flush
    // against a screen edge is the ordinary case, not the exotic one.
    return covered >= area - 0.5;
}

bool quickFrameShouldSave(bool computed, const QuickFrameRect& computedFrame, const QuickFrameRect& live) {
    if (!computed) {
        return true;
    }
    return computedFrame.x != live.x || computedFrame.y != live.y || computedFrame.width != live.width || computedFrame.height != live.height;
}

u32 quickFrameRegridExtent(u32 extent, float restoredScale, float composerScale) {
    const float from = restoredScale > 0.0f ? restoredScale : 1.0f;
    // Named rather than cast inline: clang-format reads a cast next to a
    // binary operator as a dereference and glues them together
    // (STYLE.md's own list of its quirks).
    const float points = extent;
    return min((u32)(points * composerScale / from), (u32)(UINT16_MAX));
}

namespace {

    // A single "key=value" line, value parsed as a plain signed integer.
    // False on any shape this doesn't recognize - the caller treats a
    // line it can't use exactly like one that was never there, rather
    // than failing the whole file over it.
    bool parseFrameLine(StringView line, StringView& outKey, i64& outValue) {
        StringView key;
        StringView value;
        if (!line.split('=', key, value)) {
            return false;
        }
        if (!parseI64(value.stripSpace(), outValue)) {
            return false;
        }
        // The key is stripped for the same reason the value is: this file
        // is one a user edits by hand - deleting it is the documented way
        // to forget a saved frame - and "y-points = 50" silently losing
        // its line, which then makes the whole file parse as incomplete
        // and be discarded, is not a distinction worth having (R2-test,
        // Z5).
        outKey = key.stripSpace();
        return true;
    }
}

bool loadQuickFrame(StringView path, QuickFrame& out) {
    Buffer pathBuf{path};
    Buffer text;
    try {
        readFileContent(pathBuf, text);
    } catch (Exception&) {
        return false;
    }

    QuickFrame parsed;
    bool sawX = false;
    bool sawY = false;
    bool sawWidth = false;
    bool sawHeight = false;

    StringView remaining(text);
    while (!remaining.empty()) {
        StringView line;
        StringView rest;
        if (remaining.split('\n', line, rest)) {
            remaining = rest;
        } else {
            line = remaining;
            remaining = StringView();
        }
        line = line.stripCr().stripSpace();
        if (line.empty()) {
            continue;
        }

        StringView key;
        i64 value = 0;
        if (!parseFrameLine(line, key, value)) {
            continue;
        }

        if (key == StringView(u8"x-points")) {
            parsed.x = (i32)(value);
            sawX = true;
        } else if (key == StringView(u8"y-points")) {
            parsed.y = (i32)(value);
            sawY = true;
        } else if (key == StringView(u8"width-points") && value >= 0) {
            parsed.width = (u32)(value);
            sawWidth = true;
        } else if (key == StringView(u8"height-points") && value >= 0) {
            parsed.height = (u32)(value);
            sawHeight = true;
        }
    }

    if (!sawX || !sawY || !sawWidth || !sawHeight) {
        return false;
    }

    out = parsed;
    return true;
}

bool saveQuickFrame(StringView path, const QuickFrame& frame) {
    StringBuilder tmpPath;
    tmpPath << path << StringView(u8".tmp.") << (i64)(getpid());
    Buffer tmpPathBuf{StringView(tmpPath)};

    StringBuilder content;
    content << StringView(u8"x-points=") << (i64)(frame.x) << StringView(u8"\n");
    content << StringView(u8"y-points=") << (i64)(frame.y) << StringView(u8"\n");
    content << StringView(u8"width-points=") << (i64)(frame.width) << StringView(u8"\n");
    content << StringView(u8"height-points=") << (i64)(frame.height) << StringView(u8"\n");

    try {
        const int rawFd = ::open(tmpPathBuf.cStr(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rawFd < 0) {
            Errno().raise(StringBuilder() << StringView(u8"open() failed"));
        }
        ScopedFD fd(rawFd);
        const StringView written(content);
        fd.write(written.data(), written.length());
        fd.fsync();
        fd.close();
    } catch (Exception&) {
        ::unlink(tmpPathBuf.cStr());
        return false;
    }

    Buffer pathBuf{path};
    if (::rename(tmpPathBuf.cStr(), pathBuf.cStr()) < 0) {
        ::unlink(tmpPathBuf.cStr());
        return false;
    }
    return true;
}
