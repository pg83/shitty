#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool keyboardInput(int fd) {
        InputRecorder input;
        EventSink events;
        Client client(fd, 800, 1, nullptr, &input, true, &events);
        command(fd, Command::KeyboardEnter);
        pump(*client.platform);
        if (input.focusCount != 1 || !events.lastInfo.focused) {
            fprintf(stderr, "keyboard input: focus enter failed\n");
            return false;
        }

        command(fd, Command::KeyboardPress);
        pump(*client.platform);
        for (u32 attempt = 0; attempt != 20 && input.repeatCount == 0; ++attempt) {
            pump(*client.platform);
        }
        if (input.pressCount != 1 || input.repeatCount == 0
            || input.textCount == 0
            || input.pressedKey.key != plt::InputKey::Printable
            || input.pressedKey.layoutCodepoint != 'a'
            || input.pressedKey.baseCodepoint != 'a'
            || input.lastText.codepoint != 'a') {
            fprintf(stderr, "keyboard input: key/text/repeat translation failed\n");
            return false;
        }

        // A newer serial arrives while the key still repeats: replayed
        // repeats must not roll latestSerial back to the original press.
        command(fd, Command::PointerSequence);
        pump(*client.platform);
        const u32 repeatsBeforeProbe = input.repeatCount;
        for (u32 attempt = 0; attempt != 20 && input.repeatCount == repeatsBeforeProbe; ++attempt) {
            pump(*client.platform);
        }
        writeClipboard(*client.window->secondary(), stl::StringView(u8"serial-probe"));
        pump(*client.platform);
        const Reply serialProbe = command(fd, Command::QuerySelectionSerial);
        if (serialProbe.count + 2 < (u32)(serialProbe.first)) {
            fprintf(
                stderr,
                "keyboard input: repeat rolled the serial back, selection=%u latest=%d\n",
                serialProbe.count,
                serialProbe.first
            );
            return false;
        }

        command(fd, Command::KeyboardRelease);
        pump(*client.platform);
        const u32 repeats = input.repeatCount;
        for (u32 attempt = 0; attempt != 5; ++attempt) {
            pump(*client.platform);
        }
        if (input.releaseCount != 1 || input.repeatCount != repeats) {
            fprintf(stderr, "keyboard input: release did not stop repeat\n");
            return false;
        }

        const auto checkControl = [&](Command modifiers, u32 layout, u16 expectedModifiers, const char* failure) {
            const u32 presses = input.pressCount;
            const u32 text = input.textCount;
            command(fd, modifiers);
            pump(*client.platform);
            command(fd, Command::KeyboardPress);
            pump(*client.platform);
            const bool matches = input.pressCount == presses + 1
                && input.pressedKey.key == InputKey::Printable
                && input.pressedKey.layoutCodepoint == layout
                && input.pressedKey.baseCodepoint == 'a'
                && input.pressedKey.modifiers == expectedModifiers
                && input.textCount == text;
            command(fd, Command::KeyboardRelease);
            pump(*client.platform);
            if (!matches) {
                fprintf(stderr, "keyboard input: %s\n", failure);
            }
            return matches;
        };
        if (!checkControl(Command::KeyboardControl, 'a', InputControl, "Control folded the layout codepoint")
            || !checkControl(Command::KeyboardControlShift, 'a', InputControl | InputShift, "Shift changed the layout key identity")
            || !checkControl(Command::KeyboardControlCapsLock, 'a', InputControl | InputCapsLock, "Caps Lock changed the layout key identity")
            || !checkControl(Command::KeyboardRussianControl, 0x0444, InputControl, "active layout identity was not preserved")) {
            return false;
        }

        command(fd, Command::KeyboardLeave);
        pump(*client.platform);
        if (input.blurCount != 1 || events.lastInfo.focused) {
            fprintf(stderr, "keyboard input: focus leave failed\n");
            return false;
        }
        return true;
    }
}
