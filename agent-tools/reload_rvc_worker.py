"""Reload only Bender's RVC child, then warm it on a private local WebSocket.

Empty input takes the server's local ASR-miss path: no LLM request, no saved
chat changes, no device commands. Audio is counted and discarded, never played.
"""
import argparse
import asyncio
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "BendeRadio/examples/BenderRealtime/local_server"


async def probe():
    import websockets
    from probe_bender_connection import credential
    user = credential("BENDER_WS_USER", "LOCAL_WS_USER")
    password = credential("BENDER_WS_PASS", "LOCAL_WS_PASS")
    auth = base64.b64encode(f"{user}:{password}".encode()).decode()
    rows = []
    async with websockets.connect("ws://127.0.0.1:8765/v1/realtime",
                                  additional_headers={"Authorization": "Basic " + auth},
                                  open_timeout=5, close_timeout=2) as ws:
        assert json.loads(await ws.recv())["type"] == "session.created"
        for repeat in range(2):
            began = time.perf_counter()
            await ws.send(json.dumps({"type": "response.create"}))
            frames, first_ms = 0, None
            async with asyncio.timeout(60):
                while True:
                    event = json.loads(await ws.recv())
                    if event["type"] == "response.output_audio.delta":
                        frames += 1
                        if first_ms is None:
                            first_ms = (time.perf_counter()-began)*1000
                    elif event["type"] == "response.done":
                        break
            if not frames:
                raise RuntimeError("No local speech returned")
            await ws.send(json.dumps({"type": "ping"}))
            assert json.loads(await asyncio.wait_for(ws.recv(), 5))["type"] == "pong"
            rows.append({"round": repeat, "first_pcm_ms": first_ms, "audio_frames": frames,
                         "pong_after_reply": True})
            print(json.dumps(rows[-1]), flush=True)
    path = ROOT / "artifacts/voice-latency-20261011/live-worker-check.json"
    path.write_text(json.dumps(rows, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-pid", type=int)
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()
    if args.probe:
        asyncio.run(probe())
        return
    if not args.server_pid:
        parser.error("Specify the verified Bender server PID, or --probe")
    import psutil
    parent = psutil.Process(args.server_pid)
    expected_server = (HERE / "server.py").resolve()
    expected_worker = (HERE / "rvc_worker.py").resolve()
    def runs(process, script):
        return any(Path(arg).resolve() == script for arg in process.cmdline() if arg.endswith(".py"))
    if not runs(parent, expected_server):
        raise RuntimeError("PID does not run this project's server.py")
    workers = [p for p in parent.children() if runs(p, expected_worker)]
    if len(workers) != 1:
        raise RuntimeError(f"Expected exactly one RVC child, found {len(workers)}")
    worker = workers[0]
    worker.terminate()
    worker.wait(timeout=10)
    print("Reloading verified RVC child", worker.pid, "parent stays running", parent.pid, flush=True)
    result = subprocess.run([parent.exe(), "-B", str(Path(__file__).resolve()), "--probe"],
                            timeout=120, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    if result.returncode:
        raise RuntimeError("Worker warmup probe failed")
    new_workers = [p for p in parent.children() if runs(p, expected_worker)]
    if len(new_workers) != 1 or new_workers[0].pid == worker.pid:
        raise RuntimeError("RVC did not restart")
    print("New RVC worker", new_workers[0].pid, flush=True)


if __name__ == "__main__":
    main()
