#include "platform_cocoa.h"

#include "drop.h"
#include "fiber.h"
#include "input.h"
#include "loop_wake.h"
#include "poller.h"
#include "poller_loop.h"
#include "window.h"
#include "platform.h"

#include <std/sys/crt.h>
#include <dlfcn.h>
#include <std/dbg/verify.h>
#include <std/sym/i_map.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/list.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/thr/poll_fd.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#import <AppKit/AppKit.h>
#import <mach/mach.h>
#import <Carbon/Carbon.h>
#import <CoreVideo/CVDisplayLink.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <QuartzCore/CAMetalLayer.h>

// @available guards the runtime, but building against an older SDK also
// needs the declarations to exist at all; these gate every use of an API
// newer than the SDK the build runs on.
#if defined(MAC_OS_VERSION_15_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
#define PLT_SDK_MACOS_15 1
#else
#define PLT_SDK_MACOS_15 0
#endif

#if defined(MAC_OS_VERSION_26_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_26_0
#define PLT_SDK_MACOS_26 1
#else
#define PLT_SDK_MACOS_26 0
#endif

#if defined(MAC_OS_VERSION_27_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_27_0
#define PLT_SDK_MACOS_27 1
#else
#define PLT_SDK_MACOS_27 0
#endif

#include <dispatch/dispatch.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <new>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

using namespace stl;
using namespace plt;

unsigned long plt::cocoaWindowStyleMask(bool decorations) {
    if (!decorations) {
        return NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable;
    }
    return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
}

bool plt::cocoaResizeUsesExactProposal(bool fullscreen, bool viewAvailable, bool liveResize) {
    return fullscreen || (viewAvailable && !liveResize);
}

namespace plt::cocoa_detail {
    struct DisplayLinkGate {
        void attach(void* owner) {
            __atomic_store_n(&owner_, owner, __ATOMIC_RELEASE);
        }

        void detach() {
            __atomic_store_n(&owner_, nullptr, __ATOMIC_RELEASE);
        }

        void* owner() const {
            return __atomic_load_n(&owner_, __ATOMIC_ACQUIRE);
        }

        bool schedule() {
            bool expected = false;
            return __atomic_compare_exchange_n(&scheduled_, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }

        void dispatched() {
            __atomic_store_n(&scheduled_, false, __ATOMIC_RELEASE);
        }

        void* owner_ = nullptr;
        bool scheduled_ = false;
    };

}

void cocoaCloseImpl(void* owner);
void cocoaResizeImpl(void* owner);
void cocoaFrameImpl(void* owner);
void cocoaDisplayLayerImpl(void* owner);
void cocoaInvalidateImpl(void* owner);
void cocoaScreenChangedImpl(void* owner);
NSRect cocoaTextInputRectImpl(void* owner);
NSSize cocoaWillResizeImpl(void* owner, NSSize frameSize);
void cocoaFocusImpl(void* owner, bool focused);
void cocoaKeyImpl(void* owner, NSEvent* event, bool pressed);
void cocoaTextImpl(void* owner, NSString* text, NSEventModifierFlags modifiers);
void cocoaPreeditImpl(void* owner, NSString* text);
void cocoaFlushInputImpl(void* owner);
void cocoaFlagsImpl(void* owner, NSEvent* event);
void cocoaPointerImpl(void* owner, NSEvent* event);
void cocoaButtonImpl(void* owner, NSEvent* event, bool pressed);
void cocoaScrollImpl(void* owner, NSEvent* event);
void cocoaPointerPresenceImpl(void* owner, bool present);
NSDragOperation cocoaDragOverImpl(void* owner, id<NSDraggingInfo> sender);
void cocoaDragExitedImpl(void* owner);
BOOL cocoaPerformDropImpl(void* owner, id<NSDraggingInfo> sender);
void cocoaFileDescriptorReady(CFFileDescriptorRef descriptor, CFOptionFlags types, void* owner);
void cocoaTimerReady(CFRunLoopTimerRef timer, void* owner);
void cocoaWakeReady(CFMachPortRef port, void* message, CFIndex size, void* owner);

@interface PltWindow: NSWindow
@end

@interface PltWindowDelegate: NSObject <NSWindowDelegate>
@property(nonatomic, assign) void* owner;
@end

@interface PltView: NSView <NSTextInputClient, NSDraggingDestination> {
    NSMutableAttributedString* markedText_;
    NSRange selectedTextRange_;
    NSMutableSet<NSNumber*>* composedKeys_;
}
@property(nonatomic, assign) void* owner;
@property(nonatomic, strong) NSTrackingArea* tracking;
@end


// T10. The blurred backdrop -backgroundBlur asks for: the desktop
// behind the window, blurred, sitting under everything the window draws
// so a translucent terminal shows blur instead of raw wallpaper.
@interface PltBackdropView: NSVisualEffectView
@end

#if PLT_SDK_MACOS_26
// The same job as PltBackdropView above, in the same place, out of the
// system's own glass instead of a frosted pane: -backgroundBlur glass.
//
// The name carries no product brand on purpose. Two binaries are built
// from this tree under two brands, and strip removes symbols but not
// Objective-C class names or selector literals, so a class named after
// one brand would still be a substring of the other's binary - which
// tst/pretty_binary_branding.py rejects. Plt* is the neutral prefix
// this file already uses.
API_AVAILABLE(macos(26.0))
@interface PltGlassView: NSGlassEffectView {
@public
    // The floor a macOS 27 corner configuration falls back to; see the
    // -cornerConfiguration override below for why the radius stops being a
    // plain number there. Written by WindowImpl::applyCornerRadius(), which
    // owns every other statement of this window's corner radius too.
    CGFloat concentricFloor;
}
@end
#endif

@interface PltDisplayLinkTarget: NSObject {
@public
    plt::cocoa_detail::DisplayLinkGate gate;
}
@end

@implementation PltWindow

- (BOOL)canBecomeKeyWindow {
    return YES;
}

@end

@implementation PltWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    cocoaCloseImpl(self.owner);
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    cocoaResizeImpl(self.owner);
}

- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize {
    (void)sender;
    return cocoaWillResizeImpl(self.owner, frameSize);
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    cocoaResizeImpl(self.owner);
}

- (void)windowDidMove:(NSNotification*)notification {
    (void)notification;
    cocoaInvalidateImpl(self.owner);
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
    (void)notification;
    cocoaScreenChangedImpl(self.owner);
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    (void)notification;
    cocoaInvalidateImpl(self.owner);
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    (void)notification;
    cocoaInvalidateImpl(self.owner);
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    cocoaFocusImpl(self.owner, true);
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    cocoaFocusImpl(self.owner, false);
}

@end

@implementation PltView

- (CALayer*)makeBackingLayer {
    // A CAMetalLayer the Metal renderer configures (device, pixel format,
    // presentsWithTransaction) once created. needsDisplayOnBoundsChange makes
    // CoreAnimation call our displayLayer: whenever the bounds change, including
    // synchronously during a live resize, so we render the resize frame inside
    // the same transaction as the bounds change.
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.needsDisplayOnBoundsChange = YES;
    return layer;
}

// CoreAnimation's synchronous display pass. During a live resize AppKit calls
// this while assembling the resize transaction, so the frame we render here
// commits together with the new bounds.
- (void)displayLayer:(CALayer*)layer {
    (void)layer;
    cocoaDisplayLayerImpl(self.owner);
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

// NSTrackingActiveInKeyWindow, and it stays that way: this area is what
// feeds the terminal's pointer reporting (cocoaPointerImpl and the
// enter/leave pair below), and a program running inside a window the
// user is not typing into has no business being told the pointer swept
// across it on its way somewhere else. Chrome that has to react in an
// inactive window - the auto-hiding title bar, which must reveal its
// buttons before they can be clicked (A7, ui_csd_tabs.mm) - brings its
// own NSTrackingActiveAlways area for its own strip rather than
// widening this one, so that behaviour is scoped to the option that
// asked for it instead of landing on every window.
- (void)updateTrackingAreas {
    if (self.tracking != nil) {
        [self removeTrackingArea:self.tracking];
    }
    self.tracking = [[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow owner:self userInfo:nil];
    [self addTrackingArea:self.tracking];
    [super updateTrackingAreas];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return cocoaDragOverImpl(self.owner, sender);
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return cocoaDragOverImpl(self.owner, sender);
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    cocoaDragExitedImpl(self.owner);
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    (void)sender;
    return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    return cocoaPerformDropImpl(self.owner, sender);
}

- (void)keyDown:(NSEvent*)event {
    // While the input method composes, the event belongs to the IME:
    // Enter picks a candidate, arrows and Escape navigate the candidate
    // window. Delivering it to the terminal too would double every key.
    // The matching release is swallowed as well: the press was never
    // seen, so an orphan release must not leak (kitty keyboard protocol
    // reports releases).
    if ([self hasMarkedText]) {
        if (composedKeys_ == nil) {
            composedKeys_ = [NSMutableSet set];
        }
        [composedKeys_ addObject:@(event.keyCode)];
    } else {
        cocoaKeyImpl(self.owner, event, true);
    }
    [self interpretKeyEvents:@[ event ]];
    cocoaFlushInputImpl(self.owner);
}

- (void)keyUp:(NSEvent*)event {
    NSNumber* const code = @(event.keyCode);
    if ([composedKeys_ containsObject:code]) {
        [composedKeys_ removeObject:code];
        return;
    }
    cocoaKeyImpl(self.owner, event, false);
    cocoaFlushInputImpl(self.owner);
}

- (void)flagsChanged:(NSEvent*)event {
    cocoaFlagsImpl(self.owner, event);
}

- (void)mouseMoved:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)mouseDragged:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)rightMouseDragged:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)otherMouseDragged:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)mouseDown:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, true);
}

- (void)mouseUp:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, false);
}

- (void)rightMouseDown:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, true);
}

- (void)rightMouseUp:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, false);
}

- (void)otherMouseDown:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, true);
}

- (void)otherMouseUp:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, false);
}

- (void)scrollWheel:(NSEvent*)event {
    cocoaScrollImpl(self.owner, event);
}

- (void)insertText:(id)value replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString* const text = [value isKindOfClass:[NSAttributedString class]] ? [value string] : (NSString*)(value);
    [self unmarkText];
    NSEvent* const event = NSApp.currentEvent;
    cocoaTextImpl(self.owner, text, event == nil ? 0 : event.modifierFlags);
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

- (BOOL)hasMarkedText {
    return markedText_.length != 0;
}

- (NSRange)markedRange {
    return markedText_.length == 0 ? NSMakeRange(NSNotFound, 0) : NSMakeRange(0, markedText_.length);
}

- (NSRange)selectedRange {
    return markedText_.length == 0 ? NSMakeRange(NSNotFound, 0) : selectedTextRange_;
}

- (void)setMarkedText:(id)value selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if ([value isKindOfClass:[NSAttributedString class]]) {
        markedText_ = [[NSMutableAttributedString alloc] initWithAttributedString:value];
    } else {
        markedText_ = [[NSMutableAttributedString alloc] initWithString:value];
    }
    selectedTextRange_ = selectedRange;
    if (markedText_.length == 0) {
        [self unmarkText];
        return;
    }
    cocoaPreeditImpl(self.owner, markedText_.string);
}

- (void)unmarkText {
    markedText_ = nil;
    selectedTextRange_ = NSMakeRange(NSNotFound, 0);
    cocoaPreeditImpl(self.owner, nil);
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (markedText_.length == 0 || range.location == NSNotFound || NSMaxRange(range) > markedText_.length) {
        return nil;
    }
    if (actualRange != nullptr) {
        *actualRange = range;
    }
    return [markedText_ attributedSubstringFromRange:range];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange != nullptr) {
        *actualRange = range;
    }
    if (self.owner == nullptr) {
        return [self.window convertRectToScreen:NSMakeRect(0, 0, 0, 0)];
    }
    return cocoaTextInputRectImpl(self.owner);
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    cocoaPointerPresenceImpl(self.owner, true);
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    cocoaPointerPresenceImpl(self.owner, false);
}

@end

@implementation PltBackdropView

// Invisible to the event system, the same nil TerminalTitlebarFillView
// returns (ui_csd_tabs.mm) and for a stricter reason. This view is the
// full size of the window's frame, so it lies under the content view,
// under the title bar container, and under whatever the sidebar and the
// title-bar strip put in either. It draws and nothing else: two defects
// of the class "input arrived somewhere it was not aimed" have already
// cost this plan a review, and a backdrop that could take a click would
// be a third waiting to happen. Returning nil is not merely
// click-through - it removes this view from hit testing entirely, so
// every gesture lands exactly where it landed before the view existed.
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

@end

#if PLT_SDK_MACOS_26
@implementation PltGlassView

