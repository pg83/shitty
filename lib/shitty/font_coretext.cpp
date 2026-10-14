/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "font_coretext.h"

#if defined(HAVE_CORETEXT)
    #include "composer.h"
    #include "font_face.h"
    #include "font_renderer.h"
    #include "font_resolver.h"
    #include <lib/vterm/grapheme.h>
    #include "options.h"
    #include <lib/vterm/utf8.h>

    #include <std/ios/sys.h>
    #include <std/lib/buffer.h>
    #include <std/mem/obj_pool.h>
    #include <std/str/view.h>
    #include <std/sys/throw.h>

    #include <CoreFoundation/CoreFoundation.h>
    #include <CoreGraphics/CoreGraphics.h>
    #include <CoreText/CoreText.h>

using namespace stl;

namespace {
    struct CoreTextFont final: public Font {
        CoreTextFont(Composer& composer, IntrusivePtr<FontFace> source, CTFontRef font, FontKind kind, FontMetrics metrics, FontStyle synthetic);
        ~CoreTextFont() noexcept;

        void render(const u32* codepoints, size_t count, u16 cells, void* buf) override;
        bool covers(u32 codepoint) override;
        bool colored() const override;
        Font* synthesize(ObjPool& owner, FontStyle style) override;
        FontFace* face() override;

        CFStringRef makeString(const u32* codepoints, size_t count);
        CTLineRef makeLine(CFStringRef string);
        bool inspectLine(CTLineRef line, bool& color);
        bool drawLine(CTLineRef line, bool color);
        bool drawFittedSymbol(CTFontRef font, CGGlyph glyph, CGFloat x, CGFloat baseline, CGContextRef context);
        void centerFallbackColorCluster(CTFontRef font, const CGGlyph* glyphs, CGPoint* positions, size_t count);

        Composer& composer_;
        IntrusivePtr<FontFace> source_;
        CTFontRef font_;
        FontKind kind_;
        FontMetrics metrics_;
        bool syntheticBold_ = false;
        bool syntheticItalic_ = false;
        u16 canvasWidth_ = 0;
        Buffer characters_;
        Buffer runScratch_;
        Buffer columns_;
        Buffer fitted_;
        Buffer bitmap_;
    };

    struct CoreTextFontResolver final: public FontResolver {
        explicit CoreTextFontResolver(Composer& composer)
            : composer(composer)
        {
        }

        FontFace* resolve(const FontRequest& request) override;
        FontFace* resolveCluster(const u32* codepoints, size_t count, FontPlane plane) override;

        CTFontRef resolveName(const FontRequest& request);
        CTFontRef applyStyle(CTFontRef font, FontStyle style, u16 pixels);
        bool matchesName(CTFontRef font, CFStringRef name);
        bool faceSource(CTFontRef font, char path[4096], i32& faceIndex);
        FontFace* extractFace(CTFontRef font);

        Composer& composer;
    };

    struct CoreTextFontRenderer final: public FontRenderer {
        explicit CoreTextFontRenderer(Composer& composer_)
            : composer(composer_)
        {
        }

        Font* render(ObjPool& owner, IntrusivePtr<FontFace> face, u16 pixels, FontKind kind, FontMetrics& metrics) override;

        CTFontRef openFace(const FontFace& face, u16 pixels);
        FontMetrics measure(CTFontRef font);
        Composer& composer;
    };

    static u16 roundPositive(CGFloat value) {
        if (value <= 0) {
            return 0;
        }
        if (value >= UINT16_MAX) {
            return UINT16_MAX;
        }
        return (u16)(value + 0.5);
    }

    static u16 roundUpPositive(CGFloat value) {
        if (value <= 0) {
            return 0;
        }
        if (value >= UINT16_MAX) {
            return UINT16_MAX;
        }
        const u16 truncated = (u16)(value);
        return truncated + (truncated < value);
    }

    static bool sameString(CFStringRef left, CFStringRef right) {
        return left != nullptr && right != nullptr && CFStringCompare(left, right, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    }

    static bool pathName(StringView name) {
        return name.memChr('/') || name.memChr('\\');
    }

    static CFStringRef makeString(StringView value) {
        return CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)(value.data()), value.length(), kCFStringEncodingUTF8, false);
    }
}

