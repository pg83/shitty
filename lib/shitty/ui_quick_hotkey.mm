/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_quick_hotkey.h"

#include "brand.h"
#include "composer.h"
#include "options.h"
#include "quick_frame_store.h"
#include "quick_hotkey_chord.h"

#include <plt/window.h>

#include <std/alg/minmax.h>
#include <std/ios/sys.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sys/types.h>

// Same guard ui_csd_tabs.mm uses: AppKit pulls in the legacy QuickDraw
// Point/Rect typedefs transitively, which would collide with
// lib/shitty/point.h and rect.h if this file ever needs them through
// composer.h - it does not today, but there is no reason for this file
// to be the one exception to a precedent it otherwise follows closely.
#define Point MacLegacyPoint
#define Rect MacLegacyRect

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#undef Rect
#undef Point

using namespace stl;

namespace {
    // B4/B7's write-back guard, in two halves: whether the frame the last
    // show put on screen was computed here rather than restored verbatim
    // from what the user saved, and - when it was - exactly which frame
    // that is. File-scope because there is exactly one quick window per
    // process, which the Carbon hotkey, the resign-key observer and
    // applyQuickFrameToWindow() all already build on.
    //
    // The rule both halves spell out: never write back a frame this very
    // show computed. A saved frame that no longer fits - its display is
    // unplugged, the screen shrank, the arrangement moved - is adapted so
    // the window is reachable at all, but where that adaptation lands
    // says nothing about where the user wants the window, and persisting
    // it replaces the real placement for good, a little further off on
    // every show (R2-qa round 2, B4; round 3, B7).
    //
    // The second half is what keeps that from being a dead end. Comparing
    // the live frame against the computed one asks the only question that
    // matters - "is the window still exactly where this code put it?" -
    // so the moment the user drags or resizes it, the frame stops
    // matching and the placement is saved again. Deliberately a frame
    // comparison rather than an NSWindowDidMove/DidResize subscription:
    // restoring a frame is itself followed by a grid resize
    // (applySavedQuickFrame, application.cpp), which posts those same
    // notifications, and a guard that clears itself on our own adjustment
    // guards nothing.
    bool quickFrameComputed = false;
    NSRect quickFrameComputedFrame = NSZeroRect;

    // Registers Options::quickHotkey (and, best-effort, quickFullscreenHotkey)
    // as Carbon global hotkeys for the lifetime of the pool it lives in,
    // and hands every press to toggleQuickWindow() or the fullscreen
    // toggle below. Also observes the window resigning key status to
    // persist a dragged/resized frame (quickRememberFrame). A pool
    // object like createCsdTabsUi's CsdTabsUi: fire-and-forget, torn
    // down by its owning ObjPool's destructor (RegisterEventHotKey's own
    // docs say the registration itself needs no explicit cleanup at
    // process exit - "the system will take care of that for you" - but
    // unregistering explicitly here is what makes a hypothetical future
    // reconfiguration, or any path that destroys this object while the
    // process keeps running, safe rather than relying on that fallback).
    struct QuickHotkeyUi {
        explicit QuickHotkeyUi(Composer& composer);
        ~QuickHotkeyUi();

        // Shared by both hotkeys: parses chord, rejects a bare key with
        // no modifier (it would grab that key system-wide), registers
        // it under id, and reports every failure through name/disabledText.
        // Returns whether it ended up registered.
        bool registerHotkey(StringView chord, EventHotKeyID id, EventHotKeyRef& outRef, const char* name, const char* disabledText);

