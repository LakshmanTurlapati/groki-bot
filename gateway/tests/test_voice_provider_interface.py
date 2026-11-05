# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Provider-independent device lifecycle; no model or hardware connections."""

import asyncio
import threading
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from stackchan_mcp.gbot_voice_proxy import GbotVoiceProxy
from stackchan_mcp.wake_gate import WakeGate, WakeGateState


@pytest.mark.asyncio
async def test_login_voice_uses_common_wake_and_end_callbacks_without_gemini_key(
    monkeypatch,
):
    monkeypatch.delenv("GEMINI_API_KEY", raising=False)
    monkeypatch.delenv("GOOGLE_API_KEY", raising=False)
    gate = SimpleNamespace(
        available=True,
        state=WakeGateState.DORMANT,
        is_listening=False,
        is_tts_active=None,
        close=lambda: False,
    )
    fake = SimpleNamespace(
        start=AsyncMock(),
        stop=AsyncMock(),
        wait_connected=AsyncMock(return_value=True),
        running=True,
        _connected_event=asyncio.Event(),
    )
    fake._connected_event.set()
    observed = {}

    def factory(**kwargs):
        observed.update(kwargs)
        return fake

    proxy = GbotVoiceProxy(
        esp32_ref=lambda: object(),
        bridge_factory=factory,
        wake_gate_factory=lambda: gate,
    )
    try:
        assert await proxy.start(SimpleNamespace(device_id="fixture"), {}, AsyncMock())
        await proxy._wake_runner.wait_initialized()
        assert proxy.wake_gate_state == "DORMANT"
        assert not proxy.is_listening
        assert callable(observed.get("on_end_conversation"))
        assert observed.get("api_key") is None
        assert proxy.server_hello["provider"] == "gbot_voice"
    finally:
        await proxy.stop()


@pytest.mark.asyncio
async def test_grok_shared_speak_waits_for_first_delayed_audio(monkeypatch):
    monkeypatch.setenv("GEMINI_SPEAK_TIMEOUT_SECONDS", ".3")
    proxy = GbotVoiceProxy()
    ready = asyncio.Event()
    ready.set()

    async def inject(text):
        async def later():
            await asyncio.sleep(0.01)
            proxy._speak_text_frames = 1

        asyncio.create_task(later())

    proxy._bridge = SimpleNamespace(
        running=True, _connected_event=ready, inject_user_text=inject
    )
    proxy._codec = SimpleNamespace(_encode_buffer=b"")
    proxy._send_to_device = AsyncMock()
    proxy._ready_event.set()
    result = await proxy.speak_text(
        "fixture", session_id="fixture", prompt_audio_frames=[]
    )
    assert result["ok"] and result["frames_sent"] == 1


@pytest.mark.asyncio
async def test_model_warmup_does_not_delay_voice_start_or_replay_early_microphone():
    loop = asyncio.get_running_loop()
    entered = asyncio.Event()
    release = threading.Event()

    class Spotter:
        available = True

        def reset(self):
            pass

        def detect(self, pcm):
            return pcm == b"wake"

    def factory():
        loop.call_soon_threadsafe(entered.set)
        assert release.wait(2)
        return WakeGate(kws=Spotter())

    connected = asyncio.Event()
    connected.set()
    bridge = SimpleNamespace(
        start=AsyncMock(), stop=AsyncMock(), wait_connected=AsyncMock(return_value=True),
        running=True, _connected_event=connected, send_audio=AsyncMock(),
    )
    codec = SimpleNamespace(
        decode_device_frame=lambda frame: frame, reset_encode_buffer=lambda: None,
    )
    proxy = GbotVoiceProxy(
        esp32_ref=lambda: object(), bridge_factory=lambda **kwargs: bridge,
        codec_factory=lambda: codec, wake_gate_factory=factory,
        wake_confirm_hold_s=0,
    )
    proxy._signal_wake_chime = lambda *_: None
    try:
        assert await asyncio.wait_for(proxy.start(SimpleNamespace(device_id="fixture"), {}, AsyncMock()), 0.5)
        await asyncio.wait_for(entered.wait(), 1)
        assert proxy.connected and proxy.wake_gate_state == "UNAVAILABLE"
        assert await proxy.send_device_binary(b"private early speech")
        bridge.send_audio.assert_not_awaited()
        release.set()
        await proxy._wake_runner.wait_initialized()
        assert not proxy.is_listening
        await proxy.send_device_binary(b"wake")
        bridge.send_audio.assert_awaited_once_with(b"wake")
    finally:
        release.set()
        await proxy.stop()
