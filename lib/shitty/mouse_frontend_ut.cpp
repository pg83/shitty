/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include <lib/vterm/mouse_frontend.h>

#include <std/tst/ut.h>

using namespace stl;

STD_TEST_SUITE(MouseFrontend) {
    STD_TEST(ConvertsPixelsToCellCoordinates) {
        const MouseGeometry geometry{
            .textWidth = 80,
            .textHeight = 64,
            .glyphWidth = 8,
            .glyphHeight = 16,
        };

        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 0, 0, geometry).column == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 0, 0, geometry).row == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 79, 63, geometry).column == 10);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 79, 63, geometry).row == 4);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, -102, -102, geometry).column == 1);

        // The first pixel of a cell belongs to that cell, the last one to
        // the same cell — no drift at the boundary.
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 7, 0, geometry).column == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 8, 0, geometry).column == 2);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 0, 15, geometry).row == 1);
        STD_INSIST(mouseProtocolPoint(MouseTrackingEnc::SGR, 0, 16, geometry).row == 2);
    }

    STD_TEST(SgrPixelsUseContentPixelCoordinates) {
        const MouseGeometry geometry{
            .textWidth = 100,
            .textHeight = 50,
            .glyphWidth = 8,
            .glyphHeight = 16,
        };

        const MouseProtocolPoint first = mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 0, 0, geometry);
        const MouseProtocolPoint last = mouseProtocolPoint(MouseTrackingEnc::SGRPixels, 99, 49, geometry);

        STD_INSIST(first.column == 1);
        STD_INSIST(first.row == 1);
        STD_INSIST(last.column == 100);
        STD_INSIST(last.row == 50);
    }

    STD_TEST(MapsModifiersAndButtons) {
        STD_INSIST(mouseProtocolModifiers(FrontendShift | FrontendControl | FrontendAlt) == (MouseShift | MouseControl | MouseAlt));
        STD_INSIST(mouseProtocolModifiers(FrontendShift | FrontendControl | FrontendAlt, false) == (MouseShift | MouseControl));
        STD_INSIST(mouseTerminalButton(0) == 1);
        STD_INSIST(mouseTerminalButton(2) == 2);
        STD_INSIST(mouseTerminalButton(1) == 3);
        STD_INSIST(mouseTerminalButton(3) == 8);
        STD_INSIST(mouseTerminalButton(6) == 11);
        STD_INSIST(mouseTerminalButton(-1) == 0);
    }

    STD_TEST(AppliesButtonReportingRules) {
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::Disabled, MouseEventType::Press, 1));
        STD_INSIST(mouseButtonReportAllowed(MouseTrackingMode::VT200, MouseEventType::Press, 1));
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::X10_Compat, MouseEventType::Release, 1));
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::VT200, MouseEventType::Release, 4));
        STD_INSIST(!mouseButtonReportAllowed(MouseTrackingMode::VT200, MouseEventType::Press, 12));
    }

    STD_TEST(AccumulatesFractionalWheelSteps) {
        MouseWheelAccumulator wheel;

        STD_INSIST(wheel.consume(0.0, 0.4, false).y == 0);
        STD_INSIST(wheel.consume(0.0, 0.4, false).y == 0);
        STD_INSIST(wheel.consume(0.0, 0.4, false).y == 1);
        STD_INSIST(wheel.consume(0.0, -0.5, false).y == 0);
        STD_INSIST(wheel.consume(0.0, -0.8, false).y == -1);
    }

    STD_TEST(ResetsWheelRemaindersWhenModeChanges) {
        MouseWheelAccumulator wheel;
        wheel.consume(0.75, 0.75, false);

        const MouseWheelSteps reporting = wheel.consume(0.5, 0.5, true);

        STD_INSIST(reporting.x == 0);
        STD_INSIST(reporting.y == 0);
        STD_INSIST(wheel.consume(0.5, 0.5, true).x == 1);
        wheel.reset();
        STD_INSIST(wheel.consume(0.5, 0.5, true).x == 0);
    }

    STD_TEST(ResetsWheelRemaindersAtGestureBoundaries) {
        MouseWheelAccumulator wheel;
        wheel.consume(0.0, 0.75, false);
        STD_INSIST(wheel.consume(0.0, 0.25, false, plt::ScrollPhase::Begin).y == 0);

        wheel.consume(0.0, 0.75, false);
        STD_INSIST(wheel.consume(0.0, 0.25, false, plt::ScrollPhase::End).y == 0);
        STD_INSIST(wheel.consume(0.0, 0.75, false).y == 0);

        wheel.reset();
        wheel.consume(0.0, 0.75, false, plt::ScrollPhase::None, false, 1.0);
        STD_INSIST(wheel.consume(0.0, 0.25, false, plt::ScrollPhase::None, false, 1.2).y == 0);

        wheel.reset();
        wheel.consume(0.0, 0.75, false);
        STD_INSIST(wheel.consume(0.0, 0.25, false, plt::ScrollPhase::None, true).y == 0);
    }

    STD_TEST(WheelReversalDoesNotInheritTheOldFraction) {
        MouseWheelAccumulator wheel;
        wheel.consume(0.0, 0.9, false);

        STD_INSIST(wheel.consume(0.0, -1.0, false).y == -1);
        STD_INSIST(wheel.consume(0.0, -0.25, false).y == 0);
    }

    STD_TEST(TracksPressedButtonsAndMotionButtonPriority) {
        MouseFrontendState state;
        state.updateButton(1, true);
        state.updateButton(2, true);
        state.updateButton(0, true);

        STD_INSIST(state.buttons() == 7);
        STD_INSIST(state.primaryButtonPressed());
        STD_INSIST(state.motionButton() == 1);

        state.updateButton(0, false);
        STD_INSIST(state.motionButton() == 2);
        state.clearButtons();
        STD_INSIST(state.buttons() == 0);
        STD_INSIST(!state.primaryButtonPressed());
    }

    STD_TEST(SuppressesProtocolDuringSelectionOrShift) {
        MouseFrontendState state;

        STD_INSIST(state.protocolActive(0, MouseTrackingMode::VT200));
        STD_INSIST(!state.protocolActive(FrontendShift, MouseTrackingMode::VT200));
        STD_INSIST(!state.protocolActive(0, MouseTrackingMode::Disabled));
        state.beginSelection();
        STD_INSIST(!state.protocolActive(0, MouseTrackingMode::VT200));
        state.endSelection();
        STD_INSIST(state.protocolActive(0, MouseTrackingMode::VT200));
    }

    STD_TEST(CountsOnlyNearbyRapidClicks) {
        MouseFrontendState state;

        STD_INSIST(state.registerClick(0, 10.0, 10.0, 1.0) == 1);
        STD_INSIST(state.registerClick(0, 12.0, 12.0, 1.4) == 2);
        STD_INSIST(state.registerClick(0, 12.0, 12.0, 1.8) == 3);
        STD_INSIST(state.registerClick(0, 20.0, 12.0, 2.0) == 1);
        STD_INSIST(state.registerClick(1, 20.0, 12.0, 2.1) == 1);
        STD_INSIST(state.registerClick(1, 20.0, 12.0, 1.0) == 1);
    }

    STD_TEST(DeduplicatesMotionWithinSameContext) {
        MouseFrontendState state;

        STD_INSIST(state.reportMotion(1, 1, MouseTrackingMode::VT200, MouseTrackingEnc::SGR, 1));
        STD_INSIST(!state.reportMotion(1, 1, MouseTrackingMode::VT200, MouseTrackingEnc::SGR, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200, MouseTrackingEnc::SGR, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::SGR, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::UTF8, 1));
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::UTF8, 2));
        state.resetMotion();
        STD_INSIST(state.reportMotion(2, 1, MouseTrackingMode::VT200_ButtonEvent, MouseTrackingEnc::UTF8, 2));
    }
}
