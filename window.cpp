/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "window.h"

#include "clipboard.h"
#include "composer.h"
#include "desktop_actions.h"
#include "input_sink.h"
#include "listener.h"
#include "options.h"
#include "poller.h"
#include "test_mode.h"
#include "vk_renderer.h"

#include <platform/platform.h>

#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/sys/throw.h>

#include <cerrno>
#include <cmath>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace stl;

extern char** environ;

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

    struct TestInputTranslator {
        static InputKey key(int key);
        static InputAction action(int action, bool& valid);
        static u16 modifiers(int modifiers, bool rightAlt);
        static u32 baseCodepoint(int key);
        static void sendKey(Composer& composer, int key, int action, int modifiers);
        static void sendText(Composer& composer, u32 codepoint, int modifiers);
        static void contentScale(Composer& composer, float xScale, float yScale);
    };

    struct NativeWindowImpl final: public Window, public Clipboard, public DesktopActions, public plt::PlatformEvents, public plt::WindowEvents, public plt::InputSink {
        explicit NativeWindowImpl(Composer& composer);
        ~NativeWindowImpl();

        void initialize() override;
        void show() override;
        void activate() override;
        void requestClose() override;
        bool requestFrame() override;
        void cancelFrame() override;

        void setTitle(StringView title) override;
        void requestAttention() override;
        void requestRedraw() override;
        void restore() override;
        void iconify() override;
        void move(i32 x, i32 y) override;
        void focus() override;
        void setMaximized(bool maximized) override;
        void setFullscreen(bool fullscreen) override;
        void resizePixels(u32 width, u32 height) override;
        WindowInfo info() override;

        Renderer* createRender() override;
        TestModeInput* testApi() override;

        StringView readPrimary() override;
        StringView readClipboard() override;
        void writePrimary(StringView content) override;
        void writeClipboard(StringView content) override;

        bool handlesUriScheme(StringView scheme) override;
        void openUri(StringView uri) override;
        void pointerIcon(PointerIcon icon) override;

        void fdReady(const plt::FDReady& event) override;
        void timeout() override;
        void check() override;

        void close() override;
        void resized(const plt::WindowInfo& info) override;
        void redraw() override;
        void frame() override;

        void key(const plt::KeyInput& input) override;
        void text(const plt::TextInput& input) override;
        void pointerMotion(const plt::PointerMotionInput& input) override;
        void pointerButton(const plt::PointerButtonInput& input) override;
        void scroll(const plt::ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        void publish(stl::IntrusiveList& listeners, void* argument = nullptr);
        void publishWindow(const ::WindowEvents& events);
        bool queryUriScheme(StringView scheme);

        struct UriScheme {
            StringView name;
            bool handled = false;
        };

        static constexpr size_t uriSchemeCapacity = 64;

        Composer& composer;
        plt::Window* native = nullptr;
        Buffer uriBuffer;
        UriScheme uriSchemes[uriSchemeCapacity];
        size_t uriSchemeCount = 0;
        bool initialized = false;
        bool callbacksActive = false;
    };

    struct HeadlessWindowImpl final: public Window, public TestModeInput {
        explicit HeadlessWindowImpl(Composer& composer);
        ~HeadlessWindowImpl();

        void initialize() override;
        void show() override;
        void activate() override;
        void requestClose() override;
        bool requestFrame() override;
        void cancelFrame() override;

        void setTitle(StringView title) override;
        void requestAttention() override;
        void requestRedraw() override;
        void restore() override;
        void iconify() override;
        void move(i32 x, i32 y) override;
        void focus() override;
        void setMaximized(bool maximized) override;
        void setFullscreen(bool fullscreen) override;
        void resizePixels(u32 width, u32 height) override;
        WindowInfo info() override;

        Renderer* createRender() override;
        TestModeInput* testApi() override;

        void testKeyEvent(int key, int scancode, int action, int modifiers) override;
        void testTextInput(unsigned codepoint, int modifiers) override;
        void testContentScale(float xScale, float yScale) override;

        Composer& composer;
    };
}

NativeWindowImpl::NativeWindowImpl(Composer& composer_)
    : composer(composer_)
{
    composer.window = this;
    composer.clipboard = this;
    composer.desktopActions = this;
}

