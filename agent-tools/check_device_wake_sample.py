"""Browser checks for the device recorder UI. All device requests/audio are mocked."""
import asyncio
import base64
import io
import json
import subprocess
import tempfile
import threading
import time
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from check_wakeword_recorder import CHROME, browser_result
from preview_webui import preview

ROOT = Path(__file__).resolve().parents[1]
CHECKS = r"""<script>
(async()=>{
 const checks=[];window.recorderProgress=checks;function check(name,ok){checks.push({name,ok:!!ok});if(!ok)throw Error(name);}
 let micCalls=0;Object.defineProperty(navigator.mediaDevices,'getUserMedia',{value:()=>{micCalls++;throw Error('Must use the Bender microphone');}});
 let id=0,state=0,reads=0,label='hey_bender',split='train',posts=[],mode='normal',requested=[],pool=[];
 let diag={enabled:false,ready:false,listening:false,detections:0,score:0,processing_us:0,dropped:0,error:0,follow_up_seconds:8},diagPosts=[];
 function status(){return {id,state,ms:state===3?4000:500,label,split};}
 function wav(){const b=new ArrayBuffer(192044),v=new DataView(b);const txt=(p,s)=>[...s].forEach((c,i)=>v.setUint8(p+i,c.charCodeAt(0)));txt(0,'RIFF');v.setUint32(4,192036,true);txt(8,'WAVEfmt ');v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,24000,true);v.setUint32(28,48000,true);v.setUint16(32,2,true);v.setUint16(34,16,true);txt(36,'data');v.setUint32(40,192000,true);for(let i=0;i<96000;i++)v.setInt16(44+i*2,Math.round(3000*(Math.sin(2*Math.PI*1000*i/24000)+Math.sin(2*Math.PI*11000*i/24000))),true);return new Blob([b],{type:'audio/wav'});}
 window.fetch=async(url,options={})=>{
  requested.push(url);
  if(!url.startsWith('/wakeword/'))throw Error('Unexpected request '+url);
  if(url==='/wakeword/diagnostic'){
   if(options.method==='POST'){
    check('diagnostic request header',options.headers['X-Bender-Record']==='1');
    const p=new URLSearchParams(options.body);
    if(p.get('action')==='settings'){diag.follow_up_seconds=Number(p.get('follow_up_seconds'));}
    else {diagPosts.push(p.get('enabled'));diag.enabled=p.get('enabled')==='1';diag.diagnostic=p.get('mode')!=='voice';}
   }
   if(mode==='diag-fail')return {ok:false,text:async()=>'Ошибка детектора'};
   return {ok:true,json:async()=>({...diag})};
  }
  if(url==='/wakeword/samples')return {ok:true,json:async()=>({capacity:30,available:30-pool.length,items:pool})};
  if(options.method==='POST'){
   check('local microphone request header',options.headers['X-Bender-Record']==='1');
   const p=new URLSearchParams(options.body);posts.push(p);
   if(p.get('action')==='start'){if(mode==='busy')return {ok:false,text:async()=>'Поставь музыку на паузу.'};id++;state=1;reads=0;label=p.get('label');split=p.get('split');}
   else if(p.get('action')==='clear'){pool=[];state=0;}
   else {pool=pool.filter(s=>s.id!==Number(p.get('id')));if(Number(p.get('id'))===id)state=0;}
  }else if(url.startsWith('/wakeword/audio.wav'))return {ok:true,blob:async()=>mode==='truncated'?new Blob(['bad']):wav()};
  else if(state===1||state===2){reads++;state=mode==='cancel'?2:reads>=2?3:2;}
  if(state===3&&!pool.some(s=>s.id===id))pool.push({...status()});
  return {ok:true,json:async()=>status()};
 };
 try{
  location.hash='settings';showPanel();document.getElementById('wake-sample-section').open=true;
  check('no capture on opening settings',posts.length===0&&micCalls===0);
  await new Promise(resolve=>setTimeout(resolve,30));await wakeDiagnostic(true);
  check('enable local diagnostic',diag.enabled&&diagPosts.at(-1)==='1'&&document.getElementById('wake-diag-start').disabled);
  diag.ready=true;diag.listening=true;diag.detections=3;diag.score=.75;diag.processing_us=12000;await wakeDiagnostic();
  check('diagnostic live stats',document.getElementById('wake-diag-status').textContent.includes('Обращений: 3')&&document.getElementById('wake-diag-status').textContent.includes('12.0 мс'));
  diag.decision_ms=200;diag.processing_us=140000;await wakeDiagnostic();check('diagnostic processing budget',document.getElementById('wake-diag-status').textContent.includes('140.0 мс / 200 мс')&&!document.getElementById('wake-diag-status').textContent.includes('не успевает'));
  diag.processing_us=210000;await wakeDiagnostic();check('diagnostic overrun warning',document.getElementById('wake-diag-status').textContent.includes('не успевает'));diag.processing_us=140000;
  diag.listening=false;await wakeDiagnostic();check('diagnostic pause shown',document.getElementById('wake-diag-status').textContent.includes('Пауза'));
  await wakeDiagnostic(false);check('diagnostic stop',!diag.enabled&&diagPosts.at(-1)==='0'&&!document.getElementById('wake-diag-start').disabled);
  await wakeDiagnostic(true,'voice');check('voice mode explicit',diag.enabled&&diag.diagnostic===false&&document.getElementById('wake-voice-start').disabled&&!document.getElementById('wake-diag-start').disabled);
  diag.voice_state=1;await wakeDiagnostic();check('voice connecting status',document.getElementById('wake-diag-status').textContent.includes('Готовлюсь слушать'));
  diag.voice_state=2;await wakeDiagnostic();check('voice question status',document.getElementById('wake-diag-status').textContent.includes('Слушаю вопрос'));
  diag.voice_state=3;await wakeDiagnostic();check('voice answer status',document.getElementById('wake-diag-status').textContent.includes('Готовлю ответ'));
  diag.voice_state=0;await wakeDiagnostic(true);check('back to diagnostic',diag.diagnostic===true&&!document.getElementById('wake-voice-start').disabled);
  await wakeDiagnostic(false);
  const follow=document.getElementById('wake-follow-up');
  follow.value='20';follow.dispatchEvent(new Event('input'));await wakeDiagnostic();
  check('poll does not erase edited follow-up',follow.value==='20'&&document.getElementById('wake-follow-value').textContent==='20 с');
  await wakeDiagnostic(undefined,'settings',20);check('follow-up save independent of enable',diag.follow_up_seconds===20&&!diag.enabled&&!wakeFollowDirty);
  await wakeDiagnostic(true,'voice');follow.value='0';follow.dispatchEvent(new Event('input'));
  await wakeDiagnostic(undefined,'settings',0);check('follow-up can be disabled during voice mode',diag.enabled&&diag.follow_up_seconds===0&&document.getElementById('wake-follow-value').textContent==='выключено');
  await wakeDiagnostic(false);
  mode='diag-fail';await wakeDiagnostic();check('diagnostic network error recoverable',!document.getElementById('wake-diag-stop').disabled);mode='normal';
  sampleEl('label').value='privet_bender';sampleEl('split').value='test';await sampleAction(true);
  check('selected phrase and split sent',posts[0].get('label')==='privet_bender'&&posts[0].get('split')==='test');
  check('preview and download ready',!sampleEl('result').hidden&&sampleUrl&&sampleEl('download').download.includes('privet_bender_test'));
  const converted=await sampleTrainingWav(wav()),bytes=await converted.blob.arrayBuffer(),v=new DataView(bytes);
  check('16k mono PCM four seconds',bytes.byteLength===128044&&v.getUint32(24,true)===16000&&v.getUint16(22,true)===1&&v.getUint16(34,true)===16);
  function magnitude(hz){let re=0,im=0;for(let i=1000;i<63000;i++){const x=v.getInt16(44+i*2,true);re+=x*Math.cos(2*Math.PI*hz*i/16000);im+=x*Math.sin(2*Math.PI*hz*i/16000);}return Math.hypot(re,im);}
  check('resampler suppresses aliasing',magnitude(5000)<magnitude(1000)*.15);
  check('no PC microphone or AI endpoint',micCalls===0&&requested.every(p=>p.startsWith('/wakeword/')));
  sampleEl('discard').click();await new Promise(r=>setTimeout(r,40));check('delete clears preview',sampleEl('result').hidden&&!sampleUrl&&state===0);
  mode='busy';await sampleAction(true);check('busy rejection shown and retry enabled',sampleEl('status').textContent.includes('паузу')&&!sampleEl('start').disabled);
  mode='cancel';const pending=sampleAction(true);await new Promise(r=>setTimeout(r,40));sampleEl('cancel').click();await pending;
  check('cancel finishes and sends capture ID',!sampleBusy&&state===0&&posts.at(-1).get('id')===String(id));
  mode='truncated';await sampleAction(true);check('truncated WAV never downloadable',!sampleUrl&&sampleEl('result').hidden&&sampleEl('status').textContent.includes('не полностью'));
  mode='normal';state=3;await sampleAction(false);check('refresh recovers last example',!sampleEl('result').hidden&&!!sampleUrl);
  check('form fields remain independent',!sampleEl('label').form&&!sampleEl('split').form);
  const oldId=id;sampleEl('label').value='bender';await sampleAction(true);await sampleAction(true);
  check('bare name label survives recording and list',pool.at(-1).label==='bender'&&sampleEl('download').download.includes('bender_test')&&sampleEl('list').textContent.includes('Бендер · проверка'));
  check('new recordings preserve older ones',pool.length===3&&pool[0].id===oldId&&sampleEl('list').children.length===3);
  await sampleSelect(pool[0]);check('older recording can be previewed',sampleId===oldId&&!!sampleUrl);
  const beforeExport=posts.length;await sampleExport();
  check('batch download ready without deletion',sampleArchiveUrl&&!sampleEl('archive').hidden&&posts.length===beforeExport&&pool.length===3);
  const zip=sampleZip([{name:'example.txt',data:new TextEncoder().encode('hello')},{name:'test/second.txt',data:new TextEncoder().encode('second record')}]);
  const zipData=new Uint8Array(await zip.arrayBuffer());let encoded='';for(const b of zipData)encoded+=String.fromCharCode(b);
  checks.push({name:'ZIP format fixture',ok:true,archive_base64:btoa(encoded)});
  mode='truncated';await sampleExport();check('failed batch never offers partial archive',!sampleArchiveUrl&&sampleEl('archive').hidden&&pool.length===3);
  mode='normal';await sampleDelete(oldId);check('delete old record preserves latest',pool.length===2&&pool.every(s=>s.id!==oldId)&&state===3);
  const savedPool=pool.slice();window.confirm=()=>true;sampleEl('clear-pool').click();await new Promise(r=>setTimeout(r,80));
  check('explicit clear frees entire pool',pool.length===0&&sampleEl('list').children.length===0&&sampleEl('export').disabled);
  pool=savedPool;await samplePoolRefresh();
  document.querySelectorAll('#panel-settings>details').forEach(d=>d.open=d.id==='wake-sample-section');
  document.getElementById('wake-sample-section').scrollIntoView();
 }catch(e){checks.push({name:e.message,ok:false});}
 const p=document.createElement('pre');p.hidden=true;p.id='recorder-checks';p.textContent=JSON.stringify(checks);document.body.append(p);
})();
</script>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        page = (ROOT / "artifacts/webui-preview/index.html").read_text(encoding="utf-8")
        body = page.replace("</body>", CHECKS + "</body>").encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    preview()
    with tempfile.TemporaryDirectory(prefix="bender-device-recorder-") as tmp:
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        out = ROOT / "artifacts/wake-sample-preview"
        out.mkdir(parents=True, exist_ok=True)
        process = subprocess.Popen([CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
                                    "--user-data-dir=" + tmp, "--remote-debugging-port=0",
                                    "--window-size=1100,1400", f"http://127.0.0.1:{server.server_port}"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            portfile = Path(tmp) / "DevToolsActivePort"
            deadline = time.monotonic() + 10
            while not portfile.exists() and time.monotonic() < deadline:
                time.sleep(.1)
            port = portfile.read_text().splitlines()[0]
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=5) as reply:
                page = next(p for p in json.load(reply) if p["type"] == "page")
            checks = asyncio.run(browser_result(page["webSocketDebuggerUrl"], out))
            for check in checks:
                encoded = check.pop('archive_base64', None)
                if encoded:
                    with zipfile.ZipFile(io.BytesIO(base64.b64decode(encoded))) as archive:
                        assert archive.testzip() is None
                        assert archive.read('example.txt') == b'hello'
                        assert archive.read('test/second.txt') == b'second record'
            (out / "checks.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2), encoding="utf-8")
            if not all(c["ok"] for c in checks):
                raise AssertionError(checks)
            print(f"{len(checks)} device-recorder browser checks passed")
        finally:
            process.terminate()
            process.wait(timeout=10)
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
