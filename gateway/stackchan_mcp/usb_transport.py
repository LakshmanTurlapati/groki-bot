"""USB Serial/JTAG MCP transport for StackChan.

Speaks the same protocol as firmware/main/usb_mcp_endpoint.cc:
  • Lines beginning with "$MCP:" carry JSON-RPC payloads.
  • All other lines are device-side ESP_LOG output. We forward those to a
    log file so the device stays observable while the gateway holds the
    only serial handle (macOS lets a single process own /dev/cu.usbmodem*).

The transport is asyncio-friendly even though pyserial itself is blocking:
a background daemon thread owns the serial handle, lines flow back to the
asyncio loop via run_coroutine_threadsafe, and call_tool() awaits the
response future keyed on the JSON-RPC id.

The reader thread also implements hot-plug recovery: when the device
disconnects (USB cable yanked, firmware reboot), Serial.read raises
SerialException, the thread loops back to discovery, and start()'s
connected event reflects the live state.

Voice audio shares this byte stream. `$MCP:` lines stay newline-delimited.
Opus frames and XiaoZhi JSON (hello, listen, tts) use the length-prefixed
frames in ``usb_voice_framer``. The 115200 baud value is a pyserial
placeholder: the ESP32-S3 USB Serial/JTAG controller ignores it and runs
at USB full-speed, which covers 16 kHz / 24 kHz 60 ms Opus. The firmware
conversation remains half-duplex (no mic uplink while the speaker plays).
"""

from __future__ import annotations

import asyncio
import base64
import binascii
import json
import logging
import math
import os
import struct
import threading
import time
from pathlib import Path
from typing import Any, Callable

import serial
from serial.tools import list_ports

from .usb_voice_framer import CAMERA_CHUNK, UsbVoiceFrame, UsbVoiceLine, UsbVoiceStreamParser

logger = logging.getLogger(__name__)


class UsbToolError(RuntimeError):
    """Raised when the firmware MCP server returns a JSON-RPC error response.

    Without it a tools/call that returned {"error": {...}} would look like a
    success: TrackingBridge would think the head moved and never fall back to
    WS. The generic except in TrackingBridge._send_head_angles catches this
    exception and uses the WebSocket fallback.
    """

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        self.code = code
        self.message = message
        self.data = data
        super().__init__(f"USB tool error {code}: {message}")


# ESP32-S3 USB Serial/JTAG controller default identifiers.
ESP32S3_USB_JTAG_VID = 0x303A
ESP32S3_USB_JTAG_PID = 0x1001

MCP_PREFIX = "$MCP:"
DEFAULT_BAUD = 115200
DEFAULT_RECONNECT_DELAY_S = 1.0
DEFAULT_TIMEOUT_S = 2.0
DEFAULT_LOG_PATH = Path(os.path.expanduser("~/.stackchan/usb.log"))
CAMERA_FRAME_BYTES = 320 * 240 * 2


def discover_port(
    vid: int = ESP32S3_USB_JTAG_VID,
    pid: int = ESP32S3_USB_JTAG_PID,
) -> str | None:
    """Return the first /dev/cu.usbmodem* matching (vid, pid), or None.

    Matching by USB ID (not by /dev path) keeps the gateway robust to
    enumeration order changes — usbmodem101 today may be usbmodem301
    after a reboot.
    """
    for descriptor in list_ports.comports():
        if descriptor.vid == vid and descriptor.pid == pid:
            return descriptor.device
    return None


