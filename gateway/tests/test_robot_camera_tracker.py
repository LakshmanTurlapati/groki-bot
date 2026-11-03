# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
import asyncio
import json
import time
from contextlib import asynccontextmanager
from types import SimpleNamespace

import aiohttp
from aiohttp import web
import pytest

from stackchan_mcp.robot_camera_tracker import (
    FRAME_BYTES, CameraBusy, CameraError, RobotCameraConfig, RobotCameraTracker,
    SwiftFrameDetector, camera_capture_url, capture_frame, parse_detection,
)

FRAME = bytes(FRAME_BYTES)
HEADERS = {
    "X-Frame-Width": "320", "X-Frame-Height": "240",
    "X-Frame-Format": "rgb565be", "X-Frame-Id": "capture-1",
    "Content-Type": "application/octet-stream",
}
FACE = {"x": 0.6, "y": 0.5, "width": 0.2, "height": 0.3, "confidence": 0.9}


@asynccontextmanager
async def camera_server(handler):
    app = web.Application()
    app.router.add_get("/api/camera/capture", handler)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        await runner.cleanup()


async def until(predicate, timeout=1.0):
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() > deadline:
            raise AssertionError("condition not reached")
        await asyncio.sleep(0.005)


@pytest.mark.parametrize("origin", [
    "http://user:secret@robot", "http://robot/path", "http://robot?password=secret",
    "http://robot#fragment", "file:///tmp/camera", "http://robot:invalid", "http://[bad",
])
def test_camera_override_rejects_credentials_and_non_origins_without_echoing_them(origin):
    with pytest.raises(ValueError) as error:
        camera_capture_url(origin)
    assert origin not in str(error.value)
    assert "secret" not in str(error.value)


def test_camera_origin_supports_ipv6_and_custom_ports():
    assert camera_capture_url("http://[::1]:8080/") == "http://[::1]:8080/api/camera/capture?tracking=1"


def test_config_repr_does_not_expose_password():
    assert "private-password" not in repr(RobotCameraConfig(password="private-password"))


@pytest.mark.parametrize("fps", [0, -1, 8.1, float("nan"), float("inf")])
def test_poll_rate_is_bounded(fps):
    with pytest.raises(ValueError):
        RobotCameraConfig(fps=fps)


@pytest.mark.asyncio
async def test_capture_uses_separate_basic_auth_and_validates_frame():
    async def handler(request):
        assert request.query == {"tracking": "1"}
        assert aiohttp.BasicAuth.decode(request.headers["Authorization"]).password == "camera-secret"
        return web.Response(body=FRAME, headers=HEADERS)

    async with camera_server(handler) as origin, aiohttp.ClientSession() as session:
        frame_id, frame = await capture_frame(session, camera_capture_url(origin), "camera-secret")
    assert frame_id == "capture-1" and frame == FRAME


@pytest.mark.parametrize("mutation", ["width", "format", "id", "short", "large", "type"])
@pytest.mark.asyncio
async def test_capture_rejects_malformed_or_unbounded_frames(mutation):
    headers = dict(HEADERS)
    body = FRAME
    if mutation == "width":
        headers["X-Frame-Width"] = "640"
    elif mutation == "format":
        headers["X-Frame-Format"] = "bayer8-rggb"
    elif mutation == "id":
        del headers["X-Frame-Id"]
    elif mutation == "short":
        body = FRAME[:-1]
    elif mutation == "large":
        body = FRAME + b"x"
    else:
        headers["Content-Type"] = "text/html"

    async def handler(request):
        response = web.StreamResponse(headers=headers)
        await response.prepare(request)
        await response.write(body)
        await response.write_eof()
        return response

    async with camera_server(handler) as origin, aiohttp.ClientSession() as session:
        with pytest.raises(CameraError, match="invalid_frame"):
            await capture_frame(session, camera_capture_url(origin), "")


@pytest.mark.parametrize(("status", "error"), [(401, "auth_required"), (503, "http_error"), (302, "http_error")])
@pytest.mark.asyncio
async def test_capture_errors_and_redirects_do_not_reveal_or_forward_credentials(status, error):
    calls = 0

    async def handler(request):
        nonlocal calls
        calls += 1
        return web.Response(status=status, headers={"Location": "/api/camera/capture"})

    async with camera_server(handler) as origin, aiohttp.ClientSession() as session:
        with pytest.raises(CameraError, match=error):
            await capture_frame(session, camera_capture_url(origin), "private-password")
    assert calls == 1


