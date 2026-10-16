"""Compare original/cached pitch curves and exercise varying lengths on CUDA."""
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/voice-latency-20261011/pitch-verification"
SERVER = ROOT / "BendeRadio/examples/BenderRealtime/local_server"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    applio = Path("A:/Programs/Applio-main/Applio-main")
    os.chdir(applio)
    sys.path[:0] = [str(applio), str(SERVER)]
    import numpy as np
    import soundfile as sf
    import torch
    from scipy.signal import resample_poly
    from rvc.lib.predictors import f0 as f0_module
    from rvc_runtime import install_pitch_cache
    clips = []
    for index in range(4):
        pcm, sr = sf.read(ROOT / f"artifacts/voice-latency-20261011/baseline/phrase_{index}_round_1.wav")
        assert sr == 24000
        clips.append(resample_poly(pcm, 2, 3).astype(np.float32))
    originals = []
    for pcm in clips:
        model = f0_module.RMVPE(device="cuda:0", sample_rate=16000, hop_size=160)
        originals.append(model.get_f0(pcm, filter_radius=.03))
        del model
    torch.cuda.empty_cache()
    before_mb = torch.cuda.memory_allocated()/2**20
    install_pitch_cache(f0_module)
    rows = []
    for repeat in range(5):
        for index, pcm in enumerate(clips):
            began = time.perf_counter()
            model = f0_module.RMVPE(device="cuda:0", sample_rate=16000, hop_size=160)
            actual = model.get_f0(pcm, filter_radius=.03)
            torch.cuda.synchronize()
            elapsed = (time.perf_counter()-began)*1000
            expected = originals[index]
            same_shape = actual.shape == expected.shape
            delta = float(np.max(np.abs(actual-expected))) if same_shape else None
            voiced_equal = bool(np.array_equal(actual > 0, expected > 0)) if same_shape else False
            row = {"round": repeat, "phrase": index, "ms": elapsed,
                   "max_f0_delta_hz": delta, "voiced_frames_equal": voiced_equal,
                   "gpu_allocated_mb": torch.cuda.memory_allocated()/2**20}
            rows.append(row)
            print(json.dumps(row), flush=True)
            if not same_shape or not voiced_equal or delta > .001:
                raise AssertionError("Pitch contour changed")
            del model
    report = {"all_pitch_curves_equal_within_hz": .001, "before_mb": before_mb, "rows": rows}
    (OUT / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    # Isolate caching from the vocoder's random sampling: use identical input
    # and reset RNG just before voice generation, after pitch extraction.
    from rvc.infer.infer import VoiceConverter
    import rvc.infer.pipeline as pipeline
    original_conversion = pipeline.Pipeline.voice_conversion
    def seeded_conversion(*a, **kw):
        torch.manual_seed(31415)
        return original_conversion(*a, **kw)
    pipeline.Pipeline.voice_conversion = seeded_conversion
    cache = f0_module.RMVPE0Predictor
    converter = VoiceConverter()
    model_dir = SERVER / "voice_clone/models"
    pairs = []
    for index in range(4):
        signals = []
        for variant in ("uncached", "cached"):
            f0_module.RMVPE0Predictor = cache.factory if variant == "uncached" else cache
            destination = OUT / f"controlled_{index}_{variant}.wav"
            converter.convert_audio(
                audio_input_path=str(ROOT / f"artifacts/voice-latency-20261011/baseline/phrase_{index}_round_1.wav"),
                audio_output_path=str(destination), model_path=str(model_dir / "bender.pth"),
                index_path=str(model_dir / "bender.index"), pitch=-3, index_rate=.55,
                f0_method="rmvpe", volume_envelope=.75, protect=.33,
                embedder_model="contentvec", export_format="WAV")
            signal, _ = sf.read(destination)
            signals.append(signal)
        a, b = signals
        if a.shape != b.shape:
            raise AssertionError("Output duration changed for identical input")
        correlation = float(np.corrcoef(a, b)[0, 1])
        rms_error = float(np.sqrt(np.mean((a-b)**2)))
        pairs.append({"phrase": index, "correlation": correlation, "rms_error": rms_error})
        print("CONTROLLED", json.dumps(pairs[-1]), flush=True)
        if correlation < .999:
            raise AssertionError("Unexpected waveform change")
    (OUT / "controlled-audio.json").write_text(json.dumps(pairs, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
