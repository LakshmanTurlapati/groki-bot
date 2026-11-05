// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0
//
// Seam tests for avatar::ExpressionController. The VM zeros locals every
// run(), so from/to/hold/blend live here and are written into DrawContext.

#include <cmath>
#include <cstddef>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/expression_controller.hpp"
#include "test_support.hpp"

using stackchan::avatar::DrawContext;
using stackchan::avatar::Expression;
using stackchan::avatar::ExpressionController;
using stackchan::avatar::VoiceState;

namespace {

constexpr std::size_t kExprCount = stackchan::avatar::kExpressionCount;

// Left-eye half-segment lengths from grok_face.avdsl. Independent of the
// controller: a jump in these mixed values is a visible snap.
constexpr float kGrokLhs[kExprCount] = {
    11.3f, // Neutral
    3.5f,  // Happy
    12.0f, // Sad
    5.0f,  // Angry
    12.0f, // Doubt
    5.0f,  // Sleepy
    10.0f, // Listening
    13.0f, // Thinking
    8.5f,  // Excited
    12.5f, // Curious
    11.5f, // Confused
    6.0f,  // Surprised
    9.0f,  // Dizzy
    4.2f,  // Affection
    10.5f, // Bored
    14.0f, // Working
    6.5f,  // Waiting
    7.5f,  // Blocked
    3.0f,  // Done
    9.5f,  // Failed
};

bool near(float a, float b) {
    return std::fabs(a - b) < 1.0e-5f;
}

std::size_t expr_index(Expression e) {
    return static_cast<std::size_t>(e);
}

// Expression mix as grok_face.avdsl consumes DrawContext. The frozen pose is
// (1-hb)*[(1-hb2)*from + hb2*hold2_to] + hb*hold_to; visible mixes it with
// `expression` by `expression_blend`.
void mix_weights(const DrawContext& ctx, float w[kExprCount]) {
    for (std::size_t i = 0; i < kExprCount; ++i) {
        w[i] = 0.0f;
    }
    const float b = ctx.expression_blend;
    const float hb = ctx.expression_hold_blend;
    const float hb2 = ctx.expression_hold2_blend;
    w[expr_index(ctx.expression_from)] += (1.0f - hb2) * (1.0f - hb) * (1.0f - b);
    w[expr_index(ctx.expression_hold2_to)] += hb2 * (1.0f - hb) * (1.0f - b);
    w[expr_index(ctx.expression_hold_to)] += hb * (1.0f - b);
    w[expr_index(ctx.expression)] += b;
}

float mix_lhs(const DrawContext& ctx) {
    float w[kExprCount];
    mix_weights(ctx, w);
    float lhs = 0.0f;
    for (std::size_t i = 0; i < kExprCount; ++i) {
        lhs += w[i] * kGrokLhs[i];
    }
    return lhs;
}

} // namespace

