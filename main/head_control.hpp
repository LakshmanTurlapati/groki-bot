// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace stackchan::app {

enum class HeadSource { Idle, Manual, Tracking };
// Engaged: a conversation is on, so idle motion stays paused, but camera
// tracking keeps running. Conversation freezes both while the user talks.
enum class TrackingMode { Off, Tracking, Conversation, Engaged };

// All target writers share this lock. UART access remains exclusively in
// servo_task; HTTP and conversation only submit bounded mailbox requests.
class HeadControl {
public:
    struct Pose {
        float yaw;
        float pitch;
    };
    enum class Operation { Prepare, Exposure, Finalize, Apply };
    struct Request {
        std::uint32_t sequence = 0, deadline = 0, revision = 0, epoch = 0, captured_at = 0;
        Operation operation = Operation::Prepare;
        std::string frame_id;
        Pose delta{};
        bool release = false;
        bool motion = false;
    };
    struct Result {
        bool ok = false;
        std::string frame_id;
        bool unavailable = false;
        Pose measured{};
        bool motion = false;
        float capture_motion_deg = 0;
    };
    struct Action {
        Pose measured;
        Pose target;
        bool preserve_velocity = false;
        bool hold_yaw = false, hold_pitch = false;
    };
    struct Target {
        Pose pose;
        std::uint16_t speed;
        bool tracking;
    };

    std::atomic<std::uint32_t> head_hold_until_ms{0};
    std::atomic<bool> camera_available{false}, servo_available{false};
    std::atomic<bool> servo_service_started{false};

    static bool before(std::uint32_t now, std::uint32_t deadline) {
        return static_cast<std::int32_t>(now - deadline) < 0;
    }
    bool available() const {
        return camera_available.load() && servo_available.load();
    }
    // Advertise protocol support independently of the asynchronous first
    // servo probe. Capture requests still require healthy feedback.
    bool supports_tracking() const {
        return camera_available.load() && servo_service_started.load();
    }
    void set_boot_nonce(std::uint32_t nonce) {
        std::lock_guard lock(mutex_);
        nonce_ = nonce;
    }

    bool set_target(Pose pose, std::uint16_t speed, HeadSource source, std::uint32_t now) {
        if (!std::isfinite(pose.yaw) || !std::isfinite(pose.pitch))
            return false;
        std::lock_guard lock(mutex_);
        if (source == HeadSource::Idle && idle_paused_locked(now))
            return false;
        if (source == HeadSource::Manual) {
            invalidate_locked();
            cooldown_until_ = now + 2000;
            head_hold_until_ms.store(now + 2000);
            freeze_automatic_ = false;
        }
        store_target_locked(pose, speed, source);
        return true;
    }
    Pose target() const {
        std::lock_guard lock(mutex_);
        return target_pose_;
    }
    Target take_target() {
        std::lock_guard lock(mutex_);
        const auto speed = target_speed_;
        target_speed_ = 0;
        return {target_pose_, speed, source_ == HeadSource::Tracking};
    }
    void preempt_tracking(std::uint32_t now) {
        std::lock_guard lock(mutex_);
        invalidate_locked();
        cooldown_until_ = now + 2000;
        if (source_ == HeadSource::Tracking)
            freeze_automatic_ = true;
    }
    void set_tracking_mode(TrackingMode mode) {
        std::lock_guard lock(mutex_);
        if (mode_ == mode)
            return;
        mode_ = mode;
        ++epoch_;
        invalidate_locked();
        if (!tracks(mode) && source_ != HeadSource::Manual)
            freeze_automatic_ = true;
    }
    bool engaged() const {
        std::lock_guard lock(mutex_);
        return mode_ == TrackingMode::Engaged;
    }
    bool target_is_tracking() const {
        std::lock_guard lock(mutex_);
        return source_ == HeadSource::Tracking;
    }
    // The face fell asleep from idle decay: tracking and idle motion stop and
    // the head returns to rest. Manual commands still move it.
    void set_sleeping(bool sleeping) {
        std::lock_guard lock(mutex_);
        if (sleeping_ == sleeping)
            return;
        sleeping_ = sleeping;
        ++epoch_;
        invalidate_locked();
        if (!sleeping)
            return;
        head_hold_until_ms.store(0);
        freeze_automatic_ = false;
        store_target_locked({0.0f, 0.0f}, kRestSpeed, HeadSource::Idle);
    }
    bool sleeping() const {
        std::lock_guard lock(mutex_);
        return sleeping_;
    }
    void disconnect_tracking() {
        std::lock_guard lock(mutex_);
        mode_ = TrackingMode::Off;
        ++epoch_;
        invalidate_locked();
        if (source_ == HeadSource::Tracking)
            freeze_automatic_ = true;
    }
    bool freeze_automatic(Pose current) {
        std::lock_guard lock(mutex_);
        if (sleeping_)
            freeze_automatic_ = false; // Holding here would cancel the return to rest.
        if (!freeze_automatic_ || source_ == HeadSource::Manual)
            return false;
        freeze_automatic_ = false;
        store_target_locked(current, 0, HeadSource::Idle);
        return true;
    }
    bool idle_paused(std::uint32_t now) {
        std::lock_guard lock(mutex_);
        return idle_paused_locked(now);
    }

