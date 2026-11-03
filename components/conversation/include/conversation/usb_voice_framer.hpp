// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace stackchan::conversation {

// Byte stream shared with line-oriented JSON-RPC (`$MCP: ...\n`) and ESP_LOG
// lines on the ESP32-S3 USB Serial/JTAG console.
//
// Text stays newline-delimited. Opus packets and XiaoZhi JSON (hello, listen,
// tts, llm, led, head) ride in a length-prefixed frame whose sync word is
// 0xFF 0xFE — bytes that never appear in well-formed UTF-8 log lines — so a
// payload may contain newlines and the `$MCP:` prefix without splitting a
// tool-call line.
//
//   0xFF 0xFE | ver u8=1 | type u8 | len u16 LE | payload | crc16 LE
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) covers ver|type|len|payload.
//
// Throughput: the device encoder is 24 kbit/s Opus, 60 ms frames (~180 B).
// Gateway downlink is the same 60 ms grid at 24 kHz, typically a few hundred
// bytes. One direction is about 3–8 KiB/s; the conversation task is
// half-duplex, so both directions are not saturated together. USB Serial/JTAG
// ignores the host baud setting (115200 is only a pyserial placeholder) and
// the link is USB full-speed, which has room for these frames. A payload
// larger than an Opus packet (1275 B, RFC 6716) is rejected.

inline constexpr std::uint8_t kUsbVoiceSof0 = 0xFF;
inline constexpr std::uint8_t kUsbVoiceSof1 = 0xFE;
inline constexpr std::uint8_t kUsbVoiceVersion = 1;
inline constexpr std::uint16_t kUsbVoiceMaxPayload = 1275;
inline constexpr std::size_t kUsbVoiceMaxLine = 4096;

enum class UsbVoiceFrameType : std::uint8_t {
    AudioUplink = 1,   // device -> host, raw Opus (same bytes as a WS binary frame)
    AudioDownlink = 2, // host -> device, raw Opus
    VoiceJson = 3,     // either direction, compact UTF-8 JSON
};

struct UsbVoiceFrame {
    UsbVoiceFrameType type{UsbVoiceFrameType::VoiceJson};
    std::vector<std::uint8_t> payload;
};

enum class UsbVoiceEventKind : std::uint8_t { Line, Frame };

struct UsbVoiceEvent {
    UsbVoiceEventKind kind{UsbVoiceEventKind::Line};
    std::string line; // without the trailing newline
    UsbVoiceFrame frame;
};

std::uint16_t usb_voice_crc16(std::span<const std::uint8_t> data);

// Empty when `payload` exceeds kUsbVoiceMaxPayload.
std::vector<std::uint8_t> encode_usb_voice_frame(UsbVoiceFrameType type,
                                                 std::span<const std::uint8_t> payload);

// Incremental demux. Feed arbitrary chunks; pop events in arrival order.
class UsbVoiceStreamParser {
public:
    void feed(std::span<const std::uint8_t> bytes);
    bool pop(UsbVoiceEvent& out);
    void reset();

private:
    void pump();

    std::vector<std::uint8_t> buf_;
    std::vector<UsbVoiceEvent> events_;
};

} // namespace stackchan::conversation
