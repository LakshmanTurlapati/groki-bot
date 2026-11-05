// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "camera_service.hpp"

#include <sdkconfig.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>

#include <wifi_config_service/wifi_config_service.hpp>

#if CONFIG_STACKCHAN_CAMERA_ENABLED
#include "board/camera_gc0308.hpp"
#endif

namespace stackchan::app::camera_service {

#if CONFIG_STACKCHAN_CAMERA_ENABLED

namespace {

constexpr const char* kTag = "stackchan";

// Resident camera (M5Base only). See init_resident() in the header for the
// full lifetime rationale.
stackchan::board::CameraGc0308 g_camera;
bool g_supported = false;
std::mutex g_camera_mutex;

// SharedState reference for the quiesce flag; set by register_sinks.
SharedState* g_state = nullptr;

using stackchan::board::CameraFrameSize;
using stackchan::board::CameraPixelFormat;

// Keep ordinary snapshots small enough to coexist with audio and Wi-Fi.
// RAW needs VGA to preserve the sensor's Bayer mosaic.
constexpr CameraFrameSize kPhotoSize = CameraFrameSize::Qvga;

bool begin_with_retry(CameraPixelFormat format, CameraFrameSize size)
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto result = g_camera.begin(format, size);
        if (result) {
            return true;
        }
        if (attempt == 4) {
            ESP_LOGE(kTag, "camera init failed: %d", static_cast<int>(result.error()));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    return false;
}

// Exclude framebuffer rendering while camera DMA and encoding use PSRAM.
// Polling tasks observe the pause flag; I2C itself uses M5Unified's bus mutex.
struct QuiesceGuard {
    std::unique_lock<std::mutex> render_lock;
    explicit QuiesceGuard(bool tracking = false) : render_lock(g_state->camera_render_mutex, std::defer_lock) {
        g_state->i2c_quiesce.store(true, std::memory_order_release);
        render_lock.lock();
        // User-initiated diagnostics retain the conservative polling drain;
        // tracking waits only for the actual in-flight renderer above.
        if (!tracking)
            vTaskDelay(pdMS_TO_TICKS(150));
    }
    ~QuiesceGuard()
    {
        g_state->i2c_quiesce.store(false, std::memory_order_release);
    }
};

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

std::optional<HeadControl::Result> wait_for_servo(std::optional<std::uint32_t> sequence) {
    if (!sequence)
        return {};
    const auto deadline = now_ms() + 200;
    do {
        if (auto result = g_state->servo.result(*sequence))
            return result;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (HeadControl::before(now_ms(), deadline));
    g_state->servo.cancel(*sequence);
    return {};
}

bool tracking_capture(std::vector<std::uint8_t>& out, std::size_t& w, std::size_t& h, std::string& format,
                      stackchan::wifi_config::CameraCaptureMetadata& metadata,
                      const std::function<bool(std::vector<std::uint8_t>&)>& transform = {}, bool motion = false) {
    const auto started = now_ms();
    if (g_state->servo.sleeping()) {
        metadata.tracking_sleeping = true;
        return false;
    }
    metadata.tracking_busy = true;
    std::unique_lock lock(g_camera_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    if (!g_state->servo.available() || !g_camera.initialised()) {
        metadata.tracking_busy = false;
        return false;
    }
    const auto prepare_request =
        g_state->servo.submit(HeadControl::Operation::Prepare, now_ms(), {}, {}, false, 0, motion);
    if (!prepare_request)
        return false;
    auto prepared = wait_for_servo(prepare_request);
    if (!prepared || !prepared->ok) {
        metadata.tracking_busy = prepared && !prepared->unavailable;
        return false;
    }
    struct Reservation {
        std::string id;
        ~Reservation() {
            if (!id.empty())
                g_state->servo.abandon_frame(id);
        }
    } reservation{prepared->frame_id};
    const auto deadline = now_ms() + 500;
    QuiesceGuard quiesce{true};
    metadata.tracking_busy = false;
    // The resident driver has one framebuffer. Return its queued/partial
    // image once; the next acquisition is composed wholly under exclusion.
    for (int i = 0; i < 1; ++i) {
        if (!HeadControl::before(now_ms(), deadline))
            return false;
        auto discarded = g_camera.capture();
        if (!discarded || !HeadControl::before(now_ms(), deadline))
            return false;
    }
    if (!HeadControl::before(now_ms(), deadline))
        return false;
    if (motion) {
        const auto exposure_request =
            g_state->servo.submit(HeadControl::Operation::Exposure, now_ms(), prepared->frame_id);
        if (!exposure_request) {
            metadata.tracking_busy = true;
            return false;
        }
        auto exposure = wait_for_servo(exposure_request);
        if (!exposure || !exposure->ok) {
            metadata.tracking_busy = exposure && !exposure->unavailable;
            return false;
        }
    }
    auto frame = g_camera.capture();
    const auto captured_at = now_ms();
    if (!frame || !HeadControl::before(captured_at, deadline))
        return false;
    const auto finalize_request =
        g_state->servo.submit(HeadControl::Operation::Finalize, now_ms(), prepared->frame_id, {}, false, captured_at);
    if (!finalize_request) {
        metadata.tracking_busy = true;
        return false;
    }
    auto finalized = wait_for_servo(finalize_request);
    if (!finalized || !finalized->ok) {
        metadata.tracking_busy = finalized && !finalized->unavailable;
        return false;
    }
    out.assign(frame->data(), frame->data() + frame->size());
    w = frame->width();
    h = frame->height();
    format = "rgb565be";
    metadata.capture_yaw = finalized->measured.yaw;
    metadata.capture_pitch = finalized->measured.pitch;
    metadata.capture_ms = now_ms() - started;
    metadata.motion_compensated = finalized->motion;
    metadata.capture_motion_deg = finalized->capture_motion_deg;
    // Encoding the copied frame while the renderer is excluded avoids
    // competing full-screen PSRAM reads during the SIMD conversion.
    if (transform && !transform(out))
        return false;
    metadata.frame_id = finalized->frame_id;
    reservation.id.clear();
    return true;
}

bool capture_sink(const stackchan::wifi_config::CameraCaptureOptions& options, std::vector<std::uint8_t>& out,
                  std::size_t& w, std::size_t& h, std::string& format,
                  stackchan::wifi_config::CameraCaptureMetadata& metadata) {
    if (options.tracking) {
        if (options.raw_bayer || options.colorbar)
            return false;
        return tracking_capture(out, w, h, format, metadata);
    }
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    // Discard two frames after the quiesce drain so the one we keep was
    // composed entirely under quiesce.
    QuiesceGuard quiesce;

    if (!g_camera.initialised() && !begin_with_retry(CameraPixelFormat::Rgb565, kPhotoSize)) {
        return false;
    }

    // RAW Bayer: the DVP path is 1 B/px vs the resident RGB565's 2 B/px, so
    // this needs a driver re-init, not just a sensor register flip. Deinit →
    // BayerRaw init → grab → restore RGB565. The swap reuses the
    // internal-RAM blocks released by the resident driver. Allocation may
    // still fail at steady state; the next request retries a failed restore.
    if (options.raw_bayer) {
        // Format switches contend with other tasks (conversation, WebSocket)
        // for fragmented internal RAM — begin() can lose the allocation race
        // (it detects this and fails loudly), so every (re)init below
        // retries a few times.
        (void)g_camera.end();
        if (!begin_with_retry(CameraPixelFormat::BayerRaw, CameraFrameSize::Vga)) {
            ESP_LOGE(kTag, "restoring RGB565 after RAW init failure");
            (void)begin_with_retry(CameraPixelFormat::Rgb565, kPhotoSize);
            return false;
        }
        bool ok = false;
        {
            for (int i = 0; i < 2; ++i) {
                (void)g_camera.capture();
            }
            auto frame = g_camera.capture();
            if (frame) {
                out.assign(frame->data(), frame->data() + frame->size());
                w = frame->width();
                h = frame->height();
                format = "bayer8-rggb";
                ok = true;
            } else {
                ESP_LOGE(kTag, "RAW capture failed: %d",
                         static_cast<int>(frame.error()));
            }
        }
        (void)g_camera.end();
        if (!begin_with_retry(CameraPixelFormat::Rgb565, kPhotoSize)) {
            ESP_LOGE(kTag, "RGB565 restore failed; next request will retry");
        }
        return ok;
    }

    // Colour-bar test pattern: optics-independent reference for validating
    // the transfer path. Same RGB565 pipeline, the sensor just substitutes
    // its generator for the array.
    if (options.colorbar) {
        if (!g_camera.set_colorbar(true)) {
            // A failed write may still have reached the sensor. Reset on a
            // cleanup failure so a later photo cannot silently be a test pattern.
            if (!g_camera.set_colorbar(false)) {
                (void)g_camera.end();
            }
            return false;
        }
    }
    format = "rgb565be";
    for (int i = 0; i < 2; ++i) {
        (void)g_camera.capture();
    }
    auto frame = g_camera.capture();
    if (!frame) {
        // The first fb_get after a long idle-streaming stretch can time out
        // (the DVP stalls once the un-consumed frame queue overflows; the
        // failed fb_get itself kicks the driver back into motion) — seen on
        // HW after the RAW swap. One more attempt recovers it.
        ESP_LOGW(kTag, "camera capture timed out — retrying once");
        frame = g_camera.capture();
    }
    if (options.colorbar) {
        if (!g_camera.set_colorbar(false)) {
            // Return the held driver buffer before tearing its owner down.
            frame = tl::unexpected{stackchan::board::CameraError::SensorAccess};
            (void)g_camera.end();
            return false;
        }
    }
    if (!frame) {
        ESP_LOGE(kTag, "camera capture failed: %d",
                 static_cast<int>(frame.error()));
        (void)g_camera.end();
        return false;
    }
    out.assign(frame->data(), frame->data() + frame->size());
    w = frame->width();
    h = frame->height();
    return true;
}

// Raw register access shares the capture lock so RAW mode changes cannot
// interleave with a register page selection.
bool reg_sink(bool write, std::uint8_t page, std::uint8_t reg, std::uint8_t& value)
{
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    QuiesceGuard quiesce;
    if (!g_camera.initialised() && !begin_with_retry(CameraPixelFormat::Rgb565, kPhotoSize)) {
        return false;
    }
    if (write) {
        if (g_camera.write_reg(page, reg, value)) {
            return true;
        }
        (void)g_camera.end();
        return false;
    }
    auto r = g_camera.read_reg(page, reg);
    if (!r) {
        (void)g_camera.end();
        return false;
    }
    value = *r;
    return true;
}

} // namespace

void init_resident(stackchan::board::Board& board)
{
    g_supported = stackchan::board::profile_for(board.kind()).has_camera;
    if (!g_supported) {
        return;
    }
    if (auto r = g_camera.begin(stackchan::board::CameraPixelFormat::Rgb565,
                                kPhotoSize);
        !r) {
        ESP_LOGW(kTag, "resident camera init failed: %d — capture requests will retry",
                 static_cast<int>(r.error()));
    }
}

void register_sinks(SharedState& state)
{
    if (!g_supported) {
        return; // has_camera stays false — web UI hides the section
    }
    g_state = &state;
    state.servo.camera_available.store(true);
    stackchan::wifi_config::set_camera_capture_sink(&capture_sink);
    stackchan::wifi_config::set_camera_reg_sink(&reg_sink);
}

bool capture_tracking_frame(std::vector<std::uint8_t>& out, std::size_t& width, std::size_t& height,
                            std::string& format, wifi_config::CameraCaptureMetadata& metadata,
                            const std::function<bool(std::vector<std::uint8_t>&)>& transform, bool motion) {
    if (!g_state || !g_supported)
        return false;
    return tracking_capture(out, width, height, format, metadata, transform, motion);
}

#else // !CONFIG_STACKCHAN_CAMERA_ENABLED

void init_resident(stackchan::board::Board& /*board*/) {}
void register_sinks(SharedState& /*state*/) {}

bool capture_tracking_frame(std::vector<std::uint8_t>&, std::size_t&, std::size_t&, std::string&,
                            wifi_config::CameraCaptureMetadata&, const std::function<bool(std::vector<std::uint8_t>&)>&,
                            bool) {
    return false;
}

#endif // CONFIG_STACKCHAN_CAMERA_ENABLED

} // namespace stackchan::app::camera_service
