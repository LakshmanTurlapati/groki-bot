// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0
//
// Drives parse_control() with the JSON text frames the gateway sends, the
// same entry point XiaozhiClient uses.

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "conversation/activity_control.hpp"
#include "conversation/control_dispatch.hpp"
#include "conversation/tracking_control.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const char* expr, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] line %d  CHECK(%s)\n", line, expr);
    }
}

#define CHECK(cond) report((cond), #cond, __LINE__)

using stackchan::conversation::HeadCommand;
using stackchan::conversation::HeadCommandQueue;

struct Dispatch {
    std::vector<HeadCommand> heads;
    std::vector<std::string> others;
    std::vector<std::string> errors;
};

Dispatch dispatch(const char* text)
{
    Dispatch out;
    stackchan::conversation::parse_control(
        text, std::strlen(text), [&](const HeadCommand& head) { out.heads.push_back(head); },
        [&](const char* type, const cJSON*) { out.others.emplace_back(type); },
        [&](const char* error) { out.errors.emplace_back(error); });
    return out;
}

void test_head_message_reaches_head_sink()
{
    const auto d = dispatch(R"({"type":"head","yaw":30,"pitch":10,"speed":200})");
    CHECK(d.heads.size() == 1);
    CHECK(d.others.empty());
    CHECK(d.errors.empty());
    if (!d.heads.empty()) {
        CHECK(d.heads[0].yaw == 30.0f);
        CHECK(d.heads[0].pitch == 10.0f);
        CHECK(d.heads[0].speed == 200);
    }
}

void test_head_values_clamp_to_wire_range()
{
    const auto d = dispatch(R"({"type":"head","yaw":-200,"pitch":99,"speed":99999})");
    CHECK(d.heads.size() == 1);
    if (!d.heads.empty()) {
        CHECK(d.heads[0].yaw == -90.0f);
        CHECK(d.heads[0].pitch == 60.0f);
        CHECK(d.heads[0].speed == 1000);
    }
    const auto low = dispatch(R"({"type":"head","yaw":0,"pitch":-5,"speed":1})");
    CHECK(low.heads.size() == 1);
    if (!low.heads.empty()) {
        CHECK(low.heads[0].pitch == 0.0f);
        CHECK(low.heads[0].speed == 100);
    }
}

void test_head_without_speed_uses_default()
{
    const auto d = dispatch(R"({"type":"head","yaw":5,"pitch":20})");
    CHECK(d.heads.size() == 1);
    if (!d.heads.empty()) CHECK(d.heads[0].speed == 0);
}

void test_invalid_head_messages_are_dropped()
{
    const char* bad[] = {
        R"({"type":"head","pitch":10,"speed":200})",            // yaw missing
        R"({"type":"head","yaw":30,"speed":200})",              // pitch missing
        R"({"type":"head","yaw":"30","pitch":10})",             // yaw not a number
        R"({"type":"head","yaw":30,"pitch":null})",             // pitch null
        R"({"type":"head","yaw":30,"pitch":10,"speed":"fast"})", // speed not a number
        R"({"type":"head","yaw":[30],"pitch":{}})",
    };
    for (const char* text : bad) {
        const auto d = dispatch(text);
        CHECK(d.heads.empty());
        CHECK(d.others.empty());
        CHECK(d.errors.size() == 1);
    }
}

void test_other_types_pass_through()
{
    const auto led = dispatch(R"({"type":"led","r":1,"g":2,"b":3})");
    CHECK(led.heads.empty());
    CHECK(led.others.size() == 1 && led.others[0] == "led");
    const auto tts = dispatch(R"({"type":"tts","state":"start"})");
    CHECK(tts.others.size() == 1 && tts.others[0] == "tts");
}

void test_malformed_json_and_missing_type()
{
    const auto broken = dispatch(R"({"type":"head","yaw":)");
    CHECK(broken.heads.empty());
    CHECK(broken.errors.size() == 1);
    const auto untyped = dispatch(R"({"yaw":30,"pitch":10})");
    CHECK(untyped.heads.empty());
    CHECK(untyped.others.empty());
    CHECK(untyped.errors.empty());
    const auto numeric_type = dispatch(R"({"type":7})");
    CHECK(numeric_type.others.empty());
}

