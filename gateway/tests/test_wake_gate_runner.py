# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Recovery, privacy, and native-thread ownership without models or hardware."""

import asyncio
import threading

import pytest

from stackchan_mcp.wake_gate import WakeGate, WakeWordUnavailable
from stackchan_mcp.wake_gate_runner import WakeGateRunner


class Spotter:
    available = True

    def __init__(self, hits=()):
        self.hits = iter(hits)

    def reset(self):
        pass

    def detect(self, pcm):
        return next(self.hits, False)


class ControlledSleep:
    def __init__(self):
        self.delays = asyncio.Queue()
        self.resume = asyncio.Queue()

    async def __call__(self, seconds):
        await self.delays.put(seconds)
        await self.resume.get()

    async def next_delay(self):
        return await asyncio.wait_for(self.delays.get(), 1)


async def eventually(predicate):
    async def wait():
        while not predicate():
            await asyncio.sleep(0.001)

    await asyncio.wait_for(wait(), 1)


@pytest.mark.asyncio
async def test_unavailable_retries_without_audio_forever_and_requires_fresh_wake():
    sleeper = ControlledSleep()
    builds = 0

    def factory():
        nonlocal builds
        builds += 1
        if builds <= 6:
            raise WakeWordUnavailable("model missing")
        return WakeGate(kws=Spotter([False, True]))

    runner = WakeGateRunner(factory, sleep=sleeper)
    try:
        await runner.start()
        for expected in (2, 4, 8, 16, 30, 30):
            assert await sleeper.next_delay() == expected
            assert runner.state == "UNAVAILABLE"
            assert (await runner.process(b"outage")).forward_pcm == ()
            sleeper.resume.put_nowait(None)
        await eventually(lambda: runner.state == "DORMANT")
        assert runner.failures == 6  # Construction alone does not reset backoff.
        assert (await runner.process(b"quiet")).forward_pcm == ()
        assert runner.failures == 0
        decision = await runner.process(b"wake")
        assert decision.woke
        assert decision.forward_pcm == (b"quiet", b"wake")
    finally:
        await runner.stop()


@pytest.mark.asyncio
async def test_explicit_disable_passes_audio_without_rebuilding():
    builds = 0

    def disabled():
        nonlocal builds
        builds += 1
        return None

    runner = WakeGateRunner(disabled)
    try:
        await runner.start()
        assert runner.state == "DISABLED"
        for _ in range(3):
            result = await runner.process(b"pcm")
            assert result.bypassed and result.forward_pcm == (b"pcm",)
        assert builds == 1
        assert runner._recovery is None
    finally:
        await runner.stop()


@pytest.mark.asyncio
async def test_repeated_inference_errors_keep_increasing_backoff():
    sleeper = ControlledSleep()

    class BrokenGate(WakeGate):
        def process(self, pcm):
            raise RuntimeError("inference crashed")

    runner = WakeGateRunner(lambda: BrokenGate(kws=Spotter()), sleep=sleeper)
    try:
        await runner.start()
        for expected in (2, 4, 8, 16, 30):
            with pytest.raises(RuntimeError, match="inference crashed"):
                await runner.process(b"never forwarded")
            assert runner.state == "UNAVAILABLE"
            assert await sleeper.next_delay() == expected
            sleeper.resume.put_nowait(None)
            await eventually(lambda: runner.state == "DORMANT")
    finally:
        await runner.stop()


@pytest.mark.asyncio
async def test_factory_inference_reset_and_disposal_use_one_non_loop_thread():
    calls = []

    class RecordingSpotter(Spotter):
        def reset(self):
            calls.append(("reset", threading.get_ident()))

        def detect(self, pcm):
            calls.append(("detect", threading.get_ident()))
            return True

    def factory():
        calls.append(("construct", threading.get_ident()))
        return WakeGate(kws=RecordingSpotter())

    runner = WakeGateRunner(factory)
    await runner.start()
    await runner.process(b"wake")
    assert await runner.close()
    await runner.stop()
    assert {name for name, _ in calls} == {"construct", "detect", "reset"}
    assert len({thread for _, thread in calls}) == 1
    assert calls[0][1] != threading.get_ident()


