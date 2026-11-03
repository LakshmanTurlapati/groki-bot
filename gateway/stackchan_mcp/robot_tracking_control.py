# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Convert image error to bounded camera rotations using a pinhole model."""

from __future__ import annotations

import math
import os
from dataclasses import dataclass


@dataclass(frozen=True)
class RobotTrackingControl:
    horizontal_fov: float = 60.0
    vertical_fov: float = 46.8
    gain: float = 0.9
    deadzone: float = 0.012
    max_yaw_step: float = 15.0
    max_pitch_step: float = 10.0

    def __post_init__(self):
        if not (20 <= self.horizontal_fov <= 120 and 15 <= self.vertical_fov <= 100
                and 0.1 <= self.gain <= 1 and 0 <= self.deadzone <= .05
                and 0 < self.max_yaw_step <= 15 and 0 < self.max_pitch_step <= 10):
            raise ValueError("Invalid robot tracking control parameters")

    @classmethod
    def from_env(cls):
        return cls(
            horizontal_fov=float(os.getenv("STACKCHAN_TRACKING_HFOV", "60")),
            vertical_fov=float(os.getenv("STACKCHAN_TRACKING_VFOV", "46.8")),
            gain=float(os.getenv("STACKCHAN_TRACKING_GAIN", "0.9")),
        )

    def correction(self, x: float, y: float) -> tuple[float, float]:
        if not all(math.isfinite(v) and 0 <= v <= 1 for v in (x, y)):
            raise ValueError("Face coordinates must be finite and normalized")

        def axis(position: float, fov: float, maximum: float):
            error = position - .5
            if abs(error) <= self.deadzone:
                return 0.0
            error = math.copysign(abs(error) - self.deadzone, error)
            angle = math.degrees(math.atan(2 * error * math.tan(math.radians(fov / 2))))
            # Use the full gain to acquire a distant target, reducing it
            # smoothly near the center to damp detector noise and overshoot.
            gain = self.gain * (.78 + .22 * min(1.0, abs(angle) / 10))
            return max(-maximum, min(maximum, gain * angle))

        return axis(x, self.horizontal_fov, self.max_yaw_step), axis(y, self.vertical_fov, self.max_pitch_step)


class AdaptiveAngleFilter:
    """Speed-adaptive low-pass filtering with the actual sample interval."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.value = self.raw = self.at = None
        self.derivative = 0.0

    @staticmethod
    def alpha(cutoff, dt):
        return 1 / (1 + 1 / (2 * math.pi * cutoff * dt))

    def sample(self, value, at):
        if self.at is None or not 0 < at - self.at <= 1.2:
            self.value = self.raw = value
            self.at = at
            self.derivative = 0.0
            return value
        dt = at - self.at
        derivative = (value - self.raw) / dt
        self.derivative += self.alpha(1.0, dt) * (derivative - self.derivative)
        cutoff = 1.2 + .08 * abs(self.derivative)
        self.value += self.alpha(cutoff, dt) * (value - self.value)
        self.raw, self.at = value, at
        return self.value


class SmoothRobotTrackingControl:
    """Filter target bearings so the robot's own rotation is not target motion."""

    def __init__(self, config: RobotTrackingControl):
        self.config = config
        self.filters = (AdaptiveAngleFilter(), AdaptiveAngleFilter())
        self.reset()

    def reset(self):
        for filter_ in self.filters:
            filter_.reset()
        self.holding = [False, False]
        self.session = None

    def correction(self, x, y, pose, at, session):
        if not all(math.isfinite(v) for v in (x, y, *pose, at)) or not (0 <= x <= 1 and 0 <= y <= 1):
            raise ValueError("Tracking observation must be finite and normalized")
        if session != self.session:
            self.reset()
            self.session = session
        config = self.config
        corrections = []
        for axis, (position, head, fov, maximum) in enumerate(zip(
            (x, y), pose, (config.horizontal_fov, config.vertical_fov),
            (config.max_yaw_step, config.max_pitch_step), strict=True,
        )):
            scale = 2 * math.tan(math.radians(fov / 2))
            angle = math.degrees(math.atan((position - .5) * scale))
            target = self.filters[axis].sample(head + angle, at)
            error = target - head
            deadzone = math.degrees(math.atan(config.deadzone * scale))
            threshold = deadzone * (1.8 if self.holding[axis] else 1)
            self.holding[axis] = abs(error) <= threshold
            if self.holding[axis]:
                corrections.append(0.0)
                continue
            error = math.copysign(max(0, abs(error) - deadzone), error)
            gain = config.gain * (.78 + .22 * min(1, abs(error) / 10))
            corrections.append(max(-maximum, min(maximum, gain * error)))
        return tuple(corrections)
