"""Profile installed Applio without editing its files or changing the live server."""
import argparse
import cProfile
import io
import json
import os
from pathlib import Path
import pstats
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.input.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    project = Path(__file__).resolve().parents[1]
    models = project / "BendeRadio/examples/BenderRealtime/local_server/voice_clone/models"
    applio = Path("A:/Programs/Applio-main/Applio-main")
    os.chdir(applio)
    sys.path.insert(0, str(applio))
    from rvc.infer.infer import VoiceConverter
    import rvc.infer.pipeline as pipeline
    import torch
    import numpy as np
    import soundfile as sf
    vc = VoiceConverter()
    kwargs = dict(audio_input_path=str(source), audio_output_path=str(output / "probe.wav"),
                  model_path=str(models / "bender.pth"), index_path=str(models / "bender.index"),
                  pitch=-3, f0_method="rmvpe", index_rate=.55, volume_envelope=.75,
                  protect=.33, embedder_model="contentvec", export_format="WAV")
    original_empty = torch.cuda.empty_cache
    original_read = pipeline.faiss.read_index
    original_threads = pipeline.faiss.omp_get_max_threads()
    print("THREADS", {"torch": torch.get_num_threads(), "faiss": original_threads}, flush=True)
    class Index:
        def __init__(self, index):
            self.index = index
            self.embeddings = index.reconstruct_n(0, index.ntotal)
        def __getattr__(self, name):
            return getattr(self.index, name)
        def reconstruct_n(self, start, count):
            assert start == 0 and count == self.index.ntotal
            return self.embeddings
    cached = Index(original_read(kwargs["index_path"]))
    rows = []
    reference = None
    def convert(label, profile=False):
        nonlocal reference
        torch.manual_seed(1234)
        torch.cuda.synchronize()
        profiler = cProfile.Profile()
        started = time.perf_counter()
        if profile:
            profiler.enable()
        vc.convert_audio(**kwargs)
        torch.cuda.synchronize()
        elapsed = (time.perf_counter() - started) * 1000
        if profile:
            profiler.disable()
            buf = io.StringIO()
            pstats.Stats(profiler, stream=buf).sort_stats("cumtime").print_stats(45)
            (output / f"{label}-profile.txt").write_text(buf.getvalue(), encoding="utf-8")
        signal, sr = sf.read(output / "probe.wav", dtype="int16")
        if label == "baseline":
            reference = signal.copy()
        delta = None if reference is None or len(signal) != len(reference) else int(np.max(np.abs(signal.astype(np.int32)-reference.astype(np.int32))))
        row = {"label": label, "ms": elapsed, "samples": len(signal), "sr": sr,
               "max_sample_delta": delta, "gpu_allocated_mb": torch.cuda.memory_allocated()/2**20,
               "gpu_reserved_mb": torch.cuda.memory_reserved()/2**20}
        rows.append(row)
        print("RESULT", json.dumps(row), flush=True)
    convert("cold")
    convert("baseline", True)
    for threads in (1, 2, 4, 8, original_threads):
        pipeline.faiss.omp_set_num_threads(threads)
        for _ in range(2):
            convert(f"faiss_{threads}")
    pipeline.faiss.omp_set_num_threads(original_threads)
    pipeline.faiss.read_index = lambda *a, **kw: cached
    convert("index_cached", True)
    convert("index_cached")
    pipeline.faiss.read_index = original_read
    torch.cuda.empty_cache = lambda: None
    convert("retain_allocator", True)
    convert("retain_allocator")
    torch.cuda.empty_cache = original_empty
    convert("baseline_final")
    sys.path.insert(0, str(project / "BendeRadio/examples/BenderRealtime/local_server"))
    from rvc_runtime import install_pitch_cache
    from rvc.lib.predictors import f0 as f0_module
    install_pitch_cache(f0_module)
    convert("pitch_cache_cold")
    for repeat in range(5):
        convert("pitch_cached", profile=repeat == 0)
    (output / "results.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
