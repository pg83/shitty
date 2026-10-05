/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_csd_tabs.h"

#include "render_blend.h"

#include "brand.h"
#include "options.h"
#include "session.h"
#include "composer.h"

#include <lib/vterm/listener.h>

#include <std/str/view.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>

#include <plt/window.h>

#define Point MacLegacyPoint
#define Rect MacLegacyRect

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#undef Rect
#undef Point

// After AppKit, and it has to be: see the header.
#include "ui_window_tint.h"

#include <stdio.h>

// @available guards the runtime; building against an older SDK also needs the
// declarations to exist at all. Same pair, same spelling, as the one
// ext/plt/platform_cocoa.mm keeps around NSGlassEffectView itself.
#if defined(MAC_OS_VERSION_26_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_26_0
    #define UI_SDK_MACOS_26 1
#else
    #define UI_SDK_MACOS_26 0
#endif

#if defined(MAC_OS_VERSION_27_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_27_0
    #define UI_SDK_MACOS_27 1
#else
    #define UI_SDK_MACOS_27 0
#endif

using namespace stl;

#if UI_SDK_MACOS_27
// The title bar strip's sheet of glass from macOS 27 on. Same reason for the
// subclass as TerminalGlassPillView in ui_sidebar_tabs.mm - the shape lives
// behind a readonly property and is stated by overriding its getter - and the
// same reason for the neutral name.
//
// Concentric along the top only, and square along the bottom, which is the
// shape the flat zero it replaces was standing in for.
//
// The top corners are the window's own: this strip runs the full width into
// the frame's rounded corners, and concentric is how a view says "whatever
// the window's corner is". Measured live on this machine - the verbose line
// below prints it - the fill view this sheet lives in does hand the window's
// shape down, and a plain concentric configuration resolves to 16 points on
// ALL FOUR corners. Two of those are the strip's bottom edge, which is a
// straight seam across the middle of the window: rounding them would be a
// visible defect, and it is the only reason this class does not simply ask
// for containerConcentric and stop.
//
// A floor of zero rather than of the window's radius, because a window that
// declares no shape (a borderless one, where NSNextStepFrame reports nil)
// must leave the strip exactly as square as cornerRadius left it before.
API_AVAILABLE(macos(27.0))
@interface TerminalGlassStripView: NSGlassEffectView
@end

@implementation TerminalGlassStripView

- (NSViewCornerConfiguration*)cornerConfiguration {
    NSViewCornerRadius* const square = [NSViewCornerRadius fixedRadius:0];
    return [NSViewCornerConfiguration configurationWithUniformTopRadius:
                [NSViewCornerRadius containerConcentricRadiusWithMinimum:0]
                                                      bottomLeftRadius:square
                                                     bottomRightRadius:square];
}

@end
#endif

namespace {
    struct CsdTabsUi;
}

// A7: one hover transition of the auto-hiding title bar, and the whole
// of what one is allowed to touch. It takes the Composer rather than a
// CsdTabsUi because it needs nothing else: the strip's reserve is set
// once when the mode turns on and is never revisited here, so there is
// no per-hover state to keep and nothing that could re-count the grid.
// Not static, and declared again in ui_csd_tabs_ut.cpp: a unit test can
// build a Composer but not an NSTrackingArea, and this is the one path
// A7 forbids from touching geometry, so it has to be callable from one.
void csdTabsChromeHovered(Composer& composer, bool inside);

// The iTerm2 look: the title bar itself, split into tabs by hairline
// separators. The view sits inside the titlebar container next to the
// traffic lights; the native title is hidden while it shows. The view
// owns no model - it reads labels and the active index through its
// owner, which outlives it.
@interface TerminalTabBarView: NSView {
@public
    CsdTabsUi* owner;
}
@end

// The title bar's own tint, and nothing else: a plain fill behind the
// standard window buttons, in the strip AppKit stops painting once
// titlebarAppearsTransparent is on. It answers no clicks at all, so the
// bare title bar keeps dragging the window and zooming on a double
// click exactly as it does without it.
@interface TerminalTitlebarFillView: NSView
@end

// A7: the mouse tracker that reveals the auto-hidden title bar. It sits
// in the titlebar container and covers exactly it, so the strip that
// reveals the chrome is the strip the chrome occupies, with no
// coordinate arithmetic to get wrong and nothing to re-derive when the
// window is resized. It draws nothing and answers no clicks; all it
// does is tell the Composer the pointer arrived or left.
@interface TerminalChromeHoverView: NSView {
    @public
    Composer* composer;
}
@end

namespace {
    struct CallSessionsChanged final: public Listener {
        explicit CallSessionsChanged(CsdTabsUi* parent);

        void onListen(void*) override;

        CsdTabsUi* parent;
    };

    // Fires on every config reload (SIGUSR1), not just ones that touch
    // colors - the listener list carries no diff, so this repaints the
    // titlebar fill and the tab strip unconditionally. Reacting to an
    // OSC-driven background change from inside the running shell is a
    // separate, larger feature (the window would need to hear about it
    // at all) and stays out of scope here.
    struct CallConfigChanged final: public Listener {
        explicit CallConfigChanged(CsdTabsUi* parent);

        void onListen(void*) override;

        CsdTabsUi* parent;
    };