// Invisible to the event system for exactly the reasons spelled out on
// PltBackdropView's own hitTest: above. NSGlassEffectView is an ordinary
// NSView subclass and hit-tests like one, so the reasoning carries over
// unchanged - this view is the full size of the window's frame, and
// without this every gesture that missed everything above it would land
// on a decoration.
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

#if PLT_SDK_MACOS_27
// macOS 27 shapes a glass view from NSView.cornerConfiguration rather than
// from the cornerRadius property, and that property is readonly - a view
// states its shape by overriding this getter, the way it states a size by
// overriding intrinsicContentSize. Measured on a probe: where both are set,
// the configuration is what the glass takes (a 260x84 sheet with
// cornerRadius 6 and a capsule configuration comes out a capsule).
//
// Concentric rather than fixed, because this view's container is AppKit's
// own frame view and that view knows the window's real shape while this
// process only knows the number an option asked for. Measured on the same
// probe: inside a titled window NSThemeFrame reports 16 points of radius on
// this machine, and a glass sibling of the content view asking for
// containerConcentric with a floor of 12 resolves to 16 - the window's
// shape, not the guess. Inside a borderless window NSNextStepFrame declares
// no shape at all, the floor resolves unchanged, and the result is exactly
// the number cornerRadius carried before.
- (NSViewCornerConfiguration*)cornerConfiguration API_AVAILABLE(macos(27.0)) {
    return [NSViewCornerConfiguration configurationWithRadius:
        [NSViewCornerRadius containerConcentricRadiusWithMinimum:concentricFloor]];
}
#endif

@end
#endif

@implementation PltDisplayLinkTarget
@end

namespace {
    struct PlatformImpl;
    struct PollerImpl;
    struct WindowImpl;
    const StringView uriListMime(u8"text/uri-list");
    const StringView utf8Mime(u8"text/plain;charset=utf-8");

    // The DropOffer view over one dragging pasteboard, valid for the
    // duration of a DropTarget callback. Pasteboard types map onto mimes:
    // strings arrive as utf-8 text, file URLs as a text/uri-list.
    struct CocoaDropOffer final: public DropOffer {
        size_t formats() const override;
        StringView format(size_t index) const override;

        bool text = false;
        bool files = false;
    };

    struct CocoaDrop final: public Drop {
        DropOffer* what() override;
        Input* read(StringView mime) override;

        WindowImpl* window = nullptr;
        CocoaDropOffer* view = nullptr;
        NSPasteboard* pasteboard = nil;
        bool taken = false;
        bool drained = false;
    };

    // A synchronously materialized payload as a pulling stream; plain
    // delete releases it, and drained reports whether the consumer reached
    // end of payload before deleting.
    struct CocoaStreamInput final: public Input {
        CocoaStreamInput(SmallObjAllocator* allocator, Buffer&& content, bool* drained);
        ~CocoaStreamInput() noexcept override;

        void operator delete(CocoaStreamInput* input, std::destroying_delete_t) noexcept;

        size_t readImpl(void* data, size_t len) override;

        SmallObjAllocator* allocator;
        Buffer content;
        size_t offset = 0;
        bool* drained;
    };

    // A replacement pasteboard payload accumulating until finish()
    // publishes it; deleting without finish() abandons the write.
    struct CocoaStreamOutput final: public Output {
        CocoaStreamOutput(WindowImpl* window, bool primary);

        void operator delete(CocoaStreamOutput* output, std::destroying_delete_t) noexcept;

        size_t writeImpl(const void* data, size_t size) override;
        void finishImpl() override;

        WindowImpl* window;
        Buffer accumulated;
        bool primary;
        bool finished = false;
    };

    // One watched descriptor with every waiter parked on it.
    struct ArmedFD {
        ArmedFD(CFFileDescriptorRef descriptor, CFRunLoopSourceRef source);
        ~ArmedFD();

        CFFileDescriptorRef descriptor = nullptr;
        CFRunLoopSourceRef source = nullptr;
        stl::IntrusiveList waiters;
    };

    struct PollerImpl final: public Poller {
        explicit PollerImpl(ObjPool& owner);
        ~PollerImpl();

        void arm(PollWaiter& waiter) override;
        void cancel(PollWaiter& waiter) override;
        void timeout(u64 microseconds, TimerCallback& callback) override;
        void deadline(u64 monotonicMicroseconds, TimerCallback& callback) override;
        void cancel(TimerCallback& callback) override;
        void defer(TimerCallback& callback) override;

        void descriptorReady(CFFileDescriptorRef descriptor);
        void dispatchTimers();
        void scheduleTimer();
        u64 nextDeadline() const;

        IntMap<ArmedFD> armed;
        // The portable loop poller serves as the deadline queue; its poll
        // half is never used - CFFileDescriptor delivers readiness.
        PollerLoop* timers = nullptr;
        CFRunLoopTimerRef runLoopTimer = nullptr;
    };

    struct ClipboardImpl final: public Clipboard {
        Input* read() override;
        Output* write() override;

        WindowImpl* window = nullptr;
        bool primary = false;
    };

    // How long an idle display link keeps ticking before it is stopped,
    // in its own callbacks: about a second at 60Hz, less on a faster
    // panel, which is the scale of a pause between two keystrokes.
    constexpr u32 idleFramesBeforeStop = 60;

    struct WindowImpl final: public Window {
        WindowImpl(PlatformImpl& platform, const WindowOptions& options);
        ~WindowImpl();

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
        WindowInfo info() const override;
        bool visible() const override;
        bool inLiveResize() const override;
        Clipboard* primary() override;
        Clipboard* secondary() override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestOpenUri(StringView uri) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        RenderContext renderContext() const override;

        void close();
        void resized();
        void resizeFrame();
        NSRect topOfActiveScreenFrame() const;
        void startDisplayLink();
        void screenChanged();
        NSRect textInputScreenRect() const;
        void draw();
        void stopDisplayLink();
        NSSize willResize(NSSize frameSize) const;
        void focused(bool value);
        void key(NSEvent* event, bool pressed);
        void flushInput();
        void preeditChanged(NSString* text);
        void flags(NSEvent* event);
        void pointer(NSEvent* event);
        void button(NSEvent* event, bool pressed);
        void scroll(NSEvent* event);
        void pointerPresence(bool present);
        void emitText(NSString* string, u16 modifiers);
        NSPoint pointerPosition(NSEvent* event) const;
        void writePasteboard(NSPasteboard* pasteboard, StringView content);
        NSDragOperation dragOver(id<NSDraggingInfo> sender);
        void dragExited();
        BOOL performDrop(id<NSDraggingInfo> sender);
        void applySizeConstraints();
        void applyCornerRadius();
        // The one-time hand-off of window.backgroundColor, described at
        // its definition. Two unrelated options need it and neither may
        // take it twice.
        void establishFrameTransparency();

        PlatformImpl& platform;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        DropTarget* dropTarget = nullptr;
        // Mirrors WindowOptions::quick: gates the quick-terminal-only
        // behavior added in focused() (hide on key resign) since that
        // path has no options struct at hand, only the live window state.
        bool quick = false;
        // Mirrors WindowOptions::quickGeometry; resolved against a live
        // screen in topOfActiveScreenFrame(), the only reader.
        QuickGeometry quickGeometry;
        NSWindow* window = nil;
        PltView* view = nil;
        // The PltGlassView this window was given, or nil - either
        // because the backdrop is the frosted PltBackdropView, or
        // because there is no backdrop at all. Held only so
        // applyCornerRadius() can keep the glass's own corners in step
        // with the window's; typed NSView* so the field itself needs no
        // availability annotation, and cast back inside the one
        // @available scope that touches it.
        NSView* glassBackdrop = nil;
        PltWindowDelegate* delegate = nil;
        CVDisplayLinkRef displayLink = nullptr;
        PltDisplayLinkTarget* displayLinkTarget = nil;
        void* displayLinkContext = nullptr;
        i32 textInputX = 0;
        i32 textInputY = 0;
        u32 textInputWidth = 0;
        u32 textInputHeight = 0;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        u32 resizeUnitWidth = 1;
        u32 resizeUnitHeight = 1;
        u32 resizeBaseWidth = 0;
        u32 resizeBaseHeight = 0;
        // The radius requestCornerRadius() was last asked for, in points.
        // Held rather than written straight through because the layer it
        // lands on does not exist yet at the moment the option is read;
        // see applyCornerRadius().
        u16 cornerRadius = 0;
        ClipboardImpl primaryPasteboard;
        ClipboardImpl generalPasteboard;
        bool frameRequested = false;
        u32 idleFrames = 0;
        bool preeditShown = false;
    };

    // The run loop natively sleeps on a mach port set; a port message is
    // its cheapest cross-thread wake. Send-once rights die with delivery,
    // and a zero send timeout makes a full queue mean "wake already
    // pending" instead of blocking the signalling thread.
    struct MachLoopWake final: public LoopWake {
        explicit MachLoopWake(TimerCallback& callback_)
            : callback(callback_)
        {
            CFMachPortContext context{};
            context.info = this;
            port = CFMachPortCreate(kCFAllocatorDefault, cocoaWakeReady, &context, nullptr);
            STD_VERIFY(port != nullptr);
            CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, port, 0);
            STD_VERIFY(source != nullptr);
            CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
            CFRelease(source);
        }

        void signal() override {
            mach_msg_header_t header{};
            header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND_ONCE, 0);
            header.msgh_remote_port = CFMachPortGetPort(port);
            header.msgh_size = sizeof(header);
            mach_msg(&header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(header), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
        }

