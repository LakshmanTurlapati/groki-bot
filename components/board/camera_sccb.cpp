// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "sccb.h"
}

namespace {

constexpr std::uint32_t kFrequency = CONFIG_SCCB_CLK_FREQ;
constexpr const char* kTag = "camera_sccb";

bool valid_address(std::uint8_t address)
{
    return m5::In_I2C.isEnabled() && address >= 0x08 && address <= 0x77;
}

bool write_bytes(std::uint8_t address, const std::uint8_t* data, std::size_t size)
{
    // LovyanGFX releases its transaction mutex when a transfer fails. Do not
    // call stop() again on that path: it would release the same mutex twice.
    return valid_address(address) &&
           m5::In_I2C.start(address, false, kFrequency) &&
           m5::In_I2C.write(data, size) && m5::In_I2C.stop();
}

bool read_bytes(std::uint8_t address, const std::uint8_t* reg, std::size_t reg_size,
                std::uint8_t* result, std::size_t result_size)
{
    return valid_address(address) &&
           m5::In_I2C.start(address, false, kFrequency) &&
           m5::In_I2C.write(reg, reg_size) &&
           m5::In_I2C.restart(address, true, kFrequency) &&
           m5::In_I2C.read(result, result_size, true) && m5::In_I2C.stop();
}

std::array<std::uint8_t, 2> register_bytes(std::uint16_t reg)
{
    return {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
}

} // namespace

extern "C" {

int SCCB_Init(int pin_sda, int pin_scl)
{
    // M5.begin() owns the bus used by the camera, touch, power and audio
    // devices. Reuse that owner and its mutex without reconfiguring it.
    if (!m5::In_I2C.isEnabled() || pin_sda != m5::In_I2C.getSDA() || pin_scl != m5::In_I2C.getSCL()) {
        ESP_LOGE(kTag, "camera pins do not match M5 internal I2C");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

int SCCB_Use_Port(int port)
{
    return m5::In_I2C.isEnabled() && port == m5::In_I2C.getPort() ? ESP_OK : ESP_ERR_INVALID_ARG;
}

int SCCB_Deinit()
{
    // The other board devices still need this bus after camera shutdown.
    return ESP_OK;
}

int SCCB_Probe(std::uint8_t address)
{
    return valid_address(address) && m5::In_I2C.start(address, false, kFrequency) && m5::In_I2C.stop()
               ? ESP_OK : ESP_FAIL;
}

std::uint8_t SCCB_Read(std::uint8_t address, std::uint8_t reg)
{
    std::uint8_t value = 0xff;
    if (!read_bytes(address, &reg, 1, &value, 1)) {
        ESP_LOGW(kTag, "read failed: address=0x%02x register=0x%02x", address, reg);
        return 0xff;
    }
    return value;
}

int SCCB_Write(std::uint8_t address, std::uint8_t reg, std::uint8_t value)
{
    const std::array<std::uint8_t, 2> bytes{reg, value};
    return write_bytes(address, bytes.data(), bytes.size()) ? 0 : -1;
}

std::uint8_t SCCB_Read16(std::uint8_t address, std::uint16_t reg)
{
    const auto bytes = register_bytes(reg);
    std::uint8_t value = 0xff;
    return read_bytes(address, bytes.data(), bytes.size(), &value, 1) ? value : 0xff;
}

int SCCB_Write16(std::uint8_t address, std::uint16_t reg, std::uint8_t value)
{
    const auto reg_bytes = register_bytes(reg);
    const std::array<std::uint8_t, 3> bytes{reg_bytes[0], reg_bytes[1], value};
    return write_bytes(address, bytes.data(), bytes.size()) ? 0 : -1;
}

std::uint16_t SCCB_Read_Addr16_Val16(std::uint8_t address, std::uint16_t reg)
{
    const auto bytes = register_bytes(reg);
    std::array<std::uint8_t, 2> value{};
    if (!read_bytes(address, bytes.data(), bytes.size(), value.data(), value.size())) {
        return 0xffff;
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(value[0]) << 8) | value[1]);
}

int SCCB_Write_Addr16_Val16(std::uint8_t address, std::uint16_t reg, std::uint16_t value)
{
    const auto reg_bytes = register_bytes(reg);
    const auto value_bytes = register_bytes(value);
    const std::array<std::uint8_t, 4> bytes{reg_bytes[0], reg_bytes[1], value_bytes[0], value_bytes[1]};
    return write_bytes(address, bytes.data(), bytes.size()) ? 0 : -1;
}

} // extern "C"
