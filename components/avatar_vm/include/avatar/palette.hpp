// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>

namespace stackchan::avatar {

// 16-bit RGB565 palette. Colours match m5stack-avatar-rs defaults.
struct Palette {
    std::uint16_t primary;
    std::uint16_t background;
    std::uint16_t secondary;
    std::uint16_t balloon_foreground;
    std::uint16_t balloon_background;
};

inline constexpr Palette kDefaultPalette{
    .primary = 0x73AEu,            // grey #777777 (face body, Grok Bot design)
    .background = 0x1082u,         // #121212 (ground; eyes are holes in it)
    .secondary = 0xFFE0u,          // yellow (accent)
    // Balloon uses inverted colours so it stands out against the dark face:
    // a solid white panel with black text.
    .balloon_foreground = 0x0000u, // black  (text)
    .balloon_background = 0xFFFFu, // white  (panel fill)
};

} // namespace stackchan::avatar
