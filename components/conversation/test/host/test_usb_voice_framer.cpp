// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "conversation/usb_voice_framer.hpp"
#include "conversation/usb_voice_lease.hpp"

namespace {

int g_failures = 0;

void report(bool ok, const char* expr, int line)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] line %d  CHECK(%s)\n", line, expr);
    }
}

#define CHECK(cond) report((cond), #cond, __LINE__)

using stackchan::conversation::UsbVoiceEvent;
using stackchan::conversation::UsbVoiceEventKind;
using stackchan::conversation::UsbVoiceFrameType;
using stackchan::conversation::UsbVoiceStreamParser;
using stackchan::conversation::encode_usb_voice_frame;

std::vector<std::uint8_t> bytes_of(const char* text)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(text);
    return std::vector<std::uint8_t>(begin, begin + std::strlen(text));
}

} // namespace

int main()
{
    stackchan::conversation::UsbVoiceLease lease;
    CHECK(!lease.active(1000));
    CHECK(lease.enable(1000));
    CHECK(lease.refresh(1001));
    // Another task can renew after a caller samples now, before active loads
    // the renewal time. That newer timestamp must not expire the live lease.
    CHECK(lease.active(1000));
    CHECK(lease.active(11000));
    CHECK(!lease.active(11001));
    CHECK(!lease.refresh(11001));
    stackchan::conversation::UsbVoiceLease wrapped;
    CHECK(wrapped.enable(0xfffffff0u));
    CHECK(wrapped.active(0x10u));
    CHECK(wrapped.refresh(0x11u));
    CHECK(wrapped.active(0x10u));
    CHECK(!wrapped.active(0x11u + wrapped.kDurationMs));
    lease.disable();
    CHECK(!lease.active(1001));

    const std::uint8_t opus[] = {'\n', '$', 'M', 'C', 'P', ':', ' ', 'x', '\n', 0x00};
    const auto frame = encode_usb_voice_frame(UsbVoiceFrameType::AudioUplink, opus);
    CHECK(!frame.empty());
    CHECK(frame[0] == 0xFF && frame[1] == 0xFE);
    CHECK(frame[2] == 1);
    CHECK(frame[3] == static_cast<std::uint8_t>(UsbVoiceFrameType::AudioUplink));

    std::vector<std::uint8_t> stream = bytes_of("$MCP: {\"id\":1}\n");
    stream.insert(stream.end(), frame.begin(), frame.end());
    const auto log_line = bytes_of("I (9) log: still a line\n");
    stream.insert(stream.end(), log_line.begin(), log_line.end());

    UsbVoiceStreamParser parser;
    // Feed one byte at a time, including a split sync word.
    for (const auto byte : stream) {
        parser.feed(std::span<const std::uint8_t>{&byte, 1});
    }

    UsbVoiceEvent ev;
    CHECK(parser.pop(ev));
    CHECK(ev.kind == UsbVoiceEventKind::Line);
    CHECK(ev.line == "$MCP: {\"id\":1}");

    CHECK(parser.pop(ev));
    CHECK(ev.kind == UsbVoiceEventKind::Frame);
    CHECK(ev.frame.type == UsbVoiceFrameType::AudioUplink);
    CHECK(ev.frame.payload.size() == sizeof(opus));
    CHECK(std::memcmp(ev.frame.payload.data(), opus, sizeof(opus)) == 0);

    CHECK(parser.pop(ev));
    CHECK(ev.kind == UsbVoiceEventKind::Line);
    CHECK(ev.line == "I (9) log: still a line");
    CHECK(!parser.pop(ev));

    const auto too_big = std::vector<std::uint8_t>(1276, 0x11);
    CHECK(encode_usb_voice_frame(UsbVoiceFrameType::AudioDownlink, too_big).empty());

    // A bad CRC must not swallow the following text line.
    std::vector<std::uint8_t> corrupt = frame;
    corrupt.back() ^= 0xFF;
    const auto follow = bytes_of("$MCP: {\"id\":2}\n");
    corrupt.insert(corrupt.end(), follow.begin(), follow.end());
    UsbVoiceStreamParser resync;
    resync.feed(corrupt);
    bool saw_line = false;
    bool saw_frame = false;
    while (resync.pop(ev)) {
        if (ev.kind == UsbVoiceEventKind::Frame) {
            saw_frame = true;
        }
        if (ev.kind == UsbVoiceEventKind::Line && ev.line == "$MCP: {\"id\":2}") {
            saw_line = true;
        }
    }
    CHECK(!saw_frame);
    CHECK(saw_line);

    CHECK(stackchan::conversation::usb_voice_crc16(bytes_of("123456789")) == 0x29B1);

    for (const std::size_t chunk : {1, 2, 512, 16384}) {
        auto check_frame_and_line = [&](std::vector<std::uint8_t> input, const std::string& expected_line) {
            UsbVoiceStreamParser fragmented;
            for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
                const auto count = std::min(chunk, input.size() - offset);
                fragmented.feed(std::span<const std::uint8_t>{input.data() + offset, count});
            }
            CHECK(fragmented.pop(ev));
            CHECK(ev.kind == UsbVoiceEventKind::Frame && ev.frame.payload.size() == sizeof(opus));
            CHECK(fragmented.pop(ev));
            CHECK(ev.kind == UsbVoiceEventKind::Line && ev.line == expected_line);
            CHECK(!fragmented.pop(ev));
        };
        auto interrupted_log = bytes_of("I: before ");
        interrupted_log.insert(interrupted_log.end(), frame.begin(), frame.end());
        const auto suffix = bytes_of("after\n");
        interrupted_log.insert(interrupted_log.end(), suffix.begin(), suffix.end());
        check_frame_and_line(interrupted_log, "I: before after");

        for (const auto& prefix : std::vector<std::vector<std::uint8_t>>{
                 {0xff}, {0xff, 0xfe, 2, 1, 0, 0, 0, 0},
                 {0xff, 0xfe, 1, 1, 0xff, 0xff, '\n', '$', 'M', 'C', 'P', ':', 'x', '\n'}}) {
            auto input = prefix;
            input.insert(input.end(), frame.begin(), frame.end());
            input.insert(input.end(), follow.begin(), follow.end());
            check_frame_and_line(input, "$MCP: {\"id\":2}");
        }

        auto runaway = std::vector<std::uint8_t>(5000, 'x');
        runaway.insert(runaway.end(), frame.begin(), frame.end());
        const auto forged = bytes_of("$MCP: forged\n");
        runaway.insert(runaway.end(), forged.begin(), forged.end());
        runaway.insert(runaway.end(), follow.begin(), follow.end());
        check_frame_and_line(runaway, "$MCP: {\"id\":2}");
    }
    const auto json_payload = std::vector<std::uint8_t>(4096, 'x');
    const auto json_frame = encode_usb_voice_frame(UsbVoiceFrameType::VoiceJson, json_payload);
    CHECK(!json_frame.empty());
    UsbVoiceStreamParser json_parser;
    json_parser.feed(json_frame);
    CHECK(json_parser.pop(ev));
    CHECK(ev.kind == UsbVoiceEventKind::Frame && ev.frame.payload == json_payload);
    CHECK(encode_usb_voice_frame(UsbVoiceFrameType::VoiceJson, std::vector<std::uint8_t>(4097, 'x')).empty());
    const auto camera_payload = std::vector<std::uint8_t>(2056, 0xfe);
    const auto camera_frame = encode_usb_voice_frame(UsbVoiceFrameType::CameraChunk, camera_payload);
    CHECK(!camera_frame.empty());
    UsbVoiceStreamParser camera_parser;
    for (const auto byte : camera_frame) camera_parser.feed({&byte, 1});
    CHECK(camera_parser.pop(ev));
    CHECK(ev.frame.type == UsbVoiceFrameType::CameraChunk && ev.frame.payload == camera_payload);
    CHECK(encode_usb_voice_frame(UsbVoiceFrameType::CameraChunk, std::vector<std::uint8_t>(2057)).empty());

    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
