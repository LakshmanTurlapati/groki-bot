// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace stackchan::avatar {

// SharedState stores this as u8. 0-5 are the original six and must not
// move. The extended expressions occupy 6-12 (Idle is an alias of Neutral).
// Affection is the stroke face (squinting smile). Bored is the idle-decay
// look-around face (appended after them so 0-12 keep their values).
// Working..Failed (15-19) are the Grok Bot run states the gateway reports
// through the activity channel (see ExpressionController::set_activity).
enum class Expression : std::uint8_t {
    Neutral = 0,
    Happy = 1,
    Sad = 2,
    Angry = 3,
    Doubt = 4,
    Sleepy = 5,
    Listening = 6,
    Thinking = 7,
    Excited = 8,
    Curious = 9,
    Confused = 10,
    Surprised = 11,
    Dizzy = 12,
    Affection = 13,
    Bored = 14,
    Working = 15,
    Waiting = 16,
    Blocked = 17,
    Done = 18,
    Failed = 19,
};

inline constexpr std::size_t kExpressionCount = static_cast<std::size_t>(Expression::Failed) + 1;

// Coarse voice / wait state written by the conversation task. The
// expression controller maps Listening/Thinking onto persistent base
// faces; Speaking yields to the conversation mood if one is set.
enum class VoiceState : std::uint8_t {
    Idle = 0,
    Listening = 1,
    Thinking = 2,
    Speaking = 3,
};

} // namespace stackchan::avatar
