/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_wayland_chrome.h"

#include "brand.h"
#include "bookmark_probe.h"
#include "bookmarks.h"
#include "chrome_canvas.h"
#include "composer.h"
#include "font_embedded.h"
#include "options.h"
#include "process_directory.h"
#include "palette_session.h"
#include "session.h"
#include "sidebar_actions.h"
#include "sidebar_field.h"
#include "sidebar_menu.h"
#include "sidebar_rows.h"
#include "tab_rows.h"
#include "ui_text.h"
#include "window_chrome.h"

#include <lib/vterm/listener.h>
#include <lib/vterm/vt_config.h>

#include <plt/window.h>

#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>

#include <math.h>
#include <stdio.h>

using namespace stl;

namespace {
    // Text sizes in logical units: the Mac's small system size for titles,
    // a step down for the folder and branch line, a step up for a folder's
    // header.
    constexpr float titleSize = 12.5f;
    constexpr float subSize = 11.0f;
    constexpr float labelSize = 13.0f;
    constexpr float iconSize = 13.0f;
    // Nerd Font glyphs in the embedded face: a folder, a git branch, a
    // terminal, and the two chevrons of a folder's label.
    constexpr const char* folderGlyph = "\xef\x81\xbb";
    constexpr const char* branchGlyph = "\xee\x9c\xa5";
    constexpr const char* terminalGlyph = "\xef\x92\x89";
    constexpr const char* chevronDown = "\xef\x81\xb8";
    constexpr const char* chevronRight = "\xef\x81\x94";
    // Without a compositor blur the surface would show the desktop crisp
    // through it; this much of it stays however translucent it is asked to be.
    constexpr float surfaceFloor = 0.88f;
    // The list that comes out at the edge is nearly solid: it sits over
    // the terminal's text.
    constexpr float revealOpacity = 0.96f;

    ChromeColor colorOf(const Color& color, float alpha = 1) {
        return ChromeColor{color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, alpha};
    }

    ChromeColor mix(const Color& from, const Color& to, float amount, float alpha = 1) {
        const ChromeColor a = colorOf(from);
        const ChromeColor b = colorOf(to);
        return ChromeColor{a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount, a.b + (b.b - a.b) * amount, alpha};
    }

    ChromeColor withAlpha(ChromeColor color, float alpha) {
        color.a = alpha;
        return color;
    }

    // Where a row's texts are in the arena.
    struct RowText {
        size_t title = 0;
        size_t titleLength = 0;
        size_t folder = 0;
        size_t folderLength = 0;
        size_t branch = 0;
        size_t branchLength = 0;
    };

    struct WaylandChrome;

    struct CallSessionsChanged final: public Listener {
        explicit CallSessionsChanged(WaylandChrome* owner_)
            : owner(owner_)
        {
        }
        void onListen(void*) override;
        WaylandChrome* owner;
    };

    struct CallToggleSidebar final: public Listener {
        explicit CallToggleSidebar(WaylandChrome* owner_)
            : owner(owner_)
        {
        }
        void onListen(void*) override;
        WaylandChrome* owner;
    };

    struct CallCommandPalette final: public Listener {
        explicit CallCommandPalette(WaylandChrome* owner_)
            : owner(owner_)
        {
        }
        void onListen(void*) override;
        WaylandChrome* owner;
    };

    struct CallConfigChanged final: public Listener {
        explicit CallConfigChanged(WaylandChrome* owner_)
            : owner(owner_)
        {
        }
        void onListen(void*) override;
        WaylandChrome* owner;
    };

    struct WaylandChrome final: public plt::ChromeSink, public PaletteHost {
        explicit WaylandChrome(Composer& composer_);

        void chrome(const plt::ChromeEvent& event) override;
        bool capturesKeys() override;
        void chromeKey(const plt::KeyInput& key) override;
        void chromeText(u32 codepoint) override;

        // The command palette (palette_session.h), in the menu layer over
        // the terminal panel's middle.
        void paletteChanged() override;
        void openPalette();
        void closePalette();
        void placePalette();
        void drawPalette();
        void paletteKey(const plt::KeyInput& key);
        void palettePointer(const plt::ChromeEvent& event);
        ChromeRect paletteListRect() const;

        void project();
        void relayout();
        void redraw();
        void openFonts(float scale);
        void toggleList();
        void reveal(bool shown);
        void pressed(const ChromeHit& hit, u32 clicks);
        void rowSelected(size_t row);
        ChromeHit hitAt(u8 layer, float x, float y) const;
        float listRowTop(u8 layer, size_t at) const;

        // The context menu (sidebar_menu.h), in the menu layer.
        void openMenu(const plt::ChromeEvent& event, const ChromeHit& hit);
        void closeMenu();
        void drawMenu();
        void menuPicked(size_t index);
        // A name typed in place in its row (sidebar_field.h).
        void beginEditFolder(StringView folder);
        void beginEditRow(const TabRow& row);
        void commitEdit();
        void cancelEdit();
        long long editedRow() const;
        void drawField(ChromeCanvas& canvas, UiText& font, float s, float x, float lineTop, float lineHeight, float maxWidth);
        // A row dragged in the list: where it would land.
        void dragTo(float x, float y);

        void drawWindow(ChromeCanvas& canvas);
        void drawList(ChromeCanvas& canvas, float s, float left, float top, float width, float height, float listTop, const ChromeRect* buttons, float buttonsLeft, float buttonsTop);
        void drawButtons(ChromeCanvas& canvas, float s, const ChromeRect* buttons, float offsetX, float offsetY, bool hovered);
        float drawText(ChromeCanvas& canvas, UiText& font, float s, float x, float lineTop, float lineHeight, StringView text, ChromeColor color, float maxWidth);
        StringView rowText(size_t start, size_t length) const {
            return StringView((const u8*)(arena.data()) + start, length);
        }

        Composer& composer;
        plt::WindowChrome* window = nullptr;
        ChromeGeometry geometry;
        ChromeLayout layout;
        ChromeCanvas base;
        ChromeCanvas overlay;
        ChromeRect revealList;
        ChromeRect revealButtons[3];
        Vector<TabRow> rows;
        Vector<double> heights;
        Vector<RowText> texts;
        Vector<StringView> collapsed;
        Buffer arena;
        size_t active = 0;
        long long hoverRow = -1;
        u8 hoverLayer = 0;
        bool hoverButtons = false;
        bool listShown = true;
        bool revealed = false;
        float fontScale = 0;
        UiText titleFont;
        UiText activeFont;
        UiText subFont;
        UiText labelFont;
        UiText iconFont;
        ChromeCanvas menuCanvas;
        Vector<SidebarMenuItem> menuItems;
        ChromeRect menuRect;
        TabRow menuRow;
        bool menuHasRow = false;
        bool menuShown = false;
        long long menuHover = -1;
        SidebarField field;
        bool editing = false;
        bool editingFolder = false;
        StringView editFolder;
        TabRow editRow;
        bool pressing = false;
        bool dragging = false;
        TabRow pressModel;
        float pressX = 0;
        float pressY = 0;
        size_t dropIndex = 0;
        bool dropOnLabel = false;
        CallSessionsChanged sessionsChanged{this};
        CallToggleSidebar toggleSidebar{this};
        CallConfigChanged configChanged{this};
        CallCommandPalette commandPalette{this};
        PaletteSession palette{composer, *this};
        ChromeRect paletteRect;
        ChromeCanvas paletteCanvas;
        float paletteScroll = 0;
    };

    void CallSessionsChanged::onListen(void*) {
        owner->project();
    }

    void CallToggleSidebar::onListen(void*) {
        owner->toggleList();
    }

    void CallConfigChanged::onListen(void*) {
        owner->fontScale = 0;
        owner->relayout();
        owner->project();
    }
}

WaylandChrome::WaylandChrome(Composer& composer_)
    : composer(composer_)
{
    window = composer.window != nullptr ? composer.window->chrome() : nullptr;
    if (window == nullptr) {
        return;
    }
    window->setSink(this);
    composer.sessionsChangedListeners.pushBack(&sessionsChanged);
    composer.toggleSidebarListeners.pushBack(&toggleSidebar);
    composer.configChangedListeners.pushBack(&configChanged);
    composer.commandPaletteListeners.pushBack(&commandPalette);
    relayout();
    if (composer.vtConfig.config != nullptr && composer.vtConfig.config->verbose) {
        fprintf(stderr, "%s: chrome: the window is drawn by this program, the list %s wide\n", composer.brand->identifierCString(), composer.opts->sidebarWidth != 0 ? "sidebarWidth" : "default");
    }
}

// Everything the window's shape depends on: the toplevel's size and state,
// the list shown or put away, and the options. The insets go to the window
// whenever they change - the terminal is resized to what they leave.
void WaylandChrome::relayout() {
    const plt::ChromeState state = window->state();
    geometry.width = (float)(state.width);
    geometry.height = (float)(state.height);
    geometry.listShown = listShown;
    geometry.floating = !state.maximized && !state.fullscreen && !state.tiled;
    geometry.sidebarWidth = composer.opts->sidebarWidth != 0 ? (float)(composer.opts->sidebarWidth) : 220.0f;
    geometry.panelGap = (float)(composer.opts->panelGap);
    geometry.panelRadius = (float)(composer.opts->panelRadius);
    chromeLayout(geometry, layout);
    window->setInsets(layout.insetLeft, layout.insetTop, layout.insetRight, layout.insetBottom, layout.insetMargin);
    // The insets do not move with the size, but setInsets() keeps the
    // toplevel's size and gives the content what is left: lay out again
    // against what the toplevel is now.
    const plt::ChromeState after = window->state();
    geometry.width = (float)(after.width);
    geometry.height = (float)(after.height);
    chromeLayout(geometry, layout);
    chromeRevealLayout(layout, geometry.sidebarWidth, geometry.panelGap, revealList, revealButtons);
}

