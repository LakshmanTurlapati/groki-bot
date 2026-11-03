// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace stackchan::wifi_config {

struct CameraCaptureOptions {
    bool raw_bayer = false;
    bool colorbar = false;
    bool tracking = false;
};
struct CameraCaptureMetadata {
    std::string frame_id;
    bool tracking_busy = false;
    float capture_yaw = 0, capture_pitch = 0;
    std::uint32_t capture_ms = 0;
    bool motion_compensated = false;
    float capture_motion_deg = 0;
};
using CameraCaptureSink = std::function<bool(const CameraCaptureOptions&, std::vector<std::uint8_t>&, std::size_t&,
                                             std::size_t&, std::string&, CameraCaptureMetadata&)>;
using CameraRegSink = std::function<bool(bool, std::uint8_t, std::uint8_t, std::uint8_t&)>;

inline CameraCaptureSink capture_sink;
inline CameraRegSink reg_sink;
inline void set_camera_capture_sink(CameraCaptureSink sink) { capture_sink = std::move(sink); }
inline void set_camera_reg_sink(CameraRegSink sink) { reg_sink = std::move(sink); }

} // namespace stackchan::wifi_config