        Composer& composer;
        EventHotKeyRef hotkeyRef = nullptr;
        EventHotKeyRef fullscreenHotkeyRef = nullptr;
        EventHandlerRef handlerRef = nullptr;
        // True only once both the handler and quickHotkey itself are
        // actually registered; createQuickHotkey() reports this back so
        // the caller can show the window normally instead of leaving it
        // unreachable when it stays false. Independent of
        // fullscreenHotkeyRef - a broken quickFullscreenHotkey never
        // affects this.
        bool active = false;
        // The frame from just before the fullscreen chord last expanded
        // the window, so the same chord collapses it back exactly.
        // Purely in-memory and unrelated to quickRememberFrame's on-disk
        // state (quick_frame_store.h): that persists a user's manually
        // dragged/resized frame across shows; this is an ephemeral undo
        // point for one toggle.
        NSRect priorFullscreenFrame = NSZeroRect;
        // The frame actually reached last time this expanded the window -
        // read back right after -setFrame:, not recomputed from
        // screen.visibleFrame on every press. A titled window's frame
        // can differ from what was requested (AppKit keeps it clear of
        // the menu bar), so comparing against the live screen rect made
        // the fold-back branch permanently unreachable (F2's report, B3).
        NSRect appliedFullscreenFrame = NSZeroRect;
        bool hasPriorFullscreenFrame = false;
        // NSWindowDidResignKeyNotification observer for quickRememberFrame;
        // nil when the option is off or the window has no native handle
        // yet. Independent of both hotkeys - registered even if neither
        // chord parsed, since it needs no Carbon hotkey to work, only
        // the option.
        id<NSObject> resignKeyObserver = nil;
    };

    static void toggleQuickWindowFullscreen(QuickHotkeyUi& self) {
        if (self.composer.window == nullptr || !self.composer.window->visible() || self.composer.window->info().iconified) {
            return;
        }
        const plt::RenderContext context = self.composer.window->renderContext();
        // See applyQuickFrameToWindow()'s own comment below: the backend
        // tag has to be checked before the bridge cast, not just
        // nullness - every backend hands back a non-null .window, and on
        // a non-Cocoa one it does not point at an NSWindow at all. This
        // function is only ever reached from the Carbon hotkey handler
        // above, which only exists in this Cocoa-only .mm file, but the
        // guard costs nothing and matches the sibling function exactly.
        if (context.backend != plt::RenderBackend::Cocoa) {
            return;
        }
        NSWindow* const window = (__bridge NSWindow*)(context.window);
        if (window == nil) {
            return;
        }
        NSScreen* const screen = window.screen != nil ? window.screen : [NSScreen mainScreen];
        if (screen == nil) {
            return;
        }
        if (self.hasPriorFullscreenFrame && NSEqualRects(window.frame, self.appliedFullscreenFrame)) {
            [window setFrame:self.priorFullscreenFrame display:YES animate:YES];
            // Restored after the frame shrinks back, matching "corners
            // are round exactly while not covering the whole screen".
            self.composer.window->requestCornerRadius(self.composer.opts->quickCornerRadius);
            self.hasPriorFullscreenFrame = false;
            return;
        }
        self.priorFullscreenFrame = window.frame;
        self.hasPriorFullscreenFrame = true;
        // Squared off before the frame grows, so a "fullscreen" window
        // never shows desktop through masked corners even for one
        // transient frame.
        self.composer.window->requestCornerRadius(0);
        // visibleFrame, not frame: matches quickGeometry's own convention
        // (topOfActiveScreenFrame(), platform_cocoa.mm, also visibleFrame)
        // and is the region a titled window can actually occupy without
        // AppKit silently keeping it clear of the menu bar strip (measured:
        // a titled window's resulting frame lands ~30pt short of
        // screen.frame's own height - the mismatch that made the
        // window.frame == screen.frame comparison here never true).
        [window setFrame:screen.visibleFrame display:YES animate:YES];
        self.appliedFullscreenFrame = window.frame;
    }

    static OSStatus quickHotkeyPressed(EventHandlerCallRef, EventRef event, void* userData) {
        QuickHotkeyUi* const self = (QuickHotkeyUi*)(userData);
        EventHotKeyID pressedId{};
        if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(pressedId), nullptr, &pressedId) == noErr && pressedId.id == 2) {
            toggleQuickWindowFullscreen(*self);
            return noErr;
        }
        toggleQuickWindow(self->composer);
        return noErr;
    }
}

