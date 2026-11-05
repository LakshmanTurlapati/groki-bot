// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace stackchan::conversation {

enum class HeadTrackingMode : unsigned char { Off, Tracking, Conversation, Engaged };
struct HeadTrackingCommand {
    std::string frame_id;
    float delta_yaw = 0, delta_pitch = 0;
    bool release = false;
};

// Head pose requested by the gateway's `head` control message. Angles are in
// degrees; speed is the servo speed hint (100..1000, 0 = default), not a
// duration.
struct HeadCommand {
    float yaw;
    float pitch;
    std::uint16_t speed;
};

// Holds a head command that arrives while the robot is speaking. The last
// command wins; the application applies it on the Speaking -> non-Speaking
// transition so a reply cannot swallow the requested pose.
class HeadCommandQueue {
public:
    std::optional<HeadCommand> receive(HeadCommand command, bool speaking)
    {
        if (!speaking) return command;
        pending_ = command;
        return std::nullopt;
    }

    std::optional<HeadCommand> finish()
    {
        auto command = pending_;
        pending_.reset();
        return command;
    }

private:
    std::optional<HeadCommand> pending_;
};

} // namespace stackchan::conversation
