// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <board/camera_gc0308.hpp>

namespace camera_test {

struct Hardware {
    std::atomic<bool>* quiesce = nullptr;
    bool fail_rgb_init = false;
    bool fail_raw_init = false;
    bool fail_capture = false;
    bool fail_colorbar_enable = false;
    bool fail_colorbar_disable = false;
    bool fail_register = false;
    bool colorbar = false;
    bool initialized = false;
    std::size_t begin_calls = 0;
    std::size_t capture_calls = 0;
    std::size_t outstanding_frames = 0;
    std::size_t unsafe_operations = 0;
    std::function<void()> capture_hook;
    std::vector<std::uint8_t> pixels;
};

inline Hardware hardware;

} // namespace camera_test
