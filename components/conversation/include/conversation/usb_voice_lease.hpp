// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <atomic>
#include <cstdint>

namespace stackchan::conversation {

// The USB reader owns renewals; the audio sender can inspect the lease.
// A live cable alone does not mean the gateway process is still listening.
class UsbVoiceLease {
public:
    static constexpr std::uint32_t kDurationMs = 10000;

    bool active(std::uint32_t now) const {
        // A concurrent renewal can be newer than the caller's clock sample.
        // Signed elapsed time also handles the millisecond counter wrapping.
        return enabled_.load(std::memory_order_acquire) &&
               static_cast<std::int32_t>(now - renewed_at_.load(std::memory_order_relaxed)) <
                   static_cast<std::int32_t>(kDurationMs);
    }
    bool enable(std::uint32_t now) {
        const bool starting = !active(now);
        renewed_at_.store(now, std::memory_order_relaxed);
        enabled_.store(true, std::memory_order_release);
        return starting;
    }
    bool refresh(std::uint32_t now) {
        if (!active(now))
            return false;
        renewed_at_.store(now, std::memory_order_relaxed);
        return true;
    }
    void disable() {
        enabled_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint32_t> renewed_at_{0};
};

} // namespace stackchan::conversation
