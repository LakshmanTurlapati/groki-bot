# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
import math

import pytest

from stackchan_mcp.robot_tracking_control import RobotTrackingControl, SmoothRobotTrackingControl


def test_center_noise_does_not_move_the_head_and_large_errors_acquire_quickly():
    control = RobotTrackingControl()
    assert control.correction(.5, .5) == (0, 0)
    assert control.correction(.51, .49) == (0, 0)
    assert control.correction(1, 0) == (15, -10)
    fine_yaw, fine_pitch = control.correction(.54, .46)
    assert 0 < fine_yaw < 2 and -2 < fine_pitch < 0


@pytest.mark.parametrize("true_fov", [45, 60, 80])
def test_stationary_target_converges_without_oscillation_across_lens_uncertainty(true_fov):
    control = RobotTrackingControl()
    error = 18.0
    previous = error
    for _ in range(8):
        pixel = .5 + math.tan(math.radians(error)) / (2 * math.tan(math.radians(true_fov / 2)))
        correction, _ = control.correction(pixel, .5)
        error -= correction
        assert abs(error) <= abs(previous)
        assert abs(correction) <= 15
        previous = error
    assert abs(error) < 1.5


def test_parameters_and_nonfinite_detections_are_rejected():
    for kwargs in ({"gain": 2}, {"horizontal_fov": float("nan")}, {"max_yaw_step": 16}):
        with pytest.raises(ValueError):
            RobotTrackingControl(**kwargs)
    for point in ((float("nan"), .5), (.5, float("inf")), (-1, .5)):
        with pytest.raises(ValueError):
            RobotTrackingControl().correction(*point)


def test_adaptive_filter_suppresses_stationary_jitter_without_averaging_away_fast_motion():
    raw = RobotTrackingControl()
    smooth = SmoothRobotTrackingControl(raw)
    raw_energy = filtered_energy = 0
    for i in range(120):
        x = .5 + .02 * math.sin(i * 1.3)
        raw_energy += raw.correction(x, .5)[0] ** 2
        filtered_energy += smooth.correction(x, .5, (0, 0), i * .4, "session")[0] ** 2
    assert filtered_energy < raw_energy * .4
    assert smooth.correction(1, .5, (0, 0), 48, "session")[0] >= 12


def test_filter_accounts_for_the_camera_rotating_toward_a_stationary_face():
    smooth = SmoothRobotTrackingControl(RobotTrackingControl())
    for i, yaw in enumerate((0, 3, 7, 10, 11)):
        image = .5 + math.tan(math.radians(12 - yaw)) / (2 * math.tan(math.radians(30)))
        correction, _ = smooth.correction(image, .5, (yaw, 0), i * .4, "session")
        assert correction >= 0
        assert smooth.filters[0].value == pytest.approx(12)


def test_filter_discards_old_target_history_on_a_pause_or_new_session():
    smooth = SmoothRobotTrackingControl(RobotTrackingControl())
    smooth.correction(0, .5, (0, 0), 1, "old")
    assert smooth.correction(1, .5, (0, 0), 1.4, "new")[0] == 15
    assert smooth.correction(0, .5, (0, 0), 5, "new")[0] == -15
    with pytest.raises(ValueError):
        smooth.correction(.5, .5, (float("nan"), 0), 6, "new")


@pytest.mark.parametrize("true_fov", [45, 60, 80])
def test_centering_reduces_persistent_offsets_with_servo_deadband(true_fov):
    for target in (4.0, 18.0):
        residuals = []
        for smooth in (False, True):
            control = SmoothRobotTrackingControl(RobotTrackingControl()) if smooth else RobotTrackingControl()
            heading = 0.0
            for i in range(12):
                pixel = .5 + math.tan(math.radians(target - heading)) / (2 * math.tan(math.radians(true_fov / 2)))
                delta, _ = (control.correction(pixel, .5, (heading, 0), i * .4, "session") if smooth
                            else control.correction(pixel, .5))
                # The independent plant models a 2-degree motion threshold
                # and residual position error, rather than an ideal motor.
                if abs(delta) > 2:
                    heading += delta - math.copysign(.8, delta)
            residuals.append(abs(target - heading))
        # Lens uncertainty changes the attainable residual; require an
        # improvement over the original controller across the tested range.
        assert residuals[1] < residuals[0] * .9
