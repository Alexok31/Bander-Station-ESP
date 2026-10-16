"""Check one complete local character-preview exchange without the device.

Uses a synthetic question and discards the returned PCM. Never prints keys.
"""
import asyncio
import base64
import json
from pathlib import Path
import sys
from time import perf_counter

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "BendeRadio/examples/BenderRealtime/local_server"))
import server
import websockets


async def main():
    token = base64.b64encode(f"{server.WS_USER}:{server.WS_PASS}".encode()).decode()
    profile = dict(sarcasm=65, sociability=65, curiosity=30, stubbornness=40,
                   warmth=30, roughness=45, profanity=0)
    started = perf_counter()
    audio_bytes = 0
    result = None
    done = False
    async with websockets.connect(
        "ws://127.0.0.1:8765/v1/realtime",
        additional_headers={"Authorization": "Basic " + token},
        open_timeout=5,
    ) as ws:
        await ws.send(json.dumps({"type": "character.preview", "request_id": 1,
                                  "llm_model": "grok-4.3", "character": profile,
                                  "question": "Бендере, як справи?"}, ensure_ascii=False))
        while perf_counter() - started < 65:
            message = json.loads(await asyncio.wait_for(ws.recv(), timeout=65))
            kind = message.get("type")
            if kind == "response.output_audio.delta":
                audio_bytes += len(base64.b64decode(message.get("delta") or ""))
            elif kind == "character.preview.result":
                result = {"ok": message.get("ok"), "elapsed_ms": round((perf_counter()-started)*1000)}
            elif kind == "response.done":
                done = True
                break
    print(json.dumps({"result": result, "response_done": done,
                      "audio_seconds": round(audio_bytes / 48000, 2)}, ensure_ascii=False))


if __name__ == "__main__":
    asyncio.run(main())
