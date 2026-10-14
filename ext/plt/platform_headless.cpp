#include "platform_headless.h"

#include "fiber.h"
#include "loop_wake.h"
#include "poller_loop.h"

#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/mem/obj_pool.h>
#include <std/thr/poll_fd.h>
#include <std/mem/small_obj_allocator.h>

#include <algorithm>
#include <new>
#include <vector>

using namespace plt;
using namespace stl;

namespace {
    struct PlatformHeadless;

    struct ClipboardHeadless final: Clipboard {
        Input* read() override;
        Output* write() override;

        PlatformHeadless* platform = nullptr;
    };

    // Headless streams: reads are immediately empty, writes are discarded;
    // plain delete releases the object.
    struct HeadlessClipboardInput final: public Input {
        explicit HeadlessClipboardInput(SmallObjAllocator* allocator);

        void operator delete(HeadlessClipboardInput* input, std::destroying_delete_t) noexcept;

        size_t readImpl(void* data, size_t len) override;

        SmallObjAllocator* allocator;
    };

    struct HeadlessClipboardOutput final: public Output {
        explicit HeadlessClipboardOutput(SmallObjAllocator* allocator);

        void operator delete(HeadlessClipboardOutput* output, std::destroying_delete_t) noexcept;

        size_t writeImpl(const void* data, size_t size) override;

        SmallObjAllocator* allocator;
    };

    struct WindowHeadlessImpl final: WindowHeadless {
        explicit WindowHeadlessImpl(const WindowOptions& options);

        void requestShow() override;
        void requestHide() override;
        void requestShowAt(ShowPlacement placement) override;
        void requestClose() override;
        void requestFrame() override;
        void requestTitle(StringView title) override;
        void requestAttention() override;
        void requestRestore() override;
        void requestIconify() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestCornerRadius(u16 radius) override;
        void requestResize(u32 width, u32 height) override;
        void requestMinimumSize(u32 width, u32 height) override;
        void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        Clipboard* primary() override;
        Clipboard* secondary() override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestOpenUri(StringView uri) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        WindowInfo info() const override;
        bool visible() const override;
        bool inLiveResize() const override;
        RenderContext renderContext() const override;

        bool dispatchFrame() override;
        bool framePending() const override;
        void configure(const WindowInfo& info) override;
        void failNextPresentation() override;
        HeadlessFrame presentedFrame() const override;
        void setClipboards(Clipboard& primary, Clipboard& secondary) override;
        WindowSizeRequest requestedMinimumSize() const override;
        WindowResizeUnitRequest requestedResizeUnit() const override;
        WindowTextInputRect requestedTextInputRect() const override;
        PointerIcon pointerIcon() const override;
        stl::StringView openedUri() const override;
        u64 openUriCount() const override;
        stl::StringView title() const override;

        void resizeBackBuffer();
        void restoreSize();

        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        ClipboardHeadless clipboard_;
        Clipboard* primary_ = &clipboard_;
        Clipboard* secondary_ = &clipboard_;
        WindowInfo info_;
        WindowInfo restored_;
        PointerIcon icon_ = PointerIcon::Default;
        WindowSizeRequest minimumSize_;
        WindowResizeUnitRequest resizeUnit_;
        WindowTextInputRect textInputRect_;
        std::vector<u8> title_;
        std::vector<u8> uri_;
        u64 openCount_ = 0;
        mutable HeadlessRenderTarget target_;
        std::vector<u8> front_;
        std::vector<u8> back_;
        u32 frontWidth_ = 0;
        u32 frontHeight_ = 0;
        u64 generation_ = 0;
        bool pending_ = false;
        bool failNext_ = false;
        bool haveRestored_ = false;
        bool closed_ = false;
        bool shown_ = false;
    };

    struct PlatformHeadless final: Platform {
        // Frames stay with the harness: it dispatches them deterministically
        // through WindowHeadless::dispatchFrame. The loop serves timers and
        // descriptors only. The one exception is the fullscreen transition,
        // which delivers its frame from inside the request the way Cocoa
        // commits the transition's Core Animation transaction inline.
        void run() override {
            // stopped is consumed on exit, not reset on entry: a fiber
            // spawned before run() executes its prefix inline and may
            // finish the whole session — including stop() — before the
            // loop starts, and that stop must not be erased.
            while (!stopped) {
                poller_->dispatchTimers();
                if (stopped) {
                    break;
                }
                poller_->wait(poller_->nextDeadline());
            }
            stopped = false;
        }

        void stop() override {
            stopped = true;
        }

        Poller* poller() override {
            return poller_;
        }

        Scheduler* scheduler() override {
            return scheduler_;
        }

        Window* createWindow(ObjPool& windowOwner, const WindowOptions& options) override {
            WindowHeadlessImpl* const window = windowOwner.make<WindowHeadlessImpl>(options);
            window->clipboard_.platform = this;
            windows.push_back(window);
            return window;
        }

        LoopWake* createLoopWake(ObjPool& wakeOwner, TimerCallback& callback) override {
            return LoopWake::create(wakeOwner, *poller_, callback);
        }

