// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <M5Unified.h>
#include <esp_err.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include "sccb.h"
}

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

bool enabled = true;
bool locked = false;
int operation = 0;
int fail_operation = 0;
int stops = 0;
std::vector<std::uint8_t> written;
std::vector<std::uint8_t> incoming{0x12, 0x34};

void reset(int failure = 0)
{
    check(!locked, "previous transaction leaked the I2C mutex");
    operation = 0;
    stops = 0;
    fail_operation = failure;
    written.clear();
}

bool transfer()
{
    check(locked, "transfer without owning the I2C mutex");
    if (++operation == fail_operation) {
        // Pinned M5GFX releases the mutex on a failed bus transfer.
        locked = false;
        return false;
    }
    return true;
}

void test_bus_ownership()
{
    reset();
    check(SCCB_Init(12, 11) == ESP_OK, "shared camera bus rejected");
    check(SCCB_Init(11, 12) == ESP_ERR_INVALID_ARG, "wrong camera pins accepted");
    check(SCCB_Use_Port(0) == ESP_OK, "shared camera port rejected");
    check(SCCB_Use_Port(1) == ESP_ERR_INVALID_ARG, "different I2C port accepted");
    check(SCCB_Deinit() == ESP_OK, "camera SCCB teardown failed");
    check(operation == 0, "init/deinit touched the shared bus");
    enabled = false;
    check(SCCB_Init(12, 11) == ESP_ERR_INVALID_ARG, "uninitialized M5 bus accepted");
    check(SCCB_Write(0x21, 1, 2) == -1, "write used unavailable bus");
    enabled = true;
    check(SCCB_Probe(0x21) == ESP_OK && !locked, "sensor address probe failed");
    reset(2);
    check(SCCB_Probe(0x21) != ESP_OK && !locked, "probe NACK did not release mutex");
    reset();
    check(SCCB_Probe(0) != ESP_OK && operation == 0, "reserved address reached the bus");
}

void test_register_byte_order()
{
    reset();
    check(SCCB_Write(0x21, 0xfe, 1) == 0, "8-bit register write failed");
    check(written == std::vector<std::uint8_t>{0xfe, 1}, "wrong 8-bit register bytes");
    reset();
    check(SCCB_Read(0x21, 0x53) == 0x12, "8-bit register read failed");
    check(written == std::vector<std::uint8_t>{0x53}, "wrong 8-bit read address");
    reset();
    check(SCCB_Write16(0x21, 0x1234, 0x56) == 0, "16-bit register write failed");
    check(written == std::vector<std::uint8_t>{0x12, 0x34, 0x56}, "wrong 16-bit address order");
    reset();
    check(SCCB_Read16(0x21, 0x1234) == 0x12, "16-bit register read failed");
    check(written == std::vector<std::uint8_t>{0x12, 0x34}, "wrong 16-bit read address order");
    reset();
    check(SCCB_Write_Addr16_Val16(0x21, 0x1234, 0x5678) == 0, "16-bit register/value write failed");
    check(written == std::vector<std::uint8_t>{0x12, 0x34, 0x56, 0x78}, "wrong 16-bit value order");
    reset();
    check(SCCB_Read_Addr16_Val16(0x21, 0x5678) == 0x1234, "wrong 16-bit read value order");
}

void test_transfer_failures()
{
    for (int failure = 1; failure <= 5; ++failure) {
        reset(failure);
        check(SCCB_Read(0x21, 0x53) == 0xff, "failed read returned valid-looking data");
        check(!locked, "failed read leaked mutex");
        check(operation == failure, "read continued after a failed transaction");
        check(stops == (failure == 5 ? 1 : 0), "read called stop after a failure had already released mutex");
    }
    for (int failure = 1; failure <= 3; ++failure) {
        reset(failure);
        check(SCCB_Write(0x21, 0x53, 0x20) == -1, "failed write reported success");
        check(!locked && operation == failure, "write continued after failed transaction");
        check(stops == (failure == 3 ? 1 : 0), "write released mutex twice");
    }
    reset();
    check(SCCB_Write(0x21, 0x53, 0x20) == 0 && !locked, "bus did not recover after failed transfers");
}

} // namespace

bool m5::I2c::isEnabled() const { return enabled; }
int m5::I2c::getSDA() const { return 12; }
int m5::I2c::getSCL() const { return 11; }
int m5::I2c::getPort() const { return 0; }
bool m5::I2c::start(std::uint8_t address, bool read, std::uint32_t frequency) const
{
    check(!locked && address == 0x21 && !read && frequency == CONFIG_SCCB_CLK_FREQ, "invalid transaction start");
    locked = true;
    return transfer();
}
bool m5::I2c::restart(std::uint8_t address, bool read, std::uint32_t frequency) const
{
    check(address == 0x21 && read && frequency == CONFIG_SCCB_CLK_FREQ, "invalid read restart");
    return transfer();
}
bool m5::I2c::write(const std::uint8_t* data, std::size_t size) const
{
    written.insert(written.end(), data, data + size);
    return transfer();
}
bool m5::I2c::read(std::uint8_t* result, std::size_t size, bool last_nack) const
{
    check(last_nack && size <= incoming.size(), "invalid read size or missing final NACK");
    std::copy_n(incoming.begin(), size, result);
    return transfer();
}
bool m5::I2c::stop() const
{
    ++stops;
    const bool ok = transfer();
    locked = false;
    return ok;
}

int main()
{
    test_bus_ownership();
    test_register_byte_order();
    test_transfer_failures();
    std::puts("camera SCCB tests passed");
}
