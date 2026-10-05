/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "options.h"
#include "composer.h"
#include "input_bindings.h"

#include <lib/vterm/listener.h>

#include <std/tst/ut.h>
#include <std/mem/obj_pool.h>

using namespace stl;
using namespace plt;

namespace {
    struct CountBinding final: public Listener {
        void onListen(void*) override;

        size_t calls = 0;
    };

#if defined(__APPLE__)
    static constexpr u16 copyModifiers = InputSuper;
    static constexpr u16 inactiveCopyModifiers = InputControl | InputShift;
    static constexpr u16 incFontModifiers = InputSuper;
    static constexpr u32 incFontText = 0;
    static constexpr u16 tabModifiers = InputSuper;
    static constexpr u16 tabSwitchModifiers = InputSuper | InputShift;
#elif defined(__linux__)
    static constexpr u16 copyModifiers = InputControl | InputShift;
    static constexpr u16 inactiveCopyModifiers = InputSuper;
    static constexpr u16 incFontModifiers = InputControl | InputShift;
    static constexpr u32 incFontText = '+';
    static constexpr u16 tabModifiers = InputControl | InputShift;
    static constexpr u16 tabSwitchModifiers = InputControl | InputShift;
#else
    #error Unsupported platform
#endif
}

void CountBinding::onListen(void*) {
    ++calls;
}

STD_TEST_SUITE(InputBindings) {
    STD_TEST(MatchesNormalizedModifiersAndPublishes) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding listener;
        composer.copyListeners.pushBack(&listener);

        const bool consumed = composer.inputBindings->key({
            .key = InputKey::Printable,
            .action = InputAction::Press,
            .modifiers = copyModifiers | InputCapsLock | InputNumLock,
            .baseCodepoint = 'c',
        });

        STD_INSIST(consumed);
        STD_INSIST(listener.calls == 1);
        STD_INSIST(!composer.inputBindings->text({'c', copyModifiers}));
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Release, copyModifiers, 0, 'c'}));
    }

    STD_TEST(NamedKeyChordIgnoresTheBaseCodepoint) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding listener;
        composer.pageUpListeners.pushBack(&listener);

        // Cocoa reports named keys with the function-key private-use
        // codepoint in the base field (0xf72c for PageUp); the chord
        // identity is the key itself.
        STD_INSIST(composer.inputBindings->key({InputKey::PageUp, InputAction::Press, InputShift, 0, 0xf72c}));
        STD_INSIST(listener.calls == 1);
        STD_INSIST(composer.inputBindings->key({InputKey::PageUp, InputAction::Release, InputShift, 0, 0xf72c}));
    }

    STD_TEST(DoesNotConsumeMismatchedBinding) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding listener;
        composer.copyListeners.pushBack(&listener);

        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, inactiveCopyModifiers, 0, 'c'}));
        // Any letter the table does not claim will do; this one stood as
        // 'b' until ToggleSidebar took cmd+b, which is exactly the
        // regression the assertion is here to catch, one chord over.
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, copyModifiers, 0, 'q'}));
        STD_INSIST(listener.calls == 0);
    }

    // cmd+b. Bound on macOS only, and deliberately: ui_sidebar_tabs.mm
    // is in the darwin sources, so everywhere else the chord would be
    // taken away in exchange for nothing. The list is still claimed on
    // every platform (Composer's constructor), which is what lets this
    // test name it at all.
    //
    // And bound only while -sidebarTabs is on, for a reason with teeth:
    // cmd+b is reported to the program running inside the terminal
    // under the kitty keyboard protocol, and test_keyboard.py chose
    // that exact chord as its Super-only case because cmd+c was already
    // the platform Copy binding. A chord claimed to do nothing is a
    // chord taken away from whatever wanted it.
    // The command palette: cmd+k on the Mac whatever the tab bar; on
    // Linux ctrl+shift+k, both forms of the letter, and only with the
    // list pt draws - without it the chord is the program's.
    STD_TEST(OpensThePaletteOnItsChord) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        composer.setOptions(&options);
        CountBinding listener;
        composer.commandPaletteListeners.pushBack(&listener);
