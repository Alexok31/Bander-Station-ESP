"""Pronunciation regressions without model loading, network or server startup."""

import ast
from pathlib import Path
import re
from types import SimpleNamespace
import unittest
from unittest.mock import Mock


SERVER_DIR = Path(__file__).resolve().parents[1] / "BendeRadio/examples/BenderRealtime/local_server"


def load_functions():
    wanted = {"_keep_first_acute", "apply_uk_stress", "_piper_pcm", "synth"}
    constants = {"_ACUTE", "_UK_WORD", "_UK_VOWELS"}
    tree = ast.parse((SERVER_DIR / "server.py").read_text(encoding="utf-8"))
    nodes = [n for n in tree.body if
             isinstance(n, ast.FunctionDef) and n.name in wanted
             or isinstance(n, ast.Assign) and any(
                 isinstance(t, ast.Name) and t.id in constants for t in n.targets)]
    # _piper_pcm only needs these array operations for the transport boundary.
    arrays = SimpleNamespace(
        ndarray=list, int16=int, asarray=lambda values, dtype: list(values),
        concatenate=lambda chunks: [x for chunk in chunks for x in chunk],
    )
    ns = {
        "re": re, "np": arrays, "log": Mock(), "_STRESS_FIX": {},
        "_uk_stress": None,
        "stress_convert": SimpleNamespace(available=lambda: True, stress=Mock()),
    }
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "speech-functions", "exec"), ns)
    return ns


class StressTests(unittest.TestCase):
    def setUp(self):
        self.ns = load_functions()
        self.worker = self.ns["stress_convert"].stress
        self.apply = self.ns["apply_uk_stress"]

    def test_partial_accent_keeps_full_sentence_context(self):
        self.worker.return_value = "бе́ндер, розкажи́ істо́рію."
        self.assertEqual(self.apply("бе́ндер, розкажи історію."),
                         "бе́ндер, розкажи́ істо́рію.")
        self.worker.assert_called_once_with("бендер, розкажи історію.")

    def test_explicit_homographs_preserved_per_occurrence(self):
        self.worker.return_value = "за́мок і за́мок далеко́."
        self.assertEqual(self.apply("за́мок і замо́к далеко."),
                         "за́мок і замо́к далеко́.")

    def test_predicted_homographs_remain_distinct(self):
        self.worker.return_value = "за́мок і замо́к."
        self.assertEqual(self.apply("замок і замок."), "за́мок і замо́к.")

    def test_dictionary_override_has_priority(self):
        self.ns["_STRESS_FIX"] = {"бендер": "бе́ндер"}
        self.worker.return_value = "бенде́р гово́рить."
        self.assertEqual(self.apply("бенде́р говорить."), "бе́ндер гово́рить.")

    def test_fully_explicit_or_overridden_words_skip_worker(self):
        self.ns["_STRESS_FIX"] = {"бендер": "бе́ндер"}
        self.assertEqual(self.apply("бендер, розкажи́ істо́рію."),
                         "бе́ндер, розкажи́ істо́рію.")
        self.worker.assert_not_called()

    def test_worker_failure_uses_dictionary_without_losing_explicit_accents(self):
        self.worker.side_effect = RuntimeError("worker offline")
        fallback = Mock(return_value="замо́к далеко́.")
        self.ns["_uk_stress"] = fallback
        self.assertEqual(self.apply("за́мок далеко."), "за́мок далеко́.")
        fallback.assert_called_once_with("замок далеко.")

    def test_changed_words_are_rejected_and_dictionary_is_tried(self):
        self.worker.return_value = "different sentence"
        self.ns["_uk_stress"] = Mock(return_value="бе́ндер гово́рить.")
        self.assertEqual(self.apply("бе́ндер говорить."), "бе́ндер гово́рить.")

    def test_bad_fallback_cannot_rewrite_text(self):
        self.worker.side_effect = RuntimeError("offline")
        self.ns["_uk_stress"] = Mock(return_value="інший текст")
        self.assertEqual(self.apply("бе́ндер говорить."), "бе́ндер говорить.")

    def test_missing_predictors_preserve_input_and_overrides(self):
        self.ns["stress_convert"].available = lambda: False
        self.ns["_STRESS_FIX"] = {"бендер": "бе́ндер"}
        self.assertEqual(self.apply("бендер, за́мок далеко."),
                         "бе́ндер, за́мок далеко.")

    def test_punctuation_apostrophes_and_latin_are_preserved(self):
        self.worker.return_value = "п'я́тий — 25%, test!"
        self.assertEqual(self.apply("п'ятий — 25%, test!"), "п'я́тий — 25%, test!")

    def test_duplicate_and_isolated_accents_do_not_break_alignment(self):
        self.worker.return_value = " бе́ндер гово́рить."
        self.assertEqual(self.apply("́ бе́нде́р говорить."), "́ бе́ндер гово́рить.")

    def test_empty_and_single_syllable_input_skip_worker(self):
        self.assertEqual(self.apply(""), "")
        self.assertEqual(self.apply("так!"), "так!")
        self.worker.assert_not_called()


class SynthesisPreparationTests(unittest.TestCase):
    def test_synth_prepares_once_before_piper(self):
        ns = load_functions()
        ready = Mock(return_value="бе́ндер, приві́т!")
        voice = Mock()
        # Stop inside the actual Piper call, after all text preparation.
        # Reintroducing ready(text) in _piper_pcm would increment ready twice.
        voice.synthesize.side_effect = RuntimeError("stop at voice boundary")
        ns.update({
            "perf_counter": lambda: 0, "piper_voice_en": None,
            "piper_voice": voice, "piper_syn": object(), "piper_ready_uk": ready,
            "_clause_units": lambda text: [text],
        })
        with self.assertRaisesRegex(RuntimeError, "stop at voice boundary"):
            ns["synth"]("Бендер, привіт!")
        ready.assert_called_once_with("Бендер, привіт!")
        voice.synthesize.assert_called_once_with("бе́ндер, приві́т!", ns["piper_syn"])

    def test_prepared_text_reaches_piper_unchanged(self):
        ns = load_functions()
        voice = Mock()
        voice.synthesize.return_value = [SimpleNamespace(
            sample_rate=22050, audio_int16_array=[1, 2, 3])]
        pcm, rate = ns["_piper_pcm"](voice, "settings", "бе́ндер.")
        voice.synthesize.assert_called_once_with("бе́ндер.", "settings")
        self.assertEqual((pcm, rate), ([1, 2, 3], 22050))


if __name__ == "__main__":
    unittest.main()
