#!/usr/bin/env python3
"""Гоняет cloudflared и пишет https://….trycloudflare.com в tunnel_url.txt."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "tunnel_url.txt"
CF = HERE / "cloudflared.exe"
PAT = re.compile(r"https://[a-z0-9-]+\.trycloudflare\.com", re.I)


def main() -> int:
    exe = str(CF) if CF.is_file() else "cloudflared"
    proc = subprocess.Popen(
        [exe, "tunnel", "--url", "http://127.0.0.1:8765"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        m = PAT.search(line)
        if m:
            url = m.group(0)
            OUT.write_text(url, encoding="utf-8")
            print(f"\n*** Публичный URL: {url}", flush=True)
            print("*** Колонка дома в той же Wi‑Fi запомнит его сама.\n", flush=True)
    return proc.wait() or 0


if __name__ == "__main__":
    raise SystemExit(main())