    // Listens to the tab model and mirrors it into the title bar. All
    // AppKit work runs on the main queue: the listener fires on client
    // fibers (the input pump delivers tab chords, the parser fiber
    // delivers titles), and AppKit layout has no business on a fiber
    // stack. The fibers themselves run on the main thread, so the
    // deferred block never races the snapshot it reads.
    struct CsdTabsUi {
        explicit CsdTabsUi(Composer& composer);

        void project();
        void apply();
        void applyTitlebarColor();
        bool applyTitlebarGlass(NSWindow* window);
        void dropTitlebarGlass();
        void applyAutoHideChrome();
        void tabSelected(size_t index);
        void tabClosed(size_t index);
        void tabOpened();

        Composer& composer;
        CallSessionsChanged sessionsChanged{this};
        CallConfigChanged configChanged{this};
        TerminalTabBarView* bar = nil;
        // The title bar's tint, installed on demand by
        // applyTitlebarColor() and removed again when a reload turns
        // transparentTitlebar off; nil whenever the option is off.
        TerminalTitlebarFillView* titlebarFill = nil;
        // The strip's own sheet of glass, or nil when the window has no
        // glass backdrop to continue. It lives *inside* titlebarFill
        // rather than beside it, which is what keeps the title bar
        // draggable: that view answers nil to hitTest: and a nil answer
        // takes its whole subtree out of hit testing with it. Typed
        // NSView* so the field needs no availability annotation of its
        // own, the way WindowImpl::glassBackdrop is.
        NSView* titlebarGlass = nil;
        // The hover tracker, installed while autoHideChrome is on and
        // removed again when a reload turns it off; nil otherwise.
        TerminalChromeHoverView* chromeHover = nil;
        // The projected model snapshot the view draws from; nil hides
        // the strip (a lone session keeps the clean native title).
        NSArray<NSString*>* labels = nil;
        size_t active = 0;
        bool applyPending = false;
    };
}

CallSessionsChanged::CallSessionsChanged(CsdTabsUi* parent_)
    : parent(parent_)
{
}

void CallSessionsChanged::onListen(void*) {
    parent->project();
}

CallConfigChanged::CallConfigChanged(CsdTabsUi* parent_)
    : parent(parent_)
{
}

void CallConfigChanged::onListen(void*) {
    parent->applyTitlebarColor();
    // A reload can turn autoHideChrome on or off, same as every other
    // option this module projects; the reserve and the full-size content
    // view follow it either way, so the grid never keeps paying for a
    // strip nobody hides anymore.
    parent->applyAutoHideChrome();
    // And a reload can turn -sidebarTabs on or off, which decides
    // whether this module's own strip is redundant (V2). The answer is
    // read on the main queue inside apply(), by which time the sidebar's
    // own listener has run whichever order the two were registered in.
    parent->project();
}

namespace {
    // sRGB, the space the terminal itself renders in: a calibrated color
    // would land beside the grid it is supposed to continue or blend into.
    static NSColor* nsColorFromTerminalColor(Color color, CGFloat alpha = 1.0) {
        return [NSColor colorWithSRGBRed:color.red / 255.0 green:color.green / 255.0 blue:color.blue / 255.0 alpha:alpha];
    }

    // Whether the window is backed by the system's glass, asked of the
    // live view hierarchy and deliberately not re-derived from the
    // options - the same question ui_sidebar_tabs.mm asks, spelled the
    // same way and for the same reason. Three things decide it and only
    // one is an option: -backgroundBlur glass, a system that has
    // NSGlassEffectView at all, and -backgroundOpacity below 100. All
    // three are weighed in ext/plt/platform_cocoa.mm, which then either
    // installs the glass or silently falls back to the frosted pane;
    // reading the option here would answer "glass" on a machine that can
    // never show any, and the strip would stand its own tint down for a
    // backdrop that is not there.
    static bool windowBackdropIsGlass(NSWindow* window) {
#if UI_SDK_MACOS_26
        if (@available(macOS 26.0, *)) {
            NSView* const content = window == nil ? nil : window.contentView;
            NSView* const frame = content == nil ? nil : content.superview;
            for (NSView* const sibling in frame.subviews) {
                if ([sibling isKindOfClass:[NSGlassEffectView class]]) {
                    return true;
                }
            }
        }
#else
        (void)window;
#endif
        return false;
    }

    // Every backend hands back a non-null .window - the headless one
    // points it at its own render target, not at an NSWindow - so the
    // backend tag has to be checked before the bridge cast runs, not
    // just nullness: the pointer bridges fine and sending it any
    // Objective-C message does not (this crashed a headless probe test,
    // R2-qa round 2, B5). The single cast in this file lives here, so
    // every caller inherits the guard by asking for nil.
    static NSWindow* nativeWindow(const Composer& composer) {
        if (composer.window == nullptr) {
            return nil;
        }
        const plt::RenderContext context = composer.window->renderContext();
        if (context.backend != plt::RenderBackend::Cocoa) {
            return nil;
        }
        return (__bridge NSWindow*)(context.window);
    }

    // The view AppKit keeps the standard window buttons in - the title
    // bar's own strip, and the surface everything this module puts in
    // the title bar goes into. Reached through the zoom button because
    // the container itself is private API; nil when the window has no
    // decorations to hold one.
    static NSView* titlebarContainer(NSWindow* window) {
        NSButton* const zoom = [window standardWindowButton:NSWindowZoomButton];
        return zoom != nil ? zoom.superview : nil;
    }

