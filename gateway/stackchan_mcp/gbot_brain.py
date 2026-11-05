# SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
# SPDX-License-Identifier: MIT
"""Gateway-side client for the Grok Bot forwarding service.

The ``ask_grokbot`` voice tool in :mod:`stackchan_mcp.gemini_live_bridge`
hands a task to a Grok Bot agent through the local forwarding service
(:mod:`stackchan_mcp.gbot_http_proxy`, default ``http://127.0.0.1:18770``)
and gets the agent's replies back one piece at a time.

The whole feature is off unless ``STACKCHAN_TOOL_BOT`` names a Grok Bot
agent. Nothing here has a built-in agent name or id.

Environment:

``STACKCHAN_TOOL_BOT``
    Name of the Grok Bot agent (or group) that receives tasks. Setting it
    turns the feature on.
``STACKCHAN_TOOL_BOT_ID``
    Optional agent id, sent as ``target_id`` next to the name.
``STACKCHAN_TOOL_BOT_PREFIX``
    Text put in front of every task so the agent answers in a form that
    works when read aloud. Set it to an empty value to send the task as is.
``STACKCHAN_GBOT_URL``
    Forwarding service address. Default ``http://127.0.0.1:18770``.
``STACKCHAN_ASK_TIMEOUT``
    Seconds to wait for the whole exchange. Default 110.
``STACKCHAN_BOT_ACTIVITY``
    Set to ``0`` to stop showing the run state (thinking / working / done /
    waiting / blocked) on the robot's face; see :mod:`stackchan_mcp.bot_activity`.
"""

from __future__ import annotations

import json
import logging
import os
import re
import time
import urllib.error
import urllib.request
from collections.abc import Iterator
from typing import Any

from . import bot_activity

logger = logging.getLogger(__name__)

DEFAULT_URL = "http://127.0.0.1:18770"
DEFAULT_TIMEOUT_S = 110.0
DEFAULT_TASK_PREFIX = (
    "[Voice task forwarded by a desktop robot. Your reply will be read aloud to the user: "
    "answer in one or two spoken sentences, in the language of the task, without lists, links "
    "or markdown.] "
)

_SENT_RE = re.compile(r"(?s)(.+?(?:[。！？!?]+|\n+))")


class GbotBrainError(RuntimeError):
    """The forwarding service could not deliver a reply."""


def tool_bot() -> str:
    """Configured Grok Bot agent name; empty means the feature is off."""
    return os.getenv("STACKCHAN_TOOL_BOT", "").strip()


def tool_bot_id() -> str:
    return os.getenv("STACKCHAN_TOOL_BOT_ID", "").strip()


def grokbot_enabled() -> bool:
    return bool(tool_bot())


def task_prefix() -> str:
    raw = os.getenv("STACKCHAN_TOOL_BOT_PREFIX")
    return DEFAULT_TASK_PREFIX if raw is None else raw


def gbot_url() -> str:
    return os.getenv("STACKCHAN_GBOT_URL", DEFAULT_URL).rstrip("/")


def ask_timeout_s() -> float:
    try:
        return float(os.getenv("STACKCHAN_ASK_TIMEOUT", str(DEFAULT_TIMEOUT_S)))
    except ValueError:
        return DEFAULT_TIMEOUT_S


def first_short_sentence(text: str) -> str:
    """First complete short sentence (ends with 。！？!? or a newline), else ''."""
    raw = (text or "").strip()
    if not raw:
        return ""
    match = _SENT_RE.match(raw)
    if match:
        piece = match.group(1).strip()
        if len(re.sub(r"\s+", "", piece)) >= 2:
            return piece
    return ""


def split_first_sentence(text: str) -> tuple[str, str]:
    raw = (text or "").strip()
    first = first_short_sentence(raw)
    if not first:
        return raw, ""
    return first, raw[len(first) :].strip()


def _event(body: dict[str, Any], reply: str, t0: float, default_event: str) -> dict[str, Any]:
    first = body.get("first_reply_s")
    try:
        first_s = float(first) if first is not None else time.monotonic() - t0
    except (TypeError, ValueError):
        first_s = time.monotonic() - t0
    return {"reply": reply, "first_s": first_s, "event": str(body.get("event") or default_event)}


