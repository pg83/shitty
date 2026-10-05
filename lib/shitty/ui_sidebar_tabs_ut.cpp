/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pty.h"
#include "options.h"
#include "process_directory.h"
#include "startup.h"
#include "composer.h"
#include "tint_coat.h"
#include "input_bindings.h"
#include "ui_sidebar_tabs.h"

#include <lib/vterm/listener.h>

#include <std/tst/ut.h>
#include <std/lib/list.h>
#include <std/str/view.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/platform_headless.h>

using namespace stl;

// C10. Outside the darwin guard below on purpose. Everything past that
// guard needs AppKit to link against; this does not - it is plain
// arithmetic over Color, and a test that only compiles on one platform
// is how the thirteenth blind instrument of this plan went blind. The
// sidebar is its only caller today, which is why it is tested here.
STD_TEST_SUITE(TintCoat) {
    // The property, and the whole reason the pair of functions exists in
    // that shape: coatOverOpaque() is an independent inverse, so this is
    // an assertion and not a restatement of thinnestCoat()'s own formula.
    //
    // Exact, not within a tolerance. That is a measurement: over
    // 2 424 832 (target, backdrop) triples the worst round-trip error is
    // zero, so a tolerance here would be slack nobody needs and would
    // hide the day it stops being exact.
    STD_TEST(ACoatLandsOnTheColourItWasAskedFor) {
        unsigned checked = 0;
        for (unsigned backdrop = 0; backdrop < 256; backdrop += 5) {
            const Color under{(u8)(backdrop), (u8)(255 - backdrop), (u8)(backdrop / 2)};
            for (unsigned red = 0; red < 256; red += 3) {
                for (unsigned green = 0; green < 256; green += 37) {
                    const Color want{(u8)(red), (u8)(green), (u8)(255 - red)};
                    const Color got = coatOverOpaque(thinnestCoat(want, under), under);
                    STD_INSIST(got == want);
                    ++checked;
                }
            }
        }
        // The sweep really ran: a loop whose bounds slipped would leave
        // this at zero and every assertion above unexecuted.
        STD_INSIST(checked > 10000);
    }

    // The default case, and it is what shows this is the right
    // generalisation rather than a trick: asked for the panel's own
    // default colour over the project's own background, the coat comes
    // out as roughly the foreground at roughly the six percent the
    // sidebar used to paint by hand.
    STD_TEST(TheDefaultPanelAsksForAboutSixPercentOfInk) {
        const Color background{46, 52, 64};
        const Color foreground{216, 222, 233};
        // mix(bg, fg, 0.06), computed here rather than taken from the
        // sidebar: this test may not depend on the code it checks.
        const Color panel{56, 62, 74};
        const TintCoat coat = thinnestCoat(panel, background);

        STD_INSIST(coatOverOpaque(coat, background) == panel);
        // Thin: nowhere near a full coat, which is the point - a thick
        // one would stack with the terminal's own translucent background.
        STD_INSIST(coat.alpha >= 10 && coat.alpha <= 20);
        // And pushed toward the foreground's end of the range, which is
        // how the alpha got to be small.
        STD_INSIST(coat.color.red > foreground.red);
        STD_INSIST(coat.color.green > foreground.green);
        STD_INSIST(coat.color.blue > foreground.blue);
    }

    // The two ends, named, so the range is pinned from both sides.
    STD_TEST(TheEndsOfTheRangeAreNothingAndEverything) {
        const Color background{46, 52, 64};
        // A colour that is already the backdrop needs no paint at all.
        STD_INSIST(thinnestCoat(background, background).alpha == 0);

        // And one the backdrop cannot be pushed to without covering it.
        // This is a property of the request, not of the arithmetic, and
        // the option's documentation says so.
        STD_INSIST(thinnestCoat(Color{255, 255, 255}, Color{0, 0, 0}).alpha == 255);
        STD_INSIST(thinnestCoat(Color{0, 0, 0}, Color{255, 255, 255}).alpha == 255);

        // Halfway is halfway: the midpoint of black and white needs half
        // a coat, and it is a number rather than a derivation.
        const TintCoat half = thinnestCoat(Color{128, 128, 128}, Color{0, 0, 0});
        STD_INSIST(half.alpha == 128);
        STD_INSIST(half.color.red == 255);
    }

    // A channel that has further to go decides the alpha for all three -
    // a coat thin enough for the others cannot carry it.
    STD_TEST(TheFurthestChannelSetsTheThickness) {
        const Color background{0, 0, 0};
        const TintCoat coat = thinnestCoat(Color{10, 200, 10}, background);
        // Green needs 200/255 of a full coat; red and green would have
        // been happy with far less.
        STD_INSIST(coat.alpha == 200);
        STD_INSIST(coat.color.green == 255);
        STD_INSIST(coat.color.red < 20);
        STD_INSIST(coatOverOpaque(coat, background) == (Color{10, 200, 10}));
    }
}

// createSidebarTabsUi() is macOS-only chrome and only enters the build
// on darwin (build.py); off it there is nothing to link against. The
// drawing itself is AppKit's and wants a human; what is testable here is
// the half that decides how much of the window the terminal gets - the
// half a mistake in is invisible until the text is already under the
// panel - and the two decisions V2 hoisted out of the AppKit half so
// they could be checked at all.
#if defined(__APPLE__)

