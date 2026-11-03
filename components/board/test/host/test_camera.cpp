// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <board/camera_gc0308.hpp>
#include <M5Unified.h>
#include <esp_camera.h>
#include <esp_system.h>

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace stackchan::board;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

camera_config_t config{};
std::vector<std::uint8_t> pixels;
camera_fb_t framebuffer{};
camera_fb_t* next_frame = nullptr;
int returned_frames = 0;
int deinit_calls = 0;
std::size_t free_block = 128 * 1024;
bool task_exists = true;
int register_read_result = 0;
int fail_write_number = 0;
std::vector<std::pair<int, int>> writes;
shutdown_handler_t shutdown_handler = nullptr;
int shutdown_registration_result = ESP_OK;
int shutdown_registration_calls = 0;
int camera_init_calls = 0;
bool bus_enabled = true;
bool bus_locked = false;
int bus_step = 0;
int bus_fail_step = 0;
std::uint8_t expander_output = 0xff;
std::vector<std::vector<std::uint8_t>> bus_writes;

bool transfer_succeeds()
{
    if (++bus_step == bus_fail_step) {
        bus_locked = false; // LovyanGFX releases the transaction on error.
        return false;
    }
    return true;
}

void reset_bus()
{
    check(!bus_locked, "previous transaction leaked the shared-bus mutex");
    bus_step = 0;
    bus_fail_step = 0;
    expander_output = 0xbf;
    bus_writes.clear();
}

void test_shutdown_reset()
{
    CameraGc0308 camera;
    shutdown_registration_result = -1;
    auto registration_failure = camera.begin(CameraPixelFormat::Rgb565);
    check(!registration_failure && registration_failure.error() == CameraError::DriverInit,
          "shutdown registration failure was ignored");
    check(!camera.initialised() && camera_init_calls == 0,
          "sensor started without a shutdown handler");

    shutdown_registration_result = ESP_OK;
    check(static_cast<bool>(camera.begin(CameraPixelFormat::Rgb565)), "retry after registration failure failed");
    check(shutdown_handler != nullptr && shutdown_registration_calls == 2,
          "shutdown handler was not registered on retry");
    const auto registered_handler = shutdown_handler;
    for (auto format : {CameraPixelFormat::BayerRaw, CameraPixelFormat::Rgb565}) {
        check(static_cast<bool>(camera.end()), "mode swap deinit failed");
        check(static_cast<bool>(camera.begin(format, CameraFrameSize::Vga)), "mode swap init failed");
    }
    check(shutdown_registration_calls == 2 && shutdown_handler == registered_handler,
          "mode swap replaced or duplicated the shutdown handler");
    check(static_cast<bool>(camera.end()), "camera deinit failed");
    free_block = 0;
    check(!camera.begin(), "low-memory reinitialisation unexpectedly succeeded");
    free_block = 128 * 1024;

    for (int failure = 0; failure <= 7; ++failure) {
        reset_bus();
        bus_fail_step = failure;
        shutdown_handler();
        check(!bus_locked, "shutdown retained the shared-bus mutex");
        if (failure == 0) {
            check(bus_step == 7 && expander_output == 0xbe,
                  "shutdown did not assert reset while preserving other expander outputs");
            check(bus_writes == std::vector<std::vector<std::uint8_t>>{{0x03}, {0x03, 0xbe}},
                  "shutdown accessed unexpected expander registers");
        } else {
            check(bus_step == failure, "shutdown continued or released the mutex twice after transfer error");
            if (failure <= 4) {
                check(expander_output == 0xbf && bus_writes.size() <= 1,
                      "failed expander read changed output bits");
            }
        }
    }
    reset_bus();
    bus_enabled = false;
    shutdown_handler();
    check(bus_step == 0, "shutdown used a disabled shared bus");
    bus_enabled = true;
    check(static_cast<bool>(camera.begin()), "camera did not recover from failed reinitialisation");
    check(shutdown_registration_calls == 2, "recovery duplicated the shutdown handler");
}