#if defined(__APPLE__)
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'k'}));
        STD_INSIST(listener.calls == 1);
#else
        STD_INSIST(!options.sidebarTabs);
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl | InputShift, 0, 'k'}));
        STD_INSIST(listener.calls == 0);
        options.sidebarTabs = true;
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl | InputShift, 0, 'k'}));
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Release, InputControl | InputShift, 0, 'k'}));
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl | InputShift, 0, 'K'}));
        STD_INSIST(listener.calls == 2);
        // Without Shift it is the shell's kill-line, untouched.
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl, 0, 'k'}));
        STD_INSIST(listener.calls == 2);
#endif
    }

    STD_TEST(TogglesTheSidebarOnTheMacOsChordAndOnlyWithTheOption) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        composer.setOptions(&options);
        CountBinding listener;
        composer.toggleSidebarListeners.pushBack(&listener);

        STD_INSIST(!options.sidebarTabs);
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'b'}));
        STD_INSIST(listener.calls == 0);

        // The option is read on every key, so a reload turns the chord
        // on and off with the panel it drives.
        options.sidebarTabs = true;
        const bool consumed = composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'b'});

#if defined(__APPLE__)
        STD_INSIST(consumed);
        STD_INSIST(listener.calls == 1);
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Release, InputSuper, 0, 'b'}));
        // Pressed again, it fires again - a toggle that only ever fired
        // once would show the panel and never put it away.
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'b'}));

        STD_INSIST(listener.calls == 2);
#else
        STD_INSIST(!consumed);
        STD_INSIST(listener.calls == 0);
#endif
    }

    // A4: cmd+d and cmd+shift+d, and only while -panes is on. The
    // gate is the outer of the two locks on splitting - splitFocused()
    // refuses on the same option - and it is the one that decides
    // whether the chord is *consumed*: cmd+d is reported to the program
    // running inside under the kitty keyboard protocol, exactly as cmd+b
    // is, so with panes off it has to reach that program untouched.
    //
    // cmd+h is deliberately not the horizontal chord: the system Hide
    // item takes it before the application is asked, so a row for it
    // would be a row that never fires.
    STD_TEST(SplitsTheFocusedPaneOnTheTwoChordsAndOnlyWithTheOption) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        Options options;
        composer.setOptions(&options);
        CountBinding vertical;
        CountBinding horizontal;
        composer.splitVerticalListeners.pushBack(&vertical);
        composer.splitHorizontalListeners.pushBack(&horizontal);

        STD_INSIST(!options.panes);
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'd'}));
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper | InputShift, 0, 'd'}));
        STD_INSIST(vertical.calls == 0);
        STD_INSIST(horizontal.calls == 0);

        // The option is read on every key, so a reload arms the chords
        // with the feature they drive.
        options.panes = true;
        const bool consumedVertical = composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'd'});

#if defined(__APPLE__)
        STD_INSIST(consumedVertical);
        STD_INSIST(vertical.calls == 1);
        STD_INSIST(horizontal.calls == 0);
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Release, InputSuper, 0, 'd'}));

        // Shift picks the other axis and nothing else: the two rows
        // differ by that modifier alone, so a table that compared only
        // the codepoint would fire the first row twice.
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper | InputShift, 0, 'd'}));
        STD_INSIST(horizontal.calls == 1);
        STD_INSIST(vertical.calls == 1);

        // And cmd+h stays the system's - no row claims it.
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'h'}));
#else
        STD_INSIST(!consumedVertical);
        STD_INSIST(vertical.calls == 0);
