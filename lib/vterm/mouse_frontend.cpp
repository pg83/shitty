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

MouseProtocolPoint mouseProtocolPoint(MouseTrackingEnc encoding, int pixelX, int pixelY, const MouseGeometry& geometry) {
    const int contentWidth = max(1, (int)(min<u32>(geometry.textWidth, INT_MAX)));
    const int contentHeight = max(1, (int)(min<u32>(geometry.textHeight, INT_MAX)));
    const int x = min(max(pixelX, 0), contentWidth - 1);
    const int y = min(max(pixelY, 0), contentHeight - 1);
    if (encoding == MouseTrackingEnc::SGRPixels) {
        return {x + 1, y + 1};
    }
    return {
        x / max(1, geometry.glyphWidth) + 1,
        y / max(1, geometry.glyphHeight) + 1,
    };
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
    // A reversal starts a new direction: an old fraction must not consume
    // the first step of the new one or produce a step in the old direction.
    if (remainder * delta < 0.0) {
        remainder = 0.0;
    }
    const double total = remainder + min(max(delta, -100.0), 100.0);
    const int steps = (int)(trunc(total));
    remainder = total - steps;
    return steps;
}

void MouseWheelAccumulator::clearRemainders() {
    remainderX_ = 0.0;
    remainderY_ = 0.0;
}

MouseWheelSteps MouseWheelAccumulator::consume(double x, double y, bool reporting, plt::ScrollPhase phase, bool momentum, double time) {
    // Native timestamps are seconds. Unknown timestamps keep the legacy
    // behavior; a backwards jump or a pause starts a fresh accumulation.
    const bool timedOut = time > 0.0 && lastTime_ > 0.0 && (time < lastTime_ || time - lastTime_ > 0.1);
    if (reporting != reporting_ || momentum != momentum_ || phase == plt::ScrollPhase::Begin || timedOut) {
        clearRemainders();
    }
    reporting_ = reporting;
    momentum_ = momentum;

    MouseWheelSteps steps;
    steps.y = consumeAxis(y, remainderY_);
    if (reporting) {
        steps.x = consumeAxis(x, remainderX_);
    } else {
        remainderX_ = 0.0;
    }
    if (phase == plt::ScrollPhase::End || phase == plt::ScrollPhase::Cancel) {
        clearRemainders();
        lastTime_ = 0.0;
    } else {
        lastTime_ = time > 0.0 && isfinite(time) ? time : 0.0;
    }
    return steps;
}

void MouseWheelAccumulator::reset() {
    reporting_ = false;
    momentum_ = false;
    lastTime_ = 0.0;
    clearRemainders();
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
