#!/usr/bin/env python3
"""Render an offline A/B audition; never changes server settings or starts a listener.

Uses installed Piper/RVC models, no LLM, microphone, downloads or paid requests.
The experimental continuation gap assumes already prepared, contiguous audio.
It must not be used blindly for a live stream after a synthesis/network stall.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import json
from pathlib import Path
import re
import subprocess
import time
import wave


CASES = (
    ("surprise", "Удивлённый вопрос", ("Ти серйозно?",)),
    ("question", "Обычный вопрос", ("Що цього разу сталося?",)),
    ("reaction", "Короткая реакция", ("О, оце вже діло!",)),
    ("conversation", "Два связанных предложения", (
        "Гаразд, допоможу.",
        "Тільки не кажи Фраю, що я сьогодні добрий.",
    )),
)


@dataclass(frozen=True)
class Delivery:
    length_multiplier: float = 1.0
    question_rise: float = 0.0
    continuation_lead_ms: int = 220


def delivery_for(text: str, variant: str) -> Delivery:
    """Conservative punctuation/word cues, not an emotion recognition model."""
    plain = text.lower().replace("\u0301", "").rstrip(" \"'«»)]")
    words = re.findall(r"[^\W\d_]+", plain, re.UNICODE)
    question = bool(re.search(r"\?[!?]*$", plain))
    if variant == "A":
        return Delivery(question_rise=4.2 if question else 0.0)
    if variant != "B":
        raise ValueError("Variant must be A or B")
    wh = {"хто", "що", "де", "коли", "чому", "навіщо", "як", "який", "яка",
          "кто", "что", "где", "когда", "почему", "зачем", "как", "какой", "какая"}
    rise = (0.6 if words and words[0] in wh else 1.8) if question else 0.0
    speed = 0.96 if plain.endswith("!") and not question and len(words) <= 7 else 1.0
    if len(words) >= 8 and not question:
        speed = 1.02
    return Delivery(speed, rise, 100)


def shorten_continuation(pcm: bytes, sample_rate: int, lead_ms: int) -> bytes:
    """Remove only verified zero padding inserted by server._trim_pcm_silence."""
    cut = max(0, round(sample_rate * (220 - lead_ms) / 1000)) * 2
    if cut and len(pcm) > cut and not any(pcm[:cut]):
        return pcm[cut:]
    return pcm


def write_wav(path: Path, pcm: bytes, sample_rate: int) -> None:
    if not pcm or len(pcm) % 2:
        raise ValueError(f"Invalid PCM16: {path.name}")
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)


def stop_worker(proc) -> None:
    # Only workers owned by this audition process, never the running server.
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.communicate('{"cmd":"quit"}\n', timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    outdir = args.output.resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    if any(outdir.iterdir()):
        parser.error("Choose an empty output directory to preserve earlier auditions")

    # The main server is imported for the exact current text/DSP functions.
    # Its main() is not called: no port binding, chat updates or model downloads.
    import numpy as np
    import onnxruntime as ort
    import server as s
    from ukrainian_word_stress import Disambiguation, Stressifier, StressSymbol

    if not s.VOICE_ONNX.is_file() or not s.VOICE_JSON.is_file():
        raise RuntimeError("Local Piper model missing; audition does not download models")
    ok, reason = s.rvc_convert.ready()
    if not ok:
        raise RuntimeError("RVC required for an honest Bender comparison: " + reason)
    s.load_stress_words()
    # Lightweight installed dictionary, identical prepared text for A and B.
    # Does not start Stanza or download its resources for an audition.
    s._uk_stress = Stressifier(
        stress_symbol=StressSymbol.CombiningAcuteAccent,
        on_ambiguity="skip", disambiguation=Disambiguation.Dictionary,
    )
    syn = s.SynthesisConfig(speaker_id=s.PIPER_SPEAKER, length_scale=s.PIPER_LENGTH,
                           noise_scale=0.62, noise_w_scale=0.80)
    sr = s.OUT_RATE
    manifest = {
        "sample_rate": sr, "speaker_id": s.PIPER_SPEAKER,
        "base_length_scale": s.PIPER_LENGTH, "stress": "installed dictionary", "piper_seed": 20261001,
        "note": "Offline contiguous playback; no network/STT/LLM latency is simulated. "
                "Piper uses the same seed and a fresh session for every render; "
                "RVC can still vary between renders. Identical prepared text "
                "and tempo share one cached render between A and B. Timings include "
                "cold start when applicable, not a server benchmark.",
        "cases": [],
    }
    cache = {}
    aggregate = {"A": [], "B": [], "Piper_A": [], "Piper_B": []}
    silence = bytes(sr * 2)  # 1 s separator between cases, not part of speech.
    try:
        for number, (key, title, sentences) in enumerate(CASES, 1):
            prepared = [s.piper_ready_uk(text) for text in sentences]
            item = {"title": title, "text": " ".join(sentences), "prepared": prepared,
                    "variants": {}}
            for variant in ("A", "B"):
                rendered, piper_parts, settings = [], [], []
                for index, (text, ready_text) in enumerate(zip(sentences, prepared)):
                    profile = delivery_for(text, variant)
                    length = syn.length_scale * profile.length_multiplier
                    cache_key = (ready_text, length)
                    if cache_key not in cache:
                        print(f"Render {number} {variant} sentence {index + 1}", flush=True)
                        start = time.perf_counter()
                        # Reset graph RNGs by creating a fresh CPU session with
                        # the same seed; otherwise random durations can outweigh
                        # the deliberately small 2–4% tempo change in this test.
                        ort.set_seed(20261001)
                        voice = s.PiperVoice.load(s.VOICE_ONNX)
                        piper, piper_sr = s._piper_pcm(voice, replace(syn, length_scale=length), ready_text)
                        del voice
                        raw = s.resample_int16(piper, piper_sr, sr).tobytes()
                        piper_ms = (time.perf_counter() - start) * 1000
                        rvc_start = time.perf_counter()
                        converted = s.rvc_convert.convert_pcm(raw, sr)
                        # No fallback to Piper: both final variants must use RVC.
                        cache[cache_key] = (raw, converted, piper_ms,
                                           (time.perf_counter() - rvc_start) * 1000)
                    raw, converted, piper_ms, rvc_ms = cache[cache_key]
                    base = f"{number:02d}_{key}_{variant}_{index + 1}"
                    write_wav(outdir / f"{base}_piper_raw.wav", raw, sr)
                    write_wav(outdir / f"{base}_rvc_raw.wav", converted, sr)

                    def finish(pcm):
                        if profile.question_rise:
                            pcm = s._question_intonation(np.frombuffer(pcm, dtype=np.int16),
                                                         sr, profile.question_rise).tobytes()
                        pcm = s._trim_pcm_silence(pcm, sr)
                        if index and variant == "B":
                            pcm = shorten_continuation(pcm, sr, profile.continuation_lead_ms)
                        return pcm

                    rendered.append(finish(converted))
                    piper_parts.append(finish(raw))
                    settings.append({"length_scale": length, "rise_semitones": profile.question_rise,
                                     "piper_ms": round(piper_ms, 1), "rvc_ms": round(rvc_ms, 1)})
                pcm = b"".join(rendered)
                file_name = f"{number:02d}_{key}_{variant}.wav"
                write_wav(outdir / file_name, pcm, sr)
                item["variants"][variant] = {"file": file_name, "settings": settings,
                                              "seconds": round(len(pcm) / (sr * 2), 2)}
                aggregate[variant].extend([pcm, silence])
                aggregate["Piper_" + variant].extend([b"".join(piper_parts), silence])
            manifest["cases"].append(item)
        for name, parts in aggregate.items():
            write_wav(outdir / f"{name}_all.wav", b"".join(parts[:-1]), sr)
        # Alternating comparisons: A then B for each sentence/case.
        paired = []
        for item in manifest["cases"]:
            for variant in ("A", "B"):
                with wave.open(str(outdir / item["variants"][variant]["file"]), "rb") as wav:
                    paired.extend([wav.readframes(wav.getnframes()), silence])
        write_wav(outdir / "AB_pairs.wav", b"".join(paired[:-1]), sr)
        (outdir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        (outdir / "README.txt").write_text(
            "Сравнение голоса Бендера\n\n"
            "AB_pairs.wav: в каждой паре сначала A (текущий), затем B (эксперимент).\n"
            "A_all.wav / B_all.wav: все четыре ситуации подряд.\n"
            "Piper_A_all.wav / Piper_B_all.wav: те же варианты без RVC, для диагностики подачи.\n\n"
            + "\n".join(f"{i}. {title}: {' '.join(sentences)}" for i, (_, title, sentences) in enumerate(CASES, 1))
            + "\n\nB: подъём вопросов 1,8 или 0,6 вместо 4,2 полутона; короткая реакция\n"
              "с параметром длительности на 4% меньше, длинная реплика — на 2% больше.\n"
              "Перед вторым заранее подготовленным предложением тишина сокращена на 120 мс.\n"
              "В реальном стриме при задержке синтеза пауза может быть длиннее.\n"
              "Рабочий сервер и его настройки не изменены. Для A/B использован один\n"
              "подготовленный текст со словарными ударениями, без Stanza. Результат\n"
              "синтеза с одинаковым темпом общий; изменение темпа требует нового синтеза.\n"
              "Для Piper при каждом синтезе используется одинаковый seed.\n",
            encoding="utf-8",
        )
        print(f"Audition saved to {outdir}", flush=True)
    finally:
        stop_worker(s.rvc_convert._proc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