// What a row shows instead of the raw window title, and the row a click
// lands in, both defined in ui_sidebar_tabs.mm. Declared here rather
// than in ui_sidebar_tabs.h because they are the module's own business
// and nobody else's; the pattern is F4's csdTabsChromeAlpha().
StringView sidebarTabsShortTitle(StringView title);
long long sidebarTabsRowAt(double panelHeight, double offsetFromTop, size_t count, double topInset);
long long sidebarTabsRowAtHeights(double panelHeight, double offsetFromTop, const double* heights, size_t count, double topInset);
double sidebarTabsRowOffset(const double* heights, size_t count, size_t at);
bool sidebarTabsPinAt(double offsetFromLeft, double indent);
double sidebarTabsRowHeight();
double sidebarTabsListTop();
double sidebarTabsLineTop(size_t line);
double sidebarTabsLineHeight(size_t line);

// The icons on the folder and branch lines: whether a face carries a
// code point at all, how much ink one actually leaves when drawn, and
// where a line's text starts once the icon column is or is not there.
bool sidebarTabsFontCovers(StringView fontName, unsigned codepoint);
unsigned sidebarTabsIconInk(StringView fontName, unsigned codepoint, double size);
double sidebarTabsLineLeft(size_t line, double textLeft, bool iconsAvailable);

// The third line of a row, and the two pure halves of working it out.
// Declared here for the same reason: a decision reachable only through
// the filesystem is a decision no test pins down.
StringView sidebarTabsGitDirLink(StringView contents);
StringView sidebarTabsHeadBranch(StringView head);
bool sidebarTabsBranch(StringView directory, stl::Buffer& out);

namespace {
    // Everything AppKit in this module is deferred to the main queue,
    // which no unit test drains, and the deferred work is the only
    // thing that touches an NSWindow. Leaving composer.sessions null
    // keeps it that way: the projection returns before it schedules
    // anything, so nothing is left pointing at a pool this test is
    // about to drop.
    Composer& sidebarComposer(ObjPool& pool, Options& options) {
        Composer& composer = *pool.make<Composer>(&pool);
        options.border = 0;
        options.sidebarTabs = true;
        options.sidebarWidth = 200;
        composer.setOptions(&options);
        plt::Platform* const platform = plt::createHeadlessPlatform(pool);
        composer.window = platform->createWindow(pool, {});
        composer.installVtHost();
        composer.geometry.setCellPixelSize(8, 16);
        return composer;
    }

    // Whether an absolute directory has no .git at or above it. Worked
    // out here with stat(), deliberately not by asking
    // sidebarTabsBranch(): this answer chooses the ground the filesystem
    // test below stands on, and ground chosen by the code under test
    // proves nothing about that code - a sidebarTabsBranch() stuck at
    // false would pick its own alibi.
    bool outsideAnyRepository(StringView directory) {
        if (directory.length() == 0 || directory[0] != '/') {
            return false;
        }
        StringBuilder path;
        StringView here = directory;
        for (;;) {
            while (here.length() > 1 && here.back() == '/') {
                here = here.prefix(here.length() - 1);
            }
            path.reset();
            path << here;
            if (here.back() != '/') {
                path << StringView(u8"/");
            }
            path << StringView(u8".git");
            struct stat info;
            if (::stat(path.cStr(), &info) == 0) {
                return false;
            }
            if (here.length() <= 1) {
                return true;
            }
            size_t cut = here.length();
            while (cut > 1 && here[cut - 1] != '/') {
                --cut;
            }
            here = here.prefix(cut == 1 ? 1 : cut - 1);
        }
    }

    // Mirrors quick_frame_store_ut.cpp's makeTempDir(): a mkdtemp()
    // directory this process owns, torn down by the caller. Unlike the
    // others it cannot take TMPDIR on trust, because the one test that
    // uses it asserts that nothing above this root is a repository - and
    // a root inside one makes that assertion false about the machine
    // while the code answers correctly. ./build gives every command a
    // TMPDIR of its own inside the checkout (.build/tmp/<uid>), and a
    // developer with TMPDIR under a versioned home has the same root
    // with no build system to blame. So the root is chosen and checked
    // rather than assumed.
    void makeTempDir(StringBuilder& dir) {
        const char* const configured = getenv("TMPDIR");
        const StringView candidates[] = {
            StringView(configured != nullptr ? configured : "/tmp"),
            StringView(u8"/tmp"),
        };
        for (const StringView candidate : candidates) {
            if (!outsideAnyRepository(candidate)) {
                continue;
            }
            dir.reset();
            dir << candidate << StringView(u8"/ui_sidebar_tabs_ut.XXXXXX");
            STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
            return;
        }
        // Every candidate sits inside a repository, so "no repository
        // above this root" could not be told from a working lookup.
        // Failing here says that; asserting it anyway would say nothing.
        STD_INSIST(false);
    }

    void makeDir(StringView path) {
        Buffer buf{path};
        STD_INSIST(::mkdir(buf.cStr(), 0755) == 0);
    }

