# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Camera chunks share the USB console without entering the voice queue."""

import asyncio
import base64
import json
import struct
from types import SimpleNamespace

import pytest

from stackchan_mcp.usb_transport import CAMERA_FRAME_BYTES, UsbTransport
from stackchan_mcp.usb_voice_framer import (
    AUDIO_UPLINK, CAMERA_CHUNK, UsbVoiceFrame, UsbVoiceLine, UsbVoiceStreamParser, encode_usb_voice_frame,
)


def connected_transport():
    transport = UsbTransport()
    transport._loop = asyncio.get_running_loop()
    transport._send_lock = asyncio.Lock()
    transport._connected_event = asyncio.Event()
    transport._connected_event.set()
    transport._connection_generation = 3
    return transport


def rpc_line(message):
    return ("\n$MCP: " + json.dumps(message) + "\n").encode()


def camera_chunk(rid, offset, payload):
    return {"method": "camera/chunk", "params": {
        "id": rid, "offset": offset, "data": base64.b64encode(payload).decode(),
    }}


@pytest.mark.asyncio
async def test_complete_camera_frame_with_interleaved_voice_and_fragmented_reads():
    transport = connected_transport()
    pixels = bytes(range(256)) * (CAMERA_FRAME_BYTES // 256)
    voice = []
    transport.add_voice_frame_handler(voice.append)

    async def write(request):
        rid = json.loads(request.decode().split("$MCP: ", 1)[1])["id"]
        stream = bytearray()
        for offset in range(0, len(pixels), 2048):
            stream.extend(rpc_line(camera_chunk(rid, offset, pixels[offset:offset + 2048])))
            stream.extend(encode_usb_voice_frame(AUDIO_UPLINK, b"opus\n$MCP:"))
        stream.extend(rpc_line({"id": rid, "result": {
            "frame_id": "0123456789abcdef", "width": 320, "height": 240, "format": "rgb565be",
        }}))
        parser = UsbVoiceStreamParser()
        for offset in range(0, len(stream), 17):
            for event in parser.feed(stream[offset:offset + 17]):
                if isinstance(event, UsbVoiceLine) and event.text.startswith("$MCP:"):
                    transport._dispatch_response_line(event.text)
                elif isinstance(event, UsbVoiceFrame):
                    transport._dispatch_voice_frame(event)

    transport._write_locked = write
    frame_id, captured = await transport.capture_camera_frame(timeout_s=1)
    assert frame_id == "0123456789abcdef"
    assert captured == pixels
    assert len(voice) == CAMERA_FRAME_BYTES // 2048
    assert all(frame.payload == b"opus\n$MCP:" for frame in voice)
    assert not transport._pending and not transport._camera_buffers


@pytest.mark.asyncio
@pytest.mark.parametrize("fault", ["gap", "oversize", "base64", "reconnect", "short"])
async def test_invalid_camera_transfers_never_produce_a_frame(fault):
    transport = connected_transport()

    async def write(request):
        rid = json.loads(request.decode().split("$MCP: ", 1)[1])["id"]
        chunk = camera_chunk(rid, 1 if fault == "gap" else 0, bytes(2049 if fault == "oversize" else 16))
        if fault == "base64":
            chunk["params"]["data"] = "!invalid!"
        if fault == "reconnect":
            transport._connection_generation += 1
        transport._dispatch_response_line("$MCP: " + json.dumps(chunk))
        transport._dispatch_response_line('$MCP: ' + json.dumps({"id": rid, "result": {
            "frame_id": "0123456789abcdef", "width": 320, "height": 240, "format": "rgb565be",
        }}))

    transport._write_locked = write
    with pytest.raises(ConnectionError, match="Invalid USB camera"):
        await transport.capture_camera_frame(timeout_s=1)
    assert not transport._pending and not transport._camera_buffers


@pytest.mark.asyncio
async def test_camera_busy_is_retryable_and_timeout_cleans_up():
    transport = connected_transport()

    async def busy(request):
        rid = json.loads(request.decode().split("$MCP: ", 1)[1])["id"]
        transport._dispatch_response_line('$MCP: ' + json.dumps({"id": rid, "result": {"busy": True}}))

    transport._write_locked = busy
    assert await transport.capture_camera_frame(timeout_s=1) == ("", b"")

    async def no_response(request):
        pass

    transport._write_locked = no_response
    with pytest.raises(asyncio.TimeoutError):
        await transport.capture_camera_frame(timeout_s=.01)
    assert not transport._pending and not transport._camera_buffers


@pytest.mark.asyncio
@pytest.mark.parametrize("wrong_size", [False, True])
async def test_jpeg_camera_transfer_is_bounded_and_declares_its_length(wrong_size):
    transport = connected_transport()
    jpeg = b"\xff\xd8" + bytes(4096) + b"\xff\xd9"

    async def write(request):
        command = json.loads(request.decode().split("$MCP: ", 1)[1])
        assert command["params"]["format"] == "jpeg"
        rid = command["id"]
        for offset in range(0, len(jpeg), 2048):
            transport._dispatch_response_line("$MCP: " + json.dumps(camera_chunk(rid, offset, jpeg[offset:offset + 2048])))
        transport._dispatch_response_line("$MCP: " + json.dumps({"id": rid, "result": {
            "frame_id": "0123456789abcdef", "width": 320, "height": 240, "format": "jpeg",
            "bytes": len(jpeg) + int(wrong_size), "capture_ms": 250, "encode_ms": 20,
        }}))

    transport._write_locked = write
    if wrong_size:
        with pytest.raises(ConnectionError, match="Invalid USB camera"):
            await transport.capture_camera_frame(encoding="jpeg")
    else:
        assert await transport.capture_camera_frame(encoding="jpeg") == ("0123456789abcdef", jpeg)
        assert transport.last_camera_metadata["encode_ms"] == 20


@pytest.mark.asyncio
@pytest.mark.parametrize("v2", [False, True])
async def test_robot_tracker_uses_usb_without_an_http_address(monkeypatch, v2):
    from stackchan_mcp.robot_camera_tracker import RobotCameraConfig, RobotCameraTracker
    from unittest.mock import AsyncMock

    connection = SimpleNamespace(connected=True, camera_tracking_v1=True, transport="usb", session_id="usb")
    connection.camera_tracking_v2 = v2
    usb = SimpleNamespace(capture_camera_frame=AsyncMock(return_value=("0123456789abcdef", bytes(CAMERA_FRAME_BYTES))))
    usb.last_camera_metadata = {"capture_yaw": 12, "capture_pitch": 4, "motion_compensated": v2}
    received = asyncio.Event()

    async def detection(face, context):
        assert context.connection is connection
        assert context.frame_id == "0123456789abcdef"
        assert context.capture_pose == ((12, 4) if v2 else None)
        received.set()

    tracker = RobotCameraTracker(
        SimpleNamespace(connection=connection, usb_transport=usb),
        on_detection=detection, tracking_allowed=lambda: True, config=RobotCameraConfig(),
    )
    tracker._detector = SimpleNamespace(start=AsyncMock(), detect=AsyncMock(return_value=None), stop=AsyncMock())
    task = asyncio.create_task(tracker._run())
    try:
        await asyncio.wait_for(received.wait(), 1)
        usb.capture_camera_frame.assert_awaited_once()
        if v2:
            assert usb.capture_camera_frame.await_args.kwargs["motion"] is True
            assert usb.capture_camera_frame.await_args.kwargs["binary"] is True
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


@pytest.mark.asyncio
@pytest.mark.parametrize("fault", [None, "gap", "generation", "short", "pose"])
async def test_binary_jpeg_transfer_checks_crc_order_session_and_capture_pose(fault):
    transport = connected_transport()
    jpeg = b"\xff\xd8" + bytes(range(256)) * 20 + b"\xff\xd9"
    voice = []
    transport.add_voice_frame_handler(voice.append)

    async def write(request):
        command = json.loads(request.decode().split("$MCP: ", 1)[1])
        assert command["params"]["binary"] and command["params"]["motion"]
        rid = command["id"]
        stream = bytearray()
        for offset in range(0, len(jpeg), 2048):
            payload = struct.pack("<II", rid, offset + (1 if fault == "gap" else 0)) + jpeg[offset:offset + 2048]
            packet = bytearray(encode_usb_voice_frame(CAMERA_CHUNK, payload))
            if fault == "short" and offset == 0:
                packet[-1] ^= 1  # Bad CRC cannot yield image bytes.
            stream.extend(packet)
            stream.extend(encode_usb_voice_frame(AUDIO_UPLINK, b"voice\n$MCP:"))
        if fault == "generation":
            transport._connection_generation += 1
        parser = UsbVoiceStreamParser()
        for offset in range(0, len(stream), 17):
            for event in parser.feed(stream[offset:offset + 17]):
                if isinstance(event, UsbVoiceFrame):
                    transport._dispatch_voice_frame(event)
        transport._dispatch_response_line("$MCP: " + json.dumps({"id": rid, "result": {
            "frame_id": "0123456789abcdef", "width": 320, "height": 240, "format": "jpeg",
            "bytes": len(jpeg), "motion_compensated": True, "capture_yaw": float("nan") if fault == "pose" else 12,
            "capture_pitch": 4, "binary": True,
        }}))

    transport._write_locked = write
    if fault:
        with pytest.raises(ConnectionError, match="Invalid USB camera"):
            await transport.capture_camera_frame(encoding="jpeg", binary=True, motion=True)
    else:
        assert await transport.capture_camera_frame(encoding="jpeg", binary=True, motion=True) == ("0123456789abcdef", jpeg)
        assert len(voice) == 3
        assert all(frame.type == AUDIO_UPLINK for frame in voice)
    assert not transport._pending and not transport._camera_buffers
