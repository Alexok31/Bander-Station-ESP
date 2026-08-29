#!/usr/bin/env python3
"""Наголоси з морфологією (Stanza POS), CPU. Не чіпає VRAM колонки.

Протокол stdin: JSON {"cmd":"stress","text":"..."}
Відповідь: STRESSJSON {"ok":true,"text":"..."}
"""

from __future__ import annotations

import json
import os
import sys
import traceback


def emit(obj: dict) -> None:
    sys.stdout.write("STRESSJSON " + json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _has_verb_reading(stressify, word: str) -> bool:
    from ukrainian_word_stress.stressify_ import (
        _parse_dictionary_value,
        _trie_value,
    )

    vals = _trie_value(stressify.dict, word)
    if not vals:
        return False
    for tags, _acc in _parse_dictionary_value(vals[0]):
        if any(t == "upos=VERB" or str(t).endswith("=VERB") for t in tags):
            return True
    return False


def _place_stress(stressify, result, start: int, end: int, parse: dict) -> None:
    """Словник + POS; якщо повні теги не збіглись — єдиний наголос для цього upos."""
    from ukrainian_word_stress.stressify_ import (
        _accent_positions_from_values,
        _parse_dictionary_value,
        _trie_value,
    )

    word = parse.get("text") or ""
    values = _trie_value(stressify.dict, word)
    if not values:
        return
    accents = _accent_positions_from_values(values, parse, "skip")
    if not accents:
        upos = parse.get("upos") or ""
        same = [
            acc
            for tags, acc in _parse_dictionary_value(values[0])
            if not upos or f"upos={upos}" in tags
        ]
        if len({tuple(a) for a in same}) == 1:
            accents = same[0]
    if accents:
        result.replace(start, end, stressify._apply_accent_positions(word, accents))


def main() -> int:
    os.environ.setdefault("STANZA_RESOURCES_DIR", r"A:\stanza_resources")
    os.environ.setdefault("TEMP", r"A:\tmp")
    os.environ.setdefault("TMP", r"A:\tmp")
    try:
        sys.stdin.reconfigure(encoding="utf-8")
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    try:
        import stanza
        from stanza.pipeline.core import DownloadMethod
        from ukrainian_word_stress import Disambiguation, Stressifier, StressSymbol
        from ukrainian_word_stress.mutable_text import MutableText

        nlp = stanza.Pipeline(
            "uk",
            processors="tokenize,pos,mwt",
            package={"tokenize": "iu", "mwt": "iu", "pos": "iu_charlm"},
            download_method=DownloadMethod.REUSE_RESOURCES,
            use_gpu=False,
            logging_level="ERROR",
        )
        stressify = Stressifier(
            stress_symbol=StressSymbol.CombiningAcuteAccent,
            on_ambiguity="skip",
            disambiguation=Disambiguation.Dictionary,
        )
        stressify.nlp = nlp
    except Exception:
        emit({"ok": False, "event": "ready", "error": traceback.format_exc()[-800:]})
        return 1
    emit({"ok": True, "event": "ready"})

    skip_first = {"PUNCT", "CCONJ", "INTJ", "PART"}

    def stress_text(text: str) -> str:
        parsed = nlp(text)
        result = MutableText(text)
        for sent in parsed.sentences:
            first_content = True
            for token in sent.tokens:
                parse = token.to_dict()[0]
                word = parse.get("text") or ""
                upos = parse.get("upos") or ""
                start = token.start_char
                end = token.end_char
                if start is None or end is None or not word:
                    continue
                if (
                    first_content
                    and upos == "NOUN"
                    and _has_verb_reading(stressify, word)
                ):
                    parse = dict(parse)
                    parse["upos"] = "VERB"
                    parse["feats"] = "Number=Sing|Person=2|Mood=Imp|VerbForm=Fin"
                if upos not in skip_first:
                    first_content = False
                elif upos == "PUNCT" and word in ",;:":
                    first_content = True
                _place_stress(stressify, result, start, end, parse)
        return result.get_edited_text()

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
        if cmd != "stress":
            emit({"ok": False, "error": "unknown cmd"})
            continue
        text = str(msg.get("text") or "")
        try:
            emit({"ok": True, "text": stress_text(text) if text else text})
        except Exception:
            emit({"ok": False, "error": traceback.format_exc()[-800:], "text": text})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
