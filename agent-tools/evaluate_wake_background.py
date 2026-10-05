"""Check new background sources at a frozen model threshold; no threshold search."""
import argparse
import hashlib
import json
from pathlib import Path
from train_wake_prototype import Quantized, sliding_features, stable_score
from wake_training_audio import load_wav, RATE

ROOT = Path(__file__).resolve().parents[1] / 'artifacts/wake-training'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True)
    parser.add_argument('--dataset', required=True)
    parser.add_argument('--new-since', required=True)
    parser.add_argument('--split', choices=('train','test'), default='train')
    args = parser.parse_args()
    folder = ROOT / args.run
    output = folder / f'background-{args.dataset}-{args.split}.json'
    if output.exists():
        raise FileExistsError(output)
    data = ROOT / args.dataset
    manifest = json.loads((data/'manifest.json').read_text(encoding='utf-8'))
    previous = json.loads((ROOT/args.new_since/'manifest.json').read_text(encoding='utf-8'))
    known = {r['source_sha256'] for r in previous['sources']}
    sources = [r for r in manifest['sources'] if r['source']=='real' and r['label']=='negative'
               and r['split']==args.split and r['source_sha256'] not in known]
    if not sources:
        raise ValueError('No new background sources')
    config = json.loads((folder/'model.json').read_text(encoding='utf-8'))
    if config['threshold'] is None:
        raise ValueError('No operating threshold selected on validation')
    assert hashlib.sha256((folder/config['model']).read_bytes()).hexdigest()==config['sha256']
    model = Quantized(folder/config['model'])
    rows = []
    for record in sources:
        audio = load_wav((data/record['audio']).read_bytes())
        scores = model.predict(sliding_features(audio))
        rows.append(dict(id=record['id'],archive=record['source_archive'],seconds=len(audio)/RATE,
                         score=stable_score(scores),probabilities=scores.tolist()))
    result = dict(run=args.run,dataset=args.dataset,split=args.split,model_sha256=config['sha256'],
                  dataset_sha256=hashlib.sha256((data/'manifest.json').read_bytes()).hexdigest(),
                  threshold=config['threshold'],clips=len(rows),seconds=sum(r['seconds'] for r in rows),
                  false_positive_clips=sum(r['score']>=config['threshold'] for r in rows),rows=rows,
                  note='Clip-level background check; not a measurement of false alarms per hour.')
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))


if __name__ == '__main__':
    main()
