"""Prepare a source-disjoint development split and augmented training features.

Validation is same-session/same-speaker: it is not a final independent test.
All descendants of a source recording stay in its split. Validation noise never
enters augmentation or quantization calibration. Corrected archives only.
"""
import collections
import argparse
import hashlib
import json
from pathlib import Path
import wave
import zipfile
import numpy as np
from wake_training_audio import (RATE, SAMPLES, FRONTEND, MEL, WINDOW, load_wav,
    highpass, speech_bounds, fit_window, features, augment, seed_for, stream_training_window)

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'artifacts/wake-training'
OUT = WORK / 'dataset-v1'
LABELS = ('hey_bender', 'privet_bender', 'bender', 'negative')


def assign_real_splits(records, frozen_sources=None, reserve_test_per_phrase=0, reserve_test_negative=0):
    """Keep previous holdouts fixed when adding a batch; never train on user test."""
    frozen = {r['id']: r for r in (frozen_sources or []) if r['source'] == 'real'}
    if frozen_sources is not None:
        current = {r['id']: r for r in records}
        if not frozen.keys() <= current.keys():
            raise ValueError('Frozen sources are missing from the current index')
        for record in records:
            previous = frozen.get(record['id'])
            if previous:
                if any(record[k] != previous[k] for k in ('label', 'source_sha256')):
                    raise ValueError('Frozen source label/audio changed: ' + record['id'])
                record['split'] = previous['split']
                if record['original_split'] == 'test' and record['split'] != 'test':
                    raise ValueError('A user test source was previously used for development')
            else:
                record['split'] = 'test' if record['original_split'] == 'test' else 'train'
    else:
        holdout_counts = {'privet_bender':4, 'hey_bender':3, 'bender':2, 'negative':5}
        for record in records:
            record['split'] = 'test' if record['original_split'] == 'test' else 'train'
        for label in LABELS:
            group = sorted([r for r in records if r['label'] == label and r['split'] == 'train'],
                           key=lambda r: seed_for(r['id']))
            for record in group[:holdout_counts[label]]:
                record['split'] = 'validation'
    # Reserve fresh sources before training; never move an old training source
    # into test. A same-speaker, same-batch holdout is not a new-speaker test.
    for label in LABELS:
        count = reserve_test_negative if label == 'negative' else reserve_test_per_phrase
        if not count:
            continue
        candidates = sorted([r for r in records if r['id'] not in frozen and
                             r['label'] == label and r['split'] == 'train'],
                            key=lambda r: seed_for('fresh-test:' + r['source_sha256']))
        if len(candidates) < count:
            raise ValueError('Not enough fresh sources for test: ' + label)
        for record in candidates[:count]:
            record['split'] = 'test'
            record['test_reason'] = 'Reserved before training from new source recordings'


