// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "servo_task.hpp"

#include <cmath>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "groki_motion/motion.hpp"
#include "scs_servo/scs_bus.hpp"
#include "scs_servo/scs_servo.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "servo";
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(20);

// Default spring speed. While the spring is active the SCS Goal Speed is set to 0 so the servo follows
// the interpolated target without a second slow ramp on top of the spring. Goal Time = 0 means move at once.
constexpr std::uint16_t kDefaultSpringSpeed = 200;
constexpr std::uint16_t kFollowGoalSpeed = 0;
constexpr std::uint16_t kGoalTime = 0;
// Camera tracking motion profile, in degrees per second and per second squared.
constexpr float kTrackYawSpeed = 35.0f, kTrackYawAccel = 140.0f;
constexpr float kTrackPitchSpeed = 28.0f, kTrackPitchAccel = 110.0f;
// Gentler while speech plays: the servos share a power rail with the speaker amp.
constexpr float kSpeechTrackYawSpeed = 15.0f, kSpeechTrackYawAccel = 60.0f;
constexpr float kSpeechTrackPitchSpeed = 12.0f, kSpeechTrackPitchAccel = 45.0f;

void servo_task_entry(void* arg)
{
    auto& args = *static_cast<ServoTaskArgs*>(arg);

    auto bus_result = scs_servo::ScsBus::create(args.bus_cfg);
    if (!bus_result) {
        ESP_LOGE(kTag, "ScsBus::create failed: %d", static_cast<int>(bus_result.error()));
        vTaskDelete(nullptr);
        return;
    }
    auto bus = std::move(*bus_result);

    scs_servo::ScsServo yaw{bus, scs_servo::kYawId};
    scs_servo::ScsServo pitch{bus, scs_servo::kPitchId};

    bool yaw_present = false, pitch_present = false;
    if (auto r = yaw.ping(); !r) {
        ESP_LOGW(kTag, "yaw (id=%u) ping failed: %d", scs_servo::kYawId,
                 static_cast<int>(r.error()));
    } else {
        yaw_present = true;
        ESP_LOGI(kTag, "yaw (id=%u) ping OK", scs_servo::kYawId);
    }
    if (auto r = pitch.ping(); !r) {
        ESP_LOGW(kTag, "pitch (id=%u) ping failed: %d", scs_servo::kPitchId,
                 static_cast<int>(r.error()));
    } else {
        pitch_present = true;
        ESP_LOGI(kTag, "pitch (id=%u) ping OK", scs_servo::kPitchId);
    }
    args.state->servo.set_boot_nonce(esp_random());
    args.state->servo.servo_available.store(yaw_present && pitch_present);
    // Torque on demand: on before moving, released once the spring settles. While the head rests the servos stay quiet,
    // cool and can be turned by hand. When the on-device 操作 (control) switch sets servo_enabled to false,
    // torque is forced off and all motion is suppressed (limp).
    constexpr std::uint32_t kSettleMarginMs = 150;
    auto now_ms = [] { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); };
    auto target_changed = [](float a, float b) { return std::fabs(a - b) > 0.001f; };

    // Torque off at start; the first loop drives to the current commanded pose.
    std::uint16_t last_yaw_target = 0xFFFF;
    std::uint16_t last_pitch_target = 0xFFFF;
    groki_motion::SpringAxis yaw_motion;
    groki_motion::SpringAxis pitch_motion;
    bool torque_on = false;
    bool last_enabled = true;
    std::uint32_t release_at = 0; // when to drop torque after the current move
    std::uint32_t last_step_ms = now_ms();

    // Range-setting mode: torque off (so the user moves the head by hand) and
    // poll present-position every ~150 ms so the settings UIs can show / capture
    // it. Cheaper than the 20 ms control loop (each Read is a UART round-trip
    // ~3 ms; doing it at 20 ms eats half the bus and dwarfs anything else).
    bool last_range_mode = false;
    constexpr TickType_t kRangePollTicks = pdMS_TO_TICKS(150);
    TickType_t next_range_poll = 0;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        auto& control = args.state->servo;
        if (yaw_motion.initialized() && pitch_motion.initialized() &&
            control.freeze_automatic({yaw_motion.current(), pitch_motion.current()})) {
            yaw_motion.reset(yaw_motion.current());
            pitch_motion.reset(pitch_motion.current());
        }
        if (auto request = control.take_request(now_ms())) {
            auto allowed = [&] {
                // During speech only a head that already holds torque can follow: see the audio guard below.
                const bool speech_ok = !control.masked.load() || (control.engaged() && torque_on);
                return control.enabled.load() && !control.range_mode.load() && speech_ok &&
                       !args.state->audio_stream_active.load();
            };
            const bool settled = yaw_motion.initialized() && pitch_motion.initialized() && !yaw_motion.moving() &&
                                 !pitch_motion.moving();
            std::optional<HeadControl::Pose> measured;
            if (allowed() && (settled || request->motion) && !request->release) {
                auto y = yaw.read_present_position();
                auto p = pitch.read_present_position();
                if (y && p && *y <= 1023 && *p <= 1023) {
                    measured = HeadControl::Pose{scs_servo::raw_to_deg(*y, args.limits.yaw_zero),
                                                 scs_servo::raw_to_deg(*p, args.limits.pitch_zero)};
                }
            }
            auto action = control.finish(
                *request, measured, settled, allowed(), now_ms(),
                {static_cast<float>(args.limits.yaw_min_deg), static_cast<float>(args.limits.pitch_min_deg)},
                {static_cast<float>(args.limits.yaw_max_deg), static_cast<float>(args.limits.pitch_max_deg)},
                yaw_motion.initialized() && pitch_motion.initialized()
                    ? std::optional<HeadControl::Pose>{{yaw_motion.current(), pitch_motion.current()}}
                    : std::nullopt);
            if (action) {
                ESP_LOGI(kTag, "camera tracking: yaw %.1f -> %.1f, pitch %.1f -> %.1f", action->measured.yaw,
                         action->target.yaw, action->measured.pitch, action->target.pitch);
                if (!action->preserve_velocity || action->hold_yaw) {
                    yaw_motion.reset(action->measured.yaw);
                    last_yaw_target = scs_servo::deg_to_raw(action->measured.yaw, args.limits.yaw_zero);
                }
                if (!action->preserve_velocity || action->hold_pitch) {
                    pitch_motion.reset(action->measured.pitch);
                    last_pitch_target = scs_servo::deg_to_raw(action->measured.pitch, args.limits.pitch_zero);
                }
                if (!action->preserve_velocity || action->hold_yaw || action->hold_pitch)
                    last_step_ms = now_ms();
                // Motion-aware targets already refer to the measured camera
                // heading. Keep the commanded trajectory intact: rebasing it
                // to lagging feedback would introduce a backward goal step.
            }
        }
        const bool range_mode = args.state->servo.range_mode.load(std::memory_order_relaxed);
        if (range_mode) {
            if (torque_on || !last_range_mode) {
                (void)yaw.enable_torque(false);
                (void)pitch.enable_torque(false);
                torque_on = false;
            }
            last_range_mode = true;
            last_enabled = false; // force re-drive on exit
            const TickType_t now = xTaskGetTickCount();
            // Signed cast so the subtraction handles TickType_t wraparound
            // ("now has passed next_range_poll" without a raw unsigned compare).
            if (static_cast<std::int32_t>(now - next_range_poll) >= 0) {
                next_range_poll = now + kRangePollTicks;
                if (auto r = yaw.read_present_position()) {
                    args.state->servo.yaw_raw.store(static_cast<std::int16_t>(*r),
                                                    std::memory_order_relaxed);
                }
                if (auto r = pitch.read_present_position()) {
                    args.state->servo.pitch_raw.store(static_cast<std::int16_t>(*r),
                                                      std::memory_order_relaxed);
                }
            }
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            continue;
        }
        if (last_range_mode) {
            // Leaving range-setting mode: reset the springs to the position just read, then drop the old readings.
            if (const auto raw = args.state->servo.yaw_raw.load(std::memory_order_relaxed); raw >= 0) {
                yaw_motion.reset(clamp_deg(scs_servo::raw_to_deg(static_cast<std::uint16_t>(raw),
                                                                 args.limits.yaw_zero),
                                           args.limits.yaw_min_deg, args.limits.yaw_max_deg));
            }
            if (const auto raw = args.state->servo.pitch_raw.load(std::memory_order_relaxed); raw >= 0) {
                pitch_motion.reset(clamp_deg(scs_servo::raw_to_deg(static_cast<std::uint16_t>(raw),
                                                                   args.limits.pitch_zero),
                                             args.limits.pitch_min_deg, args.limits.pitch_max_deg));
            }
            args.state->servo.yaw_raw.store(-1, std::memory_order_relaxed);
            args.state->servo.pitch_raw.store(-1, std::memory_order_relaxed);
            last_yaw_target = last_pitch_target = 0xFFFF; // re-drive
            last_range_mode = false;
        }

        const bool enabled = args.state->servo.enabled.load(std::memory_order_relaxed);
        if (!enabled) {
            if (torque_on) {
                (void)yaw.enable_torque(false);
                (void)pitch.enable_torque(false);
                torque_on = false;
            }
            last_enabled = false;
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            continue;
        }
        if (!last_enabled) {
            // Re-enabled: drive to the current commanded pose again.
            last_yaw_target = last_pitch_target = 0xFFFF;
            last_enabled = true;
        }

        // Audio guard: the servos and the speaker amp/codec share a power rail,
        // and the current draw of a move (or the transient of toggling torque)
        // sags it enough to glitch / cut the audio. While speech output is in
        // progress, hold the head perfectly still — no goal writes, no torque
        // changes. The mask is set/cleared by the application at speech
        // start/end (conversation reply, idle babble); BLE / Wi-Fi streaming
        // uses audio_stream_active. We never poll the speaker directly: its
        // isPlaying() flickers false between streamed reply segments and would
        // let the head twitch mid-reply.
        //
        // One exception: in Engaged mode a reply lets camera tracking keep
        // moving with torque already on, on the gentler speech profile. Torque
        // never changes during speech, so the head only follows if it was
        // engaged before the reply began (see below).
        const bool speech = control.masked.load(std::memory_order_relaxed);
        const bool speech_tracking = speech && torque_on && control.engaged() && control.target_is_tracking();
        if (args.state->audio_stream_active.load(std::memory_order_relaxed) || (speech && !speech_tracking)) {
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            continue;
        }

        // Clamp commanded angles to the configured motion range so callers
        // (demo, conversation, future UI) all stay within the per-device limits.
        const auto command = args.state->servo.take_target();
        if (!command.tracking) {
            yaw_motion.set_motion_limits(0, 0);
            pitch_motion.set_motion_limits(0, 0);
        } else if (speech) {
            yaw_motion.set_motion_limits(kSpeechTrackYawSpeed, kSpeechTrackYawAccel);
            pitch_motion.set_motion_limits(kSpeechTrackPitchSpeed, kSpeechTrackPitchAccel);
        } else {
            yaw_motion.set_motion_limits(kTrackYawSpeed, kTrackYawAccel);
            pitch_motion.set_motion_limits(kTrackPitchSpeed, kTrackPitchAccel);
        }
        const auto pose = command.pose;
        const float yaw_deg = clamp_deg(pose.yaw, args.limits.yaw_min_deg, args.limits.yaw_max_deg);
        const float pitch_deg = clamp_deg(pose.pitch, args.limits.pitch_min_deg, args.limits.pitch_max_deg);

        if (!yaw_motion.initialized()) {
            if (auto r = yaw.read_present_position()) {
                yaw_motion.reset(clamp_deg(scs_servo::raw_to_deg(*r, args.limits.yaw_zero),
                                           args.limits.yaw_min_deg, args.limits.yaw_max_deg));
            } else {
                yaw_motion.reset(yaw_deg);
            }
            last_yaw_target = 0xFFFF; // force first goal write
        }
        if (!pitch_motion.initialized()) {
            if (auto r = pitch.read_present_position()) {
                pitch_motion.reset(clamp_deg(scs_servo::raw_to_deg(*r, args.limits.pitch_zero),
                                             args.limits.pitch_min_deg, args.limits.pitch_max_deg));
            } else {
                pitch_motion.reset(pitch_deg);
            }
            last_pitch_target = 0xFFFF; // force first goal write
        }

        // A conversation may start a reply at any moment, and torque must not
        // change once it does. Re-driving the current pose turns torque on now.
        const bool engaged = control.engaged();
        if (engaged && !torque_on) {
            last_yaw_target = last_pitch_target = 0xFFFF;
        }

        if (target_changed(yaw_deg, yaw_motion.target()) ||
            target_changed(pitch_deg, pitch_motion.target())) {
            // Speed belongs to the same command snapshot as both target axes.
            const std::uint16_t speed = command.speed != 0 ? command.speed : kDefaultSpringSpeed;
            yaw_motion.retarget(yaw_deg, speed);
            pitch_motion.retarget(pitch_deg, speed);
        }

        const std::uint32_t step_now_ms = now_ms();
        float dt_s = static_cast<float>(step_now_ms - last_step_ms) / 1000.0f;
        last_step_ms = step_now_ms;
        if (dt_s <= 0.0f) dt_s = 0.02f;
        if (dt_s > 0.05f) dt_s = 0.05f;

        (void)yaw_motion.step(dt_s,
                              static_cast<float>(args.limits.yaw_min_deg),
                              static_cast<float>(args.limits.yaw_max_deg));
        (void)pitch_motion.step(dt_s,
                                static_cast<float>(args.limits.pitch_min_deg),
                                static_cast<float>(args.limits.pitch_max_deg));

        const std::uint16_t yaw_target = scs_servo::deg_to_raw(yaw_motion.current(),
                                                               args.limits.yaw_zero);
        const std::uint16_t pitch_target = scs_servo::deg_to_raw(pitch_motion.current(),
                                                                 args.limits.pitch_zero);
        const bool yaw_write = yaw_target != last_yaw_target;
        const bool pitch_write = pitch_target != last_pitch_target;

        if (yaw_write || pitch_write) {
            // Engage torque just before driving.
            if (!torque_on) {
                (void)yaw.enable_torque(true);
                (void)pitch.enable_torque(true);
                torque_on = true;
            }
            if (yaw_write) {
                (void)yaw.write_goal_position(yaw_target, kGoalTime, kFollowGoalSpeed);
                last_yaw_target = yaw_target;
            }
            if (pitch_write) {
                (void)pitch.write_goal_position(pitch_target, kGoalTime, kFollowGoalSpeed);
                last_pitch_target = pitch_target;
            }
            release_at = step_now_ms + kSettleMarginMs;
        } else if (torque_on && !speech && !engaged && !yaw_motion.moving() && !pitch_motion.moving() &&
                   step_now_ms >= release_at && !control.tracking_holds(step_now_ms)) {
            // Move complete and holding still → release torque.
            (void)yaw.enable_torque(false);
            (void)pitch.enable_torque(false);
            torque_on = false;
        }

        vTaskDelayUntil(&last_wake, kPeriodTicks);
    }
}

} // namespace

void start_servo_task(ServoTaskArgs& args)
{
    const auto started = xTaskCreatePinnedToCore(servo_task_entry, "servo", 8192, &args, 4, nullptr, 0) == pdPASS;
    args.state->servo.servo_service_started.store(started);
    if (!started) {
        ESP_LOGE(kTag, "servo task could not be created");
    }
}

} // namespace stackchan::app
