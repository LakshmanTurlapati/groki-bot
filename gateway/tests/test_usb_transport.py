"""Tests for the USB Serial/JTAG MCP transport.

Avoid touching real /dev/cu.usbmodem* — every test injects a FakeSerial via
the serial_factory hook. The contract under test is:
  • call_tool() writes a "$MCP: <json>\\n" frame to the serial handle.
  • Lines tagged "$MCP:" that come back from the device resolve the matching
    response future by JSON-RPC id.
  • Non-prefixed lines are spooled to the log file, not the RPC queue.
  • Hot-plug: SerialException in read triggers reconnect.
  • A binary voice frame may contain newlines and `$MCP:` without
    splitting the JSON-RPC line that follows it.
"""

from __future__ import annotations

import asyncio
import json
import threading
import time
from pathlib import Path

import pytest
import serial

from stackchan_mcp.usb_transport import (
    ESP32S3_USB_JTAG_PID,
    ESP32S3_USB_JTAG_VID,
    MCP_PREFIX,
    UsbToolError,
    UsbTransport,
    discover_port,
)


# ---------------------------------------------------------------------------- fakes


class FakeSerial:
    """In-memory serial port. Single instance per (factory call, test)."""

    def __init__(self, port: str = "fake", baud: int = 115200, timeout: float = 0.1) -> None:
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.written = bytearray()
        self._rx_buf = bytearray()
        self._rx_lock = threading.Lock()
        self._closed = False
        self._fail_on_next_read = False

    # producer-side helpers --------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Push bytes that subsequent readline() calls will see."""
        with self._rx_lock:
            self._rx_buf.extend(data)

    def feed_line(self, line: str) -> None:
        self.feed((line + "\n").encode("utf-8"))

    def kill_next_read(self) -> None:
        """Force the next read to raise SerialException (hot-unplug)."""
        self._fail_on_next_read = True

    # pyserial-compatible surface --------------------------------------------

    def read(self, size: int = 1) -> bytes:
        if self._fail_on_next_read:
            self._fail_on_next_read = False
            raise serial.SerialException("simulated unplug")
        end = time.monotonic() + (self.timeout or 0.2)
        while time.monotonic() < end:
            with self._rx_lock:
                if self._rx_buf:
                    n = min(size, len(self._rx_buf))
                    out = bytes(self._rx_buf[:n])
                    del self._rx_buf[:n]
                    return out
            time.sleep(0.005)
        return b""

    def readline(self) -> bytes:
        if self._fail_on_next_read:
            self._fail_on_next_read = False
            raise serial.SerialException("simulated unplug")
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            with self._rx_lock:
                nl = self._rx_buf.find(b"\n")
                if nl >= 0:
                    out = bytes(self._rx_buf[: nl + 1])
                    del self._rx_buf[: nl + 1]
                    return out
            time.sleep(0.005)
        return b""

    def write(self, data: bytes) -> int:
        if self._closed:
            raise serial.SerialException("port closed")
        self.written.extend(data)
        return len(data)

    def close(self) -> None:
        self._closed = True


class FakeSerialFactory:
    """Returns the same FakeSerial across reopens so hot-plug tests can
    inspect both the pre-disconnect write buffer and the next session."""

    def __init__(self) -> None:
        self.instances: list[FakeSerial] = []

    def __call__(self, port: str, baud: int, timeout: float, write_timeout: float = 1.0) -> FakeSerial:
        fake = FakeSerial(port=port, baud=baud, timeout=timeout)
        self.instances.append(fake)
        return fake


# ---------------------------------------------------------------------------- discover


def test_discover_port_matches_vid_pid(monkeypatch):
    class FakeDescriptor:
        def __init__(self, device, vid, pid):
            self.device = device
            self.vid = vid
            self.pid = pid

    fake_list = [
        FakeDescriptor("/dev/cu.Bluetooth", None, None),
        FakeDescriptor("/dev/cu.usbmodem99", ESP32S3_USB_JTAG_VID, ESP32S3_USB_JTAG_PID),
        FakeDescriptor("/dev/cu.usbmodem01", 0x1234, 0x5678),
    ]
    monkeypatch.setattr(
        "stackchan_mcp.usb_transport.list_ports.comports",
        lambda: fake_list,
    )
    assert discover_port() == "/dev/cu.usbmodem99"


def test_discover_port_returns_none_when_no_match(monkeypatch):
    monkeypatch.setattr(
        "stackchan_mcp.usb_transport.list_ports.comports",
        lambda: [],
    )
    assert discover_port() is None