    // Whether the mode is live at all: an undecorated window has no
    // title bar to hide and none to reserve room for, so the option
    // means nothing there - the same scoping applyTitlebarColor() gives
    // transparentTitlebar one screen up.
    static bool autoHidingChrome(const Composer& composer) {
        return composer.opts->autoHideChrome && !composer.opts->noDecorations;
    }
}

// The whole of A7's hover decision, and deliberately not a line inside
// csdTabsChromeHovered(): there it sat below the nativeWindow() == nil
// exit, where no headless test can reach it, and inverting it - the
// pointer revealing the chrome it should hide and hiding the chrome it
// should reveal - was green across the entire suite (R4-test, N13).
// Above the cast it is an ordinary function of the options and the
// pointer, and the assignment left behind has no branch in it at all.
double csdTabsChromeAlpha(const Composer& composer, bool inside) {
    // Off means fully visible: a window whose title bar nobody hides
    // must not be dimmed by a pointer wandering over it.
    return !autoHidingChrome(composer) || inside ? 1.0 : 0.0;
}

// Where the tab bar lives is the option's answer and nothing else's
// (V3). The strip shows in the title bar when that is the placement the
// user picked; the sidebar shows in the other case, and cmd+b hides the
// sidebar without the tabs reappearing up here - which is exactly what
// this used to get wrong.
//
// It used to read the sidebar's left-edge chrome reserve, so that cmd+b
// hiding the panel brought the strip back. That was the "toggle" the
// user complained about: the chord moved the tabs from one edge to the
// other instead of putting them away. Reading the option instead is not
// only the right answer, it is one fewer cross-module channel and one
// fewer question about which module's listener ran first.
//
// Not static, and declared again in ui_csd_tabs_ut.cpp, for the same
// reason csdTabsChromeAlpha() is: below the nativeWindow() == nil exit
// no headless test could reach the decision at all, and that is exactly
// how an inverted one stayed green through a whole suite before (N13).
bool csdTabsStripShown(const Composer& composer, bool haveTabs) {
    return haveTabs && !composer.opts->sidebarTabs;
}

void csdTabsChromeHovered(Composer& composer, bool inside) {
    // A7, and the reason this function is three lines long: a hover
    // changes what is drawn in the strip and nothing else. The reserve
    // stays, the content view keeps its size, the grid keeps its rows,
    // and the shell is never told anything happened. Dropping the
    // reserve here instead - the obvious way to make the terminal use
    // the strip while the chrome is away - is what would send a
    // SIGWINCH on every crossing of the boundary and make Vterm rebuild
    // Screen with a scrollback reflow, twice per pass of the pointer.
    if (composer.vtConfig.config->verbose && autoHidingChrome(composer)) {
        // The row count travels with the line on purpose: this is the
        // trace that shows a pass of the pointer moving nothing. A
        // window: line from application.cpp between two of these would
        // be a re-counted grid, which is the failure A7 names.
        fprintf(stderr, "%s: chrome: pointer %s the strip, grid %ux%u\n", composer.brand->identifierCString(), inside ? "entered" : "left", (unsigned)(composer.geometry.columns), (unsigned)(composer.geometry.rows));
    }
    NSWindow* const window = nativeWindow(composer);
    NSView* const titlebar = window != nil ? titlebarContainer(window) : nil;
    if (titlebar == nil) {
        return;
    }
    // Alpha, not -setHidden: - a hidden title bar container is a piece
    // of window state AppKit re-derives on its own (a fullscreen
    // transition, a style mask change), and it takes the standard
    // buttons' own hidden flags with it. Alpha is ours alone and
    // survives all of that. It leaves the chrome hit-testable while
    // invisible, which costs nothing: a click in the strip is preceded
    // by the pointer entering it, and that is what makes it visible.
    //
    // Except in the layered window, where the standard buttons sit on the
    // sidebar's surface for good, the way the mock drew them: there is no
    // strip over the terminal to hide any more (applyTitlebarColor() stands
    // it down), so hiding the container would only take the buttons away.
    titlebar.alphaValue = layeredWindowShown(composer, window) ? 1.0 : csdTabsChromeAlpha(composer, inside);
}

CsdTabsUi::CsdTabsUi(Composer& composer_)
    : composer(composer_)
{
    composer.sessionsChangedListeners.pushBack(&sessionsChanged);
    composer.configChangedListeners.pushBack(&configChanged);
    // The window already exists by the time application.cpp constructs
    // this object (createCsdTabsUi runs right after createWindow), so the
    // initial color applies here instead of waiting for the first reload.
    applyTitlebarColor();
    // Before showWindow() counts the grid for the first time, so the
    // strip is reserved out of the very first row count rather than
    // taken away from it one frame later.
    applyAutoHideChrome();
}

