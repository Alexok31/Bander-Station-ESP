"""Local HolosTTS CPU/ONNX synthesis using the author's Ukrainian phonemizer."""

import json
from pathlib import Path
from time import perf_counter

import numpy as np
import onnxruntime as ort

from vendor.ipa_uk import ipa


MODELS = Path(__file__).resolve().parent / "models" / "holos"
SAMPLE_RATE = 24000


class HolosVoice:
    def __init__(self, speaker: str = "Speaker_84", speed: float = 1.0):
        if not 0.5 <= speed <= 1.5:
            raise ValueError("HolosTTS speed must be between 0.5 and 1.5")
        model = MODELS / "holos_cpu_int8.onnx"
        vocab_path = MODELS / "vocab.json"
        voices_path = MODELS / "voices.npz"
        for path in (model, vocab_path, voices_path):
            if not path.is_file():
                raise FileNotFoundError(f"HolosTTS asset missing: {path}")

        vocab = json.loads(vocab_path.read_text(encoding="utf-8"))
        self.vocab = {symbol: i for i, symbol in enumerate(vocab)}
        with np.load(voices_path, allow_pickle=False) as voices:
            if speaker not in voices:
                raise ValueError(f"HolosTTS speaker unavailable: {speaker}")
            self.voice = voices[speaker].reshape(1, 256).astype(np.float32)
        self.speaker = speaker
        self.speed = speed

        options = ort.SessionOptions()
        options.intra_op_num_threads = 4
        self.session = ort.InferenceSession(
            str(model), sess_options=options, providers=["CPUExecutionProvider"]
        )

    def synthesize(self, text: str) -> np.ndarray:
        phonemes = ipa(text)
        missing = set(phonemes) - self.vocab.keys()
        if missing:
            raise ValueError(f"HolosTTS unknown phonemes: {''.join(sorted(missing))!r}")
        tokens = np.array(
            [[0, *(self.vocab[phoneme] for phoneme in phonemes), 0]], dtype=np.int64
        )
        audio, lengths = self.session.run(
            ["audio", "audio_lengths"],
            {
                "tokens": tokens,
                "voice": self.voice,
                "speed": np.array([self.speed], dtype=np.float32),
            },
        )
        signal = np.asarray(audio, dtype=np.float32).reshape(-1)[: int(lengths[0])]
        return np.clip(np.rint(signal * 32767.0), -32768, 32767).astype(np.int16)


def benchmark(text: str, speaker: str = "Speaker_84") -> tuple[np.ndarray, float]:
    voice = HolosVoice(speaker=speaker)
    started = perf_counter()
    pcm = voice.synthesize(text)
    return pcm, perf_counter() - started