void test_queue_holds_last_command_while_speaking()
{
    HeadCommandQueue queue;
    CHECK(!queue.receive({10, 5, 200}, true));
    CHECK(!queue.receive({-15, 8, 400}, true));
    const auto deferred = queue.finish();
    CHECK(deferred.has_value());
    if (deferred) {
        CHECK(deferred->yaw == -15.0f);
        CHECK(deferred->pitch == 8.0f);
        CHECK(deferred->speed == 400);
    }
    CHECK(!queue.finish());
    const auto immediate = queue.receive({20, 15, 200}, false);
    CHECK(immediate.has_value() && immediate->yaw == 20.0f);
    CHECK(!queue.finish());
}

void test_tracking_controls() {
    using namespace stackchan::conversation;
    for (const auto* mode : {"off", "tracking", "conversation", "engaged"}) {
        const std::string json = std::string{"{\"mode\":\""} + mode + "\"}";
        auto* root = cJSON_Parse(json.c_str());
        CHECK(parse_head_tracking_mode(root).has_value());
        cJSON_Delete(root);
    }
    for (const auto* json : {R"({})", R"({"mode":true})", R"({"mode":"listening"})"}) {
        auto* root = cJSON_Parse(json);
        CHECK(!parse_head_tracking_mode(root));
        cJSON_Delete(root);
    }
    auto* root = cJSON_Parse(R"({"frame_id":"abcdef1200000001","delta_yaw":100,"delta_pitch":-100})");
    auto command = parse_head_track(root);
    CHECK(command && command->delta_yaw == 15 && command->delta_pitch == -10 && !command->release);
    cJSON_Delete(root);
    root = cJSON_Parse(R"({"frame_id":"abcdef1200000001","release":true})");
    command = parse_head_track(root);
    CHECK(command && command->release && command->frame_id == "abcdef1200000001");
    cJSON_Delete(root);
    const char* malformed[] = {
        R"({"frame_id":"abcdef1200000001","release":true,"delta_yaw":0})",
        R"({"frame_id":"abcdef1200000001","release":"true"})",
        R"({"frame_id":"abcdef1200000001","delta_yaw":0})",
        R"({"frame_id":"abcdef1200000001","delta_yaw":"1","delta_pitch":1})",
        R"({"frame_id":"abcdef1200000001","delta_yaw":1e999,"delta_pitch":1})",
        R"({"frame_id":"","delta_yaw":1,"delta_pitch":1})",
        R"({"frame_id":"abcdef120000000g","delta_yaw":1,"delta_pitch":1})",
        R"({"frame_id":"abcdef1200000001000000000000000000000000000","release":true})",
    };
    for (const auto* json : malformed) {
        root = cJSON_Parse(json);
        CHECK(!parse_head_track(root));
        cJSON_Delete(root);
    }
}

} // namespace

void test_bot_activity()
{
    using stackchan::conversation::kMaxActivityTtlMs;
    using stackchan::conversation::parse_bot_activity;
    {
        auto* root = cJSON_Parse(R"({"type":"activity","state":"working","ttl_ms":120000})");
        const auto cmd = parse_bot_activity(root);
        CHECK(cmd && cmd->state == "working" && cmd->ttl_ms == 120000u);
        cJSON_Delete(root);
    }
    {
        // ttl_ms is optional; 0 / negative / absent all mean "default".
        for (const char* json : {R"({"state":"done"})", R"({"state":"done","ttl_ms":0})",
                                 R"({"state":"done","ttl_ms":-5})", R"({"state":"done","ttl_ms":"9"})"}) {
            auto* root = cJSON_Parse(json);
            const auto cmd = parse_bot_activity(root);
            CHECK(cmd && cmd->state == "done" && cmd->ttl_ms == 0u);
            cJSON_Delete(root);
        }
    }
    {
        auto* root = cJSON_Parse(R"({"state":"waiting","ttl_ms":1e12})");
        const auto cmd = parse_bot_activity(root);
        CHECK(cmd && cmd->ttl_ms == kMaxActivityTtlMs);
        cJSON_Delete(root);
    }
    for (const char* json : {R"({})", R"({"state":5})", R"({"state":""})",
                             R"({"state":"a-name-that-is-far-too-long"})"}) {
        auto* root = cJSON_Parse(json);
        CHECK(!parse_bot_activity(root));
        cJSON_Delete(root);
    }
}

int main()
{
    test_head_message_reaches_head_sink();
    test_head_values_clamp_to_wire_range();
    test_head_without_speed_uses_default();
    test_invalid_head_messages_are_dropped();
    test_other_types_pass_through();
    test_malformed_json_and_missing_type();
    test_queue_holds_last_command_while_speaking();
    test_tracking_controls();
    test_bot_activity();
    if (g_failures == 0) {
        std::printf("[ OK ] control_dispatch: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "control_dispatch: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
