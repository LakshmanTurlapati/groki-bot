// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/led_strip.hpp"

#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::board {

namespace {

// Pack (r,g,b) into the 2-byte RGB565 LE pair the PY32 LED RAM expects.
// Matches the upstream BSP packing (((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3)).
inline void pack_rgb565_le(std::uint8_t* out, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    const std::uint16_t v = Py32Expander::rgb565(r, g, b);
    out[0] = static_cast<std::uint8_t>(v & 0xFF);       // lo: GGG BBBBB
    out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF); // hi: RRRRR GGG
}

} // namespace

tl::expected<void, Error> Py32LedStrip::begin()
{
    // Same order as M5Stack's StackChan BSP: IO14 becomes a push-pull output
    // (the PY32 only drives the WS2812 line then), then the LED count. The
    // first frame is pushed twice after a pause, as the BSP does, so it
    // latches even if the PY32 is still applying the new count.
    const std::uint8_t pin = Py32Expander::kPinLedData;
    if (auto r = expander_->set_direction(pin, true); !r) return r;
    if (auto r = expander_->set_pull_up(pin, true); !r) return r;
    if (auto r = expander_->set_open_drain(pin, false); !r) return r;
    if (auto r = expander_->set_led_count(count_); !r) return r;
    vTaskDelay(pdMS_TO_TICKS(200));
    clear();
    if (auto r = push(); !r) return r;
    vTaskDelay(pdMS_TO_TICKS(50));
    return push();
}

void Py32LedStrip::clear() noexcept
{
    buf_.fill(0);
}

void Py32LedStrip::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    for (std::size_t i = 0; i < count_; ++i) {
        pack_rgb565_le(&buf_[i * 2], r, g, b);
    }
}

void Py32LedStrip::set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (index >= count_) return;
    pack_rgb565_le(&buf_[index * 2], r, g, b);
}

tl::expected<void, Error> Py32LedStrip::show()
{
    const std::size_t bytes = static_cast<std::size_t>(count_) * 2;
    if (shown_valid_ && std::equal(buf_.begin(), buf_.begin() + bytes, shown_.begin())) {
        return {};
    }
    return push();
}

tl::expected<void, Error> Py32LedStrip::push()
{
    // A failed write leaves the cache invalid so the next show() retries.
    shown_valid_ = false;
    if (auto r = expander_->write_led_colors(buf_.data(), count_); !r) return r;
    if (auto r = expander_->refresh_leds(); !r) return r;
    shown_ = buf_;
    shown_valid_ = true;
    return {};
}

MirroredLedStrip::MirroredLedStrip(std::unique_ptr<LedStrip> primary, std::unique_ptr<LedStrip> mirror)
    : primary_{std::move(primary)}, mirror_{std::move(mirror)}, colors_(primary_->size())
{
}

tl::expected<void, Error> MirroredLedStrip::begin()
{
    auto primary = primary_->begin();
    const auto mirror = mirror_->begin();
    primary_ok_ = primary.has_value();
    mirror_ok_ = mirror.has_value();
    if (primary_ok_ || mirror_ok_) return {};
    return primary;
}

void MirroredLedStrip::clear() noexcept
{
    fill(0, 0, 0);
}

void MirroredLedStrip::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    primary_->fill(r, g, b);
    for (auto& c : colors_) c = {r, g, b};
}

void MirroredLedStrip::set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (index >= colors_.size()) return;
    primary_->set(index, r, g, b);
    colors_[index] = {r, g, b};
}

std::size_t MirroredLedStrip::source_index(std::size_t mirror_led) const noexcept
{
    const std::size_t half = colors_.size() / 2;
    const std::size_t mirror_half = mirror_->size() / 2;
    if (half == 0 || mirror_half == 0) return 0;
    const bool right = mirror_led >= mirror_half;
    const std::size_t offset = right ? mirror_led - mirror_half : mirror_led;
    return (right ? half : 0) + std::min(offset * half / mirror_half, half - 1);
}

tl::expected<void, Error> MirroredLedStrip::show()
{
    tl::expected<void, Error> result = tl::unexpected{Error::LedStripIo};
    if (primary_ok_) result = primary_->show();
    if (mirror_ok_) {
        for (std::size_t i = 0; i < mirror_->size(); ++i) {
            const auto& c = colors_[source_index(i)];
            mirror_->set(i, c[0], c[1], c[2]);
        }
        auto mirrored = mirror_->show();
        if (!result) result = mirrored;
    }
    return result;
}

} // namespace stackchan::board
