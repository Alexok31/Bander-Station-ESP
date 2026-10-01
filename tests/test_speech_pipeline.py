"""Offline regressions: no models, GPU, network, credentials or running server needed."""

import ast
import asyncio
import base64
from contextlib import aclosing
import json
from pathlib import Path
import re
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SERVER_DIR = Path(__file__).resolve().parents[1] / "BendeRadio/examples/BenderRealtime/local_server"
sys.path.insert(0, str(SERVER_DIR))
from speech_pipeline import PcmPacer, TurnTiming, stream_speech


async def sentences(*items):
    for item in items:
        yield item


class PipelineTests(unittest.IsolatedAsyncioTestCase):
    async def test_first_audio_does_not_wait_for_final_sentence(self):
        first_sent = asyncio.Event()
        sent = []
        delivered = []

        async def source():
            yield "first"
            await first_sent.wait()  # A collect-entire-reply implementation deadlocks here.
            yield "second"

        async def synth(text):
            return text.encode()

        async def send(pcm):
            sent.append(pcm)
            first_sent.set()

        await asyncio.wait_for(stream_speech(source(), synth, send, delivered), 1)
        self.assertEqual(sent, [b"first", b"second"])
        self.assertEqual(delivered, ["first", "second"])

    async def test_next_synthesis_overlaps_previous_transmission_in_order(self):
        second_ready = asyncio.Event()
        delivered = []
        active = 0

        async def synth(text):
            nonlocal active
            active += 1
            self.assertEqual(active, 1, "Voice synthesis must remain serial")
            await asyncio.sleep(0)
            if text == "second":
                second_ready.set()
            active -= 1
            return text.encode()

        async def send(pcm):
            if pcm == b"first":
                await second_ready.wait()

        await asyncio.wait_for(
            stream_speech(sentences("first", "second", "third"), synth, send, delivered), 1
        )
        self.assertEqual(delivered, ["first", "second", "third"])

    async def test_lookahead_is_bounded_and_cancel_closes_source(self):
        gate = asyncio.Event()
        closed = asyncio.Event()
        produced = []

        async def source():
            try:
                for i in range(100):
                    produced.append(i)
                    yield str(i)
            finally:
                closed.set()

        async def synth(text):
            await gate.wait()
            return text.encode()

        async def send(pcm):
            pass

        task = asyncio.create_task(stream_speech(source(), synth, send, []))
        for _ in range(20):
            await asyncio.sleep(0)
        self.assertLessEqual(len(produced), 4)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await asyncio.wait_for(task, 1)
        self.assertTrue(closed.is_set())

    async def test_transport_failure_closes_generation(self):
        closed = asyncio.Event()
        delivered = []

        async def source():
            try:
                yield "first"
                await asyncio.Event().wait()
            finally:
                closed.set()

        async def synth(text):
            return text.encode()

        async def send(pcm):
            raise ConnectionError("disconnected")

        with self.assertRaises(ConnectionError):
            await asyncio.wait_for(stream_speech(source(), synth, send, delivered), 1)
        self.assertTrue(closed.is_set())
        self.assertEqual(delivered, [])

    async def test_synthesis_failure_keeps_only_delivered_prefix(self):
        first_sent = asyncio.Event()
        delivered = []

        async def synth(text):
            if text == "second":
                await first_sent.wait()
                raise RuntimeError("voice failed")
            return text.encode()

        async def send(pcm):
            first_sent.set()

        with self.assertRaisesRegex(RuntimeError, "voice failed"):
            await asyncio.wait_for(
                stream_speech(sentences("first", "second"), synth, send, delivered), 1
            )
        self.assertEqual(delivered, ["first"])

    async def test_empty_pcm_is_not_recorded_as_spoken(self):
        delivered = []

        async def synth(text):
            return b""

        async def send(pcm):
            self.fail("Empty PCM must not be sent")

        with self.assertRaisesRegex(RuntimeError, "no audio"):
            await stream_speech(sentences("first"), synth, send, delivered)
        self.assertEqual(delivered, [])

    def test_metrics_record_first_occurrence_only(self):
        logs = []
        with patch("speech_pipeline.perf_counter", side_effect=[10, 10.25, 10.5]):
            timing = TurnTiming(logs.append)
            timing.mark("first_pcm_sent")
            timing.mark("first_pcm_sent")
            timing.mark("turn_done")
        self.assertEqual(timing.marks, {"first_pcm_sent": 250, "turn_done": 500})
        self.assertEqual(len(logs), 2)


