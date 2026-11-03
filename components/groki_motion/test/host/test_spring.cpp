// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "groki_motion/motion.hpp"
#include "test_support.hpp"

using stackchan::groki_motion::SpringAxis;

int main() {
    for (const auto speed : {200, 900, 1000}) {
        for (const float dt : {0.02f, 0.035f, 0.05f}) {
            for (const float target : {-15.0f, 15.0f}) {
                SpringAxis axis;
                axis.reset(0);
                axis.retarget(target, static_cast<std::uint16_t>(speed));
                float previous = 0;
                for (int tick = 0; tick < 300; ++tick) {
                    axis.step(dt, -90, 90);
                    const float value = axis.current();
                    CHECK(std::isfinite(value));
                    CHECK(value * target >= -0.001f);
                    CHECK(std::fabs(value) <= std::fabs(target) + 0.001f);
                    CHECK(std::fabs(value) + 0.001f >= std::fabs(previous));
                    previous = value;
                }
                CHECK(!axis.moving());
                CHECK(std::fabs(axis.current() - target) < 0.001f);
            }
        }
    }

    SpringAxis axis;
    axis.reset(0);
    for (const float target : {150.0f, -150.0f}) {
        axis.retarget(target, 900);
        for (int tick = 0; tick < 100; ++tick) {
            axis.step(tick % 3 == 0 ? 0.05f : 0.02f, -30, 30);
            CHECK(std::isfinite(axis.current()) && std::fabs(axis.current()) <= 30);
        }
        CHECK(!axis.moving());
        CHECK(axis.current() == (target > 0 ? 30 : -30));
    }
    SpringAxis continuous;
    continuous.reset(0);
    continuous.set_motion_limits(35, 140);
    continuous.retarget(25, 900);
    for (int tick = 0; tick < 200; ++tick) {
        const float dt = tick % 3 == 0 ? .05f : .02f;
        const float before = continuous.velocity();
        if (tick == 12 || tick == 30) {
            const float position = continuous.current();
            continuous.retarget(tick == 12 ? -15 : 10, 900);
            CHECK(continuous.velocity() == before);
            CHECK(continuous.current() == position);
        }
        continuous.step(dt, -40, 40);
        CHECK(std::fabs(continuous.velocity()) <= 35.001f);
        CHECK(std::fabs(continuous.velocity() - before) <= 140 * dt + .001f);
        CHECK(std::fabs(continuous.current()) <= 40);
    }
    CHECK(!continuous.moving());
    CHECK(continuous.current() == 10);
    return motiontest::finish("spring");
}
