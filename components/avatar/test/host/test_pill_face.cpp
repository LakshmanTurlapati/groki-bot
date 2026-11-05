// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
//
// Host tests for the pill face compositor (main/pill_face.hpp): keyframe
// values match the design mocks, transitions never snap, colours come from
// the palette, and every expression stays within the display-list budget.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/expression_controller.hpp"
#include "pill_face.hpp"
#include "test_support.hpp"

using stackchan::avatar::DrawContext;
using stackchan::avatar::Expression;
using stackchan::avatar::ExpressionController;
using stackchan::avatar::kExpressionCount;
namespace pill = stackchan::app::pill;
namespace d = stackchan::app::pill::detail;

namespace {

using Frame = std::array<float, pill::kOutFloats>;

bool near(float a, float b, float tol = 0.05f) {
    return std::fabs(a - b) <= tol;
}

struct Seg {
    float ax, ay, bx, by, r, col;
};

Seg seg(const Frame& f, std::size_t slot) {
    const float* s = f.data() + slot * pill::kFloatsPerSegment;
    return {s[0], s[1], s[2], s[3], s[4], s[5]};
}

// The colour float is RGB565 | end flags (pill::kFlatA / kFlatB).
std::uint32_t colour(const Seg& s) {
    return static_cast<std::uint32_t>(s.col) & 0xFFFFu;
}
std::uint32_t flags(const Seg& s) {
    return static_cast<std::uint32_t>(s.col) & ~0xFFFFu;
}

// A context fully settled on one expression (no blend in flight).
DrawContext settled(Expression e) {
    DrawContext ctx;
    ctx.expression = e;
    ctx.expression_from = e;
    ctx.expression_hold_to = e;
    ctx.expression_hold2_to = e;
    ctx.expression_blend = 1.0f;
    ctx.expression_hold_blend = 1.0f;
    ctx.expression_hold2_blend = 0.0f;
    return ctx;
}

// Common multiple of every loop period, so (kBase + t) samples each mock at
// its animation time t and the boot wake is long over.
constexpr std::uint32_t kBase = 1'260'000;

Frame render(pill::Compositor& c, const DrawContext& ctx, std::uint32_t now) {
    Frame f{};
    c.compose(ctx, now, f.data());
    return f;
}

// Frame for expression `e` at animation time `t` (since entry for one-shots).
Frame at(Expression e, std::uint32_t t) {
    pill::Compositor c;
    const DrawContext ctx = settled(e);
    Frame f = render(c, ctx, kBase);
    if (t != 0) {
        f = render(c, ctx, kBase + t);
    }
    return f;
}

float dist(float x0, float y0, float x1, float y1) {
    return std::hypot(x1 - x0, y1 - y0);
}

// Distance from point p to segment ab.
float point_seg(float px, float py, const Seg& s) {
    const float dx = s.bx - s.ax;
    const float dy = s.by - s.ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - s.ax) * dx + (py - s.ay) * dy) / len2 : 0.0f;
    t = std::fmax(0.0f, std::fmin(1.0f, t));
    return dist(px, py, s.ax + t * dx, s.ay + t * dy);
}

// Primitive count the DSL issues for one segment (see grok_face.avdsl).
int primitives(const Seg& s) {
    if (s.r <= 0.0f) {
        return 0;
    }
    const int cap_a = (flags(s) & pill::kFlatA) != 0 ? 0 : 1;
    const int cap_b = (flags(s) & pill::kFlatB) != 0 ? 0 : 1;
    if (dist(s.ax, s.ay, s.bx, s.by) < 0.5f) {
        return cap_a;
    }
    return 2 + cap_a + cap_b;
}

// Farthest point of segment `s` (as drawn) from the body's centre line,
// minus the body radius: <= 0 means fully inside the body capsule.
float outside_body(const Seg& s, const Seg& body) {
    const float len = dist(s.ax, s.ay, s.bx, s.by);
    float far = std::fmax(point_seg(s.ax, s.ay, body), point_seg(s.bx, s.by, body)) + s.r;
    if (len > 0.0f && (flags(s) & (pill::kFlatA | pill::kFlatB)) != 0) {
        // Rectangle corners reach r out sideways from each end.
        const float nx = -(s.by - s.ay) / len * s.r;
        const float ny = (s.bx - s.ax) / len * s.r;
        for (const float sign : {-1.0f, 1.0f}) {
            far = std::fmax(far, point_seg(s.ax + sign * nx, s.ay + sign * ny, body));
            far = std::fmax(far, point_seg(s.bx + sign * nx, s.by + sign * ny, body));
        }
    }
    return far - body.r;
}

} // namespace

