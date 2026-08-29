#!/usr/bin/env python3
"""Постійний Applio RVC: модель у VRAM, без холодного старту на кожну фразу.

Запуск: APPLIO_PYTHON, cwd = APPLIO_ROOT, env RVC_PTH / RVC_INDEX.
Протокол stdin: JSON-рядок {"cmd":"convert","in":"...","out":"..."}
Відповідь stdout: рядок 'RVCJSON {...}'
"""

from __future__ import annotations

import json
import os
import sys
import traceback
from pathlib import Path


def emit(obj: dict) -> None:
    sys.stdout.write("RVCJSON " + json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def main() -> int:
    root = Path(os.environ.get("APPLIO_ROOT") or os.getcwd()).resolve()
    os.chdir(root)
    sys.path.insert(0, str(root))
    os.environ.setdefault("TEMP", r"A:\tmp")
    os.environ.setdefault("TMP", r"A:\tmp")

    pth = os.environ.get("RVC_PTH") or ""
    index = os.environ.get("RVC_INDEX") or ""
    if not pth or not Path(pth).is_file():
        emit({"ok": False, "event": "ready", "error": f"no pth: {pth}"})
        return 1

    from rvc.infer.infer import VoiceConverter

    vc = VoiceConverter()
    pitch = int(os.environ.get("RVC_PITCH", "-1"))
    index_rate = float(os.environ.get("RVC_INDEX_RATE", "0.55"))
    f0 = os.environ.get("RVC_F0_METHOD", "rmvpe")
    # 0 = динаміка Piper, 1 = конверт RVC. Занадто низьке + овертрейн = «п'яний» голос.
    volume_envelope = float(os.environ.get("RVC_RMS", "0.75"))
    protect = float(os.environ.get("RVC_PROTECT", "0.5"))
    emit({"ok": True, "event": "ready"})

    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            emit({"ok": False, "error": "bad json"})
            continue
        cmd = msg.get("cmd")
        if cmd == "quit":
            emit({"ok": True, "event": "bye"})
            break
        if cmd != "convert":
            emit({"ok": False, "error": "unknown cmd"})
            continue
        try:
            vc.convert_audio(
                audio_input_path=msg["in"],
                audio_output_path=msg["out"],
                model_path=pth,
                index_path=index,
                pitch=pitch,
                f0_method=f0,
                index_rate=index_rate,
                volume_envelope=volume_envelope,
                protect=protect,
                embedder_model="contentvec",
                export_format="WAV",
            )
            emit({"ok": True, "event": "done"})
        except Exception:
            emit({"ok": False, "error": traceback.format_exc()[-1200:]})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
