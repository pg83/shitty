/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_palette.h"

#include "composer.h"
#include "options.h"
#include "palette_session.h"

#include <lib/vterm/listener.h>

#include <plt/window.h>

#include <std/alg/minmax.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

using namespace stl;

namespace {
    struct PaletteUi;
}

// The palette's field: an ordinary text field, first responder while the
// palette is up. Its chords - Cmd+Return, Cmd+B, Cmd+K and the editing
// ones, for want of an Edit menu - are answered here.
@interface TerminalPaletteField: NSTextField {
    @public
    PaletteUi* owner;
}
@end

// The panel: the field as a subview, and everything else drawn.
@interface TerminalPaletteView: NSView <NSTextFieldDelegate> {
    @public
    PaletteUi* owner;
}
@end

namespace {
    NSString* text(StringView view) {
        NSString* const made = [[[NSString alloc] initWithBytes:view.data() length:view.length() encoding:NSUTF8StringEncoding] autorelease];
        return made != nil ? made : @"";
    }

    NSColor* terminalColor(Color color, CGFloat alpha = 1.0) {
        return [NSColor colorWithSRGBRed:color.red / 255.0 green:color.green / 255.0 blue:color.blue / 255.0 alpha:alpha];
    }

    NSColor* mixed(Color from, Color to, CGFloat amount, CGFloat alpha = 1.0) {
        return [NSColor colorWithSRGBRed:(from.red + (to.red - from.red) * amount) / 255.0 green:(from.green + (to.green - from.green) * amount) / 255.0 blue:(from.blue + (to.blue - from.blue) * amount) / 255.0 alpha:alpha];
    }

    NSColor* matchColor() {
        return [NSColor colorWithSRGBRed:0xf0 / 255.0 green:0xc6 / 255.0 blue:0x74 / 255.0 alpha:1];
    }