void WaylandChrome::openFonts(float scale) {
    if (scale == fontScale) {
        return;
    }
    fontScale = scale;
    Buffer path;
    i32 index = 0;
    const void* embedded = nullptr;
    unsigned long embeddedSize = 0;
    embeddedMonoFont(embedded, embeddedSize);
    const auto open = [&](UiText& text, bool bold, float size) {
        if (findUiFont(bold, path, index) && text.openFile(path.cStr(), index, size * scale)) {
            return;
        }
        text.openMemory(embedded, embeddedSize, 0, size * scale);
    };
    open(titleFont, false, titleSize);
    open(activeFont, true, titleSize);
    open(subFont, false, subSize);
    open(labelFont, true, labelSize);
    // The icons are the embedded face's own Nerd Font glyphs, which a
    // system face would not carry.
    iconFont.openMemory(embedded, embeddedSize, 0, iconSize * scale);
}

// The rows and what each says, as the Mac's list has them: a bookmark's
// name and status, else the running title, the directory and the branch.
void WaylandChrome::project() {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr) {
        return;
    }
    tabRows(*sessions, composer.bookmarks, collapsed, rows);
    heights.clear();
    texts.clear();
    arena.reset();
    active = 0;
    Buffer directory;
    Buffer branch;
    StringBuilder text;
    const auto put = [&](StringView value, size_t& start, size_t& length) {
        start = arena.used();
        length = value.length();
        arena.append(value.data(), value.length());
    };
    for (size_t at = 0; at < rows.length(); ++at) {
        const TabRow& row = rows[at];
        const bool previousInFolder = at > 0 && !rows[at - 1].folder.empty();
        const double lead = sidebarTabsStartsSection(row.label, !row.folder.empty(), previousInFolder, row.afterBookmarks) ? SidebarMetrics::sectionGap : 0;
        heights.pushBack(row.label ? SidebarMetrics::labelRowHeight : SidebarMetrics::rowHeight + lead);
        RowText line;
        if (!row.label && !row.closed && row.activeTab && row.focused) {
            active = at;
        }
        if (row.label) {
            // A folder's header: its name, as the user wrote it.
            put(row.folder, line.title, line.titleLength);
            texts.pushBack(line);
            continue;
        }
        const Bookmark* const bookmark = row.bookmark != 0 && composer.bookmarks != nullptr ? composer.bookmarks->find(row.bookmark) : nullptr;
        if (bookmark != nullptr && (!row.grouped || row.groupFirst)) {
            const bool idle = row.closed || row.exited;
            BookmarkState state = row.closed ? BookmarkState::Closed : row.exited ? BookmarkState::Exited : BookmarkState::Open;
            if (idle && composer.bookmarkProbe != nullptr && composer.bookmarkProbe->unreachable(row.bookmark)) {
                state = BookmarkState::Unreachable;
            }
            put(bookmark->title, line.title, line.titleLength);
            bookmarkStatus(*bookmark, state, text);
            put(StringView(text), line.folder, line.folderLength);
            texts.pushBack(line);
            continue;
        }
        StringView title = !row.grouped || row.groupFirst ? sessions->tabTitle(row.tab) : StringView();
        if (title.length() == 0) {
            title = sessions->paneTitle(row.pane);
        }
        if (title.length() == 0) {
            title = composer.brand->displayName();
        }
        put(title, line.title, line.titleLength);
        if (processDirectory(sessions->panePid(row.pane), directory)) {
            put(sidebarTabsShortTitle(StringView(directory)), line.folder, line.folderLength);
            if (sidebarTabsBranch(StringView(directory), branch)) {
                put(StringView(branch), line.branch, line.branchLength);
            } else {
                put(StringView(u8"no git"), line.branch, line.branchLength);
            }
        }
        texts.pushBack(line);
    }
    redraw();
}

void WaylandChrome::redraw() {
    const plt::ChromeState state = window->state();
    if (state.pixelWidth == 0 || state.pixelHeight == 0) {
        return;
    }
    openFonts(state.scale);
    if (base.width() != state.pixelWidth || base.height() != state.pixelHeight) {
        base.resize(state.pixelWidth, state.pixelHeight);
    } else {
        base.clear();
    }
    drawWindow(base);
    window->present(0, base.data(), base.width(), base.height());
    if (revealed) {
        const float s = state.scale;
        const u32 width = (u32)(ceilf(revealList.width * s));
        const u32 height = (u32)(ceilf(revealList.height * s));
        if (overlay.width() != width || overlay.height() != height) {
            overlay.resize(width, height);
        } else {
            overlay.clear();
        }
        const ChromeColor surface = composer.opts->sidebarColorSet ? colorOf(composer.opts->sidebarColor) : mix(composer.vtConfig.config->bg, composer.vtConfig.config->fg, 0.06f);
        overlay.fillRoundedRect(0, 0, (float)(width), (float)(height), 12 * s, withAlpha(surface, revealOpacity));
        overlay.strokeRoundedRect(0, 0, (float)(width), (float)(height), 12 * s, 1 * s, colorOf(composer.vtConfig.config->fg, 0.12f));
        drawList(overlay, s, 0, 0, revealList.width, revealList.height, ChromeMetrics::listTop - ChromeMetrics::buttonTop + (revealButtons[0].y - revealList.y), revealButtons, revealList.x, revealList.y);
        window->present(1, overlay.data(), overlay.width(), overlay.height());
    }
}

float WaylandChrome::drawText(ChromeCanvas& canvas, UiText& font, float s, float x, float lineTop, float lineHeight, StringView text, ChromeColor color, float maxWidth) {
    if (!font.ready() || text.empty()) {
        return 0;
    }
    // Centred on its line by the face's own extent.
    const float ascent = font.ascent();
    const float descent = font.descent();
    const float baseline = lineTop * s + (lineHeight * s + ascent - descent) / 2;
    return font.draw(canvas, x * s, baseline, text, color, maxWidth * s) / s;
}

void WaylandChrome::drawButtons(ChromeCanvas& canvas, float s, const ChromeRect* buttons, float offsetX, float offsetY, bool hovered) {
    static const ChromeColor lit[3] = {
        ChromeColor{0xff / 255.0f, 0x5f / 255.0f, 0x57 / 255.0f, 1},
        ChromeColor{0xfe / 255.0f, 0xbc / 255.0f, 0x2e / 255.0f, 1},
        ChromeColor{0x28 / 255.0f, 0xc8 / 255.0f, 0x40 / 255.0f, 1},
    };
    const bool focused = window->state().focused;
    const ChromeColor dark{0.25f, 0.1f, 0.08f, 0.75f};
    for (int at = 0; at < 3; ++at) {
        const ChromeRect& button = buttons[at];
        const float cx = (button.x - offsetX + button.width / 2) * s;
        const float cy = (button.y - offsetY + button.height / 2) * s;
        const float r = button.width / 2 * s;
        // A window in the background keeps grey buttons, as a Mac's does,
        // until the pointer comes to them.
        const ChromeColor fill = focused || hovered ? lit[at] : colorOf(composer.vtConfig.config->fg, 0.22f);
        canvas.fillCircle(cx, cy, r, fill);
        canvas.strokeRoundedRect(cx - r, cy - r, 2 * r, 2 * r, r, 0.5f * s, ChromeColor{0, 0, 0, 0.18f});
        if (!hovered) {
            continue;
        }
        // The glyphs the pointer brings up: a cross, a bar, a plus.
        const float arm = r * 0.55f;
        const float w = 1.2f * s;
        if (at == 1 || at == 2) {
            canvas.fillRoundedRect(cx - arm, cy - w / 2, arm * 2, w, w / 2, dark);
        }
        if (at == 2) {
            canvas.fillRoundedRect(cx - w / 2, cy - arm, w, arm * 2, w / 2, dark);
        }
        if (at == 0) {
            for (int step = -8; step <= 8; ++step) {
                const float t = step / 8.0f * arm * 0.8f;
                canvas.fillCircle(cx + t, cy + t, w / 2, dark);
                canvas.fillCircle(cx + t, cy - t, w / 2, dark);
            }
        }
    }
}