@pytest.mark.asyncio
async def test_stop_during_blocked_model_build_rejects_late_result_without_blocking_loop():
    loop = asyncio.get_running_loop()
    entered = asyncio.Event()
    release = threading.Event()
    changes = []

    def factory():
        loop.call_soon_threadsafe(entered.set)
        assert release.wait(2)
        return WakeGate(kws=Spotter())

    runner = WakeGateRunner(factory, on_change=lambda r: changes.append(r.state))
    starting = asyncio.create_task(runner.start())
    await asyncio.wait_for(entered.wait(), 1)
    stopping = asyncio.create_task(runner.stop())
    await asyncio.sleep(0)
    assert runner.state == "STOPPED"
    assert not stopping.done()
    release.set()
    await asyncio.wait_for(asyncio.gather(starting, stopping, return_exceptions=True), 1)
    assert changes == ["UNAVAILABLE"]
    assert runner.snapshot is None
    assert runner._recovery is None


@pytest.mark.asyncio
async def test_stop_during_inference_discards_late_wake_and_forwarded_audio():
    loop = asyncio.get_running_loop()
    entered = asyncio.Event()
    release = threading.Event()

    class SlowSpotter(Spotter):
        def detect(self, pcm):
            loop.call_soon_threadsafe(entered.set)
            assert release.wait(2)
            return True

    runner = WakeGateRunner(lambda: WakeGate(kws=SlowSpotter()))
    await runner.start()
    processing = asyncio.create_task(runner.process(b"wake"))
    await asyncio.wait_for(entered.wait(), 1)
    stopping = asyncio.create_task(runner.stop())
    await asyncio.sleep(0)
    release.set()
    result, _ = await asyncio.wait_for(asyncio.gather(processing, stopping), 1)
    assert not result.woke and not result.forward_pcm
    assert runner.state == "STOPPED"


@pytest.mark.asyncio
async def test_stop_cancels_pending_retry_without_another_build():
    sleeper = ControlledSleep()
    builds = 0

    def factory():
        nonlocal builds
        builds += 1
        raise WakeWordUnavailable("missing runtime")

    runner = WakeGateRunner(factory, sleep=sleeper)
    await runner.start()
    assert await sleeper.next_delay() == 2
    await runner.stop()
    sleeper.resume.put_nowait(None)
    await asyncio.sleep(0)
    assert builds == 1
    assert runner._recovery is None


@pytest.mark.asyncio
async def test_unavailable_gate_returned_by_factory_is_not_disabled():
    runner = WakeGateRunner(lambda: WakeGate(kws=None))
    try:
        await runner.start()
        assert runner.state == "UNAVAILABLE"
        assert (await runner.process(b"private")).forward_pcm == ()
    finally:
        await runner.stop()


@pytest.mark.asyncio
@pytest.mark.parametrize("failure_at", ["build", "process", "close"])
async def test_native_failure_tracebacks_do_not_move_destruction_to_event_loop(failure_at):
    calls = []

    class NativeSpotter:
        available = True

        def __init__(self):
            self.resets = 0
            calls.append(("construct", threading.get_ident()))

        def reset(self):
            self.resets += 1
            if failure_at == "close" and self.resets > 1:
                raise RuntimeError("native reset failure")

        def detect(self, pcm):
            if failure_at == "process":
                raise RuntimeError("native inference failure")
            return True

        def __del__(self):
            calls.append(("destroy", threading.get_ident()))

    def factory():
        spotter = NativeSpotter()
        if failure_at == "build":
            raise RuntimeError("native construction failure")
        return WakeGate(kws=spotter)

    runner = WakeGateRunner(factory)
    try:
        await runner.start()
        if failure_at == "process":
            with pytest.raises(RuntimeError, match="native inference failure"):
                await runner.process(b"wake")
        elif failure_at == "close":
            await runner.process(b"wake")
            assert await runner.close()
        assert runner.state == "UNAVAILABLE"
        # Destruction must already have happened before the event loop drops
        # exception records or runs garbage collection.
        assert [name for name, _ in calls] == ["construct", "destroy"]
        assert calls[0][1] == calls[1][1] != threading.get_ident()
    finally:
        await runner.stop()