    void drawSymbol(NSString* name, NSRect box, NSColor* color) {
        NSImage* const symbol = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
        if (symbol == nil || !(symbol.size.width > 0) || !(symbol.size.height > 0)) {
            return;
        }
        const CGFloat scale = min(box.size.width / symbol.size.width, box.size.height / symbol.size.height);
        const NSSize size = NSMakeSize(symbol.size.width * scale, symbol.size.height * scale);
        const NSRect fitted = NSMakeRect(NSMidX(box) - size.width / 2, NSMidY(box) - size.height / 2, size.width, size.height);
        NSImage* const tinted = [NSImage imageWithSize:size flipped:NO drawingHandler:^BOOL(NSRect rect) {
            [symbol drawInRect:rect];
            [color set];
            NSRectFillUsingOperation(rect, NSCompositingOperationSourceAtop);
            return YES;
        }];
        [tinted drawInRect:fitted fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
    }

    NSString* kindSymbol(const PaletteItem& item) {
        switch (item.kind) {
            case PaletteKind::Bookmark:
                return item.command.empty() ? @"folder" : @"terminal";
            case PaletteKind::SshHost:
                return @"server.rack";
            case PaletteKind::TeleportHost:
                return @"shield.lefthalf.filled";
            case PaletteKind::Folder:
                return @"folder";
            case PaletteKind::App:
                return @"square.grid.2x2";
            case PaletteKind::Env:
                return @"key";
            case PaletteKind::Action:
                return item.action == PaletteAction::CloneUrl || item.action == PaletteAction::Clone ? @"arrow.triangle.branch" : @"bolt";
        }
        return @"bolt";
    }

    struct CallPalette final: public Listener {
        explicit CallPalette(PaletteUi* owner_)
            : owner(owner_)
        {
        }
        void onListen(void*) override;
        PaletteUi* owner;
    };

    struct PaletteUi final: public PaletteHost {
        explicit PaletteUi(Composer& composer_);

        void paletteChanged() override;
        void toggle();
        void open();
        void close();
        void pick(PaletteTarget target);
        void fieldEdited();
        NSWindow* nativeWindow() const;

        Composer& composer;
        PaletteSession session{composer, *this};
        CallPalette call{this};
        TerminalPaletteView* view = nil;
        TerminalPaletteField* field = nil;
        NSResponder* previous = nil;
        CGFloat scroll = 0;
        bool closing = false;
    };

    void CallPalette::onListen(void*) {
        owner->toggle();
    }

    PaletteUi::PaletteUi(Composer& composer_)
        : composer(composer_)
    {
        composer.commandPaletteListeners.pushBack(&call);
    }

    NSWindow* PaletteUi::nativeWindow() const {
        if (composer.window == nullptr) {
            return nil;
        }
        const plt::RenderContext context = composer.window->renderContext();
        if (context.backend != plt::RenderBackend::Cocoa) {
            return nil;
        }
        return (__bridge NSWindow*)(context.window);
    }

    void PaletteUi::toggle() {
        if (session.shown()) {
            close();
        } else {
            open();
        }
    }

    void PaletteUi::open() {
        NSWindow* const window = nativeWindow();
        NSView* const content = window != nil ? window.contentView : nil;
        NSView* const frameView = content != nil ? content.superview : nil;
        if (frameView == nil) {
            return;
        }
        // Centred over the terminal: the panel's rectangle when there is
        // one, the content otherwise, in the frame view's coordinates.
        const PixelRect panelPixels = composer.panelRect();
        const CGFloat pixelScale = composer.contentScale > 0 ? (CGFloat)(composer.contentScale) : 1.0;
        NSRect panel = content.bounds;
        if (panelPixels.width > 0 && panelPixels.height > 0) {
            const CGFloat below = (CGFloat)(composer.geometry.pixelHeight) - (CGFloat)(panelPixels.y) - (CGFloat)(panelPixels.height);
            panel = NSMakeRect((CGFloat)(panelPixels.x) / pixelScale, below / pixelScale, (CGFloat)(panelPixels.width) / pixelScale, (CGFloat)(panelPixels.height) / pixelScale);
        }
        panel = [frameView convertRect:panel fromView:content];
        const CGFloat width = min<CGFloat>(PaletteMetrics::width, max<CGFloat>(320, panel.size.width - 32));
        const CGFloat wanted = PaletteMetrics::input + PaletteMetrics::chips + PaletteMetrics::list + PaletteMetrics::footer;
        const CGFloat height = min<CGFloat>(wanted, max<CGFloat>(200, panel.size.height - PaletteMetrics::top - 16));
        const NSRect frame = NSMakeRect(floor(NSMidX(panel) - width / 2), floor(NSMaxY(panel) - PaletteMetrics::top - height), width, height);
        if (view == nil) {
            view = [[TerminalPaletteView alloc] initWithFrame:frame];
            view->owner = this;
            view.wantsLayer = YES;
            field = [[TerminalPaletteField alloc] initWithFrame:NSZeroRect];
            field->owner = this;
            field.bezeled = NO;
            field.bordered = NO;
            field.drawsBackground = NO;
            field.focusRingType = NSFocusRingTypeNone;
            field.cell.usesSingleLineMode = YES;
            field.cell.scrollable = YES;
            field.font = [NSFont systemFontOfSize:15];
            field.delegate = view;
            [view addSubview:field];
        }
        const Color fg = composer.vtConfig.config->fg;
        const Color bg = composer.vtConfig.config->bg;
        CALayer* const sheet = view.layer;
        sheet.backgroundColor = mixed(bg, fg, 0.07, 0.985).CGColor;
        sheet.cornerRadius = PaletteMetrics::radius;
        sheet.borderWidth = 1;
        sheet.borderColor = terminalColor(fg, 0.14).CGColor;
        sheet.masksToBounds = NO;
        sheet.shadowColor = NSColor.blackColor.CGColor;
        sheet.shadowOpacity = 0.5f;
        sheet.shadowRadius = 24;
        sheet.shadowOffset = CGSizeMake(0, -10);
        field.textColor = terminalColor(fg);
        field.placeholderAttributedString = [[[NSAttributedString alloc] initWithString:@"Host, folder, app, action…   @ / > ! $" attributes:@{NSForegroundColorAttributeName: terminalColor(fg, 0.38), NSFontAttributeName: field.font}] autorelease];
        view.frame = frame;
        view.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin;
        field.frame = NSMakeRect(42, 12, width - 56, 24);
        field.stringValue = @"";
        if (view.superview != frameView) {
            [view removeFromSuperview];
            [frameView addSubview:view positioned:NSWindowAbove relativeTo:content];
        }
        view.hidden = NO;
        scroll = 0;
        [previous release];
        previous = [window.firstResponder retain];
        session.open();
        [window makeFirstResponder:field];
    }

    void PaletteUi::close() {
        if (!session.shown() || closing) {
            return;
        }
        closing = true;
        session.close();
        NSWindow* const window = view.window;
        // The keys back to whatever had them - the terminal.
        if (window != nil) {
            const bool there = previous != nil && (![previous isKindOfClass:[NSView class]] || ((NSView*)(previous)).window == window);
            [window makeFirstResponder:there ? previous : nil];
        }
        [previous release];
        previous = nil;
        view.hidden = YES;
        closing = false;
    }

    void PaletteUi::fieldEdited() {
        NSString* const value = field.stringValue;
        const char* const utf8 = value.UTF8String;
        session.field.begin(StringView(utf8 != nullptr ? utf8 : ""));
        plt::KeyInput end;
        end.key = plt::InputKey::End;
        session.field.key(end);
        session.textChanged();
    }

    void PaletteUi::pick(PaletteTarget target) {
        if (session.pick(target)) {
            close();
            composer.window->requestFrame();
        }
    }

    void PaletteUi::paletteChanged() {
        if (view == nil) {
            return;
        }
        // The session may have written the text itself (Open Folder…,
        // Clone…, Tab): the field follows, caret at the end.
        const StringView now = session.field.text();
        NSString* const shown = text(now);
        if (![field.stringValue isEqualToString:shown]) {
            field.stringValue = shown;
            NSText* const editor = field.currentEditor;
            if (editor != nil) {
                editor.selectedRange = NSMakeRange(shown.length, 0);
            }
        }
        const CGFloat listHeight = view.bounds.size.height - PaletteMetrics::input - PaletteMetrics::chips - PaletteMetrics::footer;
        scroll = (CGFloat)(paletteScrollTo(session.rows(), session.selected(), (float)(scroll), (float)(listHeight)));
        view.needsDisplay = YES;
    }
}

@implementation TerminalPaletteField

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    NSText* const editor = self.currentEditor;
    const NSEventModifierFlags flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (editor == nil || owner == nullptr || event.type != NSEventTypeKeyDown || (flags & NSEventModifierFlagCommand) == 0) {
        return [super performKeyEquivalent:event];
    }
    NSString* const key = event.charactersIgnoringModifiers.lowercaseString;
    if ([key isEqualToString:@"\r"] || [key isEqualToString:@"\x03"]) {
        owner->pick(PaletteTarget::NewTab);
        return YES;
    }
    if ([key isEqualToString:@"b"]) {
        owner->session.bookmarkPicked();
        return YES;
    }
    if ([key isEqualToString:@"k"]) {
        owner->close();
        return YES;
    }
    SEL action = nullptr;
    if ([key isEqualToString:@"a"]) {
        action = @selector(selectAll:);
    } else if ([key isEqualToString:@"c"]) {
        action = @selector(copy:);
    } else if ([key isEqualToString:@"v"]) {
        action = @selector(paste:);
    } else if ([key isEqualToString:@"x"]) {
        action = @selector(cut:);
    } else if ([key isEqualToString:@"z"]) {
        action = (flags & NSEventModifierFlagShift) != 0 ? @selector(redo:) : @selector(undo:);
    }
    if (action == nullptr) {
        return [super performKeyEquivalent:event];
    }
    return [NSApp sendAction:action to:nil from:self];
}