# ---------------------------------------------------------------------------- transport behaviour


@pytest.mark.asyncio
async def test_short_response_is_processed_without_waiting_for_a_larger_read():
    transport = UsbTransport()
    transport._loop = asyncio.get_running_loop()
    result = transport._loop.create_future()
    transport._pending[7] = result
    response = bytearray(b'$MCP: {"id":7,"result":{"ok":true}}\n')

    class BurstSerial:
        first_poll = True

        @property
        def in_waiting(self):
            if self.first_poll:
                self.first_poll = False
                return 0  # The burst arrives while waiting for its first byte.
            return len(response)

        def read(self, count):
            assert count <= len(response), "Read would wait for bytes outside the completed response"
            chunk = bytes(response[:count])
            del response[:count]
            if not response:
                transport._stop_flag.set()
            return chunk

    await asyncio.to_thread(transport._read_until_eof, BurstSerial())
    assert await asyncio.wait_for(result, .1) == {"id": 7, "result": {"ok": True}}


@pytest.mark.asyncio
async def test_call_tool_round_trip(tmp_path: Path):
    factory = FakeSerialFactory()
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        reconnect_delay_s=0.05,
        serial_factory=factory,
    )
    connected = await transport.start(connect_timeout_s=2.0)
    assert connected is True
    assert factory.instances, "factory should have produced a fake serial"
    fake = factory.instances[0]

    async def respond_when_written():
        # Wait for the write to land, then push the response back.
        for _ in range(200):
            if fake.written:
                break
            await asyncio.sleep(0.01)
        sent = fake.written.decode()
        assert sent.startswith(MCP_PREFIX + " ")
        body = json.loads(sent[len(MCP_PREFIX) + 1 :])
        rid = body["id"]
        fake.feed_line(f'{MCP_PREFIX} {{"jsonrpc":"2.0","id":{rid},"result":{{"ok":true}}}}')

    responder = asyncio.create_task(respond_when_written())

    result = await transport.call_tool("self.robot.set_head_angles", {"yaw": 0, "pitch": 30, "speed": 60})

    await responder
    await transport.stop()

    assert result["id"] == 1
    assert result["result"] == {"ok": True}


@pytest.mark.asyncio
async def test_log_lines_go_to_log_file_not_rpc(tmp_path: Path):
    factory = FakeSerialFactory()
    log_path = tmp_path / "usb.log"
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=log_path,
        reconnect_delay_s=0.05,
        serial_factory=factory,
    )
    await transport.start(connect_timeout_s=2.0)
    fake = factory.instances[0]

    fake.feed_line("I (1234) StackChanBoard: hello world")
    fake.feed_line("I (1235) Si12T: init OK")
    await asyncio.sleep(0.2)

    await transport.stop()

    contents = log_path.read_text(encoding="utf-8")
    assert "StackChanBoard: hello world" in contents
    assert "Si12T: init OK" in contents


@pytest.mark.asyncio
async def test_call_tool_timeout_when_no_response(tmp_path: Path):
    factory = FakeSerialFactory()
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        reconnect_delay_s=0.05,
        serial_factory=factory,
    )
    await transport.start(connect_timeout_s=2.0)

    with pytest.raises(asyncio.TimeoutError):
        await transport.call_tool("self.never.responds", {}, timeout_s=0.2)

    await transport.stop()


@pytest.mark.asyncio
async def test_hot_plug_reconnects_after_simulated_unplug(tmp_path: Path):
    """SerialException in read must trigger a fresh open()."""
    factory = FakeSerialFactory()
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        reconnect_delay_s=0.05,
        serial_factory=factory,
    )
    await transport.start(connect_timeout_s=2.0)
    assert len(factory.instances) == 1

    factory.instances[0].kill_next_read()

    # Wait for the reader thread to recover and open a new fake.
    for _ in range(50):
        if len(factory.instances) >= 2 and transport.connected:
            break
        await asyncio.sleep(0.05)

    assert len(factory.instances) >= 2, "factory should reopen after unplug"
    assert transport.connected is True

    # The new fake should accept call_tool just like the first did.
    fake = factory.instances[-1]

    async def reply():
        for _ in range(200):
            if fake.written:
                break
            await asyncio.sleep(0.01)
        sent = fake.written.decode()
        body = json.loads(sent[len(MCP_PREFIX) + 1 :])
        fake.feed_line(f'{MCP_PREFIX} {{"jsonrpc":"2.0","id":{body["id"]},"result":{{"ok":true}}}}')

    asyncio.create_task(reply())
    result = await transport.call_tool("self.display.set_avatar", {"face": "happy"}, timeout_s=2.0)
    assert result["result"] == {"ok": True}

    await transport.stop()


