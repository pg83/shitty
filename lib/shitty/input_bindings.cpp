/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "input_bindings.h"

#include "composer.h"
#include <lib/vterm/listener.h>
#include "options.h"
#include "session.h"

#include <lib/vterm/vterm.h>

#include <std/dbg/assert.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>

using namespace stl;
using namespace plt;

namespace {
    struct InputBinding {
        InputKey key = InputKey::Unknown;
        u16 modifiers = 0;
        u32 baseCodepoint = 0;
        u32 textCodepoint = 0;
        // Rows of the -naturalEditing preset match only while the option
        // holds; it is read through the composer on every key, so a
        // config reload retunes the chords.
        bool naturalEditing = false;
        // The same, for -sidebarTabs and cmd+b. Without the option there
        // is no panel to show or hide, and a chord claimed to do nothing
        // is a chord taken away from whatever the application wanted it
        // for - the kitty keyboard protocol reports cmd+b to the program
        // running inside, and did so before this row existed
        // (test_keyboard.py picked that very chord as its Super-only
        // case, cmd+c being taken). Gated here rather than in the
        // sidebar module, so the key is not consumed in the first place.
        bool sidebarTabs = false;
        // The same, for -panes and the two split chords. cmd+d reaches
        // the program running inside under the kitty keyboard protocol
        // exactly as cmd+b does, so it stays the program's until the
        // option says a tab can be divided at all. This is the outer of
        // the two locks: splitFocused() refuses on the same option, so
        // there is no way in that skips the check.
        bool panes = false;
        // The same, for -promptEditor and cmd+a/cmd+z.
        bool promptEditor = false;
        // The same, for -tabs and the chords that open a tab or move
        // between them. st on Linux is one shell to a window, the tiling
        // manager arranging the windows; ctrl+shift+t and the rest are then
        // the program's, as they are in any terminal without tabs.
        // Closing stays bound: the last tab going closes the window.
        bool tabs = false;
    };

    struct ActionBinding {
        InputActions action;
        InputBinding input;
    };

