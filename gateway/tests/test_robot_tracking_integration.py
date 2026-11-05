# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Robot tracking uses the current camera session and pauses before audio."""

import asyncio
import json
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
import websockets

from stackchan_mcp.debug_status import DebugStatus
from stackchan_mcp.esp32_client import ESP32Connection, ESP32Manager
from stackchan_mcp.gateway import Gateway


@pytest.fixture
def gateway(monkeypatch):
    monkeypatch.delenv("STACKCHAN_FACE_TRACKER_SOURCE", raising=False)
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "0")
    monkeypatch.setenv("STACKCHAN_USB_DISABLE", "1")
    status = DebugStatus()
    monkeypatch.setattr("stackchan_mcp.gateway.get_debug_status", lambda: status)
    gw = Gateway()
    gw.esp32._debug_status = status
    proxy = SimpleNamespace(conversation_active=False, is_listening=False)
    ws = SimpleNamespace(send=AsyncMock(), remote_address=("192.0.2.8", 5555))
    conn = ESP32Connection(ws, "session-a")
    conn.camera_tracking_v1 = True
    conn.cloud_proxy = proxy
    gw.esp32._connection = conn
    return gw, conn, proxy, ws, status


def context_for(connection):
    now = asyncio.get_running_loop().time()
    return SimpleNamespace(connection=connection, session_id=connection.session_id,
                           frame_id="opaque-frame-token", started_at=now, deadline=now + 1)


@pytest.mark.asyncio
async def test_default_source_is_robot_and_modes_follow_conversation(gateway):
    gw, conn, proxy, ws, _ = gateway
    assert gw.tracking_source == "robot"
    await gw._tracking_connection_changed()
    assert gw._robot_tracking_allowed()
    proxy.conversation_active = True
    await gw._on_conversation_activity(proxy, True)
    assert not gw._robot_tracking_allowed()
    proxy.conversation_active = False
    await gw._on_conversation_activity(proxy, False)
    await gw.stop_face_tracking()
    messages = [json.loads(c.args[0]) for c in ws.send.call_args_list]
    assert [m["mode"] for m in messages] == ["tracking", "conversation", "tracking", "off"]
    assert gw.face_tracking_status()["pause_reason"] == "head_follow_disabled"


@pytest.mark.asyncio
async def test_local_tts_pauses_before_start_envelope_and_resumes_after_stop(gateway):
    gw, conn, _, ws, _ = gateway
    await gw._tracking_connection_changed()
    ws.send.reset_mock()
    await gw.esp32.send_tts_state("start")
    await gw.esp32.send_tts_state("stop")
    messages = [json.loads(c.args[0]) for c in ws.send.call_args_list]
    assert [(m["type"], m.get("mode", m.get("state"))) for m in messages] == [
        ("head_tracking", "conversation"), ("tts", "start"),
        ("tts", "stop"), ("head_tracking", "tracking"),
    ]


@pytest.mark.asyncio
async def test_tracking_runs_through_a_conversation_and_pauses_for_the_user(gateway):
    gw, conn, proxy, ws, _ = gateway
    conn.camera_tracking_engaged = True
    await gw._tracking_connection_changed()
    proxy.conversation_active = True
    await gw._on_conversation_activity(proxy, True)
    assert gw._robot_tracking_allowed()
    assert gw.face_tracking_status()["pause_reason"] is None

    proxy.user_speaking = True
    assert not gw._robot_tracking_allowed()  # Before the device has even heard about it.
    await gw._sync_tracking_mode()
    assert not gw._robot_tracking_allowed()
    assert gw.face_tracking_status()["pause_reason"] == "user_speaking"

    proxy.user_speaking = False
    await gw._sync_tracking_mode()
    assert gw._robot_tracking_allowed()
    proxy.conversation_active = False
    await gw._on_conversation_activity(proxy, False)
    messages = [json.loads(c.args[0]) for c in ws.send.call_args_list]
    assert [m["mode"] for m in messages] == ["tracking", "engaged", "conversation", "engaged", "tracking"]


@pytest.mark.asyncio
async def test_detection_is_dropped_once_the_user_speaks(gateway):
    gw, conn, proxy, ws, _ = gateway
    conn.camera_tracking_engaged = True
    proxy.conversation_active = True
    await gw._tracking_connection_changed()
    ctx = context_for(conn)
    proxy.user_speaking = True
    ws.send.reset_mock()
    await gw._handle_robot_detection({"x": 1, "y": 1, "confidence": 1}, ctx)
    ws.send.assert_not_called()