void CsdTabsUi::applyAutoHideChrome() {
    const bool on = autoHidingChrome(composer);
    // C11: nothing is reserved, in either direction. A7 charged the grid
    // a title bar's height so no text could sit under the chrome, and
    // the band of empty window that left above the first row - the
    // terminal's and the sidebar list's alike, since that list insets
    // itself by this same reserve - is what the user reported as looking
    // wrong. The point of hiding the title bar is a window with nothing
    // above the text; on the frames the pointer brings it back it is
    // drawn over the top rows, which is the trade the user asked for.
    // Still set rather than skipped: a reload that turns the mode off
    // has to clear whatever an older build left behind.
    composer.setChromeReserve(ChromeSide::Top, 0);
    NSWindow* const window = nativeWindow(composer);
    if (window == nil) {
        return;
    }
    if (on) {
        // The content view takes the whole window, title bar included,
        // and the title bar stops painting its own material over it.
        // This is what makes hiding the chrome show the terminal's
        // background there instead of a bare grey strip - and it is the
        // one geometry change in the whole feature, made once when the
        // mode turns on, never on a hover.
        window.styleMask |= NSWindowStyleMaskFullSizeContentView;
        window.titlebarAppearsTransparent = YES;
    } else {
        window.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
        // transparentTitlebar is the other owner of this bit and may
        // still want it; clearing it unconditionally would turn one
        // option off by turning another one off.
        window.titlebarAppearsTransparent = composer.opts->transparentTitlebar;
    }
    NSView* const titlebar = titlebarContainer(window);
    if (titlebar == nil) {
        return;
    }
    if (on && chromeHover == nil) {
        chromeHover = [[TerminalChromeHoverView alloc] initWithFrame:titlebar.bounds];
        chromeHover.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        chromeHover->composer = &composer;
        [titlebar addSubview:chromeHover];
        if (composer.vtConfig.config->verbose) {
            fprintf(stderr, "%s: chrome: auto-hiding title bar, %u pt reserved for good\n", composer.brand->identifierCString(), (unsigned)(composer.chromeReserve(ChromeSide::Top)));
        }
    } else if (!on && chromeHover != nil) {
        [chromeHover removeFromSuperview];
        [chromeHover release];
        chromeHover = nil;
    }
    // Hidden until the pointer says otherwise, and fully visible again
    // the moment the mode goes off - a reload that drops the option
    // must not leave the title bar it stops managing at alpha zero.
    csdTabsChromeHovered(composer, false);
}

void CsdTabsUi::project() {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || composer.window == nullptr) {
        return;
    }
    const size_t count = sessions->count();
    NSMutableArray<NSString*>* next = nil;
    if (count >= 2) {
        next = [NSMutableArray arrayWithCapacity:(NSUInteger)(count)];
        for (size_t at = 0; at < count; ++at) {
            // A tab whose shell never set a title shows the brand name,
            // like a fresh window does.
            StringView title = sessions->title(at);
            if (title.length() == 0) {
                title = composer.brand->displayName();
            }
            Buffer label(title);
            NSString* const text = [NSString stringWithUTF8String:label.cStr()];
            [next addObject:text == nil ? @"" : text];
        }
    }
    [next retain];
    [labels release];
    labels = next;
    active = sessions->activeIndex();
    if (applyPending) {
        return;
    }
    applyPending = true;
    dispatch_async(dispatch_get_main_queue(), ^{
      apply();
    });
}

void CsdTabsUi::apply() {
    applyPending = false;
    NSWindow* const window = nativeWindow(composer);
    if (window == nil) {
        if (composer.vtConfig.config->verbose) {
            fprintf(stderr, "%s: tabs: no native window in the render context\n", composer.brand->identifierCString());
        }
        return;
    }
    if (!csdTabsStripShown(composer, labels != nil)) {
        if (bar != nil) {
            [bar removeFromSuperview];
            [bar release];
            bar = nil;
        }
        // The layered window's panel says the title itself, centred over
        // the terminal; the system's copy would sit across the top of the
        // whole window, over the sidebar and the panel's edge at once.
        const bool layered = layeredWindowShown(composer, window);
        window.titleVisibility = layered ? NSWindowTitleHidden : NSWindowTitleVisible;
        if (@available(macOS 11.0, *)) {
            window.titlebarSeparatorStyle = layered ? NSTitlebarSeparatorStyleNone : NSTitlebarSeparatorStyleAutomatic;
        }
        return;
    }
    NSButton* const zoom = [window standardWindowButton:NSWindowZoomButton];
    NSView* const titlebar = zoom != nil ? zoom.superview : nil;
    if (titlebar == nil) {
        if (composer.vtConfig.config->verbose) {
            fprintf(stderr, "%s: tabs: no titlebar container to draw into\n", composer.brand->identifierCString());
        }
        return;
    }
    // Up to the very window edge: the trailing new-tab cell is never
    // filled, so nothing opaque reaches the rounded corner. The gap
    // before the first tab is bare title bar outside this view, so it
    // drags the window natively, double-click zoom included.
    const CGFloat left = NSMaxX(zoom.frame) + 56;
    const NSRect frame = NSMakeRect(left, 0, titlebar.bounds.size.width - left, titlebar.bounds.size.height);
    if (bar == nil) {
        bar = [[TerminalTabBarView alloc] initWithFrame:frame];
        bar.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        bar->owner = this;
        [titlebar addSubview:bar];
        window.titleVisibility = NSWindowTitleHidden;
        // The automatic style draws a hard rule under the title bar on
        // some releases - straight through the seam where the active tab
        // continues into its terminal.
        if (@available(macOS 11.0, *)) {
            window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        }
        if (composer.vtConfig.config->verbose) {
            fprintf(stderr, "%s: tabs: strip installed over the title bar\n", composer.brand->identifierCString());
        }
    } else {
        bar.frame = frame;
    }
    bar.needsDisplay = YES;
}