def iter_gbot_replies(
    text: str,
    *,
    timeout_s: float | None = None,
    bot: str | None = None,
    bot_id: str | None = None,
) -> Iterator[dict[str, Any]]:
    """Send ``text`` to a Grok Bot agent and yield its replies as they arrive.

    Each item is ``{"reply": str, "first_s": float, "event": "first" | "more"}``.
    Uses the streaming (NDJSON) response of ``POST /send``; if the service
    answers with a single JSON object instead, the reply is split into its
    first sentence and the rest. Raises :class:`GbotBrainError` when nothing
    usable comes back.
    """
    question = (text or "").strip()
    if not question:
        raise GbotBrainError("empty task")
    target = (bot or "").strip() or tool_bot()
    if not target:
        raise GbotBrainError("STACKCHAN_TOOL_BOT is not set")
    target_id = (bot_id if bot_id is not None else tool_bot_id()).strip()
    if timeout_s is None:
        timeout_s = ask_timeout_s()

    payload: dict[str, Any] = {"text": question, "target": target, "bot": target}
    if target_id:
        payload["target_id"] = target_id
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")

    t0 = time.monotonic()
    last_error: Exception | None = None
    # The robot's face follows the run: thinking -> working -> done / waiting
    # / blocked (see bot_activity). `finished` marks that an outcome was
    # reported; anything else (the caller stopped reading) clears the face.
    activity = bot_activity.tracker()
    activity.dispatched()
    finished = False
    try:
        # /ask is only a fallback for an older forwarding service without
        # /send (404). Every other failure means the task may already have
        # reached the agent, so it is never sent a second time.
        for path in ("/send", "/ask"):
            req = urllib.request.Request(
                gbot_url() + path,
                data=data,
                method="POST",
                headers={
                    "Content-Type": "application/json; charset=utf-8",
                    "X-Stackchan-Stream": "1",
                },
            )
            try:
                with urllib.request.urlopen(req, timeout=timeout_s) as resp:
                    ctype = (resp.headers.get("Content-Type") or "").lower()
                    if "ndjson" in ctype:
                        yielded = False
                        done_reason: str | None = None
                        last_reply = ""
                        stream_error: GbotBrainError | None = None
                        while True:
                            line = resp.readline()
                            if not line:
                                break
                            try:
                                body = json.loads(line.decode("utf-8"))
                            except json.JSONDecodeError:
                                continue
                            if not isinstance(body, dict):
                                continue
                            if not body.get("ok"):
                                stream_error = GbotBrainError(str(body.get("error") or "no reply"))
                                break
                            if body.get("event") == "done":
                                done_reason = str(body.get("reason") or "") or None
                                continue
                            reply = str(body.get("reply") or body.get("text") or "").strip()
                            if not reply:
                                continue
                            yielded = True
                            last_reply = reply
                            activity.reply(reply)
                            yield _event(body, reply, t0, "first")
                        if yielded:
                            # Replies already reached the caller; a later
                            # failure is reported to the face, not raised.
                            if stream_error is not None:
                                logger.warning("gbot %s failed after replying: %s", path, stream_error)
                                activity.error("stream")
                            else:
                                activity.done(done_reason, last_reply)
                            finished = True
                            return
                        last_error = stream_error or GbotBrainError("empty streaming reply")
                        break
                    raw = resp.read().decode("utf-8")
            except urllib.error.HTTPError as exc:
                detail = ""
                try:
                    detail = str(json.loads(exc.read().decode("utf-8")).get("error") or "")
                except Exception:  # noqa: BLE001 - best effort detail only
                    pass
                last_error = GbotBrainError(detail or f"HTTP {exc.code}")
                logger.warning("gbot %s failed: %s", path, last_error)
                if exc.code != 404:
                    break
                continue
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                last_error = exc
                logger.warning("gbot %s failed: %s", path, exc)
                break
            try:
                body = json.loads(raw)
            except json.JSONDecodeError as exc:
                last_error = exc
                break
            if not isinstance(body, dict) or not body.get("ok"):
                last_error = GbotBrainError(str((body or {}).get("error") or "no reply"))
                break
            reply = str(body.get("reply") or body.get("text") or "").strip()
            if not reply:
                last_error = GbotBrainError("empty reply")
                break
            head, rest = split_first_sentence(reply)
            activity.reply(head or reply)
            yield _event(body, head or reply, t0, "first")
            if rest:
                activity.reply(rest)
                yield {"reply": rest, "first_s": time.monotonic() - t0, "event": "more"}
            activity.done(None, reply)
            finished = True
            return
        activity.error("timeout" if isinstance(last_error, TimeoutError) else "forward")
        finished = True
        raise GbotBrainError(f"Grok Bot forwarding failed: {last_error}") from last_error
    finally:
        if not finished:
            activity.abandon()


def ask_gbot(
    text: str,
    *,
    timeout_s: float | None = None,
    bot: str | None = None,
    bot_id: str | None = None,
) -> str:
    """Blocking helper: all reply pieces joined into one string."""
    parts = [
        str(item.get("reply") or "").strip()
        for item in iter_gbot_replies(text, timeout_s=timeout_s, bot=bot, bot_id=bot_id)
    ]
    reply = " ".join(part for part in parts if part).strip()
    if not reply:
        raise GbotBrainError("empty reply")
    return reply