void WaylandChrome::drawList(ChromeCanvas& canvas, float s, float left, float top, float width, float height, float listTop, const ChromeRect* buttons, float buttonsLeft, float buttonsTop) {
    const Color fg = composer.vtConfig.config->fg;
    const ChromeColor foreground = colorOf(fg);
    const ChromeColor idleText = colorOf(fg, 0.80f);
    const ChromeColor dimText = colorOf(fg, 0.52f);
    const ChromeColor tabInk = composer.opts->sidebarTabColorSet ? colorOf(composer.opts->sidebarTabColor) : foreground;
    const float tabAlpha = composer.opts->sidebarTabOpacity / 100.0f;
    const ChromeColor activeFill = withAlpha(tabInk, tabAlpha);
    const ChromeColor hoverFill = withAlpha(tabInk, tabAlpha / 2);
    const ChromeColor activeEdge = withAlpha(tabInk, 0.10f);
    const ChromeColor groupFill = withAlpha(tabInk, tabAlpha / 3);
    const ChromeColor groupEdge = withAlpha(tabInk, 0.12f);
    const float radius = 10;
    const float inset = (float)(SidebarMetrics::pillInset);
    const float textInset = (float)(SidebarMetrics::textInset);
    const float textLeft = textInset + (float)(SidebarMetrics::numberGutter);
    const float textRight = width - inset - 8;
    const bool overlayList = buttons == revealButtons;
    const u8 layer = overlayList ? 1 : 0;
    const auto rowTop = [&](size_t at) {
        return top + listTop + (float)(sidebarTabsRowOffset(heights.data(), heights.length(), at));
    };
    const auto rowHeight = [&](size_t at) {
        return at < heights.length() ? (float)(heights[at]) : (float)(SidebarMetrics::rowHeight);
    };
    const auto fits = [&](size_t at) {
        return rowTop(at) + rowHeight(at) <= top + height;
    };
    const bool hovering = hoverLayer == layer && hoverRow >= 0;
    // A folder's rows sit in from its header (SidebarMetrics::folderIndent).
    const auto indentOf = [&](size_t at) {
        return at < rows.length() ? (float)(sidebarTabsIndent(!rows[at].label && !rows[at].folder.empty())) : 0.0f;
    };
    // A folder's header and its rows as one span: the frame round the
    // folder that holds the active tab, and hover over an open header.
    const auto folderFrame = [&](size_t head, ChromeColor fill) {
        size_t last = head;
        while (last + 1 < rows.length() && !rows[last + 1].label && rows[last + 1].folder == rows[head].folder && fits(last + 1)) {
            ++last;
        }
        const float y0 = rowTop(head) + 1;
        const float y1 = rowTop(last) + rowHeight(last) - 1;
        canvas.fillRoundedRect((left + inset) * s, y0 * s, (width - 2 * inset) * s, (y1 - y0) * s, (radius + 3) * s, fill);
    };
    if (active < rows.length() && !rows[active].folder.empty()) {
        size_t head = active;
        while (head > 0 && !rows[head].label) {
            --head;
        }
        const bool hoveredHead = hovering && (size_t)(hoverRow) == head;
        if (rows[head].label && rows[head].folder == rows[active].folder && fits(head) && !hoveredHead) {
            folderFrame(head, groupFill);
        }
    }

    // Split groups: one frame round each split tab's rows, with its map.
    for (size_t first = 0; first < rows.length(); ++first) {
        if (!rows[first].groupFirst || !fits(first)) {
            continue;
        }
        size_t last = first;
        while (last + 1 < rows.length() && !rows[last].groupLast && fits(last + 1)) {
            ++last;
        }
        const float y0 = rowTop(first) + rowHeight(first) - (float)(SidebarMetrics::rowHeight) + 2;
        const float y1 = rowTop(last) + rowHeight(last) - 2;
        const float gx = left + indentOf(first) + inset - 3;
        const float gw = width - indentOf(first) - 2 * inset + 6;
        canvas.fillRoundedRect(gx * s, (y0 - 3) * s, gw * s, (y1 - y0 + 6) * s, (radius + 3) * s, groupFill);
        canvas.strokeRoundedRect(gx * s, (y0 - 3) * s, gw * s, (y1 - y0 + 6) * s, (radius + 3) * s, 1 * s, groupEdge);
        const float mapX = gx + gw - 3 - 7 - 22;
        const float mapY = y0 + 5;
        for (size_t at = first; at <= last; ++at) {
            const TabRow& cell = rows[at];
            const float alpha = !cell.focused ? 0.28f : cell.activeTab ? 0.85f : 0.55f;
            const float cw = cell.width * 22 - 2;
            const float ch = cell.height * 16 - 2;
            if (cw > 0 && ch > 0) {
                canvas.fillRoundedRect((mapX + cell.left * 22 + 1) * s, (mapY + cell.top * 16 + 1) * s, cw * s, ch * s, 2 * s, colorOf(fg, alpha));
            }
        }
    }

    for (size_t at = 0; at < rows.length(); ++at) {
        if (!fits(at)) {
            break;
        }
        const TabRow& row = rows[at];
        const RowText& line = texts[at];
        // A row that starts a section carries the gap above it in its
        // height; what it draws starts below the gap, the rule in its middle.
        const float lead = row.label ? 0.0f : rowHeight(at) - (float)(SidebarMetrics::rowHeight);
        const float y = rowTop(at) + lead;
        const float h = rowHeight(at) - lead;
        const bool isHovered = hovering && (size_t)(hoverRow) == at;
        if (row.label) {
            // The header, as the user's browser draws a folder: its glyph,
            // its name, the chevron after the name, the count at the
            // trailing edge. Hover lifts an open folder whole, a shut one
            // as its header.
            if (isHovered) {
                if (row.collapsed) {
                    canvas.fillRoundedRect((left + inset) * s, (y + 1) * s, (width - 2 * inset) * s, (h - 2) * s, radius * s, hoverFill);
                } else {
                    folderFrame(at, hoverFill);
                }
            }
            drawText(canvas, iconFont, s, left + textInset - 1, y, h, StringView(folderGlyph), idleText, 16);
            StringBuilder count;
            count << (u64)(row.members);
            const float countWidth = subFont.ready() ? subFont.measure(StringView(count)) / s : 0;
            float nameRight = left + textRight - countWidth - 6;
            drawText(canvas, subFont, s, left + textRight - countWidth, y, h, StringView(count), dimText, countWidth + 2);
            if (row.collapsed && row.activeInside) {
                canvas.fillCircle((nameRight - 3) * s, (y + h / 2) * s, 3 * s, colorOf(fg, 0.85f));
                nameRight -= 12;
            }
            const float nameLeft = left + textLeft;
            if (editing && editingFolder && row.folder == editFolder) {
                drawField(canvas, labelFont, s, nameLeft, y, h, nameRight - nameLeft);
                continue;
            }
            const StringView name = rowText(line.title, line.titleLength);
            // The chevron follows the name; a long name gives way to it.
            const float nameWidth = max(0.0f, min(labelFont.ready() ? labelFont.measure(name) / s : 0.0f, nameRight - 16 - nameLeft));
            drawText(canvas, labelFont, s, nameLeft, y, h, name, foreground, nameWidth);
            drawText(canvas, iconFont, s, nameLeft + nameWidth + 6, y, h, StringView(row.collapsed ? chevronRight : chevronDown), dimText, 12);
            continue;
        }
        const bool isActive = at == active;
        // The line before the tabs in no folder, like the one under the
        // bookmarks: what follows is loose.
        const bool afterFolders = row.folder.empty() && at > 0 && !rows[at - 1].folder.empty();
        if (row.afterBookmarks || afterFolders) {
            canvas.fillRoundedRect((left + textInset) * s, (y - lead / 2 - 0.5f) * s, (textRight - textInset) * s, 1 * s, 0, groupEdge);
        }
        // Everything on a row is placed from its own left edge: a folder's
        // rows sit in from their header.
        const float rowLeft = left + indentOf(at);
        const float rowWidth = width - indentOf(at);
        if (isActive || isHovered) {
            const float px = rowLeft + inset + 0.5f;
            const float py = y + 1 + 0.5f;
            const float pw = rowWidth - 2 * inset - 1;
            const float ph = h - 2 - 1;
            canvas.fillRoundedRect(px * s, py * s, pw * s, ph * s, radius * s, isActive ? activeFill : hoverFill);
            if (isActive) {
                canvas.strokeRoundedRect(px * s, py * s, pw * s, ph * s, radius * s, 1 * s, activeEdge);
            }
        }
        const bool bookmarkHead = row.bookmark != 0 && (!row.grouped || row.groupFirst);
        const Bookmark* const bookmark = bookmarkHead && composer.bookmarks != nullptr ? composer.bookmarks->find(row.bookmark) : nullptr;
        const bool runs = bookmark != nullptr && !bookmark->command.empty();
        const bool unreachable = bookmarkHead && (row.closed || row.exited) && composer.bookmarkProbe != nullptr && composer.bookmarkProbe->unreachable(row.bookmark);
        const bool dotShown = bookmarkHead && (row.exited || (runs && (!row.closed || unreachable)));
        if (bookmarkHead) {
            drawText(canvas, iconFont, s, rowLeft + textInset - 1, y, h, StringView(runs ? terminalGlyph : folderGlyph), row.closed ? dimText : idleText, 16);
            if (dotShown) {
                const float mapRoom = row.groupFirst ? 29 : 0;
                const float dx = left + textRight - mapRoom - 3.5f;
                const float dy = y + h / 2;
                if (unreachable) {
                    canvas.fillCircle(dx * s, dy * s, 3.5f * s, ChromeColor{0xb0 / 255.0f, 0x64 / 255.0f, 0x5e / 255.0f, 1});
                } else if (row.exited) {
                    canvas.strokeRoundedRect((dx - 3.5f) * s, (dy - 3.5f) * s, 7 * s, 7 * s, 3.5f * s, 1.5f * s, ChromeColor{0xe9 / 255.0f, 0xbd / 255.0f, 0x6e / 255.0f, 1});
                } else {
                    canvas.fillCircle(dx * s, dy * s, 3.5f * s, ChromeColor{0x7f / 255.0f, 0xe0 / 255.0f, 0xa8 / 255.0f, 1});
                }
            }
        } else if (row.bookmark == 0 && (!row.grouped || row.groupFirst) && row.tab < 9) {
            StringBuilder number;
            number << (u64)(row.tab + 1);
            drawText(canvas, titleFont, s, rowLeft + textInset, y, h, StringView(number), dimText, 14);
        }
        const float mapRoom = row.groupFirst ? 29 : 0;
        const float dotRoom = dotShown ? 13 : 0;
        const float rowText0 = rowLeft + textLeft - left;
        const ChromeColor titleColor = isActive ? foreground : row.closed ? dimText : idleText;
        if (editing && !editingFolder && (long long)(at) == editedRow()) {
            drawField(canvas, isActive ? activeFont : titleFont, s, left + rowText0, y + (float)(sidebarTabsLineTop(0)), (float)(sidebarTabsLineHeight(0)), textRight - rowText0 - mapRoom - dotRoom);
        } else {
            drawText(canvas, isActive ? activeFont : titleFont, s, left + rowText0, y + (float)(sidebarTabsLineTop(0)), (float)(sidebarTabsLineHeight(0)), rowText(line.title, line.titleLength), titleColor, textRight - rowText0 - mapRoom - dotRoom);
        }
        // The second line: a bookmark's status, or where the tab is - the
        // folder, then the branch after it, which keeps its width (up to
        // half the line) while the folder gives way.
        const float lineTop = y + (float)(sidebarTabsLineTop(1));
        const float lineHeight = (float)(sidebarTabsLineHeight(1));
        const float lineEnd = left + textRight;
        if (bookmarkHead) {
            if (line.folderLength != 0) {
                drawText(canvas, subFont, s, left + rowText0, lineTop, lineHeight, rowText(line.folder, line.folderLength), dimText, textRight - rowText0 - dotRoom);
            }
            continue;
        }
        const float icon = (float)(SidebarMetrics::iconColumn);
        const StringView branchText = rowText(line.branch, line.branchLength);
        float branchWidth = 0;
        if (line.branchLength != 0) {
            branchWidth = min((subFont.ready() ? subFont.measure(branchText) / s : 0.0f) + icon, (lineEnd - left - rowText0) / 2);
        }
        float x = left + rowText0;
        if (line.folderLength != 0) {
            const StringView folderText = rowText(line.folder, line.folderLength);
            drawText(canvas, iconFont, s, x, lineTop, lineHeight, StringView(folderGlyph), dimText, 16);
            x += icon;
            const float room = lineEnd - x - (branchWidth > 0 ? branchWidth + 8 : 0);
            const float width = min(subFont.ready() ? subFont.measure(folderText) / s : 0.0f, room);
            if (width > 0) {
                drawText(canvas, subFont, s, x, lineTop, lineHeight, folderText, dimText, width);
                x += width + 8;
            }
        }
        if (line.branchLength != 0 && lineEnd - x > icon) {
            drawText(canvas, iconFont, s, x, lineTop, lineHeight, StringView(branchGlyph), dimText, 16);
            x += icon;
            drawText(canvas, subFont, s, x, lineTop, lineHeight, branchText, dimText, lineEnd - x);
        }
    }

    // A row being dragged: the header it would join lit, or the line where
    // it would go, in from the edge as far as the rows of its folder.
    if (dragging && !overlayList) {
        if (dropOnLabel && dropIndex < rows.length() && fits(dropIndex)) {
            const float y = rowTop(dropIndex);
            canvas.fillRoundedRect((left + inset) * s, (y + 1) * s, (width - 2 * inset) * s, (rowHeight(dropIndex) - 2) * s, radius * s, activeFill);
            canvas.strokeRoundedRect((left + inset) * s, (y + 1) * s, (width - 2 * inset) * s, (rowHeight(dropIndex) - 2) * s, radius * s, 1.5f * s, colorOf(fg, 0.55f));
        } else if (!dropOnLabel) {
            StringView folder;
            size_t before = 0;
            sidebarDropDestination(rows, dropIndex, false, composer.sessions != nullptr ? composer.sessions->count() : 0, folder, before);
            const float lead = dropIndex < rows.length() && !rows[dropIndex].label ? rowHeight(dropIndex) - (float)(SidebarMetrics::rowHeight) : 0.0f;
            const float y = rowTop(dropIndex) + lead;
            const float x = left + inset + (float)(sidebarTabsIndent(!folder.empty()));
            if (y <= top + height) {
                canvas.fillCircle((x + 3) * s, y * s, 3 * s, colorOf(fg, 0.85f));
                canvas.fillRoundedRect((x + 3) * s, (y - 1) * s, (left + width - inset - x - 3) * s, 2 * s, 1 * s, colorOf(fg, 0.85f));
            }
        }
    }

    // The new-tab row under the last: a plus, centred.
    const size_t plusRow = rows.length();
    if (fits(plusRow)) {
        const float y = rowTop(plusRow);
        const float h = (float)(SidebarMetrics::rowHeight);
        if (hovering && (size_t)(hoverRow) == plusRow) {
            canvas.fillRoundedRect((left + inset) * s, (y + 1) * s, (width - 2 * inset) * s, (h - 2) * s, radius * s, hoverFill);
        }
        const float plusWidth = titleFont.ready() ? titleFont.measure(StringView(u8"+")) / s : 8;
        drawText(canvas, titleFont, s, left + (width - plusWidth) / 2, y, h, StringView(u8"+"), dimText, plusWidth + 4);
    }
    drawButtons(canvas, s, buttons, buttonsLeft - left, buttonsTop - top, hoverButtons && hoverLayer == layer);
}

