# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Sequential robot snapshots and an in-memory Apple Vision worker."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import struct
import time
from collections.abc import Awaitable, Callable
from contextlib import suppress
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlsplit, urlunsplit

import aiohttp

from .face_tracker import autostart_enabled, default_tracker_binary

logger = logging.getLogger(__name__)
FRAME_BYTES = 320 * 240 * 2
Face = dict[str, float]


@dataclass(frozen=True)
class RobotFrameContext:
    connection: Any
    session_id: str
    frame_id: str
    started_at: float
    deadline: float
    capture_pose: tuple[float, float] | None = None
    generation: int = 0


@dataclass
class RobotCameraConfig:
    url_override: str = ""
    password: str = field(default="", repr=False)
    fps: float = 4.0
    http_timeout_s: float = 5.0
    detector_timeout_s: float = 0.5
    frame_max_age_s: float = 1.0
    usb_frame_max_age_s: float = 1.8
    initial_backoff_s: float = 5.0
    max_backoff_s: float = 30.0
    usb_encoding: str = "jpeg"
    motion: bool = True
    binary: bool = True
    jpeg_quality: int = 80

    def __post_init__(self) -> None:
        if not math.isfinite(self.fps) or not 0 < self.fps <= 8:
            raise ValueError("STACKCHAN_CAMERA_FPS must be greater than zero and at most 8")
        if self.usb_encoding not in {"jpeg", "rgb565be"}:
            raise ValueError("STACKCHAN_CAMERA_USB_FORMAT must be jpeg or rgb565be")
        if not isinstance(self.jpeg_quality, int) or not 60 <= self.jpeg_quality <= 90:
            raise ValueError("STACKCHAN_CAMERA_JPEG_QUALITY must be between 60 and 90")
        for value in (
            self.http_timeout_s, self.detector_timeout_s, self.frame_max_age_s, self.usb_frame_max_age_s,
            self.initial_backoff_s, self.max_backoff_s,
        ):
            if not math.isfinite(value) or value <= 0:
                raise ValueError("Robot camera timeouts and backoff must be positive")
        if self.url_override:
            camera_capture_url(self.url_override)

    @classmethod
    def from_env(cls) -> RobotCameraConfig:
        try:
            fps = float(os.getenv("STACKCHAN_CAMERA_FPS", "4"))
        except ValueError:
            raise ValueError("STACKCHAN_CAMERA_FPS must be a number") from None
        return cls(
            url_override=os.getenv("STACKCHAN_CAMERA_URL", "").strip(),
            password=os.getenv("STACKCHAN_CAMERA_PASSWORD", ""),
            fps=fps,
            usb_encoding=os.getenv("STACKCHAN_CAMERA_USB_FORMAT", "jpeg").strip().lower(),
            motion=os.getenv("STACKCHAN_CAMERA_TRACKING_MOTION", "1").lower() in {"1", "true", "yes"},
            binary=os.getenv("STACKCHAN_CAMERA_BINARY", "1").lower() in {"1", "true", "yes"},
            jpeg_quality=int(os.getenv("STACKCHAN_CAMERA_JPEG_QUALITY", "80")),
        )


def camera_capture_url(origin: str) -> str:
    """Validate an explicit device origin without ever reporting its contents."""
    try:
        parts = urlsplit(origin)
        valid = (
            parts.scheme in {"http", "https"} and bool(parts.hostname)
            and parts.username is None and parts.password is None
            and parts.path in {"", "/"} and not parts.query and not parts.fragment
        )
        _ = parts.port
    except ValueError:
        valid = False
    if not valid:
        raise ValueError("STACKCHAN_CAMERA_URL must be an HTTP(S) origin without credentials or a path")
    return urlunsplit((parts.scheme, parts.netloc, "/api/camera/capture", "tracking=1", ""))


class CameraError(Exception):
    """A public, credential-free camera failure reason."""


class CameraBusy(CameraError):
    pass


