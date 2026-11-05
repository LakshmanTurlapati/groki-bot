# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Grok Bot run state for the robot's face (bot_activity + gbot_brain + ESP32)."""

from __future__ import annotations

import asyncio
import json
import threading
from collections.abc import Iterator
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

import pytest
import websockets

from stackchan_mcp import bot_activity, gbot_brain
from stackchan_mcp.bot_activity import BotActivityTracker, classify_reply
from stackchan_mcp.esp32_client import ESP32Connection, ESP32Manager


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


def make_tracker(clock: FakeClock | None = None) -> tuple[BotActivityTracker, list[str]]:
    sent: list[str] = []
    tracker = BotActivityTracker(
        lambda state, ttl: sent.append(state), clock=clock or FakeClock(), is_enabled=lambda: True
    )
    return tracker, sent


# --- classification ---------------------------------------------------------


@pytest.mark.parametrize(
    ("reply", "state"),
    [
        ("The report is ready and saved to your desktop.", "done"),
        ("Which folder should it go in?", "waiting"),
        ("要不要我现在发出去？", "waiting"),
        ("I drafted the email. Should I send it now", "waiting"),
        ("Sorry, I can't help with that request.", "failed"),
        ("我无法完成这个任务。", "failed"),
        # A refusal word later in a normal answer is not a refusal.
        ("Done. The old script said it cannot connect, so I fixed the URL.", "done"),
        ("", "done"),
    ],
)
def test_classify_reply(reply: str, state: str) -> None:
    assert classify_reply(reply) == state


# --- tracker ----------------------------------------------------------------


def test_run_lifecycle_sends_each_state_once() -> None:
    tracker, sent = make_tracker()
    tracker.dispatched()
    tracker.reply("first piece.")
    tracker.reply("more.")
    tracker.done("idle", None)
    assert sent == ["thinking", "working", "done"]


def test_question_reply_waits_and_timeout_blocks() -> None:
    tracker, sent = make_tracker()
    tracker.dispatched()
    tracker.reply("Do you want the long version?")
    tracker.done("idle")
    assert sent[-1] == "waiting"
    tracker.dispatched()
    tracker.done("timeout")
    assert sent[-1] == "blocked"


def test_working_keepalive_outlives_firmware_ttl() -> None:
    clock = FakeClock()
    tracker, sent = make_tracker(clock)
    tracker.dispatched()
    tracker.reply("a")
    clock.now += 10
    tracker.reply("b")
    assert sent == ["thinking", "working"]
    clock.now += 25
    tracker.reply("c")
    assert sent == ["thinking", "working", "working"]


def test_second_failure_soon_after_a_block_is_failed() -> None:
    clock = FakeClock()
    tracker, sent = make_tracker(clock)
    tracker.dispatched()
    tracker.error("forward")
    tracker.dispatched()  # retry
    clock.now += 30
    tracker.error("forward")
    assert sent == ["thinking", "blocked", "thinking", "failed"]
    # Much later, a new failure is just blocked again.
    tracker.dispatched()
    clock.now += bot_activity.BLOCKED_MEMORY_S + 1
    tracker.error("forward")
    assert sent[-1] == "blocked"


def test_resend_only_persistent_states() -> None:
    tracker, sent = make_tracker()
    tracker.dispatched()
    tracker.resend()
    assert sent == ["thinking", "thinking"]
    tracker.done("max", "All set.")
    tracker.resend()
    assert sent == ["thinking", "thinking", "done"]


def test_disabled_sends_nothing_and_sink_errors_are_swallowed() -> None:
    sent: list[str] = []
    off = BotActivityTracker(lambda s, t: sent.append(s), is_enabled=lambda: False)
    off.dispatched()
    assert sent == []

    def boom(state: str, ttl: int) -> None:
        raise RuntimeError("socket gone")

    broken = BotActivityTracker(boom, is_enabled=lambda: True)
    broken.dispatched()  # must not raise
    assert broken.state == "thinking"


