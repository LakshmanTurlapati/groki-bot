// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>

constexpr unsigned MALLOC_CAP_INTERNAL = 1;
std::size_t heap_caps_get_largest_free_block(unsigned caps);