async def capture_frame(
    session: aiohttp.ClientSession, url: str, password: str,
) -> tuple[str, bytes]:
    auth = aiohttp.BasicAuth("stackchan", password, encoding="utf-8") if password else None
    async with session.get(url, auth=auth, allow_redirects=False) as response:
        if response.status == 409:
            raise CameraBusy("busy")
        if response.status == 401:
            raise CameraError("auth_required")
        if response.status != 200:
            raise CameraError("http_error")
        headers = response.headers
        if (
            headers.get("X-Frame-Width") != "320"
            or headers.get("X-Frame-Height") != "240"
            or headers.get("X-Frame-Format") != "rgb565be"
            or response.content_type != "application/octet-stream"
            or headers.get("Content-Encoding", "identity") != "identity"
        ):
            raise CameraError("invalid_frame")
        frame_id = headers.get("X-Frame-Id", "")
        if not frame_id or len(frame_id) > 128 or not frame_id.isascii() or not frame_id.isprintable():
            raise CameraError("invalid_frame")
        if response.content_length is not None and response.content_length != FRAME_BYTES:
            raise CameraError("invalid_frame")
        try:
            frame = await response.content.readexactly(FRAME_BYTES + 1)
        except asyncio.IncompleteReadError as exc:
            frame = exc.partial
        if len(frame) != FRAME_BYTES:
            raise CameraError("invalid_frame")
        return frame_id, frame


def parse_detection(line: bytes) -> Face | None:
    try:
        result = json.loads(line)
        if not isinstance(result, dict) or "face" not in result:
            raise ValueError
        face = result["face"]
        if face is None:
            return None
        if not isinstance(face, dict):
            raise ValueError
        values = {}
        for key in ("x", "y", "width", "height", "confidence"):
            value = face[key]
            if isinstance(value, bool) or not isinstance(value, (float, int)):
                raise ValueError
            if not math.isfinite(value) or not 0 <= value <= 1:
                raise ValueError
            values[key] = float(value)
        if values["width"] <= 0 or values["height"] <= 0:
            raise ValueError
        return values
    except (ValueError, TypeError, KeyError, UnicodeDecodeError):
        raise CameraError("invalid_detection") from None


class SwiftFrameDetector:
    def __init__(self, timeout_s: float = 0.5) -> None:
        self.timeout_s = timeout_s
        self.child: asyncio.subprocess.Process | None = None

    @property
    def running(self) -> bool:
        return self.child is not None and self.child.returncode is None

    async def start(self) -> None:
        if self.running:
            return
        await self.stop()
        binary = default_tracker_binary()
        if not binary.is_file():
            raise FileNotFoundError("Build tools/vision-tracker with swift build -c release")
        env = os.environ.copy()
        env.pop("STACKCHAN_CAMERA_PASSWORD", None)
        env.pop("STACKCHAN_CAMERA_URL", None)
        self.child = await asyncio.create_subprocess_exec(
            str(binary), "--stdin-frames", stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, limit=4096, env=env,
        )

    async def detect(self, frame: bytes, *, encoding: str = "rgb565be") -> Face | None:
        valid = len(frame) == FRAME_BYTES if encoding == "rgb565be" else (
            encoding == "jpeg" and 0 < len(frame) <= 65536
        )
        if not valid or not self.running:
            raise CameraError("detector_unavailable")

        async def exchange() -> Face | None:
            child = self.child
            assert child is not None and child.stdin is not None and child.stdout is not None
            child.stdin.write(struct.pack("<II", 1 if encoding == "rgb565be" else 2, len(frame)) + frame)
            await child.stdin.drain()
            line = await child.stdout.readline()
            if not line:
                raise CameraError("detector_exited")
            return parse_detection(line)

        try:
            return await asyncio.wait_for(exchange(), timeout=self.timeout_s)
        except BaseException:
            # A timed-out or partial exchange cannot be paired with a later frame.
            await self.stop()
            raise

    async def stop(self) -> None:
        child, self.child = self.child, None
        if child is None:
            return
        if child.returncode is None:
            with suppress(ProcessLookupError):
                child.terminate()
            try:
                await asyncio.wait_for(child.wait(), timeout=3.0)
            except asyncio.TimeoutError:
                with suppress(ProcessLookupError):
                    child.kill()
                await child.wait()
        else:
            await child.wait()


