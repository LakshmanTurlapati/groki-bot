// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#include "led_runtime.hpp"
#include "test_support.hpp"

using stackchan::app::led_runtime::Frame;
using stackchan::app::led_runtime::Hold;
using stackchan::app::led_runtime::Phase;
using stackchan::app::led_runtime::Sample;
using stackchan::app::led_runtime::from_rgb;
using stackchan::app::led_runtime::kModeSolid;
using stackchan::app::led_runtime::render;
using stackchan::app::led_runtime::speech_level;
using stackchan::conversation::led_control::LedWire;
using stackchan::conversation::led_control::parse_led_wire;

Sample listening()
{
    return Sample{.gateway = Phase::Listen, .listen_r = 40, .listen_g = 40, .listen_b = 40};
}

int main()
{
    const auto cyan = from_rgb(0, 180, 180);
    CHECK(cyan.mode == kModeSolid);
    CHECK(cyan.brightness == 255);
    CHECK(cyan.color == 0x00b4b4u);

    const auto red = from_rgb(255, 0, 0);
    CHECK(red.mode == kModeSolid);
    CHECK(red.brightness == 255);
    CHECK(red.color == 0x00ff0000u);

    const auto nested = from_rgb(0x12, 0x34, 0x56);
    CHECK(nested.color == 0x00123456u);
    CHECK(nested.mode == 1);

    Hold hold{};
    const Frame startup = render(Sample{}, hold);
    CHECK(!startup.lit);
    CHECK(startup.shown == Phase::Off);
    CHECK(startup.r == 0 && startup.g == 0 && startup.b == 0);

    const Frame listen_early = render(listening(), hold);
    const Frame listen_late = render(Sample{.gateway = Phase::Listen, .now_ms = 550,
                                            .listen_r = 40, .listen_g = 40, .listen_b = 40},
                                     hold);
    CHECK(listen_early.lit && listen_early.shown == Phase::Listen);
    CHECK(listen_early.r == 40 && listen_early.g == 40 && listen_early.b == 40);
    CHECK(listen_late.r == listen_early.r && listen_late.g == listen_early.g && listen_late.b == listen_early.b);

    Sample speaking{};
    speaking.gateway = Phase::Speak;
    speaking.now_ms = 0;
    const Frame pulse_low = render(speaking, hold);
    speaking.now_ms = 550;
    const Frame pulse_high = render(speaking, hold);
    CHECK(pulse_low.shown == Phase::Speak && pulse_low.lit);
    CHECK(pulse_low.r == pulse_low.g && pulse_low.g == pulse_low.b);
    CHECK(pulse_high.r == pulse_high.g && pulse_high.b == pulse_high.r);
    CHECK(pulse_high.r > pulse_low.r);

    speaking.mouth = 1.0f;
    speaking.now_ms = 0;
    const Frame voiced = render(speaking, hold);
    CHECK(voiced.r > pulse_low.r);
    CHECK(voiced.r == speech_level(1.0f, 0));

    // A listen message during playback does not replace speech white.
    hold = {};
    Sample armed = speaking;
    armed.mouth = 0.0f;
    armed.playback = true;
    CHECK(render(armed, hold).shown == Phase::Speak);
    armed.gateway = Phase::Listen;
    const Frame tail = render(armed, hold);
    CHECK(tail.shown == Phase::Speak);
    CHECK(tail.r == tail.g && tail.b == tail.r);
    armed.playback = false;
    const Frame after = render(armed, hold);
    CHECK(after.shown == Phase::Listen);
    CHECK(after.r == 40);

    // The wake chime is playback without a speech phase: it stays on the listen colour.
    Hold chime{};
    Sample beep = listening();
    beep.playback = true;
    CHECK(render(beep, chime).shown == Phase::Listen);

    Sample asleep = speaking;
    asleep.sleeping = true;
    asleep.playback = true;
    Hold sleep_hold{};
    const Frame rest = render(asleep, sleep_hold);
    CHECK(!rest.lit && rest.shown == Phase::Off);

    Sample dropped = speaking;
    dropped.gateway = Phase::Off;
    dropped.playback = true;
    Hold drop_hold{};
    CHECK(!render(dropped, drop_hold).lit);

    const auto speech = parse_led_wire("speech", true, 0, 80, 180);
    CHECK(speech.command == LedWire::Speak);
    const auto legacy_blue = parse_led_wire(nullptr, true, 0, 80, 180);
    CHECK(legacy_blue.command == LedWire::Ignore);
    const auto legacy_off = parse_led_wire(nullptr, true, 0, 0, 0);
    CHECK(legacy_off.command == LedWire::Off);
    const auto listen = parse_led_wire("listen", false, 0, 0, 0);
    CHECK(listen.command == LedWire::Listen);
    CHECK(listen.r == 40 && listen.g == 40 && listen.b == 40);

    return motiontest::finish("led_runtime");
}
