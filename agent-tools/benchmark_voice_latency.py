"""Offline, repeatable voice benchmark. Never plays audio or changes chat history.

Uses the same local voice settings as start.bat. Network LLM calls are not made.
The first pass is cold; subsequent passes measure warmed models.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time
import wave

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "BendeRadio/examples/BenderRealtime/local_server"
PHRASES = [
    "Повтори, будь ласка, останнє слово.",
    "Мої справи чудові, якщо не рахувати катастрофічної нестачі пива.",
    "Ти питаєш про картинку? Бачу шматку тканини під важкою залізною коробкою.",
    "Я б допоміг тобі з прибиранням, але мої блискучі металеві руки створені для важливіших справ, наприклад тримати холодне пиво.",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for key, value in {"RVC_ENABLE": "1", "RVC_PITCH": "-3", "RVC_INDEX_RATE": "0.55",
                       "RVC_RMS": "0.75", "RVC_PROTECT": "0.33",
                       "RVC_PTH": "voice_clone/models/bender.pth",
                       "RVC_INDEX": "voice_clone/models/bender.index",
                       "STANZA_RESOURCES_DIR": "A:/stanza_resources"}.items():
        os.environ.setdefault(key, value)
    sys.path.insert(0, str(SERVER))
    import server
    import holos_tts
    import numpy as np

    rows = []
    stages = {}
    normalized = []
    log_file = (args.output / "server.log").open("w", encoding="utf-8")
    def log(message):
        log_file.write(str(message) + "\n")
        log_file.flush()
    server.log = log

    def measured(owner, name, label):
        original = getattr(owner, name)
        def call(*a, **kw):
            started = time.perf_counter()
            try:
                value = original(*a, **kw)
                if label == "prepare":
                    normalized.append(value)
                return value
            finally:
                stages[label] = stages.get(label, 0) + (time.perf_counter() - started) * 1000
        setattr(owner, name, call)

    try:
        server.load_stress_words()
        server.load_uk_stress()
        server.load_holos()
        if server.holos_voice is None:
            raise RuntimeError("Holos unavailable; benchmark must not silently fall back")
        measured(server, "piper_ready_uk", "prepare")
        measured(server, "apply_uk_stress", "stress")
        measured(holos_tts, "ipa", "phonemes")
        measured(server.holos_voice, "synthesize", "holos")
        measured(server.rvc_convert, "convert_pcm", "rvc")
        for repeat in range(args.rounds):
            for index, text in enumerate(PHRASES):
                stages.clear()
                normalized.clear()
                started = time.perf_counter()
                pcm = server.synth(text)
                elapsed = (time.perf_counter() - started) * 1000
                samples = np.frombuffer(pcm, dtype=np.int16)
                if not samples.size or np.max(np.abs(samples.astype(np.int32))) == 0:
                    raise RuntimeError("Voice returned silence")
                wav_path = args.output / f"phrase_{index}_round_{repeat}.wav"
                with wave.open(str(wav_path), "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(server.OUT_RATE)
                    wav.writeframes(pcm)
                row = {"round": repeat, "phrase": index, "text": text,
                       "normalized": list(normalized), "total_ms": elapsed,
                       "stages_ms": dict(stages), "audio_seconds": samples.size / server.OUT_RATE,
                       "peak": int(np.max(np.abs(samples.astype(np.int32)))),
                       "clipped_fraction": float(np.mean(np.abs(samples.astype(np.int32)) >= 32767)),
                       "sha256": hashlib.sha256(pcm).hexdigest(), "wav": str(wav_path)}
                rows.append(row)
                print(json.dumps({"round": repeat, "phrase": index, "total_ms": round(elapsed),
                                  "stages_ms": {k: round(v) for k, v in stages.items()}}, ensure_ascii=True), flush=True)
                (args.output / "results.json").write_text(json.dumps({
                    "config": {"tts": server.TTS_BACKEND, "speaker": server.HOLOS_SPEAKER,
                               "delivery": server.HOLOS_DELIVERY, "speed": server.HOLOS_SPEED,
                               "rvc_pitch": os.environ["RVC_PITCH"]}, "rows": rows,
                }, ensure_ascii=False, indent=2), encoding="utf-8")
        warm = [row for row in rows if row["round"] > 0]
        if warm:
            print("WARM_MEDIAN_MS", round(statistics.median(row["total_ms"] for row in warm)), flush=True)
    finally:
        # Only processes created by this benchmark; never touch the live server.
        for module in (server.rvc_convert, server.stress_convert):
            proc = module._proc
            if proc is not None:
                try:
                    proc.communicate('{"cmd":"quit"}\n', timeout=5)
                except Exception:
                    proc.kill()
                    proc.wait(timeout=5)
        log_file.close()


if __name__ == "__main__":
    main()
