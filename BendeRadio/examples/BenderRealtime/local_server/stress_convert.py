"""Stanza POS + словник наголосів у процесі Applio (CPU). Сервер 3.14 без torch."""

from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

_proc: subprocess.Popen | None = None
_lock = threading.Lock()
_ready_msg = ""


def _applio_python() -> str:
    py = os.environ.get("APPLIO_PYTHON", "").strip()
    if py and Path(py).is_file():
        return py
    guessed = Path(r"A:\Programs\Applio-main\Applio-main\env\python.exe")
    if guessed.is_file():
        return str(guessed)
    return "python"


def _read_json(proc: subprocess.Popen, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    assert proc.stdout is not None
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("stress worker died")
        line = line.strip()
        if not line.startswith("STRESSJSON "):
            continue
        return json.loads(line[11:])
    raise TimeoutError("stress worker timeout")


def _start_worker() -> subprocess.Popen:
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONUTF8"] = "1"
    env.setdefault("STANZA_RESOURCES_DIR", r"A:\stanza_resources")
    env.setdefault("TEMP", r"A:\tmp")
    env.setdefault("TMP", r"A:\tmp")
    env.setdefault("TMPDIR", r"A:\tmp")
    log = Path(r"A:\tmp\stress_worker.log")
    log.parent.mkdir(parents=True, exist_ok=True)
    err = open(log, "ab", buffering=0)
    proc = subprocess.Popen(
        [_applio_python(), "-u", str(HERE / "stress_worker.py")],
        cwd=str(HERE),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=err,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        bufsize=1,
    )
    msg = _read_json(proc, 120.0)
    if not msg.get("ok"):
        raise RuntimeError(msg.get("error") or "stress worker ready fail")
    return proc


def start() -> str:
    """Підняти воркер. Повертає короткий статус."""
    global _proc, _ready_msg
    with _lock:
        if _proc is not None and _proc.poll() is None:
            return _ready_msg or "already running"
        _proc = _start_worker()
        _ready_msg = "stanza POS"
        return _ready_msg


def available() -> bool:
    return _proc is not None and _proc.poll() is None


def stress(text: str) -> str:
    global _proc
    if not text:
        return text
    with _lock:
        if _proc is None or _proc.poll() is not None:
            raise RuntimeError("stress worker not running")
        assert _proc.stdin is not None
        _proc.stdin.write(json.dumps({"cmd": "stress", "text": text}, ensure_ascii=False) + "\n")
        _proc.stdin.flush()
        try:
            msg = _read_json(_proc, 30.0)
        except Exception:
            if _proc is not None:
                _proc.kill()
            _proc = None
            raise
    if not msg.get("ok"):
        raise RuntimeError(msg.get("error") or "stress fail")
    return str(msg.get("text") or text)
