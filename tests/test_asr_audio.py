"""ASR regressions without loading models or contacting a voice/LLM server."""
import ast
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

import numpy as np

SERVER_DIR = Path(__file__).resolve().parents[1] / "BendeRadio/examples/BenderRealtime/local_server"
sys.path.insert(0, str(SERVER_DIR))
from asr_audio import ASR_RATE, pcm16_for_asr


def tone(hz=1000, rate=24000, seconds=1):
    t = np.arange(round(rate * seconds)) / rate
    return np.round(12000 * np.sin(2 * np.pi * hz * t)).astype("<i2").tobytes()


def server_functions():
    wanted = {"_asr_ok", "_asr_enhance", "_pcm_for_whisper", "_pcm_rms", "transcribe"}
    tree = ast.parse((SERVER_DIR / "server.py").read_text(encoding="utf-8"))
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in wanted]
    ns = dict(np=np, IN_RATE=24000, ASR_RATE=ASR_RATE, pcm16_for_asr=pcm16_for_asr,
              ASR_LANGS=("ru", "uk"), ASR_MIN_RMS=900, ASR_MIN_LOGPROB=-0.55,
              ASR_MAX_NO_SPEECH=0.4, _is_hallucination=lambda *args: False,
              _looks_like_credits=lambda text: False, _note_hallucination=Mock(), log=Mock())
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "asr-server-functions", "exec"), ns)
    return ns


class AsrAudioTests(unittest.TestCase):
    def test_duration_pitch_and_level_survive_resampling(self):
        audio = pcm16_for_asr(tone(), 24000)
        self.assertEqual(audio.shape, (16000,))
        self.assertEqual(audio.dtype, np.float32)
        self.assertTrue(audio.flags.c_contiguous)
        peak_hz = np.argmax(np.abs(np.fft.rfft(audio))) * ASR_RATE / audio.size
        self.assertEqual(peak_hz, 1000)
        self.assertAlmostEqual(float(np.sqrt(np.mean(audio[100:-100] ** 2))),
                               12000 / 32768 / np.sqrt(2), places=3)

    def test_downsampling_filters_frequencies_above_nyquist(self):
        audio = pcm16_for_asr(tone(10000), 24000)
        self.assertLess(float(np.sqrt(np.mean(audio[100:-100] ** 2))), 0.001)

    def test_full_fifteen_second_recording_keeps_tail(self):
        pcm = np.zeros(24000 * 15, dtype="<i2")
        pcm[-240:] = 10000
        audio = pcm16_for_asr(pcm.tobytes(), 24000)
        self.assertEqual(audio.size, 16000 * 15)
        self.assertGreater(float(audio[-1]), 0.25)

    def test_native_rate_needs_only_pcm_conversion(self):
        samples = np.array([-32768, -123, 0, 123, 32767], dtype="<i2")
        np.testing.assert_array_equal(pcm16_for_asr(samples.tobytes(), 16000),
                                      samples.astype(np.float32) / 32768)

    def test_empty_and_malformed_input(self):
        self.assertEqual(pcm16_for_asr(b"", 24000).size, 0)
        with self.assertRaises(ValueError):
            pcm16_for_asr(b"\x01", 24000)
        with self.assertRaises(ValueError):
            pcm16_for_asr(b"\x01\x00", 0)

    def test_server_enhancement_receives_real_whisper_rate(self):
        ns = server_functions()
        ns["_asr_enhance"] = Mock(side_effect=lambda audio, rate: audio)
        audio = ns["_pcm_for_whisper"](tone())
        self.assertEqual(audio.size, 16000)
        self.assertEqual(ns["_asr_enhance"].call_args.args[1], 16000)

    def test_normal_and_vad_failure_paths_both_use_16khz(self):
        for fail_vad in (False, True):
            with self.subTest(fail_vad=fail_vad):
                ns = server_functions()
                seen = []

                def recognize(audio, lang, *, use_vad, rms):
                    seen.append((audio.size, use_vad))
                    if use_vad and fail_vad:
                        raise RuntimeError("VAD unavailable")
                    return "Включи радио", lang, 1, -0.2, 0.01, True

                ns["_asr_pass"] = recognize
                self.assertTrue(ns["transcribe"](tone())[-1])
                expected = [(16000, True), (16000, False)] if fail_vad else [(16000, True)]
                self.assertEqual(seen, expected)

    def test_loudness_cannot_override_poor_recognition(self):
        ok = server_functions()["_asr_ok"]
        self.assertFalse(ok("А то я буду взять рапунт на верхнюю школу", -0.8, 0.1, 8000))
        self.assertTrue(ok("Расскажи что-нибудь интересное", -0.2, 0.1, 2000))
        self.assertFalse(ok("Расскажи что-нибудь интересное", -0.2, 0.7, 8000))


if __name__ == "__main__":
    unittest.main()