        PollerLoop* poller_ = nullptr;
        SmallObjAllocator* allocator_ = nullptr;
        Scheduler* scheduler_ = nullptr;
        std::vector<WindowHeadlessImpl*> windows;
        bool stopped = false;
    };
}

WindowHeadlessImpl::WindowHeadlessImpl(const WindowOptions& options)
    : events(options.events)
    , frame(options.frame)
{
    info_.x = 10;
    info_.y = 20;
    info_.width = std::max(1u, options.width);
    info_.height = std::max(1u, options.height);
    info_.screenPixelWidth = 1920;
    info_.screenPixelHeight = 1080;
    info_.contentScale = 1.0f;
}

void WindowHeadlessImpl::requestShow() {
    shown_ = true;
    requestFrame();
}

void WindowHeadlessImpl::requestHide() {
    // No visible frame to withhold in the headless harness, but shown_
    // still flips so visible() gives a real show/hide cycle for tests to
    // assert against - the one thing this backend can offer here.
    shown_ = false;
}

void WindowHeadlessImpl::requestShowAt(ShowPlacement) {
    // Single fixed virtual screen, so every placement is the same show.
    requestShow();
}

void WindowHeadlessImpl::requestClose() {
    closed_ = true;
    if (events != nullptr) {
        events->close();
    }
}

void WindowHeadlessImpl::requestFrame() {
    if (!closed_) {
        pending_ = true;
    }
}

void WindowHeadlessImpl::requestTitle(StringView title) {
    title_.assign(title.data(), title.data() + title.length());
}

void WindowHeadlessImpl::requestAttention() {
}

void WindowHeadlessImpl::requestRestore() {
    info_.iconified = false;
}

void WindowHeadlessImpl::requestIconify() {
    info_.iconified = true;
}

void WindowHeadlessImpl::requestMove(i32 x, i32 y) {
    info_.x = x;
    info_.y = y;
}

void WindowHeadlessImpl::requestFocus() {
    info_.focused = true;
}

void WindowHeadlessImpl::restoreSize() {
    if (!haveRestored_) {
        return;
    }
    info_.width = restored_.width;
    info_.height = restored_.height;
    haveRestored_ = false;
    requestFrame();
}

void WindowHeadlessImpl::requestMaximized(bool maximized) {
    if (maximized == info_.maximized) {
        return;
    }
    if (maximized) {
        if (!haveRestored_) {
            restored_ = info_;
            haveRestored_ = true;
        }
        info_.width = std::max(1u, info_.screenPixelWidth);
        info_.height = std::max(1u, info_.screenPixelHeight);
    } else if (!info_.fullscreen) {
        restoreSize();
    }
    info_.maximized = maximized;
    requestFrame();
}

void WindowHeadlessImpl::requestFullscreen(bool fullscreen) {
    if (fullscreen == info_.fullscreen) {
        return;
    }
    if (fullscreen) {
        if (!haveRestored_) {
            restored_ = info_;
            haveRestored_ = true;
        }
        info_.width = std::max(1u, info_.screenPixelWidth);
        info_.height = std::max(1u, info_.screenPixelHeight);
    } else if (!info_.maximized) {
        restoreSize();
    }
    info_.fullscreen = fullscreen;
    requestFrame();
    // Cocoa commits the fullscreen transition's Core Animation transaction
    // inside the request: the layer displays and the frame callback runs
    // before toggleFullScreen returns, even during startup when the caller
    // has not finished wiring itself (issue 116). Deliver the frame with
    // the same reentrancy so embedders face it on every platform.
    dispatchFrame();
}

// No visible surface to round; same scope as
// WindowOptions::quickCornerRadius already documents (window.h).
void WindowHeadlessImpl::requestCornerRadius(u16) {
}

void WindowHeadlessImpl::requestResize(u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }
    info_.width = width;
    info_.height = height;
    requestFrame();
}

// Recorded rather than ignored: these two are the only geometry a
// window is told about that it never reports back, so a caller that
// pairs a width with a height axis has no observable effect anywhere -
// on a real desktop the window manager quietly obeys the swap. Keeping
// the pair lets a test read the hand-off itself instead of only the
// arithmetic that produced it.
void WindowHeadlessImpl::requestMinimumSize(u32 width, u32 height) {
    minimumSize_.width = width;
    minimumSize_.height = height;
    ++minimumSize_.count;
}

void WindowHeadlessImpl::requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeUnit_.width = width;
    resizeUnit_.height = height;
    resizeUnit_.baseWidth = baseWidth;
    resizeUnit_.baseHeight = baseHeight;
    ++resizeUnit_.count;
}

WindowSizeRequest WindowHeadlessImpl::requestedMinimumSize() const {
    return minimumSize_;
}

WindowResizeUnitRequest WindowHeadlessImpl::requestedResizeUnit() const {
    return resizeUnit_;
}

Clipboard* WindowHeadlessImpl::primary() {
    return primary_;
}

Clipboard* WindowHeadlessImpl::secondary() {
    return secondary_;
}

void WindowHeadlessImpl::setClipboards(Clipboard& primary, Clipboard& secondary) {
    primary_ = &primary;
    secondary_ = &secondary;
}

