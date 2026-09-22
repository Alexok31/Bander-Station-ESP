"""
RVC поверх Piper: українська вимова лишається, тембр — твоя пародія.

Потрібен Applio (https://github.com/IAHispano/Applio) і натренована модель.
Сервер клікає: python core.py infer ...

Env:
  RVC_ENABLE=1
  APPLIO_ROOT=C:\\path\\to\\Applio
  RVC_PTH=voice_clone/models/bender.pth          (або абсолютний шлях)
  RVC_INDEX=voice_clone/models/bender.index      (опційно)
  RVC_PITCH=0                                    (напівтони; Бендер часто -2..+2)
  RVC_INDEX_RATE=0.6
  RVC_F0_METHOD=rmvpe
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import threading
import time
import wave
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
DEFAULT_MODELS = HERE / "voice_clone" / "models"

_proc: subprocess.Popen | None = None
_lock = threading.Lock()


def _env_bool(name: str, default: bool = False) -> bool:
    v = os.environ.get(name)
    if v is None:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


def enabled() -> bool:
    # Якщо модель є — RVC за замовчуванням увімкнений (інакше Piper «не Бендер»).
    # Вимкнути явно: RVC_ENABLE=0
    v = os.environ.get("RVC_ENABLE")
    if v is not None and v.strip() != "":
        return _env_bool("RVC_ENABLE", True)
    pth = _resolve(os.environ.get("RVC_PTH"), None)
    if pth is None:
        cands = sorted(DEFAULT_MODELS.glob("*.pth"))
        pth = cands[0] if cands else None
    return bool(pth and pth.is_file())


def _resolve(p: str | None, default: Path | None = None) -> Path | None:
    if p:
        path = Path(p)
        if not path.is_absolute():
            path = HERE / path
        return path
    return default


def config() -> dict:
    models = DEFAULT_MODELS
    pth = _resolve(os.environ.get("RVC_PTH"), None)
    index = _resolve(os.environ.get("RVC_INDEX"), None)
    if pth is None:
        cands = sorted(models.glob("*.pth"))
        pth = cands[0] if cands else None
    if index is None and pth is not None:
        sib = pth.with_suffix(".index")
        if sib.is_file():
            index = sib
        else:
            idxs = sorted(models.glob("*.index"))
            index = idxs[0] if idxs else None
    root = os.environ.get("APPLIO_ROOT", "").strip()
    if not root:
        guessed = Path(r"A:\Programs\Applio-main\Applio-main")
        if (guessed / "core.py").is_file():
            root = str(guessed)
    py = os.environ.get("APPLIO_PYTHON", "").strip()
    if not py:
        env_py = Path(root) / "env" / "python.exe" if root else None
        py = str(env_py) if env_py and env_py.is_file() else "python"
    return {
        "applio": Path(root) if root else None,
        "pth": pth,
        "index": index,
        "pitch": int(os.environ.get("RVC_PITCH", "-1")),
        "index_rate": float(os.environ.get("RVC_INDEX_RATE", "0.55")),
        "f0": os.environ.get("RVC_F0_METHOD", "rmvpe"),
        "python": py,
    }


def ready() -> tuple[bool, str]:
    if not enabled():
        return False, "RVC_ENABLE off"
    c = config()
    if not c["applio"] or not (c["applio"] / "core.py").is_file():
        return False, "задайте APPLIO_ROOT (папка з core.py)"
    if not c["pth"] or not c["pth"].is_file():
        return False, f"немає .pth у {DEFAULT_MODELS} і RVC_PTH"
    return True, f"pth={c['pth'].name}"


def _read_rvcjson(proc: subprocess.Popen, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    assert proc.stdout is not None
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("RVC worker died")
        line = line.strip()
        if not line.startswith("RVCJSON "):
            continue
        return json.loads(line[8:])
    raise TimeoutError("RVC worker timeout")


def _start_worker() -> subprocess.Popen:
    c = config()
    env = os.environ.copy()
    env["APPLIO_ROOT"] = str(c["applio"])
    env["RVC_PTH"] = str(c["pth"])
    env["RVC_INDEX"] = str(c["index"] or "")
    env["RVC_PITCH"] = str(c["pitch"])
    env["RVC_INDEX_RATE"] = str(c["index_rate"])
    env["RVC_F0_METHOD"] = str(c["f0"])
    env["PYTHONUNBUFFERED"] = "1"
    env.setdefault("TEMP", r"A:\tmp")
    env.setdefault("TMP", r"A:\tmp")
    env.setdefault("TMPDIR", r"A:\tmp")
    log = Path(r"A:\tmp\rvc_worker.log")
    log.parent.mkdir(parents=True, exist_ok=True)
    err = open(log, "ab", buffering=0)
    # Без консолі: інакше CLOSE вікна start.bat / cmd вбиває Applio (forrtl 200).
    flags = 0
    if os.name == "nt":
        flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(
        [c["python"], "-u", str(HERE / "rvc_worker.py")],
        cwd=str(c["applio"]),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=err,
        text=True,
        env=env,
        bufsize=1,
        creationflags=flags,
    )
    msg = _read_rvcjson(proc, 120.0)
    if not msg.get("ok"):
        raise RuntimeError(msg.get("error") or "RVC worker ready fail")
    return proc


def _ensure_worker() -> subprocess.Popen:
    global _proc
    if _proc is not None and _proc.poll() is None:
        return _proc
    _proc = _start_worker()
    return _proc


def warmup() -> None:
    if not enabled():
        return
    ok, reason = ready()
    if not ok:
        return
    sr = 24000
    pcm = np.zeros(sr // 5, dtype=np.int16).tobytes()
    convert_pcm(pcm, sr)


def _write_wav(path: Path, pcm16: np.ndarray, sr: int) -> None:
    pcm16 = np.asarray(pcm16, dtype=np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm16.tobytes())


def _read_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        ch, sw, sr, nframes, *_ = w.getparams()
        raw = w.readframes(nframes)
    if sw != 2:
        raise RuntimeError("RVC out: need 16-bit wav")
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return np.clip(x, -32767, 32767).astype(np.int16), sr


def convert_pcm(pcm16: bytes, sample_rate: int) -> bytes:
    """PCM s16le mono → той самий формат після RVC (resample назад якщо треба)."""
    global _proc
    ok, reason = ready()
    if not ok:
        raise RuntimeError(reason)

    x = np.frombuffer(pcm16, dtype=np.int16)
    if x.size < sample_rate // 10:
        return pcm16

    with tempfile.TemporaryDirectory(prefix="bender_rvc_") as td:
        td_path = Path(td)
        src = td_path / "in.wav"
        dst = td_path / "out.wav"
        _write_wav(src, x, sample_rate)
        with _lock:
            proc = _ensure_worker()
            assert proc.stdin is not None
            proc.stdin.write(
                json.dumps({"cmd": "convert", "in": str(src), "out": str(dst)}) + "\n"
            )
            proc.stdin.flush()
            try:
                msg = _read_rvcjson(proc, 180.0)
            except Exception:
                if _proc is not None:
                    _proc.kill()
                    _proc = None
                raise
        if not msg.get("ok") or not dst.is_file():
            err = msg.get("error") if isinstance(msg, dict) else ""
            raise RuntimeError(f"Applio infer fail: {err}")

        y, sr = _read_wav(dst)

    if sr != sample_rate and y.size:
        n_dst = int(round(y.size * sample_rate / sr))
        t_old = np.linspace(0.0, 1.0, y.size, endpoint=False)
        t_new = np.linspace(0.0, 1.0, n_dst, endpoint=False)
        yf = np.interp(t_new, t_old, y.astype(np.float32))
        y = np.clip(yf, -32767, 32767).astype(np.int16)
    return y.tobytes()