int set_reg(sensor_t*, int reg, int, int value)
{
    writes.emplace_back(reg, value);
    return static_cast<int>(writes.size()) == fail_write_number ? -1 : 0;
}

int get_reg(sensor_t*, int, int) { return register_read_result; }
int set_colorbar(sensor_t*, int) { return 0; }
sensor_t sensor{set_reg, get_reg, set_colorbar};

void reset_registers()
{
    writes.clear();
    fail_write_number = 0;
    register_read_result = 0;
}

void prepare_frame(CameraPixelFormat format, CameraFrameSize size)
{
    const std::size_t width = size == CameraFrameSize::Vga ? 640 : 320;
    const std::size_t height = size == CameraFrameSize::Vga ? 480 : 240;
    const bool rgb = format == CameraPixelFormat::Rgb565;
    pixels.resize(width * height * (rgb ? 2 : 1));
    framebuffer = {pixels.data(), pixels.size(), width, height,
                   rgb ? PIXFORMAT_RGB565 : PIXFORMAT_GRAYSCALE};
    next_frame = &framebuffer;
    returned_frames = 0;
}

void test_capture_validation(CameraPixelFormat format, CameraFrameSize size)
{
    CameraGc0308 camera;
    check(static_cast<bool>(camera.begin(format, size)), "camera init failed");
    for (int defect = 0; defect < 6; ++defect) {
        prepare_frame(format, size);
        switch (defect) {
        case 0: --framebuffer.len; break;
        case 1: ++framebuffer.len; break;
        case 2: framebuffer.buf = nullptr; break;
        case 3: ++framebuffer.width; break;
        case 4: ++framebuffer.height; break;
        case 5:
            framebuffer.format = framebuffer.format == PIXFORMAT_RGB565 ? PIXFORMAT_GRAYSCALE : PIXFORMAT_RGB565;
            break;
        }
        {
            auto frame = camera.capture();
            check(!frame && frame.error() == CameraError::WrongPixelFormat, "malformed frame was accepted");
            check(returned_frames == 1, "rejected frame was not immediately returned");
        }
        check(returned_frames == 1, "rejected frame was returned twice");
    }

    prepare_frame(format, size);
    {
        auto frame = camera.capture();
        check(static_cast<bool>(frame), "valid frame was rejected");
        check(frame->data() == pixels.data() && frame->size() == pixels.size(), "valid frame data changed");
        CameraFrame moved = std::move(*frame);
        CameraFrame assigned;
        assigned = std::move(moved);
        check(frame->data() == nullptr && moved.data() == nullptr, "moved frame retained ownership");
        check(returned_frames == 0, "frame returned before its owner was destroyed");
    }
    check(returned_frames == 1, "moved framebuffer was not returned exactly once");

    next_frame = nullptr;
    auto missing = camera.capture();
    check(!missing && missing.error() == CameraError::CaptureFailed, "capture timeout was not propagated");
}

void test_register_errors()
{
    CameraGc0308 camera;
    check(static_cast<bool>(camera.begin()), "camera init failed");

    reset_registers();
    auto zero = camera.read_reg(1, 0x53);
    check(zero && *zero == 0, "zero register value was treated as failure");
    check(writes.size() == 2 && writes.back() == std::pair{0xfe, 0}, "register read did not restore page zero");

    reset_registers();
    fail_write_number = 2;
    auto restore_failure = camera.read_reg(1, 0x53);
    check(!restore_failure && restore_failure.error() == CameraError::SensorAccess,
          "register read hid page restore failure");

    reset_registers();
    register_read_result = -1;
    auto read_failure = camera.read_reg(1, 0x53);
    check(!read_failure && writes.back() == std::pair{0xfe, 0}, "failed register read did not restore page zero");

    for (int failed_write = 1; failed_write <= 3; ++failed_write) {
        reset_registers();
        fail_write_number = failed_write;
        auto failure = camera.write_reg(1, 0x53, 0x20);
        check(!failure && failure.error() == CameraError::SensorAccess, "register write hid an I2C failure");
        check(writes.back() == std::pair{0xfe, 0}, "failed register write did not attempt page zero restore");
    }
}

