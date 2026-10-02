"""Read-only local WebSocket health check; never records or generates speech."""
import asyncio
import argparse
import base64
import json
import os
from pathlib import Path
import re
import websockets

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT/'BendeRadio/examples/BenderRealtime/local_server'

def credential(name, macro):
    value = os.environ.get(name, '').strip()
    if value:
        return value
    for path in (HERE.parent.parent/'secrets.h', HERE.parent/'secrets.h'):
        if path.is_file():
            match = re.search(rf'#define\s+{re.escape(macro)}\s+"([^"]*)"', path.read_text(encoding='utf-8'))
            if match:
                return match[1]
    return ''

async def main(preview=False):
    user = credential('BENDER_WS_USER', 'LOCAL_WS_USER')
    password = credential('BENDER_WS_PASS', 'LOCAL_WS_PASS')
    auth = base64.b64encode(f'{user}:{password}'.encode()).decode()
    try:
        async with websockets.connect('ws://127.0.0.1:8765/v1/realtime',
                                      additional_headers={'Authorization': 'Basic '+auth},
                                      open_timeout=5, close_timeout=2) as ws:
            created = json.loads(await asyncio.wait_for(ws.recv(), 5))
            print('Handshake:', created.get('type'), flush=True)
            await ws.send(json.dumps({'type': 'ping'}))
            reply = json.loads(await asyncio.wait_for(ws.recv(), 5))
            print('Application reply:', reply.get('type'), flush=True)
            if preview:
                for request_id in (9101, 9102):
                    print('Preview start:', request_id, flush=True)
                    await ws.send(json.dumps({'type': 'character.preview', 'request_id': request_id,
                                              'character': dict(sarcasm=20, sociability=10, curiosity=0,
                                                                stubbornness=0, warmth=70, roughness=0, profanity=0),
                                              'question': 'Бендере, скажи одне коротке привітання.'}))
                    frames = 0
                    async with asyncio.timeout(65):
                        while True:
                            event = json.loads(await ws.recv())
                            kind = event.get('type')
                            if kind == 'response.output_audio.delta':
                                frames += 1  # Discard sound; never play or save it.
                                if frames == 1:
                                    print('First audio:', request_id, flush=True)
                            elif kind == 'character.preview.result':
                                print('Preview result:', event.get('request_id'), event.get('ok'), flush=True)
                            elif kind == 'response.done':
                                print('Preview done:', request_id, 'audio frames:', frames, flush=True)
                                break
                    await ws.send(json.dumps({'type': 'ping'}))
                    reply = json.loads(await asyncio.wait_for(ws.recv(), 5))
                    print('After preview:', reply.get('type'), flush=True)
    except Exception as error:
        print('Connection probe:', type(error).__name__, flush=True)
        raise SystemExit(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview', action='store_true', help='Run two short model/TTS requests, discard audio')
    args = parser.parse_args()
    asyncio.run(main(args.preview))