@end

@implementation TerminalPaletteView

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)mouseDownCanMoveWindow {
    return NO;
}

- (void)controlTextDidChange:(NSNotification*)notification {
    (void)notification;
    owner->fieldEdited();
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command {
    (void)control;
    (void)textView;
    const NSEventModifierFlags flags = NSApp.currentEvent.modifierFlags;
    if (command == @selector(moveUp:)) {
        owner->session.move(-1);
        return YES;
    }
    if (command == @selector(moveDown:)) {
        owner->session.move(1);
        return YES;
    }
    if (command == @selector(insertTab:)) {
        owner->session.complete();
        return YES;
    }
    if (command == @selector(cancelOperation:)) {
        owner->close();
        return YES;
    }
    if (command == @selector(insertNewline:) || command == @selector(insertNewlineIgnoringFieldEditor:)) {
        owner->pick((flags & NSEventModifierFlagOption) != 0 ? PaletteTarget::CurrentTab : PaletteTarget::NewTab);
        return YES;
    }
    return NO;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    (void)notification;
    // The keys went elsewhere - a click into the terminal: the palette goes.
    owner->close();
}

- (long long)rowAtPoint:(NSPoint)point {
    const CGFloat top = PaletteMetrics::input + PaletteMetrics::chips;
    const CGFloat listWidth = self.bounds.size.width - (self.bounds.size.width >= 600 ? PaletteMetrics::detail : 0);
    if (point.x < 0 || point.x >= listWidth || point.y < top || point.y >= self.bounds.size.height - PaletteMetrics::footer) {
        return -1;
    }
    return paletteRowAt(owner->session.rows(), (float)(point.y - top), (float)(owner->scroll));
}

- (void)mouseDown:(NSEvent*)event {
    const long long row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row < 0) {
        return;
    }
    owner->session.select((size_t)(row));
    owner->pick(PaletteTarget::NewTab);
}

