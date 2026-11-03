// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "conversation/usb_voice_port.hpp"

#include <mutex>

namespace stackchan::conversation {
namespace {

std::mutex g_mu;
std::mutex g_cb_mu;
UsbVoicePort* g_port = nullptr;
UsbVoiceSink* g_sink = nullptr;
bool g_requested = false;
std::uint32_t g_ready_gen = 0;
std::uint32_t g_delivered_gen = 0;

UsbVoiceSink* claim_ready_locked()
{
    if (!g_requested || g_sink == nullptr) {
        return nullptr;
    }
    if (g_delivered_gen == g_ready_gen) {
        return nullptr;
    }
    g_delivered_gen = g_ready_gen;
    return g_sink;
}

void deliver_ready(UsbVoiceSink* sink)
{
    if (sink == nullptr) {
        return;
    }
    std::lock_guard cb{g_cb_mu};
    {
        std::lock_guard lock{g_mu};
        if (g_sink != sink) {
            return;
        }
    }
    sink->on_usb_host_ready();
}

} // namespace

void usb_voice_set_port(UsbVoicePort* port)
{
    std::lock_guard lock{g_mu};
    g_port = port;
}

UsbVoicePort* usb_voice_port()
{
    std::lock_guard lock{g_mu};
    return g_port;
}

void usb_voice_set_requested(bool on)
{
    std::lock_guard lock{g_mu};
    g_requested = on;
    if (!on) {
        // A late bind must not hello for a mode the host already cleared.
        g_delivered_gen = g_ready_gen;
    }
}

bool usb_voice_requested()
{
    std::lock_guard lock{g_mu};
    return g_requested;
}

void usb_voice_bind_sink(UsbVoiceSink* sink)
{
    UsbVoiceSink* notify = nullptr;
    {
        std::lock_guard lock{g_mu};
        g_sink = sink;
        notify = claim_ready_locked();
    }
    deliver_ready(notify);
}

void usb_voice_clear_sink(UsbVoiceSink* sink)
{
    // Wait until an in-flight host callback returns before the client is freed.
    std::lock_guard cb{g_cb_mu};
    std::lock_guard lock{g_mu};
    if (g_sink == sink) {
        g_sink = nullptr;
    }
}

void usb_voice_notify_host_ready()
{
    UsbVoiceSink* notify = nullptr;
    {
        std::lock_guard lock{g_mu};
        ++g_ready_gen;
        g_requested = true;
        notify = claim_ready_locked();
    }
    deliver_ready(notify);
}

void usb_voice_dispatch_json(std::string_view json)
{
    std::lock_guard cb{g_cb_mu};
    UsbVoiceSink* sink = nullptr;
    {
        std::lock_guard lock{g_mu};
        sink = g_sink;
    }
    if (sink != nullptr) {
        sink->on_usb_json(json);
    }
}

void usb_voice_dispatch_audio(std::span<const std::uint8_t> opus)
{
    std::lock_guard cb{g_cb_mu};
    UsbVoiceSink* sink = nullptr;
    {
        std::lock_guard lock{g_mu};
        sink = g_sink;
    }
    if (sink != nullptr) {
        sink->on_usb_audio(opus);
    }
}

} // namespace stackchan::conversation
