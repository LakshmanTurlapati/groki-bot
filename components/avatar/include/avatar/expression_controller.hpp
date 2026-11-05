// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"

namespace stackchan::avatar {

// Cross-frame expression ease plus three-source arbitration. The VM zeros
// locals every run(), so blend progress cannot live in DSL; this controller
// records from/to and two nested holds of interrupted mixes, then writes
// them into DrawContext.
//
// Sources (highest first): timed interactive overlay, voice listening, Grok
// Bot activity (run state reported by the gateway), voice thinking
// (including tool-call wait), idle decay (bored → sleepy), then conversation
// mood (LLM / MCP / button). `set_target` is the blender input; `resolve`
// picks the live face from the sources.
class ExpressionController {
public:
    static constexpr std::uint32_t kDurationMs = 300;
    static constexpr std::uint32_t kDefaultOverlayHoldMs = 3000;
    static constexpr std::uint32_t kBoredAfterMs = 2u * 60u * 1000u;
    static constexpr std::uint32_t kSleepyAfterMs = 10u * 60u * 1000u;
    // A Blocked run that nobody resolves gives up and shows Failed.
    static constexpr std::uint32_t kBlockedToFailedMs = 30'000;

    // Lifetime of an activity when the sender passes ttl_ms == 0. Every
    // state expires on its own so a vanished gateway cannot pin the face;
    // 0 here means "until replaced" (Blocked escalates instead).
    static constexpr std::uint32_t default_activity_ttl_ms(Expression state) noexcept {
        switch (state) {
        case Expression::Done:
            return 3'700; // ~2.2 s hop clip + 1.5 s settle, then Idle
        case Expression::Failed:
            return 2'800; // 2.5 s hold + 0.3 s recover
        case Expression::Waiting:
            return 10u * 60u * 1000u; // then idle decay takes it to Sleeping
        case Expression::Listening:
            return 45'000;
        case Expression::Blocked:
            return 0;
        default:
            return 120'000; // Thinking / Working: the gateway refreshes these
        }
    }

    void set_target(Expression to) noexcept {
        pending_ = to;
    }

    void set_base(Expression mood) noexcept {
        if (mood_ != mood) {
            mood_ = mood;
            activity_pending_ = true;
        }
    }

    void set_voice_state(VoiceState voice) noexcept {
        if (voice_ != voice) {
            voice_ = voice;
            activity_pending_ = true;
        }
    }

    // `hold_ms == 0` keeps the overlay until `clear_overlay`.
    void set_overlay(Expression overlay, std::uint32_t hold_ms) noexcept {
        overlay_ = overlay;
        overlay_hold_ms_ = hold_ms;
        overlay_started_ = false;
        activity_pending_ = true;
    }

    void clear_overlay() noexcept {
        overlay_.reset();
        overlay_started_ = false;
    }

    // Grok Bot run state. Re-sending the current state refreshes its TTL
    // (gateway keepalive) without restarting the Blocked → Failed clock.
    // `ttl_ms == 0` picks default_activity_ttl_ms().
    void set_activity(Expression state, std::uint32_t ttl_ms) noexcept {
        if (!run_state_ || *run_state_ != state) {
            run_since_pending_ = true;
        }
        run_state_ = state;
        run_ttl_ms_ = ttl_ms == 0 ? default_activity_ttl_ms(state) : ttl_ms;
        run_refresh_pending_ = true;
        activity_pending_ = true;
    }

    void clear_activity() noexcept {
        run_state_.reset();
        run_refresh_pending_ = false;
        run_since_pending_ = false;
        activity_pending_ = true;
    }

    std::optional<Expression> activity() const noexcept {
        return run_state_;
    }

    void note_activity() noexcept {
        activity_pending_ = true;
    }

    Expression resolve(std::uint32_t now_ms) noexcept {
        if (activity_pending_) {
            last_activity_ms_ = now_ms;
            activity_pending_ = false;
        }
        // Start the activity clocks at the first resolve after a write, even
        // when an overlay wins this frame, so TTLs measure from the write.
        if (run_refresh_pending_) {
            run_refresh_ms_ = now_ms;
            run_refresh_pending_ = false;
        }
        if (run_since_pending_) {
            run_since_ms_ = now_ms;
            run_since_pending_ = false;
        }
        asleep_ = false;

        if (overlay_) {
            if (!overlay_started_) {
                overlay_start_ms_ = now_ms;
                overlay_started_ = true;
            }
            const bool sticky = overlay_hold_ms_ == 0;
            if (sticky || now_ms - overlay_start_ms_ < overlay_hold_ms_) {
                last_resolved_ = *overlay_;
                return last_resolved_;
            }
            overlay_.reset();
            overlay_started_ = false;
        }

        if (voice_ == VoiceState::Listening) {
            last_resolved_ = Expression::Listening;
            return last_resolved_;
        }
        if (const auto run = resolve_activity(now_ms)) {
            last_resolved_ = *run;
            return last_resolved_;
        }
        if (voice_ == VoiceState::Thinking) {
            last_resolved_ = Expression::Thinking;
            return last_resolved_;
        }
        // Idle decay outranks a leftover mood: being ignored means the face
        // drifts bored → sleepy even if the LLM left it Happy. Any activity
        // (touch / wake / message) resets the clock and the mood shows again.
        const std::uint32_t idle_ms = now_ms - last_activity_ms_;
        if (idle_ms >= kSleepyAfterMs) {
            asleep_ = true;
            last_resolved_ = Expression::Sleepy;
            return last_resolved_;
        }
        if (idle_ms >= kBoredAfterMs) {
            last_resolved_ = Expression::Bored;
            return last_resolved_;
        }
        if (mood_ != Expression::Neutral) {
            last_resolved_ = mood_;
            return last_resolved_;
        }
        last_resolved_ = Expression::Neutral;
        return last_resolved_;
    }

