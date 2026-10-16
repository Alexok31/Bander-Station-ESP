"""Cache lifecycle regressions; no torch, models, or GPU required."""
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "BendeRadio/examples/BenderRealtime/local_server"))
from rvc_runtime import PitchPredictorCache, install_pitch_cache


class PitchCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.model = Path(self.temp.name) / "weights.pt"
        self.model.write_bytes(b"original")
        self.loads = []
        def factory(path, device=None):
            model = SimpleNamespace(weights=Path(path).read_bytes(), device=device)
            self.loads.append(model)
            return model
        self.cache = PitchPredictorCache(factory)

    def test_reuse_and_device_switch_dont_accumulate_networks(self):
        first = self.cache(self.model, device="cuda:0")
        self.assertIs(first, self.cache(str(self.model), device="cuda:0"))
        self.assertEqual(len(self.loads), 1)
        cpu = self.cache(self.model, device="cpu")
        self.assertIsNot(first, cpu)
        self.assertIsNot(first, self.cache(self.model, device="cuda:0"))
        self.assertEqual(len(self.loads), 3)

    def test_updated_or_missing_weights_never_return_old_network(self):
        old = self.cache(self.model)
        self.model.write_bytes(b"replacement weights")
        new = self.cache(self.model)
        self.assertIsNot(old, new)
        self.assertEqual(new.weights, b"replacement weights")
        self.model.unlink()
        with self.assertRaises(FileNotFoundError):
            self.cache(self.model)

    def test_load_failure_can_be_retried(self):
        original = self.cache.factory
        self.cache.factory = lambda *a, **kw: (_ for _ in ()).throw(RuntimeError("load failed"))
        with self.assertRaises(RuntimeError):
            self.cache(self.model)
        self.assertIsNone(self.cache.predictor)
        self.cache.factory = original
        self.assertEqual(self.cache(self.model).weights, b"original")

    def test_install_is_idempotent_and_only_wraps_predictor(self):
        module = SimpleNamespace(RMVPE0Predictor=self.cache.factory, RMVPE=object())
        wrapper = module.RMVPE
        install_pitch_cache(module)
        cached = module.RMVPE0Predictor
        install_pitch_cache(module)
        self.assertIs(cached, module.RMVPE0Predictor)
        self.assertIs(wrapper, module.RMVPE)


if __name__ == "__main__":
    unittest.main()
