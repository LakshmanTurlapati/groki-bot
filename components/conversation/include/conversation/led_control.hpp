// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <cstring>

namespace stackchan::conversation::led_control {

// Voice-status light requested by a XiaoZhi ``led`` message.
// Ignore: a bare non-zero RGB write from another controller. It must not
// light the strip or replace speech white.
enum class LedWire : std::uint8_t { Ignore = 0, Off = 1, Listen = 2, Speak = 3 };

struct LedWireCommand {
    LedWire command{LedWire::Ignore};
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
};

inline constexpr std::uint8_t kListenDefaultR = 40;
inline constexpr std::uint8_t kListenDefaultG = 40;
inline constexpr std::uint8_t kListenDefaultB = 40;

// ``mode`` is the optional JSON string. ``has_rgb`` is true when r, g and b
// were all present and in range. A legacy ``{"type":"led","r":0,"g":0,"b":0}``
// is Off. Any other legacy colour is Ignore so a stale blue or a model
// set_all_leds cannot override the voice policy.
inline LedWireCommand parse_led_wire(const char* mode, bool has_rgb, std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b) noexcept
{
    if (mode != nullptr && mode[0] != '\0') {
        if (std::strcmp(mode, "off") == 0) {
            return LedWireCommand{LedWire::Off, 0, 0, 0};
        }
        if (std::strcmp(mode, "listen") == 0) {
            if (!has_rgb) {
                return LedWireCommand{LedWire::Listen, kListenDefaultR, kListenDefaultG, kListenDefaultB};
            }
            return LedWireCommand{LedWire::Listen, r, g, b};
        }
        if (std::strcmp(mode, "speech") == 0 || std::strcmp(mode, "speak") == 0) {
            return LedWireCommand{LedWire::Speak, 255, 255, 255};
        }
        return LedWireCommand{};
    }
    if (!has_rgb) {
        return LedWireCommand{};
    }
    if (r == 0 && g == 0 && b == 0) {
        return LedWireCommand{LedWire::Off, 0, 0, 0};
    }
    return LedWireCommand{LedWire::Ignore, r, g, b};
}

} // namespace stackchan::conversation::led_control
