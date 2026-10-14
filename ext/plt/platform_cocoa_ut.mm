#include "platform_cocoa.h"

#include <std/tst/ut.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <objc/runtime.h>

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

    // What the *currently selected* keyboard layout answers for a
    // physical key, asked of Carbon directly rather than of the AppKit
    // call under test - an independent second opinion, not a restatement
    // of the implementation.
    u32 activeLayoutLevel(unsigned short keyCode, UInt32 carbonModifiers) {
        TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
        if (source == nullptr) {
            return 0;
        }
        u32 result = 0;
        auto layoutData = (CFDataRef)(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
        if (layoutData != nullptr) {
            const auto* layout = (const UCKeyboardLayout*)(CFDataGetBytePtr(layoutData));
            UInt32 deadKeys = 0;
            UniChar characters[4];
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, keyCode, kUCKeyActionDisplay, carbonModifiers, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &deadKeys, 4, &length, characters) == noErr && length != 0) {
                result = characters[0];
            }
        }
        CFRelease(source);
        return result;
    }
}

STD_TEST_SUITE(PlatformCocoaWindow) {
    STD_TEST(DecorationOptionSelectsTheWindowStyle) {
        const NSWindowStyleMask decorated = (NSWindowStyleMask)cocoaWindowStyleMask(true);
        STD_INSIST((decorated & NSWindowStyleMaskTitled) != 0);
        STD_INSIST((decorated & NSWindowStyleMaskClosable) != 0);
        STD_INSIST((decorated & NSWindowStyleMaskMiniaturizable) != 0);
        STD_INSIST((decorated & NSWindowStyleMaskResizable) != 0);

        const NSWindowStyleMask borderless = (NSWindowStyleMask)cocoaWindowStyleMask(false);
        STD_INSIST((borderless & NSWindowStyleMaskTitled) == 0);
        STD_INSIST((borderless & NSWindowStyleMaskClosable) == 0);
        STD_INSIST((borderless & NSWindowStyleMaskMiniaturizable) == 0);
        STD_INSIST((borderless & NSWindowStyleMaskResizable) != 0);
    }

    // A Regular-policy application gets a menu bar whether or not it
    // supplies one, so an app with no NSMenu shows an empty "Example" menu
    // when the pointer reaches the top of a fullscreen window. The same
    // gap loses Cmd+Q, which is a menu key equivalent rather than a key
    // event: with no item carrying it, nothing quits.
    STD_TEST(MainMenuCarriesTheApplicationItems) {
        @autoreleasepool {
            NSMenu* const menu = cocoaBuildMainMenu(@"Example");
            STD_INSIST(menu != nil);
            STD_INSIST(menu.numberOfItems >= 1);

            NSMenu* const application = [menu itemAtIndex:0].submenu;
            STD_INSIST(application != nil);
            STD_INSIST(application.numberOfItems > 0);

            NSMenuItem* quit = nil;
            for (NSMenuItem* item in application.itemArray) {
                if ([item.keyEquivalent isEqualToString:@"q"]) {
                    quit = item;
                }
            }
            STD_INSIST(quit != nil);
            STD_INSIST(quit.action == @selector(terminate:));
            STD_INSIST((quit.keyEquivalentModifierMask & NSEventModifierFlagCommand) != 0);
        }
    }

    STD_TEST(WindowSubclassOverridesKeyEligibility) {
        const Class windowClass = NSClassFromString(@"PltWindow");
        STD_INSIST(windowClass != Nil);
        const Method implementation = class_getInstanceMethod(windowClass, @selector(canBecomeKeyWindow));
        const Method inherited = class_getInstanceMethod([NSWindow class], @selector(canBecomeKeyWindow));
        STD_INSIST(implementation != nullptr);
        STD_INSIST(inherited != nullptr);
        STD_INSIST(method_getImplementation(implementation) != method_getImplementation(inherited));
    }

    STD_TEST(ResizePolicyDistinguishesLiveAndManagedProposals) {
        STD_INSIST(cocoaResizeUsesExactProposal(true, true, true));
        STD_INSIST(cocoaResizeUsesExactProposal(true, true, false));
        STD_INSIST(cocoaResizeUsesExactProposal(false, true, false));
        STD_INSIST(!cocoaResizeUsesExactProposal(false, true, true));
        // Preserve the bootstrap fallback before the content view exists.
        STD_INSIST(!cocoaResizeUsesExactProposal(false, false, false));
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

    STD_TEST(ControlFoldedRecoveryKeepsTheActiveLayout) {
        @autoreleasepool {
            // On a Russian layout Ctrl+B reports STX with CYRILLIC VE as
            // charactersIgnoringModifiers. The recovery restores that
            // active-layout letter - matching the Wayland level-zero
            // identity - while the base field keeps the ASCII key.
            const KeyInput input = keyInputFromEvent(keyEvent(@"\x02", @"в", NSEventModifierFlagControl, kVK_ANSI_B), true);
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST(input.layoutCodepoint == 0x0432);
            STD_INSIST(input.baseCodepoint == 'b');
        }
    }

    // -charactersByApplyingModifiers:, which the Shift path asks for both
    // levels, re-derives them from the *currently selected* keyboard
    // layout and ignores the strings a synthetic event carries. Hard
    // coding 'a'/'A' therefore only held while the machine running the
    // test happened to sit on a Latin layout, and failed outright on a
    // Russian one, where kVK_ANSI_A answers CYRILLIC EF. The expectation
    // comes from the same live layout instead, so what is asserted is
    // the actual claim: level zero lands in layoutCodepoint and level
    // one in shiftedCodepoint, on release as well as on press.
    //
    // Both levels are read for what they are, with no precondition on
    // what they contain. Requiring them to be non-empty and to differ
    // would have traded a Latin assumption for a cased-script one:
    // Hebrew, Lao, KANA and 2-Set Hangul answer the same character at
    // both levels, and Tibetan-QWERTY and Georgian-QWERTY answer nothing
    // at level one - 28 of this machine's 251 installed layouts break one
    // of those three requirements (R2-qa round 3, I12). An empty level is
    // not a broken test either, it is the documented fallback: the Shift
    // path keeps the event's own characters when the layout has nothing
    // to offer, which is the second half of each expectation below.
    //
    // baseCodepoint stays literal on purpose - it comes from the
    // ASCII-capable layout, which is Latin by definition, and the two
    // sibling tests above pin it the same way.
    STD_TEST(ShiftedPrintableKeepsBothLayoutLevelsOnRelease) {
        @autoreleasepool {
            const u32 unshifted = activeLayoutLevel(kVK_ANSI_A, 0);
            const u32 shifted = activeLayoutLevel(kVK_ANSI_A, shiftKey >> 8);
            // 'A' on both sides is what the synthetic event below carries
            // as its own characters.
            const u32 expectedLayout = unshifted != 0 ? unshifted : (u32)('A');
            const u32 expectedShifted = shifted != 0 ? shifted : (u32)('A');

            NSEvent* const event = keyEvent(
                @"A",
                @"A",
                NSEventModifierFlagShift,
                kVK_ANSI_A
            );
            const KeyInput input = keyInputFromEvent(event, false);
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST(input.action == InputAction::Release);
            STD_INSIST((input.modifiers & InputShift) != 0);
            STD_INSIST(input.layoutCodepoint == expectedLayout);
            STD_INSIST(input.shiftedCodepoint == expectedShifted);
            STD_INSIST(input.baseCodepoint == 'a');
        }
    }

    STD_TEST(OptionAsAltUsesTheUnmodifiedLayoutKey) {
        @autoreleasepool {
            const KeyInput input = keyInputFromEvent(
                keyEvent(@"ƒ", @"f", NSEventModifierFlagOption, kVK_ANSI_F),
                true
            );
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST((input.modifiers & InputAlt) != 0);
            STD_INSIST(input.layoutCodepoint == 'f');
            STD_INSIST(input.baseCodepoint == 'f');
        }
    }

    STD_TEST(OptionAsAltRecoversADeadKey) {
        @autoreleasepool {
            const KeyInput input = keyInputFromEvent(
                keyEvent(@"", @"n", NSEventModifierFlagOption, kVK_ANSI_N),
                true
            );
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST((input.modifiers & InputAlt) != 0);
            STD_INSIST(input.layoutCodepoint == 'n');
            STD_INSIST(input.baseCodepoint == 'n');
        }
    }

    STD_TEST(OptionAsAltTranslatesPunctuationWithoutOption) {
        @autoreleasepool {
            const KeyInput input = keyInputFromEvent(
                keyEvent(
                    @"…",
                    @";",
                    NSEventModifierFlagOption,
                    kVK_ANSI_Semicolon
                ),
                true
            );
            STD_INSIST(input.key == InputKey::Printable);
            STD_INSIST((input.modifiers & InputAlt) != 0);
            STD_INSIST(input.layoutCodepoint == ';');
            STD_INSIST(input.baseCodepoint == ';');
        }
    }
}
