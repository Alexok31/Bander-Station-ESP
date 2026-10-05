"""Exercise the recorder in Chrome with synthetic audio; never open a real mic."""
import asyncio
import base64
import json
import subprocess
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

import websockets

from wakeword_recorder import RecorderHandler, RecorderServer

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
ROOT = Path(__file__).resolve().parents[1]
CHECKS = r"""<script>
(async()=>{
 const checks=[];const check=(name,ok)=>{checks.push({name,ok:!!ok});if(!ok)throw Error(name);};
 let opened=0,stopped=0;
 const sleep=ms=>new Promise(r=>setTimeout(r,ms));
 async function settled(){for(let i=0;i<1000&&busy;i++)await sleep(20);check('operation finished',!busy);}
 Object.defineProperty(navigator.mediaDevices,'getUserMedia',{configurable:true,value:async()=>{opened++;return {getTracks:()=>[{stop:()=>stopped++}]};}});
 window.MediaRecorder=class{constructor(){this.mimeType='audio/wav';this.state='inactive';}start(){this.state='recording';}stop(){this.state='inactive';const samples=Float32Array.from({length:64000},(_,i)=>.1*Math.sin(2*Math.PI*440*i/16000));this.ondataavailable({data:encodeWav(samples)});this.onstop();}};
 try{
  check('no microphone on page load',opened===0);
  check('save disabled before recording',el('save').disabled);
  for(const label of ['hey_bender','privet_bender']){
   el('label').value=label;el('label').onchange();el('record').click();await settled();
   check('preview available '+label,!el('playback').hidden&&!!wavBlob);
   check('microphone stopped '+label,opened===stopped);
   el('save').click();await settled();check('clip cleared after saving '+label,!wavBlob);
  }
  const r=await fetch('/counts');const counts=await r.json();
  check('both examples saved',counts.train.hey_bender===1&&counts.train.privet_bender===1);
  check('held-out set untouched',Object.values(counts.test).every(n=>n===0));
  Object.defineProperty(navigator.mediaDevices,'getUserMedia',{value:async()=>{throw new DOMException('denied','NotAllowedError');}});
  el('record').click();await settled();check('permission error visible',el('status').textContent.includes('не разрешён'));
  check('can retry after error',!el('record').disabled);
 }catch(e){checks.push({name:e.message,ok:false});}
 const pre=document.createElement('pre');pre.id='recorder-checks';pre.hidden=true;pre.textContent=JSON.stringify(checks);document.body.append(pre);
})();
</script>"""


class CheckHandler(RecorderHandler):
    def do_GET(self):
        if self.path == "/":
            page = Path(__file__).with_name("wakeword-recorder.html").read_text(encoding="utf-8")
            return self.reply(200, page.replace("__TOKEN__", self.server.token).replace("</html>", CHECKS + "</html>"), "text/html; charset=utf-8")
        super().do_GET()


def main():
    with tempfile.TemporaryDirectory(prefix="bender-wakeword-check-") as tmp:
        server = RecorderServer(0, Path(tmp) / "samples")
        server.RequestHandlerClass = CheckHandler
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            out = ROOT / "artifacts/wakeword-preview"
            out.mkdir(parents=True, exist_ok=True)
            profile = Path(tmp) / "chrome"
            command = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                       "--autoplay-policy=no-user-gesture-required", "--user-data-dir=" + str(Path(tmp) / "chrome"),
                       "--remote-debugging-port=0", "--window-size=1000,1100", server.origin]
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                active_port = profile / "DevToolsActivePort"
                while not active_port.exists() and time.monotonic() < deadline:
                    time.sleep(.1)
                port = active_port.read_text().splitlines()[0]
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=5) as response:
                    pages = json.load(response)
                page = next(p for p in pages if p["type"] == "page")
                checks = asyncio.run(browser_result(page["webSocketDebuggerUrl"], out))
            finally:
                process.terminate()
                process.wait(timeout=10)
            (out / "checks.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2), encoding="utf-8")
            if not all(c["ok"] for c in checks):
                raise AssertionError(checks)
            print(f"{len(checks)} browser checks passed; synthetic samples stored only in temporary directory")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


async def browser_result(url, out):
    async with websockets.connect(url, max_size=8 * 1024 * 1024) as ws:
        async def rpc(number, method, params):
            await ws.send(json.dumps({"id": number, "method": method, "params": params}))
            while True:
                result = json.loads(await asyncio.wait_for(ws.recv(), 35))
                if result.get("id") == number:
                    if "error" in result:
                        raise RuntimeError(result["error"])
                    return result["result"]
        result = await rpc(1, "Runtime.evaluate", {
            "expression": "new Promise((resolve,reject)=>{const t=setInterval(()=>{const e=document.getElementById('recorder-checks');if(e){clearInterval(t);resolve(e.textContent);}},100);setTimeout(()=>{clearInterval(t);reject(Error('checks timeout'));},30000);})",
            "awaitPromise": True, "returnByValue": True})
        if "exceptionDetails" in result:
            diagnostic = await rpc(3, "Runtime.evaluate", {
                "expression": "JSON.stringify({url:location.href,state:document.readyState,progress:window.recorderProgress,body:document.body?.innerText?.slice(-2000)})",
                "returnByValue": True})
            raise RuntimeError({"exception":result["exceptionDetails"],"page":diagnostic})
        screenshot = await rpc(2, "Page.captureScreenshot", {"format": "png"})
        (out / "recorder.png").write_bytes(base64.b64decode(screenshot["data"]))
        return json.loads(result["result"]["value"])


if __name__ == "__main__":
    main()