#endif
    }

    STD_TEST(TracksRepeatedPlatformBinding) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        IntrusiveList listeners;
        CountBinding listener;
        listeners.pushBack(&listener);
        composer.inputBindings->add(InputActions::IncFontSize, &listeners);
        const KeyInput input{InputKey::Printable, InputAction::Press, incFontModifiers, '=', '='};

        STD_INSIST(composer.inputBindings->key(input));
        STD_INSIST(composer.inputBindings->key(input));
        STD_INSIST(listener.calls == 2);
        if (incFontText != 0) {
            STD_INSIST(composer.inputBindings->text({incFontText, incFontModifiers}));
            STD_INSIST(composer.inputBindings->text({incFontText, incFontModifiers}));
        }
        STD_INSIST(!composer.inputBindings->text({'+', incFontModifiers}));
    }

    STD_TEST(FlushDropsPendingTextButReleaseRemainsConsumed) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        IntrusiveList listeners;
        CountBinding listener;
        listeners.pushBack(&listener);
        composer.inputBindings->add(InputActions::IncFontSize, &listeners);
        composer.input->key({InputKey::Printable, InputAction::Press, incFontModifiers, '=', '='});

        composer.input->flush();

        STD_INSIST(!composer.inputBindings->text({'+', incFontModifiers}));
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Release, incFontModifiers, '=', '='}));
    }

    STD_TEST(FocusLossClearsConsumedAndPendingState) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        IntrusiveList listeners;
        CountBinding listener;
        listeners.pushBack(&listener);
        composer.inputBindings->add(InputActions::IncFontSize, &listeners);
        composer.input->key({InputKey::Printable, InputAction::Press, incFontModifiers, '=', '='});

        composer.input->focus(false);

        STD_INSIST(!composer.inputBindings->text({'+', incFontModifiers}));
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Release, incFontModifiers, '=', '='}));
    }

    // The tab chords must exist in both platform blocks: add() asserts it
    // found a row for the action, so a row present on one platform and
    // missing on the other trips registration on the platform that lacks
    // it rather than failing quietly.
    STD_TEST(TabActionsAreBoundOnThisPlatform) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding newTab;
        CountBinding nextTab;
        composer.newTabListeners.pushBack(&newTab);
        composer.nextTabListeners.pushBack(&nextTab);

        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabModifiers, 0, 't'}));
        STD_INSIST(newTab.calls == 1);
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabSwitchModifiers, 0, ']'}));
        STD_INSIST(nextTab.calls == 1);
    }

    // A window without tabs (-tabs off: st on Linux) leaves every chord
    // that opens or moves between tabs to the program inside, and keeps
    // the one that closes, which then closes the window.
    STD_TEST(WithoutTabsTheTabChordsAreThePrograms) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding newTab;
        CountBinding nextTab;
        CountBinding closeTab;
        composer.newTabListeners.pushBack(&newTab);
        composer.nextTabListeners.pushBack(&nextTab);
        composer.closeTabListeners.pushBack(&closeTab);

        // Premise: with tabs, as every composer starts, the chords bind.
        STD_INSIST(composer.opts->tabs);
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabModifiers, 0, 't'}));
        STD_INSIST(newTab.calls == 1);

        Options bare;
        bare.tabs = false;
        const Options* const previous = composer.opts;
        composer.setOptions(&bare);
        const bool newConsumed = composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabModifiers, 0, 't'});
        const bool nextConsumed = composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabSwitchModifiers, 0, ']'});
        const bool closeConsumed = composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabModifiers, 0, 'w'});
        composer.setOptions(previous);

        STD_INSIST(!newConsumed);
        STD_INSIST(newTab.calls == 1);
        STD_INSIST(!nextConsumed);
        STD_INSIST(nextTab.calls == 0);
        STD_INSIST(closeConsumed);
        STD_INSIST(closeTab.calls == 1);
    }

    // Shift is part of the switch chord, and the frontends disagree about
    // whether the base codepoint of a shifted bracket is the bracket or
    // the brace: Apple documents charactersIgnoringModifiers as keeping
    // Shift, the cocoa backend's own comment says it strips it. Matching
    // is exact on the base codepoint, so binding one form leaves the
    // chord silently dead wherever the other is reported. Bind both.
    STD_TEST(TabSwitchMatchesBracketAndBraceAlike) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding prev;
        CountBinding next;
        composer.prevTabListeners.pushBack(&prev);
        composer.nextTabListeners.pushBack(&next);

        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabSwitchModifiers, 0, '['}));
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabSwitchModifiers, 0, '{'}));
        STD_INSIST(prev.calls == 2);

        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabSwitchModifiers, 0, ']'}));
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, tabSwitchModifiers, 0, '}'}));
        STD_INSIST(next.calls == 2);
    }

    // Cmd+L is what a Mac user reaches for to clear the screen; the
    // Linux habit is Ctrl+L, which the shell handles itself and which the
    // terminal must keep forwarding untouched.
    STD_TEST(NaturalEditingBindsOnlyBehindThePreset) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding wordLeft;
        CountBinding killLine;
        composer.wordLeftListeners.pushBack(&wordLeft);
        composer.killLineListeners.pushBack(&killLine);

        // Off by default: the chords stay the application's.
        STD_INSIST(!composer.inputBindings->key({InputKey::Left, InputAction::Press, InputAlt, 0, 0}));
        STD_INSIST(wordLeft.calls == 0);

        Options preset;
        preset.naturalEditing = true;
        const Options* const previous = composer.opts;
        composer.setOptions(&preset);
        const bool wordConsumed = composer.inputBindings->key({InputKey::Left, InputAction::Press, InputAlt, 0, 0});
        const bool killConsumed = composer.inputBindings->key({InputKey::Backspace, InputAction::Press, InputSuper, 0, 0});
        composer.setOptions(previous);
