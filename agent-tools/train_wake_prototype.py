"""Train/export a small INT8 keyword prototype and evaluate sliding windows.

No production server, firmware, or remote service is modified. This model uses
our explicit log-mel frontend, NOT ESPHome/microWakeWord's feature contract.
"""
import os
os.environ.setdefault('TF_USE_LEGACY_KERAS','1')
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL','2')
os.environ.setdefault('CUDA_VISIBLE_DEVICES','-1')
os.environ.setdefault('OMP_NUM_THREADS','4')
import argparse
import hashlib
import json
from pathlib import Path
import time
import numpy as np
import tensorflow as tf
from wake_training_audio import FRAMES, MELS, RATE, SAMPLES, FRONTEND, features, load_wav

ROOT=Path(__file__).resolve().parents[1]
DATA=ROOT/'artifacts/wake-training/dataset-v1'


def build_model(temporal_head=False):
    layers=tf.keras.layers
    seeds=iter(range(104,120))
    initializer=lambda:tf.keras.initializers.GlorotUniform(seed=next(seeds))
    inputs=layers.Input((FRAMES,MELS,1),name='logmel')
    x=layers.Conv2D(12,(5,5),strides=(2,2),padding='same',use_bias=False,kernel_initializer=initializer())(inputs)
    x=layers.ReLU()(layers.BatchNormalization()(x))
    for channels,kernel,stride in [(20,(5,3),(2,2)),(32,(7,3),(2,2)),(48,(9,3),(1,1))]:
        x=layers.DepthwiseConv2D(kernel,strides=stride,padding='same',use_bias=False,depthwise_initializer=initializer())(x)
        x=layers.ReLU()(layers.BatchNormalization()(x))
        x=layers.Conv2D(channels,1,use_bias=False,kernel_initializer=initializer())(x)
        x=layers.ReLU()(layers.BatchNormalization()(x))
    if temporal_head:
        # Retain coarse phoneme order and frequency position. The baseline
        # averages the entire utterance into one vector, discarding both.
        x=layers.AveragePooling2D(pool_size=(4,1))(x)
        x=layers.Flatten()(x)
    else:
        x=layers.GlobalAveragePooling2D()(x)
    x=layers.Dropout(.2,seed=20261004)(x)
    x=layers.Dense(24,activation='relu',kernel_initializer=initializer())(x)
    output=layers.Dense(1,activation='sigmoid',name='wake_probability',kernel_initializer=initializer())(x)
    model=tf.keras.Model(inputs,output,name='bender_keyword_prototype')
    model.compile(optimizer=tf.keras.optimizers.Adam(.0015),loss='binary_crossentropy',
                  metrics=[tf.keras.metrics.BinaryAccuracy(name='accuracy')])
    return model