    bool tracking_holds(std::uint32_t now) {
        std::lock_guard lock(mutex_);
        expire_locked(now);
        return tracks(mode_) && source_ == HeadSource::Tracking && before(now, head_hold_until_ms.load());
    }

    std::optional<std::uint32_t> submit(Operation operation, std::uint32_t now, std::string frame_id = {},
                                        Pose delta = {}, bool release = false, std::uint32_t captured_at = 0,
                                        bool motion = false) {
        if (!std::isfinite(delta.yaw) || !std::isfinite(delta.pitch) || frame_id.size() > 32)
            return {};
        std::lock_guard lock(mutex_);
        if (!eligible_locked(now) || (request_ && before(now, request_->deadline)))
            return {};
        if (operation != Operation::Prepare && (frame_id.empty() || frame_id != frame_id_))
            return {};
        Request request;
        request.sequence = ++sequence_;
        request.deadline = now + 200;
        request.revision = revision_;
        request.epoch = epoch_;
        request.operation = operation;
        request.frame_id = std::move(frame_id);
        request.delta = {std::clamp(delta.yaw, -15.0f, 15.0f), std::clamp(delta.pitch, -10.0f, 10.0f)};
        request.release = release;
        request.captured_at = captured_at;
        request.motion = operation == Operation::Prepare ? motion : frame_motion_;
        request_ = std::move(request);
        taken_ = false;
        return sequence_;
    }
    std::optional<Request> take_request(std::uint32_t now) {
        std::lock_guard lock(mutex_);
        if (!request_ || taken_)
            return {};
        if (!before(now, request_->deadline)) {
            request_.reset();
            return {};
        }
        taken_ = true;
        return request_;
    }
    std::optional<Result> result(std::uint32_t sequence) const {
        std::lock_guard lock(mutex_);
        if (result_sequence_ != sequence)
            return {};
        return result_;
    }
    void cancel(std::uint32_t sequence) {
        std::lock_guard lock(mutex_);
        if (request_ && request_->sequence == sequence)
            request_.reset();
        if (result_sequence_ == sequence || reservation_sequence_ == sequence)
            invalidate_locked();
    }
    void abandon_frame(const std::string& frame_id) {
        std::lock_guard lock(mutex_);
        if (frame_id_ == frame_id)
            invalidate_locked();
    }

