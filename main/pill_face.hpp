// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
//
// Pill face compositor. Turns DrawContext (expression mix, time, mouth,
// commanded gaze, palette) into a display list of capsule segments that
// assets/grok_face.avdsl rasterizes through the face-parameter window
// (Var::ParamBase). The DSL only draws; all geometry and timing live here.
//
// Geometry follows the Grok Bot face design (docs/design/grokbot-faces/):
// a 197 x 135 capsule body rotated -15 degrees, eyes as capsule "holes" in
// the ground colour, no mouth (speech squashes the body). Each state's CSS
// @keyframes are transcribed into keyframe tracks below and replayed through
// the same transform chain the mocks use:
//   .pos translate/scale -> body rotate(-15) -> eye container translate/
//   rotate -> eye rotate(-11) -> eye translate/rotate/scale -> blink.
//
// Coordinates are in the 320 x 240 design space; the DSL maps them to the
// real canvas with tx()/ty()/sz().
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"

namespace stackchan::app::pill {

// --- display list ------------------------------------------------------------
// One segment = ax, ay, bx, by, r, colour. A segment is a capsule from A to B
// with radius r: circle at A, quad of width 2r, circle at B. r == 0 means
// "skip". The colour float carries RGB565 in its low 16 bits and end flags
// above them (kFlatA / kFlatB drop that end's circle; both = a rectangle).
// Slots are drawn in index order.
inline constexpr std::size_t kFloatsPerSegment = 6;
inline constexpr std::uint32_t kFlatA = 1u << 16;
inline constexpr std::uint32_t kFlatB = 2u << 16;
inline constexpr std::size_t kSatellites = 5;
inline constexpr std::size_t kSlotSatellites = 0; // Thinking dots, Waiting orbit, sparkles
inline constexpr std::size_t kSlotBody = 5;
inline constexpr std::size_t kSlotEyes = 6;        // L upper, L lower, R upper, R lower
inline constexpr std::size_t kSlotArchOuter = 10;  // L, R: ground, round top, flat bottom
inline constexpr std::size_t kSlotArchInner = 12;  // L, R: body-colour cut-out
inline constexpr std::size_t kSlotArchTrim = 14;   // L, R: body-colour strip under the flat bottom
inline constexpr std::size_t kSlotZzz = 16;        // 2 glyphs x 3 strokes
inline constexpr std::size_t kSlotBlush = 22;      // L, R
inline constexpr std::size_t kSlotHeart = 24;      // 2 lobes + 2 strokes
inline constexpr std::size_t kSegments = 28;
inline constexpr std::size_t kOutFloats = kSegments * kFloatsPerSegment;

namespace detail {

inline constexpr float kPi = 3.14159265f;

// --- pose: every animated parameter, blended linearly between expressions ---
enum PoseField : std::size_t {
    kBodyX,     // body centre, design space
    kBodyY,
    kBodyW,     // body capsule size before scaling
    kBodyH,
    kBodyRot,   // degrees
    kBodyAlpha, // body opacity over the ground (Thinking pulse)
    kPosTx,     // .pos translate (screen axes)
    kPosTy,
    kPosSx,     // .pos scale (screen axes, about the origin below)
    kPosSy,
    kPosOx,     // .pos transform origin, relative to the body centre
    kPosOy,
    kBoxX,      // eye container centre in the body frame
    kBoxY,
    kBoxTx,     // eye container animation
    kBoxTy,
    kBoxRot,
    kBoxSy,     // container scaleY (Blocked half-shut blink)
    kArch,      // 0 = pill eyes, 1 = Done arches
    kEyes,
};

enum EyeField : std::size_t {
    kEyeOx,    // eye centre relative to the container centre
    kEyeOy,
    kEyeRot,   // eye wrapper rotation (rest -11)
    kEyeW,     // eye capsule size
    kEyeH,
    kEyeSx,    // per-eye scale (Listening foreshortening, Failed shrink)
    kEyeSy,
    kEyeTx,    // translate in the eye's own frame (Blocked pinch, Failed droop)
    kEyeTy,
    kEyeSpin,  // rotation after that translate (Failed splay)
    kEyeSplit, // 0 = both halves are the whole eye, 1 = Blocked halves
    kEyeFold,  // half fold angle; upper half gets -fold, lower +fold
    kEyeBlink, // scaleY multiplier
    kEyeFields,
};

enum SatField : std::size_t { kSatX, kSatY, kSatD, kSatA, kSatFields };

inline constexpr std::size_t kSats = kEyes + 2 * kEyeFields;
inline constexpr std::size_t kPoseFloats = kSats + kSatellites * kSatFields;

using Pose = std::array<float, kPoseFloats>;

constexpr std::size_t eye(std::size_t side, std::size_t field) {
    return kEyes + side * kEyeFields + field;
}
constexpr std::size_t sat(std::size_t i, std::size_t field) {
    return kSats + i * kSatFields + field;
}

// Idle rest pose (idle.html at t = 25 %).
inline Pose rest_pose() {
    Pose p{};
    p[kBodyX] = 160.0f;
    p[kBodyY] = 120.0f;
    p[kBodyW] = 197.0f;
    p[kBodyH] = 135.0f;
    p[kBodyRot] = -15.0f;
    p[kBodyAlpha] = 1.0f;
    p[kPosSx] = 1.0f;
    p[kPosSy] = 1.0f;
    p[kBoxX] = 33.0f;
    p[kBoxY] = -19.0f;
    p[kBoxSy] = 1.0f;
    for (std::size_t s = 0; s < 2; ++s) {
        p[eye(s, kEyeOx)] = s == 0 ? -26.0f : 26.5f;
        p[eye(s, kEyeRot)] = -11.0f;
        p[eye(s, kEyeW)] = 20.0f;
        p[eye(s, kEyeH)] = 47.0f;
        p[eye(s, kEyeSx)] = 1.0f;
        p[eye(s, kEyeSy)] = 1.0f;
        p[eye(s, kEyeBlink)] = 1.0f;
    }
    // Unused satellites sit invisible at the body centre so they grow out of
    // it when a state needs them (Idle -> Thinking).
    for (std::size_t i = 0; i < kSatellites; ++i) {
        p[sat(i, kSatX)] = 160.0f;
        p[sat(i, kSatY)] = 120.0f;
        p[sat(i, kSatA)] = 1.0f;
    }
    return p;
}

inline void hide_eyes(Pose& p) {
    for (std::size_t s = 0; s < 2; ++s) {
        p[eye(s, kEyeW)] = 0.0f;
        p[eye(s, kEyeH)] = 0.0f;
    }
}

inline void set_eye_size(Pose& p, float w, float h, float rot) {
    for (std::size_t s = 0; s < 2; ++s) {
        p[eye(s, kEyeW)] = w;
        p[eye(s, kEyeH)] = h;
        p[eye(s, kEyeRot)] = rot;
    }
}

// --- timing ----------------------------------------------------------------
inline std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// Loop phase in percent. Integer modulo first: a float now_ms loses
// millisecond precision after a few hours of uptime.
inline float loop_pct(std::uint32_t t_ms, std::uint32_t period_ms, std::uint32_t offset_ms = 0) {
    return static_cast<float>((t_ms + offset_ms) % period_ms) * 100.0f / static_cast<float>(period_ms);
}

inline float loop_unit(std::uint32_t t_ms, std::uint32_t period_ms, std::uint32_t offset_ms = 0) {
    return static_cast<float>((t_ms + offset_ms) % period_ms) / static_cast<float>(period_ms);
}

// CSS cubic-bezier timing function, applied per keyframe interval.
struct Ease {
    float x1, y1, x2, y2;
};
inline constexpr Ease kLinear{0.0f, 0.0f, 1.0f, 1.0f};
inline constexpr Ease kEaseInOut{0.42f, 0.0f, 0.58f, 1.0f};
inline constexpr Ease kEaseOut{0.0f, 0.0f, 0.58f, 1.0f};

inline float bezier(float t, float p1, float p2) {
    const float u = 1.0f - t;
    return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
}

inline float bezier_slope(float t, float p1, float p2) {
    const float u = 1.0f - t;
    return 3.0f * u * u * p1 + 6.0f * u * t * (p2 - p1) + 3.0f * t * t * (1.0f - p2);
}

inline float ease(const Ease& e, float x) {
    if (x <= 0.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return 1.0f;
    }
    if (e.x1 == e.y1 && e.x2 == e.y2) {
        return x;
    }
    // Solve X(t) = x: Newton from t = x, bisection if it stalls.
    float t = x;
    for (int i = 0; i < 6; ++i) {
        const float err = bezier(t, e.x1, e.x2) - x;
        if (std::fabs(err) < 1.0e-4f) {
            return bezier(t, e.y1, e.y2);
        }
        const float slope = bezier_slope(t, e.x1, e.x2);
        if (std::fabs(slope) < 1.0e-5f) {
            break;
        }
        t = std::clamp(t - err / slope, 0.0f, 1.0f);
    }
    float lo = 0.0f;
    float hi = 1.0f;
    t = x;
    for (int i = 0; i < 16; ++i) {
        const float v = bezier(t, e.x1, e.x2);
        if (std::fabs(v - x) < 1.0e-4f) {
            break;
        }
        (v < x ? lo : hi) = t;
        t = 0.5f * (lo + hi);
    }
    return bezier(t, e.y1, e.y2);
}

inline float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Key {
    float pct;
    float v;
};

template <std::size_t N>
inline float sample(const Key (&k)[N], float pct, const Ease& e) {
    if (pct <= k[0].pct) {
        return k[0].v;
    }
    for (std::size_t i = 1; i < N; ++i) {
        if (pct <= k[i].pct) {
            const float a = k[i - 1].v;
            const float b = k[i].v;
            if (a == b) {
                return a;
            }
            const float span = k[i].pct - k[i - 1].pct;
            const float t = span > 0.0f ? (pct - k[i - 1].pct) / span : 1.0f;
            return a + (b - a) * ease(e, t);
        }
    }
    return k[N - 1].v;
}

// Design blink: close 100 ms to 25 % height, hold 75 ms, open 100 ms. One
// blink per window, started at a hashed offset in [0, jitter) so intervals
// vary (2.0 - 2.9 s for the idle window). `phase_ms` moves the window start
// so a fixed-period blink lands where the mock has it.
inline float blink(std::uint32_t now, std::uint32_t window_ms, std::uint32_t jitter_ms, std::uint32_t phase_ms = 0) {
    if (phase_ms % window_ms != 0) {
        now += window_ms - phase_ms % window_ms;
    }
    const std::uint32_t k = now / window_ms;
    const std::uint32_t off = jitter_ms != 0 ? hash_u32(k) % jitter_ms : 0;
    const std::uint32_t in = now - k * window_ms;
    if (in < off) {
        return 1.0f;
    }
    const auto t = static_cast<float>(in - off);
    if (t < 100.0f) {
        return 1.0f - 0.75f * t / 100.0f;
    }
    if (t < 175.0f) {
        return 0.25f;
    }
    if (t < 275.0f) {
        return 0.25f + 0.75f * (t - 175.0f) / 100.0f;
    }
    return 1.0f;
}

inline void set_blink(Pose& p, float b) {
    p[eye(0, kEyeBlink)] = b;
    p[eye(1, kEyeBlink)] = b;
}

// --- Idle (idle.html, 10 s): swell, then four glances ------------------------
inline constexpr Key kIdlePosX[] = {{0, 0},   {41, 0},  {44, -14}, {52, -14}, {55, -8},
                                    {62, -8}, {65, -7}, {70, -7},  {73, 0},   {100, 0}};
inline constexpr Key kIdlePosY[] = {{0, 0},   {41, 0},  {44, -3}, {52, -3}, {55, 15},
                                    {62, 15}, {65, 16}, {70, 16}, {73, 0},  {100, 0}};
inline constexpr Key kIdlePosS[] = {{0, 1.053f}, {6, 1.067f}, {16, 1.067f}, {25, 1}, {96, 1}, {100, 1.053f}};
inline constexpr Key kIdleBoxX[] = {{0, 0},   {41, 0},  {44, -34}, {52, -34}, {55, 19}, {62, 19},
                                    {65, 7},  {70, 7},  {73, 6},   {85, 6},   {90, 0},  {100, 0}};
inline constexpr Key kIdleBoxY[] = {{0, 0},   {41, 0},  {44, -8}, {52, -8}, {55, 10}, {62, 10},
                                    {65, 30}, {70, 30}, {73, -4}, {85, -4}, {90, 0},  {100, 0}};
inline constexpr Key kIdleBoxR[] = {{0, 0},   {41, 0},  {44, 10}, {52, 10},  {55, -10}, {62, -10},
                                    {65, -4}, {70, -4}, {73, -14}, {85, -14}, {90, 0},  {100, 0}};

inline void pose_idle(Pose& p, std::uint32_t now, std::uint32_t period_ms = 10'000, bool swell = true) {
    const float c = loop_pct(now, period_ms);
    p[kPosTx] = sample(kIdlePosX, c, kEaseInOut);
    p[kPosTy] = sample(kIdlePosY, c, kEaseInOut);
    if (swell) {
        p[kPosSx] = p[kPosSy] = sample(kIdlePosS, c, kEaseInOut);
    }
    p[kBoxTx] = sample(kIdleBoxX, c, kEaseOut);
    p[kBoxTy] = sample(kIdleBoxY, c, kEaseOut);
    p[kBoxRot] = sample(kIdleBoxR, c, kEaseOut);
    set_blink(p, blink(now, 2450, 450));
}

// --- Thinking (thinking.html, 1.4 s): body collapses into three dots --------
inline constexpr Key kPulseY[] = {{0, 0}, {46, -10.5f}, {100, 0}};
inline constexpr Key kPulseS[] = {{0, 0.8f}, {46, 1}, {100, 0.8f}};
inline constexpr Key kPulseA[] = {{0, 0.5f}, {46, 1}, {100, 0.5f}};
// animation-delay as a positive phase offset: right -0.444 s, left +0.156 s,
// middle +0.556 s. Peaks run right, left (+0.6 s), middle (+0.4 s).
inline constexpr std::uint32_t kThinkRightMs = 444;
inline constexpr std::uint32_t kThinkLeftMs = 1400 - 156;
inline constexpr std::uint32_t kThinkMidMs = 1400 - 556;

inline void pose_thinking(Pose& p, std::uint32_t now) {
    hide_eyes(p);
    const float m = loop_pct(now, 1400, kThinkMidMs);
    p[kBodyW] = 56.0f;
    p[kBodyH] = 38.0f;
    p[kPosTy] = sample(kPulseY, m, kEaseInOut);
    p[kPosSx] = p[kPosSy] = sample(kPulseS, m, kEaseInOut);
    p[kBodyAlpha] = sample(kPulseA, m, kEaseInOut);
    const std::uint32_t offsets[2] = {kThinkLeftMs, kThinkRightMs};
    const float xs[2] = {86.0f, 234.0f};
    for (std::size_t i = 0; i < 2; ++i) {
        const float c = loop_pct(now, 1400, offsets[i]);
        p[sat(i, kSatX)] = xs[i];
        p[sat(i, kSatY)] = 120.0f + sample(kPulseY, c, kEaseInOut);
        p[sat(i, kSatD)] = 56.0f * sample(kPulseS, c, kEaseInOut);
        p[sat(i, kSatA)] = sample(kPulseA, c, kEaseInOut);
    }
}

// --- Working (working.html, 4 s): big eyes roam the pill --------------------
inline constexpr Key kWorkPosX[] = {{0, -13}, {10, -13}, {18, 0},  {30, 0},   {38, 5},  {50, 5},
                                    {58, -7}, {70, -7},  {78, -12}, {90, -12}, {100, -13}};
inline constexpr Key kWorkPosY[] = {{0, -2}, {10, -2}, {18, 0}, {70, 0}, {78, -2}, {100, -2}};
inline constexpr Key kWorkBoxX[] = {{0, -45},  {10, -45}, {18, -36},  {30, -36},  {38, -22}, {50, -22},
                                    {58, -54}, {70, -54}, {78, -12},  {90, -12},  {100, -45}};
inline constexpr Key kWorkBoxY[] = {{0, 4},   {30, 4},  {38, 7},  {50, 7}, {58, 30},
                                    {70, 30}, {78, 32}, {90, 32}, {100, 4}};
inline constexpr Key kWorkBoxR[] = {{0, -12},  {10, -12}, {18, 6},   {30, 6},   {38, 16}, {50, 16},
                                    {58, -30}, {70, -30}, {78, -24}, {90, -24}, {100, -12}};

inline void pose_working(Pose& p, std::uint32_t now) {
    const float c = loop_pct(now, 4000);
    p[kPosTx] = sample(kWorkPosX, c, kEaseInOut);
    p[kPosTy] = sample(kWorkPosY, c, kEaseInOut);
    p[kBoxTx] = sample(kWorkBoxX, c, kEaseOut);
    p[kBoxTy] = sample(kWorkBoxY, c, kEaseOut);
    p[kBoxRot] = sample(kWorkBoxR, c, kEaseOut);
    set_eye_size(p, 32.0f, 62.0f, -11.0f);
    // working.html blinks at 88 % of its 3.6 s cycle.
    set_blink(p, blink(now, 3600, 0, 3168));
}

// --- Waiting (waiting.html, 3.75 s): mini body, five orbiting particles -----
inline void orbit(Pose& p, std::size_t i, float theta, float cx, float cy, float rx, float ry, float tilt_deg,
                  float max_d) {
    const float lx = rx * std::sin(theta);
    const float ly = -ry * std::cos(theta);
    const float t = tilt_deg * kPi / 180.0f;
    const float c = std::cos(theta);
    p[sat(i, kSatX)] = cx + lx * std::cos(t) - ly * std::sin(t);
    p[sat(i, kSatY)] = cy + lx * std::sin(t) + ly * std::cos(t);
    // Size 6 + 6 max(0, cos) radius, opacity clamp(0.59 + 1.64 cos, 0.18, 1).
    p[sat(i, kSatD)] = max_d * (0.5f + 0.5f * std::max(0.0f, c));
    p[sat(i, kSatA)] = std::clamp(0.59f + 1.64f * c, 0.18f, 1.0f);
}

inline void pose_waiting(Pose& p, std::uint32_t now) {
    hide_eyes(p);
    p[kBodyW] = 36.0f;
    p[kBodyH] = 25.0f;
    for (std::size_t i = 0; i < kSatellites; ++i) {
        const float u = loop_unit(now, 3750, static_cast<std::uint32_t>(750 * i));
        orbit(p, i, 2.0f * kPi * u, 160.0f, 120.0f, 49.7f, 20.8f, -15.0f, 24.0f);
    }
}

// --- Blocked (blocked.html, 2.1 s loop from entry) ---------------------------
inline constexpr Key kBlockPosX[] = {{0, 0},   {21, 0},  {31, 2}, {38, 2}, {50, -11}, {57, 2},
                                     {64, -10}, {69, -4}, {74, 0}, {81, 0}, {97, 0},   {100, 0}};
inline constexpr Key kBlockPosY[] = {{0, 0}, {21, 0}, {31, -3}, {38, -3}, {50, -4}, {81, -4}, {97, 0}, {100, 0}};
inline constexpr Key kBlockPosS[] = {{0, 1}, {38, 1}, {50, 0.84f}, {81, 0.84f}, {97, 1}, {100, 1}};
inline constexpr Key kBlockBoxX[] = {{0, 0},   {21, 0},  {31, 24}, {38, 24}, {43, 4}, {50, -16}, {57, 17},
                                     {64, -11}, {76, 17}, {81, 17}, {93, 4},  {97, 0}, {100, 0}};
inline constexpr Key kBlockBoxY[] = {{0, 0}, {21, 0}, {31, 2}, {38, 2}, {43, 4},
                                     {50, 6}, {81, 6}, {93, 2}, {97, 0}, {100, 0}};
inline constexpr Key kBlockBoxSy[] = {{0, 1},  {38, 1},  {43, 0.65f}, {50, 1},
                                      {81, 1}, {93, 0.75f}, {97, 1},  {100, 1}};
inline constexpr Key kBlockPinch[] = {{0, 0}, {38, 0}, {50, 12.5f}, {81, 12.5f}, {97, 0}, {100, 0}};
inline constexpr Key kBlockFold[] = {{0, 0}, {38, 0}, {50, 32}, {81, 32}, {97, 0}, {100, 0}};

inline void pose_blocked(Pose& p, std::uint32_t since) {
    const float c = loop_pct(since, 2100);
    p[kPosTx] = sample(kBlockPosX, c, kEaseInOut);
    p[kPosTy] = sample(kBlockPosY, c, kEaseInOut);
    p[kPosSx] = p[kPosSy] = sample(kBlockPosS, c, kEaseInOut);
    p[kBoxTx] = sample(kBlockBoxX, c, kEaseInOut);
    p[kBoxTy] = sample(kBlockBoxY, c, kEaseInOut);
    p[kBoxSy] = sample(kBlockBoxSy, c, kEaseInOut);
    const float pinch = sample(kBlockPinch, c, kEaseInOut);
    const float fold = sample(kBlockFold, c, kEaseInOut);
    p[eye(0, kEyeTx)] = pinch;
    p[eye(1, kEyeTx)] = -pinch;
    p[eye(0, kEyeFold)] = fold;
    p[eye(1, kEyeFold)] = -fold;
    p[eye(0, kEyeSplit)] = 1.0f;
    p[eye(1, kEyeSplit)] = 1.0f;
}

// --- Done (done.html): play once from the anticipation, then rest ------------
// The mock loops a 3.9 s clip; the robot starts at 35 % (anticipation) and
// ends on a rest key instead of looping back into the crouch.
inline constexpr std::uint32_t kDoneClipMs = 3900;
inline constexpr float kDoneStartPct = 35.0f;
inline constexpr Key kDoneTy[] = {{35, 0},   {46, 4},   {50, -27}, {54, -31}, {58, -13}, {62, -3},
                                  {65, -23}, {69, -31}, {73, -21}, {77, -23}, {81, -26}, {85, -5},
                                  {88, -11}, {92, -6},  {96, 0},   {100, 0}};
inline constexpr Key kDoneSx[] = {{35, 1}, {46, 1.067f}, {50, 1}, {58, 1}, {62, 1.04f}, {65, 1.056f},
                                  {69, 1}, {92, 1},      {96, 1.04f}, {100, 1}};
inline constexpr Key kDoneSy[] = {{35, 1},     {46, 0.965f}, {50, 1.1f},  {54, 1.05f}, {58, 1},
                                  {62, 0.98f}, {65, 0.99f},  {69, 1.05f}, {73, 1.02f}, {77, 1.03f},
                                  {81, 1.04f}, {85, 1.04f},  {88, 1},     {92, 1},     {96, 0.97f}, {100, 1}};
inline constexpr Key kDoneBoxX[] = {{35, 0},  {46, 0},  {50, -6}, {58, -6}, {63, 0},
                                    {69, 16}, {81, 16}, {88, 6},  {96, 0},  {100, 0}};
inline constexpr Key kDoneBoxY[] = {{35, 0},   {46, 0},   {50, -16}, {58, -16}, {63, -14},
                                    {69, -16}, {81, -16}, {88, -6},  {96, 0},   {100, 0}};
inline constexpr Key kDoneBoxR[] = {{35, 0},  {46, 0},  {50, -12}, {58, -12}, {63, 0},
                                    {69, 18}, {81, 18}, {88, 6},   {96, 0},   {100, 0}};

inline void arches(Pose& p) {
    hide_eyes(p);
    p[kArch] = 1.0f;
    p[kPosOy] = 67.5f; // done.html squashes about the bottom edge
}

inline void pose_done(Pose& p, std::uint32_t since) {
    arches(p);
    const float c = std::min(100.0f, kDoneStartPct + static_cast<float>(std::min<std::uint32_t>(since, kDoneClipMs)) *
                                                         100.0f / static_cast<float>(kDoneClipMs));
    p[kPosTy] = sample(kDoneTy, c, kEaseInOut);
    p[kPosSx] = sample(kDoneSx, c, kEaseInOut);
    p[kPosSy] = sample(kDoneSy, c, kEaseInOut);
    p[kBoxTx] = sample(kDoneBoxX, c, kEaseInOut);
    p[kBoxTy] = sample(kDoneBoxY, c, kEaseInOut);
    p[kBoxRot] = sample(kDoneBoxR, c, kEaseInOut);
}

// --- Failed (failed.html): sink and splay for 2.5 s, recover in 0.3 s --------
inline constexpr std::uint32_t kFailHoldMs = 2500;
inline constexpr std::uint32_t kFailRecoverMs = 300;

inline void pose_failed(Pose& p, std::uint32_t since) {
    float k = 1.0f;
    if (since >= kFailHoldMs + kFailRecoverMs) {
        k = 0.0f;
    } else if (since >= kFailHoldMs) {
        k = 1.0f - ease(kEaseInOut, static_cast<float>(since - kFailHoldMs) / static_cast<float>(kFailRecoverMs));
    }
    p[kPosTy] = 22.0f * k;
    const float tx[2] = {4.0f, -4.5f};
    const float spin[2] = {39.0f, -17.0f};
    for (std::size_t s = 0; s < 2; ++s) {
        p[eye(s, kEyeTx)] = tx[s] * k;
        p[eye(s, kEyeTy)] = 29.0f * k;
        p[eye(s, kEyeSpin)] = spin[s] * k;
        p[eye(s, kEyeSx)] = 1.0f - 0.2f * k;
        p[eye(s, kEyeSy)] = 1.0f - 0.2f * k;
    }
}

// --- Sleeping (sleeping.html, 5.6 s breath) ----------------------------------
inline constexpr Key kSleepS[] = {{0, 1.02f}, {11, 1}, {21, 1}, {46, 1.067f}, {54, 1.067f},
                                  {79, 1},    {89, 1}, {100, 1.02f}};
inline constexpr Key kSleepDrift[] = {{0, 0}, {21, 0}, {46, -12}, {54, -12}, {79, 0}, {100, 0}};

inline void pose_sleeping(Pose& p, std::uint32_t now) {
    const float c = loop_pct(now, 5600);
    p[kPosSx] = p[kPosSy] = sample(kSleepS, c, kEaseInOut);
    p[kBoxTx] = sample(kSleepDrift, c, kEaseInOut);
    set_eye_size(p, 37.0f, 24.0f, 0.0f);
    p[eye(1, kEyeOx)] = 26.0f;
}

// --- Listening (listening.html, 5 s): unequal blobs look around --------------
// Offsets are relative to the eye container centre (body centre + (33, -19)).
inline constexpr Key kListenLx[] = {{0, 17},  {15, 17},  {27, -3}, {39, -3}, {45, -25},
                                    {62, -25}, {69, -31}, {88, -31}, {100, 17}};
inline constexpr Key kListenLy[] = {{0, 3},    {15, 3},   {27, -11}, {39, -11}, {45, -19},
                                    {62, -19}, {69, 2},   {88, 2},   {100, 3}};
inline constexpr Key kListenLsx[] = {{0, 0.98f}, {15, 0.98f}, {27, 1}, {39, 1}, {45, 0.98f}, {100, 0.98f}};
inline constexpr Key kListenLsy[] = {{0, 1},    {15, 1},    {27, 0.98f}, {39, 0.98f}, {45, 0.9f},
                                     {62, 0.9f}, {69, 1},    {88, 1},     {100, 1}};
inline constexpr Key kListenRx[] = {{0, 53},  {15, 53}, {27, 42}, {39, 42}, {45, 25},
                                    {62, 25}, {69, 23}, {88, 23}, {100, 53}};
inline constexpr Key kListenRy[] = {{0, -11},  {15, -11}, {27, -23}, {39, -23}, {45, -27},
                                    {62, -27}, {69, -8},  {88, -8},  {100, -11}};
inline constexpr Key kListenRsx[] = {{0, 0.55f},  {15, 0.55f}, {27, 0.81f}, {39, 0.81f}, {45, 0.98f},
                                     {62, 0.98f}, {69, 1},     {88, 1},     {100, 0.55f}};
inline constexpr Key kListenRsy[] = {{0, 1},     {15, 1},     {27, 0.83f}, {39, 0.83f}, {45, 0.75f},
                                     {62, 0.75f}, {69, 0.98f}, {88, 0.98f}, {100, 1}};

inline void listening_eyes(Pose& p, float c) {
    p[eye(0, kEyeOx)] = sample(kListenLx, c, kEaseInOut);
    p[eye(0, kEyeOy)] = sample(kListenLy, c, kEaseInOut);
    p[eye(0, kEyeSx)] = sample(kListenLsx, c, kEaseInOut);
    p[eye(0, kEyeSy)] = sample(kListenLsy, c, kEaseInOut);
    p[eye(1, kEyeOx)] = sample(kListenRx, c, kEaseInOut);
    p[eye(1, kEyeOy)] = sample(kListenRy, c, kEaseInOut);
    p[eye(1, kEyeSx)] = sample(kListenRsx, c, kEaseInOut);
    p[eye(1, kEyeSy)] = sample(kListenRsy, c, kEaseInOut);
    p[eye(0, kEyeW)] = 44.0f;
    p[eye(0, kEyeH)] = 41.0f;
    p[eye(1, kEyeW)] = 42.0f;
    p[eye(1, kEyeH)] = 40.0f;
    p[eye(0, kEyeRot)] = 0.0f;
    p[eye(1, kEyeRot)] = 0.0f;
}

inline void pose_listening(Pose& p, std::uint32_t now) {
    listening_eyes(p, loop_pct(now, 5000));
}

// --- improvised moods in the same vocabulary ---------------------------------

// Looping hop for the joy faces: squash on contact, stretch in the air.
inline void hop(Pose& p, std::uint32_t now, std::uint32_t period_ms, float height) {
    const float u = loop_unit(now, period_ms);
    const float air = std::sin(u * kPi);
    float contact = 0.0f;
    if (u < 0.12f) {
        contact = 1.0f - u / 0.12f;
    } else if (u > 0.88f) {
        contact = (u - 0.88f) / 0.12f;
    }
    p[kPosTy] = -height * air;
    p[kPosSx] = 1.0f + 0.04f * contact;
    p[kPosSy] = 1.0f - 0.03f * contact + 0.04f * air * (1.0f - contact);
}

inline void sparkles(Pose& p, std::uint32_t now) {
    const float xs[3] = {262.0f, 286.0f, 236.0f};
    const float ys[3] = {44.0f, 90.0f, 24.0f};
    for (std::size_t i = 0; i < 3; ++i) {
        const float u = loop_unit(now, 900, static_cast<std::uint32_t>(300 * i));
        const float s = 0.25f + 0.75f * std::sin(u * kPi);
        p[sat(i, kSatX)] = xs[i];
        p[sat(i, kSatY)] = ys[i];
        p[sat(i, kSatD)] = 12.0f * s;
        p[sat(i, kSatA)] = s;
    }
}

inline void pose_happy(Pose& p, std::uint32_t now) {
    arches(p);
    hop(p, now, 1400, 8.0f);
}

inline void pose_excited(Pose& p, std::uint32_t now) {
    arches(p);
    hop(p, now, 1040, 14.0f);
    sparkles(p, now);
}

// Petting: closed happy arches glancing shyly down-left, a slow rock, blush
// and a floating heart (the last two are fx, see emit_fx).
inline void pose_affection(Pose& p, std::uint32_t now) {
    arches(p);
    p[kPosOy] = 0.0f;
    p[kBoxTx] = -8.0f;
    p[kBoxTy] = 6.0f;
    p[kBoxRot] = -8.0f;
    p[kBodyRot] = -15.0f + 3.0f * std::sin(2.0f * kPi * loop_unit(now, 2400));
}

// Bored: idle cycle slowed down, low flat half-lids (between Idle's pills
// and Sleeping's closed slits), no swell.
inline void pose_bored(Pose& p, std::uint32_t now) {
    pose_idle(p, now, 14'000, false);
    for (std::size_t s = 0; s < 2; ++s) {
        p[eye(s, kEyeW)] = 24.0f;
        p[eye(s, kEyeH)] = 15.0f;
        p[eye(s, kEyeRot)] = 0.0f;
        p[eye(s, kEyeTy)] = 8.0f;
    }
}

// Angry: "\ /" slant (screen -35 / +35 degrees), pulled together, tremble.
inline void pose_angry(Pose& p, std::uint32_t now) {
    p[eye(0, kEyeRot)] = -20.0f;
    p[eye(1, kEyeRot)] = 50.0f;
    p[eye(0, kEyeH)] = 34.0f;
    p[eye(1, kEyeH)] = 34.0f;
    p[eye(0, kEyeTx)] = 3.0f;
    p[eye(1, kEyeTx)] = -3.0f;
    p[kPosTx] = ((now / 70) % 2 == 0 ? 1.0f : -1.0f) * 1.1f;
    p[kPosSy] = 0.97f;
    set_blink(p, blink(now, 2450, 450));
}

// Dizzy: small round eyes circling out of phase, wobbling body, orbiting
// stars over the head (drawn before the body, so the back of the orbit hides).
inline void pose_dizzy(Pose& p, std::uint32_t now) {
    set_eye_size(p, 18.0f, 18.0f, 0.0f);
    const float w = 2.0f * kPi * loop_unit(now, 900);
    p[eye(0, kEyeOx)] = -26.0f + 6.0f * std::cos(w);
    p[eye(0, kEyeOy)] = 6.0f * std::sin(w);
    p[eye(1, kEyeOx)] = 26.5f + 6.0f * std::cos(w + kPi);
    p[eye(1, kEyeOy)] = 6.0f * std::sin(w + kPi);
    p[kBodyRot] = -15.0f + 6.0f * std::sin(2.0f * kPi * loop_unit(now, 1200));
    for (std::size_t i = 0; i < 3; ++i) {
        const float u = loop_unit(now, 1200, static_cast<std::uint32_t>(400 * i));
        // orbit() draws the front (big, opaque) at the top; a negative y
        // radius puts it at the bottom, towards the viewer.
        orbit(p, i, 2.0f * kPi * u, 178.0f, 34.0f, 62.0f, -12.0f, 0.0f, 13.0f);
    }
}

// Surprised: round eyes and a quick pop that settles slightly enlarged.
inline void pose_surprised(Pose& p, std::uint32_t since) {
    set_eye_size(p, 34.0f, 34.0f, 0.0f);
    const auto t = static_cast<float>(std::min<std::uint32_t>(since, 1000));
    const float pop = t < 120.0f ? t / 120.0f : std::max(0.35f, 1.0f - (t - 120.0f) / 330.0f * 0.65f);
    p[kPosSx] = p[kPosSy] = 1.0f + 0.06f * pop;
}

inline void pose_curious(Pose& p) {
    listening_eyes(p, 33.0f); // the up-left look, held
    p[kBodyRot] = -7.0f;
}

inline void pose_doubt(Pose& p, std::uint32_t now) {
    p[eye(1, kEyeSy)] = 0.4f;
    p[kBoxRot] = 6.0f;
    set_blink(p, blink(now, 2450, 450));
}

inline void pose_confused(Pose& p, std::uint32_t now) {
    pose_listening(p, now);
    p[kBodyRot] = -15.0f + 5.0f * std::sin(2.0f * kPi * loop_unit(now, 3000));
}

// Full pose for one expression. `since` is the time since that expression
// last became the target (one-shot clips).
inline Pose eval_pose(avatar::Expression e, std::uint32_t now, std::uint32_t since) {
    using avatar::Expression;
    Pose p = rest_pose();
    switch (e) {
    case Expression::Neutral:
        pose_idle(p, now);
        break;
    case Expression::Listening:
        pose_listening(p, now);
        break;
    case Expression::Thinking:
        pose_thinking(p, now);
        break;
    case Expression::Sleepy:
        pose_sleeping(p, now);
        break;
    case Expression::Working:
        pose_working(p, now);
        break;
    case Expression::Waiting:
        pose_waiting(p, now);
        break;
    case Expression::Blocked:
        pose_blocked(p, since);
        break;
    case Expression::Done:
        pose_done(p, since);
        break;
    case Expression::Failed:
        pose_failed(p, since);
        break;
    case Expression::Happy:
        pose_happy(p, now);
        break;
    case Expression::Excited:
        pose_excited(p, now);
        break;
    case Expression::Affection:
        pose_affection(p, now);
        break;
    case Expression::Bored:
        pose_bored(p, now);
        break;
    case Expression::Sad:
        pose_failed(p, 1000); // the Failed droop, held
        break;
    case Expression::Angry:
        pose_angry(p, now);
        break;
    case Expression::Dizzy:
        pose_dizzy(p, now);
        break;
    case Expression::Surprised:
        pose_surprised(p, since);
        break;
    case Expression::Curious:
        pose_curious(p);
        break;
    case Expression::Doubt:
        pose_doubt(p, now);
        break;
    case Expression::Confused:
        pose_confused(p, now);
        break;
    }
    return p;
}

// --- geometry ------------------------------------------------------------------
struct Vec {
    float x, y;
};

// x' = a x + c y + e, y' = b x + d y + f (CSS matrix(a, b, c, d, e, f)).
struct Affine {
    float a{1.0f}, b{0.0f}, c{0.0f}, d{1.0f}, e{0.0f}, f{0.0f};

    Vec operator()(Vec p) const {
        return {a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }
    float scale() const {
        return std::sqrt(std::fabs(a * d - b * c));
    }
};

inline Affine operator*(const Affine& m, const Affine& n) {
    return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b,         m.a * n.c + m.c * n.d,
            m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

inline Affine translate(float x, float y) {
    return {1.0f, 0.0f, 0.0f, 1.0f, x, y};
}

inline Affine rotate(float deg) {
    const float r = deg * kPi / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return {c, s, -s, c, 0.0f, 0.0f};
}

inline Affine scale(float sx, float sy) {
    return {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
}

inline Vec rotate_vec(Vec v, float deg) {
    const float r = deg * kPi / 180.0f;
    return {v.x * std::cos(r) - v.y * std::sin(r), v.x * std::sin(r) + v.y * std::cos(r)};
}

inline void put(float* out, std::size_t slot, Vec a, Vec b, float r, std::uint16_t colour, std::uint32_t flags = 0) {
    float* s = out + slot * kFloatsPerSegment;
    s[0] = a.x;
    s[1] = a.y;
    s[2] = b.x;
    s[3] = b.y;
    s[4] = r;
    s[5] = static_cast<float>(flags | colour);
}

inline void skip(float* out, std::size_t slot) {
    put(out, slot, {0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f, 0);
}

// A w x h box with fully rounded ends (CSS border-radius = half the short
// side), centred at `c`, rotated by `deg` in frame `m`.
inline void put_pill(float* out, std::size_t slot, const Affine& m, Vec c, float w, float h, float deg,
                     std::uint16_t colour) {
    const float r = 0.5f * std::min(w, h) * m.scale();
    if (w < 0.5f || h < 0.5f || r < 0.3f) {
        skip(out, slot);
        return;
    }
    const float half = 0.5f * (std::max(w, h) - std::min(w, h));
    const Vec dir = rotate_vec(w >= h ? Vec{1.0f, 0.0f} : Vec{0.0f, 1.0f}, deg);
    put(out, slot, m({c.x - dir.x * half, c.y - dir.y * half}), m({c.x + dir.x * half, c.y + dir.y * half}), r,
        colour);
}

inline void put_circle(float* out, std::size_t slot, Vec c, float d, std::uint16_t colour) {
    if (d < 0.6f) {
        skip(out, slot);
        return;
    }
    put(out, slot, c, c, 0.5f * d, colour);
}

// RGB565 per-channel mix, t = 0 -> a, t = 1 -> b.
inline std::uint16_t mix565(std::uint16_t a, std::uint16_t b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto ch = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
    const int r = ch((a >> 11) & 0x1F, (b >> 11) & 0x1F);
    const int g = ch((a >> 5) & 0x3F, (b >> 5) & 0x3F);
    const int bl = ch(a & 0x1F, b & 0x1F);
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | bl);
}

inline constexpr std::uint16_t kBlushTint = 0xFC76; // #FF8FB1

// Frames shared by the shapes and the fx of one frame.
struct Frames {
    Affine body; // body frame: centre, -15 degrees, speech squash
    Affine box;  // eye container frame
};

// Arch box centres in the container frame (done.html: 45 x 34 boxes at
// left 0 / 52, top 12 of the 90 x 70 container).
inline constexpr Vec kArchCentre[2] = {{-22.5f, -6.0f}, {29.5f, -6.0f}};

// One-glyph "z": three round-ended strokes, top-left at (x, y), size s.
inline void put_z(float* out, std::size_t slot, float x, float y, float s, std::uint16_t colour) {
    const float r = std::max(1.2f, 0.11f * s);
    put(out, slot + 0, {x, y}, {x + s, y}, r, colour);
    put(out, slot + 1, {x + s, y}, {x, y + s}, r, colour);
    put(out, slot + 2, {x, y + s}, {x + s, y + s}, r, colour);
}

inline float fade_in_out(float u) {
    if (u < 0.2f) {
        return u / 0.2f;
    }
    if (u > 0.7f) {
        return (1.0f - u) / 0.3f;
    }
    return 1.0f;
}

} // namespace detail

// Stateful only to remember when each expression became the target, so
// one-shot clips (Done, Failed, Blocked, Surprised) start at their beginning
// and keep running while they fade out.
class Compositor {
public:
    void compose(const avatar::DrawContext& ctx, std::uint32_t now_ms, float* out) {
        using namespace detail;
        if (!started_ || ctx.expression != last_to_) {
            entered_ms_[static_cast<std::size_t>(ctx.expression) % avatar::kExpressionCount] = now_ms;
            last_to_ = ctx.expression;
            started_ = true;
        }

        // Same four-way weights as ExpressionController writes them.
        const float b = std::clamp(ctx.expression_blend, 0.0f, 1.0f);
        const float hb = std::clamp(ctx.expression_hold_blend, 0.0f, 1.0f);
        const float hb2 = std::clamp(ctx.expression_hold2_blend, 0.0f, 1.0f);
        struct Mix {
            avatar::Expression e;
            float w;
        };
        Mix mix[4] = {
            {ctx.expression_from, (1.0f - hb2) * (1.0f - hb) * (1.0f - b)},
            {ctx.expression_hold2_to, hb2 * (1.0f - hb) * (1.0f - b)},
            {ctx.expression_hold_to, hb * (1.0f - b)},
            {ctx.expression, b},
        };
        // Merge duplicates so each expression is evaluated once.
        for (std::size_t i = 0; i < 4; ++i) {
            for (std::size_t j = i + 1; j < 4; ++j) {
                if (mix[j].w > 0.0f && mix[j].e == mix[i].e) {
                    mix[i].w += mix[j].w;
                    mix[j].w = 0.0f;
                }
            }
        }

        Pose pose{};
        float weight_sum = 0.0f;
        std::array<float, avatar::kExpressionCount> weight_of{};
        for (const auto& m : mix) {
            if (m.w <= 0.001f) {
                continue;
            }
            const auto idx = static_cast<std::size_t>(m.e) % avatar::kExpressionCount;
            const Pose p = eval_pose(m.e, now_ms, now_ms - entered_ms_[idx]);
            for (std::size_t k = 0; k < kPoseFloats; ++k) {
                pose[k] += p[k] * m.w;
            }
            weight_of[idx] += m.w;
            weight_sum += m.w;
        }
        if (weight_sum <= 0.0f) {
            pose = eval_pose(ctx.expression, now_ms, 0);
        } else if (weight_sum < 0.999f || weight_sum > 1.001f) {
            for (auto& v : pose) {
                v /= weight_sum;
            }
        }

        apply_global(ctx, now_ms, pose);
        const auto frames = build(ctx, pose, out);
        emit_fx(ctx, now_ms, weight_of, frames, out);
    }

private:
    // Boot wake, commanded gaze: layered on top of the blended pose.
    static void apply_global(const avatar::DrawContext& ctx, std::uint32_t now_ms, detail::Pose& p) {
        using namespace detail;
        // Boot: eyes closed for 0.8 s, then one slow blink open.
        if (now_ms < 1100) {
            const float open = now_ms < 800 ? 0.0f : static_cast<float>(now_ms - 800) / 300.0f;
            const float lid = 0.25f + 0.75f * smoothstep(open);
            for (std::size_t s = 0; s < 2; ++s) {
                p[eye(s, kEyeBlink)] = std::min(p[eye(s, kEyeBlink)], lid);
            }
        }
        // Commanded gaze (touch follow) nudges the eye container; the clamp
        // keeps the pair inside the capsule (design: up <= 15, right <= 25).
        p[kBoxTx] += std::clamp(ctx.gaze_horizontal, -1.0f, 1.0f) * 10.0f;
        p[kBoxTy] += std::clamp(ctx.gaze_vertical, -1.0f, 1.0f) * 8.0f;
    }

    static detail::Frames build(const avatar::DrawContext& ctx, const detail::Pose& p, float* out) {
        using namespace detail;
        const std::uint16_t ink = ctx.palette.primary;
        const std::uint16_t ground = ctx.palette.background;
        const std::uint16_t body_colour = mix565(ground, ink, p[kBodyAlpha]);

        // Satellites: screen space, drawn first (behind the body).
        for (std::size_t i = 0; i < kSatellites; ++i) {
            put_circle(out, kSlotSatellites + i, {p[sat(i, kSatX)], p[sat(i, kSatY)]}, p[sat(i, kSatD)],
                       mix565(ground, ink, p[sat(i, kSatA)]));
        }

        // .pos: translate + scale about the origin, in screen axes.
        const Vec centre{p[kBodyX], p[kBodyY]};
        const Vec origin{centre.x + p[kPosOx], centre.y + p[kPosOy]};
        const Affine pos =
            translate(origin.x + p[kPosTx], origin.y + p[kPosTy]) * scale(p[kPosSx], p[kPosSy]) *
            translate(-origin.x, -origin.y);
        // Speech: squash along the body's short axis (sx = 1 + 0.04a,
        // sy = 1 - 0.14a).
        const float talk = std::clamp(ctx.mouth_open_ratio, 0.0f, 1.0f);
        const float speech_x = 1.0f + 0.04f * talk;
        const float speech_y = 1.0f - 0.14f * talk;

        Frames f;
        f.body = pos * translate(centre.x, centre.y) * rotate(p[kBodyRot]) * scale(speech_x, speech_y);
        // The body capsule itself: size scaled directly so it stays a true
        // capsule (screen-axis scale ~ body axes at -15 degrees).
        put_pill(out, kSlotBody, pos * translate(centre.x, centre.y) * rotate(p[kBodyRot]), {0.0f, 0.0f},
                 p[kBodyW] * speech_x, p[kBodyH] * speech_y, 0.0f, body_colour);

        f.box = f.body * translate(p[kBoxX] + p[kBoxTx], p[kBoxY] + p[kBoxTy]) * rotate(p[kBoxRot]);

        // Eyes: two halves each, ground colour (holes).
        for (std::size_t s = 0; s < 2; ++s) {
            const Affine eye_frame = f.box * translate(p[eye(s, kEyeOx)], p[eye(s, kEyeOy)]) *
                                     rotate(p[eye(s, kEyeRot)]) *
                                     translate(p[eye(s, kEyeTx)], p[eye(s, kEyeTy)]) * rotate(p[eye(s, kEyeSpin)]);
            const float w = p[eye(s, kEyeW)] * p[eye(s, kEyeSx)];
            const float h = p[eye(s, kEyeH)] * p[eye(s, kEyeSy)] * p[eye(s, kEyeBlink)] * p[kBoxSy];
            // Split halves: blocked.html cuts a 20 x 47 eye into two 20 x 27
            // capsules 10 px either side of centre (7 px overlap).
            const float split = std::clamp(p[eye(s, kEyeSplit)], 0.0f, 1.0f);
            const float half_h_split = 0.5f * h + 0.175f * w;
            const float half_h = h + (half_h_split - h) * split;
            const float d = (0.5f * h - 0.5f * half_h_split) * split;
            const float fold = p[eye(s, kEyeFold)];
            // Upper half folds about its bottom centre, lower about its top.
            const Vec up_pivot{0.0f, -d + 0.5f * half_h};
            const Vec up_off = rotate_vec({0.0f, -0.5f * half_h}, -fold);
            const Vec lo_pivot{0.0f, d - 0.5f * half_h};
            const Vec lo_off = rotate_vec({0.0f, 0.5f * half_h}, fold);
            put_pill(out, kSlotEyes + 2 * s, eye_frame, {up_pivot.x + up_off.x, up_pivot.y + up_off.y}, w, half_h,
                     -fold, ground);
            put_pill(out, kSlotEyes + 2 * s + 1, eye_frame, {lo_pivot.x + lo_off.x, lo_pivot.y + lo_off.y}, w,
                     half_h, fold, ground);
        }

        // Done arches (done.html: 45 x 34 box, 17 px border, top corners
        // rounded 22.5, no bottom border): a ground capsule with a flat
        // bottom, a body-colour cut-out, and a body-colour strip that trims
        // the round end's overshoot below the flat bottom (the arc centre is
        // only 11.5 px above it, the radius is 22.5).
        const float arch = std::clamp(p[kArch], 0.0f, 1.0f);
        const float sc = f.box.scale();
        for (std::size_t s = 0; s < 2; ++s) {
            if (arch < 0.02f) {
                skip(out, kSlotArchOuter + s);
                skip(out, kSlotArchInner + s);
                skip(out, kSlotArchTrim + s);
                continue;
            }
            const Vec c = kArchCentre[s];
            const Vec a = f.box({c.x, c.y + 5.5f * arch});
            const Vec bot = f.box({c.x, c.y + 17.0f * arch});
            // The overshoot spans a chord of 2 * sqrt(22.5^2 - 11.5^2) = 38.7 px
            // at the flat bottom and ends 22.5 px below the arc centre; keep
            // the strip just wider than that so its corners stay on the body.
            const Vec trim_end = f.box({c.x, c.y + 28.5f * arch});
            put(out, kSlotArchOuter + s, a, bot, 22.5f * arch * sc, ground, kFlatB);
            put(out, kSlotArchInner + s, a, bot, 5.5f * arch * sc, body_colour, kFlatB);
            put(out, kSlotArchTrim + s, bot, trim_end, 20.5f * arch * sc, body_colour, kFlatA | kFlatB);
        }
        return f;
    }

    // Decorations owned by one expression; they fade with its weight instead
    // of being blended into another shape.
    static void emit_fx(const avatar::DrawContext& ctx, std::uint32_t now_ms,
                        const std::array<float, avatar::kExpressionCount>& weight_of, const detail::Frames& f,
                        float* out) {
        using namespace detail;
        const std::uint16_t ink = ctx.palette.primary;
        const std::uint16_t ground = ctx.palette.background;

        // Sleeping: two "z" glyphs drifting up and out past the top-right.
        const float zw = weight_of[static_cast<std::size_t>(avatar::Expression::Sleepy)];
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t slot = kSlotZzz + 3 * i;
            if (zw <= 0.01f) {
                for (std::size_t k = 0; k < 3; ++k) {
                    skip(out, slot + k);
                }
                continue;
            }
            const float u = loop_unit(now_ms, 2800, static_cast<std::uint32_t>(1400 * i));
            const float s = (10.0f + 3.0f * static_cast<float>(i)) * (0.8f + 0.3f * u);
            put_z(out, slot, 248.0f + 8.0f * u, 58.0f - 28.0f * u, s, mix565(ground, ink, zw * fade_in_out(u)));
        }

        // Petting: blush under each arch and a heart floating up.
        const float aw = weight_of[static_cast<std::size_t>(avatar::Expression::Affection)];
        if (aw <= 0.01f) {
            for (std::size_t k = 0; k < 2; ++k) {
                skip(out, kSlotBlush + k);
            }
            for (std::size_t k = 0; k < 4; ++k) {
                skip(out, kSlotHeart + k);
            }
            return;
        }
        const std::uint16_t body_colour = ink;
        const std::uint16_t blush = mix565(body_colour, mix565(ink, kBlushTint, 0.55f), aw);
        for (std::size_t s = 0; s < 2; ++s) {
            const Vec c = kArchCentre[s];
            put_pill(out, kSlotBlush + s, f.box, {c.x, c.y + 27.0f}, 24.0f, 10.0f, 0.0f, blush);
        }
        const float u = loop_unit(now_ms, 2000);
        const float hs = 8.5f + 2.0f * u;
        const Vec h{252.0f + 4.0f * u, 62.0f - 20.0f * u};
        const std::uint16_t hc = mix565(ground, ink, aw * fade_in_out(u));
        const Vec lobe_l{h.x - 0.48f * hs, h.y - 0.2f * hs};
        const Vec lobe_r{h.x + 0.48f * hs, h.y - 0.2f * hs};
        const Vec tip{h.x, h.y + 0.85f * hs};
        put(out, kSlotHeart + 0, lobe_l, lobe_l, 0.52f * hs, hc);
        put(out, kSlotHeart + 1, lobe_r, lobe_r, 0.52f * hs, hc);
        put(out, kSlotHeart + 2, lobe_l, tip, 0.3f * hs, hc);
        put(out, kSlotHeart + 3, lobe_r, tip, 0.3f * hs, hc);
    }

    std::array<std::uint32_t, avatar::kExpressionCount> entered_ms_{};
    avatar::Expression last_to_{avatar::Expression::Neutral};
    bool started_{false};
};

} // namespace stackchan::app::pill
