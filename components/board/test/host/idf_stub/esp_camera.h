// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr int LEDC_TIMER_0 = 0;
constexpr int LEDC_CHANNEL_0 = 0;
constexpr int CAMERA_FB_IN_PSRAM = 1;
constexpr int CAMERA_GRAB_LATEST = 1;

enum pixformat_t { PIXFORMAT_RGB565, PIXFORMAT_GRAYSCALE };
enum framesize_t { FRAMESIZE_QVGA, FRAMESIZE_VGA };

struct camera_config_t {
    int pin_pwdn, pin_reset, pin_xclk;
    int pin_sccb_sda, pin_sccb_scl;
    int pin_d7, pin_d6, pin_d5, pin_d4, pin_d3, pin_d2, pin_d1, pin_d0;
    int pin_vsync, pin_href, pin_pclk;
    int xclk_freq_hz, ledc_timer, ledc_channel, sccb_i2c_port;
    pixformat_t pixel_format;
    framesize_t frame_size;
    int jpeg_quality, fb_count, fb_location, grab_mode;
};

struct camera_fb_t {
    std::uint8_t* buf;
    std::size_t len, width, height;
    pixformat_t format;
};

struct sensor_t {
    int (*set_reg)(sensor_t*, int reg, int mask, int value);
    int (*get_reg)(sensor_t*, int reg, int mask);
    int (*set_colorbar)(sensor_t*, int enable);
};

esp_err_t esp_camera_init(const camera_config_t* config);
esp_err_t esp_camera_deinit();
camera_fb_t* esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t* frame);
sensor_t* esp_camera_sensor_get();