void WaylandChrome::drawWindow(ChromeCanvas& canvas) {
    const float s = window->state().scale;
    const Color fg = composer.vtConfig.config->fg;
    const Color bg = composer.vtConfig.config->bg;
    const ChromeRect& w = layout.window;
    const float wr = layout.windowRadius;
    const float surfaceAlpha = composer.opts->sidebarOpacity / 100.0f < surfaceFloor ? surfaceFloor : composer.opts->sidebarOpacity / 100.0f;
    const ChromeColor surface = composer.opts->sidebarColorSet ? colorOf(composer.opts->sidebarColor, surfaceAlpha) : mix(bg, fg, 0.06f, surfaceAlpha);
    if (layout.margin > 0) {
        // The window's own shadow, the compositor giving none to a window
        // that draws itself; then the window cut out of it, so a
        // translucent surface does not show its own shadow through.
        canvas.shadow(w.x * s, (w.y + 4) * s, w.width * s, w.height * s, wr * s, (layout.margin - 2) * s, ChromeColor{0, 0, 0, 0.30f});
        canvas.punchRoundedRect(w.x * s, w.y * s, w.width * s, w.height * s, wr * s);
    }
    canvas.fillRoundedRect(w.x * s, w.y * s, w.width * s, w.height * s, wr * s, surface);
    if (layout.margin > 0) {
        canvas.strokeRoundedRect(w.x * s, w.y * s, w.width * s, w.height * s, wr * s, 1 * s, colorOf(fg, 0.10f));
    }

    // The panel: its shadow on the surface, the surface cut away under it,
    // its title bar, and the terminal below that in its own surface.
    const ChromeRect& p = layout.panel;
    const float pr = layout.panelRadius;
    const float bgAlpha = composer.opts->backgroundOpacity / 100.0f;
    canvas.shadow(p.x * s, (p.y + 3) * s, p.width * s, p.height * s, pr * s, 14 * s, ChromeColor{0, 0, 0, 0.28f});
    canvas.punchRoundedRect(p.x * s, p.y * s, p.width * s, p.height * s, pr * s);
    canvas.fillRoundedRect(p.x * s, p.y * s, p.width * s, (layout.header.height + pr) * s, pr * s, colorOf(bg, bgAlpha));
    const ChromeRect& c = layout.content;
    canvas.punchRoundedRect(c.x * s, c.y * s, c.width * s, c.height * s, pr * s);
    canvas.punchRoundedRect(c.x * s, c.y * s, c.width * s, pr * s, 0);
    canvas.strokeRoundedRect(p.x * s, p.y * s, p.width * s, p.height * s, pr * s, 1 * s, colorOf(fg, 0.08f));

    // The title bar: the button that puts the list away and brings it
    // back, and the active tab's title.
    const ChromeRect& t = layout.toggle;
    const ChromeColor ink = colorOf(fg, 0.75f);
    const float ix = (t.x + 7.5f) * s;
    const float iy = (t.y + 8.5f) * s;
    canvas.strokeRoundedRect(ix, iy, 13 * s, 11 * s, 2.5f * s, 1.2f * s, ink);
    canvas.fillRoundedRect(ix + 4.5f * s, iy, 1.2f * s, 11 * s, 0, ink);
    if (active < texts.length() && !rows[active].label) {
        const StringView title = rowText(texts[active].title, texts[active].titleLength);
        const float room = layout.header.width - 2 * 44;
        const float titleWidth = titleFont.ready() ? titleFont.measure(title) / s : 0;
        const float shown = titleWidth < room ? titleWidth : room;
        drawText(canvas, titleFont, s, layout.header.x + (layout.header.width - shown) / 2, layout.header.y, layout.header.height, title, colorOf(fg, 0.72f), room);
    }

    if (!layout.sidebar.empty()) {
        drawList(canvas, s, layout.sidebar.x, layout.sidebar.y, layout.sidebar.width, layout.sidebar.height, ChromeMetrics::listTop, layout.buttons, layout.sidebar.x, layout.sidebar.y);
    }
}

ChromeHit WaylandChrome::hitAt(u8 layer, float x, float y) const {
    if (layer == 1 && revealed) {
        return chromeRevealHitTest(revealList, revealButtons, x, y, heights.data(), heights.length());
    }
    return chromeHitTest(layout, x, y, heights.data(), heights.length());
}

void WaylandChrome::toggleList() {
    if (revealed) {
        reveal(false);
    }
    listShown = !listShown;
    relayout();
    redraw();
}

void WaylandChrome::reveal(bool shown) {
    if (shown == revealed || listShown) {
        return;
    }
    revealed = shown;
    window->setOverlay(shown, (i32)(revealList.x), (i32)(revealList.y), (u32)(revealList.width), (u32)(revealList.height));
    // Hidden, the window's own picture is committed again as well: a
    // compositor that tracks damage by surface would otherwise leave what
    // the list covered as it was.
    redraw();
}