CoreTextFont::CoreTextFont(Composer& composer, IntrusivePtr<FontFace> source, CTFontRef font, FontKind kind, FontMetrics metrics, FontStyle synthetic)
    : composer_(composer)
    , source_(source)
    , font_(font)
    , kind_(kind)
    , metrics_(metrics)
    , syntheticBold_(synthetic == FontStyle::Bold || synthetic == FontStyle::BoldItalic)
    , syntheticItalic_(synthetic == FontStyle::Italic || synthetic == FontStyle::BoldItalic)
{
}

FontFace* CoreTextFont::face() {
    return source_.mutPtr();
}

Font* CoreTextFont::synthesize(ObjPool& owner, FontStyle style) {
    CFRetain(font_);
    return owner.make<CoreTextFont>(composer_, source_, font_, FontKind::Overlay, metrics_, style);
}

CoreTextFont::~CoreTextFont() noexcept {
    CFRelease(font_);
}

void CoreTextFont::render(const u32* codepoints, size_t count, u16 cells, void* buf) {
    const bool color = colored();
    const size_t bytesPerPixel = color ? 4 : 1;
    const size_t stride = (size_t)(cells)*metrics_.width * bytesPerPixel;
    CGColorSpaceRef colorSpace = color ? CGColorSpaceCreateDeviceRGB() : CGColorSpaceCreateDeviceGray();
    if (colorSpace == nullptr) {
        return;
    }
    const CGBitmapInfo bitmapInfo = color ? (CGBitmapInfo)kCGImageAlphaPremultipliedLast | (CGBitmapInfo)kCGBitmapByteOrder32Big : (CGBitmapInfo)kCGImageAlphaNone;
    CGContextRef context = CGBitmapContextCreate(buf, (size_t)(cells)*metrics_.width, metrics_.height, 8, stride, colorSpace, bitmapInfo);
    CGColorSpaceRelease(colorSpace);
    if (context == nullptr) {
        return;
    }
    CGContextSetAllowsAntialiasing(context, true);
    CGContextSetShouldAntialias(context, true);
    CGContextSetAllowsFontSmoothing(context, false);
    CGContextSetShouldSmoothFonts(context, false);
    if (color) {
        CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    } else {
        CGContextSetGrayFillColor(context, 1, 1);
    }
    if (syntheticBold_) {
        CGContextSetTextDrawingMode(context, kCGTextFillStroke);
        CGContextSetLineWidth(context, metrics_.height * 0.03);
        if (color) {
            CGContextSetRGBStrokeColor(context, 1, 1, 1, 1);
        } else {
            CGContextSetGrayStrokeColor(context, 1, 1);
        }
    }
    CGAffineTransform matrix = CGAffineTransformIdentity;
    if (syntheticItalic_) {
        matrix.c = 0.25;
    }
    CGContextSetTextMatrix(context, matrix);

    // One CTLine over the whole span - Core Text forms the ligatures -
    // but the glyphs draw at cluster-snapped positions: a font advance
    // disagreeing with the cell width must not accumulate across a long
    // run, so every cluster re-bases at its grid column and keeps its
    // intra-cluster offsets.
    columns_.reset();
    columns_.grow(2 * count * sizeof(u16));
    fitted_.reset();
    fitted_.grow(2 * count);
    auto* const utf16Columns = (u16*)(columns_.mutData());
    auto* const utf16Fitted = (u8*)(fitted_.mutData());
    size_t utf16Length = 0;
    {
        size_t position = 0;
        u16 column = 0;
        SpanCluster cluster;
        SpanCluster next;
        bool haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
        while (haveNext) {
            cluster = next;
            haveNext = composer_.vtConfig.config->widths.nextSpanCluster(codepoints, count, position, next);
            const bool nextBlank = haveNext && next.count == 1 && codepoints[next.begin] == ' ';
            const bool capturedBlank = nextBlank && column + 1 < cells;
            const bool fitted = cluster.cells == 1 && cluster.count == 1 && puaSymbol(codepoints[cluster.begin]) && !capturedBlank;
            for (size_t index = cluster.begin; index < cluster.begin + cluster.count; ++index) {
                const size_t units = codepoints[index] > 0xffff ? 2 : 1;
                for (size_t unit = 0; unit < units; ++unit) {
                    utf16Columns[utf16Length++] = column;
                    utf16Fitted[utf16Length - 1] = fitted;
                }
            }
            column = (u16)(column + cluster.cells);
        }
    }
    CFStringRef string = makeString(codepoints, count);
    if (string != nullptr) {
        CTLineRef line = makeLine(string);
        CFRelease(string);
        if (line != nullptr) {
            bool lineColor = false;
            if (inspectLine(line, lineColor) && lineColor == color) {
                const CGFloat baselineY = (CGFloat)(metrics_.height - metrics_.baseline);
                CFArrayRef runs = CTLineGetGlyphRuns(line);
                const CFIndex runCount = CFArrayGetCount(runs);
                for (CFIndex runIndex = 0; runIndex < runCount; ++runIndex) {
                    auto run = (CTRunRef)(CFArrayGetValueAtIndex(runs, runIndex));
                    const CFIndex glyphCount = CTRunGetGlyphCount(run);
                    if (glyphCount <= 0) {
                        continue;
                    }
                    runScratch_.reset();
                    runScratch_.grow((size_t)(glyphCount) * (sizeof(CGGlyph) + sizeof(CGPoint) * 2 + sizeof(CFIndex)));
                    auto* const glyphs = (CGGlyph*)(runScratch_.mutData());
                    auto* const natural = (CGPoint*)(glyphs + glyphCount);
                    auto* const adjusted = natural + glyphCount;
                    auto* const indices = (CFIndex*)(adjusted + glyphCount);
                    CTRunGetGlyphs(run, {0, glyphCount}, glyphs);
                    CTRunGetPositions(run, {0, glyphCount}, natural);
                    CTRunGetStringIndices(run, {0, glyphCount}, indices);
                    CFDictionaryRef attributes = CTRunGetAttributes(run);
                    auto runFont = (CTFontRef)(CFDictionaryGetValue(attributes, kCTFontAttributeName));
                    if (runFont == nullptr) {
                        runFont = font_;
                    }
                    CFIndex index = 0;
                    while (index < glyphCount) {
                        const CFIndex clusterBegin = index;
                        const CFIndex clusterIndex = indices[index];
                        const CGFloat target = clusterIndex >= 0 && (size_t)(clusterIndex) < utf16Length ? (CGFloat)((size_t)(utf16Columns[clusterIndex]) * metrics_.width) : natural[index].x;
                        const CGFloat base = natural[index].x;
                        while (index < glyphCount && indices[index] == clusterIndex) {
                            adjusted[index].x = target + (natural[index].x - base);
                            adjusted[index].y = baselineY + natural[index].y;
                            ++index;
                        }
                        if (kind_ == FontKind::Fallback && color) {
                            centerFallbackColorCluster(runFont, glyphs + clusterBegin, adjusted + clusterBegin, (size_t)(index - clusterBegin));
                        }
                        const bool fit = clusterIndex >= 0 && (size_t)(clusterIndex) < utf16Length && utf16Fitted[clusterIndex];
                        if (fit && index == clusterBegin + 1 && drawFittedSymbol(runFont, glyphs[clusterBegin], target, adjusted[clusterBegin].y, context)) {
                            continue;
                        }
                        CTFontDrawGlyphs(runFont, glyphs + clusterBegin, adjusted + clusterBegin, (size_t)(index - clusterBegin), context);
                    }
                }
            }
            CFRelease(line);
        }
    }
    CGContextRelease(context);
}

