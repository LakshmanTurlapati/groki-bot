# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""USB voice session: same per-connection proxy the WebSocket handler builds.

The host signals ``voice/mode`` on the existing ``$MCP:`` line. The device
answers, then sends the XiaoZhi hello as a voice frame. Opus uplink and
tts/listen JSON use the same frames the WebSocket carries as binary and text.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from typing import Any

from .audio_stream import handle_audio_frame
from .usb_voice_framer import (
    AUDIO_DOWNLINK,
    AUDIO_UPLINK,
    VOICE_JSON,
    UsbVoiceFrame,
    encode_usb_voice_frame,
)

logger = logging.getLogger(__name__)


def usb_voice_mode() -> str:
    """``auto`` (default), ``on``, or ``off``.

    ``auto`` leaves a live Wi-Fi voice session alone and only signals USB
    voice after a short defer, so a robot that can reach the gateway over
    Wi-Fi keeps that path. ``on`` signals as soon as the cable is up.
    ``/say`` does not wait for the defer: with the cable up and no WebSocket
    session it opens USB voice immediately.
    """
    raw = os.getenv("STACKCHAN_USB_VOICE", "auto").strip().lower()
    if raw in {"0", "off", "false", "no"}:
        return "off"
    if raw in {"1", "on", "true", "yes"}:
        return "on"
    return "auto"


def usb_voice_defer_s() -> float:
    try:
        return max(0.0, float(os.getenv("STACKCHAN_USB_VOICE_DEFER_S", "8")))
    except ValueError:
        return 8.0


class UsbVoiceSender:
    """Object the ESP32 connection uses in place of a WebSocket.

    Bytes are Opus downlink. Strings are XiaoZhi JSON (hello, tts, llm, led, head).
    """

    def __init__(self, transport: Any) -> None:
        self._transport = transport

    async def send(self, payload: bytes | str) -> None:
        if isinstance(payload, bytes):
            frame_type = AUDIO_DOWNLINK
            body = payload
        else:
            frame_type = VOICE_JSON
            body = payload.encode("utf-8")
        frame = encode_usb_voice_frame(frame_type, body)
        if not frame:
            raise ConnectionError("USB voice frame is larger than an Opus packet")
        write = getattr(self._transport, "write_raw", None)
        if write is None:
            raise ConnectionError("USB transport cannot carry voice frames")
        await write(frame)