@pytest.mark.asyncio
async def test_busy_is_distinct_from_camera_failure():
    async def handler(request):
        return web.Response(status=409)

    async with camera_server(handler) as origin, aiohttp.ClientSession() as session:
        with pytest.raises(CameraBusy):
            await capture_frame(session, camera_capture_url(origin), "")


@pytest.mark.parametrize("line", [
    b"{}", b"not json", b'{"face":true}',
    json.dumps({"face": {**FACE, "x": float("nan")}}).encode(),
    json.dumps({"face": {**FACE, "width": 0}}).encode(),
    json.dumps({"face": {**FACE, "y": 2}}).encode(),
])
def test_worker_output_is_finite_and_bounded(line):
    with pytest.raises(CameraError, match="invalid_detection"):
        parse_detection(line)


def test_worker_output_includes_explicit_no_face():
    assert parse_detection(b'{"face":null}\n') is None
    assert parse_detection(json.dumps({"face": FACE}).encode()) == FACE


class FakeDetector:
    def __init__(self, face=None):
        self.face = face
        self.running = False
        self.child = SimpleNamespace(pid=123)
        self.frames = []
        self.starts = 0

    async def start(self):
        self.running = True
        self.starts += 1

    async def detect(self, frame):
        self.frames.append(frame)
        return self.face

    async def stop(self):
        self.running = False


def connection(session_id="session-1", capable=True):
    return SimpleNamespace(connected=True, camera_tracking_v1=capable, peer_host="127.0.0.1", session_id=session_id)