void WaylandChrome::rowSelected(size_t row) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr || row >= rows.length()) {
        return;
    }
    if (rows[row].label) {
        // A folder's label shuts it or opens it.
        const StringView folder = rows[row].folder;
        Vector<StringView> kept;
        bool wasShut = false;
        for (const StringView name : collapsed) {
            if (name == folder) {
                wasShut = true;
            } else {
                kept.pushBack(name);
            }
        }
        if (!wasShut) {
            kept.pushBack(composer.pool->intern(folder));
        }
        collapsed.clear();
        for (const StringView name : kept) {
            collapsed.pushBack(name);
        }
        project();
        return;
    }
    if (rows[row].closed) {
        const Bookmark* const bookmark = composer.bookmarks != nullptr ? composer.bookmarks->find(rows[row].bookmark) : nullptr;
        if (bookmark != nullptr) {
            sessions->openBookmark(*bookmark);
        }
    } else {
        sessions->activatePane(rows[row].pane);
        if (rows[row].exited) {
            sessions->reconnect(rows[row].pane);
        }
    }
    composer.window->requestFrame();
}

void WaylandChrome::pressed(const ChromeHit& hit, u32 clicks) {
    switch (hit.kind) {
        case ChromeHitKind::Close:
            composer.window->requestClose();
            break;
        case ChromeHitKind::Minimize:
            composer.window->requestIconify();
            break;
        case ChromeHitKind::Maximize:
            composer.window->requestMaximized(!window->state().maximized);
            break;
        case ChromeHitKind::Toggle:
            toggleList();
            break;
        case ChromeHitKind::Row:
            if (clicks >= 2 && hit.row < rows.length() && rows[hit.row].label) {
                // The first click shut or opened the folder; the second puts
                // that back and names it, as on the Mac.
                const StringView folder = rows[hit.row].folder;
                rowSelected(hit.row);
                beginEditFolder(folder);
                break;
            }
            rowSelected(hit.row);
            if (revealed && !rows.empty() && hit.row < rows.length() && !rows[hit.row].label) {
                reveal(false);
            }
            break;
        case ChromeHitKind::NewTab:
            if (composer.sessions != nullptr) {
                composer.sessions->newSession();
                composer.window->requestFrame();
            }
            reveal(false);
            break;
        case ChromeHitKind::Move:
            // A double click where a Mac has its title bar zooms the window.
            if (clicks >= 2) {
                composer.window->requestMaximized(!window->state().maximized);
            } else {
                window->startMove();
            }
            break;
        case ChromeHitKind::Resize:
            window->startResize(hit.edges);
            break;
        case ChromeHitKind::Reveal:
        case ChromeHitKind::None:
            break;
    }
}

void WaylandChrome::chrome(const plt::ChromeEvent& event) {
    using Kind = plt::ChromeEvent::Kind;
    if (palette.shown() && event.kind != Kind::Changed) {
        palettePointer(event);
        return;
    }
    if (event.kind == Kind::Changed) {
        closeMenu();
        if (palette.shown()) {
            relayout();
            placePalette();
            redraw();
            return;
        }
        relayout();
        if (rows.empty()) {
            project();
        } else {
            redraw();
        }
        return;
    }
    if (menuShown && event.layer == 3 && event.kind == Kind::Leave) {
        if (menuHover >= 0) {
            menuHover = -1;
            drawMenu();
        }
        return;
    }
    if (event.kind == Kind::Leave) {
        const bool had = hoverRow >= 0 || hoverButtons;
        hoverRow = -1;
        hoverButtons = false;
        // Over the terminal, the list that came out at the edge goes back.
        if (event.layer == 2) {
            reveal(false);
        }
        if (had) {
            redraw();
        }
        return;
    }
    const ChromeHit hit = hitAt(event.layer, event.x, event.y);
    if (event.kind == Kind::Press) {
        if (menuShown) {
            // A pick on the menu; anywhere else, the menu goes and the
            // press with it, as a click away from a Mac menu does nothing.
            if (event.layer == 3 && event.button == 1) {
                const long long at = sidebarMenuItemAt(menuItems, event.y - menuRect.y);
                if (at >= 0) {
                    menuPicked((size_t)(at));
                }
            } else if (event.layer != 3) {
                closeMenu();
            }
            return;
        }
        if (editing) {
            // A click on the row being named leaves it be; anywhere else
            // keeps the name, then does what it would have done.
            const long long at = editedRow();
            if (event.layer != 2 && hit.kind == ChromeHitKind::Row && at >= 0 && (size_t)(at) == hit.row) {
                return;
            }
            commitEdit();
        }
        if (event.layer == 2) {
            return;
        }
        if (event.button == 3) {
            openMenu(event, hit);
            return;
        }
        if (event.button == 1) {
            if (hit.kind == ChromeHitKind::Row && event.layer == 0 && hit.row < rows.length() && !rows[hit.row].label) {
                pressing = true;
                dragging = false;
                pressModel = rows[hit.row];
                pressX = event.x;
                pressY = event.y;
            }
            pressed(hit, event.clicks);
        }
        return;
    }
    if (event.kind == Kind::Release) {
        if (event.button == 1 && pressing) {
            const bool dropped = dragging;
            pressing = false;
            dragging = false;
            SessionSet* const sessions = composer.sessions;
            if (dropped && sessions != nullptr) {
                StringView folder;
                size_t before = 0;
                sidebarDropDestination(rows, dropIndex, dropOnLabel, sessions->count(), folder, before);
                sidebarDropRow(composer, pressModel, folder, before);
                composer.window->requestFrame();
            }
            if (dropped) {
                project();
            }
        }
        return;
    }
    if (event.kind != Kind::Motion && event.kind != Kind::Enter) {
        return;
    }
    if (menuShown) {
        const long long at = event.layer == 3 ? sidebarMenuItemAt(menuItems, event.y - menuRect.y) : -1;
        if (event.layer == 3) {
            window->setCursor(plt::PointerIcon::Default);
        }
        if (at != menuHover) {
            menuHover = at;
            drawMenu();
        }
        return;
    }
    if (pressing && event.kind == Kind::Motion) {
        // A press that moves 4 points is a drag; less, still a click.
        const float dx = event.x - pressX;
        const float dy = event.y - pressY;
        if (!dragging && dx * dx + dy * dy >= 16) {
            dragging = true;
        }
        if (dragging) {
            dragTo(event.x, event.y);
            redraw();
            return;
        }
    }
    if (hit.kind == ChromeHitKind::Reveal) {
        reveal(true);
    } else if (revealed && event.layer == 0) {
        // On the window's surface away from the edge - the title bar, the
        // gap round the panel - it goes too.
        reveal(false);
    }
    const long long row = hit.kind == ChromeHitKind::Row ? (long long)(hit.row) : hit.kind == ChromeHitKind::NewTab ? (long long)(rows.length()) : -1;
    const bool buttons = hit.kind == ChromeHitKind::Close || hit.kind == ChromeHitKind::Minimize || hit.kind == ChromeHitKind::Maximize;
    plt::PointerIcon icon = plt::PointerIcon::Default;
    if (hit.kind == ChromeHitKind::Resize) {
        const bool top = (hit.edges & plt::ChromeEdgeTop) != 0;
        const bool bottom = (hit.edges & plt::ChromeEdgeBottom) != 0;
        const bool left = (hit.edges & plt::ChromeEdgeLeft) != 0;
        const bool right = (hit.edges & plt::ChromeEdgeRight) != 0;
        icon = (top && left) || (bottom && right) ? plt::PointerIcon::ResizeNorthWestSouthEast
            : (top && right) || (bottom && left)  ? plt::PointerIcon::ResizeNorthEastSouthWest
            : top || bottom                       ? plt::PointerIcon::ResizeNorthSouth
                                                  : plt::PointerIcon::ResizeEastWest;
    }
    window->setCursor(icon);
    if (row != hoverRow || buttons != hoverButtons || event.layer != hoverLayer) {
        hoverRow = row;
        hoverButtons = buttons;
        hoverLayer = event.layer;
        redraw();
    }
}

bool WaylandChrome::capturesKeys() {
    return menuShown || editing || palette.shown();
}

void WaylandChrome::chromeKey(const plt::KeyInput& key) {
    if (palette.shown()) {
        paletteKey(key);
        return;
    }
    if (menuShown) {
        if (key.key == plt::InputKey::Escape) {
            closeMenu();
        } else if (key.key == plt::InputKey::Enter && menuHover >= 0) {
            menuPicked((size_t)(menuHover));
        } else if (key.key == plt::InputKey::Up || key.key == plt::InputKey::Down) {
            // The next item that can be picked, round the ends.
            const long long count = (long long)(menuItems.length());
            const long long step = key.key == plt::InputKey::Down ? 1 : -1;
            long long at = menuHover;
            for (long long tried = 0; tried < count; ++tried) {
                at = at < 0 ? (step > 0 ? 0 : count - 1) : (at + step + count) % count;
                if (menuItems[(size_t)(at)].pickable()) {
                    menuHover = at;
                    drawMenu();
                    break;
                }
            }
        }
        return;
    }
    if (!editing) {
        return;
    }
    switch (field.key(key)) {
        case SidebarField::Outcome::Commit:
            commitEdit();
            break;
        case SidebarField::Outcome::Cancel:
            cancelEdit();
            break;
        case SidebarField::Outcome::Edited:
            redraw();
            break;
        case SidebarField::Outcome::Ignored:
            break;
    }
}

