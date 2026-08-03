#pragma once

#ifdef __OBJC__
#include "input.h"

@class NSEvent;
#endif

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;

    Platform* createCocoaPlatform(stl::ObjPool& owner);

#ifdef __OBJC__
    // The pure NSEvent -> KeyInput translation behind the window's key
    // handler, exposed so unit tests can drive it with synthesized events.
    KeyInput keyInputFromEvent(NSEvent* event, bool pressed);
#endif
}
