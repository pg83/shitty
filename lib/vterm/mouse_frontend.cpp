/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "mouse_frontend.h"

#include <std/alg/minmax.h>

#include <math.h>
#include <limits.h>

using namespace stl;

int mouseFramebufferCoordinate(double logical, double scale) {
    if (!isfinite(logical) || !isfinite(scale)) {
        return 0;
    }
    const double pixel = logical * max(1.0, scale);
    return (int)(min(max(round(pixel), (double)(INT_MIN)), (double)(INT_MAX)));
}

MouseGeometry mouseGeometry(const VtGeometry& pane, const VtGeometry& window) {
    // Designated, not positional: the origin sits between the insets and
    // the glyph size, and a positional list would have handed an origin
    // to glyphWidth the moment the struct grew.
    //
    // Every field is read off exactly one of the two, and which one is
    // the whole content of A10 here: the surface and the glyph are the
    // window's, and the border, the origin and the extent are the pane's.
    // Taking the border off the window instead is the one substitution
    // that still compiles and still answers a plausible cell - the pane
    // would be charged whatever chrome reserved a second time, on top of
    // the origin that already carries it.
    return {
        .framebufferWidth = window.pixelWidth,
        .framebufferHeight = window.pixelHeight,
        .insets = pane.insets,
        .paneOriginX = pane.originX,
        .paneOriginY = pane.originY,
        .contentWidth = pane.width,
        .contentHeight = pane.height,
        .glyphWidth = window.cellPixelWidth,
        .glyphHeight = window.cellPixelHeight,
    };
}

// The four mappings below share one coordinate system and one device:
// every near end is contentLeft()/contentTop() and every far end is
// contentRight()/contentBottom(), so an extent is always the difference
// of two edges of the same surface. Not a tidying: a clamp whose near end
// counted from the pane and whose far end was the window's *content
// extent* let the pointer past the window's own trailing inset by exactly
// paneOriginX, and did it in three of the four mappings while the fourth
// - mouseCell - already compared surface against surface (R5-qa, Q1).
//
// T10 retires the rest of it: the far ends are the pane's own now, taken
// from the extent the layout hands over beside the origin, so a pane that
// begins inside the window is no longer told about pixels past its last
// cell - they belong to its neighbour.
MouseProtocolPoint mouseProtocolPoint(MouseTrackingEnc encoding, int pixelX, int pixelY, const MouseGeometry& geometry) {
    const int contentWidth = max(1, geometry.contentRight() - geometry.contentLeft());
    const int contentHeight = max(1, geometry.contentBottom() - geometry.contentTop());
    if (encoding == MouseTrackingEnc::SGRPixels) {
        return {
            min(max(pixelX - geometry.contentLeft() + 1, 1), contentWidth),
            min(max(pixelY - geometry.contentTop() + 1, 1), contentHeight),
        };
    }
    const int columns = max(1, contentWidth / max(1, geometry.glyphWidth));
    const int rows = max(1, contentHeight / max(1, geometry.glyphHeight));
    return {
        min(max((pixelX - geometry.contentLeft()) / max(1, geometry.glyphWidth) + 1, 1), columns),
        min(max((pixelY - geometry.contentTop()) / max(1, geometry.glyphHeight) + 1, 1), rows),
    };
}

bool mouseCell(int pixelX, int pixelY, const MouseGeometry& geometry, u16& column, u16& row) {
    if (pixelX < geometry.contentLeft() || pixelY < geometry.contentTop() || pixelX >= geometry.contentRight() || pixelY >= geometry.contentBottom()) {
        return false;
    }
    column = (u16)((pixelX - geometry.contentLeft()) / max(1, geometry.glyphWidth));
    row = (u16)((pixelY - geometry.contentTop()) / max(1, geometry.glyphHeight));
    return true;
}

Point mouseSelectionCell(int pixelX, int pixelY, const MouseGeometry& geometry, int columns, int rows) {
    const int contentWidth = max(0, geometry.contentRight() - geometry.contentLeft());
    const int contentHeight = max(1, geometry.contentBottom() - geometry.contentTop());
    const int x = min(max(0, pixelX - geometry.contentLeft()), contentWidth);
    const int y = min(max(0, pixelY - geometry.contentTop()), contentHeight - 1);
    return Point(min(x / max(1, geometry.glyphWidth), columns), min(y / max(1, geometry.glyphHeight), rows - 1));
}