bool CoreTextFont::colored() const {
    return (CTFontGetSymbolicTraits(font_) & kCTFontColorGlyphsTrait) != 0;
}

bool CoreTextFont::covers(u32 codepoint) {
    UniChar characters[2];
    CFIndex length = 0;
    if (codepoint <= 0xffff && (codepoint < 0xd800 || codepoint > 0xdfff)) {
        characters[length++] = (UniChar)(codepoint);
    } else if (codepoint <= 0x10ffff) {
        const u32 scalar = codepoint - 0x10000;
        characters[length++] = (UniChar)(0xd800 + (scalar >> 10));
        characters[length++] = (UniChar)(0xdc00 + (scalar & 0x3ff));
    } else {
        return false;
    }
    CGGlyph glyphs[2] = {};
    return CTFontGetGlyphsForCharacters(font_, characters, glyphs, length);
}

CFStringRef CoreTextFont::makeString(const u32* codepoints, size_t count) {
    characters_.reset();
    characters_.grow(2 * count * sizeof(UniChar));
    for (size_t index = 0; index < count; ++index) {
        const u32 codepoint = codepoints[index];
        if (codepoint <= 0xffff && (codepoint < 0xd800 || codepoint > 0xdfff)) {
            const UniChar value = (UniChar)(codepoint);
            characters_.append(&value, sizeof(value));
        } else if (codepoint <= 0x10ffff) {
            const u32 scalar = codepoint - 0x10000;
            const UniChar values[] = {
                (UniChar)(0xd800 + (scalar >> 10)),
                (UniChar)(0xdc00 + (scalar & 0x3ff)),
            };
            characters_.append(values, sizeof(values));
        }
    }
    return CFStringCreateWithCharacters(kCFAllocatorDefault, (const UniChar*)(characters_.data()), characters_.used() / sizeof(UniChar));
}

