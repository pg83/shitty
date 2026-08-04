/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "test_input.h"

#include "composer.h"

#include <plt/input.h>

#include <std/alg/minmax.h>
#include <std/mem/obj_pool.h>

#include <math.h>

using namespace stl;
using namespace plt;

namespace {
    constexpr int testRelease = 0;
    constexpr int testPress = 1;
    constexpr int testRepeat = 2;
    constexpr int testModShift = 0x0001;
    constexpr int testModControl = 0x0002;
    constexpr int testModAlt = 0x0004;
    constexpr int testModSuper = 0x0008;
    constexpr int testModCapsLock = 0x0010;
    constexpr int testModNumLock = 0x0020;
    constexpr int testModAltGraph = 0x0040;

    struct TestInputImpl final: public TestInput {
        explicit TestInputImpl(Composer& composer);

        void key(int key, int scancode, int action, int modifiers) override;
        void layoutKey(int key, int action, int modifiers, unsigned layoutCodepoint, unsigned baseCodepoint) override;
        void text(unsigned codepoint, int modifiers) override;
        void contentScale(float xScale, float yScale) override;

        static InputKey translateKey(int key);
        static InputAction translateAction(int action, bool& valid);
        static u16 translateModifiers(int modifiers, bool rightAlt);
        static u32 baseCodepoint(int key);

        Composer& composer;
    };
}

TestInputImpl::TestInputImpl(Composer& composer_)
    : composer(composer_)
{
}

InputKey TestInputImpl::translateKey(int key) {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') || key == ' ' || (key >= '\'' && key <= '/') || key == ';' || key == '=' || (key >= '[' && key <= '`')) {
        return InputKey::Printable;
    }
    if (key >= 290 && key <= 309) {
        return (InputKey)((u8)(InputKey::F1) + key - 290);
    }
    if (key >= 320 && key <= 329) {
        return (InputKey)((u8)(InputKey::Keypad0) + key - 320);
    }
    switch (key) {
        case 256:
            return InputKey::Escape;
        case 257:
            return InputKey::Enter;
        case 258:
            return InputKey::Tab;
        case 259:
            return InputKey::Backspace;
        case 260:
            return InputKey::Insert;
        case 261:
            return InputKey::Delete;
        case 262:
            return InputKey::Right;
        case 263:
            return InputKey::Left;
        case 264:
            return InputKey::Down;
        case 265:
            return InputKey::Up;
        case 266:
            return InputKey::PageUp;
        case 267:
            return InputKey::PageDown;
        case 268:
            return InputKey::Home;
        case 269:
            return InputKey::End;
        case 280:
            return InputKey::CapsLock;
        case 281:
            return InputKey::ScrollLock;
        case 282:
            return InputKey::NumLock;
        case 283:
            return InputKey::PrintScreen;
        case 284:
            return InputKey::Pause;
        case 330:
            return InputKey::KeypadDecimal;
        case 331:
            return InputKey::KeypadDivide;
        case 332:
            return InputKey::KeypadMultiply;
        case 333:
            return InputKey::KeypadSubtract;
        case 334:
            return InputKey::KeypadAdd;
        case 335:
            return InputKey::KeypadEnter;
        case 336:
            return InputKey::KeypadEqual;
        case 340:
            return InputKey::LeftShift;
        case 341:
            return InputKey::LeftControl;
        case 342:
            return InputKey::LeftAlt;
        case 343:
            return InputKey::LeftSuper;
        case 344:
            return InputKey::RightShift;
        case 345:
            return InputKey::RightControl;
        case 346:
            return InputKey::RightAlt;
        case 347:
            return InputKey::RightSuper;
        case 348:
            return InputKey::Menu;
        default:
            return InputKey::Unknown;
    }
}

InputAction TestInputImpl::translateAction(int action, bool& valid) {
    valid = true;
    switch (action) {
        case testPress:
            return InputAction::Press;
        case testRepeat:
            return InputAction::Repeat;
        case testRelease:
            return InputAction::Release;
        default:
            valid = false;
            return InputAction::Release;
    }
}

u16 TestInputImpl::translateModifiers(int modifiers, bool rightAlt) {
    u16 result = 0;
    if (modifiers & testModShift) {
        result |= InputShift;
    }
    if (modifiers & testModControl) {
        result |= InputControl;
    }
    if (modifiers & testModAlt) {
        result |= rightAlt ? InputAltGraph : InputAlt;
    }
    if (modifiers & testModSuper) {
        result |= InputSuper;
    }
    if (modifiers & testModCapsLock) {
        result |= InputCapsLock;
    }
    if (modifiers & testModNumLock) {
        result |= InputNumLock;
    }
    if (modifiers & testModAltGraph) {
        result |= InputAltGraph;
    }
    return result;
}

u32 TestInputImpl::baseCodepoint(int key) {
    if (key >= 'A' && key <= 'Z') {
        return key - 'A' + 'a';
    }
    return translateKey(key) == InputKey::Printable ? (u32)(key) : 0;
}

void TestInputImpl::key(int keyCode, int, int actionCode, int rawModifiers) {
    bool valid;
    const InputAction inputAction = translateAction(actionCode, valid);
    const InputKey inputKey = translateKey(keyCode);
    if (!valid || inputKey == InputKey::Unknown) {
        return;
    }
    const bool rightAlt = inputKey == InputKey::RightAlt;
    const u32 codepoint = baseCodepoint(keyCode);
    composer.input->key({
        .key = inputKey,
        .action = inputAction,
        .modifiers = translateModifiers(rawModifiers, rightAlt),
        .layoutCodepoint = codepoint,
        .baseCodepoint = codepoint,
    });
}

void TestInputImpl::layoutKey(int keyCode, int actionCode, int rawModifiers, unsigned layoutCodepoint, unsigned base) {
    bool valid;
    const InputAction inputAction = translateAction(actionCode, valid);
    const InputKey inputKey = translateKey(keyCode);
    if (!valid || inputKey == InputKey::Unknown) {
        return;
    }
    composer.input->key({
        .key = inputKey,
        .action = inputAction,
        .modifiers = translateModifiers(rawModifiers, false),
        .layoutCodepoint = layoutCodepoint,
        .baseCodepoint = base,
    });
}

void TestInputImpl::text(unsigned codepoint, int rawModifiers) {
    if (codepoint != 0) {
        composer.input->text({codepoint, translateModifiers(rawModifiers, false)});
    }
}

void TestInputImpl::contentScale(float xScale, float yScale) {
    const float scale = max(xScale, yScale);
    if (isfinite(scale) && scale > 0.0f) {
        composer.setContentScale(scale);
    }
}

TestInput* TestInput::create(Composer& composer) {
    return composer.pool->make<TestInputImpl>(composer);
}
