// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>

using TickType_t = std::uint32_t;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