    void writeRawFile(StringView path, StringView content) {
        Buffer buf{path};
        const int fd = ::open(buf.cStr(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        STD_INSIST(fd >= 0);
        STD_INSIST(::write(fd, content.data(), content.length()) == (ssize_t)(content.length()));
        ::close(fd);
    }

    void removePath(StringView path) {
        Buffer buf{path};
        if (::unlink(buf.cStr()) != 0) {
            ::rmdir(buf.cStr());
        }
    }

    StringView branchOf(StringView directory, Buffer& out) {
        return sidebarTabsBranch(directory, out) ? StringView(out) : StringView();
    }

    void publish(IntrusiveList& listeners) {
        for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }
    }

    bool pressCmdB(Composer& composer) {
        return composer.inputBindings->key({plt::InputKey::Printable, plt::InputAction::Press, plt::InputSuper, 0, 'b'});
    }
}

STD_TEST_SUITE(SidebarTabsUi) {
    // The reserve is in place before the first resize, not after it:
    // application.cpp builds this object right after the window and
    // before showWindow() sizes the grid, so a panel that only claimed
    // its width once something else asked would show one frame of text
    // underneath itself and then jump.
    STD_TEST(ReservesItsWidthBeforeTheGridIsFirstCounted) {
        auto pool = ObjPool::fromMemory();
        Options options;
        Composer& composer = sidebarComposer(*pool, options);

        createSidebarTabsUi(*pool, composer);

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 200);
        STD_INSIST(composer.chromeReserve(ChromeSide::Top) == 0);
        STD_INSIST(composer.chromeReserve(ChromeSide::Bottom) == 0);
        STD_INSIST(composer.chromeReserve(ChromeSide::Right) == 0);

        composer.resize(1600, 800);

        // 200 points wide out of 1600 leaves 1400, which is 175 cells
        // of 8 - and the rows are untouched, the panel being on no
        // vertical edge. The left edge and nothing else: the title-bar
        // strip's own reserve is on top and stays there, and it is the
        // left one ui_csd_tabs.mm reads to know a tab list is up (V2).
        STD_INSIST(composer.geometry.columns == 175);
        STD_INSIST(composer.geometry.rows == 50);
        STD_INSIST(composer.contentInsets().left == 200);

        // The width is an option in points and stays one: a move to a
        // Retina display doubles what it costs the grid without anybody
        // re-applying anything.
        composer.setContentScale(2.0f);
        composer.resize(1600, 800);

        STD_INSIST(composer.contentInsets().left == 400);
        STD_INSIST(composer.geometry.columns == 150);
    }

    // V2's first complaint, and the one a test can pin down: the panel
    // is on the *left*. Every reserve above says so, and this says the
    // sides are not interchangeable - the grid's own left inset is what
    // moves, and the right one never does.
    STD_TEST(TheGridLosesItsLeftEdgeAndKeepsItsRight) {
        auto pool = ObjPool::fromMemory();
        Options options;
        Composer& composer = sidebarComposer(*pool, options);
        createSidebarTabsUi(*pool, composer);
        composer.resize(1600, 800);

        STD_INSIST(composer.contentInsets().left == 200);
        STD_INSIST(composer.contentInsets().right == 0);
        STD_INSIST(composer.contentInsets().top == 0);
        STD_INSIST(composer.contentInsets().bottom == 0);

        STD_INSIST(pressCmdB(composer));

        STD_INSIST(composer.contentInsets().left == 0);
        STD_INSIST(composer.contentInsets().right == 0);
    }

    // V2's third complaint, the readable half of it: a shell sets the
    // window title to the whole of "user@host:~/Projects/github.com/
    // shitty", and a 220pt column showed the user "...ects/github.com/
    // shitty". The last path component is what differs between tabs.
    STD_TEST(ARowShowsThePathsLastComponentAndLeavesACommandAlone) {
        STD_INSIST(sidebarTabsShortTitle(StringView(u8"kibomibo@KiboMBP:~/Projects/github.com/shitty")) == StringView(u8"shitty"));
        STD_INSIST(sidebarTabsShortTitle(StringView(u8"a/b")) == StringView(u8"b"));

        // No slash at all is a command line, or a bare host name, and
        // there is nothing to cut off it.
        STD_INSIST(sidebarTabsShortTitle(StringView(u8"vim")) == StringView(u8"vim"));
        STD_INSIST(sidebarTabsShortTitle(StringView(u8"npm run build")) == StringView(u8"npm run build"));

        // The edges: a trailing slash would leave an empty label, and an
        // empty title never reaches here (project() substitutes the
        // brand name first), but neither may walk off the end.
        STD_INSIST(sidebarTabsShortTitle(StringView(u8"~/Projects/")) == StringView(u8"~/Projects/"));
        STD_INSIST(sidebarTabsShortTitle(StringView(u8"/")) == StringView(u8"/"));
        STD_INSIST(sidebarTabsShortTitle(StringView()).length() == 0);
    }