@pytest.mark.asyncio
async def test_bot_speech_keeps_tracking_and_delivers_the_mode_first(gateway):
    gw, conn, _, ws, _ = gateway
    conn.camera_tracking_engaged = True
    await gw._tracking_connection_changed()
    ws.send.reset_mock()
    await gw.esp32.send_tts_state("start")
    assert gw._robot_tracking_allowed()
    assert gw.tracking_bridge.mode == "working"
    assert await gw.tracking_bridge.handle_robot_detection({"x": 0.8, "y": 0.5, "confidence": 1}, context_for(conn))
    await gw.esp32.send_tts_state("stop")
    messages = [json.loads(c.args[0]) for c in ws.send.call_args_list]
    assert [(m["type"], m.get("mode", m.get("state"))) for m in messages if m["type"] != "head_track"] == [
        ("head_tracking", "engaged"), ("tts", "start"),
        ("tts", "stop"), ("head_tracking", "tracking"),
    ]


@pytest.mark.asyncio
async def test_off_switch_holds_the_head_for_replies(gateway, monkeypatch):
    gw, conn, _, ws, _ = gateway
    monkeypatch.setenv("STACKCHAN_TRACK_WHILE_SPEAKING", "0")
    assert Gateway()._track_while_speaking is False
    gw._track_while_speaking = False
    conn.camera_tracking_engaged = True
    await gw._tracking_connection_changed()
    ws.send.reset_mock()
    await gw.esp32.send_tts_state("start")
    assert not gw._robot_tracking_allowed()
    assert json.loads(ws.send.call_args_list[0].args[0]) == {"type": "head_tracking", "mode": "conversation"}


@pytest.mark.asyncio
async def test_disabled_head_follow_stays_paused_during_a_conversation(gateway):
    gw, conn, proxy, ws, _ = gateway
    conn.camera_tracking_engaged = True
    await gw.stop_face_tracking()
    proxy.conversation_active = True
    await gw._on_conversation_activity(proxy, True)
    assert not gw._robot_tracking_allowed()
    assert json.loads(ws.send.call_args.args[0])["mode"] == "conversation"


def test_sleeping_robot_reports_why_tracking_stopped(gateway):
    gw, *_ = gateway
    gw.face_tracker._state = "sleeping"
    assert gw.face_tracking_status()["pause_reason"] == "sleeping"


@pytest.mark.asyncio
async def test_old_proxy_cannot_unpause_current_conversation(gateway):
    gw, _, proxy, ws, _ = gateway
    proxy.conversation_active = True
    await gw._on_conversation_activity(proxy, True)
    ws.send.reset_mock()
    await gw._on_conversation_activity(object(), False)
    assert not gw._robot_tracking_allowed()
    ws.send.assert_not_called()


@pytest.mark.asyncio
async def test_old_firmware_has_no_tracking_control_or_webcam_fallback(gateway):
    gw, conn, _, ws, _ = gateway
    conn.camera_tracking_v1 = False
    await gw._tracking_connection_changed()
    assert not gw._robot_tracking_allowed()
    assert gw.tracking_source == "robot"
    ws.send.assert_not_called()


@pytest.mark.asyncio
async def test_cloud_passthrough_keeps_robot_tracking_off(gateway, monkeypatch):
    gw, _, _, ws, _ = gateway
    monkeypatch.setenv("STACKCHAN_VOICE_BACKEND", "xiaozhi")
    await gw._tracking_connection_changed()
    assert not gw._robot_tracking_allowed()
    assert json.loads(ws.send.call_args.args[0]) == {"type": "head_tracking", "mode": "off"}
    assert gw.face_tracking_status()["pause_reason"] == "unsupported_voice_backend"
    assert "requires" in (await gw.start_face_tracking())["warning"]


@pytest.mark.asyncio
async def test_legacy_http_detections_and_absolute_snap_cannot_move_robot(gateway):
    gw, _, _, ws, _ = gateway
    response = await gw._handle_track(SimpleNamespace(json=AsyncMock(return_value={"x": 1})))
    assert response.status == 409
    assert not await gw.tracking_bridge.handle_detection({"x": 1, "y": 1, "confidence": 1})
    assert not await gw.tracking_bridge.snap_to_last_position()
    ws.send.assert_not_called()


