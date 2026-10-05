import http.client
import importlib.util
import io
import json
import math
import tempfile
import threading
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("wakeword_recorder", ROOT / "agent-tools/wakeword_recorder.py")
recorder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recorder)


def sample(rate=16000, seconds=2, amplitude=1500):
    out = io.BytesIO()
    # Little-endian independent of the host machine.
    pcm = b"".join(int(amplitude * math.sin(2 * math.pi * 440 * i / rate)).to_bytes(2, "little", signed=True)
                   for i in range(int(rate * seconds)))
    with wave.open(out, "wb") as wav:
        wav.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        wav.writeframes(pcm)
    return out.getvalue()


class RecorderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.server = recorder.RecorderServer(0, Path(self.tmp.name))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()

    def request(self, method="POST", path="/save", body=None, **overrides):
        headers = {"Origin": self.server.origin, "X-Recorder-Token": self.server.token,
                   "X-Label": "hey_bender", "X-Split": "train"}
        headers.update(overrides)
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        conn.request(method, path, body, headers)
        reply = conn.getresponse()
        status, raw = reply.status, reply.read()
        conn.close()
        return status, raw

    def test_all_phrases_and_held_out_set_remain_separate(self):
        body = sample()
        for label in ("hey_bender", "privet_bender", "bender"):
            for split in ("train", "test"):
                status, raw = self.request(body=body, **{"X-Label": label, "X-Split": split})
                self.assertEqual(status, 201, raw)
                self.assertEqual(json.loads(raw)["counts"][split][label], 1)
        self.assertEqual(len(list(Path(self.tmp.name).rglob("*.wav"))), 6)

    def test_rejects_wrong_rate_truncation_and_excess_duration(self):
        for body in (sample(rate=24000), sample()[:-100], sample(seconds=.5), sample(seconds=9), b"not a wave" * 8):
            self.assertEqual(self.request(body=body)[0], 400)
        self.assertFalse(list(Path(self.tmp.name).rglob("*.wav")))

    def test_silence_allowed_only_as_negative(self):
        body = sample(amplitude=0)
        self.assertEqual(self.request(body=body)[0], 400)
        self.assertEqual(self.request(body=body, **{"X-Label": "negative"})[0], 201)

    def test_rejects_clipping(self):
        body = bytearray(sample())
        body[44:] = b"\xff\x7f" * ((len(body) - 44) // 2)
        self.assertEqual(self.request(body=body)[0], 400)

    def test_no_external_origin_or_rebinding_or_missing_token(self):
        for headers in ({"Origin": "https://example.org"}, {"X-Recorder-Token": ""}, {"Host": "example.org"}):
            self.assertEqual(self.request(body=sample(), **headers)[0], 403)
        self.assertEqual(self.request("GET", "/", **{"Host": "example.org"})[0], 403)
        self.assertFalse(list(Path(self.tmp.name).rglob("*.wav")))

    def test_cannot_choose_a_path(self):
        self.assertEqual(self.request(body=sample(), **{"X-Label": "../outside"})[0], 400)
        self.assertEqual(self.request(body=sample(), **{"X-Split": "../outside"})[0], 400)
        self.assertEqual(self.request("GET", "/../wakeword_recorder.py")[0], 404)

    def test_repeated_saves_do_not_overwrite_and_metadata_agrees(self):
        body = sample()
        for _ in range(2):
            self.assertEqual(self.request(body=body)[0], 201)
        files = list(Path(self.tmp.name).rglob("*.wav"))
        self.assertEqual(len(files), 2)
        for file in files:
            self.assertEqual(file.read_bytes(), body)
            meta = json.loads(file.with_suffix(".json").read_text())
            self.assertEqual(meta["source"], "browser_microphone")
            self.assertEqual(meta["seconds"], 2)

    def test_page_is_local_and_does_not_record_on_load(self):
        status, raw = self.request("GET", "/")
        self.assertEqual(status, 200)
        page = raw.decode()
        self.assertNotIn("__TOKEN__", page)
        self.assertIn(self.server.token, page)
        self.assertNotIn('src="http', page)
        self.assertNotIn('href="http', page)
        self.assertIn("el('record').onclick=async()=>", page)

    def test_stop_requires_token_and_stops_server(self):
        self.assertEqual(self.request(path="/shutdown", **{"X-Recorder-Token": ""})[0], 403)
        self.assertTrue(self.thread.is_alive())
        self.assertEqual(self.request(path="/shutdown")[0], 200)
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())


if __name__ == "__main__":
    unittest.main()
