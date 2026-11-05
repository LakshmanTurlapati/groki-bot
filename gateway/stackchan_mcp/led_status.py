# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Voice-status LED phase.

The strip is off unless a voice chat is open or the robot is speaking.
Speaking is white and outranks listen, wake flash, idle, and any model colour.
A speech phase that has not been shown yet is still emitted once, so a fast
turn cannot skip the white and land on the resting colour.
"""

from __future__ import annotations

from dataclasses import dataclass

from .wake_gate import LISTENING_LED_RGB, WAKE_FLASH_LED_RGB

OFF_RGB = (0, 0, 0)
SPEECH_RGB = (255, 255, 255)


@dataclass
class LedStatus:
    playback: bool = False
    chat: bool = False
    flash: bool = False
    # Set when speech is requested and cleared only after a speech frame is sent.
    speech_pending: bool = False
    generation: int = 0

    def touch(self) -> int:
        self.generation += 1
        return self.generation

    def note_off(self) -> None:
        """Idle, startup, disconnect, error, chat close, or stop."""
        self.playback = False
        self.chat = False
        self.flash = False
        self.speech_pending = False
        self.touch()

    def note_playback(self, active: bool) -> None:
        """Assistant audio started or finished. Does not by itself close the chat."""
        self.playback = active
        if active:
            self.speech_pending = True
        else:
            self.flash = False
        self.touch()

    def note_chat(self, active: bool, *, flash: bool = False) -> None:
        """Listening window / voice chat. ``flash`` is the wake confirmation."""
        self.chat = active
        self.flash = bool(active and flash)
        if not active:
            self.flash = False
        self.touch()

    def output(self) -> tuple[str, tuple[int, int, int]]:
        """Wire mode and RGB. Speech wins over listen and off."""
        if self.playback or self.speech_pending:
            return "speech", SPEECH_RGB
        if self.chat and self.flash:
            return "listen", WAKE_FLASH_LED_RGB
        if self.chat:
            return "listen", LISTENING_LED_RGB
        return "off", OFF_RGB

    def after_send(self, mode: str) -> bool:
        """Drop a speech frame that has now been shown. True when another send is owed."""
        if mode == "speech" and self.speech_pending and not self.playback:
            self.speech_pending = False
            self.touch()
            return True
        if mode == "speech" and self.playback:
            self.speech_pending = False
        return False