void CsdTabsUi::applyTitlebarColor() {
    NSWindow* const window = nativeWindow(composer);
    if (window == nil) {
        return;
    }
    // no-decorations makes the option meaningless (there is no title bar
    // to blend): leave the window's default background alone rather than
    // paint a borderless window's whole frame.
    if (!composer.opts->transparentTitlebar || composer.opts->noDecorations) {
        // A reload can turn the option off. The tint has to go with it,
        // or the strip keeps wearing opts->bg with nothing left to
        // justify it - the same stale-state trap the arbitration below
        // is built to avoid (R2-qa round 2, Z3).
        if (titlebarFill != nil) {
            dropTitlebarGlass();
            [titlebarFill removeFromSuperview];
            [titlebarFill release];
            titlebarFill = nil;
        }
        return;
    }
    // T10: at the terminal's own opacity, or the strip stays a solid
    // step above a body the desktop shows through - the two are one
    // surface to look at and have to fade together. At the default this
    // is 1.0 and the colour is the one that was here before.
    NSColor* const tint = nsColorFromTerminalColor(composer.vtConfig.config->bg, windowTintAlpha(composer, window));
    // The tint belongs to the title bar *strip*, not to the window.
    // window.backgroundColor is the whole frame, which is why it could
    // never have two owners: a quick window that rounds its corners
    // needs that background transparent, or the corners
    // WindowImpl::requestCornerRadius() (platform_cocoa.mm) rounds show
    // a solid opts->bg rectangle instead of the desktop behind them -
    // the "square ears" the option exists to avoid (F2's report, I7).
    // A fill view sitting behind the standard buttons in the titlebar
    // container - exactly the surface AppKit stops painting once
    // titlebarAppearsTransparent is on, and nothing beyond it - carries
    // the tint instead, so the two options finally combine: rounded
    // corners with the desktop showing through them, and a title bar in
    // the terminal's own background color.
    NSView* const titlebar = titlebarContainer(window);
    if (titlebar != nil) {
        if (titlebarFill == nil) {
            titlebarFill = [[TerminalTitlebarFillView alloc] initWithFrame:titlebar.bounds];
            titlebarFill.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            titlebarFill.wantsLayer = YES;
            // Behind everything the title bar already holds: the
            // standard buttons, and the tab strip apply() adds.
            [titlebar addSubview:titlebarFill positioned:NSWindowBelow relativeTo:nil];
        }
        // T4. The strip's colour, and the one line of this method a colour
        // change can reach.
        //
        // With the system's glass behind the window the flat tint is what
        // has to go: it is opts->bg at the terminal's own alpha, and at the
        // opacities this option is worn with it covers most of what the
        // glass would have shown - a solid band across the top of a window
        // whose body refracts the desktop. A sheet of glass in its place
        // continues the body instead of interrupting it.
        //
        // Inside the fill view rather than beside it: TerminalTitlebarFillView
        // answers nil to hitTest:, which takes its whole subtree out of hit
        // testing, and that is what keeps the bare title bar dragging the
        // window and zooming on a double click.
        //
        // The layered window paints no strip at all: the surface and the
        // terminal panel are both below the title bar, the panel carries a
        // title bar of its own, and a band across the whole width would cut
        // through both.
        if (layeredWindowShown(composer, window)) {
            dropTitlebarGlass();
            titlebarFill.layer.backgroundColor = NSColor.clearColor.CGColor;
        } else if (applyTitlebarGlass(window)) {
            titlebarFill.layer.backgroundColor = NSColor.clearColor.CGColor;
        } else {
            titlebarFill.layer.backgroundColor = tint.CGColor;
        }
    }
    // Behind the content view the same tint still beats the default
    // window gray wherever the renderer has not painted yet (a live
    // resize outruns it) - but only while nobody has made this window
    // transparent for its rounded corners. That is asked of the live
    // AppKit state rather than re-derived from opts, deliberately:
    // requestCornerRadius() sets window.opaque = NO in the same call
    // that rounds the layer, so both halves of the decision are read
    // off one moment in time. Deriving it from quickCornerRadius
    // instead read a fresh config against a layer radius that no
    // reload re-applies, and a SIGUSR1 that dropped the radius to zero
    // brought the square ears back on a still-rounded window (R2-qa
    // round 2, Z3). Order between the two writers does not matter:
    // whichever runs first, this one stops writing the moment the
    // window goes transparent, and never writes over clearColor.
    if (window.opaque) {
        window.backgroundColor = tint;
    }
    // Every color drawRect: mixes in this mode - the accent bar and the
    // idle text/hairline blends alike - reads opts->bg or opts->fg
    // straight from composer.opts, so a reload that only changes colors
    // still needs a repaint even though the tab model itself (project())
    // never fired. Only reachable here, under transparentTitlebar: the
    // plain title bar's active fill and system label colors already
    // track window state AppKit repaints on its own.
    bar.needsDisplay = YES;
}