@pytest.mark.asyncio
async def test_call_tool_before_start_raises(tmp_path: Path):
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        serial_factory=FakeSerialFactory(),
    )
    with pytest.raises(RuntimeError, match="not awaited yet"):
        await transport.call_tool("anything")


@pytest.mark.asyncio
async def test_call_tool_raises_on_jsonrpc_error(tmp_path: Path):
    """An error envelope must surface as UsbToolError so
    TrackingBridge's generic except triggers the WS fallback."""
    factory = FakeSerialFactory()
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        reconnect_delay_s=0.05,
        serial_factory=factory,
    )
    await transport.start(connect_timeout_s=2.0)
    fake = factory.instances[0]

    async def reply_with_error():
        for _ in range(200):
            if fake.written:
                break
            await asyncio.sleep(0.01)
        sent = fake.written.decode()
        body = json.loads(sent[len(MCP_PREFIX) + 1 :])
        rid = body["id"]
        fake.feed_line(
            f'{MCP_PREFIX} {{"jsonrpc":"2.0","id":{rid},'
            f'"error":{{"code":-32000,"message":"Unknown tool: foo"}}}}'
        )

    asyncio.create_task(reply_with_error())

    with pytest.raises(UsbToolError) as ei:
        await transport.call_tool("foo", {}, timeout_s=2.0)
    assert ei.value.code == -32000
    assert "Unknown tool" in ei.value.message

    await transport.stop()


@pytest.mark.asyncio
async def test_disconnect_fails_pending_immediately(tmp_path: Path):
    """A yanked cable mid-call must surface ConnectionError
    right away, not wait the full timeout. Otherwise TrackingBridge sits on
    300 ms of dead air per frame before falling back to WS."""
    factory = FakeSerialFactory()
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        reconnect_delay_s=10.0,  # long delay so reconnect doesn't race
        serial_factory=factory,
    )
    await transport.start(connect_timeout_s=2.0)
    fake = factory.instances[0]

    # Start a long-running call_tool: writes the frame, then awaits a response
    # that never comes (we won't feed one).
    pending = asyncio.create_task(
        transport.call_tool("self.long.running", {}, timeout_s=10.0)
    )
    # Let the write land before we yank the cable.
    for _ in range(50):
        if fake.written:
            break
        await asyncio.sleep(0.01)

    # Yank the cable.
    fake.kill_next_read()

    # The pending future should resolve to ConnectionError very quickly —
    # well under the call_tool timeout. Bound the wait so a regression
    # ("we still wait for timeout") shows up as a test failure.
    with pytest.raises(ConnectionError):
        await asyncio.wait_for(pending, timeout=1.0)

    await transport.stop()


@pytest.mark.asyncio
async def test_voice_frame_with_newline_does_not_split_mcp_line(tmp_path: Path):
    """Opus bytes may contain \\n and the `$MCP:` prefix. They must stay one frame."""
    from stackchan_mcp.usb_voice_framer import AUDIO_UPLINK, encode_usb_voice_frame

    factory = FakeSerialFactory()
    transport = UsbTransport(
        port="/dev/cu.fake",
        log_path=tmp_path / "usb.log",
        reconnect_delay_s=0.05,
        serial_factory=factory,
    )
    seen: list[bytes] = []
    transport.add_voice_frame_handler(lambda frame: seen.append(frame.payload))
    await transport.start(connect_timeout_s=2.0)
    fake = factory.instances[0]

    opus = b"\n$MCP: not-a-line\n\x00opus"
    fake.feed(encode_usb_voice_frame(AUDIO_UPLINK, opus))
    fake.feed_line(f'{MCP_PREFIX} {{"jsonrpc":"2.0","id":7,"result":{{"ok":true}}}}')

    for _ in range(50):
        if seen:
            break
        await asyncio.sleep(0.02)
    assert seen == [opus]

    # A later tool response still resolves by id. Seed a pending call by
    # writing one and answering with the line already fed above... the line
    # may have been consumed before call_tool. Send a fresh exchange.
    seen.clear()

    async def reply():
        for _ in range(200):
            if fake.written:
                break
            await asyncio.sleep(0.01)
        sent = fake.written.decode()
        body = json.loads(sent[len(MCP_PREFIX) + 1:])
        fake.feed(encode_usb_voice_frame(AUDIO_UPLINK, b"\nmore\n"))
        fake.feed_line(f'{MCP_PREFIX} {{"jsonrpc":"2.0","id":{body["id"]},"result":{{"ok":true}}}}')

    asyncio.create_task(reply())
    result = await transport.call_tool("self.robot.set_head_angles", {"yaw": 1}, timeout_s=2.0)
    assert result["result"] == {"ok": True}
    for _ in range(50):
        if any(payload == b"\nmore\n" for payload in seen):
            break
        await asyncio.sleep(0.02)
    assert b"\nmore\n" in seen
    await transport.stop()