CTLineRef CoreTextFont::makeLine(CFStringRef string) {
    // The CTFont carries the exact grid policy: liga and dlig are off,
    // calt is on. Do not add kCTLigatureAttributeName here: level 2 means
    // "all ligatures" and overrides that policy on newer Core Text.
    const void* keys[] = {
        kCTFontAttributeName,
        kCTForegroundColorFromContextAttributeName,
    };
    const void* values[] = {
        font_,
        kCFBooleanTrue,
    };
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (attributes == nullptr) {
        return nullptr;
    }
    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
    CFRelease(attributes);
    if (attributed == nullptr) {
        return nullptr;
    }
    CTLineRef line = CTLineCreateWithAttributedString(attributed);
    CFRelease(attributed);
    return line;
}

bool CoreTextFont::inspectLine(CTLineRef line, bool& color) {
    color = false;
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    const CFIndex runCount = CFArrayGetCount(runs);
    if (runCount == 0) {
        return false;
    }
    for (CFIndex runIndex = 0; runIndex < runCount; ++runIndex) {
        auto run = (CTRunRef)(CFArrayGetValueAtIndex(runs, runIndex));
        const CFIndex glyphCount = CTRunGetGlyphCount(run);
        if (glyphCount == 0) {
            return false;
        }
        const CGGlyph* glyphs = CTRunGetGlyphsPtr(run);
        if (glyphs != nullptr) {
            for (CFIndex glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
                if (glyphs[glyphIndex] == 0) {
                    return false;
                }
            }
        } else {
            for (CFIndex glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
                CGGlyph glyph = 0;
                CTRunGetGlyphs(run, {glyphIndex, 1}, &glyph);
                if (glyph == 0) {
                    return false;
                }
            }
        }
        CFDictionaryRef attributes = CTRunGetAttributes(run);
        auto font = (CTFontRef)(CFDictionaryGetValue(attributes, kCTFontAttributeName));
        if (font != nullptr && (CTFontGetSymbolicTraits(font) & kCTFontColorGlyphsTrait) != 0) {
            color = true;
        }
    }
    return true;
}

bool CoreTextFont::drawLine(CTLineRef line, bool color) {
    const size_t bytesPerPixel = color ? 4 : 1;
    const size_t stride = (size_t)(canvasWidth_)*bytesPerPixel;
    bitmap_.zero(stride * metrics_.height);

    CGColorSpaceRef colorSpace = color ? CGColorSpaceCreateDeviceRGB() : CGColorSpaceCreateDeviceGray();
    if (colorSpace == nullptr) {
        return false;
    }
    const CGBitmapInfo bitmapInfo = color ? (CGBitmapInfo)kCGImageAlphaPremultipliedLast | (CGBitmapInfo)kCGBitmapByteOrder32Big : (CGBitmapInfo)kCGImageAlphaNone;
    CGContextRef context = CGBitmapContextCreate(bitmap_.mutData(), canvasWidth_, metrics_.height, 8, stride, colorSpace, bitmapInfo);
    CGColorSpaceRelease(colorSpace);
    if (context == nullptr) {
        return false;
    }

    CGContextSetAllowsAntialiasing(context, true);
    CGContextSetShouldAntialias(context, true);
    CGContextSetAllowsFontSmoothing(context, false);
    CGContextSetShouldSmoothFonts(context, false);
    if (color) {
        CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    } else {
        CGContextSetGrayFillColor(context, 1, 1);
    }
    if (syntheticBold_) {
        // Fake bold: fill and stroke, the stroke width scaled to the size.
        CGContextSetTextDrawingMode(context, kCGTextFillStroke);
        CGContextSetLineWidth(context, metrics_.height * 0.03);
        if (color) {
            CGContextSetRGBStrokeColor(context, 1, 1, 1, 1);
        } else {
            CGContextSetGrayStrokeColor(context, 1, 1);
        }
    }
    CGAffineTransform matrix = CGAffineTransformIdentity;
    if (syntheticItalic_) {
        // Fake italic: a horizontal shear of about 14 degrees.
        matrix.c = 0.25;
    }
    CGContextSetTextMatrix(context, matrix);
    CGContextSetTextPosition(context, 0, metrics_.height - metrics_.baseline);
    CTLineDraw(line, context);
    CGContextRelease(context);
    return true;
}