- (void)updateTrackingAreas {
    for (NSTrackingArea* const area in self.trackingAreas) {
        [self removeTrackingArea:area];
    }
    NSTrackingArea* const area = [[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow owner:self userInfo:nil] autorelease];
    [self addTrackingArea:area];
    [super updateTrackingAreas];
}

- (void)mouseMoved:(NSEvent*)event {
    const long long row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0 && (size_t)(row) != owner->session.selected()) {
        owner->session.select((size_t)(row));
    }
}

- (void)scrollWheel:(NSEvent*)event {
    const Vector<PaletteRow>& rows = owner->session.rows();
    const CGFloat listHeight = self.bounds.size.height - PaletteMetrics::input - PaletteMetrics::chips - PaletteMetrics::footer;
    const CGFloat total = rows.empty() ? 0 : paletteRowTop(rows, rows.length()) + PaletteMetrics::pad;
    owner->scroll = max<CGFloat>(0, min<CGFloat>(owner->scroll - event.scrollingDeltaY, max<CGFloat>(0, total - listHeight)));
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect bounds = self.bounds;
    const Color fgColor = owner->composer.vtConfig.config->fg;
    NSColor* const ink = terminalColor(fgColor);
    NSColor* const dim = terminalColor(fgColor, 0.58);
    NSColor* const faint = terminalColor(fgColor, 0.38);
    NSColor* const rule = terminalColor(fgColor, 0.08);
    const CGFloat w = bounds.size.width;
    const CGFloat h = bounds.size.height;
    NSFont* const titleFont = [NSFont systemFontOfSize:13];
    NSFont* const boldFont = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSFont* const subFont = [NSFont systemFontOfSize:11];
    NSFont* const smallBold = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    NSMutableParagraphStyle* const clip = [[[NSMutableParagraphStyle alloc] init] autorelease];
    clip.lineBreakMode = NSLineBreakByTruncatingTail;
    const auto line = [&](NSString* string, NSFont* font, NSColor* color, NSRect box) {
        [string drawWithRect:box options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color, NSParagraphStyleAttributeName: clip} context:nil];
    };

    drawSymbol(@"magnifyingglass", NSMakeRect(14, 14, 18, 18), dim);
    [rule setFill];
    NSRectFill(NSMakeRect(0, PaletteMetrics::input - 0.5, w, 1));

    // The mode chips.
    const PaletteMode mode = owner->session.mode();
    const PaletteMode modes[] = {PaletteMode::All, PaletteMode::Hosts, PaletteMode::Folders, PaletteMode::Actions, PaletteMode::Apps, PaletteMode::Env};
    CGFloat chipX = 12;
    for (const PaletteMode one : modes) {
        StringBuilder label;
        if (!palettePrefix(one).empty()) {
            label << palettePrefix(one) << StringView(u8" ");
        }
        label << paletteModeName(one);
        NSString* const chip = text(StringView(label));
        const bool on = one == mode;
        NSDictionary* const attributes = @{NSFontAttributeName: subFont, NSForegroundColorAttributeName: on ? matchColor() : dim};
        const CGFloat chipW = [chip sizeWithAttributes:attributes].width + 18;
        const NSRect box = NSMakeRect(chipX, PaletteMetrics::input + 7, chipW, 20);
        [(on ? [matchColor() colorWithAlphaComponent:0.16] : terminalColor(fgColor, 0.06)) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:box xRadius:10 yRadius:10] fill];
        [chip drawAtPoint:NSMakePoint(chipX + 9, PaletteMetrics::input + 9) withAttributes:attributes];
        chipX += chipW + 6;
    }

    // The list.
    const CGFloat listTop = PaletteMetrics::input + PaletteMetrics::chips;
    const CGFloat listWidth = w - (w >= 600 ? PaletteMetrics::detail : 0);
    const CGFloat listBottom = h - PaletteMetrics::footer;
    const Vector<PaletteRow>& rows = owner->session.rows();
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(NSMakeRect(0, listTop, listWidth, listBottom - listTop));
    for (size_t at = 0; at < rows.length(); ++at) {
        const PaletteRow& row = rows[at];
        const CGFloat rowH = row.heading ? PaletteMetrics::heading : PaletteMetrics::row;
        const CGFloat y = listTop + paletteRowTop(rows, at) - owner->scroll;
        if (y + rowH < listTop || y > listBottom) {
            continue;
        }
        if (row.heading) {
            line(text(row.title), smallBold, faint, NSMakeRect(16, y + 7, listWidth - 32, 14));
            continue;
        }
        const PaletteItem* const item = owner->session.item(at);
        if (item == nullptr) {
            continue;
        }
        if (at == owner->session.selected()) {
            [terminalColor(fgColor, 0.12) setFill];
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(6, y, listWidth - 12, rowH) xRadius:8 yRadius:8] fill];
        }
        NSColor* const iconColor = item->kind == PaletteKind::TeleportHost ? [NSColor colorWithSRGBRed:0xa9 / 255.0 green:0xc1 / 255.0 blue:1 alpha:1] : dim;
        drawSymbol(kindSymbol(*item), NSMakeRect(15, y + rowH / 2 - 8, 16, 16), iconColor);
        CGFloat right = listWidth - 14;
        if (!item->badge.empty()) {
            NSString* const badge = text(item->badge);
            NSDictionary* const attributes = @{NSFontAttributeName: subFont, NSForegroundColorAttributeName: dim};
            const CGFloat badgeW = [badge sizeWithAttributes:attributes].width + 12;
            right -= badgeW;
            NSColor* const tone = item->badge == StringView(u8"Teleport") ? [NSColor colorWithSRGBRed:0x7a / 255.0 green:0xa2 / 255.0 blue:0xf7 / 255.0 alpha:0.18] : item->badge == StringView(u8"current") ? [NSColor colorWithSRGBRed:0x7f / 255.0 green:0xe0 / 255.0 blue:0xa8 / 255.0 alpha:0.18] : terminalColor(fgColor, 0.08);
            [tone setFill];
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(right, y + rowH / 2 - 9, badgeW, 18) xRadius:9 yRadius:9] fill];
            [badge drawAtPoint:NSMakePoint(right + 6, y + rowH / 2 - 7) withAttributes:attributes];
            right -= 8;
        }
        // The title, matched letters in their own ink and weight.
        NSMutableAttributedString* const title = [[[NSMutableAttributedString alloc] init] autorelease];
        const StringView whole = item->title;
        size_t start = 0;
        size_t mark = 0;
        while (start < whole.length()) {
            const bool marked = mark < row.markCount && row.marks[mark] == start;
            size_t end = start + 1;
            if (marked) {
                ++mark;
                while (end < whole.length() && (((u8)(whole.data()[end])) & 0xC0) == 0x80) {
                    ++end;
                }
            } else {
                while (end < whole.length() && !(mark < row.markCount && row.marks[mark] == end)) {
                    ++end;
                }
            }
            NSString* const run = text(StringView(whole.data() + start, end - start));
            [title appendAttributedString:[[[NSAttributedString alloc] initWithString:run attributes:@{NSFontAttributeName: marked ? boldFont : titleFont, NSForegroundColorAttributeName: marked ? matchColor() : ink, NSParagraphStyleAttributeName: clip}] autorelease]];
            start = end;
        }
        const CGFloat titleWidth = min<CGFloat>(title.size.width, right - 42);
        [title drawWithRect:NSMakeRect(42, y + 3, titleWidth, 16) options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine context:nil];
        if (item->star && 42 + titleWidth + 16 < right) {
            drawSymbol(@"star.fill", NSMakeRect(42 + titleWidth + 5, y + 5, 10, 10), matchColor());
        }
        line(text(item->subtitle), subFont, dim, NSMakeRect(42, y + 19, right - 42, 14));
    }
    [NSGraphicsContext restoreGraphicsState];

    // An empty list says where its rows come from.
    PaletteHint hint;
    if (owner->session.hint(hint)) {
        NSMutableParagraphStyle* const centre = [[clip mutableCopy] autorelease];
        centre.alignment = NSTextAlignmentCenter;
        const auto centred = [&](StringView string, NSFont* font, NSColor* color, CGFloat top, CGFloat height) {
            [text(string) drawWithRect:NSMakeRect(16, top, listWidth - 32, height) options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color, NSParagraphStyleAttributeName: centre} context:nil];
        };
        CGFloat y = listTop + 40;
        centred(StringView(hint.title), boldFont, ink, y, 18);
        y += 26;
        for (const StringBuilder& one : hint.lines) {
            if (!StringView(one).empty()) {
                centred(StringView(one), subFont, dim, y, 16);
                y += 18;
            }
        }
        if (!hint.example[0].empty()) {
            NSFont* const mono = [NSFont monospacedSystemFontOfSize:11.5 weight:NSFontWeightRegular];
            NSDictionary* const attributes = @{NSFontAttributeName: mono, NSForegroundColorAttributeName: dim};
            CGFloat widest = 0;
            for (const StringView one : hint.example) {
                widest = max<CGFloat>(widest, [text(one) sizeWithAttributes:attributes].width);
            }
            const CGFloat bx = (listWidth - widest) / 2 - 14;
            y += 12;
            [terminalColor(fgColor, 0.06) setFill];
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(bx, y, widest + 28, 3 * 18 + 16) xRadius:8 yRadius:8] fill];
            for (const StringView one : hint.example) {
                [text(one) drawAtPoint:NSMakePoint(bx + 14, y + 9) withAttributes:attributes];
                y += 18;
            }
        }
    }

    // The detail of the picked row.
    if (listWidth < w) {
        [rule setFill];
        NSRectFill(NSMakeRect(listWidth, listTop, 1, listBottom - listTop));
        if (const PaletteItem* const item = owner->session.item(owner->session.selected())) {
            const CGFloat px = listWidth + 16;
            const CGFloat pw = w - px - 14;
            drawSymbol(kindSymbol(*item), NSMakeRect(px, listTop + 14, 24, 24), ink);
            line(text(item->title), boldFont, ink, NSMakeRect(px + 32, listTop + 12, pw - 32, 18));
            line(text(item->badge.empty() ? item->subtitle : item->badge), subFont, dim, NSMakeRect(px + 32, listTop + 30, pw - 32, 14));
            CGFloat y = listTop + 62;
            for (int at = 0; at < 4; ++at) {
                if (item->detailLabels[at].empty()) {
                    continue;
                }
                line(text(item->detailLabels[at]), subFont, dim, NSMakeRect(px, y, 64, 16));
                line(text(item->detailValues[at]), subFont, ink, NSMakeRect(px + 68, y, pw - 68, 16));
                y += 22;
            }
        }
    }

    // The keys.
    const CGFloat fy = h - PaletteMetrics::footer;
    [rule setFill];
    NSRectFill(NSMakeRect(0, fy, w, 1));
    NSString* const keys[][2] = {{@"↵", @"new tab"}, {@"⌥↵", @"this tab"}, {@"⌘B", @"bookmark"}, {@"⇥", @"complete"}, {@"esc", @""}};
    CGFloat hx = 14;
    for (const auto& hint : keys) {
        NSDictionary* const attributes = @{NSFontAttributeName: subFont, NSForegroundColorAttributeName: dim};
        const CGFloat kw = [hint[0] sizeWithAttributes:attributes].width + 10;
        [terminalColor(fgColor, 0.08) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(hx, fy + 8, kw, 16) xRadius:4 yRadius:4] fill];
        [hint[0] drawAtPoint:NSMakePoint(hx + 5, fy + 9) withAttributes:attributes];
        hx += kw + 5;
        if (hint[1].length != 0) {
            [hint[1] drawAtPoint:NSMakePoint(hx, fy + 9) withAttributes:attributes];
            hx += [hint[1] sizeWithAttributes:attributes].width + 12;
        }
    }
}

@end

void createPaletteUi(ObjPool& owner, Composer& composer) {
    owner.make<PaletteUi>(composer);
}
