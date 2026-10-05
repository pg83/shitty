/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "drop_target.h"

#include "session.h"
#include "composer.h"

#include <lib/vterm/vterm.h>

#include <std/str/view.h>
#include <std/ios/input.h>
#include <std/ptr/scoped.h>
#include <std/mem/obj_pool.h>

#include <plt/drop.h>

using namespace stl;

namespace {
    static const StringView uriListMime(u8"text/uri-list");
    static const StringView utf8Mime(u8"text/plain;charset=utf-8");
    static const StringView utf8StringMime(u8"UTF8_STRING");
    static const StringView plainMime(u8"text/plain");

    static StringView preferredMime(const plt::DropOffer& offer) {
        bool uriList = false;
        bool utf8 = false;
        bool utf8String = false;
        bool plain = false;
        for (size_t index = 0; index != offer.formats(); ++index) {
            const StringView mime = offer.format(index);
            uriList = uriList || mime == uriListMime;
            utf8 = utf8 || mime == utf8Mime;
            utf8String = utf8String || mime == utf8StringMime;
            plain = plain || mime == plainMime;
        }
        if (uriList) {
            return uriListMime;
        }
        if (utf8) {
            return utf8Mime;
        }
        if (utf8String) {
            return utf8StringMime;
        }
        if (plain) {
            return plainMime;
        }
        return {};
    }

    struct VtermDropTarget final: public plt::DropTarget {
        explicit VtermDropTarget(Composer& composer);

        plt::DropReply dragOver(const plt::DropOffer& offer, i32 x, i32 y) override;
        void dragLeft() override;
        void dropped(plt::Drop& drop) override;

        Composer& composer;
        // S3: dropped() is handed no coordinates, so the last hover's are
        // kept - both backends call dragOver() on entry and on every motion
        // before ever settling a drop (platform_cocoa.mm:1835,
        // platform_wayland.cpp:2254), in the same surface-pixel space the
        // pointer events use.
        i32 overX = 0;
        i32 overY = 0;
        bool haveOver = false;
    };
}

VtermDropTarget::VtermDropTarget(Composer& composer_)
    : composer(composer_)
{
}

plt::DropReply VtermDropTarget::dragOver(const plt::DropOffer& offer, i32 x, i32 y) {
    overX = x;
    overY = y;
    haveOver = true;
    return {
        .mime = preferredMime(offer),
        .action = plt::DropAction::Copy,
    };
}

void VtermDropTarget::dragLeft() {
    // The drag left the surface: the position it left from is not where a
    // later drop lands, and a drop that follows without a hover of its own
    // is better sent to the focused pane than to a remembered pixel.
    haveOver = false;
}

void VtermDropTarget::dropped(plt::Drop& drop) {
    const StringView mime = preferredMime(*drop.what());
    if (mime.empty()) {
        return;
    }
    const ScopedPtr<Input> source{drop.read(mime)};
    if (source.ptr == nullptr) {
        return;
    }
    // S3: the pane under the pointer, not the pane holding the focus. They
    // were the same thing until a tab could hold more than one pane, and
    // this file did not have to change for that to stop being true: a path
    // quoted for the shell would go to whichever pane had the keyboard,
    // which may be sitting at a sudo prompt or inside an ssh session.
    Vterm* const terminal = haveOver ? composer.sessions->terminalAt(overX, overY) : composer.sessions->activeTerminal();
    if (mime == uriListMime) {
        terminal->dropUriList(*source.ptr);
    } else {
        terminal->dropText(*source.ptr);
    }
}

plt::DropTarget* createDropTarget(ObjPool& owner, Composer& composer) {
    return owner.make<VtermDropTarget>(composer);
}
