// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
//
// Runs the real face pipeline on the host: pill compositor (main/
// pill_face.hpp) -> face-parameter window -> avatar VM running the compiled
// assets/grok_face.avdsl -> a software RGB565 canvas.
//
//   face_preview check  <face.avbc>
//       every expression for 10 s at 50 ms steps: VM runs cleanly and stays
//       within the primitive budget (registered as a ctest).
//   face_preview render <face.avbc> <out_dir> <expr>:<t_ms>[,<t_ms>...] ...
//       write <out_dir>/<expr>_<t_ms>.ppm frames (320 x 240) for comparing
//       against the design mocks at the same animation time. For one-shot
//       states (done, failed, blocked, surprised) t is the time since entry.
//       FACE_PREVIEW_SIZE=128x128 renders another panel size;
//       FACE_PREVIEW_ROUND=1 marks it round (StopWatch).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "avatar/canvas.hpp"
#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/expression_names.hpp"
#include "avatar/face_tuning.hpp"
#include "avatar_vm/bytecode.hpp"
#include "avatar_vm/opcodes.hpp"
#include "avatar_vm/vm.hpp"
#include "pill_face.hpp"

namespace {

using stackchan::avatar::DrawContext;
using stackchan::avatar::Expression;

// Minimal RGB565 rasterizer with the primitive set the face uses.
class SoftCanvas final : public stackchan::avatar::Canvas {
public:
    SoftCanvas(std::int32_t w, std::int32_t h, bool round = false)
        : w_{w}, h_{h}, round_{round}, px_(static_cast<std::size_t>(w * h), 0) {}

    std::int32_t width() const override { return w_; }
    std::int32_t height() const override { return h_; }
    bool is_circular() const noexcept override { return round_; }

    void fillScreen(std::uint16_t c) override {
        std::fill(px_.begin(), px_.end(), c);
        ++ops;
    }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint16_t c) override {
        ++ops;
        for (std::int32_t yy = std::max(0, y); yy < std::min(h_, y + h); ++yy) {
            span(yy, x, x + w - 1, c);
        }
    }
    void fillCircle(std::int32_t cx, std::int32_t cy, std::int32_t r, std::uint16_t c) override {
        ++ops;
        for (std::int32_t dy = -r; dy <= r; ++dy) {
            const auto half = static_cast<std::int32_t>(std::sqrt(static_cast<float>(r * r - dy * dy + r)));
            span(cy + dy, cx - half, cx + half, c);
        }
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::int32_t x2,
                      std::int32_t y2, std::uint16_t c) override {
        ++ops;
        // Sort by y, then fill each scanline between the long edge and the
        // two short edges.
        if (y0 > y1) {
            std::swap(y0, y1);
            std::swap(x0, x1);
        }
        if (y1 > y2) {
            std::swap(y1, y2);
            std::swap(x1, x2);
        }
        if (y0 > y1) {
            std::swap(y0, y1);
            std::swap(x0, x1);
        }
        const auto edge = [](std::int32_t ya, std::int32_t xa, std::int32_t yb, std::int32_t xb, std::int32_t y) {
            if (yb == ya) {
                return static_cast<float>(xa);
            }
            return static_cast<float>(xa) +
                   static_cast<float>(xb - xa) * static_cast<float>(y - ya) / static_cast<float>(yb - ya);
        };
        for (std::int32_t y = y0; y <= y2; ++y) {
            const float a = edge(y0, x0, y2, x2, y);
            const float b = y < y1 ? edge(y0, x0, y1, x1, y) : edge(y1, x1, y2, x2, y);
            span(y, static_cast<std::int32_t>(std::lround(std::min(a, b))),
                 static_cast<std::int32_t>(std::lround(std::max(a, b))), c);
        }
    }
    void begin_group(std::int32_t, std::int32_t, std::int32_t, std::int32_t) override {}
    void end_group() override {}
    void begin_frame(std::uint16_t bg) override { std::fill(px_.begin(), px_.end(), bg); }
    void end_frame() override {}
    void request_full_repaint() override {}

    bool write_ppm(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return false;
        }
        out << "P6\n" << w_ << ' ' << h_ << "\n255\n";
        for (const std::uint16_t p : px_) {
            const auto r = static_cast<unsigned char>(((p >> 11) & 0x1F) * 255 / 31);
            const auto g = static_cast<unsigned char>(((p >> 5) & 0x3F) * 255 / 63);
            const auto b = static_cast<unsigned char>((p & 0x1F) * 255 / 31);
            out.put(static_cast<char>(r)).put(static_cast<char>(g)).put(static_cast<char>(b));
        }
        return static_cast<bool>(out);
    }

    int ops = 0;

private:
    void span(std::int32_t y, std::int32_t x0, std::int32_t x1, std::uint16_t c) {
        if (y < 0 || y >= h_) {
            return;
        }
        for (std::int32_t x = std::max(0, x0); x <= std::min(w_ - 1, x1); ++x) {
            px_[static_cast<std::size_t>(y * w_ + x)] = c;
        }
    }

    std::int32_t w_;
    std::int32_t h_;
    bool round_;
    std::vector<std::uint16_t> px_;
};

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

