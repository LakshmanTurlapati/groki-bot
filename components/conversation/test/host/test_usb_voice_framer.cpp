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
#include "conversation/usb_voice_port.hpp"

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

    struct Sink : stackchan::conversation::UsbVoiceSink {
        int ready = 0;
        void on_usb_host_ready() override { ++ready; }
        void on_usb_json(std::string_view) override {}
        void on_usb_audio(std::span<const std::uint8_t>) override {}
    };
    Sink sink;
    stackchan::conversation::usb_voice_notify_host_ready(); // no sink yet
    stackchan::conversation::usb_voice_bind_sink(&sink);
    CHECK(sink.ready == 1);
    stackchan::conversation::usb_voice_bind_sink(&sink); // same enable, no second hello
    CHECK(sink.ready == 1);
    stackchan::conversation::usb_voice_notify_host_ready();
    CHECK(sink.ready == 2);
    stackchan::conversation::usb_voice_set_requested(false);
    stackchan::conversation::usb_voice_bind_sink(&sink);
    CHECK(sink.ready == 2);
    stackchan::conversation::usb_voice_clear_sink(&sink);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