    static constexpr ActionBinding defaultBindings[] = {
        {InputActions::PastePrimary, {InputKey::Insert, InputShift}},
        {InputActions::PastePrimary, {InputKey::Keypad0, InputShift}},
        {InputActions::PageUp, {InputKey::PageUp, InputShift}},
        {InputActions::PageDown, {InputKey::PageDown, InputShift}},
#if defined(__APPLE__)
        {InputActions::Copy, {InputKey::Printable, InputSuper, 'c'}},
        {InputActions::Paste, {InputKey::Printable, InputSuper, 'v'}},
        {InputActions::IncFontSize, {InputKey::Printable, InputSuper, '='}},
        {InputActions::IncFontSize, {InputKey::Printable, InputSuper | InputShift, '='}},
        {InputActions::DecFontSize, {InputKey::Printable, InputSuper, '-'}},
        {InputActions::ResetFontSize, {InputKey::Printable, InputSuper, '0'}},
        {InputActions::NewTab, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = 't', .tabs = true}},
        {InputActions::CloseTab, {InputKey::Printable, InputSuper, 'w'}},
        // The splits. cmd+d and cmd+shift+d, and deliberately not cmd+h
        // for "split horizontally": the system Hide item eats cmd+h
        // before an application sees it.
        {InputActions::SplitVertical, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = 'd', .panes = true}},
        // Both forms, for the same reason the bracket chords below carry
        // two rows: the chord holds Shift, and whether a shifted key's
        // base codepoint keeps the shift is a question the frontends
        // answer differently. Cocoa's own correction is supposed to hand
        // back the unshifted 'd', but a single row makes the chord silent
        // if it ever does not - and silent is exactly how this one was
        // reported.
        {InputActions::SplitHorizontal, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = 'd', .panes = true}},
        {InputActions::SplitHorizontal, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = 'D', .panes = true}},
        // Moving between panes, vim's directions on ctrl+shift: plain
        // ctrl+h/j/k/l are the shell's (Backspace, newline, kill-line,
        // clear). macOS only: elsewhere ctrl+shift+l is the platform's
        // Clear chord (below), and a set missing one direction would be
        // worse than none. Both forms of each letter, for the reason the shifted
        // split chord carries two: whether a shifted key's base codepoint
        // keeps the shift is answered differently by the frontends.
        {InputActions::FocusPaneLeft, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'h', .panes = true}},
        {InputActions::FocusPaneLeft, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'H', .panes = true}},
        {InputActions::FocusPaneDown, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'j', .panes = true}},
        {InputActions::FocusPaneDown, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'J', .panes = true}},
        {InputActions::FocusPaneUp, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'k', .panes = true}},
        {InputActions::FocusPaneUp, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'K', .panes = true}},
        {InputActions::FocusPaneRight, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'l', .panes = true}},
        {InputActions::FocusPaneRight, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'L', .panes = true}},
        // And iTerm2's and Ghostty's spelling of the same moves. Distinct
        // from both -naturalEditing sets, which take opt+arrows and
        // cmd+arrows each alone and never the two together.
        {InputActions::FocusPaneLeft, {.key = InputKey::Left, .modifiers = InputSuper | InputAlt, .panes = true}},
        {InputActions::FocusPaneDown, {.key = InputKey::Down, .modifiers = InputSuper | InputAlt, .panes = true}},
        {InputActions::FocusPaneUp, {.key = InputKey::Up, .modifiers = InputSuper | InputAlt, .panes = true}},
        {InputActions::FocusPaneRight, {.key = InputKey::Right, .modifiers = InputSuper | InputAlt, .panes = true}},
        // Both forms: the chord carries Shift and the frontends disagree
        // about whether the base codepoint of a shifted bracket keeps it.
        {InputActions::PrevTab, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = '[', .tabs = true}},
        {InputActions::PrevTab, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = '{', .tabs = true}},
        {InputActions::NextTab, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = ']', .tabs = true}},
        {InputActions::NextTab, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = '}', .tabs = true}},
        // Cmd+arrows walk the tabs - unless the -naturalEditing preset
        // holds, whose line-start/end rows register first and shadow
        // these (the issue 82 reservation).
        {InputActions::PrevTab, {.key = InputKey::Left, .modifiers = InputSuper, .tabs = true}},
        {InputActions::NextTab, {.key = InputKey::Right, .modifiers = InputSuper, .tabs = true}},
        {InputActions::SelectTab1, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '1', .tabs = true}},
        {InputActions::SelectTab2, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '2', .tabs = true}},
        {InputActions::SelectTab3, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '3', .tabs = true}},
        {InputActions::SelectTab4, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '4', .tabs = true}},
        {InputActions::SelectTab5, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '5', .tabs = true}},
        {InputActions::SelectTab6, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '6', .tabs = true}},
        {InputActions::SelectTab7, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '7', .tabs = true}},
        {InputActions::SelectTab8, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '8', .tabs = true}},
        {InputActions::SelectTab9, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = '9', .tabs = true}},
        // Plain Ctrl+L stays the shell's, on both platforms.
        {InputActions::Clear, {InputKey::Printable, InputSuper, 'l'}},
        // The command line as a text field: cmd+a selects it, cmd+z and
        // cmd+shift+z undo and redo through zsh. Redo carries both forms of
        // the shifted key, as the split chord above does.
        {InputActions::SelectCommandLine, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = 'a', .promptEditor = true}},
        {InputActions::UndoCommandLine, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = 'z', .promptEditor = true}},
        {InputActions::RedoCommandLine, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = 'z', .promptEditor = true}},
        {InputActions::RedoCommandLine, {.key = InputKey::Printable, .modifiers = InputSuper | InputShift, .baseCodepoint = 'Z', .promptEditor = true}},
        // The sidebar tab list. macOS only, like the module that answers
        // it (ui_sidebar_tabs.mm is in the darwin sources), and only
        // while -sidebarTabs is on: binding the chord where nothing can
        // act on it would only take a keystroke away for no gain.
        {InputActions::ToggleSidebar, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = 'b', .sidebarTabs = true}},
        {InputActions::CommandPalette, {.key = InputKey::Printable, .modifiers = InputSuper, .baseCodepoint = 'k'}},
        // The -naturalEditing preset: the natural-text-editing chords of
        // Terminal.app, Ghostty's defaults and iTerm2's Natural Text
        // Editing preset. Not bound by default - the Command arrows stay
        // reserved for tab navigation - and, like every chord here, the
        // preset wins over whatever keyboard protocol the application
        // enabled.
        {InputActions::WordLeft, {InputKey::Left, InputAlt, 0, 0, true}},
        {InputActions::WordRight, {InputKey::Right, InputAlt, 0, 0, true}},
        {InputActions::LineStart, {InputKey::Left, InputSuper, 0, 0, true}},
        {InputActions::LineEnd, {InputKey::Right, InputSuper, 0, 0, true}},
        {InputActions::KillLine, {InputKey::Backspace, InputSuper, 0, 0, true}},
        {InputActions::EraseWord, {InputKey::Backspace, InputAlt, 0, 0, true}},