        TimerCallback& callback;
        CFMachPortRef port = nullptr;
    };

    struct PlatformImpl final: public Platform {
        explicit PlatformImpl(ObjPool& owner);

        Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
        LoopWake* createLoopWake(ObjPool& owner, TimerCallback& callback) override;
        Poller* poller() override;
        Scheduler* scheduler() override;
        void run() override;
        void stop() override;

        void ensureApplication(StringView appName, bool quick);

        PollerImpl* poller_ = nullptr;
        SmallObjAllocator* allocator_ = nullptr;
        Scheduler* scheduler_ = nullptr;
        bool applicationReady_ = false;
        bool stopRequested_ = false;
    };

    NSString* stringFromView(StringView value) {
        return [[NSString alloc] initWithBytes:value.data() length:value.length() encoding:NSUTF8StringEncoding];
    }

    // Mirrors NSCursorFrameResizePosition, whose name cannot appear outside
    // an @available(macOS 15) scope without an availability warning.
    constexpr NSUInteger frameResizeTop = 1 << 0;
    constexpr NSUInteger frameResizeLeft = 1 << 1;
    constexpr NSUInteger frameResizeBottom = 1 << 2;
    constexpr NSUInteger frameResizeRight = 1 << 3;

    NSCursor* frameResizeCursor(NSUInteger position, NSCursor* fallback) {
#if PLT_SDK_MACOS_15
        if (@available(macOS 15.0, *)) {
            return [NSCursor frameResizeCursorFromPosition:(NSCursorFrameResizePosition)(position) inDirections:NSCursorFrameResizeDirectionsAll];
        }
#else
        (void)position;
#endif
        return fallback;
    }

    NSCursor* pointerCursor(PointerIcon icon) {
        switch (icon) {
            case PointerIcon::Default:
                return [NSCursor arrowCursor];
            case PointerIcon::ContextMenu:
                return [NSCursor contextualMenuCursor];
            case PointerIcon::Help:
                // AppKit has no public help cursor.
                return [NSCursor arrowCursor];
            case PointerIcon::Pointer:
                return [NSCursor pointingHandCursor];
            case PointerIcon::Progress:
            case PointerIcon::Wait:
                // AppKit shows the system busy cursor on its own; a stand-in
                // does not exist in the public API.
                return [NSCursor arrowCursor];
            case PointerIcon::Cell:
            case PointerIcon::Crosshair:
                return [NSCursor crosshairCursor];
            case PointerIcon::Text:
                return [NSCursor IBeamCursor];
            case PointerIcon::VerticalText:
                return [NSCursor IBeamCursorForVerticalLayout];
            case PointerIcon::Alias:
                return [NSCursor dragLinkCursor];
            case PointerIcon::Copy:
                return [NSCursor dragCopyCursor];
            case PointerIcon::DndAsk:
                // The undecided drag has no own cursor; copy is the usual
                // visual until the target picks the operation.
                return [NSCursor dragCopyCursor];
            case PointerIcon::Move:
            case PointerIcon::AllScroll:
            case PointerIcon::ResizeAll:
            case PointerIcon::Grab:
                // The open hand is the only omnidirectional-manipulation
                // cursor AppKit offers.
                return [NSCursor openHandCursor];
            case PointerIcon::Grabbing:
                return [NSCursor closedHandCursor];
            case PointerIcon::NoDrop:
            case PointerIcon::NotAllowed:
                return [NSCursor operationNotAllowedCursor];
            case PointerIcon::ResizeEast:
                return [NSCursor resizeRightCursor];
            case PointerIcon::ResizeNorth:
                return [NSCursor resizeUpCursor];
            case PointerIcon::ResizeSouth:
                return [NSCursor resizeDownCursor];
            case PointerIcon::ResizeWest:
                return [NSCursor resizeLeftCursor];
            case PointerIcon::ResizeEastWest:
                return [NSCursor resizeLeftRightCursor];
            case PointerIcon::ResizeNorthSouth:
                return [NSCursor resizeUpDownCursor];
            // Diagonal resize cursors are public API only since macOS 15;
            // older systems fall back to the horizontal resize cursor, the
            // closest generic resize visual.
            case PointerIcon::ResizeNorthEast:
            case PointerIcon::ResizeNorthEastSouthWest:
                return frameResizeCursor(frameResizeTop | frameResizeRight, [NSCursor resizeLeftRightCursor]);
            case PointerIcon::ResizeNorthWest:
            case PointerIcon::ResizeNorthWestSouthEast:
                return frameResizeCursor(frameResizeTop | frameResizeLeft, [NSCursor resizeLeftRightCursor]);
            case PointerIcon::ResizeSouthEast:
                return frameResizeCursor(frameResizeBottom | frameResizeRight, [NSCursor resizeLeftRightCursor]);
            case PointerIcon::ResizeSouthWest:
                return frameResizeCursor(frameResizeBottom | frameResizeLeft, [NSCursor resizeLeftRightCursor]);
            case PointerIcon::ResizeColumn:
#if PLT_SDK_MACOS_15
                if (@available(macOS 15.0, *)) {
                    return [NSCursor columnResizeCursor];
                }
#endif
                return [NSCursor resizeLeftRightCursor];
            case PointerIcon::ResizeRow:
#if PLT_SDK_MACOS_15
                if (@available(macOS 15.0, *)) {
                    return [NSCursor rowResizeCursor];
                }
#endif
                return [NSCursor resizeUpDownCursor];
            case PointerIcon::ZoomIn:
#if PLT_SDK_MACOS_15
                if (@available(macOS 15.0, *)) {
                    return [NSCursor zoomInCursor];
                }
#endif
                // No magnifier before macOS 15; the crosshair at least keeps
                // the aim-at-a-spot meaning.
                return [NSCursor crosshairCursor];
            case PointerIcon::ZoomOut:
#if PLT_SDK_MACOS_15
                if (@available(macOS 15.0, *)) {
                    return [NSCursor zoomOutCursor];
                }
#endif
                return [NSCursor crosshairCursor];
            case PointerIcon::DisappearingItem:
                return [NSCursor disappearingItemCursor];
        }
        return [NSCursor arrowCursor];
    }

    CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* context) {
        PltDisplayLinkTarget* const target = (__bridge PltDisplayLinkTarget*)(context);
        if (!target->gate.schedule()) {
            return kCVReturnSuccess;
        }
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
          target->gate.dispatched();
          void* const owner = target->gate.owner();
          if (owner != nullptr) {
              cocoaFrameImpl(owner);
          }
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return kCVReturnSuccess;
    }

    // Reads the environment variable lib/shitty/quick_companion.cpp sets
    // on a spawned quick-terminal companion right before exec() (the
    // literal is duplicated here on purpose - ext/plt never #includes a
    // lib/shitty header, only names one in comments, same as the
    // quick_geometry cross-references elsewhere in this file and in
    // window.h), and if present, watches that pid for exit through a GCD
    // proc source - GCD's supported wrapper over kqueue's EVFILT_PROC/
    // NOTE_EXIT - delivered on the main queue, which already pumps
    // cooperatively with this file's CFRunLoop (same context the hotkey
    // module's Carbon handling runs in, so this adds no separate thread
    // and no interference with it).
    //
    // This exists because application.cpp's stopQuickCompanion() only
    // reaches the companion on the paths where the parent's own close()
    // or destructor gets to run at all; a signal that skips those -
    // SIGKILL, or a bare SIGTERM with no handler - skips that kill()
    // too, and the companion would otherwise outlive a parent that no
    // longer exists, holding onto the global hotkey with nothing left to
    // show. This is the companion-side half that closes that gap
    // regardless of how the parent went away.
    static void watchParentForExit() {
        const char* const parentPidText = getenv("TERMINAL_QUICK_COMPANION_PARENT_PID");
        if (parentPidText == nullptr) {
            return;
        }
        char* end = nullptr;
        const long parsed = strtol(parentPidText, &end, 10);
        // Unset either way, valid or not: this must not reach the shell
        // this process spawns for itself further down in run(), the same
        // reason application.cpp clears it in its own process right
        // after fork().
        unsetenv("TERMINAL_QUICK_COMPANION_PARENT_PID");
        if (end == parentPidText || *end != '\0' || parsed <= 0) {
            return;
        }
        const pid_t parentPid = (pid_t)(parsed);
        // static, not a plain local: this file compiles with ARC (see
        // -fobjc-arc), where a dispatch_source_t is an ordinary strong
        // Objective-C reference - a local one would be released, and the
        // watch torn down with it, the moment this function returns.
        // static gives it process-duration storage instead, matching the
        // "never released on purpose" intent below.
        static dispatch_source_t parentWatch;
        parentWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)(parentPid), DISPATCH_PROC_EXIT, dispatch_get_main_queue());
        if (parentWatch == nil) {
            return;
        }
        dispatch_source_set_event_handler(parentWatch, ^{
          _exit(0);
        });
        dispatch_resume(parentWatch);
        // Never released on purpose: the watch has to outlive every other
        // object in the process, and both ways this process can still end
        // - the handler above, or something else killing it outright -
        // make releasing it moot.
        if (getppid() != parentPid) {
            // Already reparented to launchd by the time the watch above
            // registered - the parent died in the fork()/exec() window,
            // before EVFILT_PROC had a live process left to attach to.
            // Same outcome as the watch firing, forced by hand since the
            // watch itself will never see it.
            _exit(0);
        }
    }
}

PlatformImpl::PlatformImpl(ObjPool& owner)
    : poller_(owner.make<PollerImpl>(owner))
    , allocator_(SmallObjAllocator::create(&owner))
    , scheduler_(Scheduler::create(owner, *poller_))
{
}

NSMenu* plt::cocoaBuildMainMenu(NSString* appName) {
    NSMenu* const bar = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* const applicationItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [bar addItem:applicationItem];

    NSMenu* const application = [[NSMenu alloc] initWithTitle:appName];
    [application addItemWithTitle:[@"About " stringByAppendingString:appName]
                           action:@selector(orderFrontStandardAboutPanel:)
                    keyEquivalent:@""];
    [application addItem:[NSMenuItem separatorItem]];
    [application addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                           action:@selector(hide:)
                    keyEquivalent:@"h"];
    NSMenuItem* const hideOthers = [application addItemWithTitle:@"Hide Others"
                                                          action:@selector(hideOtherApplications:)
                                                   keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [application addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [application addItem:[NSMenuItem separatorItem]];
    [application addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
    applicationItem.submenu = application;
    return bar;
}

void PlatformImpl::ensureApplication(StringView appName, bool quick) {
    if (applicationReady_) {
        return;
    }
    // Independent of quick below: a process re-exec'd by a
    // quickCompanion spawn watches its parent regardless of what its own
    // config happens to set, since the watch is about "something else
    // manages this process's lifetime", not about being a quick window
    // specifically. A no-op for every ordinary launch - the environment
    // variable it looks for is simply absent.
    watchParentForExit();
    // Press and Hold swallows the auto-repeat of every key the system
    // deems accent-capable - which keys those are shifts with layout
    // and OS release, so 'q' stops repeating while 'w' still does.  A
    // terminal wants the repeat; registerDefaults scopes the opt-out
    // to this process without persisting anything.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        @"ApplePressAndHoldEnabled" : @NO
    }];
    [NSApplication sharedApplication];
    // A quick-terminal window gets no Dock icon and no Cmd-Tab entry:
    // Accessory, not Regular. It still becomes the key window and takes
    // keyboard input normally once shown - requestShowAt() already calls
    // makeKeyAndOrderFront: and activateIgnoringOtherApps:, neither of
    // which Accessory refuses - this only changes whether the process
    // itself is Dock/switcher visible. A quickCompanion process is
    // exactly this case: a second `st` process the user did not launch
    // by hand and should not see a second icon for. Every other process
    // (including the one that spawned a quickCompanion) stays Regular:
    // this is gated on quick alone, never on quickCompanion.
    [NSApp setActivationPolicy:quick ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
    // The embedder's name, or the process name - which Foundation
    // always supplies. A platform library has no name of its own to
    // fall back on, and naming one here would put a brand in the layer
    // every brand built out of this tree links (see GenericBrand in
    // lib/shitty/brand.cpp and tst/pretty_binary_branding.py).
    NSString* name = nil;
    if (appName.length() != 0) {
        name = [[NSString alloc] initWithBytes:appName.data() length:appName.length() encoding:NSUTF8StringEncoding];
    }
    if (name == nil) {
        name = [[NSProcessInfo processInfo] processName];
    }
    [NSApp setMainMenu:cocoaBuildMainMenu(name)];
    [NSApp finishLaunching];
    applicationReady_ = true;
}

Window* PlatformImpl::createWindow(ObjPool& owner, const WindowOptions& options) {
    ensureApplication(options.appName, options.quick);
    return owner.make<WindowImpl>(*this, options);
}

LoopWake* PlatformImpl::createLoopWake(ObjPool& owner, TimerCallback& callback) {
    return owner.make<MachLoopWake>(callback);
}

Poller* PlatformImpl::poller() {
    return poller_;
}

Scheduler* PlatformImpl::scheduler() {
    return scheduler_;
}

PollerImpl::PollerImpl(ObjPool& owner)
    : armed(ObjPool::create(&owner))
    , timers(PollerLoop::create(owner))
{
    CFRunLoopTimerContext context{};
    context.info = this;
    runLoopTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, DBL_MAX, 0.000'000'1, 0, 0, cocoaTimerReady, &context);
    STD_VERIFY(runLoopTimer != nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), runLoopTimer, kCFRunLoopCommonModes);
}

PollerImpl::~PollerImpl() {
    CFRunLoopTimerInvalidate(runLoopTimer);
    CFRelease(runLoopTimer);
}

ArmedFD::ArmedFD(CFFileDescriptorRef descriptor_, CFRunLoopSourceRef source_)
    : descriptor(descriptor_)
    , source(source_)
{
}

ArmedFD::~ArmedFD() {
    if (source != nullptr) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        CFRelease(source);
    }
    if (descriptor != nullptr) {
        CFFileDescriptorInvalidate(descriptor);
        CFRelease(descriptor);
    }
}

namespace {
    u32 entryFlags(const ArmedFD& entry) {
        u32 flags = 0;
        for (const stl::IntrusiveNode* node = entry.waiters.front(); node != entry.waiters.end(); node = node->next) {
            flags |= static_cast<const PollWaiter*>(node)->fd.flags;
        }
        return flags;
    }

    void enableEntryCallbacks(const ArmedFD& entry) {
        const u32 flags = entryFlags(entry);
        CFOptionFlags types = 0;
        if (flags & (PollFlag::In | PollFlag::Err | PollFlag::Hup)) {
            types |= kCFFileDescriptorReadCallBack;
        }
        if (flags & PollFlag::Out) {
            types |= kCFFileDescriptorWriteCallBack;
        }
        CFFileDescriptorEnableCallBacks(entry.descriptor, types);
    }
}

void PollerImpl::arm(PollWaiter& waiter) {
    waiter.unlink();
    ArmedFD* entry = armed.find(waiter.fd.fd);
    if (entry == nullptr) {
        CFFileDescriptorContext context{};
        context.info = this;
        CFFileDescriptorRef descriptor = CFFileDescriptorCreate(kCFAllocatorDefault, waiter.fd.fd, false, cocoaFileDescriptorReady, &context);
        STD_VERIFY(descriptor != nullptr);
        CFRunLoopSourceRef source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, descriptor, 0);
        STD_VERIFY(source != nullptr);
        armed.insert(waiter.fd.fd, descriptor, source);
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        entry = armed.find(waiter.fd.fd);
    }
    entry->waiters.pushBack(&waiter);
    enableEntryCallbacks(*entry);
}

void PollerImpl::cancel(PollWaiter& waiter) {
    // Works whichever list currently holds the node; an empty entry is
    // reclaimed on its next readiness callback.
    waiter.unlink();
}

void PollerImpl::timeout(u64 microseconds, TimerCallback& callback) {
    timers->timeout(microseconds, callback);
    scheduleTimer();
}

void PollerImpl::deadline(u64 monotonicMicroseconds, TimerCallback& callback) {
    timers->deadline(monotonicMicroseconds, callback);
    scheduleTimer();
}

