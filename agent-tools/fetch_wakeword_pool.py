"""Download the Bender RAM pool as one verified ZIP. Never deletes device data.

CLI export preserves the original PCM16/mono/24 kHz WAVs. The WebUI ZIP is
resampled to 16 kHz instead. Source rate is explicit in the manifest.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
from datetime import datetime, timezone
import urllib.request
import wave
import zipfile

LABELS = ("hey_bender", "privet_bender", "negative", "bender")
SPLITS = ("train", "test")
WAV_BYTES = 192044


def validate_items(data):
    items = data.get("items") if isinstance(data, dict) else None
    if not isinstance(items, list) or not 1 <= len(items) <= 30:
        raise ValueError("Pool is empty or the manifest is invalid")
    ids = set()
    for item in items:
        if not isinstance(item, dict):
            raise ValueError("Invalid record")
        ident = item.get("id")
        if type(ident) is not int or not 0 < ident <= 0xffffffff or ident in ids:
            raise ValueError("Invalid or duplicate sample ID")
        if item.get("label") not in LABELS or item.get("split") not in SPLITS:
            raise ValueError("Invalid sample label/split")
        ids.add(ident)
    return items


def validate_wav(body):
    if len(body) != WAV_BYTES:
        raise ValueError("Incomplete WAV download")
    with wave.open(io.BytesIO(body), "rb") as audio:
        if (audio.getframerate(), audio.getnchannels(), audio.getsampwidth(),
                audio.getnframes(), audio.getcomptype()) != (24000, 1, 2, 96000, "NONE"):
            raise ValueError("Unexpected WAV format")
        if len(audio.readframes(96000)) != 192000:
            raise ValueError("Truncated PCM data")


def fetch_pool(base, destination):
    destination = Path(destination)
    if destination.exists():
        raise FileExistsError(destination)
    # Device access is direct, not through an environment-configured proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def get(path, maximum):
        with opener.open(base.rstrip("/") + path, timeout=15) as reply:
            body = reply.read(maximum + 1)
        if len(body) > maximum:
            raise ValueError("Device response is too large")
        return body

    items = validate_items(json.loads(get("/wakeword/samples", 32768)))
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".partial")
    manifest = []
    # Exclusive creation also prevents two downloads from overwriting each other.
    with partial.open("xb") as handle:
        with zipfile.ZipFile(handle, "w", compression=zipfile.ZIP_STORED) as archive:
            for item in items:
                body = get("/wakeword/audio.wav?id=" + str(item["id"]), WAV_BYTES)
                validate_wav(body)
                name = f'{item["split"]}/{item["label"]}/{item["id"]}.wav'
                archive.writestr(name, body)
                manifest.append(dict(item, file=name, rate=24000, sha256=hashlib.sha256(body).hexdigest()))
                print(f'Saved {len(manifest)}/{len(items)}: {name}')
            archive.writestr("manifest.json", json.dumps({"version": 1,
                "source": "bender-microphone", "exported_at": datetime.now(timezone.utc).isoformat(),
                "items": manifest}, ensure_ascii=False, indent=2))
    with zipfile.ZipFile(partial) as archive:
        if archive.testzip() is not None:
            raise ValueError("Archive integrity check failed")
    # Windows rename fails if a destination appeared concurrently; never overwrite.
    os.rename(partial, destination)
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="http://192.168.0.174")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] /
        "artifacts" / "wakeword-device" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + ".zip"))
    args = parser.parse_args()
    print(fetch_pool(args.device, args.output))
