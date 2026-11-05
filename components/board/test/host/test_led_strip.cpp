// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "board/led_strip.hpp"

#include <M5Unified.h>
#include <freertos/task.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using stackchan::board::Error;
using stackchan::board::LedStrip;
using stackchan::board::MirroredLedStrip;
using stackchan::board::Py32Expander;
using stackchan::board::Py32LedStrip;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

// Simulated PY32 register file at 0x6F.
std::array<std::uint8_t, 256> regs{};
struct Write {
    std::uint8_t reg;
    std::vector<std::uint8_t> bytes;
};
std::vector<Write> writes;
std::vector<std::uint32_t> delays;
bool fail_writes = false;

void reset_bus()
{
    regs.fill(0);
    regs[0x02] = 0x41; // firmware version, so probe() succeeds
    writes.clear();
    delays.clear();
    fail_writes = false;
}

std::size_t writes_to(std::uint8_t reg)
{
    std::size_t n = 0;
    for (const auto& w : writes) n += w.reg == reg;
    return n;
}

bool bit(std::uint8_t reg, unsigned b)
{
    return (regs[reg] >> b) & 1u;
}

class RecordingStrip : public LedStrip {
public:
    explicit RecordingStrip(std::size_t n, bool ok = true) : colors(n), ok_{ok} {}
    tl::expected<void, Error> begin() override
    {
        if (!ok_) return tl::unexpected{Error::LedStripIo};
        return {};
    }
    void clear() noexcept override { fill(0, 0, 0); }
    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override
    {
        for (auto& c : colors) c = {r, g, b};
    }
    void set(std::size_t i, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override
    {
        if (i < colors.size()) colors[i] = {r, g, b};
    }
    tl::expected<void, Error> show() override
    {
        ++shows;
        return {};
    }
    std::size_t size() const noexcept override { return colors.size(); }

    std::vector<std::array<std::uint8_t, 3>> colors;
    int shows = 0;

private:
    bool ok_;
};

Py32Expander expander()
{
    auto e = Py32Expander::probe();
    check(e.has_value(), "probe failed against the simulated PY32");
    return std::move(*e);
}

void test_begin_configures_led_pin_like_the_bsp()
{
    reset_bus();
    regs[0x0C] = 0x20; // pull-down on IO14 must be cleared
    regs[0x14] = 0x20; // open-drain on IO14 must be cleared
    regs[0x03] = 0x01; // servo-power pin direction must stay untouched
    auto e = expander();
    Py32LedStrip strip{e, 12};

    check(strip.begin().has_value(), "begin failed");

    check(bit(0x04, 5), "IO14 is not an output");
    check(bit(0x0A, 5), "IO14 pull-up not enabled");
    check(!bit(0x0C, 5), "IO14 pull-down still enabled");
    check(!bit(0x14, 5), "IO14 is not push-pull");
    check(regs[0x03] == 0x01, "servo-power pin direction changed");
    check((regs[0x24] & 0x3F) == 12, "LED count not 12");
    check(writes_to(0x30) == 2, "first frame should be pushed twice");
    check(delays == std::vector<std::uint32_t>({200, 50}), "BSP settle delays missing");
}

void test_show_writes_rgb565_once_per_change()
{
    reset_bus();
    auto e = expander();
    Py32LedStrip strip{e, 12};
    check(strip.begin().has_value(), "begin failed");
    writes.clear();

    strip.fill(40, 40, 40);
    check(strip.show().has_value(), "show failed");
    check(writes_to(0x30) == 1 && writes_to(0x24) == 1, "changed frame not written once");
    check(writes.front().bytes.size() == 24, "RAM write should cover 12 LEDs");
    check(writes.front().bytes[0] == 0x45 && writes.front().bytes[1] == 0x29, "not RGB565 little-endian");
    check(regs[0x24] == (12 | 0x40), "refresh bit not set with the count");

    check(strip.show().has_value(), "repeat show failed");
    check(writes_to(0x30) == 1, "unchanged frame was written again");

    strip.set(3, 255, 255, 255);
    check(strip.show().has_value(), "show after change failed");
    check(writes_to(0x30) == 2, "changed LED was not written");
}

void test_failed_write_is_retried()
{
    reset_bus();
    auto e = expander();
    Py32LedStrip strip{e, 12};
    check(strip.begin().has_value(), "begin failed");
    writes.clear();

    strip.fill(255, 0, 0);
    fail_writes = true;
    check(!strip.show().has_value(), "failed write reported success");
    fail_writes = false;
    check(strip.show().has_value(), "retry failed");
    check(writes_to(0x30) == 1, "failed frame was not retried");
}

void test_mirror_samples_each_half()
{
    auto primary_owner = std::make_unique<RecordingStrip>(18);
    auto mirror_owner = std::make_unique<RecordingStrip>(12);
    auto* primary = primary_owner.get();
    auto* mirror = mirror_owner.get();
    MirroredLedStrip strip{std::move(primary_owner), std::move(mirror_owner)};
    check(strip.begin().has_value(), "mirrored begin failed");
    check(strip.size() == 18, "size should follow the primary strip");

    const std::size_t left[] = {0, 1, 3, 4, 6, 7};
    for (std::size_t i = 0; i < 6; ++i) {
        check(strip.source_index(i) == left[i], "left half mapping wrong");
        check(strip.source_index(6 + i) == 9 + left[i], "right half mapping wrong");
    }

    // led_task's level meter: paint everything, then blank the unlit LEDs.
    strip.fill(0, 0, 255);
    for (std::size_t i = 0; i < 18; ++i) {
        const std::size_t ear = i % 9;
        if (ear != 0 && ear != 8) strip.set(i, 0, 0, 0);
    }
    check(strip.show().has_value(), "mirrored show failed");
    check(primary->shows == 1 && mirror->shows == 1, "both strips must be shown");
    check(mirror->colors[0][2] == 255 && mirror->colors[6][2] == 255, "lit outer LED lost on the mirror");
    check(mirror->colors[1][2] == 0, "blanked LED shown lit on the mirror");
}

void test_mirror_survives_one_missing_strip()
{
    auto mirror_owner = std::make_unique<RecordingStrip>(12);
    auto* mirror = mirror_owner.get();
    MirroredLedStrip strip{std::make_unique<RecordingStrip>(18, false), std::move(mirror_owner)};
    check(strip.begin().has_value(), "one usable strip should be enough");
    strip.fill(1, 2, 3);
    check(strip.show().has_value(), "show through the usable strip failed");
    check(mirror->shows == 1 && mirror->colors[11][0] == 1, "usable strip not driven");

    MirroredLedStrip none{std::make_unique<RecordingStrip>(18, false), std::make_unique<RecordingStrip>(12, false)};
    check(!none.begin().has_value(), "no usable strip should fail begin");
}

} // namespace