int main() {
    // --- easing --------------------------------------------------------------
    CHECK(near(d::ease(d::kLinear, 0.3f), 0.3f, 1e-6f));
    CHECK(near(d::ease(d::kEaseInOut, 0.5f), 0.5f, 1e-3f));
    // CSS ease-in-out at x = 0.25 is ~0.1291; ease-out at x = 0.5 is ~0.6865.
    CHECK(near(d::ease(d::kEaseInOut, 0.25f), 0.1291f, 2e-3f));
    CHECK(near(d::ease(d::kEaseOut, 0.5f), 0.6865f, 2e-3f));
    CHECK(d::ease(d::kEaseOut, 0.0f) == 0.0f && d::ease(d::kEaseOut, 1.0f) == 1.0f);

    // --- Idle rest geometry (idle.html at 25 %, eyes open) --------------------
    {
        std::uint32_t t = 2500;
        while (d::blink(kBase + t, 2450, 450) < 1.0f) {
            t += 10;
        }
        CHECK(t < 2900); // the swell/glance keys are flat until 41 %
        const Frame f = at(Expression::Neutral, t);
        const Seg body = seg(f, pill::kSlotBody);
        CHECK(near(body.r, 67.5f));
        CHECK(near((body.ax + body.bx) * 0.5f, 160.0f) && near((body.ay + body.by) * 0.5f, 120.0f));
        CHECK(near(dist(body.ax, body.ay, body.bx, body.by), 62.0f)); // 197 - 135
        // -15 degrees: the right end is higher on screen.
        CHECK(near(std::atan2(body.by - body.ay, body.bx - body.ax) * 180.0f / d::kPi, -15.0f, 0.1f));
        // Eyes sit 33 px right / 19 px up of the body centre in the body
        // frame, at u = 7 and 59.5; each is a 20 x 47 pill at -26 degrees.
        const Seg l = seg(f, pill::kSlotEyes);
        CHECK(near(l.r, 10.0f));
        CHECK(near(dist(l.ax, l.ay, l.bx, l.by), 27.0f));
        const d::Vec lc = d::rotate_vec({7.0f, -19.0f}, -15.0f);
        CHECK(near((l.ax + l.bx) * 0.5f, 160.0f + lc.x, 0.1f) && near((l.ay + l.by) * 0.5f, 120.0f + lc.y, 0.1f));
        const Seg r = seg(f, pill::kSlotEyes + 2);
        const d::Vec rc = d::rotate_vec({59.5f, -19.0f}, -15.0f);
        CHECK(near((r.ax + r.bx) * 0.5f, 160.0f + rc.x, 0.1f) && near((r.ay + r.by) * 0.5f, 120.0f + rc.y, 0.1f));
        // Long axis tilted -26 degrees from vertical on screen.
        CHECK(near(std::atan2(l.bx - l.ax, l.by - l.ay) * 180.0f / d::kPi, 26.0f, 0.2f));
        // Colours: body ink, eyes are ground-colour holes, nothing else drawn.
        CHECK(body.col == static_cast<float>(stackchan::avatar::kDefaultPalette.primary));
        CHECK(l.col == static_cast<float>(stackchan::avatar::kDefaultPalette.background));
        for (std::size_t i = 0; i < pill::kSatellites; ++i) {
            CHECK(seg(f, pill::kSlotSatellites + i).r == 0.0f);
        }
        for (std::size_t i = pill::kSlotArchOuter; i < pill::kSegments; ++i) {
            CHECK(seg(f, i).r == 0.0f);
        }
    }

    // --- keyframe fidelity (pose values straight from the mocks) -------------
    {
        // Idle 48 %: first glance held, body drifted.
        const d::Pose p = d::eval_pose(Expression::Neutral, kBase + 4800, 0);
        CHECK(near(p[d::kBoxTx], -34.0f) && near(p[d::kBoxTy], -8.0f) && near(p[d::kBoxRot], 10.0f));
        CHECK(near(p[d::kPosTx], -14.0f) && near(p[d::kPosTy], -3.0f));
    }
    {
        // Working 0 % and 64 %: held gazes; eyes 32 x 62.
        d::Pose p = d::eval_pose(Expression::Working, kBase, 0);
        CHECK(near(p[d::kBoxTx], -45.0f) && near(p[d::kBoxTy], 4.0f) && near(p[d::kBoxRot], -12.0f));
        CHECK(near(p[d::kPosTx], -13.0f) && near(p[d::kPosTy], -2.0f));
        CHECK(near(p[d::eye(0, d::kEyeW)], 32.0f) && near(p[d::eye(0, d::kEyeH)], 62.0f));
        p = d::eval_pose(Expression::Working, kBase + 2560, 0);
        CHECK(near(p[d::kBoxTx], -54.0f) && near(p[d::kBoxTy], 30.0f) && near(p[d::kBoxRot], -30.0f));
    }
    {
        // Blocked 78 % since entry: pinched chevrons, shrunk body.
        const d::Pose p = d::eval_pose(Expression::Blocked, kBase, 1638);
        CHECK(near(p[d::kBoxTx], 17.0f) && near(p[d::kBoxTy], 6.0f));
        CHECK(near(p[d::kPosSx], 0.84f) && near(p[d::kPosTy], -4.0f));
        CHECK(near(p[d::eye(0, d::kEyeTx)], 12.5f) && near(p[d::eye(1, d::kEyeTx)], -12.5f));
        CHECK(near(p[d::eye(0, d::kEyeFold)], 32.0f) && near(p[d::eye(1, d::kEyeFold)], -32.0f));
    }
    {
        // Done: starts at the anticipation (35 %), first hop apex at 54 %.
        d::Pose p = d::eval_pose(Expression::Done, kBase, 0);
        CHECK(near(p[d::kPosTy], 0.0f) && near(p[d::kArch], 1.0f) && near(p[d::eye(0, d::kEyeW)], 0.0f));
        p = d::eval_pose(Expression::Done, kBase, 741);
        CHECK(near(p[d::kPosTy], -31.0f, 0.2f) && near(p[d::kPosSy], 1.05f, 0.01f));
        // Ends at rest, not back in the crouch.
        p = d::eval_pose(Expression::Done, kBase, 10'000);
        CHECK(near(p[d::kPosTy], 0.0f) && near(p[d::kPosSx], 1.0f) && near(p[d::kPosSy], 1.0f));
    }
    {
        // Failed: sunk and splayed, then recovered after 2.8 s.
        d::Pose p = d::eval_pose(Expression::Failed, kBase, 1000);
        CHECK(near(p[d::kPosTy], 22.0f) && near(p[d::eye(0, d::kEyeSpin)], 39.0f) &&
              near(p[d::eye(1, d::kEyeSpin)], -17.0f) && near(p[d::eye(0, d::kEyeSx)], 0.8f));
        p = d::eval_pose(Expression::Failed, kBase, 2800);
        CHECK(near(p[d::kPosTy], 0.0f) && near(p[d::eye(0, d::kEyeSpin)], 0.0f));
    }
    {
        // Thinking: right dot peaks at 0.2 s (46 % of its phase).
        const Frame f = at(Expression::Thinking, 200);
        const Seg right = seg(f, pill::kSlotSatellites + 1);
        CHECK(near(right.ax, 234.0f) && near(right.ay, 109.5f) && near(right.r, 28.0f));
        CHECK(right.col == static_cast<float>(stackchan::avatar::kDefaultPalette.primary));
        // Eyes are gone; the body is the 56 x 38 mini pill.
        CHECK(seg(f, pill::kSlotEyes).r == 0.0f);
        const Seg body = seg(f, pill::kSlotBody);
        CHECK(body.r < 19.5f);
    }
    {
        // Waiting: particle 0 at the top of the tilted orbit, full size.
        const Frame f = at(Expression::Waiting, 0);
        const Seg p0 = seg(f, pill::kSlotSatellites);
        const d::Vec v = d::rotate_vec({0.0f, -20.8f}, -15.0f);
        CHECK(near(p0.ax, 160.0f + v.x) && near(p0.ay, 120.0f + v.y) && near(p0.r, 12.0f));
        // Back of the orbit: small and dim (clamped to 18 % opacity).
        const d::Pose p = d::eval_pose(Expression::Waiting, kBase + 1875, 0);
        CHECK(near(p[d::sat(0, d::kSatD)], 12.0f) && near(p[d::sat(0, d::kSatA)], 0.18f));
    }
    {
        // Listening 0 %: left blob at body + (50, -16), right foreshortened.
        const d::Pose p = d::eval_pose(Expression::Listening, kBase, 0);
        CHECK(near(p[d::kBoxX] + p[d::eye(0, d::kEyeOx)], 50.0f) &&
              near(p[d::kBoxY] + p[d::eye(0, d::kEyeOy)], -16.0f));
        CHECK(near(p[d::eye(1, d::kEyeSx)], 0.55f));
    }

    // --- Done arches: inner cut stays inside the body ------------------------
    for (const Expression e : {Expression::Done, Expression::Happy, Expression::Excited, Expression::Affection}) {
        for (std::uint32_t t = 0; t < 4000; t += 50) {
            const Frame f = at(e, t);
            const Seg body = seg(f, pill::kSlotBody);
            for (std::size_t s = 0; s < 2; ++s) {
                const Seg outer = seg(f, pill::kSlotArchOuter + s);
                const Seg inner = seg(f, pill::kSlotArchInner + s);
                const Seg trim = seg(f, pill::kSlotArchTrim + s);
                CHECK(flags(outer) == pill::kFlatB && flags(inner) == pill::kFlatB);
                CHECK(flags(trim) == (pill::kFlatA | pill::kFlatB));
                CHECK(colour(outer) == stackchan::avatar::kDefaultPalette.background);
                CHECK(colour(inner) == colour(body) && colour(trim) == colour(body));
                // Body-colour pieces painted outside the body would show.
                CHECK(outside_body(inner, body) <= 0.5f);
                if (outside_body(trim, body) > 0.5f) {
                    std::fprintf(stderr, "arch trim outside body: expr %u t %u by %.2f px\n",
                                 static_cast<unsigned>(e), static_cast<unsigned>(t),
                                 static_cast<double>(outside_body(trim, body)));
                }
                CHECK(outside_body(trim, body) <= 0.5f);
                // The trim covers the round top's overshoot below the flat
                // bottom: it starts at the arch's bottom and is wider.
                CHECK(near(trim.ax, outer.bx, 1e-3f) && near(trim.ay, outer.by, 1e-3f));
                CHECK(trim.r >= 0.86f * outer.r); // >= half the 38.7 px overshoot chord
            }
        }
    }

    // --- palette and speech ----------------------------------------------------
    {
        pill::Compositor c;
        DrawContext ctx = settled(Expression::Neutral);
        ctx.palette.primary = 0xF800;
        ctx.palette.background = 0x001F;
        Frame f = render(c, ctx, kBase + 2500);
        CHECK(seg(f, pill::kSlotBody).col == static_cast<float>(0xF800));
        CHECK(seg(f, pill::kSlotEyes).col == static_cast<float>(0x001F));
        const float rest_r = seg(f, pill::kSlotBody).r;
        ctx.mouth_open_ratio = 1.0f;
        f = render(c, ctx, kBase + 2500);
        CHECK(near(seg(f, pill::kSlotBody).r, rest_r * 0.86f, 0.1f));
    }
    {
        // Half-transparent Thinking dot = channel mix of ink and ground.
        CHECK(d::mix565(0x0000, 0xFFFF, 0.5f) == static_cast<std::uint16_t>((16u << 11) | (32u << 5) | 16u));
        CHECK(d::mix565(0x73AE, 0x1082, 0.0f) == 0x73AE && d::mix565(0x73AE, 0x1082, 1.0f) == 0x1082);
    }

    // --- boot wake: eyes closed, then open ------------------------------------
    {
        pill::Compositor c;
        const DrawContext ctx = settled(Expression::Neutral);
        const Seg closed = seg(render(c, ctx, 200), pill::kSlotEyes);
        const Seg open = seg(render(c, ctx, 2000), pill::kSlotEyes);
        CHECK(dist(closed.ax, closed.ay, closed.bx, closed.by) + 2.0f * closed.r <
              0.5f * (dist(open.ax, open.ay, open.bx, open.by) + 2.0f * open.r));
    }

    // --- no snaps: drive the real controller through state changes ------------
    {
        struct Step {
            std::uint32_t at;
            Expression e;
        };
        const Step script[] = {
            {1500, Expression::Neutral},   {3000, Expression::Listening}, {4500, Expression::Thinking},
            {5200, Expression::Working},   {9000, Expression::Done},      {10'000, Expression::Blocked},
            {11'000, Expression::Thinking}, {11'150, Expression::Waiting}, {14'000, Expression::Failed},
            {15'000, Expression::Happy},   {16'500, Expression::Affection}, {18'000, Expression::Sleepy},
            {20'000, Expression::Dizzy},   {21'500, Expression::Surprised}, {22'500, Expression::Angry},
            {24'000, Expression::Sad},     {25'500, Expression::Excited},  {27'000, Expression::Curious},
            {28'000, Expression::Doubt},   {29'000, Expression::Confused}, {30'000, Expression::Bored},
            {31'500, Expression::Neutral},
        };
        ExpressionController ctrl;
        pill::Compositor comp;
        DrawContext ctx;
        Frame prev{};
        bool have_prev = false;
        float worst = 0.0f;
        std::size_t next = 0;
        for (std::uint32_t now = 1200; now < 33'000; now += 16) {
            while (next < std::size(script) && now >= script[next].at) {
                ctrl.set_target(script[next].e);
                ++next;
            }
            ctrl.apply(ctx, now);
            ctx.now_ms = now;
            Frame f{};
            comp.compose(ctx, now, f.data());
            if (have_prev) {
                // Shapes (not fx, which restart while invisible).
                for (std::size_t i = 0; i < pill::kSlotZzz; ++i) {
                    const Seg a = seg(prev, i);
                    const Seg b = seg(f, i);
                    if (a.r == 0.0f || b.r == 0.0f) {
                        continue;
                    }
                    const float jump = std::fmax(dist(a.ax, a.ay, b.ax, b.ay), dist(a.bx, a.by, b.bx, b.by));
                    worst = std::fmax(worst, std::fmax(jump, std::fabs(std::fabs(a.r) - std::fabs(b.r))));
                }
            }
            prev = f;
            have_prev = true;
        }
        if (worst > 12.0f) {
            std::fprintf(stderr, "largest per-frame jump %.2f px\n", static_cast<double>(worst));
        }
        CHECK(worst <= 12.0f);
    }

    // --- budget: every expression, 10 s, 50 ms steps --------------------------
    {
        int worst = 0;
        for (std::size_t e = 0; e < kExpressionCount; ++e) {
            pill::Compositor c;
            const DrawContext ctx = settled(static_cast<Expression>(e));
            for (std::uint32_t t = 0; t < 10'000; t += 50) {
                const Frame f = render(c, ctx, kBase + t);
                int prims = 1; // background fill
                for (std::size_t i = 0; i < pill::kSegments; ++i) {
                    const Seg s = seg(f, i);
                    CHECK(std::isfinite(s.ax) && std::isfinite(s.ay) && std::isfinite(s.bx) &&
                          std::isfinite(s.by) && std::isfinite(s.r));
                    prims += primitives(s);
                }
                worst = std::max(worst, prims);
            }
        }
        std::printf("pill face: at most %d primitives per frame\n", worst);
        CHECK(worst <= 80);
    }

    // --- an out-of-range expression byte does not crash ----------------------
    {
        pill::Compositor c;
        DrawContext ctx = settled(static_cast<Expression>(200));
        const Frame f = render(c, ctx, kBase);
        CHECK(seg(f, pill::kSlotBody).r > 0.0f);
    }

    return avtest::finish("pill_face");
}
