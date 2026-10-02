"""Headless Chrome checks against local sample HTML; never connects to Bender."""
from pathlib import Path
import html
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'artifacts/webui-preview'
CHROME = r'C:\Program Files\Google\Chrome\Application\chrome.exe'

def main():
    profile = tempfile.mkdtemp(prefix='bender-ui-check-')
    common = [CHROME, '--headless=new', '--disable-gpu', '--no-first-run',
              '--no-default-browser-check', '--allow-file-access-from-files',
              '--user-data-dir=' + profile, '--virtual-time-budget=1500']
    result = subprocess.run(common + ['--dump-dom', (OUT/'checks.html').as_uri()],
                            capture_output=True, timeout=60)
    dom = result.stdout.decode('utf-8')
    match = re.search(r'<pre id="ui-test-results">(.*?)</pre>', dom, re.S)
    if not match: raise RuntimeError('Browser did not return test results')
    checks = json.loads(html.unescape(match[1]))
    (OUT/'browser-checks.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2),encoding='utf-8')
    if not all(c['ok'] for c in checks): raise AssertionError(checks)
    print(f'{len(checks)} browser interaction checks passed')
    for name, size, url in [
        ('character-desktop', '1280,1000', (OUT/'index.html').as_uri()+'#character'),
        ('settings-desktop', '1280,1100', (OUT/'index.html').as_uri()+'#settings'),
    ]:
        subprocess.run(common + ['--window-size='+size, '--screenshot='+str(OUT/(name+'.png')),url],capture_output=True,timeout=60,check=True)
    mobile = '''<!doctype html><meta charset="utf-8"><style>body{margin:0;background:#111a1b}iframe{border:0;width:390px;height:1350px;display:block;margin:auto}</style><iframe src="index.html#settings" title="Мобильный предпросмотр"></iframe><script>document.querySelector('iframe').onload=function(){let d=this.contentDocument,w=this.contentWindow,results=[];for(let width of [320,390]){this.style.width=width+'px';for(let key of ['radio','settings','character']){w.location.hash=key;w.showPanel();d.querySelectorAll('details').forEach(e=>e.open=true);results.push({section:key,width:d.documentElement.clientWidth,scroll:d.documentElement.scrollWidth});}}this.style.width='390px';w.location.hash='settings';w.showPanel();d.querySelectorAll('details').forEach((e,i)=>e.open=i===0);let p=document.createElement('pre');p.id='layout-check';p.hidden=true;p.textContent=JSON.stringify(results);document.body.append(p);};</script>'''
    (OUT/'mobile.html').write_text(mobile,encoding='utf-8')
    result = subprocess.run(common + ['--window-size=430,1450', '--dump-dom', '--screenshot='+str(OUT/'settings-mobile.png'), (OUT/'mobile.html').as_uri()],capture_output=True,timeout=60,check=True)
    match = re.search(r'<pre id="layout-check"[^>]*>(.*?)</pre>',result.stdout.decode('utf-8'),re.S)
    if not match: raise RuntimeError('Missing mobile layout result')
    layout = json.loads(html.unescape(match[1]))
    if any(item['scroll'] > item['width'] for item in layout): raise AssertionError(layout)
    print(f'Mobile layout: {layout}')

if __name__ == '__main__': main()