namespace m5 {
std::uint8_t I2c::readRegister8(std::uint8_t address, std::uint8_t reg, std::uint32_t) const
{
    return address == Py32Expander::kAddress ? regs[reg] : 0;
}

bool I2c::writeRegister8(std::uint8_t address, std::uint8_t reg, std::uint8_t value, std::uint32_t) const
{
    return writeRegister(address, reg, &value, 1, 0);
}

bool I2c::writeRegister(std::uint8_t address, std::uint8_t reg, const std::uint8_t* data, std::size_t length,
                        std::uint32_t) const
{
    if (fail_writes || address != Py32Expander::kAddress) return false;
    writes.push_back({reg, std::vector<std::uint8_t>(data, data + length)});
    for (std::size_t i = 0; i < length; ++i) regs[reg + i] = data[i];
    // The PY32 latches and clears the refresh bit itself; keep it visible
    // here so the test can see that it was requested.
    return true;
}
} // namespace m5

void vTaskDelay(std::uint32_t ticks)
{
    delays.push_back(ticks);
}

int main()
{
    test_begin_configures_led_pin_like_the_bsp();
    test_show_writes_rgb565_once_per_change();
    test_failed_write_is_retried();
    test_mirror_samples_each_half();
    test_mirror_survives_one_missing_strip();
    std::puts("led strip tests passed");
}
