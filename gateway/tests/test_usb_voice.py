# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""USB voice session and local POST /say.

The cable stands in for the WebSocket: a voice/mode signal, a XiaoZhi hello,
then the same Opus frames and tts/listen JSON. /say must open that session
without a wake word, and only from loopback.
"""

from __future__ import annotations

import json
import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock
from typing import Any

import pytest

from stackchan_mcp.gateway import Gateway
from stackchan_mcp.usb_voice import UsbVoiceSender, UsbVoiceSession
from stackchan_mcp.usb_voice_framer import (
    AUDIO_UPLINK,
    VOICE_JSON,
    UsbVoiceFrame,
    UsbVoiceLine,
    UsbVoiceStreamParser,
    encode_usb_voice_frame,
)


def test_framer_round_trip_and_mcp_line_coexist():
    parser = UsbVoiceStreamParser()
    opus = b"\n$MCP: {\"id\":1}\n\x00\xff"
    blob = b"I (1) boot: hi\n" + encode_usb_voice_frame(AUDIO_UPLINK, opus) + b"$MCP: {\"ok\":1}\n"
    # Split the blob so a frame header can straddle a read.
    events = []
    events.extend(parser.feed(blob[:5]))
    events.extend(parser.feed(blob[5:]))
    kinds = [type(event).__name__ for event in events]
    assert kinds == ["UsbVoiceLine", "UsbVoiceFrame", "UsbVoiceLine"]
    assert isinstance(events[0], UsbVoiceLine)
    assert events[0].text.startswith("I (1) boot")
    assert isinstance(events[1], UsbVoiceFrame)
    assert events[1].type == AUDIO_UPLINK
    assert events[1].payload == opus
    assert isinstance(events[2], UsbVoiceLine)
    assert events[2].text.startswith("$MCP:")


def test_framer_rejects_oversize_payload():
    assert encode_usb_voice_frame(AUDIO_UPLINK, b"x" * 1276) == b""


def test_bad_crc_does_not_eat_the_following_mcp_line():
    parser = UsbVoiceStreamParser()
    frame = bytearray(encode_usb_voice_frame(AUDIO_UPLINK, b"\n$MCP: x\n"))
    frame[-1] ^= 0xFF
    events = parser.feed(bytes(frame) + b'$MCP: {"id":2}\n')
    lines = [event.text for event in events if isinstance(event, UsbVoiceLine)]
    frames = [event for event in events if isinstance(event, UsbVoiceFrame)]
    assert frames == []
    assert lines == ['$MCP: {"id":2}']


class FakeTransport:
    def __init__(self) -> None:
        self.connected = True
        self.handlers: list[Any] = []
        self.mode: bool | None = None
        self.writes: list[bytes] = []
        self.connection_generation = 1
        self.pings = 0
        self.features = {"mcp": False}

    def add_voice_frame_handler(self, handler: Any) -> None:
        self.handlers.append(handler)

    def remove_voice_frame_handler(self, handler: Any) -> None:
        self.handlers.remove(handler)

    async def ping_voice(self, timeout_s: float = 3.0):
        self.pings += 1
        return {"result": {"ok": self.mode is True, "lease_ms": 10000}}

    async def set_voice_mode(self, enable: bool, timeout_s: float = 3.0) -> dict[str, Any]:
        self.mode = enable
        if not enable:
            return {"result": {"ok": True}}
        hello = json.dumps({
            "type": "hello",
            "version": 1,
            "transport": "usb",
            "features": self.features,
            "device_id": "usb-test",
            "audio_params": {"format": "opus", "sample_rate": 16000, "frame_duration": 60},
        }).encode()
        frame = UsbVoiceFrame(type=VOICE_JSON, payload=hello)
        for handler in list(self.handlers):
            handler(frame)
        return {"result": {"ok": True, "transport": "usb", "duplex": "half"}}

    async def write_raw(self, data: bytes, *, is_current=None) -> None:
        if is_current is not None and not is_current():
            raise ConnectionError("USB session expired")
        self.writes.append(data)


class FakeProxy:
    def __init__(self) -> None:
        self.started = False
        self.connected = True
        self.binary: list[bytes] = []
        self.jsons: list[dict[str, Any]] = []
        self.spoken: dict[str, Any] | None = None

    async def start(self, info: Any, hello: dict[str, Any], send: Any) -> bool:
        self.started = True
        self.hello = hello
        self.send = send
        return True

    async def stop(self) -> None:
        self.connected = False

    async def send_device_binary(self, data: bytes) -> bool:
        self.binary.append(data)
        return True

    async def send_device_json(self, message: dict[str, Any]) -> bool:
        self.jsons.append(message)
        return True

    async def speak_text(self, text: str, *, session_id: str, prompt_audio_frames: list[bytes], emotion: str | None = None):
        self.spoken = {
            "text": text,
            "session_id": session_id,
            "frames": prompt_audio_frames,
            "emotion": emotion,
        }
        return {"ok": True, "text": text, "frames_sent": 1, "provider": "gemini_live"}


def _gateway_with_usb() -> tuple[Gateway, FakeTransport, FakeProxy]:
    gw = Gateway()
    transport = FakeTransport()
    proxy = FakeProxy()
    gw.usb_transport = transport
    gw.esp32.usb_transport = transport
    gw.esp32._cloud_proxy_factory = lambda: proxy
    gw.usb_voice = UsbVoiceSession(gw.esp32, transport)
    gw.usb_voice.start()
    return gw, transport, proxy


@pytest.mark.asyncio
async def test_usb_hello_creates_voice_proxy_and_forwards_audio():
    gw, _transport, proxy = _gateway_with_usb()
    opened = await gw.usb_voice.ensure_session(timeout_s=2.0)
    assert opened is proxy
    assert proxy.started is True
    assert proxy.hello["transport"] == "usb"
    assert gw.esp32.connection is not None
    assert gw.esp32.connection.transport == "usb"
    assert gw.esp32.connection.mcp_supported is False

    opus = b"\x00\nframe"
    for handler in gw.usb_voice._transport.handlers:
        handler(UsbVoiceFrame(type=AUDIO_UPLINK, payload=opus))
    for _ in range(20):
        if proxy.binary:
            break
        await _sleep()
    assert proxy.binary == [opus]

    listen = json.dumps({"type": "listen", "state": "start", "mode": "auto"}).encode()
    for handler in gw.usb_voice._transport.handlers:
        handler(UsbVoiceFrame(type=VOICE_JSON, payload=listen))
    for _ in range(20):
        if proxy.jsons:
            break
        await _sleep()
    assert proxy.jsons[0]["type"] == "listen"
    await gw.usb_voice.stop()


@pytest.mark.asyncio
async def test_say_opens_usb_session_without_wake_word():
    gw, transport, proxy = _gateway_with_usb()

    class Request:
        remote = "127.0.0.1"
        headers = {}
        host = "127.0.0.1:8766"
        scheme = "http"

        async def json(self) -> dict[str, Any]:
            return {"text": "the build finished", "emotion": "happy"}

    response = await gw._handle_say(Request())
    body = json.loads(response.text)
    assert response.status == 200
    assert body["ok"] is True
    assert transport.mode is True
    assert proxy.spoken is not None
    assert proxy.spoken["text"] == "the build finished"
    assert proxy.spoken["emotion"] == "happy"
    assert proxy.spoken["frames"] == []
    await gw.usb_voice.stop()


@pytest.mark.asyncio
async def test_say_reuses_existing_proxy_without_usb_signal():
    gw = Gateway()
    called = {}

    async def speak_text(text, *, session_id, prompt_audio_frames, emotion=None):
        called["text"] = text
        return {"ok": True, "text": text}

    class Conn:
        connected = True
        transport = "websocket"
        cloud_proxy = type("P", (), {"speak_text": staticmethod(speak_text), "connected": True})()

    gw.esp32._connection = Conn()

    class Request:
        remote = "::1"
        headers = {}
        host = "127.0.0.1:8766"
        scheme = "http"

        async def json(self) -> dict[str, Any]:
            return {"text": "again"}

    response = await gw._handle_say(Request())
    assert response.status == 200
    assert called["text"] == "again"


@pytest.mark.asyncio
async def test_say_rejects_non_loopback():
    gw = Gateway()

    class Request:
        remote = "192.168.1.20"
        headers = {}
        host = "127.0.0.1:8766"
        scheme = "http"

        async def json(self) -> dict[str, Any]:
            return {"text": "nope"}

    response = await gw._handle_say(Request())
    assert response.status == 403
    assert json.loads(response.text)["error"] == "local only"


@pytest.mark.asyncio
async def test_usb_voice_does_not_replace_websocket_session():
    gw, transport, _proxy = _gateway_with_usb()

    class Conn:
        connected = True
        transport = "websocket"

    gw.esp32._connection = Conn()
    opened = await gw.usb_voice.ensure_session(timeout_s=0.2)
    assert opened is None
    assert transport.mode is None
    await gw.usb_voice.stop()


async def _sleep() -> None:
    import asyncio
    await asyncio.sleep(0.01)


async def _wait_until(predicate, timeout=1.0):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(0)


@pytest.mark.asyncio
async def test_stop_disables_lease_and_unregisters_receiver():
    gw, transport, _ = _gateway_with_usb()
    await gw.usb_voice.ensure_session(timeout_s=1)
    assert len(transport.handlers) == 1
    await gw.usb_voice.stop()
    assert transport.mode is False
    assert not transport.handlers
    assert gw.esp32.connection is None
    assert not gw.usb_voice._cleanup_tasks
    gw.usb_voice.start()
    assert len(transport.handlers) == 1
    await gw.usb_voice.stop()
    assert not transport.handlers


@pytest.mark.asyncio
async def test_heartbeat_runs_during_slow_proxy_start_and_stop_cancels_start():
    gw, transport, proxy = _gateway_with_usb()
    starting = asyncio.Event()
    cancelled = asyncio.Event()

    async def start(*args):
        starting.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()

    proxy.start = start
    gw.usb_voice.HEARTBEAT_S = .01
    opener = asyncio.create_task(gw.usb_voice.ensure_session(timeout_s=1))
    try:
        await asyncio.wait_for(starting.wait(), 1)
        await _wait_until(lambda: transport.pings >= 2)
        assert not opener.done()
        await asyncio.wait_for(gw.usb_voice.stop(), 1)
        assert await opener is None
        assert cancelled.is_set()
        assert transport.mode is False
        assert gw.esp32.connection is None
        assert not transport.handlers
    finally:
        await gw.usb_voice.stop()
        opener.cancel()
        await asyncio.gather(opener, return_exceptions=True)


@pytest.mark.asyncio
async def test_usb_audio_and_listen_events_are_processed_in_order():
    gw, transport, proxy = _gateway_with_usb()
    await gw.usb_voice.ensure_session(timeout_s=1)
    first_started = asyncio.Event()
    release_first = asyncio.Event()
    events = []

    async def audio(data):
        events.append(data)
        if data == b"first":
            first_started.set()
            await release_first.wait()
        return True

    async def control(data):
        events.append(data["state"])
        return True

    proxy.send_device_binary = audio
    proxy.send_device_json = control
    try:
        transport.handlers[0](UsbVoiceFrame(AUDIO_UPLINK, b"first"))
        await asyncio.wait_for(first_started.wait(), 1)
        transport.handlers[0](UsbVoiceFrame(VOICE_JSON, b'{"type":"listen","state":"stop"}'))
        transport.handlers[0](UsbVoiceFrame(AUDIO_UPLINK, b"second"))
        await asyncio.sleep(.01)
        assert events == [b"first"]
        release_first.set()
        await _wait_until(lambda: len(events) == 3)
        assert events == [b"first", "stop", b"second"]
    finally:
        release_first.set()
        await gw.usb_voice.stop()


@pytest.mark.asyncio
async def test_websocket_replacement_stops_usb_proxy_once_and_drops_stale_callbacks():
    from stackchan_mcp.esp32_client import ESP32Connection

    gw, transport, proxy = _gateway_with_usb()
    proxy.stop = AsyncMock(side_effect=proxy.stop)
    proxy.on_conversation_activity = AsyncMock()
    proxy.esp32_ref = lambda: gw.esp32
    await gw.usb_voice.ensure_session(timeout_s=1)
    old_connection = gw.esp32.connection
    old_endpoint = proxy.esp32_ref()
    stale_handler = transport.handlers[0]
    gw.esp32._retire_connection(old_connection)
    replacement = ESP32Connection(SimpleNamespace(send=AsyncMock()), "replacement")
    replacement.cloud_proxy = SimpleNamespace()
    gw.esp32._connection = replacement
    try:
        stale_handler(UsbVoiceFrame(AUDIO_UPLINK, b"stale"))
        stale_handler(UsbVoiceFrame(VOICE_JSON, b'{"type":"tts","state":"stop"}'))
        await gw.usb_voice._tick()
        await gw.usb_voice.stop()
        await gw.esp32.stop()
        assert proxy.binary == [] and proxy.jsons == []
        proxy.stop.assert_awaited_once()
        assert gw.esp32.connection is replacement
        assert transport.mode is False
        with pytest.raises(ConnectionError, match="no longer current"):
            await old_endpoint.send_head(20, 10, 200)
        await proxy.on_conversation_activity(False)
        replacement._ws.send.assert_not_called()
    finally:
        await gw.usb_voice.stop()
        await gw.esp32.stop()


@pytest.mark.asyncio
async def test_sender_rechecks_session_when_queued_write_is_released():
    transport = FakeTransport()
    started = asyncio.Event()
    release = asyncio.Event()

    async def queued_write(data, *, is_current):
        started.set()
        await release.wait()
        if not is_current():
            raise ConnectionError("USB session expired")
        transport.writes.append(data)

    transport.write_raw = queued_write
    sender = UsbVoiceSender(transport)
    task = asyncio.create_task(sender.send(b"old audio"))
    await started.wait()
    await sender.close()
    release.set()
    with pytest.raises(ConnectionError):
        await task
    assert transport.writes == []


@pytest.mark.asyncio
@pytest.mark.parametrize("disabled", [False, True])
async def test_usb_microphone_obeys_real_wake_failure_and_explicit_disable(monkeypatch, disabled):
    from test_gemini_voice_proxy import _make_proxy
    from stackchan_mcp.wake_gate import WakeWordUnavailable

    gw, transport, _ = _gateway_with_usb()
    proxy, _, _ = _make_proxy(monkeypatch=monkeypatch, esp32=gw.esp32)

    def missing_model():
        raise WakeWordUnavailable("missing test model")

    proxy.wake_gate_factory = (lambda: None) if disabled else missing_model
    gw.esp32._cloud_proxy_factory = lambda: proxy
    try:
        assert await gw.usb_voice.ensure_session(timeout_s=1) is proxy
        await proxy._wake_runner.wait_initialized()
        transport.handlers[0](UsbVoiceFrame(AUDIO_UPLINK, b"microphone"))
        await _wait_until(lambda: bool(proxy._codec.decoded))
        await _wait_until(lambda: not gw.usb_voice._frames.qsize())
        # Wait for the worker decision after decoding.
        await asyncio.sleep(.02)
        assert proxy.wake_gate_state == ("DISABLED" if disabled else "UNAVAILABLE")
        assert proxy._bridge.audio_sent == ([b"PCM" * 4] if disabled else [])
    finally:
        await gw.usb_voice.stop()


@pytest.mark.asyncio
async def test_usb_wake_pauses_head_before_confirmation_and_resumes_after_close(monkeypatch):
    from test_gemini_voice_proxy import _make_proxy, ScriptedKeywordSpotter
    from stackchan_mcp.wake_gate import WakeGate

    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_AUTOSTART", "0")
    monkeypatch.setenv("STACKCHAN_FACE_TRACKER_SOURCE", "robot")
    gw, transport, _ = _gateway_with_usb()
    transport.features["camera_tracking_v1"] = True
    proxy, _, _ = _make_proxy(monkeypatch=monkeypatch, esp32=gw.esp32)
    proxy.wake_gate_factory = lambda: WakeGate(kws=ScriptedKeywordSpotter([True]), activity_rms_threshold=0)
    proxy.on_conversation_activity = lambda active: gw._on_conversation_activity(proxy, active)
    gw.esp32._cloud_proxy_factory = lambda: proxy

    def controls():
        parser = UsbVoiceStreamParser()
        frames = [frame for data in transport.writes for frame in parser.feed(data)]
        return [json.loads(frame.payload) for frame in frames
                if isinstance(frame, UsbVoiceFrame) and frame.type == VOICE_JSON]

    try:
        assert await gw.usb_voice.ensure_session(timeout_s=1) is proxy
        await proxy._wake_runner.wait_initialized()
        assert gw.esp32.connection.camera_tracking_v1
        assert gw._robot_tracking_allowed()
        transport.handlers[0](UsbVoiceFrame(AUDIO_UPLINK, b"wake"))
        await _wait_until(lambda: bool(proxy._bridge.audio_sent))
        assert not gw._robot_tracking_allowed()
        messages = controls()
        conversation_index = next(i for i, msg in enumerate(messages)
                                  if msg.get("type") == "head_tracking" and msg.get("mode") == "conversation")
        confirmation_index = next(i for i, msg in enumerate(messages) if msg.get("type") == "llm")
        assert conversation_index < confirmation_index
        assert await proxy._wake_runner.close()
        await proxy._wake_window_closed(proxy._bridge)
        # The wake chime can still own the speaking pause after listening ends.
        await _wait_until(gw._robot_tracking_allowed)
        assert controls()[-1] == {"type": "head_tracking", "mode": "tracking"}
    finally:
        await gw.usb_voice.stop()