#if defined(__APPLE__)
        STD_INSIST(wordConsumed);
        STD_INSIST(wordLeft.calls == 1);
        STD_INSIST(killConsumed);
        STD_INSIST(killLine.calls == 1);
#else
        // The preset table only exists on macOS; Alt+Left stays the
        // application's CSI 1;3D elsewhere.
        STD_INSIST(!wordConsumed);
        STD_INSIST(wordLeft.calls == 0);
        STD_INSIST(!killConsumed);
        STD_INSIST(killLine.calls == 0);
#endif
    }

    STD_TEST(ClearIsBoundWhereThePlatformExpectsIt) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding clear;
        composer.clearListeners.pushBack(&clear);

#if defined(__APPLE__)
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputSuper, 0, 'l'}));
        STD_INSIST(clear.calls == 1);
        // Plain Ctrl+L stays the shell's business.
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl, 0, 'l'}));
        STD_INSIST(clear.calls == 1);
#else
        STD_INSIST(composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl | InputShift, 0, 'l'}));
        STD_INSIST(clear.calls == 1);
        STD_INSIST(!composer.inputBindings->key({InputKey::Printable, InputAction::Press, InputControl, 0, 'l'}));
        STD_INSIST(clear.calls == 1);
#endif
    }

    STD_TEST(ActionCanHaveMultiplePlatformBindings) {
        auto pool = ObjPool::fromMemory();
        Composer& composer = *pool->make<Composer>(pool.mutPtr());
        CountBinding listener;
        composer.pastePrimaryListeners.pushBack(&listener);

        STD_INSIST(composer.inputBindings->key({InputKey::Insert, InputAction::Press, InputShift}));
        STD_INSIST(composer.inputBindings->key({InputKey::Insert, InputAction::Release, InputShift}));
        STD_INSIST(composer.inputBindings->key({InputKey::Keypad0, InputAction::Press, InputShift | InputCapsLock}));
        STD_INSIST(composer.inputBindings->key({InputKey::Keypad0, InputAction::Release, InputShift}));
        STD_INSIST(listener.calls == 2);
    }
}
