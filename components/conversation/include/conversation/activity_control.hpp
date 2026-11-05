// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <cJSON.h>

namespace stackchan::conversation {

// Gateway `activity` message: the Grok Bot run state for the avatar.
//   {"type":"activity","state":"working","ttl_ms":120000}
// `state` is an expression name (thinking / working / waiting / blocked /
// done / failed / listening) or "idle" to clear. `ttl_ms` is optional;
// 0 or absent lets the firmware pick the per-state default.
struct BotActivityCommand {
    std::string state;
    std::uint32_t ttl_ms{0};
};

// Longest TTL accepted: one hour (the SharedState word stores 100 ms units
// in 15 bits, ~54 min, and caps anything longer).
inline constexpr std::uint32_t kMaxActivityTtlMs = 60u * 60u * 1000u;

inline std::optional<BotActivityCommand> parse_bot_activity(const cJSON* root) {
    const auto* state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state) || state->valuestring == nullptr) {
        return {};
    }
    const std::size_t len = std::strlen(state->valuestring);
    if (len == 0 || len > 16) {
        return {};
    }
    BotActivityCommand cmd{state->valuestring, 0};
    const auto* ttl = cJSON_GetObjectItemCaseSensitive(root, "ttl_ms");
    if (cJSON_IsNumber(ttl) && ttl->valuedouble > 0.0) {
        cmd.ttl_ms = ttl->valuedouble >= static_cast<double>(kMaxActivityTtlMs)
                         ? kMaxActivityTtlMs
                         : static_cast<std::uint32_t>(ttl->valuedouble);
    }
    return cmd;
}

} // namespace stackchan::conversation