class UsbVoiceSession:
    """One USB device, one voice proxy, created the way the WS handler does."""

    def __init__(self, esp32: Any, transport: Any | None) -> None:
        self._esp32 = esp32
        self._transport = transport
        self._connection: Any | None = None
        self._ready = asyncio.Event()
        self._stop = asyncio.Event()
        self._attach_lock = asyncio.Lock()
        self._hello_lock = asyncio.Lock()
        self._task: asyncio.Task | None = None
        self._usb_since: float | None = None
        self._attached = False

    @property
    def connection(self) -> Any | None:
        return self._connection

    def start(self) -> None:
        transport = self._transport
        if transport is None or self._task is not None:
            return
        add = getattr(transport, "add_voice_frame_handler", None)
        if add is not None:
            add(self._on_frame)
        self._stop.clear()
        self._task = asyncio.create_task(self._supervise(), name="usb-voice")

    async def stop(self) -> None:
        self._stop.set()
        task = self._task
        self._task = None
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        await self._detach()

    def _ws_voice_active(self) -> bool:
        conn = getattr(self._esp32, "connection", None)
        if conn is None or not getattr(conn, "connected", False):
            return False
        return getattr(conn, "transport", "websocket") != "usb"

    async def ensure_session(self, *, timeout_s: float = 8.0) -> Any | None:
        """Open or reuse the voice proxy. Does not require the wake word.

        A live WebSocket session wins, so ``/say`` keeps using Wi-Fi when the
        robot is already connected there. Otherwise a connected USB device is
        signaled immediately, without waiting for ``STACKCHAN_USB_VOICE_DEFER_S``.
        """
        if usb_voice_mode() == "off":
            return None
        proxy = self._live_proxy()
        if proxy is not None:
            return proxy
        if self._ws_voice_active():
            return None
        transport = self._transport
        if transport is None or not getattr(transport, "connected", False):
            return None
        return await self._signal_and_wait(timeout_s)

    async def _supervise(self) -> None:
        while not self._stop.is_set():
            try:
                await self._tick()
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("USB voice supervise failed")
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=0.25)
            except asyncio.TimeoutError:
                pass

    async def _tick(self) -> None:
        mode = usb_voice_mode()
        transport = self._transport
        connected = transport is not None and bool(getattr(transport, "connected", False))
        if mode == "off" or not connected:
            self._usb_since = None
            if self._attached:
                await self._detach()
            return
        if self._ws_voice_active():
            # Wi-Fi voice is up. Do not signal USB voice over the top of it.
            self._usb_since = None
            return
        if self._live_proxy() is not None:
            return
        if mode == "on":
            await self._signal_and_wait(8.0)
            return
        now = time.monotonic()
        if self._usb_since is None:
            self._usb_since = now
        if now - self._usb_since >= usb_voice_defer_s():
            await self._signal_and_wait(8.0)

    def _live_proxy(self) -> Any | None:
        conn = self._connection
        if conn is None or not getattr(conn, "connected", False):
            return None
        proxy = getattr(conn, "cloud_proxy", None)
        if getattr(proxy, "speak_text", None) is None:
            return None
        return proxy

    async def _signal_and_wait(self, timeout_s: float) -> Any | None:
        async with self._attach_lock:
            proxy = self._live_proxy()
            if proxy is not None:
                return proxy
            if self._ws_voice_active():
                return None
            transport = self._transport
            if transport is None or not getattr(transport, "connected", False):
                return None
            self._ready.clear()
            try:
                await transport.set_voice_mode(True)
            except Exception as exc:
                logger.warning("USB voice/mode signal failed: %s", exc)
                return None
            try:
                await asyncio.wait_for(self._ready.wait(), timeout=timeout_s)
            except asyncio.TimeoutError:
                logger.warning("USB voice hello did not arrive within %.1fs", timeout_s)
                return None
            return self._live_proxy()

    def _on_frame(self, frame: UsbVoiceFrame) -> None:
        asyncio.create_task(self._handle_frame(frame))

    async def _handle_frame(self, frame: UsbVoiceFrame) -> None:
        if frame.type == AUDIO_UPLINK:
            conn = self._connection
            proxy = getattr(conn, "cloud_proxy", None) if conn is not None else None
            if conn is None or proxy is None:
                return
            await handle_audio_frame(frame.payload, conn.session_id, cloud_proxy=proxy)
            return
        if frame.type != VOICE_JSON or not frame.payload:
            return
        try:
            data = json.loads(frame.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            logger.warning("USB voice JSON was not valid")
            return
        if not isinstance(data, dict):
            return
        if data.get("type") == "hello":
            await self._accept_hello(data)
            return
        conn = self._connection
        if conn is None:
            return
        observe = getattr(self._esp32, "observe_device_message", None)
        if observe is not None:
            observe(data)
        proxy = getattr(conn, "cloud_proxy", None)
        if proxy is not None:
            await proxy.send_device_json(data)

    async def _accept_hello(self, hello: dict[str, Any]) -> None:
        async with self._hello_lock:
            if self._ws_voice_active():
                logger.info("USB hello ignored; WebSocket voice session is active")
                return
            begin = getattr(self._esp32, "begin_usb_voice", None)
            if begin is None:
                return
            sender = UsbVoiceSender(self._transport)
            device_id = str(hello.get("device_id") or "usb")
            connection = await begin(sender=sender, hello=hello, device_id=device_id)
            if connection is None:
                return
            previous = self._connection
            self._connection = connection
            self._attached = True
            self._ready.set()
            if previous is not None and previous is not connection:
                proxy = getattr(previous, "cloud_proxy", None)
                if proxy is not None:
                    await proxy.stop()
                previous.disconnect()

    async def _detach(self) -> None:
        conn = self._connection
        self._connection = None
        self._attached = False
        self._ready.clear()
        if conn is None:
            return
        proxy = getattr(conn, "cloud_proxy", None)
        if proxy is not None:
            stop = getattr(proxy, "stop", None)
            if stop is not None:
                await stop()
        conn.disconnect()
        release = getattr(self._esp32, "release_connection", None)
        if release is not None:
            release(conn)