void PollerImpl::cancel(TimerCallback& callback) {
    timers->cancel(callback);
    scheduleTimer();
}

void PollerImpl::defer(TimerCallback& callback) {
    // CFRunLoop services every ready source once per pass before firing
    // timers, so a zero timer already gives the descriptor waiters their
    // round here.
    timeout(0, callback);
}

u64 PollerImpl::nextDeadline() const {
    return timers->nextDeadline();
}

void PollerImpl::dispatchTimers() {
    timers->dispatchTimers();
    scheduleTimer();
}

void PollerImpl::scheduleTimer() {
    const u64 deadline = nextDeadline();
    if (deadline == UINT64_MAX) {
        CFRunLoopTimerSetNextFireDate(runLoopTimer, DBL_MAX);
        return;
    }
    const u64 now = monotonicNowUs();
    const CFTimeInterval delay = deadline > now ? (deadline - now) / 1'000'000.0 : 0.0;
    CFRunLoopTimerSetNextFireDate(runLoopTimer, CFAbsoluteTimeGetCurrent() + delay);
}

void PollerImpl::descriptorReady(CFFileDescriptorRef descriptor) {
    const int fd = CFFileDescriptorGetNativeDescriptor(descriptor);
    ArmedFD* const entry = armed.find(fd);
    if (entry == nullptr || entry->descriptor != descriptor) {
        return;
    }
    if (entry->waiters.empty()) {
        armed.erase(fd);
        return;
    }
    struct pollfd event{fd, (short)0, 0};
    event.events = PollFD{.fd = fd, .flags = entryFlags(*entry)}.toPollEvents();
    const int pollResult = ::poll(&event, 1, 0);
    if (pollResult <= 0 || event.revents == 0) {
        enableEntryCallbacks(*entry);
        return;
    }
    const u32 readyFlags = PollFD::fromPollEvents(event.revents);
    // Detach every matching waiter before the first callback runs; a
    // callback that cancels or re-arms another waiter pulls it out of this
    // round's list.
    stl::IntrusiveList ready;
    for (stl::IntrusiveNode* node = entry->waiters.mutFront(); node != entry->waiters.mutEnd();) {
        PollWaiter* const waiter = static_cast<PollWaiter*>(node);
        node = node->next;
        if ((waiter->fd.flags | PollFlag::Err | PollFlag::Hup) & readyFlags) {
            waiter->readyFlags = readyFlags;
            waiter->unlink();
            ready.pushBack(waiter);
        }
    }
    while (!ready.empty()) {
        PollWaiter* const waiter = static_cast<PollWaiter*>(ready.popFront());
        waiter->callback->ready({
            .fd = fd,
            .flags = waiter->readyFlags,
        });
    }
    ArmedFD* const remaining = armed.find(fd);
    if (remaining != nullptr && remaining->descriptor == descriptor) {
        if (remaining->waiters.empty()) {
            armed.erase(fd);
        } else {
            enableEntryCallbacks(*remaining);
        }
    }
    NSEvent* wakeup = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wakeup atStart:NO];
}

void PlatformImpl::run() {
    if (stopRequested_) {
        stopRequested_ = false;
        return;
    }
    if (applicationReady_) {
        [NSApp run];
    } else {
        // Descriptors and timers live directly on the main CFRunLoop. A
        // windowless platform therefore needs no NSApplication (and no
        // WindowServer), which keeps the poller usable by services and tests.
        CFRunLoopRun();
    }
    stopRequested_ = false;
}

void PlatformImpl::stop() {
    stopRequested_ = true;
    if (applicationReady_) {
        [NSApp stop:nil];
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
        [NSApp postEvent:event atStart:NO];
    } else {
        CFRunLoopStop(CFRunLoopGetMain());
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
}

WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
    : platform(platform_)
    , input(options.input)
    , events(options.events)
    , frame(options.frame)
    , dropTarget(options.drop)
    , quick(options.quick)
    , quickGeometry(options.quickGeometry)
{
    primaryPasteboard.window = this;
    primaryPasteboard.primary = true;
    generalPasteboard.window = this;
    if (options.icon.length() != 0) {
        // The Dock icon for the whole unbundled binary: without a bundle
        // there is no Info.plist to name an icns, so the image is applied
        // at run time.
        NSData* const bytes = [NSData dataWithBytes:options.icon.data() length:options.icon.length()];
        NSImage* const image = [[NSImage alloc] initWithData:bytes];
        if (image != nil) {
            NSApp.applicationIconImage = image;
        }
    }
    if (options.appName.length() != 0) {
        // The menu bar of an unbundled binary shows argv[0]: without an
        // Info.plist there is nothing else for AppKit to read. Launch
        // Services accepts a display name for the running process; the
        // interfaces are private, so they resolve dynamically and a macOS
        // that drops them simply keeps the old label. The Cmd-Tab
        // switcher is out of reach either way - its label comes from the
        // application bundle.
        typedef const void* (*CurrentAsn)(void);
        typedef OSStatus (*SetItem)(int, const void*, CFStringRef, CFStringRef, CFDictionaryRef*);
        const auto currentAsn = (CurrentAsn)(dlsym(RTLD_DEFAULT, "_LSGetCurrentApplicationASN"));
        const auto setItem = (SetItem)(dlsym(RTLD_DEFAULT, "_LSSetApplicationInformationItem"));
        if (currentAsn != nullptr && setItem != nullptr) {
            CFStringRef name = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)(options.appName.data()), (CFIndex)(options.appName.length()), kCFStringEncodingUTF8, false);
            if (name != nullptr) {
                // -2 addresses the current login session; the key string
                // is the value behind _kLSDisplayNameKey.
                setItem(-2, currentAsn(), CFSTR("LSDisplayName"), name, nullptr);
                CFRelease(name);
            }
        }
    }
    const NSRect frame = NSMakeRect(0, 0, max(1u, options.width), max(1u, options.height));
    window = [[PltWindow alloc] initWithContentRect:frame styleMask:(NSWindowStyleMask)cocoaWindowStyleMask(options.decorations) backing:NSBackingStoreBuffered defer:NO];
    delegate = [PltWindowDelegate new];
    delegate.owner = this;
    window.delegate = delegate;
    view = [[PltView alloc] initWithFrame:frame];
    view.owner = this;
    view.wantsLayer = YES;
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    window.contentView = view;
    if (options.backdrop != Backdrop::None && options.backgroundOpacity < 100) {
        // Under the *frame* view rather than inside the content view.
        //
        // A subview of the content view could not do this job at all: it
        // would draw above the content view's own layer, and that layer
        // is the CAMetalLayer the terminal is rendered into - the
        // backdrop would cover the terminal instead of backing it. The
        // frame view is the one surface that is genuinely behind
        // everything, and placing the backdrop at the bottom of it also
        // puts the blur behind the title bar, which is where it has to be
        // for a translucent title strip not to read as a step above a
        // blurred body.
        //
        // Nothing else in the window moves. window.contentView keeps its
        // identity, so ui_sidebar_tabs.mm still finds the same view to
        // add its list to and Composer::setChromeReserve() still reserves
        // the same edges; ui_csd_tabs.mm still reaches the title bar
        // container through the zoom button, which is this file's
        // precedent for touching the frame hierarchy at all. The
        // alternative - making a visual effect view the content view and
        // re-parenting PltView inside it - would move the view the
        // sidebar reads, and was not taken for that reason.
        NSView* const frameView = view.superview;
        if (frameView != nil) {
            NSView* backdrop = nil;
#if PLT_SDK_MACOS_26
            if (options.backdrop == Backdrop::Glass) {
                if (@available(macOS 26.0, *)) {
                    PltGlassView* const glass = [[PltGlassView alloc] initWithFrame:frameView.bounds];
                    // Regular, not Clear: Clear is the style meant for
                    // glass laid over imagery the user is looking at,
                    // and this pane has a terminal on top of it.
                    glass.style = NSGlassEffectViewStyleRegular;
                    // Left without a contentView deliberately. That
                    // property is the one placement NSGlassEffectView's
                    // header actually guarantees, and it is also the one
                    // that was measured to drive the *window's* size
                    // through Auto Layout - a 640x440 window came up
                    // 640x43. Nothing needs to live inside this glass:
                    // the terminal is a sibling above it, which is
                    // where the recon measured the glass to show
                    // through.
                    //
                    // Which also means the autoresizing mask below has
                    // to be honoured rather than ignored: a glass view
                    // ships translatesAutoresizingMaskIntoConstraints
                    // set NO, and a view in that state takes its frame
                    // from constraints nobody here writes.
                    glass.translatesAutoresizingMaskIntoConstraints = YES;
                    glassBackdrop = glass;
                    backdrop = glass;
                }
            }
#endif
            if (backdrop == nil) {
                // Either -backgroundBlur blur, or glass on a system that
                // has none. The fallback is silent: a warning here would
                // fire on every start for anyone who wrote glass into a
                // config once, on a machine that can never satisfy it.
                PltBackdropView* const frosted = [[PltBackdropView alloc] initWithFrame:frameView.bounds];
                // "What is under the window" is literally the question
                // this option asks, and it is the one material named for
                // a position rather than for a role: Sidebar,
                // HeaderView, Menu and the rest each carry the tint of
                // the AppKit element they belong to, which would put a
                // system control's colour cast between the terminal and
                // the desktop.
                frosted.material = NSVisualEffectMaterialUnderWindowBackground;
                // Behind the window, not within it: the pixels to blur
                // are the desktop's, not this window's own.
                frosted.blendingMode = NSVisualEffectBlendingModeBehindWindow;
                // Held active instead of following the window's key
                // state. The default stops blurring the moment the
                // window is not key, which for a terminal means the blur
                // disappears every time the user looks at another
                // application - the window would flicker between blurred
                // and clear as focus moves. NSGlassEffectView exposes no
                // counterpart to this: its whole API is contentView,
                // cornerRadius, tintColor and style, so whatever it does
                // when the window stops being key, it does unasked.
                frosted.state = NSVisualEffectStateActive;
                backdrop = frosted;
            }
            backdrop.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            // The superview owns it from here. This file is compiled
            // under ARC (unlike lib/shitty's Objective-C++, which is
            // not), so the local reference needs no release of its own.
            [frameView addSubview:backdrop positioned:NSWindowBelow relativeTo:view];
        }
    }
    if (options.backgroundOpacity < 100) {
        // T10. Both halves of the decision, in one call, from one
        // reading of the option - the same discipline requestCornerRadius()
        // keeps for its own pair.
        //
        // The layer's half is not optional and not a duplicate of the
        // window's: a CAMetalLayer marked opaque has its drawable's alpha
        // channel discarded by CoreAnimation, so a renderer writing alpha
        // into it produces a *darker* background rather than a
        // see-through one - the colour has been multiplied down and
        // nothing composites it back. render_metal.mm reads this flag
        // back off the live layer for exactly that reason, instead of
        // trusting an option that a reload can have moved since.
        view.layer.opaque = NO;
        establishFrameTransparency();
    }
    if (options.transparentTitlebar && options.decorations) {
        // A borderless window (no-decorations) has no title bar to make
        // transparent; setting the property there would be a no-op, but
        // the guard keeps the option's effect scoped to where it means
        // something. The actual fill color - matching the terminal
        // background - is app-level (Options::bg) and applied by
        // ui_csd_tabs.mm once the window exists, not here: this plt layer
        // has no Color type to draw with.
        window.titlebarAppearsTransparent = YES;
    }
    if (options.quick) {
        // Scoped to quick windows only, matching quickCornerRadius's own
        // name and doc - requestCornerRadius() itself is a general
        // primitive (also used live by the fullscreen chord,
        // ui_quick_hotkey.mm), the scoping happens here at the one
        // construction-time call site.
        requestCornerRadius(options.quickCornerRadius);
    }
    if (options.quick) {
        // A quick-terminal window has no business minimizing to the
        // Dock: it starts hidden and is meant to be summoned and
        // dismissed by hotkey only. Dropping the style bit makes both
        // the (absent) minimize button and Cmd-M's performMiniaturize:
        // no-ops (verified live: miniaturized/visible stay unchanged
        // through both), which also sidesteps a real ordering hazard -
        // window.miniaturized only flips true on windowDidMiniaturize:,
        // one notification *after* windowDidResignKey: - that would
        // otherwise let the hide-on-blur below race a Cmd-M genie
        // animation on a window that was never supposed to allow one.
        window.styleMask &= ~NSWindowStyleMaskMiniaturizable;
        // collectionBehavior is permanent, unlike window.level below
        // (see requestShowAt(TopOfActiveScreen)/requestHide()), which IS
        // toggled per show/hide. Toggling both together raced AppKit's
        // own frame relayout - a collectionBehavior change triggers one -
        // against the setFrame: call right after it in
        // requestShowAt(TopOfActiveScreen): about a third of shows
        // landed at a plain window's default frame instead of
        // topOfActiveScreenFrame() (measured live: 11/36 anomalies with
        // it toggled, 0/24 with it held constant here - window-chrome
        // R3-qa-final, F4).
        //
        // FullScreenAuxiliary's job is membership: it is what lets
        // AppKit place this window on a *different* app's fullscreen
        // Space at all, which a plain window can't join. It is NOT
        // about winning stacking order against that app's content - a
        // fullscreen app's own window sits at the ordinary level 0
        // itself (measured live), so a quick window that reaches that
        // Space already outranks nothing there by level; the elevated
        // level granted in requestShowAt(TopOfActiveScreen) exists to
        // clear the fullscreen app's *own* status-level chrome and the
        // system menu bar, not its content (an earlier version of this
        // comment claimed the opposite - wrong, see window-chrome
        // R3-sec-round2, finding S3-r).
        //
        // Being constant means this membership is technically reachable
        // from the data stream too, even while the window is hidden -
        // R3-sec-round2 flagged this as S3-r/S6. The guard isn't here
        // though: requestFocus(), requestRestore() and
        // requestFullscreen() below each refuse to touch a hidden quick
        // window (F5/F6) - together the only VtermImpl::windowOperation
        // calls (CSI 5t/1t/10t) that could otherwise put a hidden one on
        // screen. requestMove() (CSI 3t) still reaches a hidden window,
        // but only repositions it; requestShowAt(TopOfActiveScreen)
        // overwrites that position on the next real show regardless.
        window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    }
    window.acceptsMouseMovedEvents = YES;
    [view registerForDraggedTypes:@[ NSPasteboardTypeString, NSPasteboardTypeFileURL ]];
    requestTitle(options.title);
    requestMinimumSize(options.minimumWidth, options.minimumHeight);
    // CVDisplayLink drives frame pacing. NSView.displayLink (CADisplayLink)
    // was tried here but broke atomic resize: with a view-owned display link
    // AppKit stops servicing the layer's synchronous display pass inside the
    // resize commit, so the new-size surface lands a tick after the bounds
    // change and the old surface flashes at the new size. screenChanged()
    // retargets this link across displays.
    if (CVDisplayLinkCreateWithActiveCGDisplays(&displayLink) == kCVReturnSuccess && displayLink != nullptr) {
        displayLinkTarget = [PltDisplayLinkTarget new];
        displayLinkTarget->gate.attach(this);
        displayLinkContext = (__bridge_retained void*)(displayLinkTarget);
        if (CVDisplayLinkSetOutputCallback(displayLink, displayLinkCallback, displayLinkContext) != kCVReturnSuccess) {
            displayLinkTarget->gate.detach();
            CFBridgingRelease(displayLinkContext);
            displayLinkContext = nullptr;
            displayLinkTarget = nil;
            CVDisplayLinkRelease(displayLink);
            displayLink = nullptr;
        }
    }
}