// A one-cell private-use pictogram with no captured blank owns exactly one
// grid cell. Core Text otherwise draws the face's natural (usually two-cell)
// outline and lets the bitmap clip it at the cell edge. Fit the outline to an
// inset cell and center its integral ink bounds, mirroring the FreeType path.
bool CoreTextFont::drawFittedSymbol(CTFontRef font, CGGlyph glyph, CGFloat x, CGFloat baseline, CGContextRef context) {
    if (metrics_.width <= 2) {
        return false;
    }
    const CGFloat targetWidth = metrics_.width - 2;
    const CGFloat originalSize = CTFontGetSize(font);
    CGFloat size = originalSize;
    CGRect bounds = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
    CGRect ink = CGRectIntegral(bounds);
    if (CGRectIsNull(ink) || CGRectIsEmpty(ink)) {
        return false;
    }
    if (ink.size.width > targetWidth) {
        size = (CGFloat)((u16)(originalSize * targetWidth / ink.size.width));
        if (size < 4) {
            size = 4;
        }
    }
    while (size >= 4) {
        CTFontRef fitted = CTFontCreateCopyWithAttributes(font, size, nullptr, nullptr);
        if (fitted == nullptr) {
            return false;
        }
        bounds = CTFontGetBoundingRectsForGlyphs(fitted, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
        ink = CGRectIntegral(bounds);
        if (!CGRectIsNull(ink) && !CGRectIsEmpty(ink) && ink.size.width <= targetWidth) {
            const CGFloat left = (CGFloat)((u16)((metrics_.width - ink.size.width) / 2));
            const CGPoint position = {
                x + left - ink.origin.x,
                baseline,
            };
            CTFontDrawGlyphs(fitted, &glyph, &position, 1, context);
            CFRelease(fitted);
            return true;
        }
        CFRelease(fitted);
        size -= 1;
    }
    return false;
}

// A fallback does not impose its own line metrics: it draws into the cell
// defined by the primary face. Baseline placement therefore has no common
// coordinate system for a color emoji face (Apple Color Emoji in issue 89),
// whose ink otherwise sits high above the middle of the primary cell. Match
// the FreeType color path and center the cluster's actual ink bounds instead.
void CoreTextFont::centerFallbackColorCluster(CTFontRef font, const CGGlyph* glyphs, CGPoint* positions, size_t count) {
    CGRect ink = CGRectNull;
    for (size_t index = 0; index < count; ++index) {
        CGRect bounds;
        CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, glyphs + index, &bounds, 1);
        if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
            continue;
        }
        bounds.origin.x += positions[index].x;
        bounds.origin.y += positions[index].y;
        ink = CGRectIsNull(ink) ? bounds : CGRectUnion(ink, bounds);
    }
    if (CGRectIsNull(ink) || CGRectIsEmpty(ink)) {
        return;
    }
    const CGFloat shift = metrics_.height * 0.5 - CGRectGetMidY(ink);
    for (size_t index = 0; index < count; ++index) {
        positions[index].y += shift;
    }
}

bool CoreTextFontResolver::matchesName(CTFontRef font, CFStringRef name) {
    CFStringRef family = CTFontCopyFamilyName(font);
    CFStringRef full = CTFontCopyFullName(font);
    CFStringRef postscript = CTFontCopyPostScriptName(font);
    const bool matches = sameString(name, family) || sameString(name, full) || sameString(name, postscript);
    if (family != nullptr) {
        CFRelease(family);
    }
    if (full != nullptr) {
        CFRelease(full);
    }
    if (postscript != nullptr) {
        CFRelease(postscript);
    }
    return matches;
}

