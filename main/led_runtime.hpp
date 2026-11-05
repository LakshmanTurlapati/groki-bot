// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "conversation/led_control.hpp"

#include <cstdint>
#include <cmath>

namespace stackchan::app::led_runtime {

constexpr std::uint8_t kModeSolid = 1;

struct Solid {
    std::uint32_t color;
    std::uint8_t mode;
    std::uint8_t brightness;
};

inline Solid from_rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    return Solid{
        .color = (static_cast<std::uint32_t>(r) << 16) |
                 (static_cast<std::uint32_t>(g) << 8) |
                 static_cast<std::uint32_t>(b),
        .mode = kModeSolid,
        .brightness = 255,
    };
}

// What the strip is allowed to show. Speech outranks listen and off while
// assistant audio is still coming out of the speaker, so a listen/off message
// that arrives before the buffer drains cannot replace the white pulse.
// Sleep, an explicit Off, and startup (the default) extinguish the strip.
enum class Phase : std::uint8_t { Off = 0, Listen = 1, Speak = 2 };

struct Hold {
    bool armed{false};
    bool saw_playback{false};
};

struct Sample {
    Phase gateway{Phase::Off};
    bool playback{false};
    bool sleeping{false};
    float mouth{0.0f};
    std::uint32_t now_ms{0};
    std::uint8_t listen_r{40};
    std::uint8_t listen_g{40};
    std::uint8_t listen_b{40};
};

struct Frame {
    Phase shown{Phase::Off};
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    bool lit{false};
};

// White level for one speech frame. A live mouth envelope (conversation
// playback writes mouth_open) tracks the voice. Silence between words, or a
// speech phase that has not produced PCM yet, falls back to a slow pulse so
// the strip still reads as "speaking" and is dark only when the phase ends.
inline std::uint8_t speech_level(float mouth, std::uint32_t now_ms) noexcept
{
    if (mouth < 0.0f) {
        mouth = 0.0f;
    } else if (mouth > 1.0f) {
        mouth = 1.0f;
    }
    constexpr std::uint32_t kPeriodMs = 1100;
    const float cycle = static_cast<float>(now_ms % kPeriodMs) / static_cast<float>(kPeriodMs);
    const float wave = 0.5f - 0.5f * std::cos(cycle * 6.2831853f);
    const float pulse = 0.12f + 0.48f * wave;
    const float level = mouth >= 0.05f ? (0.18f + 0.82f * mouth) : pulse;
    const float clamped = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
    return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
}

inline Frame render(const Sample& in, Hold& hold) noexcept
{
    if (in.sleeping || in.gateway == Phase::Off) {
        hold = {};
        return {};
    }
    if (in.gateway == Phase::Speak) {
        hold.armed = true;
    }
    if (hold.armed && in.playback) {
        hold.saw_playback = true;
    }
    const bool tail = hold.armed && hold.saw_playback && in.playback;
    if (in.gateway == Phase::Speak || tail) {
        const auto level = speech_level(in.mouth, in.now_ms);
        return Frame{Phase::Speak, level, level, level, level != 0};
    }
    if (!in.playback) {
        hold = {};
    }
    if (in.gateway == Phase::Listen) {
        const bool lit = (in.listen_r | in.listen_g | in.listen_b) != 0;
        return Frame{Phase::Listen, in.listen_r, in.listen_g, in.listen_b, lit};
    }
    hold = {};
    return {};
}

} // namespace stackchan::app::led_runtime
