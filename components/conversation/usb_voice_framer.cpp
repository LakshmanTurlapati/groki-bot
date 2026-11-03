// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "conversation/usb_voice_framer.hpp"

#include <algorithm>

namespace stackchan::conversation {

std::uint16_t usb_voice_crc16(std::span<const std::uint8_t> data)
{
    std::uint16_t crc = 0xFFFF;
    for (const std::uint8_t byte : data) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000) != 0) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

std::vector<std::uint8_t> encode_usb_voice_frame(UsbVoiceFrameType type,
                                                 std::span<const std::uint8_t> payload)
{
    if (payload.size() > kUsbVoiceMaxPayload) {
        return {};
    }
    const auto len = static_cast<std::uint16_t>(payload.size());
    std::vector<std::uint8_t> covered;
    covered.reserve(4 + payload.size());
    covered.push_back(kUsbVoiceVersion);
    covered.push_back(static_cast<std::uint8_t>(type));
    covered.push_back(static_cast<std::uint8_t>(len & 0xFF));
    covered.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    covered.insert(covered.end(), payload.begin(), payload.end());
    const std::uint16_t crc = usb_voice_crc16(covered);

    std::vector<std::uint8_t> out;
    out.reserve(6 + payload.size() + 2);
    out.push_back(kUsbVoiceSof0);
    out.push_back(kUsbVoiceSof1);
    out.insert(out.end(), covered.begin(), covered.end());
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    return out;
}

void UsbVoiceStreamParser::reset()
{
    buf_.clear();
    events_.clear();
}

void UsbVoiceStreamParser::feed(std::span<const std::uint8_t> bytes)
{
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    pump();
}

bool UsbVoiceStreamParser::pop(UsbVoiceEvent& out)
{
    if (events_.empty()) {
        return false;
    }
    out = std::move(events_.front());
    events_.erase(events_.begin());
    return true;
}

void UsbVoiceStreamParser::pump()
{
    while (!buf_.empty()) {
        if (buf_[0] == kUsbVoiceSof0) {
            if (buf_.size() < 2) {
                return;
            }
            if (buf_[1] != kUsbVoiceSof1) {
                // A lone 0xFF is text (or noise). Fall through to the line scanner
                // by treating this byte as the start of a line below.
            } else {
                if (buf_.size() < 6) {
                    return;
                }
                const auto len = static_cast<std::uint16_t>(buf_[4] | (static_cast<std::uint16_t>(buf_[5]) << 8));
                if (buf_[2] != kUsbVoiceVersion || len > kUsbVoiceMaxPayload) {
                    buf_.erase(buf_.begin());
                    continue;
                }
                const std::size_t total = 6 + static_cast<std::size_t>(len) + 2;
                if (buf_.size() < total) {
                    return;
                }
                const auto covered = std::span<const std::uint8_t>{buf_.data() + 2, 4 + static_cast<std::size_t>(len)};
                const auto expect = usb_voice_crc16(covered);
                const auto got = static_cast<std::uint16_t>(buf_[6 + len] | (static_cast<std::uint16_t>(buf_[7 + len]) << 8));
                if (got != expect) {
                    // Header was plausible (version and length). Drop the whole
                    // candidate so a flipped CRC cannot glue onto the next line.
                    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));
                    continue;
                }
                UsbVoiceEvent ev;
                ev.kind = UsbVoiceEventKind::Frame;
                ev.frame.type = static_cast<UsbVoiceFrameType>(buf_[3]);
                ev.frame.payload.assign(buf_.begin() + 6, buf_.begin() + 6 + len);
                events_.push_back(std::move(ev));
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));
                continue;
            }
        }

        const auto nl = std::find(buf_.begin(), buf_.end(), static_cast<std::uint8_t>('\n'));
        if (nl == buf_.end()) {
            if (buf_.size() > kUsbVoiceMaxLine) {
                // Keep a trailing SOF candidate so a frame that begins after a
                // runaway log line is not eaten.
                const bool keep_sof = buf_.back() == kUsbVoiceSof0;
                buf_.clear();
                if (keep_sof) {
                    buf_.push_back(kUsbVoiceSof0);
                }
            }
            return;
        }
        std::string line(buf_.begin(), nl);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        buf_.erase(buf_.begin(), nl + 1);
        if (!line.empty()) {
            UsbVoiceEvent ev;
            ev.kind = UsbVoiceEventKind::Line;
            ev.line = std::move(line);
            events_.push_back(std::move(ev));
        }
    }
}

} // namespace stackchan::conversation