// True when the strip now carries glass and its own tint must stand down.
bool CsdTabsUi::applyTitlebarGlass(NSWindow* window) {
#if UI_SDK_MACOS_26
    if (@available(macOS 26.0, *)) {
        if (titlebarFill != nil && windowBackdropIsGlass(window)) {
            if (titlebarGlass == nil) {
                NSGlassEffectView* sheet = nil;
#if UI_SDK_MACOS_27
                if (@available(macOS 27.0, *)) {
                    sheet = [[TerminalGlassStripView alloc] initWithFrame:titlebarFill.bounds];
                }
#endif
                if (sheet == nil) {
                    sheet = [[NSGlassEffectView alloc] initWithFrame:titlebarFill.bounds];
                }
                // Clear, where the window's own backdrop is Regular, and
                // the difference is the whole reason a second sheet is worth
                // having. Regular frosts what is behind it; the backdrop has
                // already done that, and frosting a frosted pane again only
                // takes the colour out of it - measured on a probe, the
                // strip came out 21 units darker than the body it belongs to
                // against 7 for Clear, and looked it. Clear is the style
                // meant for glass laid over something already composed,
                // which after the backdrop is exactly what this is.
                sheet.style = NSGlassEffectViewStyleClear;
                // Square: the strip runs the full width of the window and
                // meets the frame's own rounded corners, which are already
                // rounded by the window. On macOS 27 the concentric
                // configuration on TerminalGlassStripView outranks this and
                // resolves to the same zero, measured; the line stays because
                // it is the whole statement on 26.
                sheet.cornerRadius = 0;
#if UI_SDK_MACOS_27
                // The strip is the background of the tab buttons, which is
                // the case the header names for this flag. Measured on a
                // probe: no difference at all at rest or under the pointer,
                // a visible lift under a press, and the lift only where hit
                // testing gives the glass the event. It does not here -
                // TerminalTitlebarFillView answers nil to hitTest: and takes
                // its whole subtree, this sheet included, out of hit testing
                // so the title bar stays draggable. Set for the same reason
                // as on the pill: it is what this view is, and the cost at
                // rest is measured at zero.
                if (@available(macOS 27.0, *)) {
                    sheet.effectIsInteractive = YES;
                }
#endif
                // A glass view ships this set NO, and a view in that state
                // takes its frame from constraints nobody here writes - the
                // same thing T3 measured on the backdrop.
                sheet.translatesAutoresizingMaskIntoConstraints = YES;
                sheet.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
                titlebarGlass = sheet;
                [titlebarFill addSubview:sheet];
                if (composer.vtConfig.config->verbose) {
                    // Both ends printed, not one: the strip's corners differ
                    // from each other by design from macOS 27 on, and a single
                    // number would hide exactly the half that has to stay
                    // square. Zero and zero on macOS 26, where nothing
                    // resolves anything and cornerRadius is the whole story.
                    double top = 0;
                    double bottom = 0;
#if UI_SDK_MACOS_27
                    if (@available(macOS 27.0, *)) {
                        NSViewCornerRadii* const radii = sheet.effectiveCornerRadii;
                        top = radii == nil ? 0 : (double)(radii.topLeft);
                        bottom = radii == nil ? 0 : (double)(radii.bottomLeft);
                    }
#endif
                    fprintf(stderr, "%s: tabs: glass under the title bar strip, corner radius %.1f top, %.1f bottom\n", composer.brand->identifierCString(), top, bottom);
                }
            }
            return true;
        }
    }
#endif
    (void)window;
    dropTitlebarGlass();
    return false;
}

void CsdTabsUi::dropTitlebarGlass() {
    if (titlebarGlass != nil) {
        [titlebarGlass removeFromSuperview];
        [titlebarGlass release];
        titlebarGlass = nil;
    }
}

void CsdTabsUi::tabSelected(size_t index) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || index >= sessions->count()) {
        return;
    }
    sessions->activate(index);
    composer.window->requestFrame();
}

void CsdTabsUi::tabClosed(size_t index) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || index >= sessions->count()) {
        return;
    }
    if (sessions->close(index)) {
        composer.window->requestFrame();
    } else {
        // The strip only shows with two or more tabs, so this is
        // unreachable in practice; the chord path's semantics anyway.
        composer.window->requestClose();
    }
}

void CsdTabsUi::tabOpened() {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr) {
        return;
    }
    sessions->newSession();
    composer.window->requestFrame();
}

// The trailing new-tab cell is square-ish; everything left of it is
// split evenly between the tabs. The close glyph answers clicks in a
// fixed leading zone of each tab.
static const CGFloat tabPlusWidth = 34;
static const CGFloat tabCloseZone = 24;

@implementation TerminalTitlebarFillView

// Invisible to the event system, not merely click-through: returning nil
// keeps every title bar gesture the bare strip offers - drag, double
// click to zoom, the standard buttons' own tracking - reaching whatever
// sits above and around this fill, exactly as if it were not there.
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

@end

@implementation TerminalChromeHoverView

