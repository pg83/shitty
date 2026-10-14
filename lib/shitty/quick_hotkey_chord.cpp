/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_hotkey_chord.h"

#if defined(__APPLE__)

    #include <Carbon/Carbon.h>

using namespace stl;

namespace {
    // Not the same vocabulary as -remap's plt::InputKey table (see
    // input_keys.h, generated from plt/input.h): RegisterEventHotKey wants
    // a physical ANSI virtual keycode, and Carbon has never modeled that
    // as a general "key name" the way plt does. This table only needs to
    // cover keys someone would plausibly put in a global toggle chord.
    struct QuickHotkeyKeyName {
        const char* name;
        UInt32 code;
    };

    static constexpr QuickHotkeyKeyName quickHotkeyKeyNames[] = {
        {"a", kVK_ANSI_A},
        {"b", kVK_ANSI_B},
        {"c", kVK_ANSI_C},
        {"d", kVK_ANSI_D},
        {"e", kVK_ANSI_E},
        {"f", kVK_ANSI_F},
        {"g", kVK_ANSI_G},
        {"h", kVK_ANSI_H},
        {"i", kVK_ANSI_I},
        {"j", kVK_ANSI_J},
        {"k", kVK_ANSI_K},
        {"l", kVK_ANSI_L},
        {"m", kVK_ANSI_M},
        {"n", kVK_ANSI_N},
        {"o", kVK_ANSI_O},
        {"p", kVK_ANSI_P},
        {"q", kVK_ANSI_Q},
        {"r", kVK_ANSI_R},
        {"s", kVK_ANSI_S},
        {"t", kVK_ANSI_T},
        {"u", kVK_ANSI_U},
        {"v", kVK_ANSI_V},
        {"w", kVK_ANSI_W},
        {"x", kVK_ANSI_X},
        {"y", kVK_ANSI_Y},
        {"z", kVK_ANSI_Z},
        {"0", kVK_ANSI_0},
        {"1", kVK_ANSI_1},
        {"2", kVK_ANSI_2},
        {"3", kVK_ANSI_3},
        {"4", kVK_ANSI_4},
        {"5", kVK_ANSI_5},
        {"6", kVK_ANSI_6},
        {"7", kVK_ANSI_7},
        {"8", kVK_ANSI_8},
        {"9", kVK_ANSI_9},
        // The chord grammar's own default, ctrl+grave: the key above Tab,
        // to the left of 1 on a US layout.
        {"grave", kVK_ANSI_Grave},
        {"minus", kVK_ANSI_Minus},
        {"equal", kVK_ANSI_Equal},
        {"leftbracket", kVK_ANSI_LeftBracket},
        {"rightbracket", kVK_ANSI_RightBracket},
        {"semicolon", kVK_ANSI_Semicolon},
        {"quote", kVK_ANSI_Quote},
        {"comma", kVK_ANSI_Comma},
        {"period", kVK_ANSI_Period},
        {"slash", kVK_ANSI_Slash},
        {"backslash", kVK_ANSI_Backslash},
        {"space", kVK_Space},
        {"tab", kVK_Tab},
        {"escape", kVK_Escape},
        {"esc", kVK_Escape},
        {"enter", kVK_Return},
        {"return", kVK_Return},
        {"backspace", kVK_Delete},
        {"delete", kVK_ForwardDelete},
        {"up", kVK_UpArrow},
        {"down", kVK_DownArrow},
        {"left", kVK_LeftArrow},
        {"right", kVK_RightArrow},
        {"home", kVK_Home},
        {"end", kVK_End},
        {"pageup", kVK_PageUp},
        {"pagedown", kVK_PageDown},
        {"f1", kVK_F1},
        {"f2", kVK_F2},
        {"f3", kVK_F3},
        {"f4", kVK_F4},
        {"f5", kVK_F5},
        {"f6", kVK_F6},
        {"f7", kVK_F7},
        {"f8", kVK_F8},
        {"f9", kVK_F9},
        {"f10", kVK_F10},
        {"f11", kVK_F11},
        {"f12", kVK_F12},
    };

    // Same four modifier spellings as -remap (input_remap.cpp:121-131),
    // mapped onto Carbon's modifier bits instead of plt::Input*.
    static bool quickHotkeyModifierBit(StringView token, UInt32& modifiers) {
        if (token == StringView(u8"ctrl") || token == StringView(u8"control")) {
            modifiers |= controlKey;
        } else if (token == StringView(u8"shift")) {
            modifiers |= shiftKey;
        } else if (token == StringView(u8"alt") || token == StringView(u8"opt") || token == StringView(u8"option")) {
            modifiers |= optionKey;
        } else if (token == StringView(u8"super") || token == StringView(u8"cmd") || token == StringView(u8"command") || token == StringView(u8"meta")) {
            modifiers |= cmdKey;
        } else {
            return false;
        }
        return true;
    }
}

// "mod+mod+...+key", the same shape as a -remap chord side
// (input_remap.cpp:103-153): every token but the last is a modifier,
// the last one is the key.
bool parseQuickHotkey(StringView text, u32& modifiers, u32& keyCode) {
    UInt32 parsedModifiers = 0;
    UInt32 parsedKeyCode = 0;
    size_t begin = 0;
    while (begin < text.length()) {
        size_t end = begin;
        while (end < text.length() && text[end] != '+') {
            ++end;
        }
        const StringView token(text.data() + begin, end - begin);
        const bool last = end == text.length();
        begin = end + 1;
        if (!last) {
            if (!quickHotkeyModifierBit(token, parsedModifiers)) {
                return false;
            }
            continue;
        }
        for (const QuickHotkeyKeyName& entry : quickHotkeyKeyNames) {
            if (token == StringView(entry.name)) {
                parsedKeyCode = entry.code;
                modifiers = parsedModifiers;
                keyCode = parsedKeyCode;
                return true;
            }
        }
        return false;
    }
    return false;
}

#endif
