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
