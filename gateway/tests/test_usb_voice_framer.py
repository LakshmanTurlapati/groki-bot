# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
import pytest

from stackchan_mcp.usb_voice_framer import (
    AUDIO_UPLINK, VOICE_JSON, UsbVoiceFrame, UsbVoiceLine, UsbVoiceStreamParser,
    encode_usb_voice_frame, usb_voice_crc16,
)


def feed_chunks(blob, size):
    parser = UsbVoiceStreamParser()
    events = []
    for offset in range(0, len(blob), size):
        events.extend(parser.feed(blob[offset:offset + size]))
    return events


def test_crc_matches_standard_check_vector():
    assert usb_voice_crc16(b"123456789") == 0x29B1


@pytest.mark.parametrize("size", [1, 2, 5, 17, 8192])
def test_binary_frame_can_interrupt_a_console_line(size):
    blob = b"I: before " + encode_usb_voice_frame(AUDIO_UPLINK, b"\n$MCP: not a line\n") + b"after\r\n"
    assert feed_chunks(blob, size) == [
        UsbVoiceFrame(AUDIO_UPLINK, b"\n$MCP: not a line\n"), UsbVoiceLine("I: before after"),
    ]


@pytest.mark.parametrize("prefix", [b"\xff", b"\xff\xfe\x02\x01\x00\x00\x00\x00"])
@pytest.mark.parametrize("size", [1, 8192])
def test_stray_sync_or_unsupported_version_does_not_hide_next_frame(prefix, size):
    frame = encode_usb_voice_frame(AUDIO_UPLINK, b"voice")
    assert feed_chunks(prefix + frame + b'$MCP: {"id":1}\n', size) == [
        UsbVoiceFrame(AUDIO_UPLINK, b"voice"), UsbVoiceLine('$MCP: {"id":1}'),
    ]


@pytest.mark.parametrize("size", [1, 512, 8192])
def test_oversize_header_resync_does_not_promote_payload_to_mcp(size):
    invalid = b"\xff\xfe\x01\x01\xff\xff\n$MCP: forged\n"
    good = encode_usb_voice_frame(AUDIO_UPLINK, b"voice")
    assert feed_chunks(invalid + good + b"$MCP: legitimate\n", size) == [
        UsbVoiceFrame(AUDIO_UPLINK, b"voice"), UsbVoiceLine("$MCP: legitimate"),
    ]


def test_corrupt_payload_is_never_a_line_or_nested_frame():
    nested = encode_usb_voice_frame(VOICE_JSON, b'{"type":"head"}')
    corrupt = bytearray(encode_usb_voice_frame(AUDIO_UPLINK, b"\n$MCP: forged\n" + nested))
    corrupt[-1] ^= 1
    assert feed_chunks(bytes(corrupt) + b"$MCP: real\n", 1) == [UsbVoiceLine("$MCP: real")]


@pytest.mark.parametrize("size", [1, 512, 16384])
def test_long_text_is_discarded_without_promoting_suffix_to_mcp(size):
    blob = b"x" * 5000 + encode_usb_voice_frame(AUDIO_UPLINK, b"voice") + b"$MCP: forged\n$MCP: real\n"
    assert feed_chunks(blob, size) == [UsbVoiceFrame(AUDIO_UPLINK, b"voice"), UsbVoiceLine("$MCP: real")]


def test_json_has_its_own_bounded_payload_limit():
    payload = b"x" * 4096
    encoded = encode_usb_voice_frame(VOICE_JSON, payload)
    assert feed_chunks(encoded, 13) == [UsbVoiceFrame(VOICE_JSON, payload)]
    assert encode_usb_voice_frame(VOICE_JSON, payload + b"x") == b""
    assert encode_usb_voice_frame(AUDIO_UPLINK, b"x" * 1276) == b""
    assert encode_usb_voice_frame(255, b"x") == b""
