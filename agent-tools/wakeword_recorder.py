"""Local-only collection of real examples for Bender's future on-device model.

Run with --open. This is a dataset recorder, NOT wake-word recognition.
No third-party dependencies, cloud uploads, device access, or automatic recording.
"""
import argparse
import array
import io
import json
import math
import secrets
import sys
import threading
import uuid
import wave
import webbrowser
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABELS = ("hey_bender", "privet_bender", "negative", "bender")
SPLITS = ("train", "test")
MAX_BYTES = 16000 * 2 * 8 + 1024


def inspect_wav(body):
    if len(body) > MAX_BYTES:
        raise ValueError("Запись слишком длинная: максимум 8 секунд.")
    try:
        with wave.open(io.BytesIO(body), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, 16000, "NONE"):
                raise ValueError("Нужен WAV PCM16, моно, 16000 Гц.")
            count = wav.getnframes()
            if not 16000 <= count <= 128000:
                raise ValueError("Длительность записи должна быть от 1 до 8 секунд.")
            pcm = wav.readframes(count)
            if len(pcm) != count * 2:
                raise ValueError("WAV обрезан: аудиоданные неполные.")
    except (wave.Error, EOFError) as exc:
        raise ValueError("Не удалось прочитать WAV.") from exc
    samples = array.array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    rms = math.sqrt(sum(s * s for s in samples) / count)
    clipped = sum(abs(s) >= 32760 for s in samples) / count
    return {"seconds": round(count / 16000, 3), "rms": round(rms, 1),
            "peak": max(abs(s) for s in samples), "clipped_fraction": round(clipped, 6)}


class RecorderServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, output):
        super().__init__(("127.0.0.1", port), RecorderHandler)
        self.output = Path(output)
        self.token = secrets.token_urlsafe(32)
        self.origin = f"http://127.0.0.1:{self.server_port}"
        self.lock = threading.Lock()

    def counts(self):
        return {split: {label: len(list((self.output / split / label).glob("*.wav")))
                        for label in LABELS} for split in SPLITS}


class RecorderHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, status, value, mime="application/json; charset=utf-8"):
        # Drain a bounded rejected request: closing a Windows TCP socket with
        # unread audio can reset it before the browser receives the error.
        unread = getattr(self, "unread_body", 0)
        if 0 < unread <= MAX_BYTES * 2:
            self.connection.settimeout(2)
            try:
                self.rfile.read(unread)
            except OSError:
                pass
            self.unread_body = 0
        body = value.encode("utf-8") if isinstance(value, str) else json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Permissions-Policy", "microphone=(self)")
        self.end_headers()
        self.wfile.write(body)

    def trusted_host(self):
        return self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}"

    def do_GET(self):
        if not self.trusted_host():
            return self.reply(403, {"error": "Недопустимый адрес."})
        if self.path == "/":
            page = Path(__file__).with_name("wakeword-recorder.html").read_text(encoding="utf-8")
            return self.reply(200, page.replace("__TOKEN__", self.server.token), "text/html; charset=utf-8")
        if self.path == "/counts":
            return self.reply(200, self.server.counts())
        self.reply(404, {"error": "Не найдено."})

    def do_POST(self):
        try:
            self.unread_body = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return self.reply(400, {"error": "Недопустимый размер записи."})
        if (not self.trusted_host() or self.headers.get("Origin") != self.server.origin
                or not secrets.compare_digest(self.headers.get("X-Recorder-Token", ""), self.server.token)):
            return self.reply(403, {"error": "Открой страницу записи заново."})
        if self.path == "/shutdown":
            self.reply(200, {"stopped": True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        if self.path != "/save":
            return self.reply(404, {"error": "Не найдено."})
        label = self.headers.get("X-Label", "")
        split = self.headers.get("X-Split", "")
        if label not in LABELS or split not in SPLITS:
            return self.reply(400, {"error": "Неизвестная группа записи."})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 44 <= length <= MAX_BYTES:
                raise ValueError("Недопустимый размер записи.")
            self.connection.settimeout(10)
            body = self.rfile.read(length)
            self.unread_body = 0
            if len(body) != length:
                raise ValueError("Передача записи прервалась.")
            quality = inspect_wav(body)
            if label != "negative" and quality["peak"] < 100:
                raise ValueError("Почти тишина. Проверь выбранный микрофон и запиши ещё раз.")
            if quality["clipped_fraction"] > 0.02:
                raise ValueError("Микрофон перегружен. Отойди немного и повтори запись.")
        except (ValueError, TimeoutError) as exc:
            return self.reply(400, {"error": str(exc)})
        filename = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "_" + uuid.uuid4().hex[:12]
        metadata = {"label": label, "split": split, "source": "browser_microphone",
                    "created_utc": datetime.now(timezone.utc).isoformat(), **quality}
        try:
            with self.server.lock:
                directory = self.server.output / split / label
                directory.mkdir(parents=True, exist_ok=True)
                (directory / (filename + ".wav")).write_bytes(body)
                (directory / (filename + ".json")).write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
        except OSError:
            return self.reply(500, {"error": "Не удалось сохранить запись на диск."})
        self.reply(201, {"saved": True, "quality": quality, "counts": self.server.counts()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/wakeword-dataset")
    parser.add_argument("--open", action="store_true", help="Open the local page; recording still requires a click.")
    args = parser.parse_args()
    with RecorderServer(args.port, args.output) as server:
        print(f"Bender wake-word dataset recorder: {server.origin}", flush=True)
        print(f"Output: {server.output.resolve()}", flush=True)
        if args.open:
            webbrowser.open(server.origin)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