    Expression last_resolved() const noexcept {
        return last_resolved_;
    }

    // True only when idle decay put the robot to sleep. A Sleepy mood or
    // gesture shows the same face but the robot is still awake.
    bool asleep() const noexcept {
        return asleep_;
    }

    void apply(DrawContext& ctx, std::uint32_t now_ms) noexcept {
        if (pending_) {
            if (*pending_ != to_) {
                retarget(*pending_);
                start_ms_ = now_ms;
            }
            pending_.reset();
        }

        if (from_ == to_ && !hold_active()) {
            blend_ = 1.0f;
        } else {
            const std::uint32_t elapsed = now_ms - start_ms_;
            float t = 1.0f;
            if (elapsed < kDurationMs) {
                t = static_cast<float>(elapsed) / static_cast<float>(kDurationMs);
            }
            blend_ = ease(t);
            if (blend_ >= 1.0f) {
                from_ = to_;
                hold_to_ = to_;
                hold_blend_ = 1.0f;
                hold2_to_ = to_;
                hold2_blend_ = 0.0f;
                blend_ = 1.0f;
            }
        }

        ctx.expression = to_;
        ctx.expression_from = from_;
        ctx.expression_blend = blend_;
        ctx.expression_hold_to = hold_to_;
        ctx.expression_hold_blend = hold_blend_;
        ctx.expression_hold2_to = hold2_to_;
        ctx.expression_hold2_blend = hold2_blend_;
    }

    Expression from() const noexcept {
        return from_;
    }
    Expression to() const noexcept {
        return to_;
    }
    float blend() const noexcept {
        return blend_;
    }
    Expression hold_to() const noexcept {
        return hold_to_;
    }
    float hold_blend() const noexcept {
        return hold_blend_;
    }
    Expression hold2_to() const noexcept {
        return hold2_to_;
    }
    float hold2_blend() const noexcept {
        return hold2_blend_;
    }

private:
    // Live run state after TTL expiry and Blocked → Failed escalation.
    // Expiry is not activity: the idle clock keeps counting from the last
    // write, so an unanswered Waiting (10 min TTL) lands straight on Sleepy.
    std::optional<Expression> resolve_activity(std::uint32_t now_ms) noexcept {
        if (!run_state_) {
            return std::nullopt;
        }
        if (*run_state_ == Expression::Blocked && now_ms - run_since_ms_ >= kBlockedToFailedMs) {
            run_state_ = Expression::Failed;
            run_ttl_ms_ = default_activity_ttl_ms(Expression::Failed);
            run_since_ms_ = now_ms;
            run_refresh_ms_ = now_ms;
        }
        if (run_ttl_ms_ != 0 && now_ms - run_refresh_ms_ >= run_ttl_ms_) {
            run_state_.reset();
            return std::nullopt;
        }
        return run_state_;
    }

    bool hold_active() const noexcept {
        return hold_blend_ < 1.0f || hold_to_ != from_;
    }

    bool hold2_active() const noexcept {
        return hold2_blend_ > 0.0f;
    }

    void retarget(Expression next) noexcept {
        // Freeze the on-screen mix as the new from-pose by shifting the chain
        // one level: the in-flight (to, blend) pair becomes the hold, and a
        // still-active hold moves into hold2. The frozen pose
        //   (1-hb)*[(1-hb2)*from + hb2*hold2_to] + hb*hold_to
        // then equals the pre-interrupt mix exactly for up to two nested
        // interrupts. A third simultaneous interrupt folds the two oldest
        // components into whichever carries more weight; the error is bounded
        // by the lighter, already-decaying weight.
        if (hold_active()) {
            if (hold2_active() && hold2_blend_ >= 0.5f) {
                from_ = hold2_to_;
            }
            hold2_to_ = hold_to_;
            hold2_blend_ = hold_blend_;
        } else {
            hold2_to_ = from_;
            hold2_blend_ = 0.0f;
        }
        hold_to_ = to_;
        hold_blend_ = blend_;
        to_ = next;
    }

    static float ease(float t) noexcept {
        if (t <= 0.0f) {
            return 0.0f;
        }
        if (t >= 1.0f) {
            return 1.0f;
        }
        return t * t * (3.0f - 2.0f * t);
    }

    Expression from_{Expression::Neutral};
    Expression to_{Expression::Neutral};
    Expression hold_to_{Expression::Neutral};
    Expression hold2_to_{Expression::Neutral};
    std::optional<Expression> pending_{};
    std::uint32_t start_ms_{0};
    float blend_{1.0f};
    float hold_blend_{1.0f};
    float hold2_blend_{0.0f};

    Expression mood_{Expression::Neutral};
    VoiceState voice_{VoiceState::Idle};
    std::optional<Expression> overlay_{};
    std::uint32_t overlay_hold_ms_{0};
    std::uint32_t overlay_start_ms_{0};
    bool overlay_started_{false};
    std::optional<Expression> run_state_{};
    std::uint32_t run_ttl_ms_{0};
    std::uint32_t run_refresh_ms_{0};
    std::uint32_t run_since_ms_{0};
    bool run_refresh_pending_{false};
    bool run_since_pending_{false};
    bool activity_pending_{false};
    std::uint32_t last_activity_ms_{0};
    bool asleep_{false};
    Expression last_resolved_{Expression::Neutral};
};

} // namespace stackchan::avatar
