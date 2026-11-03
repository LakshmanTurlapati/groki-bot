# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""USB voice session: same per-connection proxy the WebSocket handler builds.

The host signals ``voice/mode`` on the existing ``$MCP:`` line. The device
answers, then sends the XiaoZhi hello as a voice frame. Opus uplink and
tts/listen JSON use the same frames the WebSocket carries as binary and text.
"""

from __future__ import annotations

import asyncio
import inspect
import json
import logging
import os
import time
from collections.abc import Callable
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

    remote_address = None

    def __init__(self, transport: Any, is_current: Callable[[], bool] = lambda: True) -> None:
        self._transport = transport
        self._is_current = is_current
        self._closed = False

    async def close(self, *, code: int = 1000, reason: str = "") -> None:
        # Retiring a voice connection must leave the shared USB tool link open.
        self._closed = True

    async def send(self, payload: bytes | str) -> None:
        if self._closed or not self._is_current() or not getattr(self._transport, "connected", False):
            raise ConnectionError("USB voice connection is no longer current")
        if isinstance(payload, bytes):
            frame_type = AUDIO_DOWNLINK
            body = payload
        else:
            frame_type = VOICE_JSON
            body = payload.encode("utf-8")
        frame = encode_usb_voice_frame(frame_type, body)
        if not frame:
            raise ConnectionError("USB voice frame exceeds payload limit")
        write = getattr(self._transport, "write_raw", None)
        if write is None:
            raise ConnectionError("USB transport cannot carry voice frames")
        try:
            await write(frame, is_current=lambda: not self._closed and self._is_current())
        except (RuntimeError, OSError) as exc:
            raise ConnectionError("USB voice write failed") from exc


class UsbVoiceSession:
    """One ordered, bounded voice receiver tied to a leased USB connection."""

    HEARTBEAT_S = 2.0
    MAX_QUEUED_FRAMES = 32
    MAX_AUDIO_AGE_S = 1.0

    def __init__(self, esp32: Any, transport: Any | None) -> None:
        self._esp32 = esp32
        self._transport = transport
        self._connection: Any | None = None
        self._ready = asyncio.Event()
        self._stop = asyncio.Event()
        self._stop.set()
        self._lease_ready = asyncio.Event()
        self._attach_lock = asyncio.Lock()
        self._detach_lock = asyncio.Lock()
        self._frames: asyncio.Queue = asyncio.Queue(maxsize=self.MAX_QUEUED_FRAMES)
        self._task: asyncio.Task | None = None
        self._receiver: asyncio.Task | None = None
        self._heartbeat: asyncio.Task | None = None
        self._opening: asyncio.Task | None = None
        self._reset_task: asyncio.Task | None = None
        self._cleanup_tasks: set[asyncio.Task] = set()
        self._handler_registered = False
        self._usb_since: float | None = None
        self._epoch = 0
        self._transport_generation: int | None = None
        self._lease_requested = False
        self._detaching = False
        self._next_attempt_at = 0.0
        self._retry_delay_s = 2.0

    @property
    def connection(self) -> Any | None:
        return self._connection

    def _transport_epoch(self) -> int:
        return int(getattr(self._transport, "connection_generation", 0))

    def start(self) -> None:
        if self._transport is None or self._task is not None:
            return
        if not self._handler_registered:
            add = getattr(self._transport, "add_voice_frame_handler", None)
            if callable(add):
                add(self._on_frame)
                self._handler_registered = True
        self._stop.clear()
        self._receiver = asyncio.create_task(self._receive(), name="usb-voice-receive")
        self._heartbeat = asyncio.create_task(self._renew_lease(), name="usb-voice-heartbeat")
        self._task = asyncio.create_task(self._supervise(), name="usb-voice")

    async def stop(self) -> None:
        self._stop.set()
        self._epoch += 1
        tasks = [task for task in (self._task, self._receiver, self._heartbeat, self._reset_task) if task is not None]
        self._task = self._receiver = self._heartbeat = self._reset_task = None
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        await self._detach()
        if self._cleanup_tasks:
            await asyncio.gather(*list(self._cleanup_tasks), return_exceptions=True)
        remove = getattr(self._transport, "remove_voice_frame_handler", None)
        if self._handler_registered and callable(remove):
            remove(self._on_frame)
            self._handler_registered = False

    def _ws_voice_active(self) -> bool:
        conn = getattr(self._esp32, "connection", None)
        return bool(conn is not None and getattr(conn, "connected", False)
                    and getattr(conn, "transport", "websocket") != "usb")

    def _lease_current(self, epoch: int | None = None) -> bool:
        return bool(
            not self._stop.is_set() and not self._detaching and self._lease_requested
            and usb_voice_mode() != "off" and self._transport is not None
            and getattr(self._transport, "connected", False)
            and self._transport_generation == self._transport_epoch()
            and (epoch is None or epoch == self._epoch) and not self._ws_voice_active()
        )

    async def ensure_session(self, *, timeout_s: float = 20.0) -> Any | None:
        """Open or reuse USB voice; a current WebSocket always has priority."""
        if self._stop.is_set() or usb_voice_mode() == "off" or self._ws_voice_active():
            return None
        proxy = self._live_proxy()
        if proxy is not None:
            return proxy
        if self._transport is None or not getattr(self._transport, "connected", False):
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
            await asyncio.sleep(0.25)

    async def _tick(self) -> None:
        mode = usb_voice_mode()
        connected = self._transport is not None and bool(getattr(self._transport, "connected", False))
        changed = self._lease_requested and self._transport_generation != self._transport_epoch()
        if mode == "off" or not connected or changed or self._ws_voice_active():
            self._usb_since = None
            if self._lease_requested or self._connection is not None or self._opening is not None:
                await self._detach()
            return
        if self._live_proxy() is not None or time.monotonic() < self._next_attempt_at:
            return
        if mode == "on":
            await self._signal_and_wait(20.0)
            return
        now = time.monotonic()
        if self._usb_since is None:
            self._usb_since = now
        if now - self._usb_since >= usb_voice_defer_s():
            await self._signal_and_wait(20.0)

    def _live_proxy(self) -> Any | None:
        conn = self._connection
        if (not self._lease_current() or conn is None or not getattr(conn, "connected", False)
                or getattr(self._esp32, "connection", None) is not conn):
            return None
        proxy = getattr(conn, "cloud_proxy", None)
        if getattr(proxy, "speak_text", None) is None:
            return None
        if not getattr(proxy, "connected", True) and not getattr(proxy, "running", False):
            return None
        return proxy

    async def _signal_and_wait(self, timeout_s: float) -> Any | None:
        async with self._attach_lock:
            proxy = self._live_proxy()
            if proxy is not None:
                return proxy
            if (self._stop.is_set() or self._detaching or self._ws_voice_active()
                    or usb_voice_mode() == "off" or self._transport is None
                    or not getattr(self._transport, "connected", False)):
                return None
            if self._lease_requested or self._connection is not None:
                await self._detach()
            self._epoch += 1
            epoch = self._epoch
            self._transport_generation = self._transport_epoch()
            self._lease_requested = True
            self._ready.clear()
            try:
                await self._transport.set_voice_mode(True)
                if not self._lease_current(epoch):
                    await self._disable_mode()
                    return None
                self._lease_ready.set()
                await asyncio.wait_for(self._ready.wait(), timeout=timeout_s)
                proxy = self._live_proxy()
                if proxy is not None:
                    self._retry_delay_s = 2.0
                    self._next_attempt_at = 0.0
                    return proxy
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                logger.warning("USB voice attach failed: %s", exc)
            self._next_attempt_at = time.monotonic() + self._retry_delay_s
            self._retry_delay_s = min(30.0, self._retry_delay_s * 2)
            await self._detach()
            return None

    async def _renew_lease(self) -> None:
        while not self._stop.is_set():
            await self._lease_ready.wait()
            await asyncio.sleep(self.HEARTBEAT_S)
            if not self._lease_ready.is_set():
                continue
            epoch = self._epoch
            if not self._lease_current(epoch):
                await self._detach()
                continue
            try:
                result = await self._transport.ping_voice(timeout_s=3.0)
                payload = result.get("result", {}) if isinstance(result, dict) else {}
                if payload.get("ok") is not True:
                    raise ConnectionError("USB voice lease expired")
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                logger.warning("USB voice heartbeat failed: %s", exc)
                if epoch == self._epoch:
                    await self._detach()

    def _clear_frames(self) -> None:
        while not self._frames.empty():
            self._frames.get_nowait()

    def _on_frame(self, frame: UsbVoiceFrame) -> None:
        if not self._lease_current():
            return
        try:
            self._frames.put_nowait((self._epoch, time.monotonic(), frame))
        except asyncio.QueueFull:
            # Never allow a backlog to bypass the wake/pause ordering or grow
            # without a limit. A fresh handshake is required after overflow.
            logger.warning("USB voice receive queue overflow; restarting the session")
            self._epoch += 1
            self._clear_frames()
            self._lease_ready.clear()
            self._lease_requested = False
            if self._connection is not None:
                self._connection.disconnect()
                retire = getattr(self._connection.cloud_proxy, "retire", None)
                if callable(retire):
                    retire()
            if self._reset_task is None or self._reset_task.done():
                self._reset_task = asyncio.create_task(self._detach(force_disable=True), name="usb-voice-overflow")

    async def _receive(self) -> None:
        while not self._stop.is_set():
            epoch, received_at, frame = await self._frames.get()
            if not self._lease_current(epoch):
                continue
            if frame.type == AUDIO_UPLINK and time.monotonic() - received_at > self.MAX_AUDIO_AGE_S:
                continue
            try:
                await self._handle_frame(frame, epoch)
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("USB voice frame failed")
                await self._detach()

    async def _handle_frame(self, frame: UsbVoiceFrame, epoch: int | None = None) -> None:
        if not self._lease_current(epoch):
            return
        if frame.type == VOICE_JSON:
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
        elif frame.type != AUDIO_UPLINK:
            return
        conn = self._connection
        if (conn is None or not conn.connected or getattr(self._esp32, "connection", None) is not conn):
            return
        proxy = getattr(conn, "cloud_proxy", None)
        if frame.type == AUDIO_UPLINK:
            observe = getattr(self._esp32, "observe_device_audio", None)
            if callable(observe):
                observe(conn)
            if proxy is not None:
                await handle_audio_frame(frame.payload, conn.session_id, cloud_proxy=proxy)
            return
        observe = getattr(self._esp32, "observe_device_message", None)
        if callable(observe):
            result = observe(conn, data)
            if inspect.isawaitable(result):
                await result
        if self._lease_current(epoch) and proxy is not None:
            await proxy.send_device_json(data)

    async def _accept_hello(self, hello: dict[str, Any]) -> None:
        if not self._lease_current():
            return
        if self._connection is not None:
            self._epoch += 1
            self._clear_frames()
            previous, self._connection = self._connection, None
            await self._release_connection(previous)
        epoch = self._epoch
        sender = UsbVoiceSender(self._transport, lambda: self._lease_current(epoch))
        self._opening = asyncio.create_task(self._esp32.begin_usb_voice(
            sender=sender, hello=hello, device_id=str(hello.get("device_id") or "usb"),
        ), name="usb-voice-open")
        opening = self._opening
        try:
            connection = await asyncio.shield(opening)
        except asyncio.CancelledError:
            if asyncio.current_task().cancelling():
                raise
            return
        finally:
            if self._opening is opening and opening.done():
                self._opening = None
        if not self._lease_current(epoch):
            if connection is not None:
                await self._release_connection(connection)
            return
        self._connection = connection
        self._ready.set()

    async def _release_connection(self, connection: Any) -> None:
        if getattr(connection, "_usb_released", False):
            return
        connection._usb_released = True
        connection.disconnect()
        proxy = getattr(connection, "cloud_proxy", None)
        retire = getattr(proxy, "retire", None)
        if callable(retire):
            retire()
        release = getattr(self._esp32, "release_connection", None)
        if callable(release):
            result = release(connection)
            if inspect.isawaitable(result):
                await result
        if proxy is not None:
            start_stop = getattr(self._esp32, "_start_proxy_stop", None)
            if callable(start_stop):
                task = start_stop(connection)
            else:
                async def cleanup():
                    try:
                        await proxy.stop()
                    except Exception:
                        logger.exception("USB voice proxy cleanup failed")
                task = asyncio.create_task(cleanup(), name="usb-voice-cleanup")
            if task is not None:
                self._cleanup_tasks.add(task)
                task.add_done_callback(self._cleanup_tasks.discard)

    async def _disable_mode(self) -> None:
        if self._transport is None or not getattr(self._transport, "connected", False):
            return
        try:
            await self._transport.set_voice_mode(False, timeout_s=1.0)
        except Exception as exc:
            logger.debug("USB voice release failed: %s", exc)

    async def _detach(self, *, force_disable: bool = False) -> None:
        async with self._detach_lock:
            self._detaching = True
            requested = self._lease_requested or force_disable
            self._epoch += 1
            self._lease_requested = False
            self._lease_ready.clear()
            self._clear_frames()
            self._ready.set()  # Release an attach waiter on shutdown/replacement.
            opening, self._opening = self._opening, None
            conn, self._connection = self._connection, None
            try:
                if opening is not None and not opening.done():
                    opening.cancel()
                    await asyncio.gather(opening, return_exceptions=True)
                elif opening is not None and not opening.cancelled():
                    try:
                        opened = opening.result()
                    except Exception:
                        opened = None
                    if conn is None:
                        conn = opened
                if conn is not None:
                    await self._release_connection(conn)
                if requested:
                    await self._disable_mode()
            finally:
                self._detaching = False
