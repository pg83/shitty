/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "mouse_protocol.h"

#include <std/sys/types.h>

#include <plt/input.h>

enum FrontendModifier : unsigned {
    FrontendShift = 1,
    FrontendControl = 2,
    FrontendAlt = 4,
};

struct MouseGeometry {
    u32 textWidth = 1;
    u32 textHeight = 1;
    int glyphWidth = 1;
    int glyphHeight = 1;
};

struct MouseProtocolPoint {
    int column = 1;
    int row = 1;
};

MouseProtocolPoint mouseProtocolPoint(MouseTrackingEnc encoding, int pixelX, int pixelY, const MouseGeometry& geometry);
unsigned mouseProtocolModifiers(unsigned frontendModifiers, bool reportAlt = true);
int mouseTerminalButton(int frontendButton);
bool mouseButtonReportAllowed(MouseTrackingMode mode, MouseEventType type, int button);

struct MouseWheelSteps {
    int x = 0;
    int y = 0;
};

class MouseWheelAccumulator {
public:
    MouseWheelSteps consume(double x, double y, bool reporting, plt::ScrollPhase phase = plt::ScrollPhase::None, bool momentum = false, double time = 0);
    void reset();

private:
    static int consumeAxis(double delta, double& remainder);
    void clearRemainders();

    bool reporting_ = false;
    bool momentum_ = false;
    double remainderX_ = 0.0;
    double remainderY_ = 0.0;
    double lastTime_ = 0.0;
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

    MouseWheelSteps consumeWheel(double x, double y, bool reporting, plt::ScrollPhase phase = plt::ScrollPhase::None, bool momentum = false, double time = 0) {
        return wheel_.consume(x, y, reporting, phase, momentum, time);
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
