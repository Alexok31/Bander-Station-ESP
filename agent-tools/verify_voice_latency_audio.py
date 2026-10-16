"""Offline pronunciation smoke check for before/after latency WAVs."""
import json
from pathlib import Path
import re
import sys
import unicodedata
import wave

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/voice-latency-20261011"
sys.path.insert(0, str(ROOT / "BendeRadio/examples/BenderRealtime/local_server"))


def words(text):
    text = "".join(c for c in unicodedata.normalize("NFD", text.lower()) if not unicodedata.combining(c))
    return re.findall(r"[^\W\d_]+", text)


def distance(a, b):
    row = list(range(len(b)+1))
    for i, x in enumerate(a, 1):
        previous, row = row, [i]
        for j, y in enumerate(b, 1):
            row.append(min(row[-1]+1, previous[j]+1, previous[j-1]+(x != y)))
    return row[-1]


def main():
    import server
    server.load_whisper()
    rows = []
    for variant in ("baseline", "pitch-cached"):
        report = json.loads((OUT / variant / "results.json").read_text(encoding="utf-8"))
        for source in report["rows"]:
            if source["round"] != 1:
                continue
            with wave.open(str(ROOT / source["wav"]), "rb") as wav:
                pcm = wav.readframes(wav.getnframes())
            audio = server._pcm_for_whisper(pcm)
            segments, _ = server.whisper_model.transcribe(audio, language="uk", beam_size=5,
                                                         temperature=0, vad_filter=False,
                                                         condition_on_previous_text=False)
            heard = " ".join(s.text.strip() for s in segments)
            expected_words, heard_words = words(source["text"]), words(heard)
            row = {"variant": variant, "phrase": source["phrase"], "text": source["text"],
                   "heard": heard, "word_error_rate": distance(expected_words, heard_words)/len(expected_words)}
            rows.append(row)
            print(json.dumps(row, ensure_ascii=True), flush=True)
    (OUT / "pronunciation.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
