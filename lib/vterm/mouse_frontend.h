/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "mouse_protocol.h"
#include "vt_geometry.h"

#include <lib/vterm/point.h>

#include <std/sys/types.h>

enum FrontendModifier : unsigned {
    FrontendShift = 1,
    FrontendControl = 2,
    FrontendAlt = 4,
};

// A1: the pointer's half of the layout. The content box starts at
// (contentLeft(), contentTop()) and ends before (contentRight(),
// contentBottom()); everything outside it belongs to the border, to
// whatever chrome reserves a side, and - since T10 - to the pane next
// door.
struct MouseGeometry {
    // The whole surface. The window's, shared by every pane on it, and
    // read from the window's own VtGeometry rather than copied into each
    // pane's - see mouseGeometry() below.
    int framebufferWidth = 1;
    int framebufferHeight = 1;
    // A1/A10: this pane's own border, per side - and nothing else. What
    // window chrome reserves is not here and never was a term of these
    // mappings' arithmetic after T5.1: it is already spent inside
    // paneOriginX below. Filling this from the *window's* insets instead
    // would charge the reserve twice on every pane that does not start
    // at the window's own edge, which is a click landing in the wrong
    // column and nothing louder.
    VtInsets insets;
    // A8: this pane's top-left on the surface, in backing pixels,
    // counted from (0, 0) of the surface. Deliberately its own pair of
    // fields rather than something added into `insets`: the inset is the
    // air this pane keeps around its own text and the origin is where
    // the layout put the pane, and folding them together would repeat
    // exactly the mistake A1 removed, where an option and a reserve
    // shared one number and neither could be read back out.
    //
    // Whatever chrome reserved is inside this number, because the
    // embedder took it off the window before it cut the window into
    // panes. Zero only for a pane that starts at the surface's own
    // corner, which is a window with no chrome and one terminal.
    int paneOriginX = 0;
    int paneOriginY = 0;
    // T10, the debt A8 left: how far this pane's content reaches from
    // contentLeft()/contentTop(), in backing pixels. Handed in like the
    // origin is, and for the same reason - the layout knows it and the
    // window does not.
    //
    // Deliberately not derived from the caller's own columns x glyphWidth.
    // A grid is what is left after the extent is divided by the glyph, so
    // recovering the extent from it loses whatever did not fill a cell -
    // exactly the sliver NamesACellPastTheGridWhenTheContentBoxEndsMidCell
    // pins as being inside the box. The two numbers would then disagree
    // about the last few pixels of every pane whose box does not divide
    // evenly, and the mappings that clamp would disagree with the one
    // that does not.
    //
    // No default that means "the window": zero is an empty box and says
    // so. A pane whose extent was forgotten then names no cell at all,
    // which is a bug that shows itself on the first click rather than one
    // that quietly hands the pointer its neighbour's pixels.
    int contentWidth = 0;
    int contentHeight = 0;
    int glyphWidth = 1;
    int glyphHeight = 1;

    // Where this pane's text starts on the surface: where the layout put
    // the pane, plus the air the pane keeps on that side. The one place
    // the two are added, so no mapping can add them twice or forget one
    // of them - and the only two terms there are, because the third one
    // a window with chrome used to need is already inside the first.
    int contentLeft() const {
        return insets.left + paneOriginX;
    }

    int contentTop() const {
        return insets.top + paneOriginY;
    }

    // The far side of the pane's content box, exclusive, on the same
    // surface the near side is measured on: the near end plus the extent,
    // which is the only way two ends can be made to name one origin by
    // construction rather than by agreement.
    //
    // Named here so every clamp reads both of its ends off one surface. A
    // clamp that subtracts the pane's origin from a pixel and then bounds
    // the result by the *window's content extent* (framebufferWidth minus
    // both insets) has taken its two ends from two different origins, and
    // overshoots the window's own edge by exactly paneOriginX: with a pane
    // starting 500 px in, the near end counts from 500 and the far end from
    // 0 (R5-qa, Q1). The extent a mapping may use is therefore always a
    // difference of these two, never an inset arithmetic of its own.
    int contentRight() const {
        return contentLeft() + contentWidth;
    }

