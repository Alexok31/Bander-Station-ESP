"""Small worker-local adapters for the installed Applio inference library."""
from pathlib import Path


class PitchPredictorCache:
    """Keep one RMVPE network alive across serial conversions.

    Applio creates an RMVPE wrapper per phrase. Only its weight-bearing
    predictor is retained here; per-call high-register settings and audio
    still go through Applio unchanged. No files in the Applio install change.
    The worker processes one request at a time, so this cache is serial too.
    """

    def __init__(self, factory):
        self.factory = factory
        self.key = None
        self.predictor = None

    def __call__(self, model_path, device=None):
        path = Path(model_path).resolve()
        stat = path.stat()  # Never return old weights if the file disappeared.
        key = (str(path), stat.st_mtime_ns, stat.st_size, str(device))
        if key != self.key:
            # Release the old network before allocating a different one.
            self.key = None
            self.predictor = None
            predictor = self.factory(str(path), device=device)
            self.predictor = predictor
            self.key = key
        return self.predictor


def install_pitch_cache(f0_module):
    """Adapt the constructor used by Applio's RMVPE wrapper, once per worker."""
    if not isinstance(f0_module.RMVPE0Predictor, PitchPredictorCache):
        f0_module.RMVPE0Predictor = PitchPredictorCache(f0_module.RMVPE0Predictor)
