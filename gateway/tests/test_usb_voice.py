# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""USB voice session and local POST /say.

The cable stands in for the WebSocket: a voice/mode signal, a XiaoZhi hello,
then the same Opus frames and tts/listen JSON. /say must open that session
without a wake word, and only from loopback.
"""

from __future__ import annotations

import json
from typing import Any

import pytest

from stackchan_mcp.gateway import Gateway
from stackchan_mcp.usb_voice import UsbVoiceSession
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

    def add_voice_frame_handler(self, handler: Any) -> None:
        self.handlers.append(handler)

    async def set_voice_mode(self, enable: bool, timeout_s: float = 3.0) -> dict[str, Any]:
        self.mode = enable
        hello = json.dumps({
            "type": "hello",
            "version": 1,
            "transport": "usb",
            "features": {"mcp": False},
            "device_id": "usb-test",
            "audio_params": {"format": "opus", "sample_rate": 16000, "frame_duration": 60},
        }).encode()
        frame = UsbVoiceFrame(type=VOICE_JSON, payload=hello)
        for handler in list(self.handlers):
            handler(frame)
        return {"result": {"ok": True, "transport": "usb", "duplex": "half"}}

    async def write_raw(self, data: bytes) -> None:
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