bool QuickHotkeyUi::registerHotkey(StringView chord, EventHotKeyID id, EventHotKeyRef& outRef, const char* name, const char* disabledText) {
    u32 modifiers = 0;
    u32 keyCode = 0;
    if (!parseQuickHotkey(chord, modifiers, keyCode)) {
        sysE << composer.brand->identifier() << StringView(u8": ") << StringView(name) << StringView(u8": unrecognized chord '") << chord << StringView(u8"'; ") << StringView(disabledText) << StringView(u8" is disabled") << endL;
        return false;
    }
    if (modifiers == 0) {
        // A bare key with no modifier would grab that key system-wide -
        // every application, every text field, for as long as this
        // process runs. RegisterEventHotKey accepts it without complaint
        // (verified: it returns noErr the same as any other chord), so
        // this has to be rejected here instead of relied on to fail.
        sysE << composer.brand->identifier() << StringView(u8": ") << StringView(name) << StringView(u8": '") << chord << StringView(u8"' has no modifier (ctrl/shift/alt/super); ") << StringView(disabledText) << StringView(u8" is disabled") << endL;
        return false;
    }
    if (RegisterEventHotKey(keyCode, modifiers, id, GetApplicationEventTarget(), 0, &outRef) != noErr) {
        // Not a same-chord conflict: Carbon lets multiple processes (and
        // even this chord already held elsewhere) register the very same
        // combination without complaint (verified). A real refusal here
        // has some other cause the system does not report back.
        sysE << composer.brand->identifier() << StringView(u8": ") << StringView(name) << StringView(u8": the system refused to register '") << chord << StringView(u8"'; ") << StringView(disabledText) << StringView(u8" is disabled") << endL;
        outRef = nullptr;
        return false;
    }
    return true;
}