// Same nil as TerminalTitlebarFillView's, for a stronger reason: this
// one covers the standard window buttons and the tab strip, and taking
// their clicks would make the revealed chrome useless. A tracking area
// is geometric - AppKit delivers entered/exited from the pointer's
// position, not from hit testing - so being invisible to the event
// system costs this view nothing it needs.
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.trackingAreas.count != 0) {
        return;
    }
    // NSTrackingActiveAlways, unlike the content view's own area
    // (NSTrackingActiveInKeyWindow, platform_cocoa.mm): that one feeds
    // the terminal's pointer reporting, which has no business firing
    // for a window the user is not in, while this one has to work
    // precisely there. Reaching an inactive window's close button means
    // hovering it before clicking it, and with InKeyWindow the button
    // would still be invisible at that moment - the chrome would only
    // appear after a click that landed on nothing. AppKit's own
    // traffic lights light up on hover in inactive windows for the same
    // reason.
    //
    // NSTrackingInVisibleRect keeps the area in step with the strip as
    // the window is resized (the rect argument is then ignored), so
    // there is no geometry to recompute here and none to get wrong.
    NSTrackingArea* const area = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:area];
    [area release];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    csdTabsChromeHovered(*composer, true);
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    csdTabsChromeHovered(*composer, false);
}

@end

@implementation TerminalTabBarView

