"""Offline Piper/RVC consonant comparison using installed models only."""
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "BendeRadio/examples/BenderRealtime/local_server"
sys.path.insert(0, str(SERVER))

import server as s
from preview_delivery import stop_worker, write_wav
from ukrainian_word_stress import Disambiguation, Stressifier, StressSymbol


def main():
    out = ROOT / "artifacts/consonants-kartynku-20261001"
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise RuntimeError("Use an empty output directory to preserve earlier comparisons")
    if not s.VOICE_ONNX.is_file():
        raise RuntimeError("Installed Piper model required")
    ready, reason = s.rvc_convert.ready()
    if not ready:
        raise RuntimeError(reason)
    s.load_stress_words()
    s._uk_stress = Stressifier(stress_symbol=StressSymbol.CombiningAcuteAccent,
                              on_ambiguity="skip", disambiguation=Disambiguation.Dictionary)
    text = "Картинку. Подивись на цю картинку. Покажи картинку, а потім хатинку."
    prepared = s.piper_ready_uk(text)
    print("Prepared:", prepared, flush=True)
    syn = s.SynthesisConfig(speaker_id=s.PIPER_SPEAKER, length_scale=s.PIPER_LENGTH,
                           noise_scale=0.62, noise_w_scale=0.80)
    voice = s.PiperVoice.load(s.VOICE_ONNX)
    pcm, rate = s._piper_pcm(voice, syn, prepared)
    raw = s.resample_int16(pcm, rate, s.OUT_RATE).tobytes()
    write_wav(out / "01_Piper.wav", raw, s.OUT_RATE)
    outputs = []
    try:
        for label, protect in (("02_Bender_before", "0.5"), ("03_Bender_protected", "0.33")):
            print("Render", label, "protect", protect, flush=True)
            os.environ["RVC_PROTECT"] = protect
            converted = s.rvc_convert.convert_pcm(raw, s.OUT_RATE)
            write_wav(out / (label + ".wav"), converted, s.OUT_RATE)
            outputs.append(converted)
            stop_worker(s.rvc_convert._proc)
            s.rvc_convert._proc = None
        gap = bytes(s.OUT_RATE * 2)
        write_wav(out / "Before_then_after.wav", gap.join(outputs), s.OUT_RATE)
        (out / "manifest.json").write_text(json.dumps({
            "text": text, "prepared": prepared, "sample_rate": s.OUT_RATE,
            "length_scale": s.PIPER_LENGTH, "protect": [0.5, 0.33],
            "note": "Same Piper PCM used for both RVC renders; no live server changes. "
                    "RVC renders can vary. Requires listening, not an ASR quality verdict.",
        }, ensure_ascii=False, indent=2), encoding="utf-8")
        print("Saved", out, flush=True)
    finally:
        stop_worker(s.rvc_convert._proc)
        s.rvc_convert._proc = None


if __name__ == "__main__":
    main()
