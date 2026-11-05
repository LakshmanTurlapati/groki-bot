// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0
//
// Multi-producer tests for SharedState's overlay command channel: concurrent
// publishers must never let a reader observe a torn command, and a stale
// clear must never remove a newer overlay (Issue #4 arbitration).

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "shared_state.hpp"
#include "test_support.hpp"

using stackchan::app::SharedState;
using stackchan::avatar::Expression;

int main() {
    // Concurrent publishers: every observed command must be internally
    // consistent — the hold encodes the same thread id as the expression.
    {
        SharedState st;
        constexpr std::size_t kThreads = 4;
        constexpr std::int32_t kIters = 20000;
        std::atomic<bool> stop{false};
        std::atomic<std::int32_t> torn{0};

        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint32_t cmd = st.face.overlay_command.load(std::memory_order_acquire);
                if ((cmd & SharedState::kOverlayValidBit) != 0) {
                    const auto expr = static_cast<std::uint8_t>((cmd >> 15) & 0xFF);
                    const auto hold = cmd & 0x7FFFu;
                    // Publisher t writes expression t and hold 1000 + t.
                    if (hold != 1000u + expr) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });

        std::vector<std::thread> writers;
        for (std::size_t t = 0; t < kThreads; ++t) {
            writers.emplace_back([&, t] {
                for (std::int32_t i = 0; i < kIters; ++i) {
                    st.request_face_overlay(static_cast<Expression>(t), 1000u + static_cast<std::uint32_t>(t));
                }
            });
        }
        for (auto& w : writers) {
            w.join();
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();
        CHECK(torn.load() == 0);
    }

    // A stale clear must not remove a newer overlay from another source.
    {
        SharedState st;
        const std::uint32_t mine = st.request_face_overlay(Expression::Happy, 0);
        const std::uint32_t newer = st.request_face_overlay(Expression::Listening, 5000);
        CHECK(!st.clear_face_overlay_if(mine));
        CHECK(st.face.overlay_command.load() == newer);
        CHECK(st.clear_face_overlay_if(newer));
        CHECK((st.face.overlay_command.load() & SharedState::kOverlayValidBit) == 0);
        // Clearing with an invalid token is a no-op.
        CHECK(!st.clear_face_overlay_if(0));
    }

    // Bot activity: one word carries state + TTL (100 ms units, rounded up,
    // capped), a re-send of the same state is still a new word (keepalive),
    // and a clear drops the valid bit. Every write counts as activity.
    {
        SharedState st;
        const std::uint32_t before = st.face.activity_seq.load();
        st.request_bot_activity(Expression::Working, 120'000);
        const std::uint32_t first = st.face.activity_command.load();
        CHECK((first & SharedState::kOverlayValidBit) != 0);
        CHECK(((first >> 15) & 0xFF) == static_cast<std::uint32_t>(Expression::Working));
        CHECK((first & 0x7FFFu) * SharedState::kActivityTtlUnitMs == 120'000u);
        st.request_bot_activity(Expression::Working, 120'000);
        CHECK(st.face.activity_command.load() != first);
        st.request_bot_activity(Expression::Done, 0);
        CHECK((st.face.activity_command.load() & 0x7FFFu) == 0u);
        st.request_bot_activity(Expression::Waiting, 50);
        CHECK((st.face.activity_command.load() & 0x7FFFu) == 1u);
        st.request_bot_activity(Expression::Waiting, 0xFFFFFFFFu);
        CHECK((st.face.activity_command.load() & 0x7FFFu) == SharedState::kOverlayHoldCapMs);
        st.clear_bot_activity();
        CHECK((st.face.activity_command.load() & SharedState::kOverlayValidBit) == 0);
        CHECK(st.face.activity_seq.load() == before + 6);

        // By name (gateway message / HTTP): known states publish, idle
        // clears, unknown names change nothing.
        CHECK(st.request_bot_activity(std::string_view{"blocked"}, 0));
        const std::uint32_t blocked = st.face.activity_command.load();
        CHECK(((blocked >> 15) & 0xFF) == static_cast<std::uint32_t>(Expression::Blocked));
        CHECK(!st.request_bot_activity(std::string_view{"partying"}, 0));
        CHECK(st.face.activity_command.load() == blocked);
        CHECK(st.request_bot_activity(std::string_view{"idle"}, 0));
        CHECK((st.face.activity_command.load() & SharedState::kOverlayValidBit) == 0);
    }

    // A message (balloon) resets idle decay: activity_seq must bump.
    {
        SharedState st;
        const std::uint32_t before = st.face.activity_seq.load();
        st.set_balloon_text("在！", 1000);
        CHECK(st.face.activity_seq.load() == before + 1);
    }

    return avtest::finish("overlay_publish");
}
