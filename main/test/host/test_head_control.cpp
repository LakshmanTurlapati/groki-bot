// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#include "head_control.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace stackchan::app;
using Pose = HeadControl::Pose;
constexpr Pose minimum{-40, -10}, maximum{40, 25};
void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct Fixture {
    HeadControl control;
    std::uint32_t now = 10000;
    Fixture() {
        control.camera_available = true;
        control.servo_available = true;
        control.set_boot_nonce(0xabcdef12);
        control.set_tracking_mode(TrackingMode::Tracking);
    }
    std::optional<HeadControl::Action> complete(std::uint32_t sequence, std::optional<Pose> pose = Pose{20, 10},
                                                bool settled = true, bool allowed = true,
                                                std::optional<Pose> expected = {}) {
        auto request = control.take_request(now);
        check(request && request->sequence == sequence, "mailbox lost request");
        return control.finish(*request, pose, settled, allowed, now, minimum, maximum, expected);
    }
    std::string capture(Pose pose = {20, 10}) {
        auto sequence = control.submit(HeadControl::Operation::Prepare, now);
        check(sequence.has_value(), "prepare rejected");
        complete(*sequence, pose);
        auto prepared = control.result(*sequence);
        check(prepared && prepared->ok, "prepare failed");
        sequence = control.submit(HeadControl::Operation::Finalize, now, prepared->frame_id, {}, false, now);
        check(sequence.has_value(), "finalize rejected");
        complete(*sequence, pose);
        check(control.result(*sequence)->ok, "finalize failed");
        return prepared->frame_id;
    }
};