CTFontRef CoreTextFontResolver::applyStyle(CTFontRef font, FontStyle style, u16 pixels) {
    CTFontSymbolicTraits traits = 0;
    if (style == FontStyle::Bold || style == FontStyle::BoldItalic) {
        traits |= kCTFontBoldTrait;
    }
    if (style == FontStyle::Italic || style == FontStyle::BoldItalic) {
        traits |= kCTFontItalicTrait;
    }
    if (traits == 0) {
        return font;
    }
    CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(font, pixels, nullptr, traits, traits);
    CFRelease(font);
    if (styled == nullptr || (CTFontGetSymbolicTraits(styled) & traits) != traits) {
        if (styled != nullptr) {
            CFRelease(styled);
        }
        return nullptr;
    }
    return styled;
}

CTFontRef CoreTextFontResolver::resolveName(const FontRequest& request) {
    if (request.name == StringView(u8"monospace")) {
        CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, request.pixels, nullptr);
        return font == nullptr ? nullptr : applyStyle(font, request.style, request.pixels);
    }

    CFStringRef name = makeString(request.name);
    if (name == nullptr) {
        return nullptr;
    }
    CTFontRef font = CTFontCreateWithName(name, request.pixels, nullptr);
    if (font != nullptr && !matchesName(font, name)) {
        CFRelease(font);
        font = nullptr;
    }
    CFRelease(name);
    return font == nullptr ? nullptr : applyStyle(font, request.style, request.pixels);
}

// The resolved artifact is the file behind the CTFont plus the index of
// its face inside the collection, found by PostScript name among the
// file's descriptors. Path requests fall through to the generic mmap
// resolver.
bool CoreTextFontResolver::faceSource(CTFontRef font, char path[4096], i32& faceIndex) {
    auto url = (CFURLRef)(CTFontCopyAttribute(font, kCTFontURLAttribute));
    if (url == nullptr) {
        return false;
    }
    if (!CFURLGetFileSystemRepresentation(url, true, (UInt8*)(path), 4096)) {
        CFRelease(url);
        return false;
    }
    faceIndex = 0;
    CFStringRef wanted = CTFontCopyPostScriptName(font);
    CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromURL(url);
    CFRelease(url);
    if (descriptors != nullptr) {
        const CFIndex count = CFArrayGetCount(descriptors);
        for (CFIndex index = 0; index < count; ++index) {
            auto descriptor = (CTFontDescriptorRef)(CFArrayGetValueAtIndex(descriptors, index));
            auto name = (CFStringRef)(CTFontDescriptorCopyAttribute(descriptor, kCTFontNameAttribute));
            const bool matches = sameString(wanted, name);
            if (name != nullptr) {
                CFRelease(name);
            }
            if (matches) {
                faceIndex = (i32)(index);
                break;
            }
        }
        CFRelease(descriptors);
    }
    if (wanted != nullptr) {
        CFRelease(wanted);
    }
    return true;
}

FontFace* CoreTextFontResolver::extractFace(CTFontRef font) {
    char path[4096];
    i32 faceIndex = 0;
    if (!faceSource(font, path, faceIndex)) {
        return nullptr;
    }
    return openFontFile(StringView(path), faceIndex);
}