    // A click lands in the row the eye sees, which is one function's
    // answer shared by drawRect: and mouseDown: - the two disagreeing is
    // the classic way a list like this selects the tab above the one
    // under the pointer. Written against the metrics rather than against
    // their values: the row grew from one line to three during this task,
    // and a test carrying the old number would have failed for saying so
    // rather than for finding anything.
    STD_TEST(AClickLandsInTheRowThatWasDrawnThere) {
        const double row = sidebarTabsRowHeight();
        const double top = sidebarTabsListTop();
        const double tall = top + row * 8;

        // The panel starts with a gap, and a click in it selects nothing
        // rather than the first tab.
        STD_INSIST(sidebarTabsRowAt(tall, 0, 3, 0) == -1);
        STD_INSIST(sidebarTabsRowAt(tall, top - 0.5, 3, 0) == -1);

        STD_INSIST(sidebarTabsRowAt(tall, top, 3, 0) == 0);
        STD_INSIST(sidebarTabsRowAt(tall, top + row - 0.5, 3, 0) == 0);
        STD_INSIST(sidebarTabsRowAt(tall, top + row, 3, 0) == 1);
        STD_INSIST(sidebarTabsRowAt(tall, top + row * 2.5, 3, 0) == 2);

        // The new-tab row under the list, and bare panel below it.
        STD_INSIST(sidebarTabsRowAt(tall, top + row * 3, 3, 0) == 3);
        STD_INSIST(sidebarTabsRowAt(tall, top + row * 4, 3, 0) == -1);

        // A row that does not fit whole is drawn nowhere, so it answers
        // nothing either: this panel holds rows 0 and 1 and part of 2.
        const double clipped = top + row * 2 + row / 2;
        STD_INSIST(sidebarTabsRowAt(clipped, top + row, 3, 0) == 1);
        STD_INSIST(sidebarTabsRowAt(clipped, top + row * 2, 3, 0) == -1);
    }

    // Folders: a label row is short, and the one sum of heights places
    // every row after it - for drawing and for clicking alike. Premise:
    // the label is not the ordinary height, or this pins nothing.
    STD_TEST(RowsOfTwoHeightsAreFoundWhereTheyAreDrawn) {
        const double row = sidebarTabsRowHeight();
        // A folder's header row, SidebarMetrics::labelRowHeight.
        const double label = 28;
        STD_INSIST(label != row);
        const double top = sidebarTabsListTop();
        const double heights[3] = {label, row, row};
        const double tall = 10000;
        STD_INSIST(sidebarTabsRowOffset(heights, 3, 0) == 0);
        STD_INSIST(sidebarTabsRowOffset(heights, 3, 1) == label);
        STD_INSIST(sidebarTabsRowOffset(heights, 3, 2) == label + row);
        // The "+" row past the end is the ordinary height.
        STD_INSIST(sidebarTabsRowOffset(heights, 3, 4) == label + row * 3);
        STD_INSIST(sidebarTabsRowOffset(nullptr, 0, 2) == row * 2);

        STD_INSIST(sidebarTabsRowAtHeights(tall, top - 0.5, heights, 3, 0) == -1);
        STD_INSIST(sidebarTabsRowAtHeights(tall, top, heights, 3, 0) == 0);
        STD_INSIST(sidebarTabsRowAtHeights(tall, top + label - 0.5, heights, 3, 0) == 0);
        STD_INSIST(sidebarTabsRowAtHeights(tall, top + label, heights, 3, 0) == 1);
        STD_INSIST(sidebarTabsRowAtHeights(tall, top + label + row, heights, 3, 0) == 2);
        STD_INSIST(sidebarTabsRowAtHeights(tall, top + label + row * 2, heights, 3, 0) == 3);
        STD_INSIST(sidebarTabsRowAtHeights(tall, top + label + row * 3, heights, 3, 0) == -1);
        // A row cut by the bottom edge answers nothing.
        const double clipped = top + label + row * 1.5;
        STD_INSIST(sidebarTabsRowAtHeights(clipped, top + label + row, heights, 3, 0) == -1);
        STD_INSIST(sidebarTabsRowAtHeights(clipped, top + label, heights, 3, 0) == 1);
        // Uniform heights answer as the old function did.
        STD_INSIST(sidebarTabsRowAtHeights(tall, top + row * 1.5, nullptr, 3, 0) == sidebarTabsRowAt(tall, top + row * 1.5, 3, 0));
    }

    // C10. The panel now runs the full height of the window with the
    // title bar drawn over its top, so the list starts below whatever
    // the title bar reserved. What this pins is that the reserve moves
    // the list and nothing else - and, at zero, moves nothing at all.
    STD_TEST(TheListStartsBelowTheTitleBarAndTheReserveMovesOnlyIt) {
        const double row = sidebarTabsRowHeight();
        const double top = sidebarTabsListTop();
        const double inset = 28;
        const double tall = inset + top + row * 8;

        // The band the title bar is drawn over answers nothing. Without
        // this the first row would sit under the window buttons, which
        // is the reason the list is inset at all rather than the panel
        // being shortened.
        STD_INSIST(sidebarTabsRowAt(tall, 0, 3, inset) == -1);
        STD_INSIST(sidebarTabsRowAt(tall, inset - 0.5, 3, inset) == -1);
        STD_INSIST(sidebarTabsRowAt(tall, inset + top - 0.5, 3, inset) == -1);

        // And every row is exactly the reserve further down than it was.
        // Asserted against the un-inset answer rather than against
        // rewritten arithmetic: a test that recomputed the offsets with
        // the same expression would agree with any inset at all.
        const double probes[] = {top, top + row - 0.5, top + row, top + row * 2.5, top + row * 3, top + row * 4};
        for (double probe : probes) {
            STD_INSIST(sidebarTabsRowAt(tall, inset + probe, 3, inset) == sidebarTabsRowAt(tall - inset, probe, 3, 0));
        }
        // The premise of the loop above: those probes do not all answer
        // the same thing, so agreeing with them means something.
        STD_INSIST(sidebarTabsRowAt(tall, inset + top, 3, inset) == 0);
        STD_INSIST(sidebarTabsRowAt(tall, inset + top + row * 3, 3, inset) == 3);
        STD_INSIST(sidebarTabsRowAt(tall, inset + top + row * 4, 3, inset) == -1);

        // The bottom clamp counts the inset too: a panel this tall held
        // rows 0..2 with no title bar, and holds one fewer with one.
        const double clipped = inset + top + row * 2 + row / 2;
        STD_INSIST(sidebarTabsRowAt(clipped, inset + top + row, 3, inset) == 1);
        STD_INSIST(sidebarTabsRowAt(clipped, inset + top + row * 2, 3, inset) == -1);
        // Same height, no title bar: now row 2 does fit.
        STD_INSIST(sidebarTabsRowAt(clipped, top + row * 2, 3, 0) == 2);
    }

