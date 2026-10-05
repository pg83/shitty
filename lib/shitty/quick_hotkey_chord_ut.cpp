/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "quick_hotkey_chord.h"

#if defined(__APPLE__)

    #include <std/str/view.h>
    #include <std/tst/ut.h>

    #include <Carbon/Carbon.h>

using namespace stl;

STD_TEST_SUITE(QuickHotkeyChord) {
    STD_TEST(DefaultChordParsesToControlAndGrave) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(parseQuickHotkey(StringView(u8"ctrl+grave"), modifiers, keyCode));
        STD_INSIST(modifiers == (u32)(controlKey));
        STD_INSIST(keyCode == (u32)(kVK_ANSI_Grave));
    }

    STD_TEST(MultipleModifiersAndANamedKeyParse) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(parseQuickHotkey(StringView(u8"cmd+shift+t"), modifiers, keyCode));
        STD_INSIST(modifiers == (u32)(cmdKey | shiftKey));
        STD_INSIST(keyCode == (u32)(kVK_ANSI_T));
    }

    // Undocumented before this test, but real: the grammar has never
    // case-folded either modifier keywords or key names, so a chord
    // spelled with the wrong case is simply an unrecognized chord. Each
    // half is checked with the other half spelled correctly, so a case
    // laxity introduced on either side alone is caught.
    STD_TEST(ChordIsCaseSensitive) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(u8"CTRL+grave"), modifiers, keyCode));
        STD_INSIST(!parseQuickHotkey(StringView(u8"ctrl+GRAVE"), modifiers, keyCode));
    }

    STD_TEST(UnknownModifierIsRejected) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(u8"foo+grave"), modifiers, keyCode));
    }

    STD_TEST(UnknownKeyNameIsRejected) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(u8"ctrl+nonsense"), modifiers, keyCode));
    }

    STD_TEST(EmptyStringIsRejected) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(), modifiers, keyCode));
    }

    // "ctrl+shift" has no key token at all: the parser treats the last
    // '+'-separated piece as the key unconditionally, and "shift" is a
    // modifier keyword, not an entry in the key-name table.
    STD_TEST(ModifiersWithoutATrailingKeyAreRejected) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(u8"ctrl+shift"), modifiers, keyCode));
    }

    // A trailing '+' with nothing after it is a different shape of the
    // same failure: the last token is the empty string, which matches no
    // entry in the key-name table either.
    STD_TEST(TrailingPlusWithNoKeyIsRejected) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(u8"ctrl+"), modifiers, keyCode));
    }

    STD_TEST(GarbageIsRejected) {
        u32 modifiers = 0;
        u32 keyCode = 0;

        STD_INSIST(!parseQuickHotkey(StringView(u8"!!!garbage!!!"), modifiers, keyCode));
    }

    // The load-bearing case behind the S1 finding (R3-sec): a chord that
    // names only a key, no modifier, still parses successfully - the
    // grammar itself does not reject it. modifiers == 0 out of this
    // function is exactly the signal ui_quick_hotkey.mm's caller checks
    // separately to refuse registering a bare-key global hotkey with its
    // own, more specific diagnostic (see quick_hotkey_chord.h's comment).
    // If a future change to this function started returning a nonzero
    // modifiers value for a single-token chord instead of leaving it at
    // 0, that caller's check would stop firing and S1 would reopen
    // silently - this test exists to catch exactly that.
    STD_TEST(BareKeyWithoutModifierParsesWithZeroModifiers) {
        u32 modifiers = 123;
        u32 keyCode = 0;

        STD_INSIST(parseQuickHotkey(StringView(u8"space"), modifiers, keyCode));
        STD_INSIST(modifiers == 0);
        STD_INSIST(keyCode == (u32)(kVK_Space));
    }
}

#endif
