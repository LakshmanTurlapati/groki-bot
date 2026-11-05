# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Voice-status LED phase: off unless listening or speaking, speech wins."""

from stackchan_mcp.led_status import SPEECH_RGB, LedStatus
from stackchan_mcp.wake_gate import LISTENING_LED_RGB, WAKE_FLASH_LED_RGB


def test_startup_silence_disconnect_and_chat_close_are_off() -> None:
    led = LedStatus()
    assert led.output() == ("off", (0, 0, 0))
    led.note_chat(True)
    led.note_off()
    assert led.output() == ("off", (0, 0, 0))
    assert led.speech_pending is False


def test_listening_and_wake_flash_do_not_pulse() -> None:
    led = LedStatus()
    led.note_chat(True, flash=True)
    assert led.output() == ("listen", WAKE_FLASH_LED_RGB)
    led.note_chat(True, flash=False)
    assert led.output() == ("listen", LISTENING_LED_RGB)


def test_speech_outranks_listen_flash_and_off_until_it_has_been_shown() -> None:
    led = LedStatus()
    led.note_chat(True, flash=True)
    led.note_playback(True)
    assert led.output() == ("speech", SPEECH_RGB)

    led.note_playback(False)
    led.note_chat(True, flash=False)
    # The white frame is still owed, so a fast turn cannot skip it.
    assert led.output() == ("speech", SPEECH_RGB)
    assert led.after_send("speech") is True
    assert led.output() == ("listen", LISTENING_LED_RGB)
    assert led.after_send("listen") is False


def test_closing_the_chat_during_playback_keeps_speech_then_goes_dark() -> None:
    led = LedStatus()
    led.note_playback(True)
    led.note_chat(False)
    assert led.output()[0] == "speech"
    led.playback = False
    assert led.after_send("speech") is True
    assert led.output() == ("off", (0, 0, 0))


def test_error_clears_a_pending_speech_frame() -> None:
    led = LedStatus()
    led.note_playback(True)
    led.note_off()
    assert led.output() == ("off", (0, 0, 0))
    assert led.after_send("off") is False