- (BOOL)mouseDownCanMoveWindow {
    return NO;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSArray<NSString*>* const labels = owner->labels;
    const NSUInteger count = labels.count;
    if (count == 0) {
        return;
    }
    const NSUInteger active = (NSUInteger)(owner->active);
    const NSRect bounds = self.bounds;
    const CGFloat tabsWidth = bounds.size.width - tabPlusWidth;
    const CGFloat cellWidth = tabsWidth / (CGFloat)(count);
    // With an opaque title bar, the active tab is a piece of the terminal
    // it fronts: its cell wears the terminal's background and foreground,
    // and idle tabs stay bare so the title bar's own material shows
    // through. transparentTitlebar paints that same background onto the
    // title bar itself (WindowImpl's titlebarAppearsTransparent plus
    // CsdTabsUi::applyTitlebarColor), so the fill that used to set the
    // active tab apart would now match its surroundings and vanish; the
    // accent bar below stands in for it instead.
    const bool transparentTitlebar = owner->composer.opts->transparentTitlebar;
    NSColor* const activeFill = nsColorFromTerminalColor(owner->composer.vtConfig.config->bg);
    NSColor* const activeText = nsColorFromTerminalColor(owner->composer.vtConfig.config->fg);
    NSColor* const activeGlyphs = [activeText colorWithAlphaComponent:0.75];
    // Only the transparentTitlebar branch below ever fills with this.
    NSColor* const activeAccent = transparentTitlebar ? nsColorFromTerminalColor(owner->composer.vtConfig.config->cr) : nil;
    // The strip is our own surface, and the system label tiers are tuned
    // for controls on the standard material: tertiary label over a dark
    // title bar measures 1.16:1 against it (issue 84), which is nothing.
    // Every idle tier moves one step up, and the hairlines are mixed from
    // the label color so they keep following the appearance.
    //
    // That fix assumed the title bar's own material stays in step with
    // NSAppearance, which transparentTitlebar breaks: the title bar now
    // wears opts->bg, a color the window's appearance (and so the system
    // label tiers) knows nothing about. A user on a light system theme
    // with the project's own default dark bg measured out at 1.60:1 -
    // effectively invisible, and the hairlines marking tab boundaries
    // disappeared with it, since they are mixed from the same tier.
    // Flipping window.appearance by opts->bg's brightness would drag the
    // tiers back in step, but they would still be blind to opts->bg's
    // actual color, same as the pre-fix issue 84 case; every idle color
    // here is mixed straight from opts->fg and opts->bg instead, so idle
    // tabs are legible by construction against the exact bg painted
    // above, independent of the system theme entirely - not just brought
    // back in step with it. The active tab keeps the full fg strength;
    // idle tabs sit further toward bg so the active one reads as the
    // brighter of the two without a fill to say so anymore, and the
    // hairline sits furthest toward bg of the three, same low-emphasis
    // role the alpha-faded label color played before.
    NSColor* const idleText = transparentTitlebar ? [activeText blendedColorWithFraction:0.4 ofColor:activeFill] : NSColor.labelColor;
    NSColor* const idleGlyphs = transparentTitlebar ? [activeText blendedColorWithFraction:0.55 ofColor:activeFill] : NSColor.secondaryLabelColor;
    NSColor* const hairline = transparentTitlebar ? [activeText blendedColorWithFraction:0.68 ofColor:activeFill] : [NSColor.labelColor colorWithAlphaComponent:0.4];
    NSMutableParagraphStyle* const centered = [[[NSMutableParagraphStyle alloc] init] autorelease];
    centered.alignment = NSTextAlignmentCenter;
    // Long shell titles differ at the tail; keep it, iTerm style.
    centered.lineBreakMode = NSLineBreakByTruncatingHead;
    NSFont* const activeFont = [NSFont titleBarFontOfSize:0];
    NSFont* const idleFont = [NSFont systemFontOfSize:activeFont.pointSize];
    NSDictionary* const activeAttributes = @{
        NSFontAttributeName : activeFont,
        NSForegroundColorAttributeName : activeText,
        NSParagraphStyleAttributeName : centered,
    };
    NSDictionary* const idleAttributes = @{
        NSFontAttributeName : idleFont,
        NSForegroundColorAttributeName : idleText,
        NSParagraphStyleAttributeName : centered,
    };
    NSDictionary* const activeGlyphAttributes = @{
        NSFontAttributeName : activeFont,
        NSForegroundColorAttributeName : activeGlyphs,
    };
    NSDictionary* const idleGlyphAttributes = @{
        NSFontAttributeName : idleFont,
        NSForegroundColorAttributeName : idleGlyphs,
    };
    const auto drawGlyph = [&](NSString* glyph, CGFloat x, NSDictionary* attributes) {
        const NSSize size = [glyph sizeWithAttributes:attributes];
        [glyph drawAtPoint:NSMakePoint(x, bounds.origin.y + (bounds.size.height - size.height) / 2) withAttributes:attributes];
    };
    for (NSUInteger at = 0; at < count; ++at) {
        const NSRect cell = NSMakeRect(bounds.origin.x + cellWidth * (CGFloat)(at), bounds.origin.y, cellWidth, bounds.size.height);
        if (at == active) {
            if (transparentTitlebar) {
                // A solid opts->bg fill would be invisible against the
                // now-matching title bar behind it, so the active tab is
                // marked with a cursor-colored bar along its bottom edge
                // instead. cr is guaranteed distinct from bg - the cursor
                // itself would be invisible in the grid otherwise - but
                // not from fg: options.cpp defaults cr to fg when unset,
                // in which case the bar simply matches the active text,
                // which is harmless here. Either way it reads the same
                // way the terminal's own cursor does: "the active thing
                // is here".
                [activeAccent setFill];
                NSRectFill(NSMakeRect(cell.origin.x, cell.origin.y, cell.size.width, 2));
            } else {
                // Everything but the top point: that row is where the
                // window's own frame edge lives, and an opaque fill over
                // it darkens the border itself rather than the title bar.
                [activeFill setFill];
                NSRectFill(NSMakeRect(cell.origin.x, cell.origin.y, cell.size.width, cell.size.height - 1));
            }
        }
        // Hairlines separate bare cells only; the active tab draws its
        // own edge - a solid fill in the plain title bar, the accent
        // bar's own left/right extent under transparentTitlebar (see
        // above). A hairline touching either would double up on that
        // edge and read as a wall boxing the active tab in, which is
        // exactly what the accent bar alone is meant to avoid. The
        // leftmost tab has the drag gap to its left, and that seam wants
        // the same line unless the tab itself is the marked one.
        if (at != active && (at == 0 || at - 1 != active)) {
            [hairline setFill];
            NSRectFillUsingOperation(NSMakeRect(cell.origin.x, cell.origin.y + 4, 1, cell.size.height - 8), NSCompositingOperationSourceOver);
        }
        NSDictionary* const glyphAttributes = at == active ? activeGlyphAttributes : idleGlyphAttributes;
        drawGlyph(@"\u00d7", cell.origin.x + 9, glyphAttributes);
        CGFloat trailing = 8;
        if (at < 9) {
            NSString* const hint = [NSString stringWithFormat:@"\u2318%u", (unsigned)(at + 1)];
            const NSSize hintSize = [hint sizeWithAttributes:glyphAttributes];
            trailing += hintSize.width + 8;
            drawGlyph(hint, NSMaxX(cell) - 8 - hintSize.width, glyphAttributes);
        }
        NSDictionary* const attributes = at == active ? activeAttributes : idleAttributes;
        NSString* const label = labels[at];
        const NSSize size = [label sizeWithAttributes:attributes];
        const CGFloat leading = tabCloseZone;
        const CGFloat available = cell.size.width - leading - trailing;
        if (available <= 0) {
            continue;
        }
        const NSRect text = NSMakeRect(cell.origin.x + leading, cell.origin.y + (cell.size.height - size.height) / 2, available, size.height);
        [label drawWithRect:text options:NSStringDrawingUsesLineFragmentOrigin attributes:attributes context:nil];
    }
    // The trailing new-tab cell: bare material, a plus, and a hairline
    // against the last tab unless the active tab already draws that edge
    // itself - same reasoning as the loop above.
    if (count - 1 != active) {
        [hairline setFill];
        NSRectFillUsingOperation(NSMakeRect(bounds.origin.x + tabsWidth, bounds.origin.y + 4, 1, bounds.size.height - 8), NSCompositingOperationSourceOver);
    }
    NSString* const plus = @"+";
    const NSSize plusSize = [plus sizeWithAttributes:idleGlyphAttributes];
    drawGlyph(plus, bounds.origin.x + tabsWidth + (tabPlusWidth - plusSize.width) / 2, idleGlyphAttributes);
}

- (void)mouseDown:(NSEvent*)event {
    const NSUInteger count = owner->labels.count;
    if (count == 0) {
        return;
    }
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const NSRect bounds = self.bounds;
    const CGFloat tabsWidth = bounds.size.width - tabPlusWidth;
    if (point.x >= bounds.origin.x + tabsWidth) {
        owner->tabOpened();
        return;
    }
    const CGFloat cellWidth = tabsWidth / (CGFloat)(count);
    NSUInteger index = (NSUInteger)((point.x - bounds.origin.x) / cellWidth);
    if (index >= count) {
        index = count - 1;
    }
    if (point.x - bounds.origin.x - cellWidth * (CGFloat)(index) < tabCloseZone) {
        owner->tabClosed((size_t)(index));
        return;
    }
    owner->tabSelected((size_t)(index));
}

@end

void createCsdTabsUi(ObjPool& owner, Composer& composer) {
    owner.make<CsdTabsUi>(composer);
}