WindowImpl::~WindowImpl() {
    if (displayLinkTarget != nil) {
        displayLinkTarget->gate.detach();
    }
    stopDisplayLink();
    if (displayLink != nullptr) {
        CVDisplayLinkRelease(displayLink);
    }
    if (displayLinkContext != nullptr) {
        CFBridgingRelease(displayLinkContext);
    }
    window.delegate = nil;
    view.owner = nullptr;
    delegate.owner = nullptr;
    [window orderOut:nil];
}

void WindowImpl::requestShow() {
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    // Ordering the window front is what gets its frame view a layer to
    // round; see applyCornerRadius(). Re-applied on every show rather
    // than only the first, because that view is AppKit's and nothing
    // here owns whether it keeps a layer, or the same one, across a hide.
    applyCornerRadius();
    resized();
}

void WindowImpl::requestHide() {
    if (quick) {
        // Undoes the level grant made in requestShowAt(TopOfActiveScreen)
        // on every hide, quick-window or not shown yet - see the comment
        // there. NSNormalWindowLevel is AppKit's own default for an
        // untouched window, not a value invented here. collectionBehavior
        // is not touched: it is held constant from creation, not toggled
        // per show/hide - see the constructor for why.
        window.level = NSNormalWindowLevel;
    }
    [window orderOut:nil];
}

// The screen under the mouse pointer, not the window's own .screen - the
// quick-terminal window starts hidden (off any screen's active area) and
// TopOfActiveScreen means "wherever the user is right now", which the
// window's last-known screen cannot answer on a fresh show.
static NSScreen* screenUnderPointer() {
    const NSPoint pointer = [NSEvent mouseLocation];
    for (NSScreen* screen in [NSScreen screens]) {
        if (NSMouseInRect(pointer, screen.frame, NO)) {
            return screen;
        }
    }
    return [NSScreen mainScreen];
}

namespace {

    // dim.value / 100.0 first, extent second: the same two floating-point
    // operations quickGeometry's hard default (100%/40%) used to be
    // spelled as literals (extent itself, extent * 0.4), computed in the
    // same order. A single combined expression like extent * value / 100
    // would round twice and was measured to occasionally disagree with
    // that literal form in the last bit - resolveQuickGeometryDim keeps
    // the default's frame bit-for-bit identical to the pre-quickGeometry
    // behavior, which the option is required to reproduce exactly.
    static CGFloat resolveQuickGeometryDim(const QuickGeometryDim& dim, CGFloat extent) {
        if (!dim.percent) {
            return (CGFloat)(dim.value);
        }
        return extent * ((CGFloat)(dim.value) / (CGFloat)(100.0));
    }

    static CGFloat clampRange(CGFloat value, CGFloat lo, CGFloat hi) {
        return max(lo, min(value, hi));
    }
}

// A rect on the pointer's screen, sized and offset by quickGeometry
// against visibleFrame (which already excludes the menu bar and Dock, so
// this never sits under either). The default - 100% width, 40% height,
// zero offset - reproduces the placement this replaced, bit-for-bit; see
// resolveQuickGeometryDim.
//
// width/height/x/y are each clamped into the screen after resolving:
// quick_geometry.cpp validates every component in isolation (a percent
// within 0..100, a pixel count within 0..UINT16_MAX), but has no
// NSScreen to check the four against each other or against a specific
// display - a 1200px width plus a 90% x offset is each individually
// valid and can still overshoot a screen narrower than that. Clamping
// here, against the one screen this call actually targets, is simpler
// and more honest than rejecting such a config at startup on every
// machine regardless of its actual displays: min/max keep the window
// fully on screen without ever discarding a value the user didn't ask to
// have clamped (the defaults hit every clamp's already-satisfied branch,
// which is what keeps them exact - see resolveQuickGeometryDim).
NSRect WindowImpl::topOfActiveScreenFrame() const {
    const NSRect visible = screenUnderPointer().visibleFrame;
    const CGFloat rawWidth = resolveQuickGeometryDim(quickGeometry.width, visible.size.width);
    const CGFloat rawHeight = resolveQuickGeometryDim(quickGeometry.height, visible.size.height);
    const CGFloat width = clampRange(rawWidth, (CGFloat)(1), visible.size.width);
    const CGFloat height = clampRange(rawHeight, (CGFloat)(1), visible.size.height);
    const CGFloat rawX = resolveQuickGeometryDim(quickGeometry.x, visible.size.width);
    const CGFloat rawY = resolveQuickGeometryDim(quickGeometry.y, visible.size.height);
    const CGFloat x = clampRange(rawX, (CGFloat)(0), visible.size.width - width);
    const CGFloat y = clampRange(rawY, (CGFloat)(0), visible.size.height - height);
    // y is measured from visibleFrame's top-left corner (the option's
    // documented origin), but AppKit's y axis grows upward from the
    // screen's bottom-left, hence the subtraction - matching the
    // unconfigurable placement this replaces, which was always anchored
    // to the top edge.
    return NSMakeRect(visible.origin.x + x, visible.origin.y + visible.size.height - height - y, width, height);
}

void WindowImpl::requestShowAt(ShowPlacement placement) {
    if (placement == ShowPlacement::TopOfActiveScreen) {
        if (quick) {
            // Only the level is granted per show (and undone in
            // requestHide()) - collectionBehavior is constant from
            // creation instead; see the constructor for why toggling
            // both together is a real bug, not a style choice
            // (window-chrome R3-qa-final, F4).
            //
            // Granted here rather than held for the window's whole
            // lifetime: this is the one call site a real user action
            // (the global hotkey, application.cpp's toggleQuickWindow)
            // reaches. requestFocus() - the other call that can raise
            // this window - has exactly one caller in the whole tree,
            // VtermImpl::windowOperation, which only runs from the
            // terminal's own data stream (CSI 5t/CSI 3t) and only when
            // allowWindowOps is on. requestMove() picked up a second
            // caller with quickRememberFrame - application.cpp's
            // applySavedQuickFrame(), reached only from toggleQuickWindow()
            // itself (a human hotkey press) as a fallback for a backend
            // without a concrete NSWindow to reach through
            // Window::renderContext() (headless; on Cocoa,
            // applyQuickFrameToWindow() in ui_quick_hotkey.mm always
            // succeeds instead, so this call site is unreached here) -
            // not the data stream either way. requestFocus(), requestRestore() and
            // requestFullscreen() below each refuse to touch a hidden
            // quick window (F5/F6 - the three calls that could
            // otherwise put a hidden one on screen: CSI 5t/1t/10t), so
            // that path can only re-raise a window a human already
            // summoned - never conjure one out of hiding at a level
            // this call didn't already grant it (window-chrome R3-sec
            // finding S3, tightened by S3-r/S6/S7 in
            // R3-sec-round2/round3, closed there for those three ops).
            //
            // NSStatusWindowLevel (used by menu-bar-extra style panels)
            // sits above NSFloatingWindowLevel and above the main menu
            // itself; that headroom is what makes the window reliably
            // clear a fullscreen app's *own* status-level chrome and the
            // system menu bar. It is not competing with that app's
            // regular content for stacking order - a fullscreen window
            // sits at ordinary level 0 itself (measured live) - so this
            // grant only matters once collectionBehavior (constant, see
            // the constructor) has already let the window onto that
            // app's Space.
            window.level = NSStatusWindowLevel;
        }
        [window setFrame:topOfActiveScreenFrame() display:NO];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        // See requestShow(): before the window is ordered front there is
        // no frame-view layer for the radius to land on. This is the
        // path a quick window actually takes.
        applyCornerRadius();
        resized();
        return;
    }
    requestShow();
}

void WindowImpl::requestClose() {
    close();
}

void WindowImpl::requestFrame() {
    if (frameRequested) {
        return;
    }
    frameRequested = true;
    startDisplayLink();
}

void WindowImpl::startDisplayLink() {
    idleFrames = 0;
    if (displayLink != nullptr && !CVDisplayLinkIsRunning(displayLink)) {
        CVDisplayLinkStart(displayLink);
    }
}

void WindowImpl::draw() {
    if (!frameRequested || frame == nullptr) {
        // Idle frames coast for a while before the link stops. Starting
        // one costs a thread wake and a sync to the display, and a
        // terminal redraws in bursts paced by the user - a full-screen
        // TUI repaints once per keystroke - so stopping between them
        // made every repaint pay that price on its way to the glass.
        // Frames arriving faster than the refresh never noticed, which
        // is why dragging a scrollbar felt nothing like scrolling an
        // application.
        if (++idleFrames >= idleFramesBeforeStop) {
            stopDisplayLink();
        }
        return;
    }
    idleFrames = 0;
    frameRequested = false;
    frame->frame(info());
}

void WindowImpl::stopDisplayLink() {
    if (displayLink != nullptr && CVDisplayLinkIsRunning(displayLink)) {
        CVDisplayLinkStop(displayLink);
    }
}

void WindowImpl::requestTitle(StringView value) {
    NSString* title = stringFromView(value);
    window.title = title == nil ? @"" : title;
}

void WindowImpl::requestAttention() {
    [NSApp requestUserAttention:NSInformationalRequest];
}

