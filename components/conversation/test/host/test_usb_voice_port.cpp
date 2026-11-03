// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "conversation/usb_voice_port.hpp"

namespace conv = stackchan::conversation;

class Sink : public conv::UsbVoiceSink {
public:
    void on_usb_host_ready() override {
        ++hellos;
    }
    void on_usb_json(std::string_view) override {
        ++messages;
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this] { return !block; });
    }
    void on_usb_audio(std::span<const std::uint8_t>) override {
        ++audio;
    }

    std::atomic<unsigned> hellos{0}, messages{0}, audio{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false, entered = false;
};

int main() {
    Sink first, replacement;
    const std::uint8_t audio[] = {1};
    conv::usb_voice_set_requested(false);
    conv::usb_voice_bind_sink(&first);
    assert(first.hellos == 0);

    const auto initial_generation = conv::usb_voice_generation();
    conv::usb_voice_notify_host_ready();
    assert(conv::usb_voice_requested());
    assert(conv::usb_voice_generation() != initial_generation);
    conv::usb_voice_dispatch_json("old session");
    assert(first.messages == 0);
    conv::usb_voice_bind_sink(&first);
    conv::usb_voice_bind_sink(&first);
    assert(first.hellos == 1);
    conv::usb_voice_dispatch_json("hello");
    conv::usb_voice_dispatch_audio(audio);
    assert(first.messages == 1 && first.audio == 1);

    // Disable/re-enable within one polling interval still invalidates the sink.
    conv::usb_voice_set_requested(false);
    conv::usb_voice_dispatch_json("disabled");
    conv::usb_voice_dispatch_audio(audio);
    conv::usb_voice_notify_host_ready();
    conv::usb_voice_dispatch_json("awaiting replacement");
    assert(first.messages == 1 && first.audio == 1);
    conv::usb_voice_bind_sink(&replacement);
    assert(replacement.hellos == 1);
    conv::usb_voice_clear_sink(&first);
    conv::usb_voice_dispatch_json("new session");
    assert(replacement.messages == 1);

    // Reconnecting a client in the same host lease must repeat its handshake.
    conv::usb_voice_clear_sink(&replacement);
    conv::usb_voice_bind_sink(&replacement);
    assert(replacement.hellos == 2);

    // Teardown cannot free a sink while its callback is still using it.
    {
        std::lock_guard lock(replacement.mutex);
        replacement.block = true;
        replacement.entered = false;
    }
    std::thread dispatch([&] { conv::usb_voice_dispatch_json("in flight"); });
    {
        std::unique_lock lock(replacement.mutex);
        replacement.changed.wait(lock, [&] { return replacement.entered; });
    }
    std::atomic<bool> clearing{false}, cleared{false};
    std::thread clear([&] {
        clearing = true;
        conv::usb_voice_clear_sink(&replacement);
        cleared = true;
    });
    while (!clearing)
        std::this_thread::yield();
    assert(!cleared);
    {
        std::lock_guard lock(replacement.mutex);
        replacement.block = false;
        replacement.changed.notify_all();
    }
    dispatch.join();
    clear.join();
    assert(cleared);
    conv::usb_voice_dispatch_json("after teardown");
    assert(replacement.messages == 2);
    conv::usb_voice_set_requested(false);
}
