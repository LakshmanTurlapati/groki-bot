// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>

using TickType_t = std::uint32_t;
constexpr TickType_t pdMS_TO_TICKS(std::uint32_t ms) { return ms; }
