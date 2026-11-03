# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Demux USB Serial/JTAG bytes into `$MCP:` lines and XiaoZhi voice frames.

The layout matches ``components/conversation/usb_voice_framer.cpp``. Text
lines stay newline-delimited so JSON-RPC tool calls are unchanged. Opus and
the hello/listen/tts JSON events use a length-prefixed frame:

    0xFF 0xFE | ver u8=1 | type u8 | len u16 LE | payload | crc16 LE

CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) covers ver|type|len|payload.

16 kHz / 24 kHz Opus at 60 ms is a few hundred bytes per frame (device
encoder is 24 kbit/s). The conversation is half-duplex, so the link is not
asked to carry both directions at full rate. USB Serial/JTAG ignores the
115200 baud placeholder; the controller runs at USB full-speed.
"""

from __future__ import annotations

from dataclasses import dataclass

USB_VOICE_SOF0 = 0xFF
USB_VOICE_SOF1 = 0xFE
USB_VOICE_VERSION = 1
USB_VOICE_MAX_PAYLOAD = 1275
USB_VOICE_MAX_LINE = 4096

AUDIO_UPLINK = 1
AUDIO_DOWNLINK = 2
VOICE_JSON = 3


@dataclass(frozen=True)
class UsbVoiceFrame:
    type: int
    payload: bytes


@dataclass(frozen=True)
class UsbVoiceLine:
    text: str


UsbVoiceEvent = UsbVoiceFrame | UsbVoiceLine


def usb_voice_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_usb_voice_frame(frame_type: int, payload: bytes) -> bytes:
    """Return one frame, or b'' when the payload is larger than an Opus packet."""
    if len(payload) > USB_VOICE_MAX_PAYLOAD:
        return b""
    length = len(payload)
    covered = bytes((
        USB_VOICE_VERSION,
        frame_type & 0xFF,
        length & 0xFF,
        (length >> 8) & 0xFF,
    )) + payload
    crc = usb_voice_crc16(covered)
    return bytes((USB_VOICE_SOF0, USB_VOICE_SOF1)) + covered + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


class UsbVoiceStreamParser:
    """Incremental demux. ``feed`` returns events in arrival order."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def reset(self) -> None:
        self._buf.clear()

    def feed(self, data: bytes) -> list[UsbVoiceEvent]:
        self._buf.extend(data)
        events: list[UsbVoiceEvent] = []
        while self._buf:
            if self._buf[0] == USB_VOICE_SOF0:
                if len(self._buf) < 2:
                    break
                if self._buf[1] == USB_VOICE_SOF1:
                    if len(self._buf) < 6:
                        break
                    length = self._buf[4] | (self._buf[5] << 8)
                    if self._buf[2] != USB_VOICE_VERSION or length > USB_VOICE_MAX_PAYLOAD:
                        del self._buf[0]
                        continue
                    total = 6 + length + 2
                    if len(self._buf) < total:
                        break
                    covered = bytes(self._buf[2:6 + length])
                    expect = usb_voice_crc16(covered)
                    got = self._buf[6 + length] | (self._buf[7 + length] << 8)
                    if got != expect:
                        # Plausible header, bad CRC: drop the candidate frame
                        # so it cannot stick to the next `$MCP:` line.
                        del self._buf[:total]
                        continue
                    payload = bytes(self._buf[6:6 + length])
                    events.append(UsbVoiceFrame(type=self._buf[3], payload=payload))
                    del self._buf[:total]
                    continue
            nl = self._buf.find(b"\n")
            if nl < 0:
                if len(self._buf) > USB_VOICE_MAX_LINE:
                    keep = bytes([USB_VOICE_SOF0]) if self._buf[-1] == USB_VOICE_SOF0 else b""
                    self._buf.clear()
                    self._buf.extend(keep)
                break
            raw = bytes(self._buf[:nl])
            del self._buf[:nl + 1]
            if raw.endswith(b"\r"):
                raw = raw[:-1]
            if raw:
                events.append(UsbVoiceLine(text=raw.decode("utf-8", errors="replace")))
        return events