HeadlessClipboardInput::HeadlessClipboardInput(SmallObjAllocator* allocator_)
    : allocator(allocator_)
{
}

void HeadlessClipboardInput::operator delete(HeadlessClipboardInput* input, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = input->allocator;
    owner->release(input);
}

size_t HeadlessClipboardInput::readImpl(void*, size_t) {
    return 0;
}

HeadlessClipboardOutput::HeadlessClipboardOutput(SmallObjAllocator* allocator_)
    : allocator(allocator_)
{
}

void HeadlessClipboardOutput::operator delete(HeadlessClipboardOutput* output, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = output->allocator;
    owner->release(output);
}

size_t HeadlessClipboardOutput::writeImpl(const void*, size_t size) {
    return size;
}

Input* ClipboardHeadless::read() {
    return platform->allocator_->make<HeadlessClipboardInput>(platform->allocator_);
}

Output* ClipboardHeadless::write() {
    return platform->allocator_->make<HeadlessClipboardOutput>(platform->allocator_);
}


void WindowHeadlessImpl::requestPointerIcon(PointerIcon icon) {
    icon_ = icon;
}

void WindowHeadlessImpl::requestOpenUri(StringView uri) {
    uri_.assign(uri.data(), uri.data() + uri.length());
    ++openCount_;
}

PointerIcon WindowHeadlessImpl::pointerIcon() const {
    return icon_;
}

StringView WindowHeadlessImpl::openedUri() const {
    return StringView(uri_.data(), uri_.size());
}

u64 WindowHeadlessImpl::openUriCount() const {
    return openCount_;
}

StringView WindowHeadlessImpl::title() const {
    return StringView(title_.data(), title_.size());
}

// Recorded for the same reason as the sizing pair above: the anchor is
// where the input method draws its candidate window, and nothing else in
// the tree reads it back. Its x comes from the horizontal inset and its y
// from the vertical one - a swap that was equivalent arithmetic until
// window chrome began reserving a single edge, and a candidate window a
// title bar's height above the caret afterwards.
void WindowHeadlessImpl::requestTextInputRect(i32 x, i32 y, u32 width, u32 height) {
    textInputRect_.x = x;
    textInputRect_.y = y;
    textInputRect_.width = width;
    textInputRect_.height = height;
    ++textInputRect_.count;
}

WindowTextInputRect WindowHeadlessImpl::requestedTextInputRect() const {
    return textInputRect_;
}

bool WindowHeadlessImpl::inLiveResize() const {
    return false;
}

WindowInfo WindowHeadlessImpl::info() const {
    return info_;
}

bool WindowHeadlessImpl::visible() const {
    return shown_;
}

RenderContext WindowHeadlessImpl::renderContext() const {
    return {
        .backend = RenderBackend::Headless,
        .connection = nullptr,
        .window = &target_,
    };
}

void WindowHeadlessImpl::resizeBackBuffer() {
    const size_t length = (size_t)(info_.width) * info_.height * 3;
    back_.resize(length);
    target_.pixels = back_.data();
    target_.length = back_.size();
    target_.width = info_.width;
    target_.height = info_.height;
    target_.stride = info_.width * 3;
}

bool WindowHeadlessImpl::dispatchFrame() {
    if (!pending_) {
        return false;
    }
    pending_ = false;
    resizeBackBuffer();
    const bool fail = failNext_;
    failNext_ = false;
    u8* const pixels = target_.pixels;
    const size_t length = target_.length;
    if (fail) {
        target_.pixels = nullptr;
        target_.length = 0;
    }
    const WindowInfo frameInfo = info_;
    const bool presented = frame != nullptr && frame->frame(frameInfo);
    target_.pixels = pixels;
    target_.length = length;
    if (!presented) {
        return false;
    }
    front_.swap(back_);
    frontWidth_ = frameInfo.width;
    frontHeight_ = frameInfo.height;
    ++generation_;
    target_.pixels = back_.data();
    target_.length = back_.size();
    return true;
}

bool WindowHeadlessImpl::framePending() const {
    return pending_;
}

void WindowHeadlessImpl::configure(const WindowInfo& info) {
    info_ = info;
    info_.width = std::max(1u, info_.width);
    info_.height = std::max(1u, info_.height);
    if (!(info_.contentScale > 0.0f)) {
        info_.contentScale = 1.0f;
    }
    requestFrame();
}

void WindowHeadlessImpl::failNextPresentation() {
    failNext_ = true;
    requestFrame();
}

HeadlessFrame WindowHeadlessImpl::presentedFrame() const {
    return {
        .pixels = front_.data(),
        .length = front_.size(),
        .width = frontWidth_,
        .height = frontHeight_,
        .stride = frontWidth_ * 3,
        .generation = generation_,
    };
}

Platform* plt::createHeadlessPlatform(ObjPool& owner) {
    PlatformHeadless* const platform = owner.make<PlatformHeadless>();
    platform->poller_ = PollerLoop::create(owner);
    platform->allocator_ = SmallObjAllocator::create(&owner);
    platform->scheduler_ = Scheduler::create(owner, *platform->poller_);
    return platform;
}
