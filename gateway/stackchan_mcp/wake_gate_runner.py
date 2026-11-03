# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Serial native wake-word ownership with asynchronous, fail-closed recovery."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any

from .wake_gate import WakeGate, WakeGateResult, WakeGateState, WakeWordUnavailable

logger = logging.getLogger(__name__)


class WakeGateProcessError(RuntimeError):
    """Inference failure with the window state observed by the serial owner."""

    def __init__(self, error: Exception, *, closed: bool):
        super().__init__(str(error))
        self.closed = closed


@dataclass(frozen=True)
class _NativeFailure:
    message: str


@dataclass(frozen=True)
class WakeGateSnapshot:
    """Event-loop-readable state; never exposes a native model or stream."""

    state: WakeGateState
    available: bool = True

    @property
    def is_listening(self) -> bool:
        return self.state == WakeGateState.LISTENING


class WakeGateRunner:
    """Own a gate on one worker thread for its entire lifetime.

    A factory returning None explicitly selects passthrough (also useful for
    injected test/embedding factories). A requested but unusable gate must
    raise, or return a gate with available=False; both keep input blocked.
    """

    def __init__(
        self,
        factory: Callable[[], WakeGate | None],
        *,
        on_change: Callable[[WakeGateRunner], None] | None = None,
        sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
    ) -> None:
        self.factory = factory
        self.on_change = on_change
        self.sleep = sleep
        self.snapshot: WakeGateSnapshot | None = None
        self.health = "UNAVAILABLE"
        self.failures = 0
        self.retry_delay_s: float | None = None
        self.last_error: str | None = None
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="wake-gate")
        self._lock = asyncio.Lock()
        self._running = False
        self._generation = 0
        self._recovery: asyncio.Task[None] | None = None
        self._initialization: asyncio.Task[None] | None = None
        # Only the executor thread accesses these fields.
        self._native_gate: WakeGate | None = None
        self._native_tts_active = False

    @property
    def state(self) -> str:
        return self.snapshot.state.value if self.snapshot is not None else self.health

    @property
    def is_listening(self) -> bool:
        return bool(self.snapshot and self.snapshot.is_listening)

    def _current(self, generation: int) -> bool:
        return self._running and generation == self._generation

    def _changed(self) -> None:
        if self.on_change is not None:
            self.on_change(self)

    async def _run(self, function: Callable[..., Any], *args: Any) -> Any:
        result = await asyncio.get_running_loop().run_in_executor(
            self._executor, self._call_native, function, args
        )
        if isinstance(result, _NativeFailure):
            raise RuntimeError(result.message)
        return result

    @staticmethod
    def _call_native(function: Callable[..., Any], args: tuple[Any, ...]) -> Any:
        try:
            return function(*args)
        except Exception as exc:
            # Exception tracebacks retain native model/stream locals. Dispose
            # them here instead of transferring their ownership to asyncio.
            return _NativeFailure(str(exc))

    def _dispose_native(self) -> None:
        gate, self._native_gate = self._native_gate, None
        if gate is not None:
            try:
                gate.close()
            except Exception as exc:
                logger.debug("failed wake gate reset during disposal: %s", str(exc))

    def _build_native(self) -> WakeGateSnapshot | None:
        self._dispose_native()
        gate = self.factory()
        if gate is None:
            return None
        self._native_gate = gate
        try:
            if not gate.available:
                raise WakeWordUnavailable("wake word factory returned an unavailable gate")
            gate.is_tts_active = lambda: self._native_tts_active
            # Rebuilt streams start dormant and never replay the outage audio.
            gate.close()
            return WakeGateSnapshot(gate.state)
        except Exception:
            self._dispose_native()
            raise

    def _process_native(self, pcm: bytes, tts_active: bool) -> tuple[WakeGateResult, WakeGateSnapshot]:
        gate = self._native_gate
        if gate is None:
            raise WakeWordUnavailable("wake word model is not loaded")
        self._native_tts_active = tts_active
        try:
            decision = gate.process(pcm)
            return decision, WakeGateSnapshot(gate.state)
        except Exception:
            self._dispose_native()
            raise

    def _close_native(self) -> WakeGateSnapshot:
        gate = self._native_gate
        if gate is None:
            raise WakeWordUnavailable("wake word model is not loaded")
        try:
            gate.close()
            return WakeGateSnapshot(gate.state)
        except Exception:
            self._dispose_native()
            raise

    def _failed(self, exc: Exception) -> None:
        self.snapshot = None
        self.health = "UNAVAILABLE"
        self.failures += 1
        self.retry_delay_s = min(30.0, 2.0 ** min(self.failures, 5))
        self.last_error = str(exc)
        logger.warning("wake gate unavailable; retrying in %.0fs: %s", self.retry_delay_s, exc)
        self._changed()

    async def _build(self, generation: int) -> bool:
        async with self._lock:
            if not self._current(generation):
                return False
            try:
                snapshot = await self._run(self._build_native)
            except Exception as exc:
                if self._current(generation):
                    self._failed(exc)
                return False
            if not self._current(generation):
                return False
            self.snapshot = snapshot
            self.health = "READY" if snapshot is not None else "DISABLED"
            self.retry_delay_s = None
            self.last_error = None
            # Keep a failure streak until inference also succeeds: a model
            # that builds but crashes on every frame must still back off.
            self._changed()
            return True

    async def start(self, *, wait_ready: bool = True) -> None:
        self._running = True
        self._generation += 1
        self._changed()
        self._initialization = asyncio.create_task(
            self._initialize(self._generation), name="wake-gate-initialization"
        )
        if wait_ready:
            await self.wait_initialized()

    async def wait_initialized(self) -> None:
        """Wait for the first attempt, whether ready, disabled, or unavailable."""
        if self._initialization is not None:
            await self._initialization

    async def _initialize(self, generation: int) -> None:
        if not await self._build(generation):
            self._ensure_recovery()

    def _ensure_recovery(self) -> None:
        if self._running and (self._recovery is None or self._recovery.done()):
            self._recovery = asyncio.create_task(self._recover(self._generation), name="wake-gate-recovery")

    async def _recover(self, generation: int) -> None:
        while self._current(generation) and self.health == "UNAVAILABLE":
            await self.sleep(self.retry_delay_s or 2.0)
            if await self._build(generation):
                return

    async def process(self, pcm: bytes, *, tts_active: bool = False) -> WakeGateResult:
        if not self._running:
            return WakeGateResult()
        if self.health == "DISABLED":
            return WakeGateResult(forward_pcm=(pcm,), bypassed=True)
        if self.health != "READY":
            return WakeGateResult()
        generation = self._generation
        async with self._lock:
            if not self._current(generation) or self.health != "READY":
                return WakeGateResult()
            was_listening = self.is_listening
            try:
                decision, snapshot = await self._run(self._process_native, pcm, tts_active)
            except Exception as exc:
                if not self._current(generation):
                    return WakeGateResult()
                self._failed(exc)
                self._ensure_recovery()
                raise WakeGateProcessError(exc, closed=was_listening) from exc
            if not self._current(generation):
                return WakeGateResult()
            self.snapshot = snapshot
            self.failures = 0
            self._changed()
            return decision

    async def close(self) -> bool:
        generation = self._generation
        async with self._lock:
            if not self._current(generation) or self.health != "READY":
                return False
            was_listening = self.is_listening
            try:
                snapshot = await self._run(self._close_native)
            except Exception as exc:
                if self._current(generation):
                    self._failed(exc)
                    self._ensure_recovery()
            else:
                if self._current(generation):
                    self.snapshot = snapshot
                    self._changed()
            return was_listening and self._current(generation)

    async def stop(self) -> None:
        self._running = False
        self._generation += 1
        self.snapshot = None
        self.health = "STOPPED"
        recovery, self._recovery = self._recovery, None
        initialization, self._initialization = self._initialization, None
        tasks = [task for task in (initialization, recovery) if task is not None]
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        async with self._lock:
            await self._run(self._dispose_native)
        # Joining a native inference already in progress must not block asyncio.
        await asyncio.to_thread(self._executor.shutdown, wait=True, cancel_futures=True)