int mouseAutoscrollDirection(int pixelY, const MouseGeometry& geometry) {
    const int top = geometry.contentTop();
    const int bottom = max(top, geometry.contentBottom() - 1);
    if (pixelY <= top) {
        return -1;
    }
    if (pixelY >= bottom) {
        return 1;
    }
    return 0;
}

unsigned mouseProtocolModifiers(unsigned modifiers, bool reportAlt) {
    unsigned result = 0;
    if (modifiers & FrontendShift) {
        result |= MouseShift;
    }
    if (reportAlt && (modifiers & FrontendAlt)) {
        result |= MouseAlt;
    }
    if (modifiers & FrontendControl) {
        result |= MouseControl;
    }
    return result;
}

int mouseTerminalButton(int button) {
    switch (button) {
        case 0:
            return 1;
        case 2:
            return 2;
        case 1:
            return 3;
        default:
            return button >= 3 ? button - 3 + 8 : 0;
    }
}

bool mouseButtonReportAllowed(MouseTrackingMode mode, MouseEventType type, int button) {
    return button >= 1 && button <= 11 && !(type == MouseEventType::Release && button > 3) && mode != MouseTrackingMode::Disabled && !(type == MouseEventType::Release && mode == MouseTrackingMode::X10_Compat);
}

int MouseWheelAccumulator::consumeAxis(double delta, double& remainder) {
    if (!isfinite(delta)) {
        remainder = 0.0;
        return 0;
    }
    const double total = remainder + min(max(delta, -100.0), 100.0);
    const int steps = (int)(trunc(total));
    remainder = total - steps;
    return steps;
}

MouseWheelSteps MouseWheelAccumulator::consume(double x, double y, bool reporting) {
    if (reporting != reporting_) {
        reporting_ = reporting;
        remainderX_ = 0.0;
        remainderY_ = 0.0;
    }

    MouseWheelSteps steps;
    steps.y = consumeAxis(y, remainderY_);
    if (reporting) {
        steps.x = consumeAxis(x, remainderX_);
    } else {
        remainderX_ = 0.0;
    }
    return steps;
}

void MouseWheelAccumulator::reset() {
    reporting_ = false;
    remainderX_ = 0.0;
    remainderY_ = 0.0;
}

bool MouseFrontendState::protocolActive(unsigned modifiers, MouseTrackingMode mode) const {
    return !selectionOngoing_ && !(modifiers & FrontendShift) && mode != MouseTrackingMode::Disabled;
}

void MouseFrontendState::updateButton(int button, bool pressed) {
    if (button < 0 || button >= (int)(sizeof(buttons_) * CHAR_BIT)) {
        return;
    }
    const unsigned mask = 1u << button;
    if (pressed) {
        buttons_ |= mask;
    } else {
        buttons_ &= ~mask;
    }
}

int MouseFrontendState::motionButton() const {
    if (buttons_ & (1u << 0)) {
        return 1;
    }
    if (buttons_ & (1u << 2)) {
        return 2;
    }
    if (buttons_ & (1u << 1)) {
        return 3;
    }
    return 0;
}

bool MouseFrontendState::primaryButtonPressed() const {
    return buttons_ & ((1u << 0) | (1u << 1) | (1u << 2));
}

int MouseFrontendState::registerClick(int button, double x, double y, double time) {
    const double elapsed = time - lastClickTime_;
    const bool repeated = button == lastButton_ && elapsed >= 0.0 && elapsed <= 0.5 && fabs(x - lastClickX_) <= 4.0 && fabs(y - lastClickY_) <= 4.0;
    clickCount_ = repeated ? clickCount_ + 1 : 1;
    lastButton_ = button;
    lastClickTime_ = time;
    lastClickX_ = x;
    lastClickY_ = y;
    return clickCount_;
}

bool MouseFrontendState::reportMotion(int column, int row, MouseTrackingMode mode, MouseTrackingEnc encoding, u32 generation) {
    if (!hasMotionContext_ || mode != lastMotionMode_ || encoding != lastMotionEncoding_ || generation != lastMotionGeneration_) {
        hasMotionContext_ = true;
        lastMotionMode_ = mode;
        lastMotionEncoding_ = encoding;
        lastMotionGeneration_ = generation;
        hasReportedMotion_ = false;
    }
    if (hasReportedMotion_ && column == lastReportColumn_ && row == lastReportRow_) {
        return false;
    }
    hasReportedMotion_ = true;
    lastReportColumn_ = column;
    lastReportRow_ = row;
    return true;
}

void MouseFrontendState::resetMotion() {
    hasReportedMotion_ = false;
    hasMotionContext_ = false;
}
