"""Bounded sentence -> voice -> transport pipeline, independent of model libraries."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Awaitable, Callable
from contextlib import aclosing
from time import perf_counter


class TurnTiming:
    """Elapsed server time; first PCM sent is not the time heard at the speaker."""

    def __init__(self, log: Callable[[str], None]):
        self.started = perf_counter()
        self.log = log
        self.marks: dict[str, float] = {}

    def mark(self, name: str) -> None:
        if name not in self.marks:
            ms = (perf_counter() - self.started) * 1000
            self.marks[name] = ms
            self.log(f"[Latency] {name}={ms:.0f}ms from response.create")


class PcmPacer:
    """PCM16 mono media clock, shared across all sentences in a response.

    Keep at most half a second of scheduled audio ahead of wall time. Reset
    the clock after synthesis gaps so later sentences don't inherit old debt.
    """

    def __init__(self, sample_rate: int = 24000, lead_seconds: float = 0.5,
                 clock=perf_counter, sleep=asyncio.sleep):
        if sample_rate <= 0 or lead_seconds < 0:
            raise ValueError("Invalid PCM pacing configuration")
        self.bytes_per_second = sample_rate * 2
        self.lead_seconds = lead_seconds
        self.clock = clock
        self.sleep = sleep
        self.scheduled_end = 0.0

    async def wait(self, byte_count: int) -> None:
        if byte_count < 0 or byte_count % 2:
            raise ValueError("PCM16 data must contain whole two-byte samples")
        now = self.clock()
        self.scheduled_end = max(self.scheduled_end, now) + byte_count / self.bytes_per_second
        delay = self.scheduled_end - now - self.lead_seconds
        if delay > 0:
            await self.sleep(delay)


async def stream_speech(
    sentences: AsyncIterator[str],
    synthesize: Callable[[str], Awaitable[bytes]],
    send_pcm: Callable[[bytes], Awaitable[None]],
    delivered: list[str],
) -> None:
    """Overlap generation, serial synthesis and sending while preserving speech order.

    Two text slots and one audio slot bound lookahead. Piper/RVC is called by
    only one worker. On error/cancellation, close the source and stop all async
    workers. An underlying to_thread synthesis may finish its current phrase.
    `delivered` records only phrases whose PCM was fully handed to transport.
    """
    text_queue: asyncio.Queue[str | None] = asyncio.Queue(maxsize=2)
    pcm_queue: asyncio.Queue[tuple[str, bytes] | None] = asyncio.Queue(maxsize=1)

    async def produce() -> None:
        async with aclosing(sentences):
            async for sentence in sentences:
                if sentence.strip():
                    await text_queue.put(sentence)
        await text_queue.put(None)

    async def synthesize_worker() -> None:
        while True:
            sentence = await text_queue.get()
            if sentence is None:
                await pcm_queue.put(None)
                return
            pcm = await synthesize(sentence)
            if not pcm:
                raise RuntimeError("TTS returned no audio for a sentence")
            await pcm_queue.put((sentence, pcm))

    async def send_worker() -> None:
        while True:
            item = await pcm_queue.get()
            if item is None:
                return
            sentence, pcm = item
            await send_pcm(pcm)
            delivered.append(sentence)

    tasks = [asyncio.create_task(fn()) for fn in (produce, synthesize_worker, send_worker)]
    try:
        await asyncio.gather(*tasks)
    finally:
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
