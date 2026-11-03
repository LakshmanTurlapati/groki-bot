// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "fake_camera.hpp"

#include <utility>

namespace {

void check_quiesce()
{
    auto& hw = camera_test::hardware;
    if (hw.quiesce != nullptr && !hw.quiesce->load()) {
        ++hw.unsafe_operations;
    }
}

void release_frame(void*& handle)
{
    if (handle != nullptr) {
        --camera_test::hardware.outstanding_frames;
        handle = nullptr;
    }
}

} // namespace

namespace stackchan::board {

CameraFrame::CameraFrame(CameraFrame&& other) noexcept
    : fb_handle_(std::exchange(other.fb_handle_, nullptr)), data_(other.data_), size_(other.size_),
      width_(other.width_), height_(other.height_)
{
}

CameraFrame& CameraFrame::operator=(CameraFrame&& other) noexcept
{
    if (this != &other) {
        release_frame(fb_handle_);
        fb_handle_ = std::exchange(other.fb_handle_, nullptr);
        data_ = other.data_;
        size_ = other.size_;
        width_ = other.width_;
        height_ = other.height_;
    }
    return *this;
}

CameraFrame::~CameraFrame() { release_frame(fb_handle_); }
CameraGc0308::~CameraGc0308() = default;

tl::expected<void, CameraError> CameraGc0308::begin(CameraPixelFormat format, CameraFrameSize size)
{
    check_quiesce();
    auto& hw = camera_test::hardware;
    ++hw.begin_calls;
    if (hw.outstanding_frames != 0) {
        ++hw.unsafe_operations;
    }
    if ((format == CameraPixelFormat::BayerRaw && hw.fail_raw_init) ||
        (format == CameraPixelFormat::Rgb565 && hw.fail_rgb_init)) {
        return tl::unexpected(CameraError::LowMemory);
    }
    initialised_ = true;
    hw.initialized = true;
    hw.colorbar = false;
    format_ = format;
    expected_w_ = size == CameraFrameSize::Vga ? 640 : 320;
    expected_h_ = size == CameraFrameSize::Vga ? 480 : 240;
    hw.pixels.resize(expected_w_ * expected_h_ * (format == CameraPixelFormat::Rgb565 ? 2 : 1));
    for (std::size_t i = 0; i < hw.pixels.size(); ++i) {
        hw.pixels[i] = static_cast<std::uint8_t>(i);
    }
    return {};
}

tl::expected<void, CameraError> CameraGc0308::end()
{
    check_quiesce();
    auto& hw = camera_test::hardware;
    if (hw.outstanding_frames != 0) {
        ++hw.unsafe_operations;
    }
    initialised_ = false;
    hw.initialized = false;
    return {};
}

tl::expected<CameraFrame, CameraError> CameraGc0308::capture()
{
    check_quiesce();
    auto& hw = camera_test::hardware;
    if (hw.capture_hook) {
        hw.capture_hook();
    }
    ++hw.capture_calls;
    if (hw.fail_capture || !initialised_) {
        return tl::unexpected(CameraError::CaptureFailed);
    }
    if (hw.outstanding_frames != 0) {
        ++hw.unsafe_operations;
        return tl::unexpected(CameraError::CaptureFailed);
    }
    CameraFrame frame;
    frame.fb_handle_ = &hw;
    frame.data_ = hw.pixels.data();
    frame.size_ = hw.pixels.size();
    frame.width_ = expected_w_;
    frame.height_ = expected_h_;
    ++hw.outstanding_frames;
    return frame;
}

tl::expected<void, CameraError> CameraGc0308::set_colorbar(bool enable)
{
    check_quiesce();
    auto& hw = camera_test::hardware;
    if ((enable && hw.fail_colorbar_enable) || (!enable && hw.fail_colorbar_disable)) {
        return tl::unexpected(CameraError::SensorAccess);
    }
    hw.colorbar = enable;
    return {};
}

tl::expected<std::uint8_t, CameraError> CameraGc0308::read_reg(std::uint8_t, std::uint8_t)
{
    check_quiesce();
    if (camera_test::hardware.fail_register || !initialised_) {
        return tl::unexpected(CameraError::SensorAccess);
    }
    return std::uint8_t{0x42};
}

tl::expected<void, CameraError> CameraGc0308::write_reg(std::uint8_t, std::uint8_t, std::uint8_t)
{
    check_quiesce();
    if (camera_test::hardware.fail_register || !initialised_) {
        return tl::unexpected(CameraError::SensorAccess);
    }
    return {};
}

} // namespace stackchan::board