def test_env_kill_switch(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("STACKCHAN_BOT_ACTIVITY", "0")
    assert not bot_activity.enabled()
    monkeypatch.setenv("STACKCHAN_BOT_ACTIVITY", "1")
    assert bot_activity.enabled()


# --- gbot_brain against a scripted forwarding service -------------------------


class ScriptedService:
    """Tiny stand-in for the forwarding service: path -> (status, NDJSON lines)."""

    def __init__(self, routes: dict[str, tuple[int, list[dict[str, Any]]]]) -> None:
        self.routes = routes
        self.requests: list[str] = []
        service = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802 - http.server API
                length = int(self.headers.get("Content-Length") or 0)
                self.rfile.read(length)
                service.requests.append(self.path)
                status, lines = service.routes.get(self.path, (404, [{"ok": False, "error": "not found"}]))
                self.send_response(status)
                self.send_header("Content-Type", "application/x-ndjson" if status == 200 else "application/json")
                self.end_headers()
                if status == 200:
                    for line in lines:
                        self.wfile.write((json.dumps(line) + "\n").encode())
                else:
                    self.wfile.write(json.dumps(lines[0]).encode())

            def log_message(self, *args: Any) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> ScriptedService:
        self.thread.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.server.shutdown()
        self.server.server_close()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server.server_address[1]}"


@pytest.fixture
def run_states(monkeypatch: pytest.MonkeyPatch) -> Iterator[list[str]]:
    sent: list[str] = []
    tracker = BotActivityTracker(lambda s, t: sent.append(s), is_enabled=lambda: True)
    monkeypatch.setattr(bot_activity, "_TRACKER", tracker)
    monkeypatch.setenv("STACKCHAN_TOOL_BOT", "assistant")
    yield sent


def ask(service: ScriptedService, monkeypatch: pytest.MonkeyPatch) -> list[str]:
    monkeypatch.setenv("STACKCHAN_GBOT_URL", service.url)
    return [item["reply"] for item in gbot_brain.iter_gbot_replies("do the thing", timeout_s=5)]


def test_streamed_run_reports_thinking_working_done(run_states, monkeypatch) -> None:
    lines = [
        {"ok": True, "event": "first", "reply": "On it."},
        {"ok": True, "event": "more", "reply": "Finished the summary."},
        {"ok": True, "event": "done", "reason": "idle"},
    ]
    with ScriptedService({"/send": (200, lines)}) as svc:
        assert ask(svc, monkeypatch) == ["On it.", "Finished the summary."]
    assert run_states == ["thinking", "working", "done"]


def test_final_question_waits(run_states, monkeypatch) -> None:
    lines = [
        {"ok": True, "event": "first", "reply": "I found two files."},
        {"ok": True, "event": "more", "reply": "Which one should I open?"},
        {"ok": True, "event": "done", "reason": "idle"},
    ]
    with ScriptedService({"/send": (200, lines)}) as svc:
        ask(svc, monkeypatch)
    assert run_states[-1] == "waiting"


def test_done_timeout_is_blocked(run_states, monkeypatch) -> None:
    lines = [
        {"ok": True, "event": "first", "reply": "Working on it."},
        {"ok": True, "event": "done", "reason": "timeout"},
    ]
    with ScriptedService({"/send": (200, lines)}) as svc:
        ask(svc, monkeypatch)
    assert run_states[-1] == "blocked"


def test_failure_before_reply_is_not_sent_twice(run_states, monkeypatch) -> None:
    lines = [{"ok": False, "error": "no first sentence within 30 s"}]
    routes = {"/send": (200, lines), "/ask": (200, lines)}
    with ScriptedService(routes) as svc:
        with pytest.raises(gbot_brain.GbotBrainError):
            ask(svc, monkeypatch)
        # /ask is the same handler: retrying it would hand the task over again.
        assert svc.requests == ["/send"]
    assert run_states == ["thinking", "blocked"]


