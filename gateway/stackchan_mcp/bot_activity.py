# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Grok Bot run state for the robot's face.

Grok Bot never streams its state to a device, so the gateway is the only
place that can see what a hand-off run is doing. This module turns the run
lifecycle into the avatar states the firmware draws and sends them with a
XiaoZhi ``{"type": "activity", "state": ..., "ttl_ms": 0}`` message (only to
firmware that announces ``features.bot_activity_v1`` in its hello).

States (see docs/design/grokbot-faces/spec.html):

``thinking``  task sent, no reply yet
``working``   reply pieces arriving
``waiting``   the reply ends in a question or an approval ask
``blocked``   the forwarding service or the agent failed, or timed out
``failed``    a second failure soon after a block, or the agent refused
``done``      the run finished normally (the firmware plays the hop, then idles)
``idle``      clears the run state

The firmware expires every state on its own (Done after its clip, Thinking /
Working after 120 s, Blocked turns into Failed after 30 s), so a gateway
that dies mid-run cannot pin the face. Set ``STACKCHAN_BOT_ACTIVITY=0`` to
stop sending these messages.
"""

from __future__ import annotations

import logging
import os
import re
import threading
import time
from collections.abc import Callable

logger = logging.getLogger(__name__)

# (state, ttl_ms) -> None. Must be safe to call from any thread.
Sink = Callable[[str, int], None]

# Re-send a long-running state this often so it outlives the firmware TTL.
KEEPALIVE_S = 30.0
# An error this soon after a block means the retry failed too.
BLOCKED_MEMORY_S = 120.0
# Persistent states that a reconnecting robot should see again.
_RESENDABLE = frozenset({"thinking", "working", "waiting", "blocked"})

_QUESTION_END = re.compile(r"[?？]\s*[\"'”’)）]*\s*$")
_APPROVAL = re.compile(
    r"\b(?:should i|shall i|do you want me to|would you like me to|want me to|"
    r"can you confirm|please confirm|ok to proceed|okay to proceed|let me know if you want)\b"
    r"|要不要|是否需要|需要我|请确认|可以吗|好吗",
    re.IGNORECASE,
)
# Only the opening of the reply is checked: a refusal leads with it.
_REFUSAL = re.compile(
    r"^\W*(?:sorry,?\s*)?(?:i can(?:no|')t help|i cannot help|i(?:'m| am) (?:unable|not able) to|"
    r"i can(?:no|')t do that|i won'?t be able to)"
    r"|^\W*(?:抱歉[，,]?\s*)?我(?:无法|不能|做不到)",
    re.IGNORECASE,
)


def enabled() -> bool:
    return os.getenv("STACKCHAN_BOT_ACTIVITY", "1").strip().lower() not in {"0", "false", "no", "off"}


def classify_reply(text: str) -> str:
    """Final-reply state: ``waiting`` (question / approval ask), ``failed`` (refusal) or ``done``."""
    reply = (text or "").strip()
    if not reply:
        return "done"
    if _REFUSAL.search(reply[:120]):
        return "failed"
    if _QUESTION_END.search(reply) or _APPROVAL.search(reply[-200:]):
        return "waiting"
    return "done"


class BotActivityTracker:
    """Thread-safe run-state machine. Call the event methods from any thread."""

    def __init__(
        self,
        sink: Sink | None = None,
        *,
        clock: Callable[[], float] = time.monotonic,
        keepalive_s: float = KEEPALIVE_S,
        is_enabled: Callable[[], bool] = enabled,
    ) -> None:
        self._sink = sink
        self._clock = clock
        self._keepalive_s = keepalive_s
        self._is_enabled = is_enabled
        self._lock = threading.Lock()
        self._state = "idle"
        self._sent_at = 0.0
        self._blocked_at: float | None = None
        self._last_reply = ""

    @property
    def state(self) -> str:
        with self._lock:
            return self._state

    def set_sink(self, sink: Sink | None) -> None:
        with self._lock:
            self._sink = sink

    # --- run lifecycle ----------------------------------------------------

    def dispatched(self) -> None:
        """A task was handed to the agent."""
        with self._lock:
            self._last_reply = ""
        self._move("thinking")

    def reply(self, text: str = "") -> None:
        """A reply piece arrived (first or more)."""
        with self._lock:
            if text:
                self._last_reply = text
        self._move("working", keepalive=True)

    def done(self, reason: str | None = None, text: str | None = None) -> None:
        """The run finished. A ``timeout`` reason counts as a failure to finish."""
        if (reason or "").lower() == "timeout":
            self.error("timeout")
            return
        with self._lock:
            final = text if text else self._last_reply
        self._move(classify_reply(final))

    def error(self, kind: str = "error") -> None:
        """The forwarding service or the agent failed."""
        now = self._clock()
        with self._lock:
            repeat = self._blocked_at is not None and now - self._blocked_at <= BLOCKED_MEMORY_S
        logger.info("Grok Bot run failed (%s)%s", kind, ", again" if repeat else "")
        self._move("failed" if repeat else "blocked")

    def abandon(self) -> None:
        """The run stopped without an outcome (caller gave up): clear the face."""
        self._move("idle")

    def resend(self) -> None:
        """Re-send a persistent state, e.g. after the robot reconnects."""
        with self._lock:
            state = self._state
        if state in _RESENDABLE:
            self._move(state, force=True)

    # --- internals ----------------------------------------------------------

    def _move(self, state: str, *, keepalive: bool = False, force: bool = False) -> None:
        if not self._is_enabled():
            return
        now = self._clock()
        with self._lock:
            unchanged = state == self._state
            if unchanged and not force:
                if not keepalive or now - self._sent_at < self._keepalive_s:
                    return
            self._state = state
            self._sent_at = now
            if state == "blocked":
                self._blocked_at = now
            elif state in {"done", "waiting"}:
                self._blocked_at = None
            sink = self._sink
        if not unchanged:
            logger.info("Grok Bot activity -> %s", state)
        if sink is None:
            return
        try:
            sink(state, 0)
        except Exception:  # noqa: BLE001 - the face must never break a run
            logger.debug("activity sink failed", exc_info=True)


_TRACKER = BotActivityTracker()


def tracker() -> BotActivityTracker:
    """The process-wide tracker (the ESP32 manager attaches its sink)."""
    return _TRACKER
