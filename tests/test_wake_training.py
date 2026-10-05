import io
import json
import os
from pathlib import Path
import sys
import unittest
import wave
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'agent-tools'))
import wake_training_audio as audio
from prepare_wake_training import assign_real_splits


class FrontendTests(unittest.TestCase):
    def test_silence_and_tone_features_are_finite_and_bounded(self):
        zero=audio.features(np.zeros(audio.SAMPLES,np.float32))
        self.assertEqual(zero.shape,(238,40,1))
        self.assertTrue(np.isfinite(zero).all())
        self.assertTrue(np.all(zero==zero[0,0,0]))
        self.assertLess(float(zero[0,0,0]),-2)
        x=np.sin(2*np.pi*1000*np.arange(audio.SAMPLES)/16000).astype(np.float32)*.1
        f=audio.features(x)
        self.assertTrue(np.isfinite(f).all())
        self.assertGreater(float(np.max(f)),0)
        self.assertLessEqual(float(np.max(f)),3)
        np.testing.assert_array_equal(f,audio.features(x))

    def test_resampler_rejects_above_nyquist_audio(self):
        def tone(hz):
            samples=(10000*np.sin(2*np.pi*hz*np.arange(24000)/24000)).astype('<i2')
            stream=io.BytesIO()
            with wave.open(stream,'wb') as w:
                w.setparams((1,2,24000,0,'NONE','not compressed'));w.writeframes(samples.tobytes())
            return audio.load_wav(stream.getvalue())[1000:-1000]
        wanted=tone(1000);aliased=tone(11000)
        self.assertLess(np.sqrt(np.mean(aliased**2)),np.sqrt(np.mean(wanted**2))*.01)

    def test_dc_filter_and_window_contract(self):
        x=audio.highpass(np.ones(16000,np.float32)*.5)
        self.assertLess(float(np.max(np.abs(x[8000:]))),1e-6)
        rng=np.random.default_rng(42)
        self.assertEqual(audio.fit_window(np.zeros(64000,np.float32),rng,False).shape,(audio.SAMPLES,))
        with self.assertRaises(ValueError):audio.features(np.zeros(100,np.float32))

    def test_isolated_click_does_not_stretch_training_phrase(self):
        rng=np.random.default_rng(7)
        x=rng.normal(0,.015,64000).astype(np.float32)
        x[16000:32000]+=.2*np.sin(2*np.pi*500*np.arange(16000)/16000)
        x[56000:56040]+=.6
        start,end=audio.speech_bounds(x)
        self.assertLessEqual(start,16000)
        self.assertGreaterEqual(end,32000)
        self.assertLess(end,40000)

    def test_stream_negative_contains_real_prefix_and_startup_padding(self):
        class FirstWindow:
            def integers(self,low,high):return low
            def uniform(self,low,high):return 0.
        x=np.linspace(-.1,.1,64000,dtype=np.float32)
        window=audio.stream_training_window(x,FirstWindow(),False)
        np.testing.assert_array_equal(window[-1600:],x[:1600])
        np.testing.assert_array_equal(window[:-1600],np.zeros(audio.SAMPLES-1600))
        # An overlong positive must not be silently truncated into a labelled word.
        self.assertIsNone(audio.stream_training_window(np.ones(64000,np.float32)*.1,
                                                      FirstWindow(),True))


class PreparedDatasetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'artifacts/wake-training'/os.environ.get('BENDER_TEST_DATASET','dataset-v2')
        if not(path/'features.npz').exists():raise unittest.SkipTest('Dataset not prepared')
        cls.manifest=json.loads((path/'manifest.json').read_text(encoding='utf-8'))
        cls.data=np.load(path/'features.npz',allow_pickle=False)

    def test_source_recordings_never_cross_splits(self):
        self.assertFalse(set(self.data['groups'])&set(self.data['val_ids']))
        sources={r['id']:r for r in self.manifest['sources']}
        for group in set(self.data['groups']):
            if group.startswith('generated_noise_'):continue
            self.assertEqual(sources[group]['split'],'train')
        for group in self.data['val_ids']:self.assertEqual(sources[group]['split'],'validation')
        heldout={r['id'] for r in self.manifest['sources'] if r['split']=='test'}
        self.assertFalse(heldout & (set(self.data['groups']) | set(self.data['val_ids'])))
        self.assertFalse(self.manifest['independent_test_available'])

    def test_corrected_negative_archive_and_all_three_phrases(self):
        real=[r for r in self.manifest['sources'] if r['source']=='real']
        self.assertEqual(len(real),self.manifest.get('source_index',{}).get('total',55))
        negative=[r for r in real if r['label']=='negative']
        self.assertEqual(len([r for r in negative if r['source_archive'].endswith('-negative.zip')]),19)
        self.assertEqual({r['label'] for r in real},{'negative','hey_bender','privet_bender','bender'})
        self.assertEqual(len([r for r in real if r['split']=='validation']),14)

    def test_synthetic_negative_texts_do_not_cross_splits(self):
        by_text={}
        for r in self.manifest['sources']:
            if r['source']=='synthetic' and r['label']=='negative':
                by_text.setdefault(r['text'],set()).add(r['split'])
        self.assertTrue(all(len(s)==1 for s in by_text.values()))


class SplitExpansionTests(unittest.TestCase):
    def test_old_holdouts_and_explicit_test_cannot_enter_training(self):
        def record(i,split='train'):
            return dict(id=str(i),label='bender',source='real',source_sha256=str(i),original_split=split)
        frozen=[dict(record(1),split='validation'),dict(record(2),split='train')]
        records=[record(1),record(2),record(3,'test'),record(4),record(5)]
        assign_real_splits(records,frozen)
        self.assertEqual([r['split'] for r in records],['validation','train','test','train','train'])
        changed=[record(1),record(2)]
        changed[0]['source_sha256']='changed'
        with self.assertRaises(ValueError):assign_real_splits(changed,frozen)


if __name__=='__main__':unittest.main()