#elif defined(__linux__)
        {InputActions::Copy, {InputKey::Printable, InputControl | InputShift, 'c'}},
        {InputActions::Paste, {InputKey::Printable, InputControl | InputShift, 'v'}},
        {InputActions::IncFontSize, {InputKey::Printable, InputControl | InputShift, '=', '+'}},
        {InputActions::DecFontSize, {InputKey::Printable, InputControl, '-', '-'}},
        {InputActions::ResetFontSize, {InputKey::Printable, InputControl, '0', '0'}},
        {InputActions::NewTab, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 't', .tabs = true}},
        {InputActions::CloseTab, {InputKey::Printable, InputControl | InputShift, 'w'}},
        {InputActions::PrevTab, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = '[', .tabs = true}},
        {InputActions::PrevTab, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = '{', .tabs = true}},
        {InputActions::NextTab, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = ']', .tabs = true}},
        {InputActions::NextTab, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = '}', .tabs = true}},
        {InputActions::Clear, {InputKey::Printable, InputControl | InputShift, 'l', 'L'}},
        // The tab list the window draws itself on Wayland (ui_wayland_chrome):
        // put away and brought back, the Mac's cmd+b. Both forms of the
        // shifted letter, for the reason the split chords carry two.
        {InputActions::ToggleSidebar, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'b', .sidebarTabs = true}},
        {InputActions::ToggleSidebar, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'B', .sidebarTabs = true}},
        // The palette is drawn by the window pt draws itself: no list, no
        // palette, and the chord reaches the program inside.
        {InputActions::CommandPalette, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'k', .sidebarTabs = true}},
        {InputActions::CommandPalette, {.key = InputKey::Printable, .modifiers = InputControl | InputShift, .baseCodepoint = 'K', .sidebarTabs = true}},
#else
    #error Unsupported platform
#endif
    };

    struct RegisteredBinding {
        InputBinding input;
        IntrusiveList* listeners = nullptr;
        unsigned pendingText = 0;
        bool consumed = false;
    };

    struct InputBindingsImpl final: public InputBindings {
        explicit InputBindingsImpl(Composer& composer);

        void add(InputActions action, IntrusiveList* listeners) override;
        bool key(const KeyInput& input) override;
        bool text(const TextInput& input) override;
        bool pointerMotion(const PointerMotionInput& input) override;
        bool pointerButton(const PointerButtonInput& input) override;
        bool scroll(const ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        static void publish(IntrusiveList& listeners);
        static u16 normalizedModifiers(u16 modifiers);
        RegisteredBinding* find(const KeyInput& input);
        bool commandLineEditable() const;

        Composer& composer_;
        Vector<RegisteredBinding> bindings_;
        bool registered_[(unsigned)(InputActions::Count)]{};
    };
}

InputBindingsImpl::InputBindingsImpl(Composer& composer)
    : composer_(composer)
{
}

void InputBindingsImpl::add(InputActions action, IntrusiveList* listeners) {
    STD_ASSERT(listeners != nullptr);
    const unsigned index = (unsigned)(action);
    STD_ASSERT(index < (unsigned)(InputActions::Count));
    STD_ASSERT(!registered_[index]);
    for (const ActionBinding& binding : defaultBindings) {
        if (binding.action == action) {
            bindings_.pushBack({binding.input, listeners});
        }
    }
    // An action may register with no chord on this platform: the
    // natural-editing preset only exists in the macOS table.
    registered_[index] = true;
}