class RobotCameraTracker:
    def __init__(
        self, esp32: Any, *,
        on_detection: Callable[[Face | None, RobotFrameContext], Awaitable[None]],
        tracking_allowed: Callable[[], bool],
        config: RobotCameraConfig | None = None,
    ) -> None:
        self.esp32 = esp32
        self.on_detection = on_detection
        self.tracking_allowed = tracking_allowed
        self.config = config or RobotCameraConfig.from_env()
        self._detector = SwiftFrameDetector(self.config.detector_timeout_s)
        self._task: asyncio.Task[None] | None = None
        self._generation = 0
        self._state = "stopped"
        self._last_frame_at: float | None = None
        self._last_latency_ms: float | None = None
        self._face_reported = False
        self._errors = 0
        self._last_error: str | None = None
        self._capture_transport: str | None = None
        self._frames_processed = 0
        self._faces_detected = 0
        self._last_face: Face | None = None
        self._last_metadata: dict[str, Any] = {}
        self._last_detection_ms: float | None = None

    @property
    def running(self) -> bool:
        return self._detector.running

    @property
    def available(self) -> bool:
        return (
            self.running and self._last_frame_at is not None
            and time.monotonic() - self._last_frame_at <= 5.0
        )

    @property
    def face_reported(self) -> bool:
        return self.available and self._face_reported

    @property
    def pid(self) -> int | None:
        child = self._detector.child
        return child.pid if self.running and child is not None else None

    def notify_face_reported(self) -> None:
        self._face_reported = True

    def status(self) -> dict[str, Any]:
        return {
            "source": "robot", "camera_state": self._state,
            "frame_age_s": None if self._last_frame_at is None else round(
                time.monotonic() - self._last_frame_at, 3,
            ),
            "capture_latency_ms": self._last_latency_ms, "camera_errors": self._errors,
            "camera_fps": self.config.fps, "last_error": self._last_error,
            "capture_transport": self._capture_transport,
            "frames_processed": self._frames_processed, "faces_detected": self._faces_detected,
            "last_face": self._last_face,
            "frame_metadata": self._last_metadata, "detection_ms": self._last_detection_ms,
        }

    def start(self, endpoint: str | None = None) -> None:
        if self._task is not None and not self._task.done():
            return
        if not autostart_enabled():
            self._state = "disabled"
            return
        self._task = asyncio.create_task(self._run(), name="stackchan-robot-camera")

    async def invalidate(self) -> None:
        # Do not await the HTTP request here: conversation audio must start promptly.
        self._generation += 1
        self._face_reported = False
        self._last_frame_at = None

    async def stop(self) -> None:
        await self.invalidate()
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task
        await self._detector.stop()
        self._state = "stopped"

    def _current(self, connection: Any, generation: int) -> bool:
        return (
            generation == self._generation and self.esp32.connection is connection
            and connection.connected and connection.camera_tracking_v1
            and self.tracking_allowed()
        )

    async def _run(self) -> None:
        config = self.config
        timeout = aiohttp.ClientTimeout(total=config.http_timeout_s, connect=min(1.0, config.http_timeout_s))
        backoff = config.initial_backoff_s
        previous_connection = None
        async with aiohttp.ClientSession(timeout=timeout, trust_env=False, auto_decompress=False) as session:
            while True:
                connection = self.esp32.connection
                if connection is not previous_connection:
                    await self.invalidate()
                    previous_connection = connection
                    backoff = config.initial_backoff_s
                if connection is None or not connection.connected:
                    self._state = "waiting_device"
                    await asyncio.sleep(0.1)
                    continue
                if not connection.camera_tracking_v1:
                    self._state = "unsupported_firmware"
                    await asyncio.sleep(0.1)
                    continue
                if not self.tracking_allowed():
                    self._state = "paused"
                    await asyncio.sleep(0.1)
                    continue
                started = asyncio.get_running_loop().time()
                generation = self._generation
                deadline = started + config.frame_max_age_s
                try:
                    usb = getattr(self.esp32, "usb_transport", None)
                    use_usb = getattr(connection, "transport", "websocket") == "usb" and usb is not None
                    self._capture_transport = "usb" if use_usb else "http"
                    url = None
                    if not use_usb:
                        origin = config.url_override
                        if not origin:
                            host = connection.peer_host
                            if not host:
                                raise CameraError("address_required")
                            origin = f"http://[{host}]" if ":" in host else f"http://{host}"
                        url = camera_capture_url(origin)
                    await self._detector.start()
                    started = asyncio.get_running_loop().time()
                    deadline = started + (config.usb_frame_max_age_s if use_usb else config.frame_max_age_s)
                    if not self._current(connection, generation):
                        continue
                    self._state = "capturing"
                    if use_usb:
                        options = {}
                        v2 = getattr(connection, "camera_tracking_v2", False)
                        if v2:
                            options = {"binary": config.binary, "motion": config.motion,
                                       "jpeg_quality": config.jpeg_quality}
                        frame_id, frame = await usb.capture_camera_frame(
                            timeout_s=config.http_timeout_s, encoding=config.usb_encoding, **options,
                        )
                        if not frame_id:
                            raise CameraBusy("busy")
                        self._last_metadata = getattr(usb, "last_camera_metadata", {})
                    else:
                        frame_id, frame = await capture_frame(session, url, config.password)
                    received = asyncio.get_running_loop().time()
                    self._last_latency_ms = round((received - started) * 1000, 1)
                    if self._current(connection, generation):
                        if received > deadline:
                            raise CameraError("stale_frame")
                        self._last_frame_at = received
                        detecting_at = asyncio.get_running_loop().time()
                        if use_usb and config.usb_encoding == "jpeg":
                            face = await self._detector.detect(frame, encoding="jpeg")
                        else:
                            face = await self._detector.detect(frame)
                        self._last_detection_ms = round((asyncio.get_running_loop().time() - detecting_at) * 1000, 1)
                        if self._current(connection, generation):
                            if asyncio.get_running_loop().time() > deadline:
                                raise CameraError("stale_frame")
                            pose = None
                            if use_usb and v2 and config.motion and self._last_metadata.get("motion_compensated") is True:
                                pose = (self._last_metadata["capture_yaw"], self._last_metadata["capture_pitch"])
                            context = RobotFrameContext(connection, connection.session_id, frame_id, started, deadline,
                                                        pose, generation)
                            await self.on_detection(face, context)
                            self._frames_processed += 1
                            self._faces_detected += int(face is not None)
                            self._last_face = face
                            self._face_reported = face is not None
                            self._state = "tracking" if face is not None else "no_face"
                            self._last_error = None
                            backoff = config.initial_backoff_s
                        else:
                            self._state = "discarded_frame"
                    else:
                        self._state = "discarded_frame"
                except CameraBusy:
                    self._state = "busy"
                    # Servo settling is not a captured frame. Retry promptly
                    # so the idle hold does not expire between useful images.
                    await asyncio.sleep(0.03)
                    continue
                except FileNotFoundError:
                    self._state = "detector_missing"
                    self._last_error = self._state
                    logger.warning("Robot camera detector missing; build tools/vision-tracker with swift build -c release")
                    return
                except Exception as exc:
                    if not self._current(connection, generation):
                        # A paused or replaced session no longer owns this
                        # exchange. Its timeout must not delay the next one.
                        self._state = "discarded_frame"
                        continue
                    self._errors += 1
                    self._state = (str(exc) if isinstance(exc, CameraError) else
                                   "capture_timeout" if isinstance(exc, asyncio.TimeoutError) else
                                   "capture_failed" if isinstance(exc, ConnectionError) else
                                   "capture_or_detector_error")
                    self._last_error = self._state
                    logger.warning("Robot camera unavailable (%s, %s); retrying in %.1fs",
                                   self._state, type(exc).__name__, backoff)
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, config.max_backoff_s)
                    continue
                # The loop owns one frame through its decision; there is no pending-frame queue.
                remaining = 1.0 / config.fps - (asyncio.get_running_loop().time() - started)
                await asyncio.sleep(max(0.0, remaining))