@pytest.mark.asyncio
async def test_no_face_reaches_callback_to_release_the_firmware_reservation(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    callbacks = []
    allowed = True

    async def detected(face, context):
        nonlocal allowed
        callbacks.append((face, context))
        allowed = False

    async def handler(request):
        return web.Response(body=FRAME, headers=HEADERS)

    manager = SimpleNamespace(connection=connection())
    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(manager, on_detection=detected, tracking_allowed=lambda: allowed,
                                     config=RobotCameraConfig(url_override=origin))
        tracker._detector = FakeDetector()
        tracker.start()
        try:
            await until(lambda: callbacks)
            assert callbacks[0][0] is None
            context = callbacks[0][1]
            assert context.frame_id == "capture-1" and context.connection is manager.connection
            assert context.deadline - context.started_at == 1
            assert tracker.available and tracker.running
            assert not tracker.face_reported
            assert tracker._detector.frames == [FRAME]
        finally:
            await tracker.stop()


@pytest.mark.parametrize("change", ["pause", "reconnect"])
@pytest.mark.asyncio
async def test_invalidation_is_immediate_and_drops_inflight_results(monkeypatch, change):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    entered, release = asyncio.Event(), asyncio.Event()
    callbacks, calls = [], []
    allowed = True

    async def detected(face, context):
        callbacks.append(context)

    async def handler(request):
        calls.append(request)
        entered.set()
        await release.wait()
        return web.Response(body=FRAME, headers=HEADERS)

    manager = SimpleNamespace(connection=connection())
    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(manager, on_detection=detected, tracking_allowed=lambda: allowed,
                                     config=RobotCameraConfig(url_override=origin))
        tracker._detector = FakeDetector(FACE)
        tracker.start()
        try:
            await asyncio.wait_for(entered.wait(), 1)
            if change == "pause":
                allowed = False
            else:
                manager.connection = connection("session-2")
            await asyncio.wait_for(tracker.invalidate(), 0.05)
            assert len(calls) == 1
            release.set()
            await until(lambda: tracker.status()["camera_state"] == "discarded_frame")
            assert callbacks == [] and tracker._detector.frames == []
        finally:
            release.set()
            await tracker.stop()


@pytest.mark.asyncio
async def test_invalidated_capture_failure_does_not_delay_recovery(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    entered, release = asyncio.Event(), asyncio.Event()
    calls = []
    allowed = True

    class Usb:
        async def capture_camera_frame(self, **kwargs):
            calls.append(time.monotonic())
            if len(calls) == 1:
                entered.set()
                await release.wait()
                raise asyncio.TimeoutError
            return "fresh-frame", FRAME

    async def detected(face, context):
        nonlocal allowed
        assert context.frame_id == "fresh-frame"
        allowed = False

    conn = connection()
    conn.transport = "usb"
    tracker = RobotCameraTracker(
        SimpleNamespace(connection=conn, usb_transport=Usb()),
        on_detection=detected, tracking_allowed=lambda: allowed,
        config=RobotCameraConfig(usb_encoding="rgb565be", initial_backoff_s=5),
    )
    tracker._detector = FakeDetector(FACE)
    tracker.start()
    try:
        await asyncio.wait_for(entered.wait(), 1)
        await tracker.invalidate()
        release.set()
        await until(lambda: not allowed)
        assert len(calls) == 2
        assert tracker.status()["camera_errors"] == 0
        assert tracker.status()["last_error"] is None
    finally:
        release.set()
        await tracker.stop()


@pytest.mark.asyncio
async def test_old_firmware_is_never_polled_even_with_override(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")

    async def detected(face, context):
        pytest.fail("Unsupported firmware must not produce detections")

    tracker = RobotCameraTracker(SimpleNamespace(connection=connection(capable=False)),
                                 on_detection=detected, tracking_allowed=lambda: True,
                                 config=RobotCameraConfig(url_override="http://127.0.0.1:1"))
    tracker._detector = FakeDetector()
    tracker.start()
    try:
        await until(lambda: tracker.status()["camera_state"] == "unsupported_firmware")
        assert tracker._detector.starts == 0 and tracker.status()["camera_errors"] == 0
    finally:
        await tracker.stop()


@pytest.mark.asyncio
async def test_swift_worker_receives_no_camera_credentials_and_is_reaped(monkeypatch, tmp_path):
    binary = tmp_path / "detector"
    binary.write_text("placeholder")
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_BIN", str(binary))
    monkeypatch.setenv("STACKCHAN_CAMERA_PASSWORD", "private-password")
    monkeypatch.setenv("STACKCHAN_CAMERA_URL", "http://private-robot")
    spawn_calls = []

    class Child:
        returncode = None
        pid = 12

        def terminate(self):
            self.returncode = 0

        async def wait(self):
            return self.returncode

    async def spawn(*args, **kwargs):
        spawn_calls.append((args, kwargs))
        return Child()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", spawn)
    detector = SwiftFrameDetector()
    await detector.start()
    args, kwargs = spawn_calls[0]
    assert args == (str(binary), "--stdin-frames")
    assert "STACKCHAN_CAMERA_PASSWORD" not in kwargs["env"]
    assert "STACKCHAN_CAMERA_URL" not in kwargs["env"]
    await detector.stop()
    assert not detector.running


@pytest.mark.asyncio
async def test_worker_timeout_reaps_unfinished_exchange():
    stopped = False

    class Writer:
        def write(self, data):
            assert len(data) == FRAME_BYTES + 8
            assert data[:8] == b"\x01\x00\x00\x00\x00\x58\x02\x00"

        async def drain(self):
            pass

    class Reader:
        async def readline(self):
            await asyncio.Event().wait()

    class Child:
        returncode = None
        stdin, stdout = Writer(), Reader()

        def terminate(self):
            nonlocal stopped
            stopped = True
            self.returncode = 0

        async def wait(self):
            return self.returncode

    detector = SwiftFrameDetector(timeout_s=0.01)
    detector.child = Child()
    with pytest.raises(asyncio.TimeoutError):
        await detector.detect(FRAME)
    assert stopped and detector.child is None


@pytest.mark.asyncio
async def test_late_capture_is_drained_without_detection(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    callbacks = []

    async def detected(face, context):
        callbacks.append(context)

    async def handler(request):
        await asyncio.sleep(0.02)
        return web.Response(body=FRAME, headers=HEADERS)

    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(SimpleNamespace(connection=connection()), on_detection=detected,
                                     tracking_allowed=lambda: True,
                                     config=RobotCameraConfig(url_override=origin, frame_max_age_s=0.01))
        tracker._detector = FakeDetector(FACE)
        tracker.start()
        try:
            await until(lambda: tracker.status()["camera_state"] == "stale_frame")
            assert callbacks == [] and tracker._detector.frames == []
        finally:
            await tracker.stop()


@pytest.mark.asyncio
async def test_pause_during_worker_launch_prevents_capture(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    starting, release = asyncio.Event(), asyncio.Event()
    calls = []
    allowed = True

    async def detected(face, context):
        pytest.fail("Paused tracker must not produce detections")

    async def handler(request):
        calls.append(request)
        return web.Response(body=FRAME, headers=HEADERS)

    class SlowStart(FakeDetector):
        async def start(self):
            starting.set()
            await release.wait()
            await super().start()

    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(SimpleNamespace(connection=connection()), on_detection=detected,
                                     tracking_allowed=lambda: allowed, config=RobotCameraConfig(url_override=origin))
        tracker._detector = SlowStart()
        tracker.start()
        try:
            await asyncio.wait_for(starting.wait(), 1)
            allowed = False
            await tracker.invalidate()
            release.set()
            await until(lambda: tracker.status()["camera_state"] == "paused")
            assert calls == []
        finally:
            release.set()
            await tracker.stop()


@pytest.mark.asyncio
async def test_capture_cycles_never_overlap_or_queue(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    entered, release, completed = asyncio.Event(), asyncio.Event(), asyncio.Event()
    calls = []
    allowed = True

    async def detected(face, context):
        nonlocal allowed
        allowed = False
        completed.set()

    async def handler(request):
        calls.append(request)
        entered.set()
        await release.wait()
        return web.Response(body=FRAME, headers=HEADERS)

    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(SimpleNamespace(connection=connection()), on_detection=detected,
                                     tracking_allowed=lambda: allowed,
                                     config=RobotCameraConfig(url_override=origin, fps=2))
        tracker._detector = FakeDetector()
        tracker.start()
        try:
            await asyncio.wait_for(entered.wait(), 1)
            await asyncio.sleep(0.55)
            assert len(calls) == 1
            release.set()
            await asyncio.wait_for(completed.wait(), 1)
            await until(lambda: tracker.status()["camera_state"] == "paused")
            assert len(calls) == 1 and len(tracker._detector.frames) == 1
        finally:
            release.set()
            await tracker.stop()


@pytest.mark.asyncio
async def test_capture_errors_back_off_and_success_clears_error(monkeypatch, caplog):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    calls = []
    allowed = True

    async def detected(face, context):
        nonlocal allowed
        allowed = False

    async def handler(request):
        calls.append(time.monotonic())
        if len(calls) <= 2:
            return web.Response(status=503, text="private-password")
        return web.Response(body=FRAME, headers=HEADERS)

    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(SimpleNamespace(connection=connection()), on_detection=detected,
                                     tracking_allowed=lambda: allowed,
                                     config=RobotCameraConfig(url_override=origin, password="private-password",
                                                              initial_backoff_s=0.02, max_backoff_s=0.04))
        tracker._detector = FakeDetector()
        tracker.start()
        try:
            await until(lambda: not allowed)
            assert len(calls) == 3
            assert calls[1] - calls[0] >= 0.018 and calls[2] - calls[1] >= 0.038
            assert tracker.status()["camera_errors"] == 2 and tracker.status()["last_error"] is None
            assert "private-password" not in caplog.text
        finally:
            await tracker.stop()


def test_camera_health_and_status_are_readable_without_event_loop():
    async def detected(face, context):
        pass

    tracker = RobotCameraTracker(SimpleNamespace(connection=connection()), on_detection=detected,
                                 tracking_allowed=lambda: True, config=RobotCameraConfig())
    tracker._detector = FakeDetector()
    tracker._detector.running = True
    tracker._last_frame_at = time.monotonic()
    assert tracker.available and not tracker.face_reported
    assert tracker.status()["frame_age_s"] is not None


@pytest.mark.asyncio
async def test_repeated_slow_frames_back_off_so_firmware_reservations_can_expire(monkeypatch):
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "1")
    calls, reservation_deadlines = [], []

    async def detected(face, context):
        pytest.fail("Stale captures must never produce head commands")

    async def handler(request):
        calls.append(time.monotonic())
        # Scale the firmware's finite reservation and transport times for this test.
        reservation_deadlines.append(calls[-1] + 0.025)
        await asyncio.sleep(0.01)
        return web.Response(body=FRAME, headers=HEADERS)

    async with camera_server(handler) as origin:
        tracker = RobotCameraTracker(SimpleNamespace(connection=connection()), on_detection=detected,
                                     tracking_allowed=lambda: True,
                                     config=RobotCameraConfig(url_override=origin, frame_max_age_s=0.005,
                                                              initial_backoff_s=0.03, max_backoff_s=0.06))
        tracker._detector = FakeDetector()
        tracker.start()
        try:
            await until(lambda: tracker.status()["camera_errors"] == 3)
            assert len(calls) == 3
            assert calls[1] > reservation_deadlines[0] and calls[2] > reservation_deadlines[1]
            assert calls[1] - calls[0] >= 0.038
            assert calls[2] - calls[1] >= 0.068
            assert tracker.status()["last_error"] == "stale_frame"
            assert tracker._detector.frames == []
        finally:
            await tracker.stop()