NativeWindowImpl::~NativeWindowImpl() {
    composer.poller->attach(nullptr);
    composer.platform = nullptr;
    composer.window = nullptr;
    composer.clipboard = nullptr;
    composer.desktopActions = nullptr;
}

void NativeWindowImpl::initialize() {
    if (initialized) {
        return;
    }
    initialized = true;
    composer.platform = plt::Platform::create(*composer.pool, *this);
    composer.poller->attach(composer.platform);
    native = composer.platform->createWindow(
        *composer.pool,
        {
            .appId = StringView(u8"shitty"),
            .title = StringView(opts.title),
            .width = (u32)(max(320, (int)(opts.nCols) * opts.fontsize / 2)),
            .height = (u32)(max(200, (int)(opts.nRows) * opts.fontsize)),
            .input = this,
            .events = this,
        }
    );
    const plt::WindowInfo current = native->info();
    if (std::isfinite(current.contentScale) && current.contentScale > 0.0f) {
        composer.setContentScale(current.contentScale);
    }
}

void NativeWindowImpl::show() {
    const u32 border = 2u * opts.border;
    const u32 width = border + (u32)(opts.nCols) * composer.glyphWidth;
    const u32 height = border + (u32)(opts.nRows) * composer.glyphHeight;
    native->setMinimumSize(border + composer.glyphWidth, border + composer.glyphHeight);
    native->setResizeUnit(composer.glyphWidth, composer.glyphHeight, border, border);
    native->resize(width, height);
    native->show();
    resized(native->info());
}

void NativeWindowImpl::activate() {
    callbacksActive = true;
}

void NativeWindowImpl::requestClose() {
    native->requestClose();
}

bool NativeWindowImpl::requestFrame() {
    return native->requestFrame();
}

void NativeWindowImpl::cancelFrame() {
    native->cancelFrame();
}

void NativeWindowImpl::setTitle(StringView title) {
    native->setTitle(title);
}

void NativeWindowImpl::requestAttention() {
    native->requestAttention();
}

void NativeWindowImpl::requestRedraw() {
    native->requestRedraw();
}

void NativeWindowImpl::restore() {
    native->restore();
}

void NativeWindowImpl::iconify() {
    native->iconify();
}

void NativeWindowImpl::move(i32 x, i32 y) {
    native->move(x, y);
}

void NativeWindowImpl::focus() {
    native->focus();
}

void NativeWindowImpl::setMaximized(bool maximized) {
    native->setMaximized(maximized);
}

void NativeWindowImpl::setFullscreen(bool fullscreen) {
    native->setFullscreen(fullscreen);
}

void NativeWindowImpl::resizePixels(u32 width, u32 height) {
    native->resize(width, height);
}

WindowInfo NativeWindowImpl::info() {
    const plt::WindowInfo source = native->info();
    return {
        .x = source.x,
        .y = source.y,
        .screenPixelWidth = source.screenPixelWidth,
        .screenPixelHeight = source.screenPixelHeight,
        .iconified = source.iconified,
        .maximized = source.maximized,
        .fullscreen = source.fullscreen,
    };
}

Renderer* NativeWindowImpl::createRender() {
    const plt::RenderContext context = native->renderContext();
    return Renderer::create(composer, context);
}

TestModeInput* NativeWindowImpl::testApi() {
    return nullptr;
}

StringView NativeWindowImpl::readPrimary() {
    return native->readPrimary();
}

StringView NativeWindowImpl::readClipboard() {
    return native->readClipboard();
}

void NativeWindowImpl::writePrimary(StringView content) {
    native->writePrimary(content);
}

void NativeWindowImpl::writeClipboard(StringView content) {
    native->writeClipboard(content);
}

