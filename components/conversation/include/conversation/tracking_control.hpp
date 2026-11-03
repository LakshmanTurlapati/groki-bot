// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <cJSON.h>
#include "conversation/head_command.hpp"

namespace stackchan::conversation {

inline std::optional<HeadTrackingMode> parse_head_tracking_mode(const cJSON* root) {
    const auto* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (!cJSON_IsString(mode))
        return {};
    if (std::strcmp(mode->valuestring, "off") == 0)
        return HeadTrackingMode::Off;
    if (std::strcmp(mode->valuestring, "tracking") == 0)
        return HeadTrackingMode::Tracking;
    if (std::strcmp(mode->valuestring, "conversation") == 0)
        return HeadTrackingMode::Conversation;
    return {};
}

inline std::optional<HeadTrackingCommand> parse_head_track(const cJSON* root) {
    const auto* id = cJSON_GetObjectItemCaseSensitive(root, "frame_id");
    if (!cJSON_IsString(id) || std::strlen(id->valuestring) != 16)
        return {};
    for (const char* p = id->valuestring; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return {};
    }
    const auto* release = cJSON_GetObjectItemCaseSensitive(root, "release");
    const auto* yaw = cJSON_GetObjectItemCaseSensitive(root, "delta_yaw");
    const auto* pitch = cJSON_GetObjectItemCaseSensitive(root, "delta_pitch");
    if (release && !cJSON_IsBool(release))
        return {};
    if (cJSON_IsTrue(release)) {
        if (yaw || pitch)
            return {};
        return HeadTrackingCommand{id->valuestring, 0, 0, true};
    }
    if (!cJSON_IsNumber(yaw) || !cJSON_IsNumber(pitch) || !std::isfinite(yaw->valuedouble) ||
        !std::isfinite(pitch->valuedouble))
        return {};
    return HeadTrackingCommand{id->valuestring, static_cast<float>(std::clamp(yaw->valuedouble, -15.0, 15.0)),
                               static_cast<float>(std::clamp(pitch->valuedouble, -10.0, 10.0)), false};
}

} // namespace stackchan::conversation
