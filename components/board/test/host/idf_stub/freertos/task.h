// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>

void* xTaskGetHandle(const char* name);
void vTaskDelay(std::uint32_t ticks);
