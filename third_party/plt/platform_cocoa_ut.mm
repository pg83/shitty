#include "platform_cocoa.h"

#include <std/tst/ut.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

using namespace plt;
using namespace stl;

namespace {
    NSEvent* keyEvent(NSString* characters, NSString* base, NSEventModifierFlags flags, unsigned short keyCode) {
        return [NSEvent keyEventWithType:NSEventTypeKeyDown
                                location:NSMakePoint(0, 0)
                           modifierFlags:flags
                               timestamp:0
                            windowNumber:0
                                 context:nil
                              characters:characters
             charactersIgnoringModifiers:base
                               isARepeat:NO
                                 keyCode:keyCode];
    }
}

STD_TEST_SUITE(PlatformCocoaKey) {
    // Cocoa folds Control into characters: Ctrl+B delivers STX there, where
    // xkbcommon reports 'b'. The layout codepoint feeds the kitty-protocol
    // key field, and a C0 control is never a valid key - reporting 2 instead
    // of 98 kills every terminal-multiplexer prefix.
    STD_TEST(ControlFoldedLetterRecoversTheLayoutKey) {
        @autoreleasepool {
            const KeyInput input = keyInputFromEvent(keyEvent(@"\x02", @"b", NSEventModifierFlagControl, kVK_ANSI_B), true);
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST((input.modifiers & InputControl) != 0);
            STD_INSIST(input.layoutCodepoint == 'b');
            STD_INSIST(input.baseCodepoint == 'b');
        }
    }

    STD_TEST(ControlFoldedPunctuationRecoversTheLayoutKey) {
        @autoreleasepool {
            // Ctrl+[ folds to ESC, the same codepoint the real Escape key
            // carries; only the Printable key may be rewritten.
            const KeyInput input = keyInputFromEvent(keyEvent(@"\x1b", @"[", NSEventModifierFlagControl, kVK_ANSI_LeftBracket), true);
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST(input.layoutCodepoint == '[');
        }
    }

    STD_TEST(NamedKeysKeepTheirControlCodepoints) {
        @autoreleasepool {
            const KeyInput escape = keyInputFromEvent(keyEvent(@"\x1b", @"\x1b", 0, kVK_Escape), true);
            STD_INSIST(escape.key == InputKey::Escape);
            STD_INSIST(escape.layoutCodepoint == 0x1b);

            const KeyInput enter = keyInputFromEvent(keyEvent(@"\r", @"\r", 0, kVK_Return), true);
            STD_INSIST(enter.key == InputKey::Enter);
            STD_INSIST(enter.layoutCodepoint == '\r');

            const KeyInput tab = keyInputFromEvent(keyEvent(@"\t", @"\t", 0, kVK_Tab), true);
            STD_INSIST(tab.key == InputKey::Tab);
            STD_INSIST(tab.layoutCodepoint == '\t');

            const KeyInput backspace = keyInputFromEvent(keyEvent(@"\x7f", @"\x7f", 0, kVK_Delete), true);
            STD_INSIST(backspace.key == InputKey::Backspace);
            STD_INSIST(backspace.layoutCodepoint == 0x7f);
        }
    }

    STD_TEST(ControlFoldedRecoveryUsesTheAsciiBase) {
        @autoreleasepool {
            // On a Russian layout Ctrl+B reports STX with CYRILLIC VE as the
            // base. The recovery must run after the ASCII-capable-layout
            // correction and pick up whatever base it resolved, never the
            // folded control.
            const KeyInput input = keyInputFromEvent(keyEvent(@"\x02", @"в", NSEventModifierFlagControl, kVK_ANSI_B), true);
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST(input.layoutCodepoint == input.baseCodepoint);
            STD_INSIST(input.layoutCodepoint >= 0x20);
        }
    }
}