int main() {
    // Idle: Neutral → Neutral, blend already complete, hold inactive.
    {
        ExpressionController c;
        CHECK(c.from() == Expression::Neutral);
        CHECK(c.to() == Expression::Neutral);
        CHECK(near(c.blend(), 1.0f));
        CHECK(c.hold_to() == Expression::Neutral);
        CHECK(near(c.hold_blend(), 1.0f));

        DrawContext ctx;
        c.apply(ctx, 10'000);
        CHECK(ctx.expression == Expression::Neutral);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 1.0f));
        CHECK(ctx.expression_hold_to == Expression::Neutral);
        CHECK(near(ctx.expression_hold_blend, 1.0f));
    }

    // set_target is a request; from/to do not change until apply() sees a clock.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        CHECK(c.to() == Expression::Neutral);
        CHECK(near(c.blend(), 1.0f));
    }

    // First apply starts the clock: blend 0 at t0, target Happy, source Neutral.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 1000);
        CHECK(c.from() == Expression::Neutral);
        CHECK(c.to() == Expression::Happy);
        CHECK(near(c.blend(), 0.0f));
        CHECK(c.hold_to() == Expression::Neutral);
        CHECK(near(c.hold_blend(), 1.0f));
        CHECK(ctx.expression == Expression::Happy);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 0.0f));
        CHECK(ctx.expression_hold_to == Expression::Neutral);
        CHECK(near(ctx.expression_hold_blend, 1.0f));
    }

    // 100 ms of 300 ms is smoothstep(1/3) = 7/27.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        CHECK(near(c.blend(), 7.0f / 27.0f));
        CHECK(ctx.expression == Expression::Happy);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 7.0f / 27.0f));
    }

    // Midpoint of 300 ms is smoothstep(0.5) = 0.5.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs / 2);
        CHECK(near(c.blend(), 0.5f));
        CHECK(ctx.expression == Expression::Happy);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 0.5f));
    }

    // At duration the blend completes and from catches up to to.
    {
        ExpressionController c;
        c.set_target(Expression::Sad);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs);
        CHECK(c.from() == Expression::Sad);
        CHECK(c.to() == Expression::Sad);
        CHECK(near(c.blend(), 1.0f));
        CHECK(c.hold_to() == Expression::Sad);
        CHECK(near(c.hold_blend(), 1.0f));
        CHECK(ctx.expression == Expression::Sad);
        CHECK(ctx.expression_from == Expression::Sad);
        CHECK(near(ctx.expression_blend, 1.0f));
    }

    // Past duration stays complete.
    {
        ExpressionController c;
        c.set_target(Expression::Angry);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs + 50);
        CHECK(c.from() == Expression::Angry);
        CHECK(near(c.blend(), 1.0f));
    }

    // Re-requesting the current target does not restart the ease.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs);
        c.set_target(Expression::Happy);
        c.apply(ctx, ExpressionController::kDurationMs + 80);
        CHECK(c.from() == Expression::Happy);
        CHECK(c.to() == Expression::Happy);
        CHECK(near(c.blend(), 1.0f));
    }

    // Mid-ease retarget freezes the current pair as hold; from stays put so
    // the visible pose does not snap to the previous target.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        c.set_target(Expression::Sleepy);
        c.apply(ctx, 140);
        CHECK(c.from() == Expression::Neutral);
        CHECK(c.to() == Expression::Sleepy);
        CHECK(near(c.blend(), 0.0f));
        CHECK(c.hold_to() == Expression::Happy);
        CHECK(near(c.hold_blend(), 7.0f / 27.0f));
        CHECK(ctx.expression == Expression::Sleepy);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 0.0f));
        CHECK(ctx.expression_hold_to == Expression::Happy);
        CHECK(near(ctx.expression_hold_blend, 7.0f / 27.0f));
    }

    // After the nested ease finishes, hold is idle on the new target.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        c.set_target(Expression::Sleepy);
        c.apply(ctx, 140);
        c.apply(ctx, 140 + ExpressionController::kDurationMs);
        CHECK(c.from() == Expression::Sleepy);
        CHECK(c.to() == Expression::Sleepy);
        CHECK(near(c.blend(), 1.0f));
        CHECK(c.hold_to() == Expression::Sleepy);
        CHECK(near(c.hold_blend(), 1.0f));
    }

    // Mid-ease return to the source eases from the frozen pose, not a snap.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        c.set_target(Expression::Neutral);
        c.apply(ctx, 140);
        CHECK(c.from() == Expression::Neutral);
        CHECK(c.to() == Expression::Neutral);
        CHECK(near(c.blend(), 0.0f));
        CHECK(c.hold_to() == Expression::Happy);
        CHECK(near(c.hold_blend(), 7.0f / 27.0f));
    }

    // A second interrupt while hold is already live must keep the current mix
    // as the from-pose. Flattening hold_blend to 1 would snap to Sleepy.
    // Overwriting hold with the current to/blend would drop Happy (140/729).
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        c.set_target(Expression::Sleepy);
        c.apply(ctx, 100);
        c.apply(ctx, 200);

        float w_before[kExprCount];
        mix_weights(ctx, w_before);
        const float lhs_before = mix_lhs(ctx);
        CHECK(near(w_before[expr_index(Expression::Neutral)], 400.0f / 729.0f));
        CHECK(near(w_before[expr_index(Expression::Happy)], 140.0f / 729.0f));
        CHECK(near(w_before[expr_index(Expression::Sleepy)], 189.0f / 729.0f));
        CHECK(near(lhs_before, 5955.0f / 729.0f));

        c.set_target(Expression::Angry);
        c.apply(ctx, 200);

        float w_after[kExprCount];
        mix_weights(ctx, w_after);
        CHECK(c.to() == Expression::Angry);
        CHECK(near(c.blend(), 0.0f));
        CHECK(ctx.expression == Expression::Angry);
        CHECK(near(ctx.expression_blend, 0.0f));
        CHECK(near(w_after[expr_index(Expression::Neutral)], w_before[expr_index(Expression::Neutral)]));
        CHECK(near(w_after[expr_index(Expression::Happy)], w_before[expr_index(Expression::Happy)]));
        CHECK(near(w_after[expr_index(Expression::Sleepy)], w_before[expr_index(Expression::Sleepy)]));
        CHECK(near(w_after[expr_index(Expression::Angry)], 0.0f));
        CHECK(near(mix_lhs(ctx), lhs_before));
    }

    // A third interrupt folds the two oldest components: the mix stays a
    // convex combination (weights sum to 1, none negative) and the visible
    // value stays within the range spanned by the involved poses.
    {
        ExpressionController c;
        DrawContext ctx;
        c.set_target(Expression::Happy);
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        c.set_target(Expression::Sleepy);
        c.apply(ctx, 100);
        c.apply(ctx, 200);
        c.set_target(Expression::Angry);
        c.apply(ctx, 200);
        c.apply(ctx, 300);
        const float lhs_before = mix_lhs(ctx);
        c.set_target(Expression::Sad);
        c.apply(ctx, 300);

        float w[kExprCount];
        mix_weights(ctx, w);
        float sum = 0.0f;
        for (std::size_t i = 0; i < kExprCount; ++i) {
            CHECK(w[i] >= -1.0e-5f);
            sum += w[i];
        }
        CHECK(near(sum, 1.0f));
        // The fold may drop the lightest decaying component, but the visible
        // value right after the interrupt must stay close to the pre-interrupt
        // mix: bounded by the folded weight, far from a full snap.
        CHECK(std::fabs(mix_lhs(ctx) - lhs_before) < 1.5f);
    }

    // The ease duration stays within 200–400 ms.
    {
        CHECK(ExpressionController::kDurationMs >= 200);
        CHECK(ExpressionController::kDurationMs <= 400);
    }

    // The extended expressions (6-12) mix through the same from/to path; Thinking's lhs (13)
    // is distinct from Neutral (11.3) once the ease completes.
    {
        ExpressionController c;
        c.set_target(Expression::Thinking);
        DrawContext ctx;
        c.apply(ctx, 0);
        CHECK(near(mix_lhs(ctx), 11.3f));
        c.apply(ctx, ExpressionController::kDurationMs);
        CHECK(near(mix_lhs(ctx), 13.0f));
        CHECK(ctx.expression == Expression::Thinking);
    }

    // --- three-source arbitration (issue #4) --------------------------------

    // Voice listening is the persistent base, not a one-shot.
    {
        ExpressionController c;
        c.set_voice_state(VoiceState::Listening);
        CHECK(c.resolve(0) == Expression::Listening);
        CHECK(c.resolve(8'000) == Expression::Listening);
    }

    // Overlay wins over listening, then falls back to listening.
    {
        ExpressionController c;
        c.set_voice_state(VoiceState::Listening);
        c.set_overlay(Expression::Happy, 3000);
        CHECK(c.resolve(0) == Expression::Happy);
        CHECK(c.resolve(2999) == Expression::Happy);
        CHECK(c.resolve(3000) == Expression::Listening);
    }

    // Execution wait (thinking, including tool calls) wins over LLM mood.
    {
        ExpressionController c;
        c.set_base(Expression::Happy);
        c.set_voice_state(VoiceState::Thinking);
        CHECK(c.resolve(0) == Expression::Thinking);
    }

    // LLM mood shows while speaking.
    {
        ExpressionController c;
        c.set_base(Expression::Happy);
        c.set_voice_state(VoiceState::Speaking);
        CHECK(c.resolve(0) == Expression::Happy);
    }

    // Speaking with no mood is Neutral (mouth carries the speech).
    {
        ExpressionController c;
        c.set_voice_state(VoiceState::Speaking);
        CHECK(c.resolve(0) == Expression::Neutral);
    }

    // Three sources at once: overlay > thinking > mood.
    {
        ExpressionController c;
        c.set_base(Expression::Curious);
        c.set_voice_state(VoiceState::Thinking);
        c.set_overlay(Expression::Happy, 2000);
        CHECK(c.resolve(0) == Expression::Happy);
        CHECK(c.resolve(2000) == Expression::Thinking);
        c.set_voice_state(VoiceState::Speaking);
        CHECK(c.resolve(2000) == Expression::Curious);
    }

    // Overlay hold 0 is sticky until cleared (stroke / dizzy).
    {
        ExpressionController c;
        c.set_voice_state(VoiceState::Listening);
        c.set_overlay(Expression::Dizzy, 0);
        CHECK(c.resolve(0) == Expression::Dizzy);
        CHECK(c.resolve(60'000) == Expression::Dizzy);
        c.clear_overlay();
        CHECK(c.resolve(60'000) == Expression::Listening);
    }

    // Idle decay: bored at 2 min, sleepy at 10 min.
    {
        ExpressionController c;
        CHECK(c.resolve(0) == Expression::Neutral);
        CHECK(c.resolve(ExpressionController::kBoredAfterMs - 1) == Expression::Neutral);
        CHECK(c.resolve(ExpressionController::kBoredAfterMs) == Expression::Bored);
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Sleepy);
    }

    // Touch / wake / message snaps sleepy back to the base.
    {
        ExpressionController c;
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Sleepy);
        c.note_activity();
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs + 10) == Expression::Neutral);
    }

    // Only idle decay means asleep: the same face from a mood or a gesture
    // leaves the robot awake.
    {
        ExpressionController c;
        c.resolve(ExpressionController::kBoredAfterMs);
        CHECK(!c.asleep());
        c.resolve(ExpressionController::kSleepyAfterMs);
        CHECK(c.asleep());
        c.note_activity();
        c.resolve(ExpressionController::kSleepyAfterMs + 10);
        CHECK(!c.asleep());
    }
    {
        ExpressionController c;
        c.set_base(Expression::Sleepy);
        CHECK(c.resolve(0) == Expression::Sleepy);
        CHECK(!c.asleep());
    }
    {
        ExpressionController c;
        c.set_overlay(Expression::Sleepy, 3000);
        CHECK(c.resolve(0) == Expression::Sleepy);
        CHECK(!c.asleep());
    }
    // A live voice session keeps it awake even past the sleep timeout.
    {
        ExpressionController c;
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Sleepy);
        CHECK(c.asleep());
        c.set_voice_state(VoiceState::Listening);
        c.resolve(ExpressionController::kSleepyAfterMs + 10);
        CHECK(!c.asleep());
    }

    // Voice listening wins over decay (a live session is not idle).
    {
        ExpressionController c;
        c.set_voice_state(VoiceState::Listening);
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Listening);
    }

    // Being ignored outranks a leftover mood: Happy drifts bored → sleepy,
    // and activity brings the mood back.
    {
        ExpressionController c;
        c.set_base(Expression::Happy);
        CHECK(c.resolve(0) == Expression::Happy);
        CHECK(c.resolve(ExpressionController::kBoredAfterMs) == Expression::Bored);
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Sleepy);
        c.note_activity();
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs + 10) == Expression::Happy);
    }

    // Listening still wins over a leftover LLM mood (wake face).
    {
        ExpressionController c;
        c.set_base(Expression::Happy);
        c.set_voice_state(VoiceState::Listening);
        CHECK(c.resolve(0) == Expression::Listening);
    }

    // --- Grok Bot activity --------------------------------------------------

    // Sleep follows the design: 10 min without input.
    CHECK(ExpressionController::kSleepyAfterMs == 10u * 60u * 1000u);

    // Activity outranks voice thinking and the mood; listening and overlays
    // outrank activity.
    {
        ExpressionController c;
        c.set_base(Expression::Happy);
        c.set_voice_state(VoiceState::Thinking);
        c.set_activity(Expression::Working, 0);
        CHECK(c.resolve(0) == Expression::Working);
        c.set_voice_state(VoiceState::Listening);
        CHECK(c.resolve(10) == Expression::Listening);
        c.set_voice_state(VoiceState::Speaking);
        c.set_overlay(Expression::Affection, 1000);
        CHECK(c.resolve(20) == Expression::Affection);
        CHECK(c.resolve(1020) == Expression::Working);
    }

    // A live run is not idle: activity outranks idle decay.
    {
        ExpressionController c;
        c.set_activity(Expression::Working, 20u * 60u * 1000u);
        CHECK(c.resolve(0) == Expression::Working);
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Working);
    }

    // Done plays its default clip + settle, then the face returns to rest.
    {
        ExpressionController c;
        CHECK(ExpressionController::default_activity_ttl_ms(Expression::Done) == 3'700u);
        c.set_activity(Expression::Done, 0);
        CHECK(c.resolve(1'000) == Expression::Done);
        CHECK(c.resolve(4'699) == Expression::Done);
        CHECK(c.resolve(4'700) == Expression::Neutral);
        CHECK(!c.activity().has_value());
    }

    // Keepalive: re-sending the state refreshes the TTL.
    {
        ExpressionController c;
        c.set_activity(Expression::Working, 1'000);
        CHECK(c.resolve(0) == Expression::Working);
        c.set_activity(Expression::Working, 1'000);
        CHECK(c.resolve(900) == Expression::Working);
        CHECK(c.resolve(1'899) == Expression::Working);
        CHECK(c.resolve(1'900) == Expression::Neutral);
    }

    // Unresolved Blocked gives up as Failed after 30 s, and a keepalive does
    // not restart that clock. Failed then expires on its own.
    {
        ExpressionController c;
        c.set_activity(Expression::Blocked, 0);
        CHECK(c.resolve(0) == Expression::Blocked);
        c.set_activity(Expression::Blocked, 0);
        CHECK(c.resolve(20'000) == Expression::Blocked);
        CHECK(c.resolve(ExpressionController::kBlockedToFailedMs - 1) == Expression::Blocked);
        CHECK(c.resolve(ExpressionController::kBlockedToFailedMs) == Expression::Failed);
        const std::uint32_t failed_end = ExpressionController::kBlockedToFailedMs +
                                         ExpressionController::default_activity_ttl_ms(Expression::Failed);
        CHECK(c.resolve(failed_end - 1) == Expression::Failed);
        CHECK(c.resolve(failed_end) == Expression::Neutral);
    }

    // A new state restarts the escalation clock: Blocked → Thinking (retry)
    // → Blocked again counts 30 s from the second Blocked.
    {
        ExpressionController c;
        c.set_activity(Expression::Blocked, 0);
        CHECK(c.resolve(0) == Expression::Blocked);
        c.set_activity(Expression::Thinking, 0);
        CHECK(c.resolve(25'000) == Expression::Thinking);
        c.set_activity(Expression::Blocked, 0);
        CHECK(c.resolve(26'000) == Expression::Blocked);
        CHECK(c.resolve(55'999) == Expression::Blocked);
        CHECK(c.resolve(56'000) == Expression::Failed);
    }

    // An unanswered question expires after 10 min straight into Sleepy:
    // expiry is not input, so the idle clock ran the whole time.
    {
        ExpressionController c;
        c.set_activity(Expression::Waiting, 0);
        CHECK(c.resolve(0) == Expression::Waiting);
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs - 1) == Expression::Waiting);
        CHECK(c.resolve(ExpressionController::kSleepyAfterMs) == Expression::Sleepy);
    }

    // Clearing the activity shows the mood again and counts as input.
    {
        ExpressionController c;
        c.set_base(Expression::Curious);
        c.set_activity(Expression::Working, 0);
        CHECK(c.resolve(0) == Expression::Working);
        c.clear_activity();
        CHECK(c.resolve(ExpressionController::kBoredAfterMs + 5) == Expression::Curious);
    }

    // The run states blend like any other expression.
    {
        ExpressionController c;
        DrawContext ctx;
        c.set_target(Expression::Done);
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs);
        CHECK(ctx.expression == Expression::Done);
        CHECK(near(mix_lhs(ctx), kGrokLhs[expr_index(Expression::Done)]));
    }

    return avtest::finish("expression_controller");
}
