"""Three real ASR/Grok turns, then replay identical Grok deltas with faster RVC.

Audio is collected to WAVs, never played. Saved chat and personal memory are
not changed. Inputs are synthetic Ukrainian speech, not a microphone test.
"""
import asyncio
import base64
from contextlib import aclosing
import json
import os
from pathlib import Path
import re
import ssl
import sys
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "BendeRadio/examples/BenderRealtime/local_server"
OUT = ROOT / "artifacts/voice-latency-20261011/dialogue"
QUESTIONS = ["Бендер, чому небо синє?", "Бендер, що ти робив би у вихідний?",
             "Бендер, розкажи короткий жарт про програмістів."]


def stop_worker(module):
    proc = module._proc
    if proc is not None:
        try:
            proc.communicate('{"cmd":"quit"}\n', timeout=5)
        except Exception:
            proc.kill()
            proc.wait(timeout=5)
        module._proc = None


async def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for k, v in {"RVC_ENABLE": "1", "RVC_PITCH": "-3", "RVC_INDEX_RATE": "0.55",
                 "RVC_RMS": "0.75", "RVC_PROTECT": "0.33",
                 "RVC_PTH": "voice_clone/models/bender.pth",
                 "RVC_INDEX": "voice_clone/models/bender.index",
                 "STANZA_RESOURCES_DIR": "A:/stanza_resources"}.items():
        os.environ.setdefault(k, v)
    sys.path.insert(0, str(HERE))
    import server
    from holos_tts import HolosVoice
    from personal_memory import PersonalMemory
    import numpy as np
    # Use Windows/system trusted roots in this diagnostic environment. Keep
    # certificate verification enabled; certifi alone lacks the local CA.
    server._SSL_VERIFY = ssl.create_default_context()
    # Synthetic benchmark only: do not send the user's memory, prompt, radio
    # stations, prior turns or stored conversation IDs to the external model.
    server.MEMORY = PersonalMemory(OUT / ("empty-" + str(uuid.uuid4()) + ".json"))
    server.DEVICE_STATIONS = []
    server.DEVICE_CURRENT_STATION = None
    server.DEVICE_CHARACTER = None
    server.bender_prompt = lambda *a, **kw: (
        "Ти вигаданий саркастичний робот. Відповідай українською двома короткими "
        "реченнями по суті запитання, без вступних підтверджень. Це синтетичний тест швидкості."
    )
    all_rows, current_logs = [], []
    def log(message):
        current_logs.append(str(message))
    server.log = log
    server.save_chat = lambda history: None
    server.CHAT.clear()
    server.CHAT_SUMMARY = ""
    server.CHAT_CONV_ID = "latency-benchmark-" + str(uuid.uuid4())
    original_grok = server._grok_pieces
    traces = {}
    try:
        server.load_whisper()
        if server.whisper_device != "cuda":
            raise RuntimeError("CUDA unavailable; refusing an unrepresentative CPU benchmark")
        server.load_stress_words()
        server.load_uk_stress()
        server.load_holos()
        # A different speaker gives ASR a source independent of Bender's RVC.
        input_voice = HolosVoice(speaker="Speaker_84", delivery="original", speed=1.0)
        inputs = []
        for index, text in enumerate(QUESTIONS):
            pcm = input_voice.synthesize(server.piper_ready_uk(text))
            pcm = np.concatenate([np.zeros(4800, dtype=np.int16), pcm, np.zeros(9600, dtype=np.int16)]).tobytes()
            inputs.append(pcm)
            with wave.open(str(OUT / f"input_{index}.wav"), "wb") as wav:
                wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000); wav.writeframes(pcm)
        for mode in ("baseline", "optimized_replay"):
            os.environ["RVC_CACHE_PITCH"] = "0" if mode == "baseline" else "1"
            stop_worker(server.rvc_convert)
            await asyncio.to_thread(server.rvc_convert.warmup)
            for index, pcm in enumerate(inputs):
                current_logs.clear()
                server.CHAT_GROK_RESP_ID = ""
                history, audio, events = [], bytearray(), []
                trace = traces.setdefault(index, [])
                if mode == "baseline":
                    async def capture(*a, **kw):
                        began = time.perf_counter()
                        async with aclosing(original_grok(*a, **kw)) as source:
                            async for piece in source:
                                trace.append({"seconds": time.perf_counter()-began, "text": piece})
                                yield piece
                    server._grok_pieces = capture
                else:
                    async def replay(*a, **kw):
                        began = time.perf_counter()
                        for item in trace:
                            await asyncio.sleep(max(0, item["seconds"]-(time.perf_counter()-began)))
                            yield item["text"]
                    server._grok_pieces = replay
                class Sink:
                    async def send(self, raw):
                        event = json.loads(raw)
                        kind = event["type"]
                        if kind == "response.output_audio.delta":
                            audio.extend(base64.b64decode(event["delta"]))
                        else:
                            events.append(event)
                await asyncio.wait_for(server.run_turn(Sink(), pcm, server.bender_prompt(), history, wake_invocation=True), 90)
                if not audio or not history or not trace:
                    (OUT / f"{mode}_{index}.log").write_text("\n".join(current_logs), encoding="utf-8")
                    raise RuntimeError(f"No real reply for {mode}/{index}; inspect its log")
                if any("fail:" in line or "stream error" in line for line in current_logs):
                    raise RuntimeError("A fallback reply is not a valid timing sample")
                marks = {}
                for line in current_logs:
                    match = re.search(r"\[Latency\] (\w+)=(\d+)ms", line)
                    if match:
                        marks[match[1]] = int(match[2])
                row = {"mode": mode, "question": index, "input_text": QUESTIONS[index],
                       "history": history, "marks_ms": marks,
                       "audio_seconds": len(audio)/48000, "events": events}
                all_rows.append(row)
                print(json.dumps(row, ensure_ascii=True), flush=True)
                with wave.open(str(OUT / f"{mode}_{index}.wav"), "wb") as wav:
                    wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000); wav.writeframes(audio)
                (OUT / f"{mode}_{index}.log").write_text("\n".join(current_logs), encoding="utf-8")
                (OUT / "results.json").write_text(json.dumps(all_rows, ensure_ascii=False, indent=2), encoding="utf-8")
                (OUT / "grok_deltas.json").write_text(json.dumps(traces, ensure_ascii=False, indent=2), encoding="utf-8")
    finally:
        stop_worker(server.rvc_convert)
        stop_worker(server.stress_convert)


if __name__ == "__main__":
    asyncio.run(main())