    // The two lines fit inside the row that holds them, in order, with
    // the same padding above the first and below the last. A row height
    // written down by hand rather than derived is how a list ends up
    // drawing its second line over the top of the next row's first.
    STD_TEST(TheTwoLinesFitTheRowAndDoNotOverlap) {
        const double row = sidebarTabsRowHeight();

        STD_INSIST(sidebarTabsLineTop(0) > 0);
        STD_INSIST(sidebarTabsLineHeight(0) > 0);
        STD_INSIST(sidebarTabsLineHeight(1) > 0);
        STD_INSIST(sidebarTabsLineTop(0) + sidebarTabsLineHeight(0) <= sidebarTabsLineTop(1));
        const double bottom = sidebarTabsLineTop(1) + sidebarTabsLineHeight(1);
        STD_INSIST(bottom <= row);
        STD_INSIST(row - bottom == sidebarTabsLineTop(0));
        // The first line is the title and is the taller of the two.
        STD_INSIST(sidebarTabsLineHeight(0) > sidebarTabsLineHeight(1));
    }

    // HEAD says which branch is out, and says it two different ways.
    STD_TEST(AHeadFileNamesTheBranchOrTheDetachedObject) {
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"ref: refs/heads/main\n")) == StringView(u8"main"));
        // Branch names carry slashes of their own, and the whole tail is
        // the name - cutting at the last slash would show "V2-sidebar-left"
        // for one branch and the same for a "wip/V2-sidebar-left".
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"ref: refs/heads/feat/V2-sidebar-left\n")) == StringView(u8"feat/V2-sidebar-left"));
        // Written without the trailing newline by some tools.
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"ref: refs/heads/main")) == StringView(u8"main"));

        // A detached head is the object id, abbreviated the way git
        // abbreviates it - a whole one would not fit the column.
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"0763f220e1b4c5d6a7f8091a2b3c4d5e6f708192\n")) == StringView(u8"0763f22"));

        // Neither shape: a corrupt or half-written HEAD reads as no
        // repository rather than as a row of garbage.
        STD_INSIST(sidebarTabsHeadBranch(StringView()).length() == 0);
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"\n")).length() == 0);
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"not a head at all\n")).length() == 0);
        // Too short to be an object id, hex or not.
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"abc123\n")).length() == 0);

        // A symbolic ref pointing somewhere other than refs/heads is
        // shown as written rather than guessed at.
        STD_INSIST(sidebarTabsHeadBranch(StringView(u8"ref: refs/tags/v1\n")) == StringView(u8"refs/tags/v1"));
    }

    // In a linked worktree .git is a file rather than a directory, and
    // this panel is being written inside one of half a dozen open on
    // this repository right now.
    STD_TEST(AWorktreesDotGitIsAFileThatPointsElsewhere) {
        STD_INSIST(sidebarTabsGitDirLink(StringView(u8"gitdir: /Users/x/p/.git/worktrees/v2\n")) == StringView(u8"/Users/x/p/.git/worktrees/v2"));
        // Submodules write it relative to the file that holds it.
        STD_INSIST(sidebarTabsGitDirLink(StringView(u8"gitdir: ../.git/modules/sub\n")) == StringView(u8"../.git/modules/sub"));
        // No newline, and a stray trailing space.
        STD_INSIST(sidebarTabsGitDirLink(StringView(u8"gitdir: /a/b ")) == StringView(u8"/a/b"));

        STD_INSIST(sidebarTabsGitDirLink(StringView()).length() == 0);
        STD_INSIST(sidebarTabsGitDirLink(StringView(u8"ref: refs/heads/main\n")).length() == 0);
    }

    // The whole lookup against a real filesystem: from a subdirectory,
    // through a worktree link, through a relative link, and out the far
    // side of a directory that is in no repository at all - which is a
    // different answer from every other one here and the only one the
    // row is allowed to call "no git".
    STD_TEST(TheBranchIsFoundFromASubdirectoryAndThroughEveryKindOfDotGit) {
        StringBuilder root;
        makeTempDir(root);

        StringBuilder repo;
        repo << StringView(root) << StringView(u8"/repo");
        makeDir(StringView(repo));
        StringBuilder gitDir;
        gitDir << StringView(repo) << StringView(u8"/.git");
        makeDir(StringView(gitDir));
        StringBuilder headPath;
        headPath << StringView(gitDir) << StringView(u8"/HEAD");
        writeRawFile(StringView(headPath), StringView(u8"ref: refs/heads/main\n"));

        Buffer out;

        // The repository root itself.
        STD_INSIST(branchOf(StringView(repo), out) == StringView(u8"main"));

        // And from a subdirectory two levels down - the walk up the tree
        // is the whole reason a tab sitting in lib/shitty finds anything.
        StringBuilder sub;
        sub << StringView(repo) << StringView(u8"/lib");
        makeDir(StringView(sub));
        StringBuilder deeper;
        deeper << StringView(sub) << StringView(u8"/shitty");
        makeDir(StringView(deeper));
        STD_INSIST(branchOf(StringView(deeper), out) == StringView(u8"main"));
        // A trailing slash is the same directory.
        StringBuilder deeperSlash;
        deeperSlash << StringView(deeper) << StringView(u8"/");
        STD_INSIST(branchOf(StringView(deeperSlash), out) == StringView(u8"main"));

        // A linked worktree: .git is a file naming an absolute gitdir,
        // and the branch comes from *there*, not from the repository the
        // link points into.
        StringBuilder worktrees;
        worktrees << StringView(gitDir) << StringView(u8"/worktrees");
        makeDir(StringView(worktrees));
        StringBuilder wtGitDir;
        wtGitDir << StringView(worktrees) << StringView(u8"/v2");
        makeDir(StringView(wtGitDir));
        StringBuilder wtHead;
        wtHead << StringView(wtGitDir) << StringView(u8"/HEAD");
        writeRawFile(StringView(wtHead), StringView(u8"ref: refs/heads/feat/V2-sidebar-left\n"));
        StringBuilder worktree;
        worktree << StringView(root) << StringView(u8"/v2");
        makeDir(StringView(worktree));
        StringBuilder wtLink;
        wtLink << StringView(worktree) << StringView(u8"/.git");
        StringBuilder wtLinkText;
        wtLinkText << StringView(u8"gitdir: ") << StringView(wtGitDir) << StringView(u8"\n");
        writeRawFile(StringView(wtLink), StringView(wtLinkText));

        STD_INSIST(branchOf(StringView(worktree), out) == StringView(u8"feat/V2-sidebar-left"));

        // A relative link, the shape a submodule writes: resolved
        // against the directory holding the file, not against this
        // process's own directory.
        StringBuilder module;
        module << StringView(root) << StringView(u8"/module");
        makeDir(StringView(module));
        StringBuilder moduleLink;
        moduleLink << StringView(module) << StringView(u8"/.git");
        writeRawFile(StringView(moduleLink), StringView(u8"gitdir: ../repo/.git/worktrees/v2\n"));

        STD_INSIST(branchOf(StringView(module), out) == StringView(u8"feat/V2-sidebar-left"));

        // A detached head in the same repository.
        writeRawFile(StringView(headPath), StringView(u8"0763f220e1b4c5d6a7f8091a2b3c4d5e6f708192\n"));
        STD_INSIST(branchOf(StringView(deeper), out) == StringView(u8"0763f22"));

        // No repository above the temp root at all. This is the only
        // answer the row may render as "no git", and it has to be
        // reachable or the row would say it about everything. It is also
        // the only assertion here that reaches the walk's own giving-up
        // branch: the relative paths below never enter the walk. That
        // nothing sits above this root is makeTempDir()'s doing and is
        // checked there, not assumed of whatever TMPDIR names.
        STD_INSIST(!sidebarTabsBranch(StringView(root), out));

        // A relative directory is refused outright rather than resolved
        // against this process's own, which is not the tab's and would
        // answer with somebody else's branch. "." is the case that can
        // actually catch a missing guard: the test binary is built and
        // run inside a checkout, so without the guard this walks into
        // *this* repository and comes back with a branch. The assertion
        // holds either way - run the binary outside a repository and the
        // answer is still false - so it is never flaky, it just stops
        // being able to catch anything.
        STD_INSIST(!sidebarTabsBranch(StringView(u8"."), out));
        STD_INSIST(!sidebarTabsBranch(StringView(u8"lib/shitty"), out));
        STD_INSIST(!sidebarTabsBranch(StringView(), out));

        removePath(StringView(moduleLink));
        removePath(StringView(module));
        removePath(StringView(wtLink));
        removePath(StringView(worktree));
        removePath(StringView(wtHead));
        removePath(StringView(wtGitDir));
        removePath(StringView(worktrees));
        removePath(StringView(headPath));
        removePath(StringView(gitDir));
        removePath(StringView(deeper));
        removePath(StringView(sub));
        removePath(StringView(repo));
        removePath(StringView(root));
    }

    // And the negative control the whole three-line row rests on: "no
    // directory to be had" has to be distinguishable from "a directory
    // with no repository in it", or the second line would go blank for
    // two different reasons and say which one neither time.
    STD_TEST(ADeadProcessHasNoDirectoryAndThatIsNotTheSameAsHavingNoRepository) {
        Buffer out;

        // A pid that certainly named a process and certainly does not
        // any more: forked, exited, reaped. Anything else either might
        // still be alive or might never have existed. The helper's own
        // suite (process_directory_ut.cpp) pins the same answers for
        // both platforms; this one is about what the row makes of them.
        const pid_t dead = fork();
        STD_INSIST(dead >= 0);
        if (dead == 0) {
            _exit(0);
        }
        int status = 0;
        STD_INSIST(waitpid(dead, &status, 0) == dead);

        STD_INSIST(!processDirectory(dead, out));
        STD_INSIST(out.used() == 0);

        // The other false, and the reason both exist: this process has a
        // directory, and whether that directory sits in a repository is
        // a second, separate question with its own answer. The two are
        // never the same call and never the same bool.
        STD_INSIST(processDirectory(getpid(), out));
        Buffer branch;
        const bool inRepository = sidebarTabsBranch(StringView(out), branch);
        // Either answer is correct here - the test binary may or may not
        // be run from inside a checkout - but the directory was found
        // regardless, which is the whole point being made.
        STD_INSIST(out.used() != 0);
        STD_INSIST(inRepository == (branch.used() != 0));
    }

    // The directory is *that* process's, not the caller's. A lookup that
    // quietly answered with our own working directory would pass every
    // assertion above - this process is a live process with a directory,
    // and the two happen to be the same one - and would put the same
    // folder on all six rows on a live window.
    STD_TEST(TheDirectoryBelongsToThatProcessAndNotToThisOne) {
        int ready[2] = {-1, -1};
        STD_INSIST(::pipe(ready) == 0);

        const pid_t child = fork();
        STD_INSIST(child >= 0);
        if (child == 0) {
            // The child outlives nothing, whatever the parent does. An
            // assertion below aborts this test without reaching the kill,
            // and a child left blocked would hold the inherited stdout
            // open - which hangs any caller reading this binary's output
            // through a pipe, and did.
            ::alarm(20);
            // Only async-signal-safe calls before _exit: this binary may
            // be running its suite on more than one thread.
            if (::chdir("/") != 0) {
                _exit(1);
            }
            const char byte = 'x';
            if (::write(ready[1], &byte, 1) != 1) {
                _exit(1);
            }
            ::pause();
            _exit(0);
        }
        ::close(ready[1]);
        char byte = 0;
        STD_INSIST(::read(ready[0], &byte, 1) == 1);

        // The child is sitting in "/", and this process certainly is not
        // - the suite runs out of the build tree.
        Buffer theirs;
        STD_INSIST(processDirectory(child, theirs));
        STD_INSIST(StringView(theirs) == StringView(u8"/"));

        Buffer ours;
        STD_INSIST(processDirectory(getpid(), ours));
        STD_INSIST(StringView(ours) != StringView(u8"/"));

        STD_INSIST(::kill(child, SIGKILL) == 0);
        int status = 0;
        STD_INSIST(waitpid(child, &status, 0) == child);
        ::close(ready[0]);
    }

    // The chain the row will actually walk, minus the one forward that is
    // still someone else's file: a real pty child, its pid straight off
    // PtyHandle::childPid(), and that pid handed to the directory lookup.
    //
    // childPid() defaults to -1 on the test doubles, so a session whose
    // handle never overrode it would feed -1 here and the row would go
    // quietly blank. This is the positive control that the pid is a live
    // process the lookup can answer for - run before anything is built on
    // top of it, not after.
    STD_TEST(APtyChildsPidIsALiveProcessTheLookupCanAnswerFor) {
        auto pool = ObjPool::fromMemory();
        plt::Platform* const platform = plt::createHeadlessPlatform(*pool);
        Pty* const pty = createPty(*pool, *platform->scheduler(), platform);

        // No cd: the child inherits this process's directory at fork,
        // before it execs, so there is nothing to wait for and no race to
        // lose. That the lookup follows the *named* process rather than
        // the caller is a separate question, asked separately above.
        char program[] = "ui_sidebar_tabs_ut";
        char execute[] = "-e";
        char shell[] = "/bin/sh";
        char flag[] = "-c";
        char script[] = "sleep 30";
        char* argv[] = {program, execute, shell, flag, script, nullptr};
        const LaunchCommand command = buildLaunchCommand(5, argv, StringView(), false);
        PtyHandle* const handle = pty->spawn(*pool, command, PtySize{}, StringView());
        STD_INSIST(handle != nullptr);

        const pid_t child = handle->childPid();
        // Not the -1 the doubles keep, and not a number nobody owns.
        STD_INSIST(child > 0);
        STD_INSIST(::kill(child, 0) == 0);

        Buffer directory;
        STD_INSIST(processDirectory(child, directory));
        char expected[4096];
        STD_INSIST(getcwd(expected, sizeof(expected)) != nullptr);
        STD_INSIST(StringView(directory) == StringView(expected));

        ::kill(child, SIGKILL);
        int status = 0;
        (void)(waitpid(child, &status, 0));
    }

    // Nothing is drawn on a guess: the face is asked whether it carries
    // the code point. Helvetica ships with the system and has no Private
    // Use Area, so it answers yes for a letter and no for either icon -
    // which is what makes it usable as both controls at once.
    STD_TEST(AFaceIsAskedWhetherItCarriesTheIconBeforeAnythingIsDrawn) {
        static const StringView helvetica(u8"Helvetica");

        // The question can come back yes, so a no below means something.
        STD_INSIST(sidebarTabsFontCovers(helvetica, 'A'));

        // The two Nerd Font code points the user picked: nf-fa-folder and
        // nf-dev-git_branch, both Private Use Area, neither in Helvetica.
        STD_INSIST(!sidebarTabsFontCovers(helvetica, 0xF07B));
        STD_INSIST(!sidebarTabsFontCovers(helvetica, 0xE725));

        // A face that is not installed, and no face at all.
        STD_INSIST(!sidebarTabsFontCovers(StringView(u8"NoSuchFace-ZZZ-DoesNotExist"), 'A'));
        STD_INSIST(!sidebarTabsFontCovers(StringView(), 'A'));
    }

    // And the control that matters: an icon the font cannot draw leaves
    // *nothing*, not the hollow replacement box a face without the glyph
    // would otherwise paint. Counting ink is the only way to tell those
    // two apart - a test that only checked the code reached the drawing
    // call would pass on a row full of empty rectangles.
    STD_TEST(AnIconWithNoGlyphLeavesNoInkAtAllRatherThanABox) {
        static const StringView helvetica(u8"Helvetica");

        // A letter Helvetica does have: ink lands, so the measurement
        // works and a zero below is a real zero.
        STD_INSIST(sidebarTabsIconInk(helvetica, 'A', 13) > 0);

        // The icons it does not have: nothing at all.
        STD_INSIST(sidebarTabsIconInk(helvetica, 0xF07B, 13) == 0);
        STD_INSIST(sidebarTabsIconInk(helvetica, 0xE725, 13) == 0);

        // And a face that is not installed draws nothing rather than
        // falling back to whatever the system would substitute.
        STD_INSIST(sidebarTabsIconInk(StringView(u8"NoSuchFace-ZZZ-DoesNotExist"), 'A', 13) == 0);
    }

    // The folder and branch lines share one left edge in both states. An
    // icon column that appeared for one line and not the other would put
    // the two texts a few points apart, which reads as a broken row.
    STD_TEST(TheFolderAndBranchLinesShareOneLeftEdgeWithOrWithoutIcons) {
        const double textLeft = 34;

        // With icons, both context lines are indented by the same column,
        // and the title stays flush against it.
        STD_INSIST(sidebarTabsLineLeft(1, textLeft, true) == sidebarTabsLineLeft(2, textLeft, true));
        STD_INSIST(sidebarTabsLineLeft(1, textLeft, true) > textLeft);
        STD_INSIST(sidebarTabsLineLeft(0, textLeft, true) == textLeft);

        // Without them the column collapses - and collapses for both, so
        // the two lines still agree with each other and with the row as
        // it looked before icons existed.
        STD_INSIST(sidebarTabsLineLeft(1, textLeft, false) == sidebarTabsLineLeft(2, textLeft, false));
        STD_INSIST(sidebarTabsLineLeft(1, textLeft, false) == textLeft);
        STD_INSIST(sidebarTabsLineLeft(0, textLeft, false) == textLeft);
    }

    // cmd+b, and the one thing about it that is not like the hover strip
    // T6 builds: this legitimately re-counts the grid (A7). The columns
    // the panel held come back, and the resize that publishes is what
    // carries the new size to the shell.
    STD_TEST(CmdBGivesTheColumnsBackAndTakesThemAgain) {
        auto pool = ObjPool::fromMemory();
        Options options;
        Composer& composer = sidebarComposer(*pool, options);
        createSidebarTabsUi(*pool, composer);
        composer.resize(1600, 800);

        STD_INSIST(composer.geometry.columns == 175);

        STD_INSIST(pressCmdB(composer));

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 0);
        STD_INSIST(composer.contentInsets().left == 0);
        STD_INSIST(composer.geometry.columns == 200);
        STD_INSIST(composer.geometry.rows == 50);
        // The window itself did not move; only the share of it the
        // terminal gets did.
        STD_INSIST(composer.geometry.pixelWidth == 1600);

        STD_INSIST(pressCmdB(composer));

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 200);
        STD_INSIST(composer.geometry.columns == 175);
    }

    // Without -sidebarTabs the window is the one it always was: nothing
    // is reserved, cmd+b is not a way to make a panel appear that the
    // user never asked for, and the chord is not even consumed - it
    // goes on reaching the program inside the terminal, which under the
    // kitty keyboard protocol is told about it.
    STD_TEST(WithoutTheOptionNothingIsReservedAndCmdBIsNotEvenTaken) {
        auto pool = ObjPool::fromMemory();
        Options options;
        Composer& composer = sidebarComposer(*pool, options);
        options.sidebarTabs = false;

        createSidebarTabsUi(*pool, composer);
        composer.resize(1600, 800);

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 0);
        STD_INSIST(composer.geometry.columns == 200);

        STD_INSIST(!pressCmdB(composer));

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 0);
        STD_INSIST(composer.geometry.columns == 200);
    }

    // A reload that turns the option off has to hand the columns back
    // as well: a reserve nobody draws in is the grid paying rent for a
    // panel that is not there. The same path picks up a new width.
    STD_TEST(AReloadThatDropsTheOptionDropsTheReserve) {
        auto pool = ObjPool::fromMemory();
        Options options;
        Composer& composer = sidebarComposer(*pool, options);
        createSidebarTabsUi(*pool, composer);
        composer.resize(1600, 800);

        STD_INSIST(composer.geometry.columns == 175);

        options.sidebarTabs = false;
        publish(composer.configChangedListeners);

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 0);
        STD_INSIST(composer.geometry.columns == 200);

        options.sidebarTabs = true;
        options.sidebarWidth = 400;
        publish(composer.configChangedListeners);

        STD_INSIST(composer.chromeReserve(ChromeSide::Left) == 400);
        STD_INSIST(composer.geometry.columns == 150);
    }
}

#endif
