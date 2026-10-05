"""Run the frozen model on inputs produced by the actual C++ streaming DSP."""
import hashlib
import json
import os
import re
from pathlib import Path
os.environ.setdefault('TF_USE_LEGACY_KERAS','1')
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL','2')
os.environ.setdefault('CUDA_VISIBLE_DEVICES','-1')
import numpy as np
import tensorflow as tf

ROOT=Path(__file__).resolve().parents[1]
RUN=ROOT/'artifacts/wake-training/prototype-v7-finetune'
OUT=ROOT/'artifacts/wake-runtime'

def main():
    policy=(ROOT/'BendeRadio/WakeDecision.h').read_text(encoding='utf-8')
    setting=lambda key:int(re.search(r'constexpr uint32_t '+key+r'\s*=\s*(\d+)',policy).group(1))
    interval=setting('intervalMs');stride=interval//setting('featureStepMs');confirmations=setting('confirmations')
    holdout=json.loads((RUN/'holdout.json').read_text(encoding='utf-8'))
    digest=hashlib.sha256((RUN/'bender.int8.tflite').read_bytes()).hexdigest()
    assert digest==holdout['model_sha256']
    interpreter=tf.lite.Interpreter(model_path=str(RUN/'bender.int8.tflite'),num_threads=2)
    interpreter.allocate_tensors()
    inp=interpreter.get_input_details()[0];out=interpreter.get_output_details()[0]
    scale,zero=out['quantization']; results=[]
    for row in holdout['rows']:
        values=np.fromfile(OUT/(row['id']+'.int8'),dtype=np.int8).reshape(-1,1,238,40,1)
        probs=[]
        for value in values:
            interpreter.set_tensor(inp['index'],value);interpreter.invoke()
            probs.append(float((int(interpreter.get_tensor(out['index']).flat[0])-zero)*scale))
        expected=np.array(row['probabilities'])
        assert len(expected)==len(probs)
        scores=[]
        for phase in range(stride):
            sequence=probs[phase::stride]
            scores.append(max(min(sequence[i:i+confirmations]) for i in range(len(sequence)-confirmations+1)))
        same=all((score>=holdout['threshold'])==(row['score']>=holdout['threshold']) for score in scores)
        delta=float(np.max(np.abs(np.array(probs)-expected)))
        # Tiny PCM/FIR differences can cross input quantization boundaries and
        # change confidence by several output steps. Assert decisions, report
        # the confidence difference rather than pretending to be bit-exact.
        assert same,(row['id'],scores,row['score'],delta)
        results.append(dict(id=row['id'],scores_by_phase=scores,max_probability_error=delta,decision_matches=same))
    report=dict(model_sha256=digest,threshold=holdout['threshold'],clips=len(results),
                decision_ms=interval,confirmations=confirmations,
                max_probability_error=max(r['max_probability_error'] for r in results),
                all_clip_decisions_match=True,rows=results,
                limitation='Regression using inspected recordings. Runtime 2 throughput still requires a new device log.')
    # Evaluate historical validation too, including short synthetic speech;
    # record changed decisions honestly instead of treating fewer runs as free.
    validation=json.loads((RUN/'report.json').read_text(encoding='utf-8'))
    summary=[]
    for source in ('real','synthetic'):
        for phase in range(stride):
            tp=fp=positives=negatives=0
            for row in validation['rows']:
                if row['source']!=source:continue
                p=row['probabilities'][phase::stride]
                hit=any(min(p[i:i+confirmations])>=holdout['threshold'] for i in range(len(p)-confirmations+1))
                if row['label']=='negative':negatives+=1;fp+=hit
                else:positives+=1;tp+=hit
            summary.append(dict(source=source,phase=phase,detected=tp,positive=positives,false_positive=fp,negative=negatives))
    report['historical_validation']=summary
    (OUT/'model-cadence-v2.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('C++ DSP model check:',report['clips'],'clips; all decisions match; max probability error',report['max_probability_error'])

if __name__=='__main__':main()