void WaylandChrome::chromeText(u32 codepoint) {
    if (palette.shown()) {
        palette.field.insert(codepoint);
        palette.textChanged();
        return;
    }
    if (editing && !menuShown) {
        field.insert(codepoint);
        redraw();
    }
}

float WaylandChrome::listRowTop(u8 layer, size_t at) const {
    const float listTop = layer == 1 ? revealList.y + ChromeMetrics::listTop - ChromeMetrics::buttonTop + (revealButtons[0].y - revealList.y) : layout.sidebar.y + ChromeMetrics::listTop;
    return listTop + (float)(sidebarTabsRowOffset(heights.data(), heights.length(), at));
}

void WaylandChrome::openMenu(const plt::ChromeEvent& event, const ChromeHit& hit) {
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr) {
        return;
    }
    const bool inList = event.layer == 1 ? revealList.contains(event.x, event.y) : layout.sidebar.contains(event.x, event.y);
    const TabRow* row = nullptr;
    if (hit.kind == ChromeHitKind::Row && hit.row < rows.length()) {
        row = &rows[hit.row];
    } else if (!(hit.kind == ChromeHitKind::NewTab || (hit.kind == ChromeHitKind::Move && inList))) {
        return;
    }
    menuHasRow = row != nullptr;
    menuRow = row != nullptr ? *row : TabRow();
    Vector<StringView> folders;
    sessions->folders(folders);
    const bool pinned = row != nullptr && row->bookmark != 0 && composer.bookmarks != nullptr && composer.bookmarks->find(row->bookmark) != nullptr;
    sidebarMenuItems(row, folders, pinned, menuItems);
    const float s = window->state().scale;
    float width = SidebarMenuMetrics::minimumWidth;
    for (const SidebarMenuItem& item : menuItems) {
        UiText& font = item.action == SidebarMenuAction::Heading ? subFont : titleFont;
        const float need = (font.ready() ? font.measure(item.label) / s : 0) + 2 * SidebarMenuMetrics::textInset + (item.indented ? SidebarMenuMetrics::indent : 0);
        width = max(width, need);
    }
    const float height = sidebarMenuHeight(menuItems);
    float x = 0;
    float y = 0;
    const ChromeRect& bounds = layout.window;
    sidebarMenuPlace(event.x + 2, event.y + 2, width, height, bounds.x, bounds.y, bounds.width, bounds.height, x, y);
    menuRect = ChromeRect{floorf(x), floorf(y), ceilf(width), ceilf(height)};
    menuShown = true;
    menuHover = -1;
    window->setMenu(true, (i32)(menuRect.x), (i32)(menuRect.y), (u32)(menuRect.width), (u32)(menuRect.height));
    drawMenu();
}

void WaylandChrome::closeMenu() {
    if (!menuShown) {
        return;
    }
    menuShown = false;
    menuHover = -1;
    window->setMenu(false, (i32)(menuRect.x), (i32)(menuRect.y), (u32)(menuRect.width), (u32)(menuRect.height));
}

void WaylandChrome::drawMenu() {
    if (!menuShown) {
        return;
    }
    const float s = window->state().scale;
    const u32 width = (u32)(ceilf(menuRect.width * s));
    const u32 height = (u32)(ceilf(menuRect.height * s));
    if (menuCanvas.width() != width || menuCanvas.height() != height) {
        menuCanvas.resize(width, height);
    } else {
        menuCanvas.clear();
    }
    const Color fg = composer.vtConfig.config->fg;
    const Color bg = composer.vtConfig.config->bg;
    const ChromeColor surface = composer.opts->sidebarColorSet ? colorOf(composer.opts->sidebarColor, 0.98f) : mix(bg, fg, 0.08f, 0.98f);
    const float radius = SidebarMenuMetrics::radius;
    menuCanvas.fillRoundedRect(0, 0, (float)(width), (float)(height), radius * s, surface);
    menuCanvas.strokeRoundedRect(0, 0, (float)(width), (float)(height), radius * s, 1 * s, colorOf(fg, 0.16f));
    const float pad = SidebarMenuMetrics::pad;
    const float inset = SidebarMenuMetrics::textInset;
    const ChromeColor tabInk = composer.opts->sidebarTabColorSet ? colorOf(composer.opts->sidebarTabColor) : colorOf(fg);
    const ChromeColor hoverFill = withAlpha(tabInk, max(0.14f, composer.opts->sidebarTabOpacity / 100.0f));
    for (size_t at = 0; at < menuItems.length(); ++at) {
        const SidebarMenuItem& item = menuItems[at];
        const float top = sidebarMenuItemTop(menuItems, at);
        const float itemHeight = sidebarMenuItemHeight(item);
        if (item.action == SidebarMenuAction::Separator) {
            menuCanvas.fillRoundedRect((pad + 6) * s, (top + itemHeight / 2 - 0.5f) * s, (menuRect.width - 2 * pad - 12) * s, 1 * s, 0, colorOf(fg, 0.12f));
            continue;
        }
        if (item.action == SidebarMenuAction::Heading) {
            drawText(menuCanvas, subFont, s, inset, top, itemHeight, item.label, colorOf(fg, 0.52f), menuRect.width - 2 * inset);
            continue;
        }
        if ((long long)(at) == menuHover) {
            menuCanvas.fillRoundedRect(pad * s, top * s, (menuRect.width - 2 * pad) * s, itemHeight * s, 5 * s, hoverFill);
        }
        const float x = inset + (item.indented ? SidebarMenuMetrics::indent : 0);
        if (item.checked) {
            menuCanvas.fillCircle((x - 7) * s, (top + itemHeight / 2) * s, 2.5f * s, colorOf(fg, 0.7f));
        }
        drawText(menuCanvas, titleFont, s, x, top, itemHeight, item.label, item.enabled ? colorOf(fg) : colorOf(fg, 0.4f), menuRect.width - x - inset);
    }
    window->present(3, menuCanvas.data(), menuCanvas.width(), menuCanvas.height());
}

void WaylandChrome::menuPicked(size_t index) {
    if (index >= menuItems.length() || !menuItems[index].pickable()) {
        return;
    }
    const SidebarMenuItem item = menuItems[index];
    const TabRow row = menuRow;
    const bool hasRow = menuHasRow;
    closeMenu();
    SessionSet* const sessions = composer.sessions;
    if (sessions == nullptr) {
        return;
    }
    switch (item.action) {
        case SidebarMenuAction::MoveToFolder:
            if (hasRow) {
                sidebarDropRow(composer, row, item.folder, sessions->count());
            }
            break;
        case SidebarMenuAction::MoveToNewFolder:
            if (hasRow) {
                const StringView folder = sidebarCreateFolder(composer);
                if (!folder.empty()) {
                    sidebarDropRow(composer, row, folder, sessions->count());
                    project();
                    beginEditFolder(folder);
                }
            }
            break;
        case SidebarMenuAction::RemoveFromFolder:
            if (hasRow) {
                sidebarDropRow(composer, row, StringView(), sessions->count());
            }
            break;
        case SidebarMenuAction::RenameRow:
            if (hasRow) {
                beginEditRow(row);
            }
            break;
        case SidebarMenuAction::Pin:
            if (hasRow) {
                sidebarPinRow(composer, row);
            }
            break;
        case SidebarMenuAction::CloseTab:
            if (hasRow && !sidebarCloseRow(composer, row)) {
                composer.window->requestClose();
                return;
            }
            break;
        case SidebarMenuAction::ToggleFolder:
            sidebarToggleFolder(composer, collapsed, row.folder);
            break;
        case SidebarMenuAction::RenameFolder:
            beginEditFolder(row.folder);
            break;
        case SidebarMenuAction::DeleteFolder:
        case SidebarMenuAction::DeleteFolderAndCloseTabs:
            if (!sidebarDeleteFolder(composer, collapsed, row.folder, item.action == SidebarMenuAction::DeleteFolderAndCloseTabs)) {
                composer.window->requestClose();
                return;
            }
            break;
        case SidebarMenuAction::NewTab:
            sessions->newSession();
            break;
        case SidebarMenuAction::NewFolder: {
            const StringView folder = sidebarCreateFolder(composer);
            if (!folder.empty()) {
                project();
                beginEditFolder(folder);
            }
            break;
        }
        case SidebarMenuAction::Separator:
        case SidebarMenuAction::Heading:
            break;
    }
    project();
    composer.window->requestFrame();
}

void WaylandChrome::beginEditFolder(StringView folder) {
    if (folder.empty()) {
        return;
    }
    editFolder = composer.pool->intern(folder);
    editingFolder = true;
    editing = true;
    field.begin(editFolder);
    redraw();
}

void WaylandChrome::beginEditRow(const TabRow& row) {
    if (row.label) {
        return;
    }
    editRow = row;
    editingFolder = false;
    editing = true;
    field.begin(sidebarRowTitle(composer, row));
    redraw();
}

long long WaylandChrome::editedRow() const {
    if (!editing) {
        return -1;
    }
    for (size_t at = 0; at < rows.length(); ++at) {
        const TabRow& row = rows[at];
        if (editingFolder) {
            if (row.label && row.folder == editFolder) {
                return (long long)(at);
            }
        } else if (!row.label && (!row.grouped || row.groupFirst)) {
            if (editRow.bookmark != 0 ? row.bookmark == editRow.bookmark : (!row.closed && row.pane == editRow.pane)) {
                return (long long)(at);
            }
        }
    }
    return -1;
}