def main():
    global OUT
    parser=argparse.ArgumentParser();parser.add_argument('--dataset',default='dataset-v2')
    parser.add_argument('--freeze-splits', help='Previous dataset directory name; new train audio stays in train')
    parser.add_argument('--reserve-test-per-phrase', type=int, default=0)
    parser.add_argument('--reserve-test-negative', type=int, default=0)
    parser.add_argument('--stream-windows', action='store_true')
    args=parser.parse_args()
    OUT=WORK/args.dataset
    if OUT.exists() and any(OUT.iterdir()):
        raise FileExistsError('Use a new --dataset name to preserve earlier experiments')
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'audio').mkdir(exist_ok=True)
    source_root = ROOT / 'artifacts/wakeword-device'
    index = json.loads((source_root / 'dataset-index.json').read_text(encoding='utf-8'))
    records = []
    audio = {}
    seen_hashes = set()
    for entry in index['archives']:
        path = source_root / entry['file']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
        with zipfile.ZipFile(path) as archive:
            for item in json.loads(archive.read('manifest.json'))['items']:
                assert item['label'] in LABELS and item['sha256'] not in seen_hashes
                seen_hashes.add(item['sha256'])
                body = archive.read(item['file'])
                assert hashlib.sha256(body).hexdigest() == item['sha256']
                ident = 'real_' + str(item['id'])
                records.append(dict(id=ident, label=item['label'], source='real', original_split=item['split'],
                                    source_archive=entry['file'], source_sha256=item['sha256']))
                audio[ident] = load_wav(body)  # already high-passed on the device
    assert len(records) == index['total']
    assert len({r['id'] for r in records}) == len(records), 'Duplicate source IDs'
    # Exact source IDs are written before augmentation. No random window split.
    frozen_path=WORK/args.freeze_splits/'manifest.json' if args.freeze_splits else None
    frozen=json.loads(frozen_path.read_text(encoding='utf-8')) if frozen_path else None
    assign_real_splits(records, frozen['sources'] if frozen else None,
                       args.reserve_test_per_phrase, args.reserve_test_negative)
    synthetic = json.loads((WORK / 'synthetic/manifest.json').read_text(encoding='utf-8'))
    for item in synthetic['items']:
        body = (WORK / 'synthetic' / item['file']).read_bytes()
        assert hashlib.sha256(body).hexdigest() == item['sha256']
        ident = 'synthetic_' + item['id']
        hold = seed_for(item['text']) % 5 == 0 if item['label']=='negative' else int(item['id'].rsplit('_',1)[1]) % 6 == 0
        records.append(dict(id=ident, label=item['label'], source='synthetic',
                            source_sha256=item['sha256'], voice=item['voice'], text=item['text'],
                            split='validation' if hold else 'train'))
        audio[ident] = highpass(load_wav(body))
    for record in records:
        ident = record['id']; x = audio[ident]
        record['audio'] = f'audio/{ident}.wav'
        pcm = np.clip(np.round(x*32768), -32768, 32767).astype('<i2').tobytes()
        with wave.open(str(OUT/record['audio']), 'wb') as wav:
            wav.setparams((1, 2, RATE, 0, 'NONE', 'not compressed')); wav.writeframes(pcm)
        # Use identical quantized PCM for feature preparation and later evaluation.
        audio[ident] = np.frombuffer(pcm, '<i2').astype(np.float32)/32768
        record['seconds'] = len(x)/RATE
        if record['label'] != 'negative':
            a,b = speech_bounds(x); record['training_trim_seconds'] = round((b-a)/RATE,3)
    backgrounds = [audio[r['id']] for r in records if r['source']=='real' and r['split']=='train' and r['label']=='negative']
    train, labels, groups = [], [], []
    validation, val_labels, val_ids = [], [], []
    for number, record in enumerate(records):
        ident=record['id']; positive=record['label']!='negative'; x=audio[ident]
        rng=np.random.default_rng(seed_for(ident))
        if record['split']=='train':
            views=(32 if positive else 48) if record['source']=='real' else (6 if positive else 8)
            for _ in range(views):
                f=features(augment(x, rng, positive, backgrounds))
                # Same feature masking distribution for both classes.
                if rng.random()<.5:
                    at=int(rng.integers(35)); f[:,at:at+int(rng.integers(1,6)),:]=-3
                if rng.random()<.35:
                    at=int(rng.integers(220)); f[at:at+int(rng.integers(1,18)),:,:]=-3
                train.append(f); labels.append(int(positive)); groups.append(ident)
            if args.stream_windows:
                for _ in range(24 if record['source']=='real' else 8):
                    window=stream_training_window(x,rng,positive)
                    if window is not None:
                        train.append(features(window));labels.append(int(positive));groups.append(ident)
        elif record['split']=='validation':
            validation.append(features(fit_window(x, rng, positive)))
            val_labels.append(int(positive)); val_ids.append(ident)
        if (number+1)%50==0: print(f'Prepared {number+1}/{len(records)} sources',flush=True)
    # Quiet/empty inputs must not activate. Pure generated noise has no source leakage.
    rng=np.random.default_rng(1004)
    for i in range(192):
        x=rng.normal(0,10**rng.uniform(-5,-1.8),SAMPLES).astype(np.float32) if i else np.zeros(SAMPLES,np.float32)
        train.append(features(x));labels.append(0);groups.append(f'generated_noise_{i}')
    train=np.asarray(train,np.float32);validation=np.asarray(validation,np.float32)
    assert not(set(groups)&set(val_ids))
    np.savez_compressed(OUT/'features.npz', train=train, labels=np.asarray(labels,np.float32),
                        groups=np.asarray(groups), validation=validation,
                        val_labels=np.asarray(val_labels,np.float32), val_ids=np.asarray(val_ids))
    counts=collections.Counter((r['source'],r['split'],r['label']) for r in records)
    manifest={'version':1,'frontend':FRONTEND,'sources':records,'counts':{'/'.join(k):v for k,v in counts.items()},
              'stream_training_windows':args.stream_windows,
              'source_index':index,
              'frozen_split_manifest_sha256':hashlib.sha256(frozen_path.read_bytes()).hexdigest() if frozen_path else None,
              'training_windows':len(train),'validation_windows':len(validation),
              'independent_test_available':False,
              'heldout_source_test_available':any(r['split']=='test' for r in records),
              'reserved_test_sources':sum(r['split']=='test' for r in records),
              'limitations':['Real holdout recordings come from the same speaker and nearby sessions.',
                             'Synthetic holdout contains the same two TTS voices; not unseen humans.',
                             'Validation is used to select a threshold and checkpoint; it is not a final test.',
                             'Reserved test, when present, is unseen audio from the same speaker/batches, not independent new speakers or long ambient audio.']}
    (OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    reference=np.sin(2*np.pi*440*np.arange(SAMPLES)/RATE).astype(np.float32)*.1
    np.savez(OUT/'frontend_reference.npz',pcm=reference,features=features(reference),mel=MEL,window=WINDOW)
    print(json.dumps({'train_windows':len(train),'validation_windows':len(validation),'counts':manifest['counts']}),flush=True)


if __name__=='__main__': main()