    // Called after feedback, with eligibility and the clock sampled again.
    // Returns a motion update only when a correction was actually accepted.
    std::optional<Action> finish(const Request& request, std::optional<Pose> measured, bool settled, bool allowed,
                                 std::uint32_t now, Pose minimum, Pose maximum, std::optional<Pose> expected = {}) {
        std::lock_guard lock(mutex_);
        if (!request_ || request_->sequence != request.sequence)
            return {};
        request_.reset();
        result_sequence_ = request.sequence;
        result_ = {};
        const bool valid = before(now, request.deadline) && request.epoch == epoch_ && request.revision == revision_ &&
                           eligible_locked(now) && allowed;
        if (!valid) {
            invalidate_frame_locked();
            return {};
        }
        if (request.operation == Operation::Apply && request.release) {
            if (request.frame_id == frame_id_) {
                invalidate_frame_locked();
                result_.ok = true;
            }
            return {};
        }
        if (!measured && (settled || request.motion)) {
            result_.unavailable = true;
            invalidate_frame_locked();
            return {};
        }
        const bool moving_tracking = request.motion && source_ == HeadSource::Tracking;
        const bool follows_model = expected && std::isfinite(expected->yaw) && std::isfinite(expected->pitch) &&
                                   measured && std::fabs(measured->yaw - expected->yaw) <= 4.5f &&
                                   std::fabs(measured->pitch - expected->pitch) <= 4.5f;
        if (!measured || (moving_tracking && !follows_model) || (!settled && !moving_tracking) ||
            !std::isfinite(measured->yaw) || !std::isfinite(measured->pitch) || measured->yaw < minimum.yaw ||
            measured->yaw > maximum.yaw || measured->pitch < minimum.pitch || measured->pitch > maximum.pitch) {
            invalidate_frame_locked();
            return {};
        }
        if (request.operation == Operation::Prepare) {
            char id[24];
            std::snprintf(id, sizeof(id), "%08lx%08lx", static_cast<unsigned long>(nonce_),
                          static_cast<unsigned long>(++frame_sequence_));
            frame_id_ = id;
            frame_pose_ = *measured;
            frame_motion_ = request.motion;
            exposure_ready_ = false;
            frame_revision_ = revision_;
            token_ready_ = false;
            reserved_until_ = now + 5000;
            reservation_sequence_ = request.sequence;
            result_ = {true, frame_id_, false, *measured, frame_motion_};
            return {};
        }
        const bool same_frame = request.frame_id == frame_id_ && frame_revision_ == revision_ &&
                                before(now, reserved_until_) &&
                                (frame_motion_ || (std::fabs(measured->yaw - frame_pose_.yaw) <= 2.0f &&
                                                   std::fabs(measured->pitch - frame_pose_.pitch) <= 2.0f));
        if (!same_frame) {
            invalidate_frame_locked();
            return {};
        }
        if (request.operation == Operation::Exposure) {
            if (!frame_motion_)
                return {};
            frame_pose_ = *measured;
            exposure_ready_ = true;
            result_ = {true, frame_id_, false, *measured, true};
            return {};
        }
        if (request.operation == Operation::Finalize) {
            float movement = 0;
            if (frame_motion_) {
                movement = std::max(std::fabs(measured->yaw - frame_pose_.yaw),
                                    std::fabs(measured->pitch - frame_pose_.pitch));
                if (!exposure_ready_ || movement > 3.0f) {
                    invalidate_frame_locked();
                    return {};
                }
                frame_pose_ = {(frame_pose_.yaw + measured->yaw) / 2, (frame_pose_.pitch + measured->pitch) / 2};
            }
            reserved_until_ = request.captured_at + (frame_motion_ ? 1200 : 2000);
            token_ready_ = before(now, reserved_until_);
            result_ = {token_ready_,  frame_id_, false, frame_motion_ ? frame_pose_ : *measured,
                       frame_motion_, movement};
            return {};
        }
        if (!token_ready_) {
            invalidate_frame_locked();
            return {};
        }
        const bool preserve = frame_motion_ && source_ == HeadSource::Tracking;
        // Zero correction is a request to hold the physical axis now. A
        // motion-compensated frame can predate movement during detection;
        // its old pose must not pull an already-centered axis backwards.
        const bool hold_yaw = request.delta.yaw == 0;
        const bool hold_pitch = request.delta.pitch == 0;
        const Pose origin{frame_motion_ && !hold_yaw ? frame_pose_.yaw : measured->yaw,
                          frame_motion_ && !hold_pitch ? frame_pose_.pitch : measured->pitch};
        const Pose target{
            std::clamp(std::clamp(origin.yaw + request.delta.yaw, measured->yaw - 15, measured->yaw + 15), minimum.yaw,
                       maximum.yaw),
            std::clamp(std::clamp(origin.pitch + request.delta.pitch, measured->pitch - 10, measured->pitch + 10),
                       minimum.pitch, maximum.pitch)};
        invalidate_frame_locked();
        store_target_locked(target, 900, HeadSource::Tracking);
        head_hold_until_ms.store(now + 2000);
        result_.ok = true;
        return Action{*measured, target, preserve, hold_yaw, hold_pitch};
    }

private:
    static constexpr std::uint16_t kRestSpeed = 200;

    static bool tracks(TrackingMode mode) {
        return mode == TrackingMode::Tracking || mode == TrackingMode::Engaged;
    }
    void expire_locked(std::uint32_t now) {
        if (cooldown_until_ != 0 && !before(now, cooldown_until_))
            cooldown_until_ = 0;
        if (reserved_until_ != 0 && !before(now, reserved_until_))
            invalidate_frame_locked();
        const auto hold = head_hold_until_ms.load();
        if (hold != 0 && !before(now, hold))
            head_hold_until_ms.store(0);
    }
    bool eligible_locked(std::uint32_t now) {
        expire_locked(now);
        return available() && tracks(mode_) && !sleeping_ && (cooldown_until_ == 0 || !before(now, cooldown_until_));
    }
    bool idle_paused_locked(std::uint32_t now) {
        expire_locked(now);
        const auto hold = head_hold_until_ms.load();
        return sleeping_ || mode_ == TrackingMode::Conversation || mode_ == TrackingMode::Engaged ||
               (reserved_until_ != 0 && before(now, reserved_until_)) || (hold != 0 && before(now, hold));
    }
    void store_target_locked(Pose pose, std::uint16_t speed, HeadSource source) {
        target_speed_ = speed;
        target_pose_ = pose;
        source_ = source;
        ++revision_;
    }
    void invalidate_frame_locked() {
        frame_id_.clear();
        reserved_until_ = 0;
        token_ready_ = false;
        frame_motion_ = exposure_ready_ = false;
    }
    void invalidate_locked() {
        invalidate_frame_locked();
        request_.reset();
    }

    mutable std::mutex mutex_;
    TrackingMode mode_ = TrackingMode::Off;
    HeadSource source_ = HeadSource::Idle;
    std::uint32_t revision_ = 0, epoch_ = 0, sequence_ = 0, nonce_ = 0, frame_sequence_ = 0;
    std::uint32_t cooldown_until_ = 0, reserved_until_ = 0, frame_revision_ = 0;
    std::uint32_t result_sequence_ = 0, reservation_sequence_ = 0;
    bool freeze_automatic_ = false, token_ready_ = false, taken_ = false, sleeping_ = false;
    bool frame_motion_ = false, exposure_ready_ = false;
    std::string frame_id_;
    Pose frame_pose_{};
    Pose target_pose_{};
    std::uint16_t target_speed_ = 0;
    std::optional<Request> request_;
    Result result_;
};

} // namespace stackchan::app