// Common multiple of every loop period: (kBase + t) puts each loop at its
// mock animation time t.
constexpr std::uint32_t kBase = 1'260'000;

struct Pipeline {
    stackchan::avatar_vm::Bytecode bc;
    stackchan::avatar_vm::Vm vm;
    stackchan::app::pill::Compositor compositor;
    std::array<float, stackchan::avatar_vm::kParamVarCount> params{};
    stackchan::avatar::FaceTuning tuning{};

    bool frame(DrawContext& ctx, std::uint32_t now, SoftCanvas& canvas) {
        compositor.compose(ctx, now, params.data());
        ctx.face_params = params.data();
        ctx.now_ms = now;
        canvas.ops = 0;
        canvas.begin_frame(ctx.palette.background);
        auto r = vm.run(bc, canvas, ctx, tuning);
        if (!r) {
            std::fprintf(stderr, "vm.run failed: %s\n", stackchan::avatar_vm::to_string(r.error()));
            return false;
        }
        return true;
    }
};

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int check(Pipeline& base_pipeline) {
    int worst = 0;
    int failures = 0;
    for (std::size_t e = 0; e < stackchan::avatar::kExpressionCount; ++e) {
        Pipeline p{base_pipeline.bc, {}, {}, {}, {}};
        DrawContext ctx = settled(static_cast<Expression>(e));
        for (std::uint32_t t = 0; t < 10'000; t += 50) {
            SoftCanvas canvas(320, 240, false);
            if (!p.frame(ctx, kBase + t, canvas)) {
                ++failures;
                break;
            }
            worst = std::max(worst, canvas.ops);
        }
    }
    // Also a small round panel and the 128 x 128 AtomS3R: scaling must not
    // break anything.
    for (const std::int32_t side : {128, 466}) {
        Pipeline p{base_pipeline.bc, {}, {}, {}, {}};
        DrawContext ctx = settled(Expression::Neutral);
        SoftCanvas canvas(side, side, side > 300);
        if (!p.frame(ctx, kBase + 2500, canvas)) {
            ++failures;
        }
    }
    std::printf("face_bytecode: at most %d primitives per frame\n", worst);
    if (worst > 80) {
        std::fprintf(stderr, "over the 80-primitive budget\n");
        ++failures;
    }
    if (failures == 0) {
        std::printf("[ OK ] face_bytecode\n");
        return 0;
    }
    return 1;
}

int render(Pipeline& base_pipeline, const std::string& dir, int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto colon = arg.find(':');
        if (colon == std::string::npos) {
            std::fprintf(stderr, "expected <expr>:<t_ms>[,<t_ms>...], got %s\n", arg.c_str());
            return 2;
        }
        const std::string name = arg.substr(0, colon);
        const auto expr = stackchan::avatar::expression_from_name(name);
        if (!expr) {
            std::fprintf(stderr, "unknown expression %s\n", name.c_str());
            return 2;
        }
        std::size_t pos = colon + 1;
        while (pos < arg.size()) {
            const auto comma = arg.find(',', pos);
            const std::string tok = arg.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? arg.size() : comma + 1;
            const auto t = static_cast<std::uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
            Pipeline p{base_pipeline.bc, {}, {}, {}, {}};
            DrawContext ctx = settled(*expr);
            int w = 320;
            int h = 240;
            if (const char* size = std::getenv("FACE_PREVIEW_SIZE")) {
                std::sscanf(size, "%dx%d", &w, &h);
            }
            const char* round = std::getenv("FACE_PREVIEW_ROUND");
            SoftCanvas canvas(w, h, round != nullptr && round[0] == '1');
            // Enter the expression at kBase, then advance to the sample time.
            if (!p.frame(ctx, kBase, canvas) || (t != 0 && !p.frame(ctx, kBase + t, canvas))) {
                return 1;
            }
            const std::string path = dir + "/" + name + "_" + tok + ".ppm";
            if (!canvas.write_ppm(path)) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 1;
            }
            std::printf("%s (%d primitives)\n", path.c_str(), canvas.ops);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: face_preview check <face.avbc>\n"
                     "       face_preview render <face.avbc> <out_dir> <expr>:<t_ms>[,<t_ms>...] ...\n");
        return 2;
    }
    const std::string mode = argv[1];
    const auto bytes = read_file(argv[2]);
    auto decoded = stackchan::avatar_vm::decode(bytes);
    if (!decoded) {
        std::fprintf(stderr, "decode failed: %s\n", stackchan::avatar_vm::to_string(decoded.error()));
        return 1;
    }
    Pipeline pipeline{std::move(*decoded), {}, {}, {}, {}};
    if (mode == "check") {
        return check(pipeline);
    }
    if (mode == "render" && argc >= 4) {
        return render(pipeline, argv[3], argc - 4, argv + 4);
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
