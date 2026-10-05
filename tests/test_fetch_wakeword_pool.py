import importlib.util
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import wave
import zipfile

SPEC = importlib.util.spec_from_file_location("pool_fetch", Path(__file__).resolve().parents[1] / "agent-tools/fetch_wakeword_pool.py")
pool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pool)


class PoolFetchTests(unittest.TestCase):
    def setUp(self):
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav:
            wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
            wav.writeframes(b'\x01\x00' * 96000)
        self.wav = buf.getvalue()
        self.items = [{"id": 7, "label": "hey_bender", "split": "train"},
                      {"id": 8, "label": "privet_bender", "split": "test"}]

    def test_validation(self):
        self.assertEqual(pool.validate_items({"items": self.items}), self.items)
        self.assertEqual(pool.validate_items({"items": [dict(self.items[0], label="bender") ]})[0]['label'], 'bender')
        pool.validate_wav(self.wav)
        for body in (self.wav[:-2], b'x' * pool.WAV_BYTES):
            with self.assertRaises((ValueError, wave.Error)):
                pool.validate_wav(body)
        for items in ([], self.items * 2, [dict(self.items[0], label="../secrets")],
                      [dict(self.items[0], id=True)], [dict(self.items[0], id=0)]):
            with self.assertRaises(ValueError):
                pool.validate_items({"items": items})

    def download(self, truncate=False):
        owner = self
        requests = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                body = json.dumps({"items": owner.items}).encode() if self.path == '/wakeword/samples' else owner.wav
                if truncate and self.path.endswith('id=8'):
                    body = body[:-10]
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                # Match the device's bounded, paced writes instead of one large
                # loopback send (which stalls intermittently on this Windows host).
                for offset in range(0, len(body), 1460):
                    self.wfile.write(body[offset:offset + 1460])
                    time.sleep(.001)
            def log_message(self, *_):
                pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as temp:
                out = Path(temp) / 'pool.zip'
                if truncate:
                    with self.assertRaises(ValueError):
                        pool.fetch_pool(f'http://127.0.0.1:{server.server_port}', out)
                    self.assertFalse(out.exists())
                else:
                    pool.fetch_pool(f'http://127.0.0.1:{server.server_port}', out)
                    with zipfile.ZipFile(out) as archive:
                        self.assertIsNone(archive.testzip())
                        manifest = json.loads(archive.read('manifest.json'))
                        self.assertEqual(len(manifest['items']), len(self.items))
                        for item in manifest['items']:
                            self.assertEqual(archive.read(item['file']), self.wav)
                    with self.assertRaises(FileExistsError):
                        pool.fetch_pool(f'http://127.0.0.1:{server.server_port}', out)
                self.assertEqual(requests, ['/wakeword/samples'] +
                                 ['/wakeword/audio.wav?id=' + str(item['id']) for item in self.items])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_batch_preserves_all_audio_and_metadata_without_deleting(self):
        self.items = [dict(self.items[i % 2], id=i + 1) for i in range(30)]
        self.download()

    def test_partial_download_is_not_published_as_complete(self):
        self.download(truncate=True)


if __name__ == '__main__':
    unittest.main()