@pytest.mark.asyncio
@pytest.mark.parametrize("face,expected", [
    ({"x": .5, "y": .5, "confidence": 1}, {"delta_yaw": 0, "delta_pitch": 0}),
    ({"x": 1, "y": 1, "confidence": 1}, {"delta_yaw": 15, "delta_pitch": 10}),
    ({"x": 0, "y": 0, "confidence": 1}, {"delta_yaw": -15, "delta_pitch": -10}),
    (None, {"release": True}),
    ({"x": .2, "y": .3, "confidence": .4}, {"release": True}),
    ({"x": float("nan"), "y": 0, "confidence": 1}, {"release": True}),
])
async def test_robot_face_results_are_bounded_relative_corrections(gateway, face, expected):
    gw, conn, _, ws, _ = gateway
    ctx = context_for(conn)
    await gw.tracking_bridge.handle_robot_detection(face, ctx)
    assert json.loads(ws.send.call_args.args[0]) == {"type": "head_track", "frame_id": ctx.frame_id, **expected}


@pytest.mark.asyncio
@pytest.mark.parametrize("invalid", ["expired", "session", "connection"])
async def test_obsolete_detection_does_not_send(gateway, invalid):
    gw, conn, _, ws, _ = gateway
    ctx = context_for(conn)
    if invalid == "expired":
        ctx.deadline = 0
    elif invalid == "session":
        ctx.session_id = "old-session"
    else:
        ctx.connection = SimpleNamespace()
    assert not await gw.tracking_bridge.handle_robot_detection({"x": 1, "y": 1, "confidence": 1}, ctx)
    ws.send.assert_not_called()


def test_peer_host_comes_from_transport_not_device_headers():
    conn = ESP32Connection(SimpleNamespace(remote_address=("::1", 3000, 0, 0)), "session")
    conn.device_id = "untrusted.example"
    assert conn.peer_host == "::1"
    assert conn.camera_tracking_v1 is False


@pytest.mark.asyncio
async def test_invalid_mode_and_nonfinite_delta_are_not_serialized():
    ws = SimpleNamespace(send=AsyncMock())
    conn = ESP32Connection(ws, "session")
    conn.camera_tracking_v1 = True
    with pytest.raises(ValueError):
        await conn.send_head_tracking("unexpected")
    with pytest.raises(ValueError):
        await conn.send_head_track("frame", delta_yaw=float("inf"))
    ws.send.assert_not_called()


@pytest.mark.asyncio
async def test_pause_send_failure_blocks_local_tts(gateway):
    gw, conn, _, ws, _ = gateway
    conn.send_head_tracking = AsyncMock(side_effect=ConnectionError("disconnected"))
    with pytest.raises(ConnectionError):
        await gw.esp32.send_tts_state("start")
    assert not gw._robot_tracking_allowed()
    ws.send.assert_not_called()


@pytest.mark.asyncio
@pytest.mark.parametrize("capability,expected", [(True, True), (False, False), ("true", False), (None, False)])
async def test_tracking_capability_comes_from_accepted_hello(monkeypatch, capability, expected):
    monkeypatch.delenv("STACKCHAN_TOKEN", raising=False)
    monkeypatch.delenv("BEARER_TOKEN", raising=False)
    manager = ESP32Manager(cloud_proxy_factory=lambda: None)
    ready = asyncio.Event()

    async def changed():
        if manager.connection is not None:
            ready.set()

    manager.on_connection_changed = changed
    await manager.start("127.0.0.1", 0)
    try:
        port = manager._server.sockets[0].getsockname()[1]
        async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
            await ws.send(json.dumps({"type": "hello", "features": {
                "mcp": False, "camera_tracking_v1": capability, "camera_tracking_v2": True,
                "camera_tracking_engaged_v1": True,
            }}))
            await asyncio.wait_for(ws.recv(), 1)
            await asyncio.wait_for(ready.wait(), 1)
            assert manager.connection.camera_tracking_v1 is expected
            assert manager.connection.camera_tracking_v2 is expected
            assert manager.connection.camera_tracking_engaged is expected
            assert manager.connection.peer_host == "127.0.0.1"
    finally:
        await manager.stop()