def load_server_functions():
    # Import only the actual control-flow functions and text filters via AST.
    # Importing server.py itself creates model folders and loads local settings.
    functions = {
        "_iter_checked_reply", "run_turn", "send_pcm_deltas", "_is_canned",
        "_strip_loop_sents", "_content_words", "_too_like_last", "_reply_head",
        "_same_frame", "_too_like_any", "_too_like_user", "_join_reply",
    }
    constants = {"_CANNED_RE", "_LOOP_SENT_RE", "_SIM_STOP"}
    tree = ast.parse((SERVER_DIR / "server.py").read_text(encoding="utf-8"))
    body = [node for node in tree.body if
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in functions
            or isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in constants for target in node.targets)]
    logs = []
    ns = {
        "asyncio": asyncio, "aclosing": aclosing, "base64": base64, "re": re,
        "TurnTiming": TurnTiming, "stream_speech": stream_speech, "PcmPacer": PcmPacer,
        "OUT_RATE": 24000,
        "LLM_MAX_SENTS": 3, "LLM_STORY_SENTS": 5, "_LLM_RETRY_NUDGE": "try again",
        "_is_story": lambda text: False, "_is_greet": lambda text: False,
        "_story_fallback": lambda history: "story fallback",
        "_greet_fallback": lambda history: "greeting fallback",
        "_miss_fallback": lambda: "Не розчув.",
        "bender_prompt": lambda: "character prompt", "log": logs.append,
        "grok_break_chain": lambda reason: None,
        "transcribe": lambda pcm: ("Розкажи щось.", "uk", 1.0, True),
        "synth": lambda text: b"\x01\x00" * 300,
        "voice_commands": SimpleNamespace(match=lambda *args: None),
        "handle_personal_command": lambda *args: None, "favorite_voice_command": lambda text: None,
        "DEVICE_STATIONS": [], "_reply_lang": lambda text, lang: lang,
        "LLM_PROVIDER": "grok", "whisper_device": "fake", "whisper_name": "fake",
        "fold_old_turns": lambda history: None, "save_chat": lambda history: None,
        "dumps": lambda obj: json.dumps(obj, separators=(",", ":")),
    }
    exec(compile(ast.Module(body=body, type_ignores=[]), "server-functions", "exec"), ns)
    return ns, logs


class ServerTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.ns, self.logs = load_server_functions()
        self.timing = TurnTiming(self.logs.append)
        self.history = [{"role": "user", "content": "Розкажи щось."}]

    async def collect(self):
        return [part async for part in self.ns["_iter_checked_reply"](
            "Розкажи щось.", "uk", 1.0, self.history, self.timing)]

    async def test_rejected_opening_is_retried_before_speech(self):
        attempts = []

        async def model(*args):
            attempts.append(args[-1])
            yield "Дякую за перегляд." if len(attempts) == 1 else "Мідний чайник кипить."

        self.ns["iter_llm_sentences"] = model
        self.assertEqual(await self.collect(), ["Мідний чайник кипить."])
        self.assertEqual(len(attempts), 2)
        self.assertIn("try again", attempts[1][-1]["content"])

    async def test_bad_tail_does_not_restart_accepted_speech(self):
        attempts = []
        closed = []

        async def model(*args):
            attempts.append(1)
            try:
                yield "Мідний чайник кипить."
                yield "Дякую за перегляд."
                self.fail("Rejected tail should close its model stream")
            finally:
                closed.append(True)

        self.ns["iter_llm_sentences"] = model
        self.assertEqual(await self.collect(), ["Мідний чайник кипить."])
        self.assertEqual(len(attempts), 1)
        self.assertEqual(closed, [True])

    async def test_repeated_sentence_is_not_voiced_twice(self):
        self.ns["iter_llm_sentences"] = lambda *args: sentences(
            "Мідний чайник кипить.", "Мідний чайник кипить.")
        self.assertEqual(await self.collect(), ["Мідний чайник кипить."])

    async def test_limit_drains_provider_but_does_not_voice_extra_sentences(self):
        self.ns["LLM_MAX_SENTS"] = 1
        drained = []

        async def model(*args):
            yield "Мідний чайник кипить."
            yield "Вікно відчинене."
            drained.append(True)

        self.ns["iter_llm_sentences"] = model
        self.assertEqual(await self.collect(), ["Мідний чайник кипить."])
        self.assertEqual(drained, [True])

    async def test_two_rejected_attempts_produce_single_fallback(self):
        self.ns["iter_llm_sentences"] = lambda *args: sentences("Дякую за перегляд.")
        out = await self.collect()
        self.assertEqual(len(out), 1)
        self.assertIn("Платівка", out[0])

    async def test_run_turn_sends_first_pcm_before_model_finishes(self):
        first_pcm = asyncio.Event()
        wire = []
        voiced = []
        history = []

        async def model(*args):
            yield "Мідний чайник кипить."
            await first_pcm.wait()
            yield "Вікно відчинене."

        def synth(text):
            voiced.append(text)
            return b"\x01\x00" * 300

        async def send(raw):
            msg = json.loads(raw)
            wire.append(msg)
            if msg["type"] == "response.output_audio.delta":
                first_pcm.set()

        self.ns["iter_llm_sentences"] = model
        self.ns["synth"] = synth
        await asyncio.wait_for(self.ns["run_turn"](SimpleNamespace(send=send), b"input", "", history), 2)
        self.assertEqual(voiced, ["Мідний чайник кипить.", "Вікно відчинене."])
        self.assertEqual([m["type"] for m in wire], ["response.created",
                         "response.output_audio.delta", "response.output_audio.delta",
                         "response.output_audio.done", "response.done"])
        self.assertEqual(history[-1]["content"], "Мідний чайник кипить. Вікно відчинене.")
        self.assertTrue(any("first_pcm_sent=" in line for line in self.logs))
        self.assertTrue(any("stt_done=" in line for line in self.logs))

    async def test_voice_command_still_follows_its_confirmation_audio(self):
        wire = []
        self.ns["voice_commands"] = SimpleNamespace(match=lambda *args: SimpleNamespace(
            args={"station": 1}, name="radio.station", reply=lambda n: "Вмикаю."))

        async def send(raw):
            wire.append(json.loads(raw)["type"])

        history = []
        await self.ns["run_turn"](SimpleNamespace(send=send), b"input", "", history)
        self.assertEqual(wire, ["response.created", "response.output_audio.delta",
                               "device.command", "response.output_audio.done", "response.done"])
        self.assertEqual(history[-1]["content"], "Вмикаю.")

    async def test_asr_miss_does_not_pollute_history(self):
        self.ns["transcribe"] = lambda pcm: ("", "uk", 0, False)
        wire = []

        async def send(raw):
            wire.append(json.loads(raw)["type"])

        history = []
        await self.ns["run_turn"](SimpleNamespace(send=send), b"input", "", history)
        self.assertEqual(history, [])
        self.assertEqual(wire[-1], "response.done")

    async def test_uncertain_nonempty_asr_only_asks_to_repeat(self):
        self.ns["transcribe"] = lambda pcm: ("А то я буду взять рапунт на верхнюю школу", "ru", 1, False)
        def forbidden(*args):
            self.fail("Uncertain speech must not reach memory, commands or the LLM")
        for name in ("handle_personal_command", "favorite_voice_command", "iter_llm_sentences"):
            self.ns[name] = forbidden
        voiced, wire = [], []
        def synth(text):
            voiced.append(text)
            return b"\x01\x00" * 300
        async def send(raw):
            wire.append(json.loads(raw)["type"])
        self.ns["synth"] = synth
        history = [{"role": "user", "content": "Привет"}]
        await self.ns["run_turn"](SimpleNamespace(send=send), b"input", "", history)
        self.assertEqual(voiced, ["Не розчув."])
        self.assertEqual(history, [{"role": "user", "content": "Привет"}])
        self.assertEqual(wire[-1], "response.done")

    async def test_minute_of_pcm_across_sentences_keeps_bounded_lead_and_byte_order(self):
        now = 0.0
        received = bytearray()

        async def sleep(delay):
            nonlocal now
            now += delay

        pacer = PcmPacer(clock=lambda: now, sleep=sleep)

        async def send(raw):
            received.extend(base64.b64decode(json.loads(raw)["delta"]))
            self.assertLessEqual(len(received) / 48000 - now, 0.500001)

        # Three 20-second sentences; the media clock must not restart per phrase.
        pcm = bytes(range(256)) * 3750
        for _ in range(3):
            await self.ns["send_pcm_deltas"](SimpleNamespace(send=send), pcm, pacer=pacer)
        self.assertEqual(received, pcm * 3)
        self.assertAlmostEqual(now, 59.5, places=6)

    async def test_malformed_pcm_is_rejected_before_transport(self):
        async def send(raw):
            self.fail("Malformed PCM should not be sent")

        with self.assertRaises(ValueError):
            await self.ns["send_pcm_deltas"](SimpleNamespace(send=send), b"\x01")


class PacerTests(unittest.IsolatedAsyncioTestCase):
    async def test_initial_audio_has_no_added_wait(self):
        waits = []

        async def sleep(delay):
            waits.append(delay)

        pacer = PcmPacer(clock=lambda: 100.0, sleep=sleep)
        for _ in range(4):
            await pacer.wait(4800)
        self.assertEqual(waits, [])

    async def test_synthesis_gap_does_not_accumulate_catchup_burst(self):
        now = 0.0

        async def sleep(delay):
            nonlocal now
            now += delay

        pacer = PcmPacer(clock=lambda: now, sleep=sleep)
        for _ in range(30):
            await pacer.wait(4800)
        now += 10
        before = now
        for _ in range(10):
            await pacer.wait(4800)
        self.assertAlmostEqual(now - before, 0.5)

    async def test_partial_sample_is_rejected(self):
        with self.assertRaises(ValueError):
            await PcmPacer().wait(3)


if __name__ == "__main__":
    unittest.main()
