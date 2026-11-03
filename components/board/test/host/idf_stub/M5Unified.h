// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace m5 {
struct I2c {
    bool isEnabled() const;
    int getSDA() const;
    int getSCL() const;
    int getPort() const;
    bool start(std::uint8_t address, bool read, std::uint32_t frequency) const;
    bool restart(std::uint8_t address, bool read, std::uint32_t frequency) const;
    bool write(const std::uint8_t* data, std::size_t size) const;
    bool read(std::uint8_t* result, std::size_t size, bool last_nack) const;
    bool stop() const;
};
inline I2c In_I2C;
} // namespace m5