    int contentBottom() const {
        return contentTop() + contentHeight;
    }
};

struct MouseProtocolPoint {
    int column = 1;
    int row = 1;
};

int mouseFramebufferCoordinate(double logical, double scale);

// The one place a geometry becomes a pointer geometry, so no caller can
// pair a side with the wrong axis on its way in.
//
// Two arguments and two VtGeometry instances, because there are two
// things here and they belong to different owners. `pane` is the
// layout's: where this pane sits, how far its text reaches, and the air
// it keeps around it. `window` is the embedder's: the surface everything
// is measured on and the cell size the font gives it, one per window and
// shared by every pane on it. Reading the shared half from the window
// itself is what keeps a font change from having to reach every pane
// before the next click lands in the right cell - and what stops N panes
// each holding their own copy of one number.
//
// A8 is why there is no single-argument form: `mouseGeometry(x)` used to
// mean "the pane that fills the window", which is the assumption
// production does not get to make any more. mouse_geometry_guard still
// meters exactly that spelling.
MouseGeometry mouseGeometry(const VtGeometry& pane, const VtGeometry& window);

MouseProtocolPoint mouseProtocolPoint(MouseTrackingEnc encoding, int pixelX, int pixelY, const MouseGeometry& geometry);

// The cell a pixel lands in; false when it lands in an inset rather than
// in the content box, and then column and row are left alone.
bool mouseCell(int pixelX, int pixelY, const MouseGeometry& geometry, u16& column, u16& row);

// The cell a selection endpoint names: clamped into the content box, and
// allowed to land one column past the last one, which is what the open
// end of an extent means.
Point mouseSelectionCell(int pixelX, int pixelY, const MouseGeometry& geometry, int columns, int rows);

// -1 above the content box, +1 below it, 0 inside - the direction a drag
// held at this pixel scrolls.
int mouseAutoscrollDirection(int pixelY, const MouseGeometry& geometry);

unsigned mouseProtocolModifiers(unsigned frontendModifiers, bool reportAlt = true);
int mouseTerminalButton(int frontendButton);
bool mouseButtonReportAllowed(MouseTrackingMode mode, MouseEventType type, int button);

struct MouseWheelSteps {
    int x = 0;
    int y = 0;
};

class MouseWheelAccumulator {
public:
    MouseWheelSteps consume(double x, double y, bool reporting);
    void reset();

private:
    static int consumeAxis(double delta, double& remainder);

    bool reporting_ = false;
    double remainderX_ = 0.0;
    double remainderY_ = 0.0;
};

class MouseFrontendState {
public:
    bool protocolActive(unsigned modifiers, MouseTrackingMode mode) const;
    void updateButton(int button, bool pressed);

    void clearButtons() {
        buttons_ = 0;
    }

    unsigned buttons() const {
        return buttons_;
    }

    int motionButton() const;
    bool primaryButtonPressed() const;

    int registerClick(int button, double x, double y, double time);

    void beginSelection() {
        selectionOngoing_ = true;
    }

    void endSelection() {
        selectionOngoing_ = false;
    }

    bool selectionOngoing() const {
        return selectionOngoing_;
    }

    bool reportMotion(int column, int row, MouseTrackingMode mode, MouseTrackingEnc encoding, u32 generation);
    void resetMotion();

    MouseWheelSteps consumeWheel(double x, double y, bool reporting) {
        return wheel_.consume(x, y, reporting);
    }

private:
    bool selectionOngoing_ = false;
    unsigned buttons_ = 0;
    int lastButton_ = -1;
    int clickCount_ = 0;
    double lastClickTime_ = 0.0;
    double lastClickX_ = 0.0;
    double lastClickY_ = 0.0;
    bool hasReportedMotion_ = false;
    bool hasMotionContext_ = false;
    MouseTrackingMode lastMotionMode_ = MouseTrackingMode::Disabled;
    MouseTrackingEnc lastMotionEncoding_ = MouseTrackingEnc::Default;
    u32 lastMotionGeneration_ = 0;
    int lastReportColumn_ = 0;
    int lastReportRow_ = 0;
    MouseWheelAccumulator wheel_;
};
