// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

inline void camera_host_log(const char*, const char*, ...) {}
#define ESP_LOGI(...) camera_host_log(__VA_ARGS__)
#define ESP_LOGW(...) camera_host_log(__VA_ARGS__)
#define ESP_LOGE(...) camera_host_log(__VA_ARGS__)
