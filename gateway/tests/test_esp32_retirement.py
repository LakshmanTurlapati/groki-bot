# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Device replacement must retire voice without closing external MCP access."""

import asyncio
import json
from unittest.mock import AsyncMock

import pytest
import websockets

from stackchan_mcp.debug_status import DebugStatus
from stackchan_mcp.esp32_client import ESP32Manager

HELLO = json.dumps({"type": "hello", "features": {"mcp": False}})


async def eventually(predicate):
    async def wait():
        while not predicate():
            await asyncio.sleep(0.001)
    await asyncio.wait_for(wait(), 1)


class Proxy:
    def __init__(self, manager, callbacks, index):
        self.esp32_ref = lambda: manager
        self.on_head_command = lambda: callbacks.append((index, "head"))
        self.on_device_state = lambda state: callbacks.append((index, state))
        self.on_conversation_activity = AsyncMock(side_effect=lambda active: callbacks.append((index, active)))
        self.set_face_tracking = AsyncMock(side_effect=lambda active: callbacks.append((index, "tracking")))
        self.frames = []
        self.messages = []
        self.started = False
        self.retired = False
        self.stop_count = 0
        self.stop_started = asyncio.Event()
        self.stop_done = asyncio.Event()
        self.release_stop = asyncio.Event()
        self.release_stop.set()

    async def start(self, info, hello, send):
        # Gemini creates its tools before ESP32Manager registers the socket.
        assert self.esp32_ref() is not None
        self.started = True
        return True

    def retire(self):
        self.retired = True

    async def stop(self):
        self.stop_count += 1
        self.stop_started.set()
        await self.release_stop.wait()
        # Simulate callbacks already captured by an upstream voice bridge.
        self.on_head_command()
        self.on_device_state("late idle")
        await self.on_conversation_activity(False)
        await self.set_face_tracking(True)
        self.stop_done.set()

    async def send_device_binary(self, data):
        self.frames.append(data)
        return True

    async def send_device_json(self, message):
        self.messages.append(message)
        return True

    async def try_handle_device_mcp(self, message):
        return False


@pytest.mark.asyncio
async def test_replacement_retires_voice_and_rejects_queued_old_socket_work(monkeypatch):
    monkeypatch.delenv("STACKCHAN_TOKEN", raising=False)
    monkeypatch.delenv("BEARER_TOKEN", raising=False)
    callbacks, proxies = [], []
    local_tool = AsyncMock()

    def factory():
        proxy = Proxy(manager, callbacks, len(proxies))
        proxies.append(proxy)
        return proxy

    manager = ESP32Manager(cloud_proxy_factory=factory, local_tool_handler=local_tool, debug_status=DebugStatus())
    await manager.start("127.0.0.1", 0)
    uri = f"ws://127.0.0.1:{manager._server.sockets[0].getsockname()[1]}"
    close_started, release_close, final_old_frame = asyncio.Event(), asyncio.Event(), asyncio.Event()
    marker = json.dumps({"type": "mcp", "payload": {
        "jsonrpc": "2.0", "id": 99, "method": "tools/call",
        "params": {"name": "set_face_tracking", "arguments": {"enabled": True}},
    }})
    try:
        async with websockets.connect(uri) as old:
            await old.send(HELLO)
            await asyncio.wait_for(old.recv(), 1)
            await eventually(lambda: manager.connection is not None)
            old_connection, old_proxy = manager.connection, proxies[0]
            old_proxy.release_stop.clear()
            await old.send(b"accepted before replacement")
            await eventually(lambda: len(old_proxy.frames) == 1)

            original_close, original_recv = old_connection._ws.close, old_connection._ws.recv

            async def delayed_close(*args, **kwargs):
                close_started.set()
                await release_close.wait()
                await original_close(*args, **kwargs)

            async def observed_recv(*args, **kwargs):
                message = await original_recv(*args, **kwargs)
                if message == marker:
                    final_old_frame.set()
                return message

            monkeypatch.setattr(old_connection._ws, "close", delayed_close)
            monkeypatch.setattr(old_connection._ws, "recv", observed_recv)
            async with websockets.connect(uri) as new:
                await new.send(HELLO)
                await asyncio.wait_for(new.recv(), 0.5)
                await eventually(lambda: manager.connection is not old_connection)
                await asyncio.wait_for(old_proxy.stop_started.wait(), 0.5)
                await asyncio.wait_for(close_started.wait(), 0.5)
                assert old_proxy.retired and not old_connection.connected
                assert not old_proxy.stop_done.is_set()
                assert old_proxy.esp32_ref() is None
                assert proxies[1].esp32_ref().connection is manager.connection
                assert proxies[1].esp32_ref().device_connected

                # The old transport is deliberately still open, so this also
                # exercises messages queued before its close handshake.
                await old.send(b"stale microphone")
                for message in (
                    {"type": "listen", "state": "detect"},
                    {"type": "abort"}, {"type": "tts", "state": "start"},
                ):
                    await old.send(json.dumps(message))
                await old.send(HELLO)  # A retired socket cannot reclaim the robot.
                await old.send(marker)  # Nor become an external MCP client.
                await asyncio.wait_for(final_old_frame.wait(), 0.5)
                assert old_proxy.frames == [b"accepted before replacement"]
                assert old_proxy.messages == []
                assert not manager._debug_status.tts_active
                local_tool.assert_not_awaited()
                assert manager.connection.cloud_proxy is proxies[1]

                await new.send(b"current microphone")
                await eventually(lambda: proxies[1].frames == [b"current microphone"])
                old_proxy.release_stop.set()
                await asyncio.wait_for(old_proxy.stop_done.wait(), 0.5)
                assert callbacks == []
                proxies[1].on_head_command()
                assert callbacks == [(1, "head")]
                release_close.set()
                await asyncio.wait_for(old.wait_closed(), 1)
                assert old_proxy.stop_count == 1
    finally:
        release_close.set()
        for proxy in proxies:
            proxy.release_stop.set()
        await manager.stop()