void InputBindingsImpl::publish(IntrusiveList& listeners) {
    for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

u16 InputBindingsImpl::normalizedModifiers(u16 modifiers) {
    return modifiers & ~(InputCapsLock | InputNumLock);
}

// A named key carries its whole identity in `key`; the codepoints only
// disambiguate printables. Cocoa reports arrows and friends with the
// function-key private-use codepoint in the base field, so comparing it
// for named keys would unmatch every such chord there.
static bool sameChordKey(const InputBinding& binding, InputKey key, u32 baseCodepoint) {
    if (binding.key != key) {
        return false;
    }
    return key != InputKey::Printable || binding.baseCodepoint == baseCodepoint;
}

// cmd+a and cmd+z act on a command line only where there is one to edit;
// anywhere else they are the program's, which under the kitty keyboard
// protocol hears them as keys of its own.
bool InputBindingsImpl::commandLineEditable() const {
    if (!composer_.opts->vt.promptEditor || composer_.sessions == nullptr) {
        return false;
    }
    Vterm* const terminal = composer_.sessions->activeTerminal();
    return terminal != nullptr && terminal->commandLineEditable();
}

RegisteredBinding* InputBindingsImpl::find(const KeyInput& input) {
    const u16 modifiers = normalizedModifiers(input.modifiers);
    for (RegisteredBinding* binding = bindings_.mutBegin(); binding != bindings_.mutEnd(); ++binding) {
        if (binding->input.naturalEditing && !composer_.opts->naturalEditing) {
            continue;
        }
        if (binding->input.sidebarTabs && !composer_.opts->sidebarTabs) {
            continue;
        }
        if (binding->input.panes && !composer_.opts->panes) {
            continue;
        }
        if (binding->input.tabs && !composer_.opts->tabs) {
            continue;
        }
        if (binding->input.promptEditor && !commandLineEditable()) {
            continue;
        }
        if (sameChordKey(binding->input, input.key, input.baseCodepoint) && binding->input.modifiers == modifiers) {
            return binding;
        }
    }
    return nullptr;
}

bool InputBindingsImpl::key(const KeyInput& input) {
    if (input.action == InputAction::Release) {
        bool consumed = false;
        for (RegisteredBinding* binding = bindings_.mutBegin(); binding != bindings_.mutEnd(); ++binding) {
            if (binding->consumed && sameChordKey(binding->input, input.key, input.baseCodepoint)) {
                binding->consumed = false;
                binding->pendingText = 0;
                consumed = true;
            }
        }
        return consumed;
    }
    RegisteredBinding* const binding = find(input);
    if (binding == nullptr) {
        return false;
    }

    binding->consumed = true;
    if (binding->input.textCodepoint != 0) {
        ++binding->pendingText;
    }
    publish(*binding->listeners);
    return true;
}

bool InputBindingsImpl::text(const TextInput& input) {
    for (RegisteredBinding* binding = bindings_.mutBegin(); binding != bindings_.mutEnd(); ++binding) {
        if (binding->pendingText != 0 && binding->input.textCodepoint == input.codepoint) {
            --binding->pendingText;
            return true;
        }
    }
    return false;
}

bool InputBindingsImpl::pointerMotion(const PointerMotionInput&) {
    return false;
}

bool InputBindingsImpl::pointerButton(const PointerButtonInput&) {
    return false;
}

bool InputBindingsImpl::scroll(const ScrollInput&) {
    return false;
}

void InputBindingsImpl::focus(bool focused) {
    if (focused) {
        return;
    }
    for (RegisteredBinding* binding = bindings_.mutBegin(); binding != bindings_.mutEnd(); ++binding) {
        binding->pendingText = 0;
        binding->consumed = false;
    }
}

void InputBindingsImpl::pointerPresence(bool) {
}

void InputBindingsImpl::flush() {
    for (RegisteredBinding* binding = bindings_.mutBegin(); binding != bindings_.mutEnd(); ++binding) {
        binding->pendingText = 0;
    }
}

InputBindings* InputBindings::create(Composer& composer) {
    return composer.pool->make<InputBindingsImpl>(composer);
}
