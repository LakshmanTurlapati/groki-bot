// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <tl/expected.hpp>

#include "board/board.hpp"
#include "board/io_expander_py32.hpp"

namespace stackchan::board {

// Abstract LED strip — same surface used by main/led_task and SharedState's
// led_mode / led_color / led_brightness fields, so the animation code is
// strip-agnostic. Concrete drivers (Py32LedStrip for the M5-base PY32 ring,
// NekomimiLedStrip for the GPIO9 cat-ear WS2812 chain) plug in via
// std::unique_ptr<LedStrip> on Board::Impl.
class LedStrip {
public:
    virtual ~LedStrip() noexcept = default;

    // Allocate / hand-shake the wire format on the strip's underlying
    // transport. Must be called once before the first show().
    virtual tl::expected<void, Error> begin() = 0;

    // Local-buffer writes — none of these touch the wire; show() pushes the
    // whole buffer in one burst (or RMT transmit).
    virtual void clear() noexcept = 0;
    virtual void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept = 0;
    virtual void set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept = 0;

    // Push the local frame buffer to the strip + latch.
    virtual tl::expected<void, Error> show() = 0;

    // Number of LEDs the driver owns. set()'s `index` must be < size().
    virtual std::size_t size() const noexcept = 0;
};

// NeoPixel strip on the M5 Stack-chan base (12 × WS2812 on the back). The PY32
// MCU on the base handles the WS2812 timing; this class is a thin host-side
// frame buffer + I2C burst writer. A single show() pushes the whole buffer to
// PY32 RAM in one transaction and latches it onto the strip with a refresh.
// Not present on the Takao base — Board::led_strip() returns nullptr there.
class Py32LedStrip : public LedStrip {
public:
    Py32LedStrip(Py32Expander& expander, std::uint8_t count) noexcept
        : expander_{&expander}, count_{count}
    {
    }

    // Make IO14 a push-pull output, push the strip count to the PY32 and
    // clear the local frame buffer + strip (M5Stack StackChan BSP sequence).
    // Call once before the first show().
    tl::expected<void, Error> begin() override;

    void clear() noexcept override;
    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override;
    void set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override;

    // Push the local buffer to the strip + latch. Skips the I2C traffic when
    // the frame equals the last one pushed: in_I2C is shared with the display
    // and touch, and a steady 10 Hz refresh once triggered an lgfx mutex
    // race (docs/known_issues.md §1).
    tl::expected<void, Error> show() override;

    std::size_t size() const noexcept override { return count_; }

private:
    tl::expected<void, Error> push();

    Py32Expander* expander_;
    std::uint8_t count_;
    // 2 bytes per LED stored as RGB565 little-endian (= [lo, hi]). The PY32
    // firmware itself converts to WS2812 GRB on the wire; the host stays in
    // 565 LE. See docs/py32_ioexpander.md §6 for the bit layout.
    std::array<std::uint8_t, Py32Expander::kMaxLeds * 2> buf_{};
    std::array<std::uint8_t, Py32Expander::kMaxLeds * 2> shown_{};
    bool shown_valid_ = false;
};

// Drives a primary strip and mirrors it onto a second, shorter strip whose
// LEDs are split the same way into a left and a right half. The M5 base
// uses it for the optional cat-ear chain (primary, 9 per ear) plus the
// base's own 12 LEDs (6 per side), so either kind of robot lights up. size()
// is the primary's, so led_task's per-ear effects keep their layout.
class MirroredLedStrip : public LedStrip {
public:
    MirroredLedStrip(std::unique_ptr<LedStrip> primary, std::unique_ptr<LedStrip> mirror);

    // Succeeds when at least one strip is usable.
    tl::expected<void, Error> begin() override;
    void clear() noexcept override;
    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override;
    void set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept override;
    // Samples the primary frame onto the mirror, then shows both.
    tl::expected<void, Error> show() override;
    std::size_t size() const noexcept override { return primary_->size(); }

    // Primary LED whose colour mirror LED `mirror_led` shows (same half).
    std::size_t source_index(std::size_t mirror_led) const noexcept;

private:
    std::unique_ptr<LedStrip> primary_;
    std::unique_ptr<LedStrip> mirror_;
    // Primary frame, kept so later set() calls cannot overwrite a mirror LED
    // that several primary LEDs share.
    std::vector<std::array<std::uint8_t, 3>> colors_;
    bool primary_ok_ = false;
    bool mirror_ok_ = false;
};

} // namespace stackchan::board
