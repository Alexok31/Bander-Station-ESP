#!/usr/bin/env python3
"""
Готує записи пародії під навчання RVC (Applio).

Клади сирі файли в voice_clone/raw/ (wav/mp3/m4a/flac/ogg).
Запуск з папки local_server:

  python voice_clone/prepare_samples.py

Результат: voice_clone/dataset/*.wav — mono, 48 kHz, нормалізовані.
Потім у Applio: Train → dataset_path = .../voice_clone/dataset
"""

from __future__ import annotations

import subprocess
import sys
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
OUT = HERE / "dataset"
TARGET_SR = 48000
EXTS = {".wav", ".mp3", ".m4a", ".flac", ".ogg", ".webm", ".aac"}


def have_ffmpeg() -> bool:
    try:
        subprocess.run(
            ["ffmpeg", "-version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return True
    except FileNotFoundError:
        return False


def convert_ffmpeg(src: Path, dst: Path) -> None:
    # loudnorm м'який; Applio ще раз поріже на сегменти.
    cmd = [
        "ffmpeg",
        "-y",
        "-i",
        str(src),
        "-ac",
        "1",
        "-ar",
        str(TARGET_SR),
        "-c:a",
        "pcm_s16le",
        "-af",
        "highpass=f=80,loudnorm=I=-18:TP=-1.5:LRA=16",
        str(dst),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-800:] if r.stderr else "ffmpeg fail")


def convert_wave_only(src: Path, dst: Path) -> None:
    """Без ffmpeg — лише вже готові WAV."""
    if src.suffix.lower() != ".wav":
        raise RuntimeError(f"без ffmpeg потрібен WAV: {src.name}")
    with wave.open(str(src), "rb") as w:
        ch, sw, sr, n, _, _ = w.getparams()
        raw = w.readframes(n)
    if sw != 2:
        raise RuntimeError(f"потрібен 16-bit PCM: {src.name}")
    # Простий даунмікс / ресемпл через numpy, якщо є.
    try:
        import numpy as np
    except ImportError as e:
        raise RuntimeError("постав numpy або ffmpeg") from e

    x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    if sr != TARGET_SR and x.size:
        n_dst = int(round(x.size * TARGET_SR / sr))
        t_old = np.linspace(0.0, 1.0, x.size, endpoint=False)
        t_new = np.linspace(0.0, 1.0, n_dst, endpoint=False)
        x = np.interp(t_new, t_old, x)
    peak = float(np.max(np.abs(x))) if x.size else 1.0
    if peak > 1:
        x = x * (28000.0 / peak)
    y = np.clip(x, -32767, 32767).astype(np.int16)
    dst.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(dst), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(TARGET_SR)
        out.writeframes(y.tobytes())


def main() -> int:
    RAW.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in RAW.iterdir() if p.suffix.lower() in EXTS and p.is_file())
    if not files:
        print(f"Порожньо: поклади записи в {RAW}")
        print(f"Текст для читання: {HERE / 'phrases_uk.txt'}")
        return 1

    use_ff = have_ffmpeg()
    if not use_ff:
        print("ffmpeg не знайдено — конвертую лише WAV. Краще постав ffmpeg.")

    ok = 0
    for i, src in enumerate(files, 1):
        dst = OUT / f"bender_{i:03d}.wav"
        print(f"[{i}/{len(files)}] {src.name} -> {dst.name}")
        try:
            if use_ff:
                convert_ffmpeg(src, dst)
            else:
                convert_wave_only(src, dst)
            ok += 1
        except Exception as e:
            print(f"  FAIL: {e}")

    print(f"OK: {ok}/{len(files)} -> {OUT}")
    print("Dalee: Applio Train, model_name=Bender, dataset_path=voice_clone/dataset")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