QuickHotkeyUi::QuickHotkeyUi(Composer& composer_)
    : composer(composer_)
{
    const EventTypeSpec pressed = {kEventClassKeyboard, kEventHotKeyPressed};
    if (InstallEventHandler(GetApplicationEventTarget(), quickHotkeyPressed, 1, &pressed, this, &handlerRef) != noErr) {
        sysE << composer.brand->identifier() << StringView(u8": quickHotkey: could not install the event handler; the quick-terminal hotkey is disabled") << endL;
        handlerRef = nullptr;
    } else {
        active = registerHotkey(composer.opts->quickHotkey, EventHotKeyID{(OSType)(1), 1}, hotkeyRef, "quickHotkey", "the quick-terminal hotkey");
        // Empty means disabled (Options only stores and never requires
        // this one non-empty, unlike quickHotkey above) - no diagnostic
        // for that case, same as quickCompanion's own "no value" shape.
        if (!composer.opts->quickFullscreenHotkey.empty()) {
            registerHotkey(composer.opts->quickFullscreenHotkey, EventHotKeyID{(OSType)(1), 2}, fullscreenHotkeyRef, "quickFullscreenHotkey", "the quick-terminal fullscreen hotkey");
        }
        if (!active && fullscreenHotkeyRef == nullptr) {
            // Neither chord took - nothing left listening on this handler.
            RemoveEventHandler(handlerRef);
            handlerRef = nullptr;
        }
    }

    if (composer.opts->quickRememberFrame && composer.window != nullptr && composer.window->renderContext().backend == plt::RenderBackend::Cocoa) {
        NSWindow* const window = (__bridge NSWindow*)(composer.window->renderContext().window);
        if (window != nil) {
            QuickHotkeyUi* const self = this;
            // Fires for both hide paths - the explicit hotkey
            // (toggleQuickWindow, application.cpp) and hide-on-resign-key
            // (WindowImpl::focused, platform_cocoa.mm) - since a key
            // window resigns key as part of being ordered out either
            // way. The frame is valid to read regardless of exactly when
            // orderOut: runs relative to this notification: hiding never
            // changes window.frame. A local block variable, rather than
            // one inline in the call below, keeps the four-keyword
            // selector on one short line instead of clang-format
            // spreading it across a deeply indented column.
            void (^saveFrame)(NSNotification*) = ^(NSNotification*) {
              if (self->hasPriorFullscreenFrame) {
                  // The fullscreen chord's own frame is not a manual
                  // placement - the option is documented as remembering
                  // what the user "manually set" (shitty.toml). Saving it
                  // here would silently overwrite the user's actual
                  // dragged/resized frame the next time the window hides
                  // while still expanded (F2's report, I2).
                  return;
              }
              const QuickFrameRect computed{
                  .x = quickFrameComputedFrame.origin.x,
                  .y = quickFrameComputedFrame.origin.y,
                  .width = quickFrameComputedFrame.size.width,
                  .height = quickFrameComputedFrame.size.height,
              };
              const QuickFrameRect live{
                  .x = window.frame.origin.x,
                  .y = window.frame.origin.y,
                  .width = window.frame.size.width,
                  .height = window.frame.size.height,
              };
              if (!quickFrameShouldSave(quickFrameComputed, computed, live)) {
                  // The window is still exactly where this show's clamp
                  // put it (applyQuickFrameToWindow): a frame computed
                  // here, not a placement the user made (B4, B7). The
                  // rule and the reason it is a frame comparison rather
                  // than a flag alone are in quick_frame_store.h, where
                  // the tests can reach them.
                  return;
              }
              StringBuilder path;
              if (!defaultQuickFramePath(self->composer.opts->configPath, path)) {
                  return;
              }
              // Points on both axes and both sizes (quick_frame_store.h):
              // the frame's own origin, and the content rect the frame
              // encloses, asked of this very window rather than derived
              // from a scale factor that means something different on
              // the next display (B4).
              const NSRect content = [window contentRectForFrameRect:window.frame];
              const QuickFrame frame{
                  .x = (i32)(window.frame.origin.x),
                  .y = (i32)(window.frame.origin.y),
                  .width = (u32)(max(1.0, content.size.width)),
                  .height = (u32)(max(1.0, content.size.height)),
              };
              if (!saveQuickFrame(StringView(path), frame)) {
                  // Best-effort by design (quick_frame_store.h) but not
                  // silent: a directory that does not exist or is not
                  // writable otherwise leaves quickRememberFrame looking
                  // broken with no diagnostic at all (F2's report, I3).
                  sysE << self->composer.brand->identifier() << StringView(u8": quickRememberFrame: could not save the window frame to ") << StringView(path) << endL;
              }
            };
            resignKeyObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification object:window queue:nil usingBlock:saveFrame];
        }
    }
}

QuickHotkeyUi::~QuickHotkeyUi() {
    if (resignKeyObserver != nil) {
        [[NSNotificationCenter defaultCenter] removeObserver:resignKeyObserver];
    }
    if (fullscreenHotkeyRef != nullptr) {
        UnregisterEventHotKey(fullscreenHotkeyRef);
    }
    if (hotkeyRef != nullptr) {
        UnregisterEventHotKey(hotkeyRef);
    }
    if (handlerRef != nullptr) {
        RemoveEventHandler(handlerRef);
    }
}

bool createQuickHotkey(ObjPool& owner, Composer& composer) {
    return owner.make<QuickHotkeyUi>(composer)->active;
}