void WindowImpl::requestRestore() {
    if (quick && !window.visible) {
        // Same invariant as the guard in requestFocus() below: CSI 1t is
        // VtermImpl::windowOperation's other call that could put a
        // hidden quick window on screen (deminiaturize: on an
        // orderOut:'d-but-not-miniaturized window), so it needs the same
        // refusal rather than relying on requestFocus() alone
        // (window-chrome R3-sec-round3, finding S7).
        return;
    }
    [window deminiaturize:nil];
    if ((window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
        [window toggleFullScreen:nil];
    }
    if ([window isZoomed]) {
        [window zoom:nil];
    }
}

void WindowImpl::requestIconify() {
    [window miniaturize:nil];
}

void WindowImpl::requestMove(i32 x, i32 y) {
    NSPoint point = NSMakePoint(x, y);
    [window setFrameOrigin:point];
}

void WindowImpl::requestFocus() {
    if (quick && !window.visible) {
        // The only caller of requestFocus() is VtermImpl::windowOperation
        // (CSI 5t), reachable from the terminal's own data stream under
        // allowWindowOps. Without this guard it un-hid a fully hidden
        // quick window (measured live) at plain NSNormalWindowLevel, but
        // still carrying the permanent FullScreenAuxiliary|CanJoinAllSpaces
        // membership from the constructor - technically reachable on a
        // fullscreen Space a human never asked to show anything on
        // (window-chrome R3-sec-round2, findings S3-r and S6). The data
        // stream may still re-raise a window a human already summoned
        // through the hotkey; it can no longer conjure one out of hiding.
        return;
    }
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void WindowImpl::requestMaximized(bool value) {
    if ([window isZoomed] != value) {
        [window zoom:nil];
    }
}

void WindowImpl::requestFullscreen(bool value) {
    if (quick && !window.visible) {
        // Same invariant as requestRestore() above / requestFocus()
        // below: CSI 10;1t is the third VtermImpl::windowOperation call
        // that could put a hidden quick window on screen - toggleFullScreen:
        // on a hidden window has current == false, so without this guard
        // it would run unconditionally (window-chrome R3-sec-round3,
        // finding S7).
        return;
    }
    const bool current = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (current != value) {
        [window toggleFullScreen:nil];
    }
}

// Rounds the window's frame view, not the content view's own layer.
//
// The content view's layer is the CAMetalLayer the renderer presents
// drawables to (makeBackingLayer above), and rounding *that* is the shape
// this option shipped in. It cannot round this window, and the reason is
// geometric rather than anything to do with Metal: measured live on a
// quick window with the option on, the content view is 1728x402 inside a
// 1728x434 frame, sitting 32pt below the top, because the window is
// titled (styleMask 0xb). Its two top corners are therefore interior
// points hidden under the title bar, not corners of the window at all -
// so rounding that layer can only ever produce two round corners at the
// bottom plus, if the clip lands, two notches carved into the content
// under the title bar. Never the window.
//
// Whether a CAMetalLayer honors its own rounded-rect clip at all is left
// deliberately unanswered here: it was measured that the radius reaches
// that layer and stays (cornerRadius=12.00, masksToBounds=1, unchanged
// two seconds after the show), but reading the composited pixels back is
// not possible from this process, and the change does not depend on the
// answer either way.
//
// One view up, the frame view owns the entire window rect, backs an
// ordinary NSViewBackingLayer rather than a Metal one, and holds both the
// content view and the title bar container in its subtree (measured:
// NSTitlebarView two levels under it). Clipping there is the plain
// ancestor masking CoreAnimation applies to a sublayer tree, it reaches
// all four corners of the actual window, and it leaves the title bar in
// place - which ui_csd_tabs.mm's transparentTitlebar tint needs, since
// that tint lives in a fill view inside NSTitlebarView.
//
// The radius is in points and so is the layer's own geometry - no
// contentScale multiplication here, unlike the pixel-denominated Options
// fields composer.h warns about. Toggling it costs a layer property and
// a shadow invalidation, which is what lets the geometric fullscreen
// chord (ui_quick_hotkey.mm) square the window off and restore it per
// keypress; it never runs mid-resize, so it stays clear of the
// displayLayer:/presentsWithTransaction handshake in render_metal.mm.
void WindowImpl::applyCornerRadius() {
    // The layer is read live, never cached: the frame view is AppKit's,
    // and it is not layer-backed yet when the constructor reads the
    // option - measured -layer nil at that point, which is exactly why
    // writing the radius there went nowhere and this had to become a
    // remember-then-apply pair. Both callers below run after the window
    // has been ordered front, or are a later re-request against a window
    // that already is.
#if PLT_SDK_MACOS_26
    // Ahead of the early return below, and unconditionally: this one is
    // our own view, not AppKit's frame view, so there is no "leave it as
    // we found it" to honour - and the first caller runs from the
    // constructor, where the frame view has no layer yet and the return
    // below would otherwise skip the radius the option just asked for.
    if (glassBackdrop != nil) {
        if (@available(macOS 26.0, *)) {
            ((NSGlassEffectView*)(glassBackdrop)).cornerRadius = (CGFloat)(cornerRadius);
        }
#if PLT_SDK_MACOS_27
        // Still written above, and deliberately: cornerRadius is the whole
        // statement on macOS 26, and on 27 it is the fallback should the
        // configuration below ever return nil. Here the same number becomes
        // the floor of a concentric configuration, and the invalidation is
        // what makes AppKit read the getter again - it caches the answer.
        if (@available(macOS 27.0, *)) {
            PltGlassView* const glass = (PltGlassView*)(glassBackdrop);
            glass->concentricFloor = (CGFloat)(cornerRadius);
            [glass invalidateCornerConfiguration];
        }
#endif
    }
#endif
    CALayer* const layer = view.superview.layer;
    if (layer == nil || (cornerRadius == 0 && layer.cornerRadius == 0)) {
        // Nothing asked for and nothing to undo: an ordinary window that
        // never rounds anything leaves AppKit's own frame view exactly
        // as it found it, invalidateShadow() included.
        return;
    }
    layer.cornerRadius = (CGFloat)(cornerRadius);
    // Raised, never lowered: AppKit already ships this YES on its own
    // frame view (measured), the view is not ours to reconfigure, and a
    // radius of 0 is spelled by the radius alone.
    if (cornerRadius > 0) {
        layer.masksToBounds = YES;
    }
    // The window shadow is traced from the window's shape and cached;
    // without this the square shadow of the un-rounded frame stays as a
    // rectangular halo behind the new corners.
    [window invalidateShadow];
}

void WindowImpl::requestCornerRadius(u16 radius) {
    cornerRadius = radius;
    applyCornerRadius();
    // window.opaque/backgroundColor are NOT this call's to keep
    // re-touching on every toggle: an opaque window would show its own
    // background color as square corners poking out past the round
    // content, so the *window as a whole* needs to go transparent
    // behind it - but window.backgroundColor is also ui_csd_tabs.mm's
    // (transparentTitlebar tints it to match the terminal background),
    // and clobbering it back to windowBackgroundColor on every
    // fullscreen fold-back reset that tint until the next config reload
    // (F2's report, I7). Transparency capability is established exactly
    // once - the first time this window is asked to round its corners
    // at all, window.opaque still at the AppKit default - and left
    // alone after that regardless of later radius changes; whatever
    // owns backgroundColor by then (this function, or ui_csd_tabs.mm
    // having run since) keeps owning it. window.opaque is what
    // ui_csd_tabs.mm reads to decide whether the window background is
    // still its to tint - it is the live record of this decision, and
    // it is set here in the same call that rounds the layer, so both
    // sides of that arbitration always see one moment in time. Its own
    // tint for the transparentTitlebar option went to a fill view
    // inside the title bar for that reason: the strip can be painted
    // while the frame behind the rounded corners stays clear.
    if (radius > 0) {
        establishFrameTransparency();
    }
}

// Hands window.backgroundColor over to transparency, once and for
// whoever asks first. Two options need a transparent window frame and
// they need exactly the same thing from it: rounded corners, so the
// window's own background does not poke square ears past the rounded
// content, and -backgroundOpacity, so what shows through the content is
// the desktop and not a solid rectangle underneath it.
//
// Once, and never re-touched, for the reason the comment in
// requestCornerRadius() gives at length: window.backgroundColor is also
// ui_csd_tabs.mm's while the window is opaque, and window.opaque is the
// live record of which of them owns it. Re-establishing on every toggle
// reset that module's tint until the next config reload (F2's report,
// I7). Whoever owns the colour by the time this runs keeps owning it.
void WindowImpl::establishFrameTransparency() {
    if (!window.opaque) {
        return;
    }
    window.opaque = NO;
    window.backgroundColor = [NSColor clearColor];
}

void WindowImpl::requestResize(u32 width, u32 height) {
    const CGFloat scale = window.backingScaleFactor;
    const NSSize size = NSMakeSize(max(1u, width) / scale, max(1u, height) / scale);
    NSWindow* const target = window;
    // Asynchronous, like every request*. -setContentSize: posts windowDidResize
    // synchronously, so applying it inline would re-enter the frame callback: a
    // font or content-scale change resizes the window from inside frame(), and a
    // synchronous resize path would then recurse. Defer it, so the window system
    // delivers a fresh frame() with the new size instead of recursing.
    dispatch_async(dispatch_get_main_queue(), ^{
        // A zoom or a fullscreen transition may have taken the window over
        // while this waited: entering either is synchronous, changing the
        // frame of a zoomed window un-zooms it, and a resize computed before
        // the transition would tear the new state right back down (issue
        // 118). Such a window is not ours to size; drop the stale request
        // and let the next frame reflow the grid over the pixels it has.
        if ((target.styleMask & NSWindowStyleMaskFullScreen) != 0 || [target isZoomed]) {
            return;
        }
        [target setContentSize:size];
    });
}

void WindowImpl::requestMinimumSize(u32 width, u32 height) {
    minimumWidth = max(1u, width);
    minimumHeight = max(1u, height);
    applySizeConstraints();
}

void WindowImpl::requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeUnitWidth = max(1u, width);
    resizeUnitHeight = max(1u, height);
    resizeBaseWidth = baseWidth;
    resizeBaseHeight = baseHeight;
}

void WindowImpl::applySizeConstraints() {
    const CGFloat scale = window.backingScaleFactor;
    window.contentMinSize = NSMakeSize(minimumWidth / scale, minimumHeight / scale);
}

bool WindowImpl::inLiveResize() const {
    return view != nil && view.inLiveResize;
}

WindowInfo WindowImpl::info() const {
    const NSRect content = [view convertRectToBacking:view.bounds];
    NSScreen* screen = window.screen != nil ? window.screen : [NSScreen mainScreen];
    const NSRect screenFrame = [screen convertRectToBacking:screen.frame];
    return {
        .x = (i32)(window.frame.origin.x),
        .y = (i32)(window.frame.origin.y),
        .width = (u32)(max(1.0, content.size.width)),
        .height = (u32)(max(1.0, content.size.height)),
        .screenPixelWidth = (u32)(max(0.0, screenFrame.size.width)),
        .screenPixelHeight = (u32)(max(0.0, screenFrame.size.height)),
        .contentScale = (float)(window.backingScaleFactor),
        .focused = (bool)(window.keyWindow),
        .iconified = (bool)(window.miniaturized),
        .maximized = (bool)([window isZoomed]),
        .fullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0,
    };
}

bool WindowImpl::visible() const {
    return window.visible;
}

size_t CocoaDropOffer::formats() const {
    return (size_t)(text) + (size_t)(files);
}

StringView CocoaDropOffer::format(size_t index) const {
    if (files && index == 0) {
        return uriListMime;
    }
    return utf8Mime;
}

DropOffer* CocoaDrop::what() {
    return view;
}

Input* CocoaDrop::read(StringView mime) {
    Buffer content;
    bool* flag = nullptr;
    const bool first = !taken;
    taken = true;
    if (first && view->files && mime == uriListMime) {
        NSArray<NSURL*>* const urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
        for (NSURL* url in urls) {
            NSData* const encoded = [url.absoluteString dataUsingEncoding:NSUTF8StringEncoding];
            if (encoded != nil && encoded.length != 0) {
                content.append(encoded.bytes, encoded.length);
                content.append("\r\n", 2);
            }
        }
        flag = content.empty() ? nullptr : &drained;
    } else if (first && view->text && mime == utf8Mime) {
        NSString* const value = [pasteboard stringForType:NSPasteboardTypeString];
        NSData* const data = value == nil ? nil : [value dataUsingEncoding:NSUTF8StringEncoding];
        if (data != nil) {
            content.append(data.bytes, data.length);
            flag = &drained;
        }
    }
    return window->platform.allocator_->make<CocoaStreamInput>(window->platform.allocator_, static_cast<Buffer&&>(content), flag);
}

NSDragOperation WindowImpl::dragOver(id<NSDraggingInfo> sender) {
    if (dropTarget == nullptr) {
        return NSDragOperationNone;
    }
    NSPasteboard* const pasteboard = [sender draggingPasteboard];
    CocoaDropOffer offer;
    offer.text = [pasteboard availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil;
    offer.files = [pasteboard canReadObjectForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    NSPoint point = [view convertPoint:[sender draggingLocation] fromView:nil];
    point.y = view.bounds.size.height - point.y;
    point = [view convertPointToBacking:point];
    const DropReply reply = dropTarget->dragOver(offer, (i32)(point.x), (i32)(point.y));
    bool known = false;
    for (size_t index = 0; index != offer.formats(); ++index) {
        if (offer.format(index) == reply.mime) {
            known = true;
        }
    }
    if (reply.mime.empty() || !known || reply.action == DropAction::None) {
        return NSDragOperationNone;
    }
    return reply.action == DropAction::Move ? NSDragOperationMove : NSDragOperationCopy;
}

void WindowImpl::dragExited() {
    if (dropTarget != nullptr) {
        dropTarget->dragLeft();
    }
}

BOOL WindowImpl::performDrop(id<NSDraggingInfo> sender) {
    if (dropTarget == nullptr) {
        return NO;
    }
    NSPasteboard* const pasteboard = [sender draggingPasteboard];
    CocoaDropOffer offer;
    offer.text = [pasteboard availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil;
    offer.files = [pasteboard canReadObjectForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    CocoaDrop drop;
    drop.window = this;
    drop.view = &offer;
    drop.pasteboard = pasteboard;
    dropTarget->dropped(drop);
    return drop.drained ? YES : NO;
}

void WindowImpl::writePasteboard(NSPasteboard* pasteboard, StringView content) {
    [pasteboard clearContents];
    NSString* value = stringFromView(content);
    [pasteboard setString:value == nil ? @"" : value forType:NSPasteboardTypeString];
}

Clipboard* WindowImpl::primary() {
    return &primaryPasteboard;
}

Clipboard* WindowImpl::secondary() {
    return &generalPasteboard;
}

CocoaStreamInput::CocoaStreamInput(SmallObjAllocator* allocator_, Buffer&& content_, bool* drained_)
    : allocator(allocator_)
    , content(static_cast<Buffer&&>(content_))
    , drained(drained_)
{
}

CocoaStreamInput::~CocoaStreamInput() noexcept {
    if (drained != nullptr) {
        *drained = offset == content.length();
    }
}

void CocoaStreamInput::operator delete(CocoaStreamInput* input, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = input->allocator;
    owner->release(input);
}

size_t CocoaStreamInput::readImpl(void* data, size_t len) {
    const size_t count = min(len, content.length() - offset);
    memcpy(data, (const u8*)(content.data()) + offset, count);
    offset += count;
    return count;
}

CocoaStreamOutput::CocoaStreamOutput(WindowImpl* window_, bool primary_)
    : window(window_)
    , primary(primary_)
{
}

void CocoaStreamOutput::operator delete(CocoaStreamOutput* output, std::destroying_delete_t) noexcept {
    SmallObjAllocator* const owner = output->window->platform.allocator_;
    owner->release(output);
}

size_t CocoaStreamOutput::writeImpl(const void* data, size_t size) {
    accumulated.append(data, size);
    return size;
}

void CocoaStreamOutput::finishImpl() {
    if (finished) {
        return;
    }
    finished = true;
    NSPasteboard* const pasteboard = primary ? [NSPasteboard pasteboardWithName:NSPasteboardNameFind] : [NSPasteboard generalPasteboard];
    window->writePasteboard(pasteboard, StringView(accumulated));
}

Input* ClipboardImpl::read() {
    // The pasteboard is synchronous: the payload is already materialized by
    // the system, so no fiber blocking is involved.
    NSPasteboard* const pasteboard = primary ? [NSPasteboard pasteboardWithName:NSPasteboardNameFind] : [NSPasteboard generalPasteboard];
    NSString* const value = [pasteboard stringForType:NSPasteboardTypeString];
    NSData* const data = value == nil ? nil : [value dataUsingEncoding:NSUTF8StringEncoding];
    Buffer content;
    if (data != nil) {
        content.append(data.bytes, data.length);
    }
    return window->platform.allocator_->make<CocoaStreamInput>(window->platform.allocator_, static_cast<Buffer&&>(content), nullptr);
}

Output* ClipboardImpl::write() {
    return window->platform.allocator_->make<CocoaStreamOutput>(window, primary);
}

void WindowImpl::requestPointerIcon(PointerIcon icon) {
    [pointerCursor(icon) set];
}

void WindowImpl::requestOpenUri(StringView uri) {
    NSString* const text = [[NSString alloc] initWithBytes:uri.data() length:uri.length() encoding:NSUTF8StringEncoding];
    if (text == nil) {
        return;
    }
    NSURL* const url = [NSURL URLWithString:text];
    if (url != nil) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

RenderContext WindowImpl::renderContext() const {
    return {
        .backend = RenderBackend::Cocoa,
        .connection = (__bridge void*)(view.layer),
        // The native window, for a client that builds its own chrome on
        // AppKit; the platform stays out of whatever it does there.
        .window = (__bridge void*)(window),
    };
}

void WindowImpl::close() {
    if (events != nullptr) {
        events->close();
    }
}

void WindowImpl::resized() {
    applySizeConstraints();
    ((CAMetalLayer*)(view.layer)).contentsScale = window.backingScaleFactor;
    // Mark the layer for display; CoreAnimation then calls displayLayer:, which
    // renders the frame (synchronously and in this transaction during a live
    // resize). needsDisplayOnBoundsChange already does this for a bounds change,
    // but a backing-property change (scale) reaches resized() too.
    [view.layer setNeedsDisplay];
}

void WindowImpl::resizeFrame() {
    // A frame the window system asked for during layout. Render synchronously in
    // the current (resize) transaction so bounds and contents commit together.
    // Stop the display link for this frame: a link tick would present in its own
    // transaction, one step out of sync with the bounds. frame() rebuilds the
    // vterm to the new size and renders; it never re-enters (request* are async).
    stopDisplayLink();
    frameRequested = false;
    if (frame != nullptr) {
        frame->frame(info());
    }
    startDisplayLink();
}

void WindowImpl::screenChanged() {
    // CVDisplayLink must be retargeted to the window's new display, or it
    // keeps pacing frames at the previous display's refresh rate.
    if (displayLink != nullptr) {
        NSScreen* const screen = window.screen;
        NSNumber* const number = screen == nil ? nil : screen.deviceDescription[@"NSScreenNumber"];
        if (number != nil) {
            CVDisplayLinkSetCurrentCGDisplay(displayLink, (CGDirectDisplayID)(number.unsignedIntValue));
        }
    }
    requestFrame();
}

void WindowImpl::requestTextInputRect(i32 x, i32 y, u32 width, u32 height) {
    textInputX = x;
    textInputY = y;
    textInputWidth = width;
    textInputHeight = height;
}

NSRect WindowImpl::textInputScreenRect() const {
    const CGFloat scale = window.backingScaleFactor;
    NSRect rect = NSMakeRect(textInputX / scale, view.bounds.size.height - (textInputY + (CGFloat)(max(1u, textInputHeight))) / scale, max(1u, textInputWidth) / scale, max(1u, textInputHeight) / scale);
    rect = [view convertRect:rect toView:nil];
    return [window convertRectToScreen:rect];
}

NSSize WindowImpl::willResize(NSSize frameSize) const {
    const bool fullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    const bool viewAvailable = view != nil;
    if (cocoaResizeUsesExactProposal(fullscreen, viewAvailable, viewAvailable && view.inLiveResize)) {
        // Fullscreen and non-interactive proposals from window managers must
        // land exactly. Cell snapping is only for live user drags.
        return frameSize;
    }
    const NSRect content = [window contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    const CGFloat scale = window.backingScaleFactor;
    u32 width = (u32)(max(1.0, content.size.width * scale) + 0.5);
    u32 height = (u32)(max(1.0, content.size.height * scale) + 0.5);
    if (resizeUnitWidth > 1 && width > resizeBaseWidth) {
        width = resizeBaseWidth + ((width - resizeBaseWidth) / resizeUnitWidth) * resizeUnitWidth;
    }
    if (resizeUnitHeight > 1 && height > resizeBaseHeight) {
        height = resizeBaseHeight + ((height - resizeBaseHeight) / resizeUnitHeight) * resizeUnitHeight;
    }
    const NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, width / scale, height / scale)];
    return frame.size;
}

void WindowImpl::focused(bool value) {
    requestFrame();
    if (input != nullptr) {
        input->focus(value);
        input->flush();
    }
    // Quick-terminal windows hide themselves on losing key status, after
    // the terminal above has already seen the same blur it would get on
    // an ordinary window. window.miniaturized is a defensive check, not
    // the real guard - the constructor drops Miniaturizable from the
    // style mask, so a quick window should never actually get here with
    // it set; this is a fallback in case something iconifies it anyway.
    if (!value && quick && !window.miniaturized && visible()) {
        requestHide();
    }
}

static u16 modifiers(NSEventModifierFlags flags) {
    u16 result = 0;
    if (flags & NSEventModifierFlagShift) {
        result |= InputShift;
    }
    if (flags & NSEventModifierFlagControl) {
        result |= InputControl;
    }
    if (flags & NSEventModifierFlagOption) {
        result |= InputAlt;
    }
    if (flags & NSEventModifierFlagCommand) {
        result |= InputSuper;
    }
    if (flags & NSEventModifierFlagCapsLock) {
        result |= InputCapsLock;
    }
    return result;
}

static u32 firstCodepoint(NSString* string) {
    if (string.length == 0) {
        return 0;
    }
    const unichar first = [string characterAtIndex:0];
    if (CFStringIsSurrogateHighCharacter(first) && string.length > 1) {
        return CFStringGetLongCharacterForSurrogatePair(first, [string characterAtIndex:1]);
    }
    return first;
}

static InputKey inputKey(NSEvent* event) {
    switch (event.keyCode) {
        case kVK_ANSI_Keypad0:
            return InputKey::Keypad0;
        case kVK_ANSI_Keypad1:
            return InputKey::Keypad1;
        case kVK_ANSI_Keypad2:
            return InputKey::Keypad2;
        case kVK_ANSI_Keypad3:
            return InputKey::Keypad3;
        case kVK_ANSI_Keypad4:
            return InputKey::Keypad4;
        case kVK_ANSI_Keypad5:
            return InputKey::Keypad5;
        case kVK_ANSI_Keypad6:
            return InputKey::Keypad6;
        case kVK_ANSI_Keypad7:
            return InputKey::Keypad7;
        case kVK_ANSI_Keypad8:
            return InputKey::Keypad8;
        case kVK_ANSI_Keypad9:
            return InputKey::Keypad9;
        case kVK_ANSI_KeypadDecimal:
            return InputKey::KeypadDecimal;
        case kVK_ANSI_KeypadDivide:
            return InputKey::KeypadDivide;
        case kVK_ANSI_KeypadMultiply:
            return InputKey::KeypadMultiply;
        case kVK_ANSI_KeypadMinus:
            return InputKey::KeypadSubtract;
        case kVK_ANSI_KeypadPlus:
            return InputKey::KeypadAdd;
        case kVK_ANSI_KeypadEnter:
            return InputKey::KeypadEnter;
        case kVK_ANSI_KeypadEquals:
            return InputKey::KeypadEqual;
        case kVK_ANSI_KeypadClear:
            return InputKey::NumLock;
        default:
            break;
    }
    const u32 value = firstCodepoint(event.charactersIgnoringModifiers);
    switch (value) {
        case 0x1b:
            return InputKey::Escape;
        case '\r':
            return InputKey::Enter;
        case 0x7f:
            return InputKey::Backspace;
        case '\t':
        case NSBackTabCharacter:
            return InputKey::Tab;
        case NSInsertFunctionKey:
            return InputKey::Insert;
        case NSDeleteFunctionKey:
            return InputKey::Delete;
        case NSHomeFunctionKey:
            return InputKey::Home;
        case NSEndFunctionKey:
            return InputKey::End;
        case NSUpArrowFunctionKey:
            return InputKey::Up;
        case NSDownArrowFunctionKey:
            return InputKey::Down;
        case NSLeftArrowFunctionKey:
            return InputKey::Left;
        case NSRightArrowFunctionKey:
            return InputKey::Right;
        case NSPageUpFunctionKey:
            return InputKey::PageUp;
        case NSPageDownFunctionKey:
            return InputKey::PageDown;
        default:
            if (value >= NSF1FunctionKey && value <= NSF35FunctionKey) {
                return (InputKey)((u8)(InputKey::F1) + value - NSF1FunctionKey);
            }
            return value != 0 ? InputKey::Printable : InputKey::Unknown;
    }
}

// charactersIgnoringModifiers strips Shift and Option but not the layout:
// on a Russian layout the V key reports CYRILLIC EM, and neither the
// terminal bindings (Cmd+V) nor the kitty alternate-key field can match.
// Translate the physical key through the user's ASCII-capable layout -
// QWERTY for a Russian user, AZERTY for a French one - the way kitty and
// iTerm2 derive their base-layout key.
static u32 asciiBaseCodepoint(NSEvent* event) {
    TISInputSourceRef source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
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
        if (UCKeyTranslate(layout, event.keyCode, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &deadKeys, 4, &length, characters) == noErr && length != 0) {
            result = characters[0];
        }
    }
    CFRelease(source);
    return result;
}

void WindowImpl::key(NSEvent* event, bool pressed) {
    if (input == nullptr) {
        return;
    }
    input->key(keyInputFromEvent(event, pressed));
}

void WindowImpl::flushInput() {
    if (input != nullptr) {
        input->flush();
    }
}

void WindowImpl::preeditChanged(NSString* text) {
    if (input == nullptr) {
        return;
    }
    if (text == nil || text.length == 0) {
        if (preeditShown) {
            input->preedit({}, -1, -1);
            input->flush();
            preeditShown = false;
        }
        return;
    }
    NSData* const data = [text dataUsingEncoding:NSUTF8StringEncoding];
    if (data == nil) {
        return;
    }
    input->preedit(StringView((const u8*)(data.bytes), data.length), -1, -1);
    input->flush();
    preeditShown = true;
}

void WindowImpl::emitText(NSString* string, u16 mods) {
    if (input == nullptr || (mods & (InputControl | InputSuper))) {
        return;
    }
    const NSUInteger length = string.length;
    for (NSUInteger index = 0; index < length;) {
        const unichar first = [string characterAtIndex:index++];
        u32 codepoint = first;
        if (CFStringIsSurrogateHighCharacter(first)) {
            if (index == length) {
                continue;
            }
            const unichar second = [string characterAtIndex:index];
            if (!CFStringIsSurrogateLowCharacter(second)) {
                continue;
            }
            ++index;
            codepoint = CFStringGetLongCharacterForSurrogatePair(first, second);
        } else if (CFStringIsSurrogateLowCharacter(first)) {
            continue;
        }
        if (codepoint >= 0x20 && codepoint != 0x7f && !(codepoint >= 0xf700 && codepoint <= 0xf7ff)) {
            input->text({codepoint, mods});
        }
    }
    input->flush();
}

void WindowImpl::flags(NSEvent* event) {
    struct ModifierKey {
        u16 keyCode;
        u64 stateFlag;
        u64 otherStateFlag;
        u64 aggregateFlag;
        InputKey key;
    };

    const ModifierKey keys[] = {
        {56, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, NSEventModifierFlagShift, InputKey::LeftShift},
        {60, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, NSEventModifierFlagShift, InputKey::RightShift},
        {59, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, NSEventModifierFlagControl, InputKey::LeftControl},
        {62, NX_DEVICERCTLKEYMASK, NX_DEVICELCTLKEYMASK, NSEventModifierFlagControl, InputKey::RightControl},
        {58, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, NSEventModifierFlagOption, InputKey::LeftAlt},
        {61, NX_DEVICERALTKEYMASK, NX_DEVICELALTKEYMASK, NSEventModifierFlagOption, InputKey::RightAlt},
        {55, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, NSEventModifierFlagCommand, InputKey::LeftSuper},
        {54, NX_DEVICERCMDKEYMASK, NX_DEVICELCMDKEYMASK, NSEventModifierFlagCommand, InputKey::RightSuper},
        {57, NSEventModifierFlagCapsLock, 0, NSEventModifierFlagCapsLock, InputKey::CapsLock},
    };
    for (const ModifierKey& current : keys) {
        if (event.keyCode != current.keyCode) {
            continue;
        }
        const bool statePressed = (event.modifierFlags & current.stateFlag) != 0;
        const bool otherStatePressed = (event.modifierFlags & current.otherStateFlag) != 0;
        const bool aggregatePressed = (event.modifierFlags & current.aggregateFlag) != 0;
        const bool pressed = aggregatePressed != (statePressed || otherStatePressed) ? aggregatePressed : statePressed;
        if (input != nullptr) {
            input->key({.key = current.key, .action = pressed ? InputAction::Press : InputAction::Release, .modifiers = modifiers(event.modifierFlags)});
        }
        break;
    }
    if (input != nullptr) {
        input->flush();
    }
}

NSPoint WindowImpl::pointerPosition(NSEvent* event) const {
    NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
    point.y = view.bounds.size.height - point.y;
    return [view convertPointToBacking:point];
}

void WindowImpl::pointer(NSEvent* event) {
    if (input != nullptr) {
        const NSPoint point = pointerPosition(event);
        input->pointerMotion({(int)(point.x), (int)(point.y), modifiers(event.modifierFlags)});
        input->flush();
    }
}

void WindowImpl::button(NSEvent* event, bool pressed) {
    if (input == nullptr) {
        return;
    }
    const NSPoint point = pointerPosition(event);
    const i64 number = event.buttonNumber;
    const PointerButton button = number == 0 ? PointerButton::Primary : number == 1 ? PointerButton::Secondary : number == 2 ? PointerButton::Middle : (PointerButton)(min<i64>((i64)(PointerButton::Auxiliary5), (i64)(PointerButton::Auxiliary1) + number - 3));
    input->pointerButton({
        .button = button,
        .pressed = pressed,
        .pixelX = (int)(point.x),
        .pixelY = (int)(point.y),
        .modifiers = modifiers(event.modifierFlags),
        .time = event.timestamp,
    });
    input->flush();
}

ScrollPhase scrollPhase(NSEventPhase phase) {
    if (phase & (NSEventPhaseCancelled | NSEventPhaseMayBegin)) {
        return phase & NSEventPhaseCancelled ? ScrollPhase::Cancel : ScrollPhase::Begin;
    }
    if (phase & NSEventPhaseBegan) {
        return ScrollPhase::Begin;
    }
    if (phase & (NSEventPhaseChanged | NSEventPhaseStationary)) {
        return ScrollPhase::Update;
    }
    if (phase & NSEventPhaseEnded) {
        return ScrollPhase::End;
    }
    return ScrollPhase::None;
}

void WindowImpl::scroll(NSEvent* event) {
    if (input != nullptr) {
        const NSPoint point = pointerPosition(event);
        const double scale = event.hasPreciseScrollingDeltas ? 0.1 : 1.0;
        const bool momentum = event.momentumPhase != NSEventPhaseNone;
        input->scroll({
            .x = event.scrollingDeltaX * scale,
            .y = event.scrollingDeltaY * scale,
            .pixelX = (int)(point.x),
            .pixelY = (int)(point.y),
            .modifiers = modifiers(event.modifierFlags),
            .phase = scrollPhase(momentum ? event.momentumPhase : event.phase),
            .precise = event.hasPreciseScrollingDeltas,
            .momentum = momentum,
            .time = event.timestamp,
        });
        input->flush();
    }
}

void WindowImpl::pointerPresence(bool present) {
    if (input != nullptr) {
        input->pointerPresence(present);
        input->flush();
    }
}

void cocoaCloseImpl(void* owner) {
    ((WindowImpl*)(owner))->close();
}

void cocoaResizeImpl(void* owner) {
    ((WindowImpl*)(owner))->resized();
}

void cocoaFrameImpl(void* owner) {
    ((WindowImpl*)(owner))->draw();
}

void cocoaDisplayLayerImpl(void* owner) {
    ((WindowImpl*)(owner))->resizeFrame();
}

void cocoaInvalidateImpl(void* owner) {
    ((WindowImpl*)(owner))->requestFrame();
}

void cocoaScreenChangedImpl(void* owner) {
    ((WindowImpl*)(owner))->screenChanged();
}

NSRect cocoaTextInputRectImpl(void* owner) {
    return ((WindowImpl*)(owner))->textInputScreenRect();
}

NSSize cocoaWillResizeImpl(void* owner, NSSize frameSize) {
    return ((WindowImpl*)(owner))->willResize(frameSize);
}

void cocoaFocusImpl(void* owner, bool focused) {
    ((WindowImpl*)(owner))->focused(focused);
}

void cocoaKeyImpl(void* owner, NSEvent* event, bool pressed) {
    ((WindowImpl*)(owner))->key(event, pressed);
}

void cocoaTextImpl(void* owner, NSString* text, NSEventModifierFlags flags) {
    WindowImpl* const window = (WindowImpl*)(owner);
    window->emitText(text, modifiers(flags));
}

void cocoaFlushInputImpl(void* owner) {
    ((WindowImpl*)(owner))->flushInput();
}

void cocoaPreeditImpl(void* owner, NSString* text) {
    if (owner != nullptr) {
        ((WindowImpl*)(owner))->preeditChanged(text);
    }
}

void cocoaFlagsImpl(void* owner, NSEvent* event) {
    ((WindowImpl*)(owner))->flags(event);
}

void cocoaPointerImpl(void* owner, NSEvent* event) {
    ((WindowImpl*)(owner))->pointer(event);
}

void cocoaButtonImpl(void* owner, NSEvent* event, bool pressed) {
    ((WindowImpl*)(owner))->button(event, pressed);
}

void cocoaScrollImpl(void* owner, NSEvent* event) {
    ((WindowImpl*)(owner))->scroll(event);
}

void cocoaPointerPresenceImpl(void* owner, bool present) {
    ((WindowImpl*)(owner))->pointerPresence(present);
}

NSDragOperation cocoaDragOverImpl(void* owner, id<NSDraggingInfo> sender) {
    return ((WindowImpl*)(owner))->dragOver(sender);
}

void cocoaDragExitedImpl(void* owner) {
    ((WindowImpl*)(owner))->dragExited();
}

BOOL cocoaPerformDropImpl(void* owner, id<NSDraggingInfo> sender) {
    return ((WindowImpl*)(owner))->performDrop(sender);
}

void cocoaFileDescriptorReady(CFFileDescriptorRef descriptor, CFOptionFlags types, void* owner) {
    (void)types;
    ((PollerImpl*)(owner))->descriptorReady(descriptor);
}

void cocoaWakeReady(CFMachPortRef, void*, CFIndex, void* owner) {
    ((MachLoopWake*)(owner))->callback.ready();
}

void cocoaTimerReady(CFRunLoopTimerRef, void* owner) {
    ((PollerImpl*)(owner))->dispatchTimers();
}

KeyInput plt::keyInputFromEvent(NSEvent* event, bool pressed) {
    const InputAction action = !pressed ? InputAction::Release : (event.isARepeat ? InputAction::Repeat : InputAction::Press);
    u16 mods = modifiers(event.modifierFlags);
    const InputKey key = inputKey(event);
    if (key >= InputKey::Keypad0 && key <= InputKey::KeypadDecimal) {
        mods |= InputNumLock;
    }
    u32 layout = firstCodepoint(event.characters);
    u32 shifted = 0;
    const u32 rawBase = firstCodepoint(event.charactersIgnoringModifiers);
    u32 base = rawBase;
    // This frontend always exposes Option as the terminal Alt modifier.  Its
    // composed text (Option+F => ƒ, or an empty dead-key string) is therefore
    // not the key identity: translate the event without Option, while keeping
    // Alt in `mods`.  Native Option text, if it is ever made configurable,
    // must instead arrive through the text-input path with no Alt modifier.
    if (key == InputKey::Printable && (mods & InputAlt) && rawBase >= 0x20) {
        layout = rawBase;
    }
    if (key == InputKey::Printable && (mods & InputShift)) {
        // charactersIgnoringModifiers deliberately keeps Shift.  Ask AppKit
        // for the two active-layout levels explicitly, so Shift+A and
        // Shift+5 retain both the unshifted key and the produced alternate
        // on key-up as well as key-down.
        const u32 unshifted = firstCodepoint(
            [event charactersByApplyingModifiers:0]
        );
        if (unshifted != 0) {
            layout = unshifted;
        }
        shifted = firstCodepoint(
            [event charactersByApplyingModifiers:NSEventModifierFlagShift]
        );
        if (shifted == 0) {
            shifted = firstCodepoint(event.characters);
        }
    }
    if (key == InputKey::Printable && (base >= 0x80 || (mods & InputShift))) {
        const u32 ascii = asciiBaseCodepoint(event);
        if (ascii >= 0x20 && ascii < 0x7f) {
            base = ascii;
        }
    }
    // Cocoa folds Control into characters: Ctrl+B reports STX, where xkbcommon
    // reports 'b'. A C0 control is never a layout key, and the kitty key field
    // needs the layout one - reporting 2 instead of 98 kills every multiplexer
    // prefix. The recovery restores the unfolded active-layout key - the raw
    // charactersIgnoringModifiers, before the ASCII correction - so a Russian
    // Ctrl+B reports the active-layout letter with the Latin key in the base
    // field, exactly like the Wayland backend's level-zero identity. The named
    // keys carry their own codes, so only printables recover.
    if (key == InputKey::Printable && layout < 0x20 && rawBase >= 0x20) {
        layout = rawBase;
    }
    return {
        .key = key,
        .action = action,
        .modifiers = mods,
        .layoutCodepoint = layout,
        .baseCodepoint = base,
        .shiftedCodepoint = shifted,
    };
}

Platform* plt::createCocoaPlatform(ObjPool& owner) {
    return owner.make<PlatformImpl>(owner);
}