void WaylandChrome::commitEdit() {
    if (!editing) {
        return;
    }
    editing = false;
    StringView name = field.text();
    while (!name.empty() && (name.data()[0] == ' ' || name.data()[0] == '\t')) {
        name = StringView(name.data() + 1, name.length() - 1);
    }
    while (!name.empty() && (name.data()[name.length() - 1] == ' ' || name.data()[name.length() - 1] == '\t')) {
        name = StringView(name.data(), name.length() - 1);
    }
    if (editingFolder) {
        sidebarRenameFolder(composer, collapsed, editFolder, name);
    } else {
        sidebarRenameRow(composer, editRow, name);
    }
    project();
    composer.window->requestFrame();
}

void WaylandChrome::cancelEdit() {
    editing = false;
    redraw();
}

void WaylandChrome::drawField(ChromeCanvas& canvas, UiText& font, float s, float x, float lineTop, float lineHeight, float maxWidth) {
    const Color fg = composer.vtConfig.config->fg;
    const Color bg = composer.vtConfig.config->bg;
    const float pad = 3;
    const auto widthOf = [&](size_t from, size_t to) {
        StringBuilder part;
        field.utf8(from, to, part);
        return font.ready() ? font.measure(StringView(part)) / s : 0.0f;
    };
    canvas.fillRoundedRect((x - pad) * s, (lineTop + 1) * s, (maxWidth + 2 * pad) * s, (lineHeight - 2) * s, 4 * s, colorOf(bg, 0.92f));
    canvas.strokeRoundedRect((x - pad) * s, (lineTop + 1) * s, (maxWidth + 2 * pad) * s, (lineHeight - 2) * s, 4 * s, 1 * s, colorOf(fg, 0.45f));
    const size_t low = field.caret() < field.anchor() ? field.caret() : field.anchor();
    const size_t high = field.caret() < field.anchor() ? field.anchor() : field.caret();
    if (field.selected()) {
        const float x0 = min(widthOf(0, low), maxWidth);
        const float x1 = min(widthOf(0, high), maxWidth);
        canvas.fillRoundedRect((x + x0) * s, (lineTop + 3) * s, (x1 - x0) * s, (lineHeight - 6) * s, 2 * s, colorOf(fg, 0.28f));
    }
    drawText(canvas, font, s, x, lineTop, lineHeight, field.text(), colorOf(fg), maxWidth);
    if (!field.selected()) {
        const float caretX = min(widthOf(0, field.caret()), maxWidth);
        canvas.fillRoundedRect((x + caretX) * s, (lineTop + 3) * s, 1.2f * s, (lineHeight - 6) * s, 0, colorOf(fg));
    }
}

void WaylandChrome::dragTo(float x, float y) {
    const ChromeHit hit = hitAt(0, x, y);
    dropOnLabel = false;
    if (hit.kind == ChromeHitKind::Row && hit.row < rows.length()) {
        if (rows[hit.row].label) {
            dropOnLabel = true;
            dropIndex = hit.row;
            return;
        }
        const float top = listRowTop(0, hit.row);
        const float middle = top + (float)(heights[hit.row]) / 2;
        dropIndex = y < middle ? hit.row : hit.row + 1;
        return;
    }
    dropIndex = y < listRowTop(0, 0) ? 0 : rows.length();
}

void CallCommandPalette::onListen(void*) {
    if (owner->palette.shown()) {
        owner->closePalette();
    } else {
        owner->openPalette();
    }
}

namespace {
    constexpr const char* searchGlyph = "\xef\x80\x82";
    constexpr const char* serverGlyph = "\xef\x88\xb3";
    constexpr const char* shieldGlyph = "\xef\x84\xb2";
    constexpr const char* starGlyph = "\xef\x80\x85";
    constexpr const char* appGlyph = "\xef\x80\x89";
    constexpr const char* keyGlyph = "\xef\x82\x84";
    constexpr const char* boltGlyph = "\xef\x83\xa7";
    // The highlight of matched letters, and of the mode chip in use.
    constexpr ChromeColor matchInk{0xf0 / 255.0f, 0xc6 / 255.0f, 0x74 / 255.0f, 1};

    const char* kindGlyph(const PaletteItem& item) {
        switch (item.kind) {
            case PaletteKind::Bookmark:
                return item.command.empty() ? folderGlyph : terminalGlyph;
            case PaletteKind::SshHost:
                return serverGlyph;
            case PaletteKind::TeleportHost:
                return shieldGlyph;
            case PaletteKind::Folder:
                return folderGlyph;
            case PaletteKind::App:
                return appGlyph;
            case PaletteKind::Env:
                return keyGlyph;
            case PaletteKind::Action:
                return item.action == PaletteAction::CloneUrl || item.action == PaletteAction::Clone ? branchGlyph : boltGlyph;
        }
        return boltGlyph;
    }
}

void WaylandChrome::paletteChanged() {
    if (!palette.shown()) {
        return;
    }
    const ChromeRect list = paletteListRect();
    paletteScroll = paletteScrollTo(palette.rows(), palette.selected(), paletteScroll, list.height);
    drawPalette();
}

void WaylandChrome::openPalette() {
    if (palette.shown()) {
        return;
    }
    closeMenu();
    if (editing) {
        commitEdit();
    }
    paletteScroll = 0;
    placePalette();
    palette.open();
}

void WaylandChrome::closePalette() {
    if (!palette.shown()) {
        return;
    }
    palette.close();
    window->setMenu(false, (i32)(paletteRect.x), (i32)(paletteRect.y), (u32)(paletteRect.width), (u32)(paletteRect.height));
}

void WaylandChrome::placePalette() {
    const ChromeRect& panel = layout.panel.empty() ? layout.window : layout.panel;
    const float width = min(PaletteMetrics::width, max(320.0f, panel.width - 32));
    const float wanted = PaletteMetrics::input + PaletteMetrics::chips + PaletteMetrics::list + PaletteMetrics::footer;
    const float height = min(wanted, max(200.0f, panel.height - PaletteMetrics::top - 16));
    paletteRect = ChromeRect{floorf(panel.x + (panel.width - width) / 2), floorf(panel.y + min(PaletteMetrics::top, max(8.0f, panel.height - height - 8))), ceilf(width), ceilf(height)};
    window->setMenu(true, (i32)(paletteRect.x), (i32)(paletteRect.y), (u32)(paletteRect.width), (u32)(paletteRect.height));
}

ChromeRect WaylandChrome::paletteListRect() const {
    // In the palette's own coordinates.
    const float top = PaletteMetrics::input + PaletteMetrics::chips;
    const float detail = paletteRect.width >= 600 ? PaletteMetrics::detail : 0;
    return ChromeRect{0, top, paletteRect.width - detail, paletteRect.height - top - PaletteMetrics::footer};
}