class UsbTransport:
    """Single-device USB MCP transport bound to one asyncio loop."""

    def __init__(
        self,
        *,
        port: str | None = None,
        baud: int = DEFAULT_BAUD,
        log_path: Path | None = None,
        reconnect_delay_s: float = DEFAULT_RECONNECT_DELAY_S,
        serial_factory: Any = None,
    ) -> None:
        """Create the transport.

        port: pin to a specific /dev/cu.* device. None = auto-discover by VID/PID.
        log_path: where to spool ESP_LOG lines. None = ~/.stackchan/usb.log.
        serial_factory: dependency injection for tests; defaults to serial.Serial.
        """
        self._port = port
        self._baud = baud
        self._log_path = Path(log_path) if log_path is not None else DEFAULT_LOG_PATH
        self._reconnect_delay = reconnect_delay_s
        self._serial_factory = serial_factory or serial.Serial

        self._serial: Any | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._reader_thread: threading.Thread | None = None
        self._stop_flag = threading.Event()
        self._next_id = 1
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._pending_generations: dict[int, int] = {}
        self._send_lock: asyncio.Lock | None = None
        self._connected_event: asyncio.Event | None = None
        self._log_file: Any | None = None
        self._voice_handlers: list[Any] = []
        self._connection_generation = 0
        self._camera_buffers: dict[int, tuple[int, bytearray]] = {}
        self._camera_lock = asyncio.Lock()
        self.last_camera_metadata: dict[str, Any] = {}

    # ------------------------------------------------------------------ lifecycle

    @property
    def connected(self) -> bool:
        return self._connected_event is not None and self._connected_event.is_set()

    @property
    def connection_generation(self) -> int:
        return self._connection_generation

    async def start(self, *, wait_connect: bool = True, connect_timeout_s: float = 5.0) -> bool:
        """Open the serial port and start the read loop.

        Returns True if the device connected within connect_timeout_s,
        False otherwise (the reader keeps retrying in the background).
        """
        if self._reader_thread is not None:
            return self.connected
        self._loop = asyncio.get_running_loop()
        self._send_lock = asyncio.Lock()
        self._connected_event = asyncio.Event()
        self._log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self._log_path.open("a", encoding="utf-8")
        self._stop_flag.clear()
        self._reader_thread = threading.Thread(
            target=self._reader_loop,
            name="usb-mcp-reader",
            daemon=True,
        )
        self._reader_thread.start()

        if not wait_connect:
            return self.connected
        try:
            await asyncio.wait_for(self._connected_event.wait(), timeout=connect_timeout_s)
            return True
        except asyncio.TimeoutError:
            return False

    async def stop(self) -> None:
        """Tear down the read loop and close the serial port."""
        self._stop_flag.set()
        self._connection_generation += 1
        self._connected_event_clear(self._connection_generation)
        ser = self._serial
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if self._reader_thread is not None:
            await asyncio.to_thread(self._reader_thread.join, 2.0)
            self._reader_thread = None
        if self._log_file is not None:
            try:
                self._log_file.close()
            except Exception:
                pass
            self._log_file = None
        for fut in self._pending.values():
            if not fut.done():
                fut.cancel()
        self._pending.clear()
        self._pending_generations.clear()

    # ------------------------------------------------------------------ rpc

    async def call_tool(
        self,
        name: str,
        arguments: dict[str, Any] | None = None,
        *,
        timeout_s: float = DEFAULT_TIMEOUT_S,
    ) -> dict[str, Any]:
        """Send tools/call and await the JSON-RPC response.

        Raises UsbToolError when the firmware returns a JSON-RPC error
        envelope (e.g. unknown tool, validation failure). The exception is
        the trigger for the WS fallback in TrackingBridge; without it a
        firmware-side error looks like success and we'd never retry.
        """
        request = {
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}},
        }
        data = await self._send_rpc(request, timeout_s=timeout_s)
        self._raise_if_error(data)
        return data

    async def list_tools(self, *, timeout_s: float = DEFAULT_TIMEOUT_S) -> dict[str, Any]:
        """Send tools/list and await the JSON-RPC response."""
        data = await self._send_rpc(
            {"method": "tools/list", "params": {}},
            timeout_s=timeout_s,
        )
        self._raise_if_error(data)
        return data

    async def initialize(self, *, timeout_s: float = DEFAULT_TIMEOUT_S) -> dict[str, Any]:
        """Send initialize handshake. Useful as a connectivity smoke test."""
        data = await self._send_rpc(
            {"method": "initialize", "params": {"capabilities": {}}},
            timeout_s=timeout_s,
        )
        self._raise_if_error(data)
        return data

    async def set_voice_mode(self, enable: bool, *, timeout_s: float = 3.0) -> dict[str, Any]:
        """Ask the device to carry XiaoZhi voice on this serial link.

        ``enable=False`` returns the device to the Wi-Fi WebSocket. The
        response is a normal ``$MCP:`` JSON-RPC result; audio itself is not
        carried in that line.
        """
        data = await self._send_rpc(
            {
                "method": "voice/mode",
                "params": {
                    "enable": bool(enable),
                    "audio": {
                        "format": "opus",
                        "sample_rate": 16000,
                        "channels": 1,
                        "frame_duration": 60,
                    },
                },
            },
            timeout_s=timeout_s,
        )
        self._raise_if_error(data)
        return data

    def add_voice_frame_handler(self, handler: Any) -> None:
        """Register ``handler(UsbVoiceFrame)`` called on the asyncio loop."""
        if handler not in self._voice_handlers:
            self._voice_handlers.append(handler)

    def remove_voice_frame_handler(self, handler: Any) -> None:
        if handler in self._voice_handlers:
            self._voice_handlers.remove(handler)

    async def ping_voice(self, *, timeout_s: float = 3.0) -> dict[str, Any]:
        """Renew an active USB voice lease without enabling or restarting it."""
        data = await self._send_rpc({"method": "voice/ping", "params": {}}, timeout_s=timeout_s)
        self._raise_if_error(data)
        return data

    async def write_raw(self, data: bytes, *, is_current: Callable[[], bool] | None = None) -> None:
        """Write a frame only while its connection and optional session remain current."""
        await self._write_locked(data, is_current=is_current)

    async def capture_camera_frame(self, *, timeout_s: float = 5.0, encoding: str = "rgb565be",
                                   binary: bool = False, motion: bool = False,
                                   jpeg_quality: int = 80) -> tuple[str, bytes]:
        """Collect one bounded camera snapshot without queuing images or audio."""
        if encoding not in {"rgb565be", "jpeg"}:
            raise ValueError("USB camera encoding must be jpeg or rgb565be")
        if not 60 <= jpeg_quality <= 90:
            raise ValueError("Camera JPEG quality must be between 60 and 90")
        async with self._camera_lock:
            generation = self.connection_generation
            pixels = bytearray()
            response = await self._send_rpc(
                {"method": "camera/capture", "params": {
                    "tracking": True, "format": encoding, "binary": binary,
                    "motion": motion, "quality": jpeg_quality,
                }},
                timeout_s=timeout_s, camera_buffer=pixels,
            )
            self._raise_if_error(response)
            result = response.get("result", {})
            if not isinstance(result, dict):
                raise ConnectionError("Invalid USB camera response")
            if result.get("busy") is True:
                return "", b""
            if result.get("error") in {"capture_failed", "encoding_failed"}:
                raise ConnectionError("USB camera " + result["error"])
            frame_id = result.get("frame_id")
            valid_pixels = (len(pixels) == CAMERA_FRAME_BYTES if encoding == "rgb565be" else
                            0 < len(pixels) <= 65536 and pixels[:2] == b"\xff\xd8" and pixels[-2:] == b"\xff\xd9"
                            and result.get("bytes") == len(pixels))
            if (generation != self.connection_generation or not self.connected
                    or result.get("width") != 320 or result.get("height") != 240
                    or result.get("format") != encoding or not valid_pixels
                    or not isinstance(frame_id, str) or len(frame_id) != 16
                    or any(c not in "0123456789abcdef" for c in frame_id)):
                raise ConnectionError("Invalid USB camera frame")
            self.last_camera_metadata = {key: result[key] for key in (
                "frame_id", "format", "bytes", "capture_ms", "encode_ms", "capture_yaw", "capture_pitch",
                "motion_compensated", "capture_motion_deg", "binary", "jpeg_quality",
            ) if key in result}
            if result.get("motion_compensated") is True:
                pose = [result.get("capture_yaw"), result.get("capture_pitch")]
                if not all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)
                           and abs(v) <= 180 for v in pose):
                    raise ConnectionError("Invalid USB camera capture pose")
            return frame_id, bytes(pixels)

    @staticmethod
    def _raise_if_error(data: dict[str, Any]) -> None:
        err = data.get("error") if isinstance(data, dict) else None
        if not isinstance(err, dict):
            return
        raise UsbToolError(
            code=int(err.get("code", -1)),
            message=str(err.get("message", "unknown USB tool error")),
            data=err.get("data"),
        )

    async def _send_rpc(
        self,
        rpc: dict[str, Any],
        *,
        timeout_s: float,
        camera_buffer: bytearray | None = None,
    ) -> dict[str, Any]:
        if self._loop is None or self._send_lock is None:
            raise RuntimeError("UsbTransport.start() not awaited yet")

        rid = self._next_id
        self._next_id += 1
        body = {"jsonrpc": "2.0", "id": rid, **rpc}
        frame = MCP_PREFIX + " " + json.dumps(body, ensure_ascii=False) + "\n"
        fut = self._loop.create_future()
        self._pending[rid] = fut
        self._pending_generations[rid] = self.connection_generation
        if camera_buffer is not None:
            self._camera_buffers[rid] = (self.connection_generation, camera_buffer)
        try:
            await self._write_locked(frame.encode("utf-8"))
            return await asyncio.wait_for(fut, timeout=timeout_s)
        except asyncio.TimeoutError:
            self._pending.pop(rid, None)
            if camera_buffer is not None:
                logger.warning("USB camera request %d timed out after receiving %d bytes", rid, len(camera_buffer))
            raise
        except Exception:
            self._pending.pop(rid, None)
            raise
        finally:
            self._pending.pop(rid, None)
            self._pending_generations.pop(rid, None)
            self._camera_buffers.pop(rid, None)
            if not fut.done():
                fut.cancel()

    async def _write_locked(self, data: bytes, *, is_current: Callable[[], bool] | None = None) -> None:
        if self._loop is None or self._send_lock is None:
            raise RuntimeError("UsbTransport.start() not awaited yet")
        generation = self.connection_generation
        async with self._send_lock:
            if generation != self.connection_generation or (is_current is not None and not is_current()):
                raise ConnectionError("USB session changed before write")
            ser = self._serial
            if ser is None or not self.connected:
                raise RuntimeError("USB transport not connected")
            write = asyncio.create_task(asyncio.to_thread(self._write_all, ser, data))
            try:
                await asyncio.shield(write)
            except asyncio.CancelledError:
                # Cancelling to_thread does not stop the OS write. Retain the
                # frame lock until it ends, including repeated cancellation.
                while not write.done():
                    try:
                        await asyncio.shield(write)
                    except asyncio.CancelledError:
                        continue
                    except Exception:
                        break
                if write.done() and not write.cancelled():
                    write.exception()
                raise

    @staticmethod
    def _write_all(ser: Any, data: bytes) -> None:
        deadline = time.monotonic() + 1.0
        offset = 0
        try:
            while offset < len(data):
                if time.monotonic() >= deadline:
                    raise ConnectionError("USB write timed out")
                count = ser.write(data[offset:])
                if not isinstance(count, int) or count <= 0 or count > len(data) - offset:
                    raise ConnectionError("USB write did not complete")
                offset += count
        except Exception as exc:
            # A partial frame leaves the stream uncertain; reconnect before
            # another writer can append bytes to it.
            try:
                ser.close()
            except Exception:
                pass
            raise ConnectionError("USB serial write failed") from exc

    # ------------------------------------------------------------------ reader

    def _reader_loop(self) -> None:
        """Background thread: open port, read lines, hot-plug reconnect."""
        while not self._stop_flag.is_set():
            target_port = self._port or discover_port()
            if target_port is None:
                self._sleep_or_stop(self._reconnect_delay)
                continue

            try:
                ser = self._serial_factory(target_port, self._baud, timeout=0.2, write_timeout=1.0)
            except Exception as exc:
                logger.warning("USB serial open(%s) failed: %s", target_port, exc)
                self._sleep_or_stop(self._reconnect_delay)
                continue

            self._serial = ser
            self._connection_generation += 1
            generation = self._connection_generation
            self._signal_event(self._connected_event_set, generation)
            logger.info("USB transport connected on %s", target_port)
            try:
                self._read_until_eof(ser, generation)
            except Exception as exc:
                logger.info("USB transport read loop ended: %s", exc)
            finally:
                self._connection_generation += 1
                self._serial = None
                self._signal_event(self._connected_event_clear, self._connection_generation)
                # Fail every in-flight RPC immediately on disconnect
                # so callers (TrackingBridge) skip the 300 ms USB timeout and
                # fall back to WS within the same tick. Without this a yanked
                # cable + new face frame waits the full timeout AND lets the
                # firmware execute a stale command twice (USB + WS).
                self._signal_event(self._fail_pending_due_to_disconnect, generation)
                try:
                    ser.close()
                except Exception:
                    pass

            if self._stop_flag.is_set():
                break
            self._sleep_or_stop(self._reconnect_delay)

    def _read_until_eof(self, ser: Any, generation: int | None = None) -> None:
        # Byte-oriented. readline() would split an Opus frame on an embedded
        # newline and could swallow the next `$MCP:` line with it.
        generation = self.connection_generation if generation is None else generation
        parser = UsbVoiceStreamParser()
        while not self._stop_flag.is_set():
            # read(512) waits for the timeout when a short camera result or
            # voice packet ends a burst. Block for only the first byte, then
            # drain what the serial driver already has without waiting to fill.
            waiting = getattr(ser, "in_waiting", 0)
            count = min(4096, waiting) if isinstance(waiting, int) and waiting > 0 else 1
            chunk = ser.read(count)
            if not chunk:
                continue
            for event in parser.feed(chunk):
                if isinstance(event, UsbVoiceLine):
                    if event.text.startswith(MCP_PREFIX):
                        self._dispatch_response_line(event.text, generation)
                    else:
                        self._append_log(event.text)
                elif isinstance(event, UsbVoiceFrame):
                    self._dispatch_voice_frame(event, generation)

    def _dispatch_response_line(self, line: str, generation: int | None = None) -> None:
        generation = self.connection_generation if generation is None else generation
        payload = line[len(MCP_PREFIX):].lstrip()
        try:
            data = json.loads(payload)
        except Exception:
            logger.warning("USB transport bad JSON: %.120s", payload)
            return
        if not isinstance(data, dict):
            return
        if data.get("method") == "camera/chunk":
            params = data.get("params")
            if isinstance(params, dict) and self._loop is not None:
                self._loop.call_soon_threadsafe(
                    self._receive_camera_chunk, params, generation,
                )
            return
        rid = data.get("id")
        if not isinstance(rid, int) or isinstance(rid, bool):
            return
        loop = self._loop
        if loop is None:
            return
        loop.call_soon_threadsafe(self._resolve_pending, rid, data, generation)

    def _dispatch_voice_frame(self, frame: UsbVoiceFrame, generation: int | None = None) -> None:
        generation = self.connection_generation if generation is None else generation
        loop = self._loop
        if frame.type == CAMERA_CHUNK:
            if loop is not None:
                loop.call_soon_threadsafe(self._receive_camera_bytes, frame.payload, generation)
            return
        if loop is None or not self._voice_handlers:
            return
        handlers = list(self._voice_handlers)
        loop.call_soon_threadsafe(self._emit_voice_frame, handlers, frame, generation)

    def _emit_voice_frame(self, handlers: list[Any], frame: UsbVoiceFrame, generation: int) -> None:
        if generation != self._connection_generation or not self.connected:
            return
        for handler in handlers:
            if handler not in self._voice_handlers:
                continue
            try:
                handler(frame)
            except Exception:
                logger.exception("USB voice frame handler failed")

    def _receive_camera_chunk(self, params: dict[str, Any], generation: int) -> None:
        rid = params.get("id")
        if not isinstance(rid, int) or isinstance(rid, bool):
            return
        transfer = self._camera_buffers.get(rid)
        fut = self._pending.get(rid)
        if transfer is None or fut is None or fut.done():
            return
        expected_generation, pixels = transfer
        try:
            offset, encoded = params.get("offset"), params.get("data")
            if (generation != expected_generation or generation != self.connection_generation
                    or not isinstance(offset, int) or isinstance(offset, bool) or offset != len(pixels)
                    or not isinstance(encoded, str) or len(encoded) > 2732):
                raise ValueError
            chunk = base64.b64decode(encoded, validate=True)
            if not 0 < len(chunk) <= 2048 or len(pixels) + len(chunk) > CAMERA_FRAME_BYTES:
                raise ValueError
            pixels.extend(chunk)
        except (ValueError, binascii.Error):
            fut.set_exception(ConnectionError("Invalid USB camera chunk"))

    def _receive_camera_bytes(self, payload: bytes, generation: int) -> None:
        if len(payload) < 8:
            return
        rid, offset = struct.unpack_from("<II", payload)
        transfer, fut = self._camera_buffers.get(rid), self._pending.get(rid)
        if transfer is None or fut is None or fut.done():
            return
        expected_generation, pixels = transfer
        chunk = payload[8:]
        if (generation != expected_generation or generation != self.connection_generation
                or offset != len(pixels) or not 0 < len(chunk) <= 2048
                or len(pixels) + len(chunk) > CAMERA_FRAME_BYTES):
            fut.set_exception(ConnectionError("Invalid USB camera chunk"))
            return
        pixels.extend(chunk)

    def _resolve_pending(self, rid: int, data: dict[str, Any], generation: int) -> None:
        if (generation != self.connection_generation
                or generation != self._pending_generations.get(rid, generation)):
            return
        fut = self._pending.pop(rid, None)
        self._pending_generations.pop(rid, None)
        if fut is not None and not fut.done():
            fut.set_result(data)

    def _fail_pending_due_to_disconnect(self, generation: int) -> None:
        """Drain _pending with ConnectionError. Runs in the asyncio loop."""
        if not self._pending:
            return
        err = ConnectionError("USB transport disconnected")
        # Snapshot the dict to avoid mutation-during-iteration.
        for rid, fut in list(self._pending.items()):
            if self._pending_generations.get(rid, -1) > generation:
                continue
            if not fut.done():
                fut.set_exception(err)
                # If the caller is concurrently unwinding through a write
                # error / timeout path, asyncio may otherwise log "Future
                # exception was never retrieved" even though the transport is
                # deliberately failing fast for WS fallback.
                fut.exception()
            self._pending.pop(rid, None)
            self._pending_generations.pop(rid, None)

    def _append_log(self, text: str) -> None:
        if self._log_file is None:
            return
        try:
            self._log_file.write(text + "\n")
            self._log_file.flush()
        except Exception:
            pass

    def _signal_event(self, fn: Any, *args: Any) -> None:
        loop = self._loop
        if loop is None:
            return
        loop.call_soon_threadsafe(fn, *args)

    def _connected_event_set(self, generation: int) -> None:
        if generation == self.connection_generation and not self._stop_flag.is_set() and self._connected_event is not None:
            self._connected_event.set()

    def _connected_event_clear(self, generation: int) -> None:
        if generation == self.connection_generation and self._connected_event is not None:
            self._connected_event.clear()

    def _sleep_or_stop(self, seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self._stop_flag.is_set():
                return
            time.sleep(min(0.1, end - time.monotonic()))
