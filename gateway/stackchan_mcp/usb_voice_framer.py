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

import binascii
from dataclasses import dataclass

USB_VOICE_SOF0 = 0xFF
USB_VOICE_SOF1 = 0xFE
USB_VOICE_VERSION = 1
USB_VOICE_MAX_PAYLOAD = 1275
USB_VOICE_MAX_JSON = 4096
USB_VOICE_MAX_LINE = 4096

AUDIO_UPLINK = 1
AUDIO_DOWNLINK = 2
VOICE_JSON = 3
CAMERA_CHUNK = 4
USB_CAMERA_MAX_PAYLOAD = 2056  # uint32 request id + uint32 offset + 2048 bytes


@dataclass(frozen=True)
class UsbVoiceFrame:
    type: int
    payload: bytes


@dataclass(frozen=True)
class UsbVoiceLine:
    text: str


UsbVoiceEvent = UsbVoiceFrame | UsbVoiceLine


def usb_voice_crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)


def payload_limit(frame_type: int) -> int:
    return {VOICE_JSON: USB_VOICE_MAX_JSON, CAMERA_CHUNK: USB_CAMERA_MAX_PAYLOAD}.get(
        frame_type, USB_VOICE_MAX_PAYLOAD,
    )


def encode_usb_voice_frame(frame_type: int, payload: bytes) -> bytes:
    """Return a bounded frame, or b'' for an unknown type or oversized payload."""
    limit = payload_limit(frame_type)
    if frame_type not in {AUDIO_UPLINK, AUDIO_DOWNLINK, VOICE_JSON, CAMERA_CHUNK} or len(payload) > limit:
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
        self._discard_text = False
        self._text = bytearray()
        self._drop_line = False

    def reset(self) -> None:
        self._buf.clear()
        self._discard_text = False
        self._text.clear()
        self._drop_line = False

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
                    frame_type = self._buf[3]
                    limit = payload_limit(frame_type)
                    if frame_type not in {AUDIO_UPLINK, AUDIO_DOWNLINK, VOICE_JSON, CAMERA_CHUNK} or length > limit:
                        # Its boundary is untrustworthy. Ignore binary-looking
                        # text until a valid frame restores synchronization.
                        del self._buf[:6]
                        self._discard_text = True
                        self._text.clear()
                        self._drop_line = False
                        continue
                    total = 6 + length + 2
                    if len(self._buf) < total:
                        break
                    if self._buf[2] != USB_VOICE_VERSION:
                        del self._buf[:total]
                        continue
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
                    self._discard_text = False
                    del self._buf[:total]
                    continue
            nl = self._buf.find(b"\n")
            sof = self._buf.find(bytes((USB_VOICE_SOF0, USB_VOICE_SOF1)))
            if sof >= 0 and (self._discard_text or nl < 0 or sof < nl):
                # Console fragments or a stray byte must not hide the next
                # binary frame, including when its SOF spans two reads.
                prefix = self._buf[:sof]
                if not self._discard_text and 0xFF not in prefix and 0xFE not in prefix:
                    self._text.extend(prefix)
                    if len(self._text) > USB_VOICE_MAX_LINE:
                        self._text.clear()
                        self._drop_line = True
                else:
                    self._text.clear()
                del self._buf[:sof]
                continue
            if self._discard_text:
                keep = self._buf[-1:] if self._buf[-1] == USB_VOICE_SOF0 else b""
                self._buf.clear()
                self._buf.extend(keep)
                break
            if nl < 0:
                # Do not rescan the entire pending log on every serial byte.
                keep = self._buf[-1:] if self._buf[-1] == USB_VOICE_SOF0 else b""
                prefix = self._buf[:-1] if keep else self._buf
                if 0xFF in prefix or 0xFE in prefix:
                    self._text.clear()
                elif not self._drop_line:
                    self._text.extend(prefix)
                    if len(self._text) > USB_VOICE_MAX_LINE:
                        self._text.clear()
                        self._drop_line = True
                self._buf.clear()
                self._buf.extend(keep)
                break
            raw = bytes(self._text + self._buf[:nl])
            self._text.clear()
            del self._buf[:nl + 1]
            if raw.endswith(b"\r"):
                raw = raw[:-1]
            if raw and not self._drop_line and len(raw) <= USB_VOICE_MAX_LINE:
                events.append(UsbVoiceLine(text=raw.decode("utf-8", errors="replace")))
            self._drop_line = False
        return events
