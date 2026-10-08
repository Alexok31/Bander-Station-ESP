"""Make two identical Ukrainian phrases with all available Piper voices.

Run from this directory: python compare_uk_tts.py
The samples are raw Piper output so pronunciation can be judged before RVC.
"""

from pathlib import Path
import wave

import numpy as np
from piper import PiperVoice, SynthesisConfig

import server


HERE = Path(__file__).resolve().parent
MODELS = HERE / "models"
OUT = HERE.parents[3] / "artifacts" / "tts_comparison"
TEXT = (
    "Зараз я покажу картинку. Повтори, будь ласка, останнє речення. "
    "Слухай уважно: я не хочу ковтати букви."
)
STRESS_TEXT = (
    "Він визнає́ помилку. Дай мені по шматку́ пирога. "
    "З такою важко́ю коробкою я далеко не піду."
)
VARIANTS = (
    ("current_fast", "uk_UA-ukrainian_tts-medium.onnx", 0.70, 1),
    ("current_balanced", "uk_UA-ukrainian_tts-medium.onnx", 0.90, 1),
    ("current_normal", "uk_UA-ukrainian_tts-medium.onnx", 1.00, 1),
    ("mykyta_high", "uk_UA-mykyta-high.onnx", 0.90, None),
    ("oleksa_high", "uk_UA-oleksa-high.onnx", 0.90, None),
    ("tetiana_high", "uk_UA-tetiana-high.onnx", 0.90, None),
)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "text.txt").write_text(TEXT + "\n", encoding="utf-8")
    (OUT / "text_stress.txt").write_text(STRESS_TEXT + "\n", encoding="utf-8")
    server.load_stress_words()
    server.load_uk_stress()
    loaded = {}
    for test_name, text in (("", TEXT), ("_stress", STRESS_TEXT)):
        spoken_text = server.piper_ready_uk(text)
        (OUT / f"prepared_text{test_name}.txt").write_text(
            spoken_text + "\n", encoding="utf-8"
        )
        for label, filename, length, speaker_id in VARIANTS:
            if test_name and label in ("current_fast", "current_normal"):
                continue
            model = MODELS / filename
            if not model.is_file():
                print(f"{label}: missing {model}")
                continue
            if filename not in loaded:
                loaded[filename] = PiperVoice.load(str(model))
            voice = loaded[filename]
            config = SynthesisConfig(
                speaker_id=speaker_id,
                length_scale=length,
                noise_scale=0.62,
                noise_w_scale=0.80,
            )
            chunks = list(voice.synthesize(spoken_text, config))
            if not chunks:
                raise RuntimeError(f"No audio for {label}{test_name}")
            audio = np.concatenate(
                [np.asarray(chunk.audio_int16_array, dtype=np.int16) for chunk in chunks]
            )
            path = OUT / f"{label}{test_name}.wav"
            with wave.open(str(path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(chunks[0].sample_rate)
                wav.writeframes(audio.tobytes())
            print(f"{label}{test_name}: {len(audio) / chunks[0].sample_rate:.2f}s -> {path}")


if __name__ == "__main__":
    main()
