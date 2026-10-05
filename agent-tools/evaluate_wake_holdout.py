"""Evaluate a chosen INT8 model once on reserved audio; never select a threshold here."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from train_wake_prototype import Quantized, sliding_features, stable_score, metrics
from wake_training_audio import load_wav, RATE

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True)
    parser.add_argument('--previous-test-dataset', help='Separate previously inspected regression files from fresh test files')
    args = parser.parse_args()
    folder = ROOT / 'artifacts/wake-training' / args.run
    output = folder / 'holdout.json'
    if output.exists():
        raise FileExistsError('Keep the original holdout result; do not tune against it')
    report = json.loads((folder / 'report.json').read_text(encoding='utf-8'))
    config = json.loads((folder / 'model.json').read_text(encoding='utf-8'))
    data = ROOT / 'artifacts/wake-training' / report['dataset']
    assert hashlib.sha256((data / 'manifest.json').read_bytes()).hexdigest() == report['dataset_manifest_sha256']
    assert hashlib.sha256((folder / config['model']).read_bytes()).hexdigest() == config['sha256']
    manifest = json.loads((data / 'manifest.json').read_text(encoding='utf-8'))
    test = [r for r in manifest['sources'] if r['split'] == 'test']
    if not test:
        raise ValueError('No reserved test recordings')
    with np.load(data / 'features.npz', allow_pickle=False) as arrays:
        used = set(arrays['groups']) | set(arrays['val_ids'])
    assert not used & {r['id'] for r in test}
    test_hashes = {r['source_sha256'] for r in test}
    assert not test_hashes & {r['source_sha256'] for r in manifest['sources'] if r['split'] != 'test'}
    model = Quantized(folder / config['model'])
    assert model.input['shape'].tolist() == config['input_shape']
    assert not any(np.issubdtype(t['dtype'], np.floating) for t in model.interpreter.get_tensor_details())
    rows = []
    for r in test:
        x = load_wav((data / r['audio']).read_bytes())
        probabilities = model.predict(sliding_features(x))
        rows.append(dict(id=r['id'], label=r['label'], source=r['source'], seconds=len(x)/RATE,
                         score=stable_score(probabilities), probabilities=probabilities.tolist()))
    result = dict(run=args.run, dataset=report['dataset'], model_sha256=config['sha256'],
                  threshold=report['threshold'], threshold_source='Frozen from development validation before test',
                  diagnostic_only=report['recommended_threshold'] is None,
                  metrics=metrics(rows, report['threshold']), rows=rows,
                  ready_for_device=False,
                  limitations=['Unseen source recordings, but the same speaker and collection batches.',
                               'Short background clips do not establish false activations per hour.',
                               'No on-device runtime/performance validation yet.'])
    if args.previous_test_dataset:
        previous = json.loads((ROOT/'artifacts/wake-training'/args.previous_test_dataset/'manifest.json').read_text(encoding='utf-8'))
        previous_ids = {r['id'] for r in previous['sources'] if r['split']=='test'}
        assert previous_ids <= {r['id'] for r in test}
        result['previous_test_dataset'] = args.previous_test_dataset
        result['previous_test_regression'] = metrics([r for r in rows if r['id'] in previous_ids], report['threshold'])
        result['fresh_test'] = metrics([r for r in rows if r['id'] not in previous_ids], report['threshold'])
        result['limitations'].append('Previous test files were already inspected for an earlier model; use fresh_test for newly unseen sources.')
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