int main() {
    {
        HeadControl control;
        check(!control.supports_tracking(), "unsupported board advertised tracking");
        control.camera_available = true;
        check(!control.supports_tracking(), "unstarted servo service advertised tracking");
        control.servo_service_started = true;
        check(control.supports_tracking() && !control.available(),
              "capability waited for the asynchronous first servo probe");
        control.servo_available = true;
        check(control.supports_tracking() && control.available(), "successful feedback did not enable capture");
        control.servo_available = false;
        check(control.supports_tracking() && !control.available(), "feedback health changed the protocol capability");
    }
    {
        Fixture f;
        const auto id = f.capture();
        check(id.size() == 16, "frame id must be opaque 16 digit hex");
        check(f.control.idle_paused(f.now), "capture did not reserve idle motion");
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {0, 0});
        check(seq.has_value(), "centered frame rejected");
        auto action = f.complete(*seq);
        check(action && action->target.yaw == 20 && action->target.pitch == 10, "centered face reset nonzero pose");
        check(f.control.target().yaw == 20, "tracking failed to rebase stale commanded target");
        check(f.control.tracking_holds(f.now), "tracking released motor holding torque between frames");
        check(f.control.take_target().speed == 900, "tracking reused the slow idle motion profile");
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {6, 4}), "token replay accepted");
        check(f.control.idle_paused(f.now + 1999) && !f.control.idle_paused(f.now + 2000), "face hold does not expire");
    }
    {
        Fixture f;
        auto id = f.capture({39, 24});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {1000, 1000});
        auto action = f.complete(*seq, Pose{39, 24});
        check(action && action->target.yaw == 40 && action->target.pitch == 25, "configured limits not honored");
    }
    {
        Fixture f;
        auto id = f.capture({0, 0});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {1000, -1000});
        auto action = f.complete(*seq, Pose{0, 0});
        check(action && action->target.yaw == 15 && action->target.pitch == -10, "per-frame delta is unbounded");
    }
    {
        Fixture f;
        auto id = f.capture();
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1});
        check(!f.complete(*seq, Pose{25, 10}), "physical movement did not invalidate image");
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1}), "rejected token was not consumed");
    }
    {
        Fixture f;
        auto id = f.capture();
        f.now += 2000;
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1});
        check(!seq || !f.complete(*seq), "expired frame applied");
    }
    {
        Fixture f;
        auto id = f.capture();
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1});
        auto request = f.control.take_request(f.now);
        check(seq && request, "manual-race request missing");
        f.control.set_target({-10, 5}, 400, HeadSource::Manual, f.now);
        check(!f.control.finish(*request, Pose{20, 10}, true, true, f.now, minimum, maximum),
              "late tracking overwrote manual head");
        check(f.control.target().yaw == -10, "manual pose lost");
        check(!f.control.submit(HeadControl::Operation::Prepare, f.now + 1999), "manual cooldown missing");
        check(f.control.submit(HeadControl::Operation::Prepare, f.now + 2000).has_value(),
              "manual cooldown never expires");
    }
    {
        Fixture f;
        auto id = f.capture();
        check(!f.control.set_target({0, 0}, 200, HeadSource::Idle, f.now), "idle replaced captured pose");
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {}, true);
        f.complete(*seq, std::nullopt);
        check(!f.control.idle_paused(f.now), "no-face release kept idle reserved");
    }
    {
        Fixture f;
        f.control.set_target({30, 20}, 200, HeadSource::Idle, f.now);
        auto id = f.capture();
        f.control.head_hold_until_ms = f.now + 2000;
        f.control.set_tracking_mode(TrackingMode::Conversation);
        f.now += 100000;
        check(f.control.idle_paused(f.now), "conversation idle pause expired");
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1}), "conversation accepted tracking");
        check(f.control.freeze_automatic({15, 10}), "conversation did not freeze automatic spring");
        check(f.control.target().yaw == 15, "automatic freeze reset to neutral");
        f.control.set_target({-10, 5}, 300, HeadSource::Manual, f.now);
        check(!f.control.freeze_automatic({15, 10}), "conversation canceled explicit head movement");
        f.control.head_hold_until_ms = f.now + 2000;
        f.control.set_tracking_mode(TrackingMode::Off);
        check(f.control.idle_paused(f.now + 100), "mode change released existing head hold");
        check(!f.control.idle_paused(f.now + 3000), "off mode permanently froze idle");
    }
    {
        Fixture f;
        auto id = f.capture();
        f.control.disconnect_tracking();
        f.control.set_tracking_mode(TrackingMode::Tracking);
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {}), "old connection frame survived");
        const auto newer = f.capture();
        check(newer != id, "new connection reused frame token");
    }
    {
        Fixture f;
        auto seq = f.control.submit(HeadControl::Operation::Prepare, f.now);
        auto request = f.control.take_request(f.now);
        check(!f.control.submit(HeadControl::Operation::Prepare, f.now), "mailbox allowed concurrent requests");
        f.control.cancel(*seq);
        f.control.finish(*request, Pose{20, 10}, true, true, f.now, minimum, maximum);
        check(!f.control.idle_paused(f.now), "cancelled request resurrected reservation");
        seq = f.control.submit(HeadControl::Operation::Prepare, f.now);
        f.complete(*seq, std::nullopt);
        check(!f.control.result(*seq)->ok && f.control.result(*seq)->unavailable,
              "missing servo feedback was not distinguished from busy motion");
        seq = f.control.submit(HeadControl::Operation::Prepare, f.now);
        f.complete(*seq, Pose{20, 10}, false);
        check(!f.control.result(*seq)->ok, "moving spring accepted capture");
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, "bogus",
                                {std::numeric_limits<float>::infinity(), 0}),
              "infinite correction accepted");
    }
    {
        Fixture f;
        auto id = f.capture();
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {3, 2});
        check(seq && f.complete(*seq), "touch test could not start automatic motion");
        f.control.preempt_tracking(f.now);
        check(f.control.freeze_automatic({21, 11}), "light touch did not stop accepted tracking motion");
        f.control.set_target({-10, 5}, 300, HeadSource::Manual, f.now);
        f.control.preempt_tracking(f.now);
        check(!f.control.freeze_automatic({21, 11}), "touch canceled an explicit manual target");
    }
    {
        Fixture f;
        f.now = 0x90000000;
        check(!f.control.idle_paused(f.now), "inactive deadlines look active after 25 days");
        auto id = f.capture();
        check(!id.empty(), "tracking cooldown stuck after 25 days");
    }
    {
        Fixture f;
        f.now = 0xffffff00;
        auto id = f.capture();
        f.now += 1000;
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1});
        check(seq && f.complete(*seq), "valid frame rejected across clock wrap");
    }
    {
        Fixture f;
        f.control.set_target({-10, 5}, 300, HeadSource::Manual, f.now);
        check(!f.control.set_target({0, 0}, 200, HeadSource::Idle, f.now), "idle raced manual hold publication");
        auto command = f.control.take_target();
        check(command.pose.yaw == -10 && command.pose.pitch == 5 && command.speed == 300,
              "target axes/speed were not sampled coherently");
        check(f.control.take_target().speed == 0, "one-shot speed hint persisted");
        check(!f.control.idle_paused(f.now + 2001), "expired manual hold did not clear");
        check(!f.control.idle_paused(f.now + 0x90000000), "expired nonzero deadline became active again");
    }
    {
        Fixture f;
        auto id = f.capture({0, 0});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {15, 5});
        check(seq && f.complete(*seq, Pose{0, 0}), "could not start motion-aware test");
        seq = f.control.submit(HeadControl::Operation::Prepare, f.now, {}, {}, false, 0, true);
        f.complete(*seq, Pose{5, 2}, false, true, Pose{5, 2});
        id = f.control.result(*seq)->frame_id;
        check(f.control.result(*seq)->ok, "automatic movement prevented a motion-aware capture");
        seq = f.control.submit(HeadControl::Operation::Exposure, f.now, id);
        f.complete(*seq, Pose{10, 3}, false, true, Pose{10, 3});
        seq = f.control.submit(HeadControl::Operation::Finalize, f.now, id, {}, false, f.now);
        f.complete(*seq, Pose{12, 4}, false, true, Pose{12, 4});
        const auto frame = f.control.result(*seq);
        check(frame->ok && frame->motion && frame->measured.yaw == 11 && frame->measured.pitch == 3.5f,
              "frame was not based on the exposure's measured midpoint");
        seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {6, 2});
        auto action = f.complete(*seq, Pose{18, 6}, false, true, Pose{18, 6});
        check(action && action->target.yaw == 17 && action->target.pitch == 5.5f && action->preserve_velocity,
              "new correction ignored camera motion or reset the moving trajectory");
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {6, 2}), "motion token replay accepted");
    }
    for (const Pose delta : {Pose{0, 0}, Pose{0, 2}, Pose{6, 0}}) {
        Fixture f;
        auto id = f.capture({0, 0});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {15, 5});
        f.complete(*seq, Pose{0, 0});
        seq = f.control.submit(HeadControl::Operation::Prepare, f.now, {}, {}, false, 0, true);
        f.complete(*seq, Pose{5, 2}, false, true, Pose{5, 2});
        id = f.control.result(*seq)->frame_id;
        seq = f.control.submit(HeadControl::Operation::Exposure, f.now, id);
        f.complete(*seq, Pose{10, 3}, false, true, Pose{10, 3});
        seq = f.control.submit(HeadControl::Operation::Finalize, f.now, id, {}, false, f.now);
        f.complete(*seq, Pose{12, 4}, false, true, Pose{12, 4});
        seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, delta);
        const auto action = f.complete(*seq, Pose{18, 6}, false, true, Pose{18, 6});
        check(action.has_value(), "centered motion token rejected");
        check(action->target.yaw == (delta.yaw == 0 ? 18 : 17) && action->target.pitch == (delta.pitch == 0 ? 6 : 5.5f),
              "centered axis snapped back to the exposure pose");
        check(action->hold_yaw == (delta.yaw == 0) && action->hold_pitch == (delta.pitch == 0),
              "centered axis retained its previous trajectory velocity");
    }
    for (const bool blur : {false, true}) {
        Fixture f;
        auto id = f.capture({0, 0});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {15, 5});
        f.complete(*seq, Pose{0, 0});
        seq = f.control.submit(HeadControl::Operation::Prepare, f.now, {}, {}, false, 0, true);
        f.complete(*seq, Pose{5, 2}, false, true, Pose{5, 2});
        id = f.control.result(*seq)->frame_id;
        seq = f.control.submit(HeadControl::Operation::Exposure, f.now, id);
        f.complete(*seq, Pose{5, 2}, false, true, Pose{5, 2});
        seq = f.control.submit(HeadControl::Operation::Finalize, f.now, id, {}, false, f.now);
        f.complete(*seq, Pose{10, 2}, false, true, blur ? Pose{10, 2} : Pose{0, 2});
        check(!f.control.result(*seq)->ok,
              blur ? "excessive exposure movement was accepted" : "uncommanded physical movement was accepted");
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id), "rejected motion token remained live");
    }
    {
        Fixture f;
        f.control.set_tracking_mode(TrackingMode::Engaged);
        check(f.control.engaged(), "engaged mode not reported");
        check(f.control.idle_paused(f.now), "engaged mode allowed idle motion");
        auto id = f.capture({0, 0});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {5, 2});
        check(f.complete(*seq, Pose{0, 0}).has_value(), "engaged mode refused a correction");
        check(f.control.target_is_tracking(), "engaged correction lost its tracking source");
        check(f.control.tracking_holds(f.now), "engaged mode did not hold the tracked pose");
        f.control.set_tracking_mode(TrackingMode::Tracking);
        check(!f.control.freeze_automatic({1, 1}), "tracking to engaged transition froze the head");
        f.control.set_tracking_mode(TrackingMode::Engaged);
        check(!f.control.freeze_automatic({1, 1}), "engaged transition froze the head");
        f.control.set_tracking_mode(TrackingMode::Conversation);
        check(!f.control.engaged() && f.control.freeze_automatic({3, 4}), "user speech did not freeze the head");
        check(f.control.target().yaw == 3 && !f.control.target_is_tracking(), "user speech left a tracking target");
    }
    {
        Fixture f;
        auto id = f.capture({0, 0});
        auto seq = f.control.submit(HeadControl::Operation::Apply, f.now, id, {10, 5});
        f.complete(*seq, Pose{0, 0});
        f.control.set_sleeping(true);
        check(f.control.sleeping(), "sleep not reported");
        check(f.control.target().yaw == 0 && f.control.target().pitch == 0, "sleep did not target the rest pose");
        check(!f.control.target_is_tracking(), "rest pose kept the tracking source");
        check(f.control.take_target().speed == 200, "rest pose lost its gentle speed");
        check(!f.control.submit(HeadControl::Operation::Prepare, f.now), "sleep accepted a capture");
        check(f.control.idle_paused(f.now + 100000), "sleep allowed idle motion");
        check(!f.control.set_target({9, 9}, 200, HeadSource::Idle, f.now), "sleep accepted an idle pose");
        f.control.set_tracking_mode(TrackingMode::Conversation);
        check(!f.control.freeze_automatic({7, 7}), "mode change cancelled the return to rest");
        check(f.control.target().yaw == 0, "freeze moved the rest target");
        f.control.set_tracking_mode(TrackingMode::Tracking);
        f.control.set_sleeping(false);
        check(!f.control.sleeping() && !f.control.idle_paused(f.now), "waking left the head paused");
        f.capture({0, 0});
    }
    {
        Fixture f;
        auto id = f.capture({0, 0});
        f.control.set_sleeping(true);
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1}), "sleep kept a live frame token");
        f.control.set_sleeping(false);
        check(!f.control.submit(HeadControl::Operation::Apply, f.now, id, {1, 1}), "waking revived a stale frame");
        f.control.set_target({6, 6}, 200, HeadSource::Manual, f.now);
        f.control.set_sleeping(true);
        f.control.set_target({-6, -6}, 200, HeadSource::Manual, f.now);
        check(f.control.target().yaw == -6, "sleep blocked an explicit head command");
    }
    std::cout << "Head tracking control checks passed\n";
}
