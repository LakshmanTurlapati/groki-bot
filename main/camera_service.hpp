// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "board/board.hpp"
#include "shared_state.hpp"
#include <wifi_config_service/wifi_config_service.hpp>

// Resident GC0308 camera lifecycle and HTTP capture/register sinks.
// Camera operations are serialized. The render/LED/demo loops pause during
// capture to reduce PSRAM contention; I2C transactions themselves share
// M5Unified's bus mutex through the camera SCCB adapter.
namespace stackchan::app::camera_service {

// Reserve the camera's DMA resources at boot, before audio/network tasks
// fragment internal RAM. Ordinary photos use QVGA RGB565 (150 KiB PSRAM);
// RAW diagnostics temporarily switch to VGA. On boards without a camera,
// or camera-disabled builds, these entry points are no-ops.
// The standalone QR test owns a different driver instance and must stay off
// while the resident camera is enabled.
void init_resident(stackchan::board::Board& board);

// Register capability even if boot initialization failed. Capture requests
// retry initialization after transient boot or RAW-restore failures, instead
// of hiding the camera until reboot. Skipped when init_resident wasn't called
// (for example, ASR mode) or the board has no camera.
void register_sinks(SharedState& state);

// USB snapshots use the same pose reservation and capture lock as HTTP.
bool capture_tracking_frame(std::vector<std::uint8_t>& out, std::size_t& width, std::size_t& height,
                            std::string& format, wifi_config::CameraCaptureMetadata& metadata,
                            const std::function<bool(std::vector<std::uint8_t>&)>& transform = {}, bool motion = false);

} // namespace stackchan::app::camera_service
