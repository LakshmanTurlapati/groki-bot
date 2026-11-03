// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace stackchan::conversation {

// Non-owning bridge between the USB serial service (main/) and XiaoZhiClient.
// The serial service owns the port. The client registers a sink while a USB
// voice session is starting. Pointers are not owning; clear the sink before
// the client is destroyed.

class UsbVoicePort {
public:
    virtual ~UsbVoicePort() = default;
    virtual bool send_json(std::string_view json) = 0;
    virtual bool send_audio(std::span<const std::uint8_t> opus) = 0;
    // True after the host has asked for USB voice and the serial driver is up.
    virtual bool active() const = 0;
};

class UsbVoiceSink {
public:
    virtual ~UsbVoiceSink() = default;
    // Host sent voice/mode enable. Send the XiaoZhi hello on the USB link.
    virtual void on_usb_host_ready() = 0;
    virtual void on_usb_json(std::string_view json) = 0;
    virtual void on_usb_audio(std::span<const std::uint8_t> opus) = 0;
};

void usb_voice_set_port(UsbVoicePort* port);
UsbVoicePort* usb_voice_port();

// Latched by the serial service when the host sends voice/mode. The
// conversation task polls this so it can leave the Wi-Fi wait and, later,
// fall back to the WebSocket when the host clears the mode.
void usb_voice_set_requested(bool on);
bool usb_voice_requested();

// Bind the client. If the host already enabled voice mode, on_usb_host_ready
// runs once. A second bind for the same enable does not send a second hello.
void usb_voice_bind_sink(UsbVoiceSink* sink);
void usb_voice_clear_sink(UsbVoiceSink* sink);

// Host enabled voice mode. Delivers on_usb_host_ready at most once per enable
// (shared with bind, so start() and the serial task cannot both send hello).
void usb_voice_notify_host_ready();

void usb_voice_dispatch_json(std::string_view json);
void usb_voice_dispatch_audio(std::span<const std::uint8_t> opus);

} // namespace stackchan::conversation