@pytest.mark.asyncio
async def test_unregistered_clients_drop_voice_but_keep_external_mcp(monkeypatch):
    monkeypatch.delenv("STACKCHAN_TOKEN", raising=False)
    monkeypatch.delenv("BEARER_TOKEN", raising=False)
    callbacks, proxies = [], []

    def factory():
        proxy = Proxy(manager, callbacks, len(proxies))
        proxies.append(proxy)
        return proxy

    manager = ESP32Manager(cloud_proxy_factory=factory, debug_status=DebugStatus())
    await manager.start("127.0.0.1", 0)
    uri = f"ws://127.0.0.1:{manager._server.sockets[0].getsockname()[1]}"
    try:
        async with websockets.connect(uri) as external:
            await external.send(b"not a registered microphone")
            await external.send(json.dumps({"type": "tts", "state": "start"}))
            await external.send(json.dumps({"type": "listen", "state": "detect"}))
            await external.send(json.dumps({"type": "mcp", "payload": {
                "jsonrpc": "2.0", "id": 42, "method": "initialize", "params": {},
            }}))
            response = json.loads(await asyncio.wait_for(external.recv(), 1))
            assert response["payload"]["id"] == 42
            assert "result" in response["payload"]
            assert manager.connection is None
            assert not manager._debug_status.tts_active
            assert not proxies[0].started and not proxies[0].frames and not proxies[0].messages
            assert callbacks == []
    finally:
        await manager.stop()


@pytest.mark.asyncio
async def test_replaced_socket_close_is_bounded_and_aborts_stalled_transport():
    from types import SimpleNamespace
    from unittest.mock import Mock

    manager = ESP32Manager(cloud_proxy_factory=lambda: None)
    close_started = asyncio.Event()

    async def stalled_close(**kwargs):
        close_started.set()
        await asyncio.Future()

    from stackchan_mcp.esp32_client import ESP32Connection
    transport = SimpleNamespace(abort=Mock())
    connection = ESP32Connection(SimpleNamespace(close=stalled_close, transport=transport), "old")
    manager._retire_connection(connection)
    await asyncio.wait_for(close_started.wait(), 0.5)
    await asyncio.wait_for(asyncio.gather(*list(manager._retirement_tasks)), 1.5)
    assert not connection.connected
    transport.abort.assert_called_once()


