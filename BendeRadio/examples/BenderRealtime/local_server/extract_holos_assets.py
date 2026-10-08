"""Extract small, torch-free runtime assets from official HolosTTS .pt files.

Run once with a Python environment that has torch and numpy installed. The
generated files allow the voice server to use the CPU ONNX model without torch.
"""

import json
from pathlib import Path

import numpy as np
import torch


MODELS = Path(__file__).resolve().parent / "models" / "holos"


def main() -> None:
    checkpoint = torch.load(MODELS / "holos.pt", map_location="cpu", weights_only=True)
    vocab = checkpoint["vocab"]
    if not isinstance(vocab, (str, list, tuple)) or not vocab:
        raise ValueError("HolosTTS vocabulary missing")
    (MODELS / "vocab.json").write_text(
        json.dumps(vocab, ensure_ascii=False), encoding="utf-8"
    )

    voices = torch.load(MODELS / "voices.pt", map_location="cpu", weights_only=True)
    arrays = {name: value.detach().float().numpy() for name, value in voices.items()}
    if not arrays or any(value.shape != (256,) for value in arrays.values()):
        raise ValueError("HolosTTS voice vectors invalid")
    np.savez_compressed(MODELS / "voices.npz", **arrays)
    print(f"HolosTTS assets: {len(vocab)} phonemes, {len(arrays)} voices")


if __name__ == "__main__":
    main()