bool applyQuickFrameToWindow(Composer& composer, const QuickFrame& frame) {
    if (composer.window == nullptr) {
        return false;
    }
    const plt::RenderContext context = composer.window->renderContext();
    // Every backend hands back a non-null .window - headless points it
    // at its own internal render target, not an NSWindow at all - so the
    // backend tag has to be checked before the bridge cast runs, not
    // just nullness (this crashed the headless regression tests: the
    // pointer bridged fine, sending it any Objective-C message did not).
    if (context.backend != plt::RenderBackend::Cocoa) {
        return false;
    }
    NSWindow* const window = (__bridge NSWindow*)(context.window);
    if (window == nil) {
        return false;
    }

    // What the saved content size costs once the window's own chrome is
    // around it, asked of this window rather than assumed: the titlebar
    // is part of what has to fit the screen (R2-qa round 2, Z2). Only
    // the height differs - -frameRectForContentRect: never widens a
    // frame for any style mask this window can carry.
    const CGFloat probeSize = 100;
    const NSRect probe = [window frameRectForContentRect:NSMakeRect(0, 0, probeSize, probeSize)];
    const double titlebarHeight = probe.size.height - probeSize;
    // The saved frame as a whole window frame, in the same shape
    // quickFrameTarget() would hand back if it left it alone.
    const QuickFrameRect saved{
        .x = (double)(frame.x),
        .y = (double)(frame.y),
        .width = max(1.0, (double)(frame.width)),
        .height = max(1.0, (double)(frame.height)) + max(0.0, titlebarHeight),
    };

    // Asked once and kept: the two loops below have to agree on which
    // rect belongs to which screen.
    NSArray<NSScreen*>* const screens = [NSScreen screens];
    Vector<QuickFrameRect> screenFrames;
    for (NSScreen* const candidate in screens) {
        screenFrames.pushBack({
            .x = candidate.frame.origin.x,
            .y = candidate.frame.origin.y,
            .width = candidate.frame.size.width,
            .height = candidate.frame.size.height,
        });
    }

    // A frame that is entirely on the displays that are attached is
    // applied exactly as saved - no screen picked for it, no clamp. A
    // window straddling two monitors has its origin on one of them, and
    // clamping it into that one screen is what dragged it wholesale off
    // the other and then wrote the result back over the user's own
    // placement (R2-qa round 3, B7). Screen frames rather than visible
    // frames on purpose: the strip a neighbouring display reserves for
    // its menu bar is exactly what such a frame reaches across, and it
    // is not a reason to move the window.
    QuickFrameRect target = saved;
    const bool fits = quickFrameFitsScreens(saved, screenFrames.data(), screenFrames.length());
    if (!fits) {
        // Part of the frame is nowhere - its display is unplugged, the
        // arrangement moved, a screen shrank - so it does need a screen
        // picked for it. The one it overlaps most, not the one its
        // origin happens to land in: a frame mostly on one display with
        // its top-left corner poking into another used to be dragged
        // onto the smaller share of itself. Zero overlap everywhere -
        // the display it was saved on is simply gone - leaves the
        // window's own screen, which is where requestShowAt() just put
        // it (WindowImpl::topOfActiveScreenFrame, platform_cocoa.mm).
        NSScreen* screen = nil;
        double bestOverlap = 0;
        for (size_t at = 0; at < screenFrames.length(); ++at) {
            const double overlap = quickFrameOverlap(saved, screenFrames[at]);
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                screen = screens[at];
            }
        }
        if (screen == nil) {
            screen = window.screen != nil ? window.screen : [NSScreen mainScreen];
        }
        if (screen == nil) {
            return false;
        }
        const QuickFrameRect visible{
            .x = screen.visibleFrame.origin.x,
            .y = screen.visibleFrame.origin.y,
            .width = screen.visibleFrame.size.width,
            .height = screen.visibleFrame.size.height,
        };
        target = quickFrameTarget(frame, visible, titlebarHeight);
    }

    // One atomic call, not requestMove()+requestResize(): the latter set
    // the origin against the window's still-old size (requestResize() is
    // asynchronous by design, see its own comment in platform_cocoa.mm),
    // then let -setContentSize: hold the top edge while the bottom moved -
    // drifting the saved position by the height delta on every show
    // (F2's report: measured -80pt/cycle). display:YES applies this
    // immediately instead of deferring like requestResize() does.
    [window setFrame:NSMakeRect(target.x, target.y, target.width, target.height) display:YES animate:NO];
    // Read back rather than assumed: AppKit is free to answer with a
    // frame of its own, and it is the frame the resign-key observer will
    // compare against that has to be recorded here.
    quickFrameComputed = !fits;
    quickFrameComputedFrame = window.frame;
    return true;
}
