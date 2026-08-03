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


#include <errno.h>
#include <float.h>
#include <limits.h>
#include <new>
#include <poll.h>

using namespace stl;
using namespace plt;

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


@interface PltDisplayLinkTarget: NSObject {
@public
    plt::cocoa_detail::DisplayLinkGate gate;
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

    struct WindowImpl final: public Window {
        WindowImpl(PlatformImpl& platform, const WindowOptions& options);
        ~WindowImpl();

        void requestShow() override;
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
        void requestResize(u32 width, u32 height) override;
        void requestMinimumSize(u32 width, u32 height) override;
        void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        WindowInfo info() const override;
        Clipboard* primary() override;
        Clipboard* secondary() override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestOpenUri(StringView uri) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        RenderContext renderContext() const override;

        void close();
        void resized();
        void resizeFrame();
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

        PlatformImpl& platform;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        DropTarget* dropTarget = nullptr;
        NSWindow* window = nil;
        PltView* view = nil;
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
        ClipboardImpl primaryPasteboard;
        ClipboardImpl generalPasteboard;
        bool frameRequested = false;
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

        PollerImpl* poller_ = nullptr;
        SmallObjAllocator* allocator_ = nullptr;
        Scheduler* scheduler_ = nullptr;
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

}

PlatformImpl::PlatformImpl(ObjPool& owner)
    : poller_(owner.make<PollerImpl>(owner))
    , allocator_(SmallObjAllocator::create(&owner))
    , scheduler_(Scheduler::create(owner, *poller_))
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
}

Window* PlatformImpl::createWindow(ObjPool& owner, const WindowOptions& options) {
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
    [NSApp run];
}

void PlatformImpl::stop() {
    [NSApp stop:nil];
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:event atStart:NO];
}

WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
    : platform(platform_)
    , input(options.input)
    , events(options.events)
    , frame(options.frame)
    , dropTarget(options.drop)
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
    const NSRect frame = NSMakeRect(0, 0, max(1u, options.width), max(1u, options.height));
    window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    delegate = [PltWindowDelegate new];
    delegate.owner = this;
    window.delegate = delegate;
    view = [[PltView alloc] initWithFrame:frame];
    view.owner = this;
    view.wantsLayer = YES;
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    window.contentView = view;
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
    resized();
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
    if (displayLink != nullptr && !CVDisplayLinkIsRunning(displayLink)) {
        CVDisplayLinkStart(displayLink);
    }
}

void WindowImpl::draw() {
    if (!frameRequested || frame == nullptr) {
        stopDisplayLink();
        return;
    }
    frameRequested = false;
    frame->frame(info());
    if (!frameRequested) {
        stopDisplayLink();
    }
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
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void WindowImpl::requestMaximized(bool value) {
    if ([window isZoomed] != value) {
        [window zoom:nil];
    }
}

void WindowImpl::requestFullscreen(bool value) {
    const bool current = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (current != value) {
        [window toggleFullScreen:nil];
    }
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
        .window = nullptr,
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
    if ((window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
        // A fullscreen window must fill its tile exactly; snapping the
        // proposal down to cell increments fights the window server,
        // visibly so with large cells.
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

void WindowImpl::scroll(NSEvent* event) {
    if (input != nullptr) {
        const NSPoint point = pointerPosition(event);
        const double scale = event.hasPreciseScrollingDeltas ? 0.1 : 1.0;
        input->scroll({
            .x = event.scrollingDeltaX * scale,
            .y = event.scrollingDeltaY * scale,
            .pixelX = (int)(point.x),
            .pixelY = (int)(point.y),
            .modifiers = modifiers(event.modifierFlags),
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
    u32 base = firstCodepoint(event.charactersIgnoringModifiers);
    if (key == InputKey::Printable && base >= 0x80) {
        const u32 ascii = asciiBaseCodepoint(event);
        if (ascii >= 0x20 && ascii < 0x7f) {
            base = ascii;
        }
    }
    // Cocoa folds Control into characters: Ctrl+B reports STX, where xkbcommon
    // reports 'b'. A C0 control is never a layout key, and the kitty key field
    // needs the layout one - reporting 2 instead of 98 kills every multiplexer
    // prefix. The named keys carry their own codes, so only printables recover.
    if (key == InputKey::Printable && layout < 0x20 && base >= 0x20) {
        layout = base;
    }
    return {
        .key = key,
        .action = action,
        .modifiers = mods,
        .layoutCodepoint = layout,
        .baseCodepoint = base,
    };
}

Platform* plt::createCocoaPlatform(ObjPool& owner) {
    return owner.make<PlatformImpl>(owner);
}
