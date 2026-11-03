// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "conversation/usb_voice_framer.hpp"

#include <algorithm>

namespace stackchan::conversation {

std::uint16_t usb_voice_crc16(std::span<const std::uint8_t> data) {
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

std::vector<std::uint8_t> encode_usb_voice_frame(UsbVoiceFrameType type, std::span<const std::uint8_t> payload) {
    const auto limit = type == UsbVoiceFrameType::VoiceJson     ? kUsbVoiceMaxJson
                       : type == UsbVoiceFrameType::CameraChunk ? kUsbCameraMaxPayload
                                                                : kUsbVoiceMaxPayload;
    if ((type != UsbVoiceFrameType::AudioUplink && type != UsbVoiceFrameType::AudioDownlink &&
         type != UsbVoiceFrameType::VoiceJson && type != UsbVoiceFrameType::CameraChunk) ||
        payload.size() > limit) {
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

void UsbVoiceStreamParser::reset() {
    buf_.clear();
    events_.clear();
    discard_text_ = false;
    text_.clear();
    drop_line_ = false;
}

void UsbVoiceStreamParser::feed(std::span<const std::uint8_t> bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    pump();
}

bool UsbVoiceStreamParser::pop(UsbVoiceEvent& out) {
    if (events_.empty()) {
        return false;
    }
    out = std::move(events_.front());
    events_.erase(events_.begin());
    return true;
}

void UsbVoiceStreamParser::pump() {
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
                const auto type = static_cast<UsbVoiceFrameType>(buf_[3]);
                const auto limit = type == UsbVoiceFrameType::VoiceJson     ? kUsbVoiceMaxJson
                                   : type == UsbVoiceFrameType::CameraChunk ? kUsbCameraMaxPayload
                                                                            : kUsbVoiceMaxPayload;
                if ((type != UsbVoiceFrameType::AudioUplink && type != UsbVoiceFrameType::AudioDownlink &&
                     type != UsbVoiceFrameType::VoiceJson && type != UsbVoiceFrameType::CameraChunk) ||
                    len > limit) {
                    // An invalid length has no trusted boundary. Do not let
                    // binary payload bytes become line-oriented commands.
                    buf_.erase(buf_.begin(), buf_.begin() + 6);
                    discard_text_ = true;
                    text_.clear();
                    drop_line_ = false;
                    continue;
                }
                const std::size_t total = 6 + static_cast<std::size_t>(len) + 2;
                if (buf_.size() < total) {
                    return;
                }
                if (buf_[2] != kUsbVoiceVersion) {
                    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));
                    continue;
                }
                const auto covered = std::span<const std::uint8_t>{buf_.data() + 2, 4 + static_cast<std::size_t>(len)};
                const auto expect = usb_voice_crc16(covered);
                const auto got =
                    static_cast<std::uint16_t>(buf_[6 + len] | (static_cast<std::uint16_t>(buf_[7 + len]) << 8));
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
                discard_text_ = false;
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));
                continue;
            }
        }

        const auto nl = std::find(buf_.begin(), buf_.end(), static_cast<std::uint8_t>('\n'));
        constexpr std::uint8_t sync[] = {kUsbVoiceSof0, kUsbVoiceSof1};
        const auto sof = std::search(buf_.begin(), buf_.end(), std::begin(sync), std::end(sync));
        if (sof != buf_.end() && (discard_text_ || sof < nl)) {
            // A console fragment or stray byte cannot hide the next frame.
            if (!discard_text_ && std::find(buf_.begin(), sof, kUsbVoiceSof0) == sof &&
                std::find(buf_.begin(), sof, kUsbVoiceSof1) == sof) {
                text_.insert(text_.end(), buf_.begin(), sof);
                if (text_.size() > kUsbVoiceMaxLine) {
                    text_.clear();
                    drop_line_ = true;
                }
            } else {
                text_.clear();
            }
            buf_.erase(buf_.begin(), sof);
            continue;
        }
        if (discard_text_) {
            const bool keep_sof = buf_.back() == kUsbVoiceSof0;
            buf_.clear();
            if (keep_sof)
                buf_.push_back(kUsbVoiceSof0);
            return;
        }
        if (nl == buf_.end()) {
            // Consume the scanned prefix so one-byte console reads stay linear.
            // A trailing 0xFF may be the first half of a binary sync word.
            const bool keep_sof = buf_.back() == kUsbVoiceSof0;
            const auto end = buf_.end() - (keep_sof ? 1 : 0);
            if (std::find(buf_.begin(), end, kUsbVoiceSof0) != end ||
                std::find(buf_.begin(), end, kUsbVoiceSof1) != end) {
                text_.clear();
            } else if (!drop_line_) {
                text_.insert(text_.end(), buf_.begin(), end);
                if (text_.size() > kUsbVoiceMaxLine) {
                    text_.clear();
                    drop_line_ = true;
                }
            }
            buf_.clear();
            if (keep_sof) {
                buf_.push_back(kUsbVoiceSof0);
            }
            return;
        }
        std::string line(text_.begin(), text_.end());
        line.append(buf_.begin(), nl);
        text_.clear();
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        buf_.erase(buf_.begin(), nl + 1);
        if (!line.empty() && !drop_line_ && line.size() <= kUsbVoiceMaxLine) {
            UsbVoiceEvent ev;
            ev.kind = UsbVoiceEventKind::Line;
            ev.line = std::move(line);
            events_.push_back(std::move(ev));
        }
        drop_line_ = false;
    }
}

} // namespace stackchan::conversation
