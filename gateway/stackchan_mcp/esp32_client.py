"""ESP32 connection manager.

Acts as a WebSocket server that ESP32 connects TO,
and as an MCP client that sends commands TO the ESP32.
"""

from __future__ import annotations

import asyncio
import inspect
import json
import logging
import math
import os
import uuid
from collections.abc import Awaitable, Callable
from typing import Any

import websockets
import websockets.exceptions
from websockets.asyncio.server import ServerConnection

from . import bot_activity
from .audio_stream import SpeakResult, handle_audio_frame, speak_text
from .cloud_proxy import CloudConnectionInfo, CloudProxy
from .debug_status import DebugStatus, get_debug_status
from .protocol import HelloResponse, make_mcp_message, parse_jsonrpc_response

logger = logging.getLogger(__name__)

# Timeout for waiting for ESP32 responses
RESPONSE_TIMEOUT = 300.0


def _require_cloud_speak() -> bool:
    """Return whether speak must use the xiaozhi cloud proxy.

    Default is False so speak uses gateway-side TTS and preserves the exact
    input text instead of asking the cloud LLM to repeat it.
    """
    return os.getenv("STACKCHAN_REQUIRE_CLOUD", "0").lower() in {"1", "true", "yes", "on"}


class ESP32Connection:
    """Manages a single ESP32 device connection."""

    def __init__(self, ws: ServerConnection, session_id: str):
        self._ws = ws
        self.session_id = session_id
        self.device_id: str = "unknown"
        self.tools: list[dict[str, Any]] = []
        self._request_id = 0
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._connected = True
        self._initialized = False
        # Device-declared WebSocket protocol version (from the hello
        # message). Defaults to 1, which matches the firmware's default
        # (firmware/main/protocols/websocket_protocol.h: ``version_ = 1``)
        # and the audio framing this gateway emits today (raw Opus
        # payload). v2/v3 add a BinaryProtocol header that this gateway
        # does not yet wrap — see Issue follow-up to #70.
        self.protocol_version: int = 1
        self.cloud_proxy: Any | None = None
        self._speak_lock = asyncio.Lock()
        # Device-declared MCP support (from the hello ``features.mcp``
        # flag). Some chassis firmwares (e.g. ciniml/stackchan-idf's
        # XiaoZhiClient) speak the same xiaozhi hello/listen/tts dialect
        # but carry no MCP client at all — they send ``features.mcp:
        # false`` and never answer an "mcp" envelope. Defaults to True so
        # a connection built outside the hello handshake (as in tests)
        # keeps the historical "assume full MCP" behaviour.
        self.mcp_supported: bool = True
        self.transport: str = "websocket"
        self.camera_tracking_v1 = False
        self.camera_tracking_v2 = False
        self.camera_tracking_engaged = False
        # Firmware draws Grok Bot run states from {"type": "activity"}.
        self.bot_activity_v1 = False
        self._proxy_stop_task: asyncio.Task[None] | None = None

    @property
    def peer_host(self) -> str | None:
        """The transport peer, independent of device-supplied HTTP headers."""
        address = getattr(self._ws, "remote_address", None)
        return address[0] if isinstance(address, tuple) and address and isinstance(address[0], str) else None

    @property
    def connected(self) -> bool:
        return self._connected

    @property
    def initialized(self) -> bool:
        return self._initialized

    def _next_id(self) -> int:
        self._request_id += 1
        return self._request_id

    async def send_mcp_request(
        self, method: str, params: dict[str, Any]
    ) -> tuple[Any, dict[str, Any] | None]:
        """Send an MCP request to ESP32 and wait for response.

        Returns (result, error).
        """
        if not self._connected:
            return None, {"code": -32000, "message": "ESP32 not connected"}

        req_id = self._next_id()
        message = make_mcp_message(self.session_id, method, params, req_id)

        future: asyncio.Future[dict[str, Any]] = asyncio.get_event_loop().create_future()
        self._pending[req_id] = future

        try:
            await self._ws.send(json.dumps(message))
            response = await asyncio.wait_for(future, timeout=RESPONSE_TIMEOUT)
            return parse_jsonrpc_response(response)
        except asyncio.TimeoutError:
            self._pending.pop(req_id, None)
            return None, {"code": -32000, "message": f"Timeout waiting for ESP32 response (method={method})"}
        except Exception as exc:
            self._pending.pop(req_id, None)
            return None, {"code": -32000, "message": f"ESP32 communication error: {exc}"}

    async def initialize(self, vision_url: str = "", vision_token: str = "") -> bool:
        """Send MCP initialize to ESP32."""
        capabilities: dict[str, Any] = {}
        if vision_url:
            vision: dict[str, Any] = {"url": vision_url}
            if vision_token:
                vision["token"] = vision_token
            capabilities["vision"] = vision
        result, error = await self.send_mcp_request("initialize", {"capabilities": capabilities})
        if error:
            logger.error("ESP32 initialize failed: %s", error)
            return False

        logger.info(
            "ESP32 initialized: protocol=%s server=%s",
            result.get("protocolVersion", "?"),
            result.get("serverInfo", {}),
        )
        self._initialized = True
        return True

    async def discover_tools(self) -> list[dict[str, Any]]:
        """Discover tools available on ESP32."""
        all_tools: list[dict[str, Any]] = []
        cursor = ""

        while True:
            params: dict[str, Any] = {"cursor": cursor}
            result, error = await self.send_mcp_request("tools/list", params)

            if error:
                logger.error("tools/list failed: %s", error)
                break

            tools = result.get("tools", [])
            all_tools.extend(tools)

            next_cursor = result.get("nextCursor", "")
            if not next_cursor:
                break
            cursor = next_cursor

        self.tools = all_tools
        logger.info("Discovered %d tools on ESP32", len(all_tools))
        return all_tools

    async def call_tool(
        self, name: str, arguments: dict[str, Any]
    ) -> tuple[Any, dict[str, Any] | None]:
        """Call a tool on ESP32."""
        if not self.mcp_supported:
            return None, {
                "code": -32000,
                "message": "ESP32 MCP unsupported (features.mcp=false)",
            }
        return await self.send_mcp_request(
            "tools/call", {"name": name, "arguments": arguments}
        )

    async def send_emotion(self, emotion: str) -> dict[str, Any]:
        """Send a XiaoZhi ``llm.emotion`` message over the live WebSocket."""
        if not self._connected:
            raise ConnectionError("ESP32 not connected")
        message = {
            "session_id": self.session_id,
            "type": "llm",
            "emotion": emotion,
        }
        await self._ws_send(json.dumps(message, ensure_ascii=False))
        return {"ok": True, "emotion": emotion}

    async def send_led(self, r: int, g: int, b: int) -> dict[str, Any]:
        """Send a XiaoZhi ``led`` message over the live WebSocket."""
        if not self._connected:
            raise ConnectionError("ESP32 not connected")
        message = {
            "type": "led",
            "r": max(0, min(255, int(r))),
            "g": max(0, min(255, int(g))),
            "b": max(0, min(255, int(b))),
        }
        await self._ws_send(json.dumps(message))
        return {"ok": True, "r": message["r"], "g": message["g"], "b": message["b"]}

    async def send_head(self, yaw: int, pitch: int, speed: int) -> dict[str, Any]:
        """Send a XiaoZhi ``head`` message over the live WebSocket."""
        if not self._connected:
            raise ConnectionError("ESP32 not connected")
        message = {
            "type": "head",
            "yaw": max(-90, min(90, int(yaw))),
            "pitch": max(0, min(60, int(pitch))),
            "speed": max(100, min(1000, int(speed))),
        }
        await self._ws_send(json.dumps(message))
        return {
            "ok": True,
            "yaw": message["yaw"],
            "pitch": message["pitch"],
            "speed": message["speed"],
        }

    async def send_head_tracking(self, mode: str) -> None:
        if mode not in {"tracking", "engaged", "conversation", "off"}:
            raise ValueError("Invalid head tracking mode")
        if not self.connected or not self.camera_tracking_v1:
            raise ConnectionError("Camera tracking is unavailable")
        await asyncio.wait_for(self._ws_send(json.dumps({"type": "head_tracking", "mode": mode})), 1.0)

    async def send_head_track(
        self, frame_id: str, *, delta_yaw: float = 0, delta_pitch: float = 0, release: bool = False,
    ) -> None:
        """Send a best-effort correction; firmware validates and consumes the frame token."""
        if not self.connected or not self.camera_tracking_v1:
            raise ConnectionError("Camera tracking is unavailable")
        if not isinstance(frame_id, str) or not frame_id or len(frame_id) > 128:
            raise ValueError("Invalid tracking frame token")
        message: dict[str, Any] = {"type": "head_track", "frame_id": frame_id}
        if release:
            message["release"] = True
        else:
            if not math.isfinite(delta_yaw) or not math.isfinite(delta_pitch):
                raise ValueError("Tracking corrections must be finite")
            message.update(delta_yaw=max(-15.0, min(15.0, delta_yaw)),
                           delta_pitch=max(-10.0, min(10.0, delta_pitch)))
        await asyncio.wait_for(self._ws_send(json.dumps(message)), 1.0)

    async def send_activity(self, state: str, ttl_ms: int = 0) -> None:
        """Send a Grok Bot run state for the face; no-op on firmware without the feature."""
        if not self.connected or not self.bot_activity_v1:
            return
        message = {"type": "activity", "state": state, "ttl_ms": max(0, int(ttl_ms))}
        await asyncio.wait_for(self._ws_send(json.dumps(message)), 1.0)

    async def speak(self, text: str, emotion: str | None = None) -> SpeakResult:
        """Queue one TTS utterance and stream it to the ESP32."""

        async def _set_avatar(face: str) -> None:
            await self.call_tool("self.display.set_avatar", {"face": face})

        async with self._speak_lock:
            return await speak_text(
                self._ws.send,
                text,
                emotion=emotion,
                set_avatar=_set_avatar,
                cloud_proxy=self.cloud_proxy,
                session_id=self.session_id,
                require_cloud=_require_cloud_speak(),
            )

    def handle_response(self, payload: dict[str, Any]) -> bool:
        """Handle an incoming MCP response from ESP32."""
        req_id = payload.get("id")
        if req_id is not None and req_id in self._pending:
            future = self._pending.pop(req_id)
            if not future.done():
                future.set_result(payload)
            return True
        else:
            # Notification (no id) — log and discard for now
            method = payload.get("method", "")
            logger.info("ESP32 notification: %s", method)
            return False

    async def _ws_send(self, payload: bytes | str) -> None:
        """Send a payload, translating websockets errors to ConnectionError.

        The ``websockets`` library raises its own exception hierarchy
        (``ConnectionClosed`` and friends), which is *not* a subclass
        of the built-in :class:`ConnectionError`. Without translation
        the orchestrator's ``except ConnectionError`` filter — and the
        MCP handler's ``except RuntimeError`` filter — would let those
        errors leak as raw tracebacks into the MCP transport, breaking
        the say() tool's clean error JSON contract on mid-stream
        disconnect.
        """
        try:
            await self._ws.send(payload)
        except (
            websockets.exceptions.ConnectionClosed,
            OSError,
        ) as exc:
            # Mark the connection dead so subsequent calls fail fast
            # rather than each one re-discovering the broken socket.
            self.disconnect()
            raise ConnectionError(f"WebSocket send failed: {exc}") from exc

    async def send_audio_frame(self, opus_frame: bytes) -> None:
        """Send a single Opus frame to the ESP32 as a WebSocket binary frame.

        The device's ``OnData`` handler (firmware/main/protocols/
        websocket_protocol.cc) treats every binary frame as an Opus
        audio payload to feed into its decoder, so this method is the
        TTS pipeline's egress point.
        """
        if not self._connected:
            raise ConnectionError("ESP32 not connected")
        await self._ws_send(opus_frame)

    async def send_tts_state(self, state: str) -> None:
        """Send a TTS state notification (``start`` / ``stop`` / ...).

        The device's :func:`Application::OnIncomingJson` translates
        ``{"type":"tts","state":"start"}`` into
        :data:`kDeviceStateSpeaking`, which is the gate for
        :func:`OnIncomingAudio` pushing packets into the decode queue
        (see ``firmware/main/application.cc``). Without bracketing the
        audio frames in start/stop, the device drops them on the floor
        and the speaker stays silent — the TTS tool returns success
        without anything actually playing.
        """
        if not self._connected:
            raise ConnectionError("ESP32 not connected")
        message = {
            "session_id": self.session_id,
            "type": "tts",
            "state": state,
        }
        await self._ws_send(json.dumps(message))

    def disconnect(self) -> None:
        """Mark connection as disconnected."""
        self._connected = False
        self._initialized = False
        # Cancel all pending futures
        for future in self._pending.values():
            if not future.done():
                future.set_exception(ConnectionError("ESP32 disconnected"))
        self._pending.clear()


