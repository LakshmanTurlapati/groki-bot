"""One-command end-to-end self-check.

Runs four checks in order and prints a PASS/FAIL table; the exit code is
non-zero when any check fails:

1. Status: ``GET /debug/status`` shows device / gemini / wake_gate online.
2. Text Q&A: ``POST /debug/inject-text`` injects a question ("自检：现在几点",
   "self-check: what time is it"); a new transcript must appear within 15 s.
3. Voice wake (only with ``--with-voice``, needs the Mac speaker): ``say -v
   Tingting`` speaks the wake word; ``STACKCHAN_E2E_SAY_DEVICE`` is passed to
   ``say -a`` to pick the output device. A new wake must be recorded within
   15 s.
4. Music control: inject a "play a song" request and check that the player
   state becomes playing, then a "pause" request and check paused (Spotify
   first, then Music, with the same logic as mac_control).

Usage::

    cd gateway && uv run python -m stackchan_mcp.e2e_check
    uv run python -m stackchan_mcp.e2e_check --with-voice
    ./scripts/e2e_check.sh --with-voice

Requests go straight to the loopback address and bypass the system proxy (a
local HTTP_PROXY would hijack localhost). All external I/O (HTTP, child
processes, sleep, clock) is injectable, so unit tests drive the core logic
with fakes. The injected prompts are Chinese on purpose: they exercise the
Chinese voice path.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from .mac_control import _MEDIA_NO_PLAYER, _media_player_script, _media_state_script

DEFAULT_BASE_URL = "http://127.0.0.1:8766"
DEFAULT_TIMEOUT_S = 15.0
TEXT_QA_PROMPT = "自检：现在几点"
VOICE_WAKE_UTTERANCE = "Hey Grok. 自检测试"
SAY_DEVICE_ENV = "STACKCHAN_E2E_SAY_DEVICE"
MUSIC_PLAY_PROMPT = "帮我放首歌"
MUSIC_PAUSE_PROMPT = "暂停音乐"


@dataclass
class CheckResult:
    """Outcome of one check."""

    name: str
    passed: bool
    detail: str = ""
    skipped: bool = False

    @property
    def label(self) -> str:
        if self.skipped:
            return "SKIP"
        return "PASS" if self.passed else "FAIL"


def _no_proxy_opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _default_fetch_json(url: str, *, timeout: float) -> Any:
    with _no_proxy_opener().open(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _default_post_json(
    url: str,
    payload: dict[str, Any],
    headers: dict[str, str],
    *,
    timeout: float,
) -> Any:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json", **headers},
        method="POST",
    )
    with _no_proxy_opener().open(request, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _default_run_cmd(cmd: list[str]) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except FileNotFoundError:
        return 127, f"{cmd[0]} not found"
    except subprocess.TimeoutExpired:
        return 124, "timed out"
    output = proc.stdout.strip() or proc.stderr.strip()
    return proc.returncode, output


def gateway_token_from_env() -> str:
    """inject-text uses the gateway auth token (same as the capture server)."""
    return (
        os.getenv("STACKCHAN_TOKEN")
        or os.getenv("BEARER_TOKEN")
        or os.getenv("VISION_TOKEN")
        or ""
    )


def voice_wake_say_command() -> tuple[list[str], str | None]:
    """Build the macOS say command for the voice wake check."""
    say_device = os.getenv(SAY_DEVICE_ENV)
    cmd = ["say", "-v", "Tingting"]
    if say_device:
        cmd.extend(["-a", say_device])
    cmd.append(VOICE_WAKE_UTTERANCE)
    return cmd, say_device


class E2EChecker:
    """Core self-check logic. All external I/O is injected through the constructor."""

    def __init__(
        self,
        *,
        base_url: str = DEFAULT_BASE_URL,
        token: str = "",
        timeout_s: float = DEFAULT_TIMEOUT_S,
        poll_interval_s: float = 1.0,
        fetch_json: Callable[..., Any] | None = None,
        post_json: Callable[..., Any] | None = None,
        run_cmd: Callable[[list[str]], tuple[int, str]] | None = None,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout_s = timeout_s
        self.poll_interval_s = poll_interval_s
        self._fetch_json = fetch_json or _default_fetch_json
        self._post_json = post_json or _default_post_json
        self._run_cmd = run_cmd or _default_run_cmd
        self._sleep = sleep
        self._clock = clock

    # ---- helpers ---------------------------------------------------------

    def fetch_status(self) -> dict[str, Any]:
        return self._fetch_json(f"{self.base_url}/debug/status", timeout=5.0)

    def inject_text(self, text: str) -> dict[str, Any]:
        headers = {"Authorization": f"Bearer {self.token}"} if self.token else {}
        return self._post_json(
            f"{self.base_url}/debug/inject-text",
            {"text": text},
            headers,
            timeout=10.0,
        )

    def _poll(
        self,
        predicate: Callable[[], tuple[bool, str]],
    ) -> tuple[bool, str]:
        """Poll predicate every poll_interval; on timeout return the last detail."""
        deadline = self._clock() + self.timeout_s
        while True:
            ok, detail = predicate()
            if ok:
                return True, detail
            if self._clock() >= deadline:
                return False, detail
            self._sleep(self.poll_interval_s)

    # ---- check 1: status --------------------------------------------------

    def check_status(self) -> CheckResult:
        name = "Status"
        try:
            st = self.fetch_status()
        except Exception as exc:
            return CheckResult(name, False, f"cannot read /debug/status: {exc}")
        problems: list[str] = []
        if not st.get("device", {}).get("connected"):
            problems.append("device not connected")
        if not st.get("gemini", {}).get("connected"):
            problems.append("gemini not connected")
        if not st.get("wake_gate", {}).get("available"):
            problems.append("wake_gate unavailable")
        if problems:
            return CheckResult(name, False, "；".join(problems))
        return CheckResult(name, True, "device / gemini / wake_gate all online")

    # ---- check 2: text Q&A --------------------------------------------------

    @staticmethod
    def _latest_transcript_at(st: dict[str, Any]) -> float:
        entries = st.get("recent", {}).get("transcripts", [])
        return max((float(e.get("at", 0.0)) for e in entries), default=0.0)

    def check_text_qa(self) -> CheckResult:
        name = "Text Q&A"
        try:
            baseline = self._latest_transcript_at(self.fetch_status())
            resp = self.inject_text(TEXT_QA_PROMPT)
        except Exception as exc:
            return CheckResult(name, False, f"inject failed: {exc}")
        if not resp.get("ok") or not resp.get("active_session"):
            return CheckResult(
                name,
                False,
                f"inject-text did not reach an active session: {resp}",
            )

        def new_transcript() -> tuple[bool, str]:
            st = self.fetch_status()
            latest = self._latest_transcript_at(st)
            if latest > baseline:
                texts = st["recent"]["transcripts"]
                return True, f"new reply: {texts[0].get('text', '')}"
            return False, f"{self.timeout_s:.0f} s without a new transcript"

        ok, detail = self._poll(new_transcript)
        return CheckResult(name, ok, detail)

    # ---- check 3: voice wake --------------------------------------------------

    def check_voice_wake(self) -> CheckResult:
        name = "Voice wake"
        try:
            baseline = int(
                self.fetch_status().get("wake_gate", {}).get("wake_count", 0)
            )
        except Exception as exc:
            return CheckResult(name, False, f"cannot read the baseline status: {exc}")
        cmd, say_device = voice_wake_say_command()
        rc, output = self._run_cmd(cmd)
        if rc != 0:
            detail = f"say failed (rc={rc}): {output}"
            if say_device:
                detail += (
                    f"; {SAY_DEVICE_ENV}={say_device} is set, "
                    "run `say -a '?'` to list valid output devices"
                )
            return CheckResult(name, False, detail)

        def woke() -> tuple[bool, str]:
            st = self.fetch_status()
            count = int(st.get("wake_gate", {}).get("wake_count", 0))
            if count > baseline:
                return True, f"wake count {baseline} → {count}"
            return False, f"{self.timeout_s:.0f} s without a new wake"

        ok, detail = self._poll(woke)
        return CheckResult(name, ok, detail)

    # ---- check 4: music control --------------------------------------------------

    def _player_state(self) -> str:
        """Return playing/paused/stopped, or no-player when no player runs."""
        rc, player = self._run_cmd(["osascript", "-e", _media_player_script()])
        if rc != 0 or not player or player == _MEDIA_NO_PLAYER:
            return "no-player"
        rc, state = self._run_cmd(["osascript", "-e", _media_state_script(player)])
        if rc != 0:
            return f"error:{state}"
        return state.strip().lower()

    def _expect_player_state(self, expected: str) -> tuple[bool, str]:
        def probe() -> tuple[bool, str]:
            state = self._player_state()
            if state == expected:
                return True, f"player state = {state}"
            return False, f"player state = {state} (expected {expected})"

        return self._poll(probe)

    def check_music(self) -> CheckResult:
        name = "Music control"
        try:
            resp = self.inject_text(MUSIC_PLAY_PROMPT)
        except Exception as exc:
            return CheckResult(name, False, f"injecting the play request failed: {exc}")
        if not resp.get("ok") or not resp.get("active_session"):
            return CheckResult(name, False, f"the play request did not reach an active session: {resp}")
        ok, detail = self._expect_player_state("playing")
        if not ok:
            return CheckResult(name, False, f"playback did not start: {detail}")

        try:
            resp = self.inject_text(MUSIC_PAUSE_PROMPT)
        except Exception as exc:
            return CheckResult(name, False, f"injecting the pause request failed: {exc}")
        if not resp.get("ok") or not resp.get("active_session"):
            return CheckResult(name, False, f"the pause request did not reach an active session: {resp}")
        ok, detail = self._expect_player_state("paused")
        if not ok:
            return CheckResult(name, False, f"playback did not pause: {detail}")
        return CheckResult(name, True, "play and pause both confirmed")

    # ---- run all --------------------------------------------------------------

    def run_all(
        self,
        *,
        with_voice: bool = False,
        skip_music: bool = False,
    ) -> list[CheckResult]:
        results = [self.check_status()]
        gateway_unreachable = not results[0].passed and results[0].detail.startswith(
            "cannot read"
        )
        if gateway_unreachable:
            reason = "gateway unreachable, skipped"
            results.append(CheckResult("Text Q&A", False, reason, skipped=True))
            if with_voice:
                results.append(CheckResult("Voice wake", False, reason, skipped=True))
            if not skip_music:
                results.append(CheckResult("Music control", False, reason, skipped=True))
            return results

        results.append(self.check_text_qa())
        if with_voice:
            results.append(self.check_voice_wake())
        else:
            results.append(
                CheckResult(
                    "Voice wake",
                    True,
                    "not enabled (add --with-voice; needs the Mac speaker)",
                    skipped=True,
                )
            )
        if skip_music:
            results.append(
                CheckResult("Music control", True, "skipped (--skip-music)", skipped=True)
            )
        else:
            results.append(self.check_music())
        return results


def render_report(results: list[CheckResult]) -> str:
    """Format the PASS/FAIL summary table."""
    width = max(len(r.name) for r in results)
    lines = ["=" * 56, "Groki Bot end-to-end self-check", "=" * 56]
    for r in results:
        lines.append(f"[{r.label}] {r.name.ljust(width)}  {r.detail}")
    lines.append("=" * 56)
    passed = sum(1 for r in results if r.passed and not r.skipped)
    failed = sum(1 for r in results if not r.passed and not r.skipped)
    skipped = sum(1 for r in results if r.skipped)
    lines.append(f"Result: {passed} passed, {failed} failed, {skipped} skipped")
    return "\n".join(lines)


def has_failure(results: list[CheckResult]) -> bool:
    return any(not r.passed and not r.skipped for r in results)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m stackchan_mcp.e2e_check",
        description="One-command end-to-end self-check for the Groki Bot gateway",
    )
    parser.add_argument(
        "--base-url",
        default=os.getenv("STACKCHAN_DEBUG_BASE_URL", DEFAULT_BASE_URL),
        help=f"capture server URL (default {DEFAULT_BASE_URL})",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_S,
        help=f"seconds to wait for each check (default {DEFAULT_TIMEOUT_S:.0f})",
    )
    parser.add_argument(
        "--with-voice",
        action="store_true",
        help="run the voice wake check (speaks the wake word through the Mac speaker)",
    )
    parser.add_argument(
        "--skip-music",
        action="store_true",
        help="skip the music control check",
    )
    args = parser.parse_args(argv)

    checker = E2EChecker(
        base_url=args.base_url,
        token=gateway_token_from_env(),
        timeout_s=args.timeout,
    )
    results = checker.run_all(with_voice=args.with_voice, skip_music=args.skip_music)
    print(render_report(results))
    return 1 if has_failure(results) else 0


if __name__ == "__main__":
    sys.exit(main())
