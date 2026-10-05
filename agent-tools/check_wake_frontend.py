"""Numerically compare the actual portable C++ DSP to the training frontend."""
import io
import json
import os
import subprocess
import wave
import zipfile
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly
from wake_training_audio import features, SAMPLES

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'artifacts/wake-runtime'
HOST=OUT/'wake_frontend_host.exe'

def main():
    OUT.mkdir(exist_ok=True,parents=True)
    zig=OUT/'tools/ziglang/zig.exe'
    env=dict(os.environ,ZIG_LOCAL_CACHE_DIR=str(OUT/'zig-cache'),ZIG_GLOBAL_CACHE_DIR=str(OUT/'zig-global-cache'))
    subprocess.run([str(zig),'c++','-std=c++17','-O2',str(ROOT/'tests/wake_frontend_host.cpp'),
                    '-o',str(HOST)],check=True,env=env)
    rng=np.random.default_rng(42)
    cases={'silence':np.zeros(96000,dtype=np.int16),
           'noise':rng.integers(-10000,10000,96000,dtype=np.int16),
           'tones':np.int16(5000*np.sin(2*np.pi*1000*np.arange(96000)/24000)+
                            5000*np.sin(2*np.pi*11000*np.arange(96000)/24000))}
    # Real held-out audio never changes weights/threshold; only verifies DSP parity.
    manifest=json.loads((ROOT/'artifacts/wake-training/dataset-v6/manifest.json').read_text(encoding='utf-8'))
    for source in manifest['sources']:
        if source['source']!='real' or source['split']!='test': continue
        with zipfile.ZipFile(ROOT/'artifacts/wakeword-device'/source['source_archive']) as archive:
            suffix='/'+source['id'].removeprefix('real_')+'.wav'
            member=next(n for n in archive.namelist() if n.endswith(suffix))
            with wave.open(io.BytesIO(archive.read(member))) as wav:
                assert wav.getframerate()==24000 and wav.getnchannels()==1 and wav.getsampwidth()==2
                cases[source['id']]=np.frombuffer(wav.readframes(wav.getnframes()),dtype='<i2')
    results=[]
    for name,pcm in cases.items():
        raw=OUT/(name+'.pcm'); dest=OUT/(name+'.features'); quant=OUT/(name+'.int8')
        # Flush the centered FIR's 15 native lookahead samples at end-of-file.
        np.pad(pcm,(0,15)).astype('<i2').tofile(raw)
        subprocess.run([str(HOST),str(raw),str(dest),str(quant)],check=True)
        actual=np.fromfile(dest,dtype=np.float32).reshape(-1,238,40,1)
        x=resample_poly(pcm.astype(np.float32)/32768,2,3).astype(np.float32)
        x=np.clip(np.round(x*32768),-32768,32767).astype(np.float32)/32768
        expected=[]
        for i in range(len(actual)):
            end=(i+1)*1600; clip=x[max(0,end-SAMPLES):end]
            expected.append(features(np.pad(clip,(SAMPLES-len(clip),0))))
        expected=np.stack(expected)
        delta=np.abs(actual-expected)
        scale=.022312475368380547
        qa=np.fromfile(quant,dtype=np.int8).reshape(expected.shape).astype(np.int16)
        qe=np.clip(np.rint(expected/scale+6),-128,127).astype(np.int16)
        result=dict(case=name,windows=len(actual),max_feature_error=float(delta.max()),
                    mean_feature_error=float(delta.mean()),max_quantized_error=int(np.abs(qa-qe).max()),
                    different_quantized_values=int(np.sum(qa!=qe)))
        results.append(result);print(result)
        assert delta.max()<.004 and np.abs(qa-qe).max()<=1,result
    (OUT/'frontend-parity.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