// The system's answer for one uncovered cluster: Core Text picks the
// face it would cascade to for these codepoints - CJK, Apple Color
// Emoji, any script the configured families miss - ahead of the
// embedded last resort in the resolver chain.
FontFace* CoreTextFontResolver::resolveCluster(const u32* codepoints, size_t count, FontPlane plane) {
    // Core Text chooses the plane itself: emoji cascade to Apple Color
    // Emoji, text to a mask face.
    (void)(plane);
    if (count == 0) {
        return nullptr;
    }
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, 0.0, nullptr);
    if (base == nullptr) {
        return nullptr;
    }
    UniChar units[64];
    CFIndex length = 0;
    for (size_t index = 0; index < count && length + 2 <= (CFIndex)(sizeof(units) / sizeof(units[0])); ++index) {
        const u32 codepoint = codepoints[index];
        if (codepoint >= 0x10000) {
            units[length++] = (UniChar)(0xd800 + ((codepoint - 0x10000) >> 10));
            units[length++] = (UniChar)(0xdc00 + ((codepoint - 0x10000) & 0x3ff));
        } else {
            units[length++] = (UniChar)(codepoint);
        }
    }
    CFStringRef string = CFStringCreateWithCharacters(kCFAllocatorDefault, units, length);
    if (string == nullptr) {
        CFRelease(base);
        return nullptr;
    }
    CTFontRef font = CTFontCreateForString(base, string, CFRangeMake(0, length));
    CFRelease(string);
    CFRelease(base);
    if (font == nullptr) {
        return nullptr;
    }
    // LastResort answers for everything with placeholder boxes; a miss
    // must fall through to the embedded faces instead.
    CFStringRef name = CTFontCopyPostScriptName(font);
    const bool lastResort = name != nullptr && CFStringFind(name, CFSTR("LastResort"), 0).location != kCFNotFound;
    if (name != nullptr) {
        CFRelease(name);
    }
    if (lastResort) {
        CFRelease(font);
        return nullptr;
    }
    FontFace* face = nullptr;
    try {
        face = extractFace(font);
    } catch (Exception&) {
        // A cascade answer whose file cannot be opened falls through to
        // the next resolver instead of unwinding the frame.
    }
    CFRelease(font);
    return face;
}

FontFace* CoreTextFontResolver::resolve(const FontRequest& request) {
    if (pathName(request.name)) {
        return nullptr;
    }
    CTFontRef font = resolveName(request);
    if (font == nullptr) {
        return nullptr;
    }
    FontFace* face = nullptr;
    try {
        face = extractFace(font);
    } catch (Exception&) {
    }
    CFRelease(font);
    return face;
}

namespace {

    // The grid's ligature policy, pinned on the font itself: liga and
    // dlig collapse two cells' codepoints into one narrow glyph, so they
    // go off; coding fonts carry their ligatures in calt, which Core
    // Text's fixed-pitch heuristic must not suppress - JetBrains Mono
    // lost its arrows without an explicit opt-in (bb4d76af).
    static CTFontRef withGridFeatures(CTFontRef font) {
        if (font == nullptr) {
            return nullptr;
        }

        struct Setting {
            CFStringRef tag;
            int value;
        };

        static const Setting settings[] = {
            {CFSTR("liga"), 0},
            {CFSTR("dlig"), 0},
            {CFSTR("calt"), 1},
        };
        CFMutableArrayRef features = CFArrayCreateMutable(kCFAllocatorDefault, 3, &kCFTypeArrayCallBacks);
        if (features == nullptr) {
            return font;
        }
        for (const Setting& setting : settings) {
            CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &setting.value);
            if (value == nullptr) {
                CFRelease(features);
                return font;
            }
            const void* keys[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
            const void* values[] = {setting.tag, value};
            CFDictionaryRef feature = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFRelease(value);
            if (feature == nullptr) {
                CFRelease(features);
                return font;
            }
            CFArrayAppendValue(features, feature);
            CFRelease(feature);
        }
        const void* keys[] = {kCTFontFeatureSettingsAttribute};
        const void* values[] = {features};
        CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(features);
        if (attributes == nullptr) {
            return font;
        }
        CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
        CFRelease(attributes);
        if (descriptor == nullptr) {
            return font;
        }
        CTFontRef pinned = CTFontCreateCopyWithAttributes(font, 0.0, nullptr, descriptor);
        CFRelease(descriptor);
        if (pinned == nullptr) {
            return font;
        }
        CFRelease(font);
        return pinned;
    }
}

CTFontRef CoreTextFontRenderer::openFace(const FontFace& face, u16 pixels) {
    // No copy: every font born from this data keeps an IntrusivePtr to
    // the face, so the mapping outlives the CTFont and everything it
    // caches. Copying instead would multiply Apple Color Emoji's
    // hundreds of megabytes across the fallback chain.
    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8*)(face.data()), (CFIndex)(face.size()), kCFAllocatorNull);
    if (data == nullptr) {
        return nullptr;
    }
    CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromData(data);
    CFRelease(data);
    if (descriptors == nullptr) {
        return nullptr;
    }
    CTFontRef font = nullptr;
    const CFIndex count = CFArrayGetCount(descriptors);
    if (count > 0) {
        const CFIndex index = face.faceIndex() >= 0 && face.faceIndex() < count ? face.faceIndex() : 0;
        auto descriptor = (CTFontDescriptorRef)(CFArrayGetValueAtIndex(descriptors, index));
        font = CTFontCreateWithFontDescriptor(descriptor, pixels, nullptr);
    }
    CFRelease(descriptors);
    return withGridFeatures(font);
}

