// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "led_task.hpp"

#include "led_runtime.hpp"

#include <cstdint>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "led";
// 10 Hz is enough for breathing / rainbow visually, and triples the I2C bus
// + CPU 1 headroom we used to spend at 30 Hz. Dropped from 30 Hz on
// 2026-06-07 after task_wdt on IDLE1 started firing once Phase 2 SSE +
// conv-task TLS + LED + render + speaker/mic all crowded CPU 1 (touch taps
// were being dropped, render dt stretched). See docs/known_issues.md §1.
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(100);

using led_runtime::Hold;
using led_runtime::Phase;
using led_runtime::Sample;

void led_task_entry(void* arg)
{
    auto& args = *static_cast<LedTaskArgs*>(arg);
    auto& strip = *args.strip;
    auto& state = *args.state;
    const std::size_t n = strip.size();
    if (n == 0) {
        ESP_LOGW(kTag, "strip size = 0, exiting");
        vTaskDelete(nullptr);
        return;
    }

    // Frame counter — drives breath phase and gradient scroll. Using a wall-
    // clock-derived value (esp_timer) instead of a frame index keeps animations
    // running at the right speed even if the task ever gets paused / preempted.
    auto now_ms = [] { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); };

    TickType_t last_wake = xTaskGetTickCount();
    Hold hold{};
    for (;;) {
        // Camera session: the strip refresh reaches the PY32 over In_I2C on
        // some boards, which would re-init the I2C controller under the
        // camera's SCCB driver. Skip the whole frame while quiesced (LEDs
        // just hold their last state for the ~1.5 s session).
        if (state.i2c_quiesce.load(std::memory_order_acquire)) {
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            continue;
        }
        // Voice status owns the strip. Saved rainbow/breath settings stay in
        // NVS but do not light the base while the robot is idle, asleep, or
        // between chats. Speech white is rendered here so a later solid
        // colour cannot replace the pulse until playback and the speech
        // phase have both ended.
        const auto packed = state.led.voice_color.load(std::memory_order_relaxed);
        const auto frame = led_runtime::render(
            Sample{
                .gateway = static_cast<Phase>(state.led.voice_phase.load(std::memory_order_relaxed)),
                .playback = state.led.playback_active.load(std::memory_order_relaxed),
                .sleeping = state.servo.sleeping(),
                .mouth = state.face.mouth_open.load(std::memory_order_relaxed),
                .now_ms = now_ms(),
                .listen_r = static_cast<std::uint8_t>((packed >> 16) & 0xFF),
                .listen_g = static_cast<std::uint8_t>((packed >> 8) & 0xFF),
                .listen_b = static_cast<std::uint8_t>(packed & 0xFF),
            },
            hold);
        if (!frame.lit) {
            strip.clear();
        } else {
            strip.fill(frame.r, frame.g, frame.b);
        }

        (void)strip.show();
        vTaskDelayUntil(&last_wake, kPeriodTicks);
    }
}

} // namespace

void start_led_task(LedTaskArgs& args)
{
    // 4 KiB is comfortable for the sin/HSV math + 64 B local frame buffer.
    // Core 1 keeps the I2C bursts off core 0 where NimBLE + Wi-Fi live.
    xTaskCreatePinnedToCore(led_task_entry, "led", 4096, &args, 2, nullptr, 1);
}

} // namespace stackchan::app
