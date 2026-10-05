"""Fetch the exact two Piper voices used for this prototype; verify SHA-256."""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
SOURCES=Path(__file__).with_name('wake-voice-sources.json')


def main():
    folder=ROOT/'artifacts/wake-training/voices';folder.mkdir(parents=True,exist_ok=True)
    sources=json.loads(SOURCES.read_text(encoding='utf-8'))
    for item in sources:
        target=folder/item['file']
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest()!=item['sha256']:
                raise ValueError(f'Existing voice checksum differs: {target.name}')
            continue
        partial=target.with_suffix(target.suffix+'.partial')
        with urllib.request.urlopen(item['url'],timeout=60) as response,partial.open('wb') as output:
            while chunk:=response.read(1024*1024):output.write(chunk)
        if hashlib.sha256(partial.read_bytes()).hexdigest()!=item['sha256']:
            raise ValueError(f'Upstream voice changed: {target.name}. Review source before updating lock.')
        partial.rename(target)
    (folder/'sources.json').write_text(json.dumps(sources,indent=2),encoding='utf-8')
    print('Two voice models/configs/cards verified against SHA-256.')


if __name__=='__main__':main()