namespace {
    // The font-wide maximum advance, for a face without the narrow
    // representative: hhea's advanceWidthMax in font units, scaled to
    // the instance size. The same fallback the FreeType backend takes
    // through max_advance_width.
    static CGFloat maxAdvance(CTFontRef font) {
        CFDataRef hhea = CTFontCopyTable(font, kCTFontTableHhea, kCTFontTableOptionNoOptions);
        if (hhea == nullptr) {
            return 0;
        }
        CGFloat advance = 0;
        // advanceWidthMax is the unsigned 16-bit word at offset 10.
        if (CFDataGetLength(hhea) >= 12) {
            const UInt8* bytes = CFDataGetBytePtr(hhea);
            const unsigned units = ((unsigned)(bytes[10]) << 8) | bytes[11];
            const unsigned upem = CTFontGetUnitsPerEm(font);
            if (upem != 0) {
                advance = CTFontGetSize(font) * units / upem;
            }
        }
        CFRelease(hhea);
        return advance;
    }
}

FontMetrics CoreTextFontRenderer::measure(CTFontRef font) {
    // The narrow cell is the advance of 'M'; a face without one (a CJK
    // family) falls back to the font-wide maximum.
    const UniChar character = 'M';
    CGGlyph glyph = 0;
    CGSize advance{};
    if (CTFontGetGlyphsForCharacters(font, &character, &glyph, 1) && glyph != 0) {
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &advance, 1);
    } else {
        advance.width = maxAdvance(font);
    }
    const CGFloat ascent = CTFontGetAscent(font);
    const CGFloat descent = CTFontGetDescent(font);
    const CGFloat leading = CTFontGetLeading(font);
    return {
        .width = roundPositive(advance.width),
        .height = roundUpPositive(ascent + descent + leading),
        .baseline = roundUpPositive(ascent),
    };
}

Font* CoreTextFontRenderer::render(ObjPool& owner, IntrusivePtr<FontFace> face, u16 pixels, FontKind kind, FontMetrics& metrics) {
    CTFontRef font = openFace(*face, pixels);
    if (font == nullptr) {
        return nullptr;
    }
    const FontMetrics actual = measure(font);
    // Only the cell-defining kinds answer for their own numbers: a
    // fallback draws into the cell the primary imposed, and rejecting
    // one over its own metrics threw Apple Color Emoji away and turned
    // every emoji cluster into the notdef box (issue 85).
    if (kind != FontKind::Fallback && (actual.width == 0 || actual.height == 0 || actual.baseline == 0)) {
        CFRelease(font);
        return nullptr;
    }
    if (composer.vtConfig.config->verbose) {
        sysO << StringView(u8"coretext face: kind ") << (u64)((u8)(kind)) << StringView(u8" at ") << pixels << StringView(u8"px, cell ") << actual.width << StringView(u8"x") << actual.height << StringView(u8" baseline ") << actual.baseline << StringView(u8"\n");
    }
    if (kind == FontKind::Primary) {
        metrics = actual;
    } else if (kind == FontKind::Overlay && (metrics.height != actual.height || metrics.baseline != actual.baseline)) {
        CFRelease(font);
        return nullptr;
    }
    return owner.make<CoreTextFont>(composer, face, font, kind, metrics, FontStyle::Regular);
}

FontResolver* createCoreTextFontResolver(Composer& composer) {
    return composer.pool->make<CoreTextFontResolver>(composer);
}

FontRenderer* createCoreTextFontRenderer(Composer& composer) {
    return composer.pool->make<CoreTextFontRenderer>(composer);
}
#else
FontResolver* createCoreTextFontResolver(Composer& composer) {
    (void)(composer);
    return nullptr;
}

FontRenderer* createCoreTextFontRenderer(Composer& composer) {
    (void)(composer);
    return nullptr;
}
#endif