@pytest.mark.asyncio
@pytest.mark.parametrize("backend", ["gemini", "gbot"])
async def test_captured_voice_tool_endpoints_are_revoked_before_slow_stop(monkeypatch, backend):
    from types import SimpleNamespace

    from stackchan_mcp.gbot_voice_proxy import GbotVoiceProxy
    from stackchan_mcp.gemini_voice_proxy import GeminiVoiceProxy
    from stackchan_mcp.voice_tools import VoiceToolExecutor

    monkeypatch.delenv("STACKCHAN_TOKEN", raising=False)
    monkeypatch.delenv("BEARER_TOKEN", raising=False)
    monkeypatch.setenv("GEMINI_API_KEY", "test-key")
    monkeypatch.setenv("STACKCHAN_WAKE_WORD", "0")
    monkeypatch.setenv("STACKCHAN_MAC_CONTROL", "0")

    class Bridge(VoiceToolExecutor):
        def __init__(self, esp32, **kwargs):
            super().__init__(
                esp32, usb_transport=kwargs.get("usb_transport"),
                on_head_command=kwargs.get("on_head_command"),
            )
            self.running = True
            self._connected_event = asyncio.Event()
            self._connected_event.set()
            self.stop_started = asyncio.Event()
            self.release_stop = asyncio.Event()
            self.release_stop.set()

        async def start(self):
            pass

        async def wait_connected(self, timeout):
            return True

        async def stop(self):
            self.stop_started.set()
            await self.release_stop.wait()
            self.running = False
            await self.close()

    proxies = []
    usb = SimpleNamespace(connected=True, call_tool=AsyncMock(return_value={"result": "ok"}))

    def factory():
        proxy_class = GeminiVoiceProxy if backend == "gemini" else GbotVoiceProxy
        proxy = proxy_class(
            esp32_ref=lambda: manager,
            usb_transport=usb,
            codec_factory=lambda: SimpleNamespace(reset_encode_buffer=lambda: None),
            bridge_factory=Bridge,
        )
        proxies.append(proxy)
        return proxy

    manager = ESP32Manager(cloud_proxy_factory=factory)
    manager.send_head = AsyncMock(return_value=({"ok": True}, None))
    manager.on_end_conversation = AsyncMock()
    await manager.start("127.0.0.1", 0)
    uri = f"ws://127.0.0.1:{manager._server.sockets[0].getsockname()[1]}"
    old_bridge = None
    try:
        async with websockets.connect(uri) as old:
            await old.send(HELLO)
            await asyncio.wait_for(old.recv(), 1)
            await eventually(lambda: manager.connection is not None)
            old_connection = manager.connection
            old_bridge = proxies[0]._bridge
            old_bridge.release_stop.clear()
            cached_head = old_bridge._esp32.send_head
            cached_end = old_bridge._esp32.on_end_conversation
            cached_usb = old_bridge._usb_transport.call_tool
            # The endpoint is usable while this socket owns the robot.
            assert (await old_bridge._dispatch_device_head({"yaw": 1, "pitch": 2, "speed": 200}))["ok"]
            manager.send_head.reset_mock()
            async with websockets.connect(uri) as new:
                await new.send(HELLO)
                await asyncio.wait_for(new.recv(), 1)
                await eventually(lambda: manager.connection is not old_connection)
                await asyncio.wait_for(old_bridge.stop_started.wait(), 1)
                assert not old_bridge.release_stop.is_set()
                assert not old_bridge._esp32.device_connected
                assert old_bridge._esp32.connection is None
                assert not old_bridge._usb_transport.connected

                # Both the executor and already-cached methods must recheck
                # ownership even though teardown is deliberately still waiting.
                result = await old_bridge._dispatch_device_head({"yaw": 6, "pitch": 4, "speed": 200})
                assert not result["ok"]
                for callback, args in (
                    (cached_head, (6, 4, 200)),
                    (cached_end, ()),
                    (cached_usb, ("self.robot.set_head_angles", {"yaw": 6})),
                ):
                    with pytest.raises(ConnectionError, match="no longer current"):
                        await callback(*args)
                manager.send_head.assert_not_awaited()
                manager.on_end_conversation.assert_not_awaited()
                usb.call_tool.assert_not_awaited()

                current_bridge = proxies[1]._bridge
                assert (await current_bridge._dispatch_device_head({"yaw": 0, "pitch": 0, "speed": 200}))["ok"]
                manager.send_head.assert_awaited_once()
                old_bridge.release_stop.set()
    finally:
        if old_bridge is not None:
            old_bridge.release_stop.set()
        await manager.stop()