def sliding_features(x):
    out=[]
    for end in range(RATE//10,len(x)+RATE//10,RATE//10):
        end=min(end,len(x)); chunk=x[max(0,end-SAMPLES):end]
        window=np.zeros(SAMPLES,np.float32);window[-len(chunk):]=chunk
        out.append(features(window))
    return np.asarray(out,np.float32)


class Quantized:
    def __init__(self,path):
        self.interpreter=tf.lite.Interpreter(model_path=str(path),num_threads=2)
        self.interpreter.allocate_tensors()
        self.input=self.interpreter.get_input_details()[0]
        self.output=self.interpreter.get_output_details()[0]
        assert self.input['dtype']==np.int8 and self.output['dtype']==np.int8
    def predict(self,windows):
        scale,zero=self.input['quantization']; out_scale,out_zero=self.output['quantization']
        results=[]
        for window in windows:
            quant=np.clip(np.rint(window/scale+zero),-128,127).astype(np.int8)[None]
            self.interpreter.set_tensor(self.input['index'],quant)
            self.interpreter.invoke()
            value=self.interpreter.get_tensor(self.output['index']).astype(np.float32)
            results.append(float((value[0,0]-out_zero)*out_scale))
        return np.asarray(results)


def stable_score(probabilities):
    # Require three successive 100 ms decisions; suppress isolated peaks.
    if len(probabilities)<3: return 0.
    return float(np.max(np.minimum(np.minimum(probabilities[:-2],probabilities[1:-1]),probabilities[2:])))


def metrics(rows,threshold):
    positive=[r for r in rows if r['label']!='negative'];negative=[r for r in rows if r['label']=='negative']
    return dict(positive=len(positive),detected=sum(r['score']>=threshold for r in positive),
                negative=len(negative),false_positive_clips=sum(r['score']>=threshold for r in negative),
                negative_seconds=round(sum(r['seconds'] for r in negative),2),
                per_phrase={label:dict(total=sum(r['label']==label for r in positive),
                    detected=sum(r['label']==label and r['score']>=threshold for r in positive))
                    for label in ('hey_bender','privet_bender','bender')})


def ranking_auc(rows):
    pos=np.asarray([r['score'] for r in rows if r['label']!='negative'])
    neg=np.asarray([r['score'] for r in rows if r['label']=='negative'])
    differences=pos[:,None]-neg[None,:]
    return float(np.mean((differences>0)+.5*(differences==0)))


def evaluate(model,quant,manifest):
    rows=[]; max_error=0.
    validation=[r for r in manifest['sources'] if r['split']=='validation']
    for number,record in enumerate(validation):
        x=load_wav((DATA/record['audio']).read_bytes())
        windows=sliding_features(x)
        q=quant.predict(windows)
        f=model.predict(windows,batch_size=64,verbose=0).reshape(-1)
        max_error=max(max_error,float(np.max(np.abs(q-f))))
        rows.append(dict(id=record['id'],source=record['source'],label=record['label'],seconds=len(x)/RATE,
                         score=stable_score(q),float_score=stable_score(f),probabilities=q.tolist()))
        if (number+1)%10==0: print(f'Validated stream {number+1}/{len(validation)}',flush=True)
    # A development operating point, chosen on this validation set, not an unseen test.
    negative_scores=[r['score'] for r in rows if r['label']=='negative']
    candidates=[t/256 for t in range(64,256)]
    acceptable=[t for t in candidates if
        metrics([r for r in rows if r['source']=='real'],t)['detected']>=8 and
        metrics([r for r in rows if r['source']=='synthetic'],t)['detected']>=24 and
        not any(r['score']>=t for r in rows if r['label']=='negative')]
    # Never disguise a disabled detector (threshold 1.0) as a successful model.
    recommended=acceptable[0] if acceptable else None
    threshold=recommended if recommended is not None else .5
    real=metrics([r for r in rows if r['source']=='real'],threshold)
    synthetic=metrics([r for r in rows if r['source']=='synthetic'],threshold)
    return dict(threshold=threshold,recommended_threshold=recommended,
                development_gate_passed=recommended is not None,
                threshold_source='development validation; 0.5 diagnostic only when gate fails',
                real_validation=real,synthetic_validation=synthetic,max_float_int8_probability_error=max_error,
                sweep=[dict(threshold=t,real=metrics([r for r in rows if r['source']=='real'],t),
                            synthetic=metrics([r for r in rows if r['source']=='synthetic'],t))
                       for t in (.5,.6,.7,.8,.9,.95,.98,.995)],rows=rows)


def main():
    global DATA
    parser=argparse.ArgumentParser();parser.add_argument('--epochs',type=int,default=35)
    parser.add_argument('--run',default='prototype-v2');parser.add_argument('--dataset',default='dataset-v2')
    parser.add_argument('--stream-selection',action='store_true')
    parser.add_argument('--temporal-head',action='store_true')
    parser.add_argument('--initial-run',help='Fine-tune an existing compatible checkpoint')
    parser.add_argument('--learning-rate',type=float,default=.0015)
    parser.add_argument('--freeze-batchnorm',action='store_true')
    args=parser.parse_args()
    DATA=ROOT/'artifacts/wake-training'/args.dataset
    out=ROOT/'artifacts/wake-training'/args.run
    if out.exists() and any(out.iterdir()):
        raise FileExistsError('Use a new --run name to preserve earlier experiments')
    out.mkdir(parents=True,exist_ok=True)
    tf.config.threading.set_intra_op_parallelism_threads(4)
    tf.config.threading.set_inter_op_parallelism_threads(1)
    tf.keras.utils.set_random_seed(20261004)
    manifest=json.loads((DATA/'manifest.json').read_text(encoding='utf-8'))
    data=np.load(DATA/'features.npz',allow_pickle=False)
    assert not(set(data['groups'])&set(data['val_ids']))
    # NPZ decompresses on every indexing operation: materialize once, especially
    # before the representative-data generator used hundreds of times by LiteRT.
    train_x,train_y=data['train'],data['labels']
    val_x,val_y=data['validation'],data['val_labels']
    model=build_model(args.temporal_head)
    initial_sha256=None
    if args.initial_run:
        initial=ROOT/'artifacts/wake-training'/args.initial_run
        initial_report=json.loads((initial/'report.json').read_text(encoding='utf-8'))
        if initial_report.get('temporal_head',False)!=args.temporal_head:
            raise ValueError('Initial checkpoint uses a different architecture')
        initial_sha256=hashlib.sha256((initial/'best.weights.h5').read_bytes()).hexdigest()
        model.load_weights(str(initial/'best.weights.h5'))
    if args.freeze_batchnorm:
        for layer in model.layers:
            if isinstance(layer,tf.keras.layers.BatchNormalization):layer.trainable=False
    model.compile(optimizer=tf.keras.optimizers.Adam(args.learning_rate),loss='binary_crossentropy',
                  metrics=[tf.keras.metrics.BinaryAccuracy(name='accuracy')])
    print(f'Parameters: {model.count_params()}, train windows: {len(train_x)}',flush=True)
    class Progress(tf.keras.callbacks.Callback):
        def on_epoch_end(self,epoch,logs=None):
            print(json.dumps(dict(epoch=epoch+1,**{k:round(float(v),5) for k,v in (logs or {}).items()})),flush=True)
    callbacks=[]
    monitor='val_loss'; mode='min'
    if args.stream_selection:
        validation_sources=[r for r in manifest['sources'] if r['split']=='validation']
        stream_x=[];slices=[];start=0
        for record in validation_sources:
            windows=sliding_features(load_wav((DATA/record['audio']).read_bytes()))
            stream_x.append(windows);slices.append((start,start+len(windows)));start+=len(windows)
        stream_x=np.concatenate(stream_x)
        class StreamValidation(tf.keras.callbacks.Callback):
            def on_epoch_end(self,epoch,logs=None):
                probabilities=self.model.predict(stream_x,batch_size=64,verbose=0).reshape(-1)
                rows=[dict(source=r['source'],label=r['label'],score=stable_score(probabilities[a:b]))
                      for r,(a,b) in zip(validation_sources,slices)]
                real_auc=ranking_auc([r for r in rows if r['source']=='real'])
                synth_auc=ranking_auc([r for r in rows if r['source']=='synthetic'])
                logs['val_stream_auc']=.75*real_auc+.25*synth_auc if epoch>=4 else 0.
                logs['real_stream_auc']=real_auc
        callbacks.append(StreamValidation());monitor='val_stream_auc';mode='max'
    callbacks += [Progress(),tf.keras.callbacks.ModelCheckpoint(str(out/'best.weights.h5'),monitor=monitor,mode=mode,save_best_only=True,save_weights_only=True),
               tf.keras.callbacks.ReduceLROnPlateau(monitor='val_loss',factor=.5,patience=3,min_lr=.00008),
               tf.keras.callbacks.EarlyStopping(monitor=monitor,mode=mode,patience=10,restore_best_weights=True)]
    started=time.monotonic()
    history=model.fit(train_x,train_y,batch_size=48,epochs=args.epochs,
                      validation_data=(val_x,val_y),class_weight={0:1.15,1:1.},
                      callbacks=callbacks,verbose=0)
    model.load_weights(str(out/'best.weights.h5'))
    model.save(str(out/'model.keras'))
    # Calibrate only on training windows, including silence/background.
    indexes=np.random.default_rng(123).choice(len(train_x),min(256,len(train_x)),replace=False)
    converter=tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations=[tf.lite.Optimize.DEFAULT]
    converter.representative_dataset=lambda:([train_x[i:i+1]] for i in indexes)
    converter.target_spec.supported_ops=[tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type=tf.int8;converter.inference_output_type=tf.int8
    blob=converter.convert();path=out/'bender.int8.tflite';path.write_bytes(blob)
    quant=Quantized(path)
    floats=[t['name'] for t in quant.interpreter.get_tensor_details() if np.issubdtype(t['dtype'],np.floating)]
    assert not floats, floats
    report=evaluate(model,quant,manifest)
    report.update(dataset=args.dataset,temporal_head=args.temporal_head,
                  initial_run=args.initial_run,initial_checkpoint_sha256=initial_sha256,
                  learning_rate=args.learning_rate,freeze_batchnorm=args.freeze_batchnorm,
                  training_code_sha256={name:hashlib.sha256((ROOT/'agent-tools'/name).read_bytes()).hexdigest()
                                        for name in ('train_wake_prototype.py','wake_training_audio.py')},
                  dataset_manifest_sha256=hashlib.sha256((DATA/'manifest.json').read_bytes()).hexdigest(),
                  model_bytes=len(blob),parameters=model.count_params(),elapsed_seconds=round(time.monotonic()-started,1),
                  epochs_run=len(history.history['loss']),independent_test_available=False,ready_for_device=False,
                  limitations=manifest['limitations']+['Only 20 seconds of real validation background: insufficient to establish false alarms per hour.',
                    'On-device memory, inference time, power use and AirPlay coexistence have not been measured.',
                    'Uses bender-logmel-v1; not a drop-in microWakeWord/WakeNet model.'])
    (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    (out/'history.json').write_text(json.dumps({k:[float(v) for v in values] for k,values in history.history.items()},indent=2),encoding='utf-8')
    config=dict(name='Bender experimental prototype',version=1,experimental=True,ready_for_device=False,
        phrases=['Эй, Бендер','Привет, Бендер','Бендер'],model='bender.int8.tflite',
        sha256=hashlib.sha256(blob).hexdigest(),frontend=FRONTEND,
        input_shape=quant.input['shape'].tolist(),input_quantization=list(quant.input['quantization']),
        output_quantization=list(quant.output['quantization']),threshold=report['recommended_threshold'],
        diagnostic_threshold=report['threshold'],
        decision_stride_ms=100,consecutive_hits=3,refractory_ms=2000,
        ops=sorted({op['op_name'] for op in quant.interpreter._get_ops_details() if op['op_name']!='DELEGATE'}))
    (out/'model.json').write_text(json.dumps(config,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('rows','sweep')},ensure_ascii=False,indent=2),flush=True)


if __name__=='__main__': main()
