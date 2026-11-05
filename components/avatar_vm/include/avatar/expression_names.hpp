// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <optional>
#include <string_view>

#include "avatar/expression.hpp"

namespace stackchan::avatar {

// Canonical lowercase names (plus aliases) for every Expression. Single
// source for the conversation tool, MCP sink and any future name-based
// entry point, so the supported set can never drift between them.
struct ExpressionName {
    const char* name;
    Expression value;
};

inline constexpr ExpressionName kExpressionNames[] = {
    {"neutral", Expression::Neutral},     {"idle", Expression::Neutral},
    {"happy", Expression::Happy},         {"sad", Expression::Sad},
    {"angry", Expression::Angry},         {"doubt", Expression::Doubt},
    {"sleepy", Expression::Sleepy},       {"listening", Expression::Listening},
    {"thinking", Expression::Thinking},   {"excited", Expression::Excited},
    {"curious", Expression::Curious},     {"confused", Expression::Confused},
    {"surprised", Expression::Surprised}, {"dizzy", Expression::Dizzy},
    {"affection", Expression::Affection}, {"bored", Expression::Bored},
    {"working", Expression::Working},     {"waiting", Expression::Waiting},
    {"blocked", Expression::Blocked},     {"done", Expression::Done},
    {"failed", Expression::Failed},
};

inline std::optional<Expression> expression_from_name(std::string_view name) noexcept {
    for (const auto& entry : kExpressionNames) {
        if (name == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

inline std::optional<Expression> expression_from_name(const char* name) noexcept {
    if (name == nullptr) {
        return std::nullopt;
    }
    return expression_from_name(std::string_view{name});
}

} // namespace stackchan::avatar