bool NativeWindowImpl::queryUriScheme(StringView scheme) {
    Buffer mime;
    mime.append("x-scheme-handler/", 17);
    mime.append(scheme.data(), scheme.length());

    int output[2];
    if (pipe(output) != 0) {
        return false;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        ::close(output[0]);
        ::close(output[1]);
        return false;
    }
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);

    char* const arguments[] = {
        (char*)("xdg-mime"),
        (char*)("query"),
        (char*)("default"),
        mime.cStr(),
        nullptr,
    };
    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, arguments[0], &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(output[1]);
    if (spawned != 0) {
        ::close(output[0]);
        return false;
    }

    bool content = false;
    u8 bytes[256];
    for (;;) {
        const ssize_t count = read(output[0], bytes, sizeof(bytes));
        if (count > 0) {
            for (ssize_t index = 0; index < count; ++index) {
                content |= bytes[index] > ' ';
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(output[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return content && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool NativeWindowImpl::handlesUriScheme(StringView scheme) {
    for (size_t index = 0; index < uriSchemeCount; ++index) {
        const UriScheme& cached = uriSchemes[index];
        if (cached.name == scheme) {
            return cached.handled;
        }
    }
    if (uriSchemeCount == uriSchemeCapacity) {
        return false;
    }
    const bool handled = queryUriScheme(scheme);
    uriSchemes[uriSchemeCount++] = {
        .name = composer.pool->intern(scheme),
        .handled = handled,
    };
    return handled;
}

void NativeWindowImpl::openUri(StringView uri) {
    uriBuffer.reset();
    uriBuffer.append(uri.data(), uri.length());
    char* const arguments[] = {
        (char*)("xdg-open"),
        uriBuffer.cStr(),
        nullptr,
    };
    pid_t pid = -1;
    posix_spawnp(&pid, arguments[0], nullptr, nullptr, arguments, environ);
}

void NativeWindowImpl::pointerIcon(PointerIcon icon) {
    native->pointerIcon(icon == PointerIcon::Link ? plt::PointerIcon::Link : plt::PointerIcon::Text);
}

void NativeWindowImpl::publish(IntrusiveList& listeners, void* argument) {
    for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen(argument);
    }
}

void NativeWindowImpl::publishWindow(const ::WindowEvents& events) {
    publish(composer.windowEventListeners, (void*)(&events));
}

void NativeWindowImpl::fdReady(const plt::FDReady& source) {
    const FDReady event{
        .fd = source.fd,
        .what = source.what,
    };
    publish(composer.onFDReady, (void*)(&event));
}

void NativeWindowImpl::timeout() {
    publish(composer.onTimeout);
}

void NativeWindowImpl::check() {
    publish(composer.eventLoopCheckListeners);
}

void NativeWindowImpl::close() {
    if (callbacksActive) {
        publishWindow({.close = true});
    }
    composer.platform->stop();
}

void NativeWindowImpl::resized(const plt::WindowInfo& current) {
    if (std::isfinite(current.contentScale) && current.contentScale > 0.0f) {
        composer.setContentScale(current.contentScale);
    }
    composer.resize((u16)(min(current.width, (u32)(UINT16_MAX))), (u16)(min(current.height, (u32)(UINT16_MAX))));
    if (callbacksActive) {
        publishWindow({.resized = true});
    }
}

void NativeWindowImpl::redraw() {
    if (callbacksActive) {
        publishWindow({.redraw = true});
    }
}

void NativeWindowImpl::frame() {
    if (callbacksActive) {
        publish(composer.frameReadyListeners);
    }
}

void NativeWindowImpl::key(const plt::KeyInput& input) {
    if (callbacksActive) {
        composer.input->key(input);
    }
}

void NativeWindowImpl::text(const plt::TextInput& input) {
    if (callbacksActive) {
        composer.input->text(input);
    }
}

void NativeWindowImpl::pointerMotion(const plt::PointerMotionInput& input) {
    if (callbacksActive) {
        composer.input->pointerMotion(input);
    }
}

void NativeWindowImpl::pointerButton(const plt::PointerButtonInput& input) {
    if (callbacksActive) {
        composer.input->pointerButton(input);
    }
}

void NativeWindowImpl::scroll(const plt::ScrollInput& input) {
    if (callbacksActive) {
        composer.input->scroll(input);
    }
}

void NativeWindowImpl::focus(bool focused) {
    if (callbacksActive) {
        composer.input->focus(focused);
    }
}

void NativeWindowImpl::pointerPresence(bool present) {
    if (callbacksActive) {
        composer.input->pointerPresence(present);
    }
}

void NativeWindowImpl::flush() {
    if (callbacksActive) {
        composer.input->flush();
    }
}

HeadlessWindowImpl::HeadlessWindowImpl(Composer& composer_)
    : composer(composer_)
{
    composer.window = this;
}

HeadlessWindowImpl::~HeadlessWindowImpl() {
    composer.window = nullptr;
}

void HeadlessWindowImpl::initialize() {
}

void HeadlessWindowImpl::show() {
}

void HeadlessWindowImpl::activate() {
}

void HeadlessWindowImpl::requestClose() {
}

bool HeadlessWindowImpl::requestFrame() {
    return false;
}

void HeadlessWindowImpl::cancelFrame() {
}

void HeadlessWindowImpl::setTitle(StringView) {
}

void HeadlessWindowImpl::requestAttention() {
}

void HeadlessWindowImpl::requestRedraw() {
}

void HeadlessWindowImpl::restore() {
}

void HeadlessWindowImpl::iconify() {
}

void HeadlessWindowImpl::move(i32, i32) {
}

void HeadlessWindowImpl::focus() {
}

void HeadlessWindowImpl::setMaximized(bool) {
}

void HeadlessWindowImpl::setFullscreen(bool) {
}

void HeadlessWindowImpl::resizePixels(u32 width, u32 height) {
    composer.resize((u16)(min(width, (u32)(UINT16_MAX))), (u16)(min(height, (u32)(UINT16_MAX))));
}

WindowInfo HeadlessWindowImpl::info() {
    return {
        .screenPixelWidth = composer.pixelWidth,
        .screenPixelHeight = composer.pixelHeight,
    };
}

Renderer* HeadlessWindowImpl::createRender() {
    Errno(ENOTSUP).raise(StringView(u8"headless window has no renderer"));
}

TestModeInput* HeadlessWindowImpl::testApi() {
    return this;
}

void HeadlessWindowImpl::testKeyEvent(int key, int, int action, int modifiers) {
    TestInputTranslator::sendKey(composer, key, action, modifiers);
}

void HeadlessWindowImpl::testTextInput(unsigned codepoint, int modifiers) {
    TestInputTranslator::sendText(composer, codepoint, modifiers);
}

void HeadlessWindowImpl::testContentScale(float xScale, float yScale) {
    TestInputTranslator::contentScale(composer, xScale, yScale);
}

InputKey TestInputTranslator::key(int key) {
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

InputAction TestInputTranslator::action(int action, bool& valid) {
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

u16 TestInputTranslator::modifiers(int modifiers, bool rightAlt) {
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
    return result;
}

u32 TestInputTranslator::baseCodepoint(int key) {
    if (key >= 'A' && key <= 'Z') {
        return key - 'A' + 'a';
    }
    return TestInputTranslator::key(key) == InputKey::Printable ? (u32)(key) : 0;
}

void TestInputTranslator::sendKey(Composer& composer, int keyCode, int actionCode, int rawModifiers) {
    bool valid;
    const InputAction inputAction = action(actionCode, valid);
    const InputKey inputKey = key(keyCode);
    if (!valid || inputKey == InputKey::Unknown) {
        return;
    }
    const bool rightAlt = inputKey == InputKey::RightAlt;
    const u32 codepoint = baseCodepoint(keyCode);
    composer.input->key({
        .key = inputKey,
        .action = inputAction,
        .modifiers = modifiers(rawModifiers, rightAlt),
        .layoutCodepoint = codepoint,
        .baseCodepoint = codepoint,
    });
}

void TestInputTranslator::sendText(Composer& composer, u32 codepoint, int rawModifiers) {
    if (codepoint != 0) {
        composer.input->text({codepoint, modifiers(rawModifiers, false)});
    }
}

void TestInputTranslator::contentScale(Composer& composer, float xScale, float yScale) {
    const float scale = max(xScale, yScale);
    if (std::isfinite(scale) && scale > 0.0f) {
        composer.setContentScale(scale);
    }
}

Window* Window::create(Composer& composer) {
    return composer.pool->make<NativeWindowImpl>(composer);
}

Window* Window::createHeadless(Composer& composer) {
    return composer.pool->make<HeadlessWindowImpl>(composer);
}