void test_initialisation_failures()
{
    CameraGc0308 camera;
    auto raw_qvga = camera.begin(CameraPixelFormat::BayerRaw, CameraFrameSize::Qvga);
    check(!raw_qvga && raw_qvga.error() == CameraError::BadArgument, "subsampled Bayer was accepted");
    free_block = 1024;
    auto low_memory = camera.begin();
    check(!low_memory && low_memory.error() == CameraError::LowMemory, "low internal memory was ignored");
    free_block = 128 * 1024;
    task_exists = false;
    const int previous_deinits = deinit_calls;
    auto missing_task = camera.begin();
    check(!missing_task && missing_task.error() == CameraError::DriverInit, "missing camera task was ignored");
    check(!camera.initialised() && deinit_calls == previous_deinits + 1, "failed init leaked driver state");
    task_exists = true;
}

} // namespace

namespace m5 {
bool I2c::isEnabled() const { return bus_enabled; }
int I2c::getSDA() const { return 12; }
int I2c::getSCL() const { return 11; }
bool I2c::start(std::uint8_t address, bool read, std::uint32_t frequency) const
{
    check(!bus_locked, "shutdown tried to acquire the shared-bus mutex twice");
    check(address == 0x58 && !read && frequency == 100'000, "wrong shutdown bus target");
    if (!transfer_succeeds()) return false;
    bus_locked = true;
    return true;
}
bool I2c::restart(std::uint8_t address, bool read, std::uint32_t frequency) const
{
    check(bus_locked && address == 0x58 && frequency == 100'000, "restart without owned bus");
    check(read == (bus_step == 2), "wrong shutdown transaction direction");
    return transfer_succeeds();
}
bool I2c::write(const std::uint8_t* data, std::size_t size) const
{
    check(bus_locked, "write without owned bus");
    if (!transfer_succeeds()) return false;
    bus_writes.emplace_back(data, data + size);
    if (size == 2) expander_output = data[1];
    return true;
}
bool I2c::read(std::uint8_t* result, std::size_t size, bool last_nack) const
{
    check(bus_locked && size == 1 && last_nack, "wrong shutdown read");
    if (!transfer_succeeds()) return false;
    *result = expander_output;
    return true;
}
bool I2c::stop() const
{
    check(bus_locked, "stop released the shared-bus mutex twice");
    const bool result = transfer_succeeds();
    bus_locked = false;
    return result;
}
} // namespace m5

int esp_register_shutdown_handler(shutdown_handler_t handler)
{
    ++shutdown_registration_calls;
    if (shutdown_registration_result == ESP_OK) shutdown_handler = handler;
    return shutdown_registration_result;
}

std::size_t heap_caps_get_largest_free_block(unsigned) { return free_block; }
void* xTaskGetHandle(const char*) { return task_exists ? &config : nullptr; }
esp_err_t esp_camera_init(const camera_config_t* value)
{
    ++camera_init_calls;
    config = *value;
    reset_registers();
    return ESP_OK;
}
esp_err_t esp_camera_deinit()
{
    ++deinit_calls;
    return ESP_OK;
}
camera_fb_t* esp_camera_fb_get() { return next_frame; }
void esp_camera_fb_return(camera_fb_t* frame)
{
    check(frame == &framebuffer, "wrong framebuffer returned");
    ++returned_frames;
}
sensor_t* esp_camera_sensor_get() { return &sensor; }

int main()
{
    test_shutdown_reset();
    test_capture_validation(CameraPixelFormat::Rgb565, CameraFrameSize::Vga);
    test_capture_validation(CameraPixelFormat::Grayscale, CameraFrameSize::Qvga);
    test_capture_validation(CameraPixelFormat::BayerRaw, CameraFrameSize::Vga);
    test_register_errors();
    test_initialisation_failures();
    std::puts("camera tests passed");
}