class _ConnectionBoundEndpoint:
    """Recheck device ownership when a cached voice-tool reference dispatches."""

    _METHODS = frozenset({"call_tool", "send_head", "send_led", "send_emotion", "on_end_conversation"})

    def __init__(self, target: Any, connection: ESP32Connection, is_current: Callable[[], bool]):
        self._target = target
        self._connection = connection
        self._is_current = is_current

    @property
    def connection(self) -> ESP32Connection | None:
        return self._connection if self._is_current() else None

    @property
    def device_connected(self) -> bool:
        return self._is_current() and bool(getattr(self._target, "device_connected", False))

    @property
    def connected(self) -> bool:
        return self._is_current() and bool(getattr(self._target, "connected", False))

    @property
    def mcp_supported(self) -> bool:
        return self._is_current() and self._connection.mcp_supported

    def __getattr__(self, name: str) -> Any:
        if name not in self._METHODS:
            raise AttributeError(name)
        callback = getattr(self._target, name, None)
        if not callable(callback):
            return None

        async def dispatch(*args, **kwargs):
            if not self._is_current():
                raise ConnectionError("Device connection is no longer current")
            result = callback(*args, **kwargs)
            return await result if inspect.isawaitable(result) else result

        return dispatch


class ESP32Manager:
    """Manages ESP32 device connections.

    Runs a WebSocket server that ESP32 devices connect to.
    Currently supports a single device connection.
    """

    def __init__(
        self,
        cloud_proxy_factory: Any | None = None,
        usb_transport: Any | None = None,
        local_tool_handler: Callable[[str, dict[str, Any]], Any] | None = None,
        debug_status: DebugStatus | None = None,
    ):
        self._debug_status = debug_status or get_debug_status()
        self._connection: ESP32Connection | None = None
        self._server: Any = None
        self._lock = asyncio.Lock()
        self._init_tasks: list[asyncio.Task] = []
        self._retirement_tasks: set[asyncio.Task[None]] = set()
        self._vision_url: str = ""
        self._vision_token: str = ""
        self.usb_transport = usb_transport
        # Per-device serialisation for TTS send sequences. Acquired by
        # the orchestrator around the entire start → frames → stop
        # block so concurrent ``say()`` invocations cannot interleave
        # their Opus frames on the same WebSocket or overlap their
        # ``tts.start``/``tts.stop`` notifications (which would yank
        # the firmware out of ``kDeviceStateSpeaking`` mid-utterance
        # and silently drop the remaining audio). The lock is scoped
        # to the manager because the manager owns the device today —
        # if multi-device support lands later, the lock should move
        # onto :class:`ESP32Connection` instead.
        self._tts_lock = asyncio.Lock()
        self._cloud_proxy_factory = cloud_proxy_factory or CloudProxy
        self._local_tool_handler = local_tool_handler
        self.on_activity: Callable[[], None] | None = None
        self.on_head_command: Callable[[], None] | None = None
        self.on_device_state: Callable[[str], None] | None = None
        self.on_wake_detected: Callable[[], Awaitable[None]] | None = None
        self.on_connection_changed: Callable[[], Awaitable[None]] | None = None
        self.on_tts_changed: Callable[[ESP32Connection], Awaitable[None]] | None = None

    @property
    def device_connected(self) -> bool:
        return self._connection is not None and self._connection.connected

    @property
    def mcp_supported(self) -> bool:
        """False only while a voice-only (features.mcp=false) device is connected."""
        conn = self._connection
        if conn is None or not conn.connected:
            return True
        return bool(getattr(conn, "mcp_supported", True))

    @property
    def usb_connected(self) -> bool:
        usb = self.usb_transport
        return usb is not None and bool(getattr(usb, "connected", False))

    @property
    def connection(self) -> ESP32Connection | None:
        return self._connection

    @property
    def tts_lock(self) -> asyncio.Lock:
        """Per-device lock guarding the TTS send sequence.

        See :attr:`_tts_lock` for the rationale; the orchestrator wraps
        the start → frames → stop block in ``async with`` on this lock.
        """
        return self._tts_lock

    def _websocket_voice_active(self) -> bool:
        conn = self._connection
        return (
            conn is not None
            and conn.connected
            and getattr(conn, "transport", "websocket") != "usb"
        )

    async def begin_usb_voice(
        self,
        *,
        sender: Any,
        hello: dict[str, Any],
        device_id: str,
    ) -> ESP32Connection | None:
        """Create the per-connection voice proxy the WebSocket hello path creates.

        Returns None when a live Wi-Fi voice session already owns the device,
        so USB voice does not replace a working WebSocket.
        """
        if self._websocket_voice_active():
            return None
        session_id = str(uuid.uuid4())
        connection = ESP32Connection(sender, session_id)
        connection.transport = "usb"
        connection.device_id = device_id or "usb"
        features = hello.get("features") if isinstance(hello, dict) else None
        if isinstance(features, dict):
            connection.mcp_supported = bool(features.get("mcp"))
            connection.camera_tracking_v1 = features.get("camera_tracking_v1") is True
            connection.camera_tracking_v2 = connection.camera_tracking_v1 and features.get("camera_tracking_v2") is True
            connection.camera_tracking_engaged = (
                connection.camera_tracking_v1 and features.get("camera_tracking_engaged_v1") is True
            )
            connection.bot_activity_v1 = features.get("bot_activity_v1") is True
        else:
            connection.mcp_supported = False
            connection.camera_tracking_v1 = False
        try:
            connection.protocol_version = int(hello.get("version", 1))
        except (TypeError, ValueError):
            connection.protocol_version = 1

        proxy = self._cloud_proxy_factory()
        connection.cloud_proxy = proxy
        bind = getattr(self, "_bind_proxy_callbacks", None)
        if callable(bind):
            bind(connection)
        info = CloudConnectionInfo(
            device_id=connection.device_id,
            protocol_version=str(connection.protocol_version),
        )
        try:
            if proxy is not None:
                await proxy.start(info, hello, sender.send)
            async with self._lock:
                if self._websocket_voice_active():
                    raise ConnectionError("WebSocket voice became active during USB startup")
                self._connection = connection
                self._debug_status.on_device_connected(connection.device_id)
            if connection.bot_activity_v1:
                bot_activity.tracker().resend()
            resp = HelloResponse(session_id=session_id, transport="usb")
            await sender.send(resp.model_dump_json())
            changed = getattr(self, "on_connection_changed", None)
            if callable(changed):
                await changed()
        except BaseException as exc:
            connection.disconnect()
            await self.release_connection(connection)
            if proxy is not None:
                stop = getattr(proxy, "stop", None)
                if callable(stop):
                    try:
                        await stop()
                    except Exception:
                        logger.exception("Failed USB voice startup cleanup")
            if isinstance(exc, asyncio.CancelledError):
                raise
            logger.warning("USB voice session failed to start: %s", exc)
            return None
        if not connection.mcp_supported:
            logger.info("USB voice ready (voice-only): device=%s", connection.device_id)
        return connection

    async def release_connection(self, connection: ESP32Connection) -> None:
        """Drop ``connection`` if it is still the current device."""
        if self._connection is not connection:
            return
        self._connection = None
        self._debug_status.on_device_disconnected()
        self._debug_status.on_tts_state(False, source="local")
        self._debug_status.on_tts_state(False, source="device")
        changed = getattr(self, "on_connection_changed", None)
        if callable(changed):
            await changed()

    def observe_device_audio(self, connection: ESP32Connection) -> None:
        if self._connection is connection and connection.connected:
            self._debug_status.on_device_audio()

    async def observe_device_message(self, connection: ESP32Connection, message: dict[str, Any]) -> None:
        """Apply listen/tts state from a USB voice JSON frame."""
        if self._connection is not connection or not connection.connected:
            return
        self._observe_device_state(message)
        callback = getattr(self, "on_tts_changed", None)
        if message.get("type") == "tts" and callable(callback):
            await callback(connection)

    def get_status_with_transports(self) -> dict[str, Any]:
        """Get status for callers that can use either WebSocket or USB."""
        status = dict(self.get_status())
        conn = self._connection
        if conn is not None and conn.connected and getattr(conn, "transport", "websocket") == "usb":
            status["connected"] = True
            status["ws_connected"] = False
            status["usb_connected"] = True
            status["transport"] = "usb"
            return status
        ws_connected = bool(status.get("connected"))
        usb_connected = self.usb_connected
        status["ws_connected"] = ws_connected
        status["usb_connected"] = usb_connected
        if usb_connected and not ws_connected:
            status["connected"] = True
            status["transport"] = "usb"
        elif ws_connected:
            status["transport"] = "websocket"
        else:
            status["transport"] = None
        return status

    async def start(
        self,
        host: str = "0.0.0.0",
        port: int = 8765,
        vision_url: str = "",
        vision_token: str = "",
    ) -> None:
        """Start the WebSocket server for ESP32 connections."""
        self._vision_url = vision_url
        self._vision_token = vision_token
        logger.info("ESP32 WebSocket server starting on ws://%s:%d", host, port)
        self._attach_activity_sink(asyncio.get_running_loop())
        self._server = await websockets.serve(
            self._handler,
            host,
            port,
            process_request=self._check_auth,
        )

    async def stop(self) -> None:
        """Stop the WebSocket server."""
        bot_activity.tracker().set_sink(None)
        # Cancel any pending initialization tasks
        for task in self._init_tasks:
            task.cancel()
        self._init_tasks.clear()

        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        if self._retirement_tasks:
            await asyncio.gather(*list(self._retirement_tasks), return_exceptions=True)

    def _check_auth(
        self, connection: ServerConnection, request: websockets.http11.Request
    ) -> None | websockets.http11.Response:
        """Validate Bearer token.

        websockets 16+ passes (connection, request) to process_request.
        """
        expected = os.getenv("STACKCHAN_TOKEN") or os.getenv("BEARER_TOKEN")
        if not expected:
            logger.warning("STACKCHAN_TOKEN not set — accepting all connections")
            return None

        auth = request.headers.get("Authorization", "")
        if auth == f"Bearer {expected}":
            return None

        logger.warning("ESP32 auth rejected")
        return websockets.http11.Response(
            401, "Unauthorized", websockets.datastructures.Headers()
        )

    def _bind_proxy_callbacks(self, connection: ESP32Connection) -> None:
        """Keep callbacks captured by a voice bridge scoped to its device."""
        proxy = connection.cloud_proxy
        if proxy is None:
            return

        def current() -> bool:
            return connection.connected and self._connection is connection

        for name in ("on_head_command", "on_device_state"):
            callback = getattr(proxy, name, None)
            if callable(callback):
                def guarded(*args, _callback=callback, **kwargs):
                    if current():
                        return _callback(*args, **kwargs)
                setattr(proxy, name, guarded)

        for name in ("on_conversation_activity", "set_face_tracking"):
            callback = getattr(proxy, name, None)
            if callable(callback):
                async def guarded_async(*args, _callback=callback, _name=name, **kwargs):
                    if not current():
                        if _name == "set_face_tracking":
                            return {"ok": False, "error": "Device connection is no longer current"}
                        return None
                    result = _callback(*args, **kwargs)
                    return await result if inspect.isawaitable(result) else result
                setattr(proxy, name, guarded_async)

        esp32_ref = getattr(proxy, "esp32_ref", None)
        if callable(esp32_ref):
            device_endpoint = None

            def bound_device():
                nonlocal device_endpoint
                if not connection.connected:
                    return None
                if device_endpoint is None:
                    target = esp32_ref()
                    if target is None:
                        return None
                    device_endpoint = _ConnectionBoundEndpoint(target, connection, current)
                return device_endpoint

            # start() can construct its bridge before registration, but tools
            # captured by that bridge can dispatch only for the current socket.
            proxy.esp32_ref = bound_device
        usb = getattr(proxy, "usb_transport", None)
        if usb is not None:
            proxy.usb_transport = _ConnectionBoundEndpoint(usb, connection, current)

    def _start_proxy_stop(self, connection: ESP32Connection) -> asyncio.Task[None] | None:
        if connection.cloud_proxy is None:
            return None
        if connection._proxy_stop_task is None:
            retire = getattr(connection.cloud_proxy, "retire", None)
            if callable(retire):
                retire()

            async def stop_proxy() -> None:
                try:
                    await connection.cloud_proxy.stop()
                except Exception:
                    logger.exception("Retired voice proxy cleanup failed")

            task = asyncio.create_task(stop_proxy(), name="retired-device-voice")
            connection._proxy_stop_task = task
            self._retirement_tasks.add(task)
            task.add_done_callback(self._retirement_tasks.discard)
        return connection._proxy_stop_task

    def _retire_connection(self, connection: ESP32Connection) -> None:
        connection.disconnect()
        self._start_proxy_stop(connection)

        async def close_socket() -> None:
            try:
                await asyncio.wait_for(
                    connection._ws.close(code=1000, reason="Replaced by a new device connection"),
                    timeout=1.0,
                )
            except (asyncio.TimeoutError, OSError, websockets.exceptions.ConnectionClosed):
                transport = getattr(connection._ws, "transport", None)
                if transport is not None:
                    transport.abort()

        task = asyncio.create_task(close_socket(), name="retired-device-socket")
        self._retirement_tasks.add(task)
        task.add_done_callback(self._retirement_tasks.discard)

    async def _handler(self, ws: ServerConnection) -> None:
        """Handle an incoming ESP32 WebSocket connection.

        Architecture: the message read loop runs continuously, dispatching
        MCP responses to pending futures. Initialization (initialize + tools/list)
        runs as a separate task so it doesn't block the read loop.
        """
        session_id = str(uuid.uuid4())
        request_headers = ws.request.headers if ws.request else {}
        device_id = request_headers.get("Device-Id", "unknown")
        logger.info("ESP32 connecting: device=%s", device_id)

        connection = ESP32Connection(ws, session_id)
        connection.device_id = device_id
        cloud_proxy: CloudProxy | None = self._cloud_proxy_factory()
        connection.cloud_proxy = cloud_proxy
        self._bind_proxy_callbacks(connection)
        registered = False

        try:
            async for message in ws:
                if isinstance(message, bytes):
                    if connection is not self._connection or not connection.connected:
                        continue
                    self._debug_status.on_device_audio()
                    await handle_audio_frame(
                        message,
                        session_id,
                        cloud_proxy=cloud_proxy,
                    )
                    continue

                try:
                    data = json.loads(message)
                except json.JSONDecodeError:
                    logger.warning("Invalid JSON from ESP32: %s", str(message)[:100])
                    continue

                if not isinstance(data, dict):
                    continue

                msg_type = data.get("type", "")
                if msg_type not in {"hello", "mcp"} and (
                    connection is not self._connection or not connection.connected
                ):
                    continue
                # A replaced socket can still deliver queued TTS events. Only
                # the current device may update its speaking state.
                if connection is self._connection and connection.connected:
                    self._observe_device_state(data)
                    if msg_type == "tts" and self.on_tts_changed is not None:
                        await self.on_tts_changed(connection)

                if msg_type == "hello":
                    if registered or not connection.connected:
                        continue
                    # ESP32 hello handshake
                    features = data.get("features", {})
                    if not isinstance(features, dict):
                        features = {}
                    mcp_supported = bool(features.get("mcp"))
                    connection.mcp_supported = mcp_supported
                    connection.camera_tracking_v1 = features.get("camera_tracking_v1") is True
                    connection.camera_tracking_v2 = connection.camera_tracking_v1 and features.get("camera_tracking_v2") is True
                    connection.camera_tracking_engaged = (
                        connection.camera_tracking_v1 and features.get("camera_tracking_engaged_v1") is True
                    )
                    connection.bot_activity_v1 = features.get("bot_activity_v1") is True
                    if not mcp_supported:
                        # Voice-only chassis (e.g. ciniml/stackchan-idf's
                        # XiaoZhiClient): accept the connection for audio +
                        # xiaozhi control JSON, but never attempt MCP
                        # initialize/tools_list — the device has no MCP
                        # client to answer them, and RESPONSE_TIMEOUT would
                        # otherwise stall the init task for 300 s per
                        # connection for nothing.
                        logger.info(
                            "ESP32 hello without MCP support (features.mcp="
                            "false); accepting as voice-only, skipping "
                            "initialize/tools_list"
                        )

                    # Capture the device's WebSocket protocol version
                    # so callers (e.g. the TTS pipeline) can decide
                    # whether their wire format is compatible. The
                    # firmware accepts raw Opus only on v1; v2/v3 wrap
                    # the payload in a BinaryProtocol header.
                    raw_version = data.get("version", 1)
                    try:
                        connection.protocol_version = int(raw_version)
                    except (TypeError, ValueError):
                        connection.protocol_version = 1
                    if connection.protocol_version != 1:
                        logger.warning(
                            "ESP32 negotiated WebSocket protocol "
                            "version=%s; the gateway emits raw Opus "
                            "binary frames matching v1 only. TTS "
                            "calls (say) will be blocked at the "
                            "orchestrator until v2/v3 BinaryProtocol "
                            "header wrapping is implemented",
                            connection.protocol_version,
                        )

                    if cloud_proxy is not None:
                        cloud_info = CloudConnectionInfo.from_headers(request_headers)
                        await cloud_proxy.start(cloud_info, data, ws.send)

                    # Send hello response only after the cloud socket has had a
                    # chance to connect and receive the ESP32 hello.  The ESP32
                    # starts streaming wake-word Opus immediately after server
                    # hello; replying too early drops the first listen/audio
                    # frames while the upstream cloud WebSocket is still opening.
                    resp = HelloResponse(session_id=session_id)
                    await ws.send(resp.model_dump_json())

                    # Register connection
                    async with self._lock:
                        if self._connection is not None and self._connection is not connection:
                            if self._connection.connected:
                                logger.warning("Replacing existing ESP32 connection")
                            self._retire_connection(self._connection)
                            # The old handler's finally no longer owns _connection,
                            # so release its device and local speaking sources now.
                            self._debug_status.on_tts_state(False, source="device")
                            self._debug_status.on_tts_state(False, source="local")
                        self._connection = connection
                        registered = True
                        self._debug_status.on_device_connected(device_id)
                    if connection.bot_activity_v1:
                        # A robot that (re)connects mid-run shows the run.
                        bot_activity.tracker().resend()

                    if self.on_connection_changed is not None:
                        await self.on_connection_changed()

                    if mcp_supported:
                        # Start initialization as a separate task so the read
                        # loop continues to pump messages (responses to
                        # initialize/tools_list)
                        task = asyncio.create_task(self._init_device(connection, device_id))
                        self._init_tasks.append(task)
                        task.add_done_callback(lambda t: self._init_tasks.remove(t) if t in self._init_tasks else None)
                    else:
                        logger.info("ESP32 ready (voice-only): device=%s", device_id)

                elif msg_type == "mcp":
                    if connection is not self._connection:
                        if not registered and connection.connected:
                            await self._handle_external_mcp_request(ws, data, session_id)
                        continue

                    # MCP response from ESP32
                    if cloud_proxy is not None and await cloud_proxy.try_handle_device_mcp(data):
                        continue
                    payload = data.get("payload", {})
                    handled_locally = connection.handle_response(payload)
                    if not handled_locally and cloud_proxy is not None:
                        await cloud_proxy.send_device_json(data)

                else:
                    if cloud_proxy is not None:
                        forwarded = await cloud_proxy.send_device_json(data)
                        if forwarded:
                            logger.debug("ESP32 message type=%s forwarded to cloud", msg_type)
                            continue
                    logger.debug("ESP32 message type=%s (ignored)", msg_type)

        except websockets.exceptions.ConnectionClosed:
            logger.info("ESP32 disconnected: device=%s", device_id)
        finally:
            connection.disconnect()
            stop_task = self._start_proxy_stop(connection)
            if stop_task is not None:
                await asyncio.shield(stop_task)
            async with self._lock:
                if self._connection is connection:
                    self._connection = None
                    self._debug_status.on_device_disconnected()
                    self._debug_status.on_tts_state(False, source="local")
                    self._debug_status.on_tts_state(False, source="device")
            if self._connection is None and self.on_connection_changed is not None:
                await self.on_connection_changed()

    async def _init_device(self, connection: ESP32Connection, device_id: str) -> None:
        """Initialize MCP session with a newly connected device."""
        if await connection.initialize(
            vision_url=self._vision_url,
            vision_token=self._vision_token,
        ):
            await connection.discover_tools()
            logger.info(
                "ESP32 ready: device=%s tools=%d",
                device_id,
                len(connection.tools),
            )
        else:
            logger.error("ESP32 MCP initialization failed")

    async def _handle_external_mcp_request(
        self,
        ws: ServerConnection,
        data: dict[str, Any],
        session_id: str,
    ) -> None:
        """Relay a hook/automation MCP request to the connected ESP32.

        The WebSocket server primarily serves the ESP32 client, but lightweight
        hook processes also use it as a command socket.  Those clients do not
        send the ESP32 hello handshake; they send one or more MCP tool calls and
        expect JSON-RPC responses on the same socket.
        """
        payload = data.get("payload", {})
        req_id = payload.get("id")
        method = payload.get("method", "")
        response_payload: dict[str, Any]

        if method == "initialize":
            response_payload = {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "serverInfo": {"name": "stackchan-hook-bridge", "version": "0.1.0"},
                    "capabilities": {"tools": {}},
                },
            }
        elif method == "tools/list":
            response_payload = {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {"tools": self._connection.tools if self._connection else [], "nextCursor": ""},
            }
        elif method == "tools/call":
            params = payload.get("params", {})
            tool_name = params.get("name", "")
            arguments = params.get("arguments", {})
            if tool_name == "get_status":
                result = {
                    "content": [
                        {
                            "type": "text",
                            "text": json.dumps(self.get_status_with_transports(), ensure_ascii=False),
                        }
                    ],
                    "isError": False,
                }
                error = None
            elif self._local_tool_handler is not None and tool_name in {"say", "set_voice_mode", "gemini_say", "set_face_tracking"}:
                try:
                    local_result = self._local_tool_handler(
                        tool_name,
                        arguments if isinstance(arguments, dict) else {},
                    )
                    if inspect.isawaitable(local_result):
                        local_result = await local_result
                    result = {
                        "content": [
                            {
                                "type": "text",
                                "text": json.dumps(local_result, ensure_ascii=False),
                            }
                        ],
                        "isError": False,
                    }
                    error = None
                except Exception as exc:
                    result = None
                    error = {"code": -32000, "message": str(exc)}
            elif tool_name == "speak":
                result, error = await self.speak(
                    str(arguments.get("text", "")) if isinstance(arguments, dict) else "",
                    str(arguments.get("emotion")) if isinstance(arguments, dict) and arguments.get("emotion") else None,
                )
            else:
                tool_args = arguments if isinstance(arguments, dict) else {}
                result, error = None, None
                if self.usb_connected:
                    try:
                        usb_response = await self.usb_transport.call_tool(tool_name, tool_args, timeout_s=2.0)
                        result = usb_response.get("result", usb_response)
                    except Exception as exc:
                        if self.device_connected:
                            logger.debug("external MCP USB %s failed; falling back to WS: %s", tool_name, exc)
                        else:
                            error = {"code": -32000, "message": str(exc)}
                if result is None and error is None:
                    result, error = await self.call_tool(tool_name, tool_args)
            if error:
                response_payload = {"jsonrpc": "2.0", "id": req_id, "error": error}
            else:
                response_payload = {"jsonrpc": "2.0", "id": req_id, "result": result}
        else:
            response_payload = {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32601, "message": f"Method not found: {method}"},
            }

        await ws.send(
            json.dumps(
                {
                    "session_id": data.get("session_id", session_id),
                    "type": "mcp",
                    "payload": response_payload,
                }
            )
        )

    async def call_tool(
        self,
        name: str,
        arguments: dict[str, Any],
        *,
        notify_activity: bool = True,
        notify_head_command: bool = True,
    ) -> tuple[Any, dict[str, Any] | None]:
        """Call a tool on the connected ESP32 device."""
        if not self._connection or not self._connection.connected:
            return None, {"code": -32000, "message": "No ESP32 device connected"}
        if not self._connection.mcp_supported:
            return None, {
                "code": -32000,
                "message": "ESP32 MCP unsupported (features.mcp=false)",
            }
        if not self._connection.initialized:
            return None, {"code": -32000, "message": "ESP32 not initialized"}
        if notify_activity and self.on_activity is not None:
            self.on_activity()
        if (
            notify_head_command
            and name == "self.robot.set_head_angles"
            and self.on_head_command is not None
        ):
            self.on_head_command()
        return await self._connection.call_tool(name, arguments)

    async def send_activity(self, state: str, ttl_ms: int = 0) -> None:
        """Grok Bot run state for the connected device's face (best effort)."""
        connection = self._connection
        if connection is None or not connection.connected:
            return
        try:
            await connection.send_activity(state, ttl_ms)
        except Exception as exc:  # noqa: BLE001 - the face is best effort
            logger.debug("activity %s not sent: %s", state, exc)

    def _attach_activity_sink(self, loop: asyncio.AbstractEventLoop) -> None:
        """Route bot_activity updates (any thread) onto this event loop."""

        def sink(state: str, ttl_ms: int) -> None:
            if loop.is_closed():
                return
            asyncio.run_coroutine_threadsafe(self.send_activity(state, ttl_ms), loop)

        bot_activity.tracker().set_sink(sink)

    async def send_emotion(
        self,
        emotion: str,
        *,
        notify_activity: bool = True,
    ) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
        """Send a non-MCP expression update to the connected device."""
        if not self._connection or not self._connection.connected:
            return None, {"code": -32000, "message": "No ESP32 device connected"}
        if notify_activity and self.on_activity is not None:
            self.on_activity()
        try:
            return await self._connection.send_emotion(emotion), None
        except Exception as exc:
            return None, {"code": -32000, "message": str(exc)}

    async def send_led(
        self,
        r: int,
        g: int,
        b: int,
        *,
        notify_activity: bool = True,
    ) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
        """Send a non-MCP LED update to the connected device."""
        if not self._connection or not self._connection.connected:
            return None, {"code": -32000, "message": "No ESP32 device connected"}
        if notify_activity and self.on_activity is not None:
            self.on_activity()
        try:
            return await self._connection.send_led(r, g, b), None
        except Exception as exc:
            return None, {"code": -32000, "message": str(exc)}

    async def send_head(
        self,
        yaw: int,
        pitch: int,
        speed: int,
        *,
        notify_activity: bool = True,
        notify_head_command: bool = True,
    ) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
        """Send a non-MCP head update to the connected device."""
        if not self._connection or not self._connection.connected:
            return None, {"code": -32000, "message": "No ESP32 device connected"}
        if notify_activity and self.on_activity is not None:
            self.on_activity()
        if notify_head_command and self.on_head_command is not None:
            self.on_head_command()
        try:
            return await self._connection.send_head(yaw, pitch, speed), None
        except Exception as exc:
            return None, {"code": -32000, "message": str(exc)}

    async def send_audio_frame(self, opus_frame: bytes) -> None:
        """Push a single Opus frame to the connected device.

        Used by the TTS pipeline to deliver synthesised audio. Raises
        :class:`ConnectionError` if no device is currently attached so
        the orchestrator can surface a clean error to the MCP client
        instead of silently dropping audio.
        """
        if not self._connection or not self._connection.connected:
            raise ConnectionError("No ESP32 device connected")
        await self._connection.send_audio_frame(opus_frame)

    async def send_tts_state(self, state: str) -> None:
        """Send a TTS state notification (``start`` / ``stop`` / ...).

        Required around audio frame egress so the device transitions
        into ``kDeviceStateSpeaking`` and back; see
        :meth:`ESP32Connection.send_tts_state` for the full rationale.
        """
        connection = self._connection
        if not connection or not connection.connected:
            raise ConnectionError("No ESP32 device connected")
        self._observe_outgoing_tts_state(state)
        if state == "start":
            self._debug_status.on_tts_state(True, source="local")
        try:
            if state == "start" and self.on_tts_changed is not None:
                await self.on_tts_changed(connection)
            await connection.send_tts_state(state)
        except BaseException:
            if state == "start" and self._connection is connection:
                self._debug_status.on_tts_state(False, source="local")
            raise
        finally:
            if state == "stop" and self._connection is connection:
                self._debug_status.on_tts_state(False, source="local")
            if self._connection is connection and self.on_tts_changed is not None:
                await self.on_tts_changed(connection)

    async def speak(self, text: str, emotion: str | None = None) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
        """Speak through the connected ESP32 device."""
        if not self._connection or not self._connection.connected:
            return None, {"code": -32000, "message": "No ESP32 device connected"}
        if not self._connection.initialized:
            return None, {"code": -32000, "message": "ESP32 not initialized"}
        if self.on_activity is not None:
            self.on_activity()
        result = await self._connection.speak(text, emotion)
        if not result.ok:
            return result.to_dict(), {"code": -32000, "message": result.error or "speak failed"}
        return result.to_dict(), None

    def _observe_device_state(self, message: dict[str, Any]) -> None:
        callback = self.on_device_state
        msg_type = message.get("type", "")
        state = str(message.get("state", "")).lower()
        if msg_type == "listen":
            if state in {"start", "detect"}:
                if callback is not None:
                    callback("listening")
                if state == "detect" and self.on_wake_detected is not None:
                    asyncio.create_task(self.on_wake_detected())
            elif state == "stop":
                if callback is not None:
                    callback("idle")
        elif msg_type == "tts":
            if state == "start":
                self._debug_status.on_tts_state(True, source="device")
                if callback is not None:
                    callback("speaking")
            elif state == "stop":
                self._debug_status.on_tts_state(False, source="device")
                if callback is not None:
                    callback("idle")

    def _observe_outgoing_tts_state(self, state: str) -> None:
        callback = self.on_device_state
        if callback is None:
            return
        normalized = state.lower()
        if normalized == "start":
            callback("speaking")
        elif normalized == "stop":
            callback("idle")

    def get_status(self) -> dict[str, Any]:
        """Get current connection status."""
        if not self._connection or not self._connection.connected:
            return {
                "connected": False,
                "device_id": None,
                "tools_count": 0,
            }
        return {
            "connected": True,
            "device_id": self._connection.device_id,
            "initialized": self._connection.initialized,
            "mcp_supported": self._connection.mcp_supported,
            "voice_proxy": (
                "connected"
                if self._connection.cloud_proxy is not None
                and getattr(self._connection.cloud_proxy, "connected", False)
                else "inactive"
            ),
            "tools_count": len(self._connection.tools),
            "tools": [t.get("name", "") for t in self._connection.tools],
        }