def test_server_error_is_blocked_without_retry(run_states, monkeypatch) -> None:
    with ScriptedService({"/send": (502, [{"ok": False, "error": "gbot not found"}])}) as svc:
        with pytest.raises(gbot_brain.GbotBrainError):
            ask(svc, monkeypatch)
        assert svc.requests == ["/send"]
    assert run_states == ["thinking", "blocked"]


def test_missing_send_route_falls_back_to_ask(run_states, monkeypatch) -> None:
    lines = [{"ok": True, "event": "first", "reply": "Hello."}, {"ok": True, "event": "done", "reason": "max"}]
    with ScriptedService({"/ask": (200, lines)}) as svc:
        assert ask(svc, monkeypatch) == ["Hello."]
        assert svc.requests == ["/send", "/ask"]
    assert run_states[-1] == "done"


def test_failure_after_reply_keeps_replies_and_blocks(run_states, monkeypatch) -> None:
    lines = [{"ok": True, "event": "first", "reply": "Started."}, {"ok": False, "error": "agent crashed"}]
    with ScriptedService({"/send": (200, lines)}) as svc:
        assert ask(svc, monkeypatch) == ["Started."]
    assert run_states == ["thinking", "working", "blocked"]


def test_caller_stopping_early_clears_the_face(run_states, monkeypatch) -> None:
    lines = [{"ok": True, "event": "first", "reply": "One."}, {"ok": True, "event": "more", "reply": "Two."}]
    with ScriptedService({"/send": (200, lines)}) as svc:
        monkeypatch.setenv("STACKCHAN_GBOT_URL", svc.url)
        replies = gbot_brain.iter_gbot_replies("do the thing", timeout_s=5)
        assert next(replies)["reply"] == "One."
        replies.close()
    assert run_states[-1] == "idle"


# --- ESP32 side -------------------------------------------------------------


class _FakeWebSocket:
    def __init__(self) -> None:
        self.sent: list[Any] = []

    async def send(self, data: Any) -> None:
        self.sent.append(data)


@pytest.mark.asyncio
async def test_connection_sends_activity_only_with_feature() -> None:
    ws = _FakeWebSocket()
    conn = ESP32Connection(ws, session_id="s")  # type: ignore[arg-type]
    await conn.send_activity("working")
    assert ws.sent == []
    conn.bot_activity_v1 = True
    await conn.send_activity("working")
    assert json.loads(ws.sent[0]) == {"type": "activity", "state": "working", "ttl_ms": 0}


@pytest.mark.asyncio
async def test_device_with_feature_receives_run_state(monkeypatch: pytest.MonkeyPatch) -> None:
    tracker = BotActivityTracker(is_enabled=lambda: True)
    monkeypatch.setattr(bot_activity, "_TRACKER", tracker)
    mgr = ESP32Manager()
    await mgr.start("127.0.0.1", 0)
    port = mgr._server.sockets[0].getsockname()[1]
    try:
        async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
            hello = {"type": "hello", "version": 1, "features": {"mcp": False, "bot_activity_v1": True}}
            await ws.send(json.dumps(hello))

            async def next_activity() -> dict[str, Any]:
                while True:
                    raw = await asyncio.wait_for(ws.recv(), 2.0)
                    if isinstance(raw, str):
                        msg = json.loads(raw)
                        if msg.get("type") == "activity":
                            return msg

            for _ in range(50):
                if mgr.connection is not None and mgr.connection.bot_activity_v1:
                    break
                await asyncio.sleep(0.02)
            assert mgr.connection is not None and mgr.connection.bot_activity_v1
            # A worker thread (like iter_gbot_replies) drives the tracker.
            await asyncio.to_thread(tracker.dispatched)
            assert (await next_activity())["state"] == "thinking"
            await asyncio.to_thread(tracker.done, "idle", "All done.")
            assert (await next_activity())["state"] == "done"
    finally:
        await mgr.stop()
    assert tracker._sink is None  # detached on stop
