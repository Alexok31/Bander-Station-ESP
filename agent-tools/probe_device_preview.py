"""Explicit device preview probe. Plays a short greeting, does not save settings."""
import json
import argparse
import time
import urllib.request
import urllib.parse
from urllib.error import HTTPError

URL = 'http://192.168.0.174/character/preview'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--play', action='store_true', help='Play up to two short greetings on Bender')
if not parser.parse_args().play:
    parser.error('--play is required; no requests sent')
values = dict(sarcasm=20, sociability=10, curiosity=0, stubbornness=0,
              warmth=70, roughness=0, profanity=0,
              question='Бендере, скажи одне коротке привітання.')
for attempt in (1, 2):
    print('Attempt:', attempt, flush=True)
    try:
        request = urllib.request.Request(URL, urllib.parse.urlencode(values).encode())
        with urllib.request.urlopen(request, timeout=8) as response:
            print('Accepted:', response.status, response.read(1000).decode(), flush=True)
    except HTTPError as error:
        print('Rejected:', error.code, error.read(1000).decode(), flush=True)
        break
    deadline = time.monotonic() + 65
    previous = None
    while time.monotonic() < deadline:
        with urllib.request.urlopen(URL, timeout=8) as response:
            result = json.load(response)
        if result != previous:
            print('State:', result, flush=True)
            previous = result
        if result.get('state') in (4, 5):
            break
        time.sleep(1)
    if previous.get('state') != 4:
        break
    if attempt == 1:
        time.sleep(10)  # Include the device's normal 8-second AI sleep/reconnect.