def ready_transport(fake=None):
    transport = UsbTransport()
    transport._loop = asyncio.get_running_loop()
    transport._send_lock = asyncio.Lock()
    transport._connected_event = asyncio.Event()
    transport._connected_event.set()
    transport._connection_generation = 3
    transport._serial = fake or FakeSerial()
    return transport


@pytest.mark.asyncio
@pytest.mark.parametrize("change", ["connection", "session"])
async def test_queued_frame_never_crosses_connection_or_session(change):
    transport = ready_transport()
    current = True
    await transport._send_lock.acquire()
    pending = asyncio.create_task(transport.write_raw(b"old frame", is_current=lambda: current))
    await asyncio.sleep(0)
    if change == "connection":
        transport._connection_generation += 1
        transport._serial = FakeSerial()
    else:
        current = False
    transport._send_lock.release()
    with pytest.raises(ConnectionError, match="session changed"):
        await pending
    assert transport._serial.written == b""


@pytest.mark.asyncio
async def test_repeated_cancellation_keeps_write_atomic_until_os_write_finishes():
    entered, release = threading.Event(), threading.Event()

    class SlowSerial(FakeSerial):
        def write(self, data):
            if data == b"first":
                entered.set()
                assert release.wait(2)
            return super().write(data)

    transport = ready_transport(SlowSerial())
    first = asyncio.create_task(transport.write_raw(b"first"))
    assert await asyncio.to_thread(entered.wait, 1)
    first.cancel()
    second = asyncio.create_task(transport.write_raw(b"second"))
    await asyncio.sleep(0)
    first.cancel()
    await asyncio.sleep(0)
    assert not first.done() and not second.done()
    assert transport._serial.written == b""
    release.set()
    with pytest.raises(asyncio.CancelledError):
        await first
    await second
    assert transport._serial.written == b"firstsecond"


def test_short_writes_complete_one_frame_and_failed_writes_close_serial():
    class ShortSerial(FakeSerial):
        def write(self, data):
            return super().write(data[:2])

    serial_port = ShortSerial()
    UsbTransport._write_all(serial_port, b"one whole frame")
    assert serial_port.written == b"one whole frame"

    class FailedSerial(FakeSerial):
        def write(self, data):
            return 0

    failed = FailedSerial()
    with pytest.raises(ConnectionError, match="write failed"):
        UsbTransport._write_all(failed, b"frame")
    assert failed._closed


@pytest.mark.asyncio
async def test_delayed_reader_callbacks_cannot_affect_new_connection():
    from stackchan_mcp.usb_voice_framer import AUDIO_UPLINK, UsbVoiceFrame

    transport = ready_transport()
    frames = []
    transport.add_voice_frame_handler(frames.append)
    future = transport._loop.create_future()
    transport._pending[10] = future
    transport._pending_generations[10] = 3
    transport._connected_event_clear(2)
    transport._fail_pending_due_to_disconnect(1)
    transport._resolve_pending(10, {"result": "old"}, 1)
    transport._emit_voice_frame([frames.append], UsbVoiceFrame(AUDIO_UPLINK, b"old"), 1)
    assert transport.connected and not future.done() and frames == []
    transport._resolve_pending(10, {"result": "current"}, 3)
    assert await future == {"result": "current"}
    assert not transport._pending_generations


@pytest.mark.asyncio
async def test_removed_handler_does_not_receive_already_queued_frame():
    from stackchan_mcp.usb_voice_framer import AUDIO_UPLINK, UsbVoiceFrame

    transport = ready_transport()
    frames = []
    transport.add_voice_frame_handler(frames.append)
    transport._dispatch_voice_frame(UsbVoiceFrame(AUDIO_UPLINK, b"queued"))
    transport.remove_voice_frame_handler(frames.append)
    await asyncio.sleep(0)
    assert frames == []