void WaylandChrome::drawPalette() {
    const float s = window->state().scale;
    openFonts(s);
    const u32 width = (u32)(ceilf(paletteRect.width * s));
    const u32 height = (u32)(ceilf(paletteRect.height * s));
    if (paletteCanvas.width() != width || paletteCanvas.height() != height) {
        paletteCanvas.resize(width, height);
    } else {
        paletteCanvas.clear();
    }
    ChromeCanvas& c = paletteCanvas;
    const Color fg = composer.vtConfig.config->fg;
    const Color bg = composer.vtConfig.config->bg;
    const ChromeColor ink = colorOf(fg);
    const ChromeColor dim = colorOf(fg, 0.58f);
    const ChromeColor faint = colorOf(fg, 0.38f);
    const ChromeColor rule = colorOf(fg, 0.08f);
    const float w = paletteRect.width;
    const float h = paletteRect.height;
    c.fillRoundedRect(0, 0, w * s, h * s, PaletteMetrics::radius * s, mix(bg, fg, 0.07f, 0.985f));
    c.strokeRoundedRect(0, 0, w * s, h * s, PaletteMetrics::radius * s, 1 * s, colorOf(fg, 0.14f));

    // The field: the search glyph, the text, the caret.
    drawText(c, iconFont, s, 16, 0, PaletteMetrics::input, StringView(searchGlyph), dim, 18);
    const StringView typed = palette.field.text();
    const float textLeft = 42;
    if (typed.empty()) {
        drawText(c, titleFont, s, textLeft, 0, PaletteMetrics::input, StringView(u8"Host, folder, app, action…  @ / > ! $"), faint, w - textLeft - 16);
    } else {
        drawText(c, titleFont, s, textLeft, 0, PaletteMetrics::input, typed, ink, w - textLeft - 16);
    }
    {
        StringBuilder before;
        palette.field.utf8(0, palette.field.caret(), before);
        const float caretX = textLeft + (titleFont.ready() ? titleFont.measure(StringView(before)) / s : 0);
        c.fillRoundedRect(caretX * s, (PaletteMetrics::input / 2 - 9) * s, 1.5f * s, 18 * s, 0, ink);
    }
    c.fillRoundedRect(0, (PaletteMetrics::input - 0.5f) * s, w * s, 1 * s, 0, rule);

    // The mode chips.
    const PaletteMode mode = palette.mode();
    const PaletteMode modes[] = {PaletteMode::All, PaletteMode::Hosts, PaletteMode::Folders, PaletteMode::Actions, PaletteMode::Apps, PaletteMode::Env};
    float chipX = 12;
    for (const PaletteMode one : modes) {
        StringBuilder label;
        const StringView prefix = palettePrefix(one);
        if (!prefix.empty()) {
            label << prefix << StringView(u8" ");
        }
        label << paletteModeName(one);
        const float textWidth = subFont.ready() ? subFont.measure(StringView(label)) / s : 40;
        const float chipW = textWidth + 18;
        const float chipY = PaletteMetrics::input + 6;
        const bool on = one == mode;
        c.fillRoundedRect(chipX * s, chipY * s, chipW * s, 20 * s, 10 * s, on ? withAlpha(matchInk, 0.16f) : colorOf(fg, 0.06f));
        drawText(c, subFont, s, chipX + 9, chipY, 20, StringView(label), on ? matchInk : dim, textWidth + 2);
        chipX += chipW + 6;
    }

    // The list, clipped to its box by drawing only the rows inside it.
    const ChromeRect list = paletteListRect();
    const Vector<PaletteRow>& rows = palette.rows();
    for (size_t at = 0; at < rows.length(); ++at) {
        const PaletteRow& row = rows[at];
        const float rowH = row.heading ? PaletteMetrics::heading : PaletteMetrics::row;
        const float y = list.y + paletteRowTop(rows, at) - paletteScroll;
        if (y < list.y || y + rowH > list.y + list.height) {
            continue;
        }
        if (row.heading) {
            drawText(c, subFont, s, 16, y + 4, rowH - 4, row.title, faint, list.width - 32);
            continue;
        }
        const PaletteItem* const item = palette.item(at);
        if (item == nullptr) {
            continue;
        }
        if (at == palette.selected()) {
            c.fillRoundedRect((list.x + 6) * s, y * s, (list.width - 12) * s, rowH * s, 8 * s, colorOf(fg, 0.12f));
        }
        drawText(c, iconFont, s, 16, y, rowH, StringView(kindGlyph(*item)), item->kind == PaletteKind::TeleportHost ? ChromeColor{0xa9 / 255.0f, 0xc1 / 255.0f, 1, 1} : dim, 18);
        float right = list.width - 14;
        if (!item->badge.empty()) {
            const float badgeW = (subFont.ready() ? subFont.measure(item->badge) / s : 40) + 12;
            right -= badgeW;
            const ChromeColor tone = item->badge == StringView(u8"Teleport") ? ChromeColor{0x7a / 255.0f, 0xa2 / 255.0f, 0xf7 / 255.0f, 0.18f} : item->badge == StringView(u8"current") ? ChromeColor{0x7f / 255.0f, 0xe0 / 255.0f, 0xa8 / 255.0f, 0.18f} : colorOf(fg, 0.08f);
            c.fillRoundedRect(right * s, (y + rowH / 2 - 9) * s, badgeW * s, 18 * s, 9 * s, tone);
            drawText(c, subFont, s, right + 6, y + rowH / 2 - 9, 18, item->badge, dim, badgeW);
            right -= 8;
        }
        // The title in runs: matched letters in their own ink.
        const float titleLeft = 42;
        const float titleTop = y + 3;
        const float lineH = 16;
        float x = titleLeft;
        size_t start = 0;
        const StringView title = item->title;
        size_t mark = 0;
        while (start < title.length() && x < right) {
            const bool marked = mark < row.markCount && row.marks[mark] == start;
            size_t end = start + 1;
            if (marked) {
                ++mark;
                while (end < title.length() && (((u8)(title.data()[end])) & 0xC0) == 0x80) {
                    ++end;
                }
            } else {
                while (end < title.length() && !(mark < row.markCount && row.marks[mark] == end)) {
                    ++end;
                }
            }
            const StringView run(title.data() + start, end - start);
            UiText& font = marked ? activeFont : titleFont;
            x += drawText(c, font, s, x, titleTop, lineH, run, marked ? matchInk : ink, right - x);
            start = end;
        }
        if (item->star && x + 14 < right) {
            drawText(c, iconFont, s, x + 5, titleTop, lineH, StringView(starGlyph), matchInk, 12);
        }
        drawText(c, subFont, s, titleLeft, y + 19, 14, item->subtitle, dim, right - titleLeft);
    }

    // An empty list says where its rows come from.
    PaletteHint hint;
    if (palette.hint(hint)) {
        const auto centred = [&](UiText& font, float top, float height, StringView text, ChromeColor color) {
            const float width = min(font.ready() ? font.measure(text) / s : 0.0f, list.width - 32);
            drawText(c, font, s, list.x + (list.width - width) / 2, top, height, text, color, width + 1);
        };
        float y = list.y + 40;
        centred(activeFont, y, 18, StringView(hint.title), ink);
        y += 26;
        for (const StringBuilder& line : hint.lines) {
            if (!StringView(line).empty()) {
                centred(subFont, y, 16, StringView(line), dim);
                y += 18;
            }
        }
        if (!hint.example[0].empty()) {
            float widest = 0;
            for (const StringView line : hint.example) {
                widest = max(widest, iconFont.ready() ? iconFont.measure(line) / s : 0.0f);
            }
            const float bx = list.x + (list.width - widest) / 2 - 14;
            y += 12;
            c.fillRoundedRect(bx * s, y * s, (widest + 28) * s, (3 * 18 + 16) * s, 8 * s, colorOf(fg, 0.06f));
            for (const StringView line : hint.example) {
                drawText(c, iconFont, s, bx + 14, y + 8, 18, line, dim, widest + 1);
                y += 18;
            }
        }
    }

    // The detail of the picked row.
    if (list.width < w) {
        const float dx = list.width;
        c.fillRoundedRect(dx * s, list.y * s, 1 * s, list.height * s, 0, rule);
        if (const PaletteItem* const item = palette.item(palette.selected())) {
            const float px = dx + 16;
            const float pw = w - px - 14;
            drawText(c, iconFont, s, px, list.y + 14, 28, StringView(kindGlyph(*item)), ink, 24);
            drawText(c, activeFont, s, px + 30, list.y + 12, 18, item->title, ink, pw - 30);
            drawText(c, subFont, s, px + 30, list.y + 30, 14, item->badge.empty() ? item->subtitle : item->badge, dim, pw - 30);
            float y = list.y + 62;
            for (int at = 0; at < 4; ++at) {
                if (item->detailLabels[at].empty()) {
                    continue;
                }
                drawText(c, subFont, s, px, y, 18, item->detailLabels[at], dim, 64);
                drawText(c, subFont, s, px + 68, y, 18, item->detailValues[at], ink, pw - 68);
                y += 22;
            }
        }
    }

    // The keys.
    const float fy = h - PaletteMetrics::footer;
    c.fillRoundedRect(0, fy * s, w * s, 1 * s, 0, rule);
    struct Hint {
        const char* key;
        const char* what;
    };
    const Hint hints[] = {{"\xe2\x86\xb5", "new tab"}, {"Alt+\xe2\x86\xb5", "this tab"}, {"Ctrl+B", "bookmark"}, {"Tab", "complete"}, {"Esc", ""}};
    float hx = 14;
    for (const Hint& hint : hints) {
        const StringView key(hint.key);
        const float kw = (subFont.ready() ? subFont.measure(key) / s : 20) + 10;
        c.fillRoundedRect(hx * s, (fy + 8) * s, kw * s, 16 * s, 4 * s, colorOf(fg, 0.08f));
        drawText(c, subFont, s, hx + 5, fy + 8, 16, key, dim, kw);
        hx += kw + 5;
        const StringView what(hint.what);
        if (!what.empty()) {
            hx += drawText(c, subFont, s, hx, fy + 8, 16, what, dim, w - hx) + 12;
        }
    }
    window->present(3, c.data(), c.width(), c.height());
}

void WaylandChrome::paletteKey(const plt::KeyInput& key) {
    const bool ctrl = (key.modifiers & plt::InputControl) != 0;
    const bool alt = (key.modifiers & plt::InputAlt) != 0;
    switch (key.key) {
        case plt::InputKey::Escape:
            closePalette();
            return;
        case plt::InputKey::Up:
            palette.move(-1);
            return;
        case plt::InputKey::Down:
            palette.move(1);
            return;
        case plt::InputKey::Tab:
            palette.complete();
            return;
        case plt::InputKey::Enter:
            if (palette.pick(alt ? PaletteTarget::CurrentTab : PaletteTarget::NewTab)) {
                closePalette();
                composer.window->requestFrame();
            }
            return;
        default:
            break;
    }
    if (ctrl && (key.baseCodepoint == 'b' || key.layoutCodepoint == 'b')) {
        palette.bookmarkPicked();
        return;
    }
    if (ctrl && (key.baseCodepoint == 'k' || key.layoutCodepoint == 'k' || key.baseCodepoint == 'K')) {
        // The chord again closes it, as a toggle.
        closePalette();
        return;
    }
    StringBuilder before;
    before << palette.field.text();
    const SidebarField::Outcome outcome = palette.field.key(key);
    if (outcome == SidebarField::Outcome::Edited) {
        if (palette.field.text() != StringView(before)) {
            palette.textChanged();
        } else {
            drawPalette();
        }
    }
}

void WaylandChrome::palettePointer(const plt::ChromeEvent& event) {
    using Kind = plt::ChromeEvent::Kind;
    if (event.layer != 3) {
        // A press anywhere else - the list, the title bar, the terminal -
        // puts it away, as a click away from a Mac's palette does.
        if (event.kind == Kind::Press) {
            closePalette();
        }
        return;
    }
    const ChromeRect list = paletteListRect();
    const float x = event.x - paletteRect.x;
    const float y = event.y - paletteRect.y;
    if (x < list.x || x >= list.x + list.width || y < list.y || y >= list.y + list.height) {
        return;
    }
    const long long row = paletteRowAt(palette.rows(), y - list.y, paletteScroll);
    if (row < 0) {
        return;
    }
    if (event.kind == Kind::Motion && (size_t)(row) != palette.selected()) {
        palette.select((size_t)(row));
    } else if (event.kind == Kind::Press && event.button == 1) {
        palette.select((size_t)(row));
        if (palette.pick(PaletteTarget::NewTab)) {
            closePalette();
            composer.window->requestFrame();
        }
    }
}

void createWaylandChrome(ObjPool& owner, Composer& composer) {
    owner.make<WaylandChrome>(composer);
}
