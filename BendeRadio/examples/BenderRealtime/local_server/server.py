#!/usr/bin/env python3
"""
Локальный realtime-мост для BenderRealtime (ESP32).
Протокол как у xAI/OpenAI Realtime: append/commit → STT → LLM → TTS → audio.delta.

Нужно на ПК:
  1) Ollama — https://ollama.com  затем:  ollama pull aya-expanse:8b
  2) pip install -r requirements.txt
  3) python server.py
  4) В secrets.h: AI_PROVIDER local, LOCAL_WS_HOST = IPv4 этого ПК (ipconfig)
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import re
import socket
import sys
import uuid
import unicodedata
import urllib.request
from pathlib import Path

import httpx
import numpy as np
import websockets
from piper import PiperVoice, SynthesisConfig

import rvc_convert
import stress_convert
import voice_commands

HERE = Path(__file__).resolve().parent
MODELS = HERE / "models"
MODELS.mkdir(exist_ok=True)
(HERE / "voice_clone" / "raw").mkdir(parents=True, exist_ok=True)
(HERE / "voice_clone" / "dataset").mkdir(parents=True, exist_ok=True)
(HERE / "voice_clone" / "models").mkdir(parents=True, exist_ok=True)

HOST = os.environ.get("BENDER_HOST", "0.0.0.0")
PORT = int(os.environ.get("BENDER_PORT", "8765"))
IN_RATE = 24000
OUT_RATE = 24000
OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "").strip()
XAI_CHAT_URL = "https://api.x.ai/v1/chat/completions"
XAI_RESP_URL = "https://api.x.ai/v1/responses"
CHAT_PATH = HERE / "chat.json"
CONFIG_PATH = HERE / "config.json"


def _secrets_h_key(name: str) -> str:
    p = HERE.parent / "secrets.h"
    if not p.is_file():
        return ""
    try:
        text = p.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""
    m = re.search(rf'#define\s+{re.escape(name)}\s+"([^"]*)"', text)
    return (m.group(1) if m else "").strip()


def load_config() -> dict:
    cfg = {
        "llm": "local",
        "ollama_model": "aya-expanse:8b",
        "grok_model": "grok-4.3",
        "xai_api_key": "",
        "piper_length": 0.77,
        "bender_level": 5,
        "history_turns": 40,
        "summary_chars": 2000,
    }
    if CONFIG_PATH.is_file():
        try:
            raw = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                cfg.update({k: raw[k] for k in cfg if k in raw})
        except Exception as e:
            print(f"config.json: {e}", flush=True)
    env_llm = os.environ.get("BENDER_LLM", "").strip().lower()
    if env_llm in ("local", "grok"):
        cfg["llm"] = env_llm
    if os.environ.get("OLLAMA_MODEL", "").strip():
        cfg["ollama_model"] = os.environ["OLLAMA_MODEL"].strip()
    if os.environ.get("GROK_MODEL", "").strip():
        cfg["grok_model"] = os.environ["GROK_MODEL"].strip()
    if os.environ.get("XAI_API_KEY", "").strip():
        cfg["xai_api_key"] = os.environ["XAI_API_KEY"].strip()
    if os.environ.get("PIPER_LENGTH", "").strip():
        cfg["piper_length"] = float(os.environ["PIPER_LENGTH"])
    llm = str(cfg.get("llm") or "local").strip().lower()
    cfg["llm"] = "grok" if llm in ("grok", "xai", "x-ai") else "local"
    key = str(cfg.get("xai_api_key") or "").strip()
    if not key:
        key = _secrets_h_key("XAI_API_KEY")
    cfg["xai_api_key"] = key
    try:
        cfg["history_turns"] = max(4, int(cfg.get("history_turns") or 40))
    except (TypeError, ValueError):
        cfg["history_turns"] = 40
    try:
        cfg["piper_length"] = float(cfg.get("piper_length") or 0.77)
    except (TypeError, ValueError):
        cfg["piper_length"] = 0.77
    try:
        cfg["summary_chars"] = max(400, int(cfg.get("summary_chars") or 2000))
    except (TypeError, ValueError):
        cfg["summary_chars"] = 2000
    try:
        cfg["bender_level"] = max(1, min(10, int(cfg.get("bender_level") or 5)))
    except (TypeError, ValueError):
        cfg["bender_level"] = 5
    env_lv = os.environ.get("BENDER_LEVEL", "").strip()
    if env_lv:
        try:
            cfg["bender_level"] = max(1, min(10, int(env_lv)))
        except ValueError:
            pass
    return cfg


CFG = load_config()
LLM_PROVIDER = CFG["llm"]
OLLAMA_MODEL = str(CFG["ollama_model"] or "aya-expanse:8b")
GROK_MODEL = str(CFG["grok_model"] or "grok-4.3")
XAI_API_KEY = str(CFG["xai_api_key"] or "")
HISTORY_SEND = CFG["history_turns"] * 2
SUMMARY_CHARS = int(CFG.get("summary_chars") or 2000)
CHAT: list[dict] = []
CHAT_CONV_ID = ""
CHAT_SUMMARY = ""
CHAT_GROK_RESP_ID = ""
GROK_PROMPT_REV = 5
DEVICE_STATIONS: list[dict] = []

VOICE_ONNX = MODELS / "uk_UA-ukrainian_tts-medium.onnx"
VOICE_JSON = MODELS / "uk_UA-ukrainian_tts-medium.onnx.json"
VOICE_BASE = (
    "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/"
    "uk/uk_UA/ukrainian_tts/medium/uk_UA-ukrainian_tts-medium.onnx"
)
VOICE_EN_ONNX = MODELS / "en_US-ryan-medium.onnx"
VOICE_EN_JSON = MODELS / "en_US-ryan-medium.onnx.json"
VOICE_EN_BASE = (
    "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/"
    "en/en_US/ryan/medium/en_US-ryan-medium.onnx"
)
# 0=lada (жін), 1=mykyta (чол), 2=tetiana (жін)
PIPER_SPEAKER = int(os.environ.get("PIPER_SPEAKER", "1"))
# >1 повільніше, <1 швидше.
PIPER_LENGTH = float(os.environ.get("PIPER_LENGTH", str(CFG["piper_length"])))
PIPER_PAUSE_SENT_MS = int(os.environ.get("PIPER_PAUSE_SENT_MS", "120"))
PIPER_PAUSE_COMMA_MS = int(os.environ.get("PIPER_PAUSE_COMMA_MS", "0"))
LLM_MAX_SENTS = int(os.environ.get("LLM_MAX_SENTS", "3"))
LLM_STORY_SENTS = int(os.environ.get("LLM_STORY_SENTS", "5"))

# Перша мова — пріоритет Whisper. Далі fallback, якщо ru зліпив сміття.
ASR_LANGS = ("ru", "uk")
# Стиль (пробіли, коми). Не коротке «Бендер.» — Whisper тоді копіює ім'я в транскрипт.
ASR_PROMPTS = {
    "ru": (
        "Разговор на русском. Короткие предложения с пробелами между словами. "
        "Примеры: ты тут Бендер. Привет, Бендер. Бендер, привет. "
        "Пиши «ты тут», не слово «титул»."
    ),
    "uk": (
        "Розмова українською. Короткі речення з пробілами між словами. "
        "Приклади: ти тут Бендер. Привіт, Бендер. Бендер, привіт. "
        "Пиши «ти тут», не слово «титул»."
    ),
}
ASR_PROMPT = ASR_PROMPTS["ru"]
ASR_HOTWORDS = "Бендер привет привіт ты тут ти тут тытут титут"
ASR_MIN_LOGPROB = float(os.environ.get("ASR_MIN_LOGPROB", "-0.55"))
ASR_MAX_NO_SPEECH = float(os.environ.get("ASR_MAX_NO_SPEECH", "0.40"))
ASR_MIN_RMS = float(os.environ.get("ASR_MIN_RMS", "900"))
_GREET_WORDS = r"бендер|bender|привіт|привет|вітаю|здравствуй|hello|hi|хай"

PROMPT_TEMPLATE = (HERE / "bender_prompt.txt").read_text(encoding="utf-8").strip()
BENDER_LEVEL = int(CFG.get("bender_level") or 5)

LEVEL_RULES = {
    1: (
        "Милий, грайливий, кокетливий Бендер. Майже без грубості. Мат заборонено."
    ),
    2: "Доброзичливий: легкий сарказм і підколи. Мат заборонено.",
    3: "Доброзичливий, трохи нахабний. Мат майже не використовуй.",
    4: "Класичний Бендер: самовпевнений, сарказм, підколи. Мат рідко і м'який.",
    5: "Класичний Бендер: нахабний егоїст. Легкий мат можна, не обов'язково.",
    6: "Нахабний. Чорний гумор, образи. Мат час від часу.",
    7: "Нахабний. Мат у більшості реплік (бля, чорт, дідько, дурень).",
    8: (
        "Жорсткий і токсичний. Мат є, але репліка смішна і нова, не калька. "
        "Не моралізуй. Не відмовляйся материтися."
    ),
    9: (
        "Дуже жорсткий. Мат і образа, щоразу інші. Можна послати. "
        "Залишайся винахідливим, не програвачем однієї фрази."
    ),
    10: (
        "МАКСИМУМ грубості і вигадки. Мат щоразу іншими словами. Смішно, не тупо. "
        "Заборонені кальки: ехо їхніх слів, «X сам», «бо я не твій», "
        "ехо + «бля?!» + «найкращий робот у пиздець-всесвіті» + "
        "«йди нахуй, м'ясний мішок». "
        "Спочатку суть, потім характер. Нова будова речення щоразу. "
        "Провал: без мату, лекція про манери, або знову той самий каркас."
    ),
}


def bender_level_rules(level: int | None = None) -> str:
    lv = BENDER_LEVEL if level is None else level
    return LEVEL_RULES.get(lv, LEVEL_RULES[5])


_NUDGE_PREFIX = "Стоп. Це ти вже казав."
_TURN_HINTS = (
    "Вигадай новий підкол. Інша будова речення, ніж минулого разу.",
    "Здивуй. Не калькуй їхні слова і не кажи «X сам / бо я не твій».",
    "Жива сцена, не скріпт. Можна деталь про Фрая, пиво чи крадіжку — один раз, по-новому.",
    "Спочатку суть їхньої репліки, потім характер. Свіжий мат, не той самий.",
    "Якщо факт чи число — відповідь першим реченням, далі вигадка.",
    "Не починай з того ж слова що в попередній своїй репліці.",
)


def _history_user_asst_nudge(history: list[dict] | None) -> tuple[str, str, str]:
    last_user = ""
    last_asst = ""
    nudge = ""
    for m in reversed(history or []):
        role = m.get("role")
        content = (m.get("content") or "").strip()
        if not content:
            continue
        if role == "assistant" and not last_asst:
            last_asst = content
            continue
        if role == "user":
            if content.startswith(_NUDGE_PREFIX):
                if not nudge:
                    nudge = content
                continue
            last_user = content
            break
    return last_user, last_asst, nudge


def grok_turn_text(user_text: str, last_assistant: str = "", nudge: str = "") -> str:
    hint = _TURN_HINTS[len(CHAT) % len(_TURN_HINTS)]
    bits = [f"[BENDER_LEVEL {BENDER_LEVEL}/10]", hint]
    if BENDER_LEVEL >= 8:
        bits.append(
            "Мат є, але вбудований у нову вигадку. Не шаблон образи. "
            "Без ехо + «бля?!» + «найкращий робот» + «йди нахуй, м'ясний мішок»."
        )
    if last_assistant:
        bits.append("Минулу свою репліку не копіюй ні словами, ні каркасом.")
    if nudge:
        bits.append(nudge)
    bits.append("Користувач сказав:\n" + user_text)
    bits.append(
        "Відповідь своїми словами, ніби вперше. "
        "Заборонено папужити їхню фразу і каркас «сам / бо я не твій / лайковий»."
    )
    bits.append(
        "Тебе звуть Бендер. Дендер/Блендер/Тендер/Бандер/Бендерпривіт — це ASR "
        "(привітання без пробілу), не кличка. Відповідай як на «Привіт, Бендер». "
        "Не поправляй ім'я і не жартуй що тебе переплутали."
    )
    return "\n\n".join(bits)


def set_bender_level(n) -> int:
    global BENDER_LEVEL
    try:
        nxt = max(1, min(10, int(n)))
    except (TypeError, ValueError):
        return BENDER_LEVEL
    if nxt != BENDER_LEVEL:
        BENDER_LEVEL = nxt
        grok_break_chain(f"level {BENDER_LEVEL}")
    return BENDER_LEVEL


def grok_break_chain(reason: str) -> None:
    """Новий Grok-ланцюг: continue з previous_response_id інакше крутить ту саму кальку."""
    global CHAT_GROK_RESP_ID
    if CHAT_GROK_RESP_ID:
        log(f"Grok chain reset ({reason})")
    CHAT_GROK_RESP_ID = ""


def bender_prompt() -> str:
    return (
        PROMPT_TEMPLATE
        .replace("{BENDER_LEVEL}", str(BENDER_LEVEL))
        .replace("{BENDER_LEVEL_RULES}", bender_level_rules())
    )

whisper_model = None
whisper_device = "cpu"
whisper_name = ""
piper_voice = None
piper_syn = None
piper_voice_en = None
piper_syn_en = None


def log(msg: str) -> None:
    print(msg, flush=True)


def dumps(obj: dict) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def load_chat() -> list[dict]:
    global CHAT_CONV_ID, CHAT_SUMMARY, CHAT_GROK_RESP_ID
    CHAT_CONV_ID = ""
    CHAT_SUMMARY = ""
    CHAT_GROK_RESP_ID = ""
    if not CHAT_PATH.is_file():
        CHAT_CONV_ID = str(uuid.uuid4())
        return []
    try:
        raw = json.loads(CHAT_PATH.read_text(encoding="utf-8"))
        if isinstance(raw, list):
            msgs = raw
        else:
            raw = raw or {}
            CHAT_CONV_ID = str(raw.get("conv_id") or "").strip()
            CHAT_SUMMARY = str(raw.get("summary") or "").strip()
            CHAT_GROK_RESP_ID = str(raw.get("grok_response_id") or "").strip()
            saved_lv = raw.get("bender_level")
            try:
                saved_lv = int(saved_lv) if saved_lv is not None else None
            except (TypeError, ValueError):
                saved_lv = None
            try:
                saved_rev = int(raw.get("grok_prompt_rev") or 0)
            except (TypeError, ValueError):
                saved_rev = 0
            if saved_lv != BENDER_LEVEL or saved_rev != GROK_PROMPT_REV:
                if CHAT_GROK_RESP_ID:
                    log(
                        f"Grok chain drop: level {saved_lv}→{BENDER_LEVEL} "
                        f"rev {saved_rev}→{GROK_PROMPT_REV}"
                    )
                CHAT_GROK_RESP_ID = ""
            msgs = raw.get("messages", [])
        out: list[dict] = []
        for m in msgs:
            role = m.get("role")
            content = m.get("content")
            if role in ("user", "assistant") and isinstance(content, str) and content.strip():
                out.append({"role": role, "content": content})
        if not CHAT_CONV_ID:
            CHAT_CONV_ID = str(uuid.uuid4())
        return out
    except Exception as e:
        log(f"chat.json skip: {e}")
        CHAT_CONV_ID = str(uuid.uuid4())
        CHAT_GROK_RESP_ID = ""
        return []


def save_chat(history: list[dict]) -> None:
    try:
        payload = {
            "conv_id": CHAT_CONV_ID,
            "grok_response_id": CHAT_GROK_RESP_ID,
            "grok_prompt_rev": GROK_PROMPT_REV,
            "bender_level": BENDER_LEVEL,
            "summary": CHAT_SUMMARY,
            "messages": history[-400:],
        }
        CHAT_PATH.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    except Exception as e:
        log(f"chat.json save: {e}")


def _clip_summary(text: str) -> str:
    t = re.sub(r"\s+", " ", text).strip()
    if len(t) <= SUMMARY_CHARS:
        return t
    return t[-SUMMARY_CHARS:].lstrip()


def fold_old_turns(history: list[dict]) -> None:
    """Локальний лог підрізаємо. Історію для Grok тримає xAI (previous_response_id)."""
    global CHAT_SUMMARY, CHAT_CONV_ID
    if LLM_PROVIDER == "grok":
        if len(history) > 400:
            del history[:-400]
        return
    keep = min(16, max(8, HISTORY_SEND // 3))
    if len(history) <= HISTORY_SEND:
        return
    overflow = history[:-keep]
    lines: list[str] = []
    for m in overflow:
        who = "Ти" if m.get("role") == "user" else "Бендер"
        bit = re.sub(r"\s+", " ", (m.get("content") or ""))[:140]
        if bit:
            lines.append(f"{who}: {bit}")
    blob = " ".join(lines)
    CHAT_SUMMARY = _clip_summary((CHAT_SUMMARY + " " + blob).strip())
    del history[:-keep]
    CHAT_CONV_ID = str(uuid.uuid4())
    log(f"chat fold: лишилось {len(history)} реплік, пам'ять {len(CHAT_SUMMARY)} символів")


def resample_int16(pcm: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate or pcm.size == 0:
        return pcm.astype(np.int16, copy=False)
    n_dst = int(round(pcm.size * dst_rate / src_rate))
    if n_dst < 1:
        return np.zeros(0, dtype=np.int16)
    x_old = np.linspace(0.0, 1.0, pcm.size, endpoint=False)
    x_new = np.linspace(0.0, 1.0, n_dst, endpoint=False)
    y = np.interp(x_new, x_old, pcm.astype(np.float32))
    return np.clip(y, -32767, 32767).astype(np.int16)


def download(url: str, dest: Path) -> None:
    if dest.exists() and dest.stat().st_size > 1000:
        return
    log(f"download {dest.name} …")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "BenderLocal/1.0"})
    with urllib.request.urlopen(req) as src, open(tmp, "wb") as out:
        out.write(src.read())
    tmp.replace(dest)


def _add_cuda_dll_dirs() -> None:
    """CTranslate2 на Windows ищет cublas/cudnn через PATH, не через add_dll_directory."""
    extra: list[Path] = []
    for key in ("CUDA_PATH", "CUDA_PATH_V12_9", "CUDA_PATH_V12_8", "CUDA_PATH_V12_6", "CUDA_PATH_V12_4"):
        v = os.environ.get(key)
        if v:
            extra.append(Path(v) / "bin")
    toolkit = Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA")
    if toolkit.is_dir():
        extra.extend(toolkit.glob("v12*\\bin"))
    for modname in (
        "nvidia.cublas",
        "nvidia.cudnn",
        "nvidia.cuda_nvrtc",
        "nvidia.cuda_runtime",
        "nvidia.cublas.lib",
        "nvidia.cudnn.lib",
    ):
        try:
            mod = __import__(modname, fromlist=["x"])
            root = Path(getattr(mod, "__file__", "") or "").resolve().parent
            if not root.is_dir():
                continue
            extra.append(root)
            extra.append(root / "bin")
            extra.append(root / "lib")
            extra.extend(root.glob("bin/*"))
            extra.extend(root.glob("lib/*"))
        except Exception:
            pass
    try:
        import site

        for sp in site.getsitepackages():
            nvidia = Path(sp) / "nvidia"
            if nvidia.is_dir():
                extra.extend(nvidia.glob("*/bin"))
                extra.extend(nvidia.glob("*/lib"))
                extra.extend(nvidia.glob("*/*/bin"))
    except Exception:
        pass

    seen: set[str] = set()
    prepend: list[str] = []
    for folder in extra:
        p = str(folder)
        if p in seen or not folder.is_dir():
            continue
        if not any(folder.glob("*.dll")):
            continue
        seen.add(p)
        prepend.append(p)
        if hasattr(os, "add_dll_directory"):
            try:
                os.add_dll_directory(p)
            except OSError:
                pass
    if prepend:
        os.environ["PATH"] = os.pathsep.join(prepend) + os.pathsep + os.environ.get("PATH", "")
        log("CUDA DLL PATH: " + "; ".join(prepend))


def _probe_whisper(model) -> None:
    audio = np.zeros(IN_RATE, dtype=np.float32)
    segments, _info = model.transcribe(
        audio, language="uk", beam_size=1, vad_filter=False, without_timestamps=True
    )
    for _ in segments:
        break


def load_whisper():
    global whisper_model, whisper_device, whisper_name
    _add_cuda_dll_dirs()
    from faster_whisper import WhisperModel
    prefer = os.environ.get("WHISPER_DEVICE", "auto").strip().lower()
    attempts: list[tuple[str, str]] = []
    if prefer == "cpu":
        attempts = [("cpu", "int8")]
    elif prefer == "cuda":
        attempts = [("cuda", "float16"), ("cpu", "int8")]
    else:
        attempts = [("cuda", "float16"), ("cpu", "int8")]

    last = None
    for device, ctype in attempts:
        model = None
        # large-v3-turbo на CPU съедает 15–20 с — колонка вешает сессию.
        name = WHISPER_MODEL or ("large-v3-turbo" if device == "cuda" else "medium")
        try:
            log(f"Whisper {name} device={device}")
            model = WhisperModel(name, device=device, compute_type=ctype)
            if device == "cuda":
                log("Whisper CUDA probe (нужен cublas64_12.dll)…")
                _probe_whisper(model)
            whisper_model = model
            whisper_device = device
            whisper_name = name
            log(f"Whisper OK ({device}, {name})")
            if device == "cpu":
                log("STT на CPU (medium). GPU STT: CUDA Toolkit 12")
            return
        except Exception as e:
            last = e
            whisper_model = None
            model = None
            log(f"Whisper {device} fail: {e}")
    raise RuntimeError(f"faster-whisper не запустился: {last}")


def load_piper():
    global piper_voice, piper_syn, piper_voice_en, piper_syn_en
    download(VOICE_BASE, VOICE_ONNX)
    download(VOICE_BASE + ".json", VOICE_JSON)
    piper_voice = PiperVoice.load(str(VOICE_ONNX))
    names = {int(i): n for n, i in (piper_voice.config.speaker_id_map or {}).items()}
    who = names.get(PIPER_SPEAKER, f"id{PIPER_SPEAKER}")
    piper_syn = SynthesisConfig(
        speaker_id=PIPER_SPEAKER,
        length_scale=PIPER_LENGTH,
        noise_scale=0.62,
        noise_w_scale=0.80,
    )
    log(f"Piper OK (uk_UA {who}, length={PIPER_LENGTH})")
    load_stress_words()
    load_uk_stress()
    try:
        download(VOICE_EN_BASE, VOICE_EN_ONNX)
        download(VOICE_EN_BASE + ".json", VOICE_EN_JSON)
        piper_voice_en = PiperVoice.load(str(VOICE_EN_ONNX))
        piper_syn_en = SynthesisConfig(
            length_scale=PIPER_LENGTH,
            noise_scale=0.62,
            noise_w_scale=0.80,
        )
        log("Piper OK (en_US ryan)")
    except Exception as e:
        piper_voice_en = None
        piper_syn_en = None
        log(f"Piper EN skip: {e}")


_UK_ONES = (
    "нуль", "один", "два", "три", "чотири", "п'ять", "шість", "сім", "вісім", "дев'ять",
)
_UK_TEENS = (
    "десять", "одинадцять", "дванадцять", "тринадцять", "чотирнадцять",
    "п'ятнадцять", "шістнадцять", "сімнадцять", "вісімнадцять", "дев'ятнадцять",
)
_UK_TENS = (
    "", "десять", "двадцять", "тридцять", "сорок", "п'ятдесят",
    "шістдесят", "сімдесят", "вісімдесят", "дев'яносто",
)
_UK_HUND = (
    "", "сто", "двісті", "триста", "чотириста", "п'ятсот",
    "шістсот", "сімсот", "вісімсот", "дев'ятсот",
)
_EN_ONES = (
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
)
_EN_TEENS = (
    "ten", "eleven", "twelve", "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
)
_EN_TENS = (
    "", "ten", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
)
_LAT_UK = {
    "ok": "окей", "okay": "окей", "wifi": "вай фай", "wi-fi": "вай фай",
    "grok": "грок", "bender": "бендер", "planet": "планет", "express": "експрес",
    "rvc": "ер ві сі", "ai": "ей ай", "cpu": "сі пі ю", "gpu": "джі пі ю",
    "usb": "ю ес бі", "led": "лед", "http": "ейч ті ті пі", "https": "ейч ті ті пі ес",
    "www": "ве ве ве", "ip": "ай пі", "ssid": "ес ес ай ді", "tts": "ті ті ес",
    "stt": "ес ті ті", "llm": "ел ел ем", "futurama": "футурама", "fry": "фрай",
    "leela": "ліла", "zoidberg": "зойдберг", "hello": "хелоу", "yes": "єс", "no": "ноу",
}
_UK_LAT_LETTER = {
    "a": "а", "b": "бе", "c": "сі", "d": "ді", "e": "і", "f": "еф", "g": "джі",
    "h": "ейч", "i": "ай", "j": "джей", "k": "кей", "l": "ел", "m": "ем", "n": "ен",
    "o": "о", "p": "пі", "q": "кью", "r": "ар", "s": "ес", "t": "ті", "u": "ю",
    "v": "ві", "w": "дабл ю", "x": "екс", "y": "вай", "z": "зі",
}
# Шкільні назви літер. Гола «б» у Piper — не «бе», а шум/тиша (алфавіт звучав як «а а а»).
_UK_CYR_LETTER = {
    "а": "а", "б": "бе", "в": "ве", "г": "ге", "ґ": "ґе", "д": "де",
    "е": "е", "є": "є", "ж": "же", "з": "зе", "и": "и", "і": "і",
    "ї": "ї", "й": "йот", "к": "ка", "л": "ел", "м": "ем", "н": "ен",
    "о": "о", "п": "пе", "р": "ер", "с": "ес", "т": "те", "у": "у",
    "ф": "еф", "х": "ха", "ц": "це", "ч": "че", "ш": "ша", "щ": "ща",
    "ь": "м'який знак", "ю": "ю", "я": "я",
}
_LETTER_CHAR = r"[A-Za-zА-Яа-яІіЇїЄєҐґЬь]"
_LETTER_LIST_SEP = r"(?:\s*[,;]\s*|\s*\.\s+)"
_SPELLED_LETTERS = re.compile(
    rf"(?<!{_LETTER_CHAR})(?:{_LETTER_CHAR}{_LETTER_LIST_SEP}){{2,}}{_LETTER_CHAR}\.?(?!{_LETTER_CHAR})"
)
_SPACE_LETTER_RUN = re.compile(
    rf"(?<!\S){_LETTER_CHAR}(?:\s+{_LETTER_CHAR}){{4,}}(?!\S)"
)


def _letter_name(ch: str) -> str:
    c = ch.lower()
    if c in _UK_CYR_LETTER:
        return _UK_CYR_LETTER[c]
    return _UK_LAT_LETTER.get(c, c)


def _expand_letter_run(blob: str) -> str:
    chars = re.findall(_LETTER_CHAR, blob)
    if len(chars) < 3:
        return blob
    return ", ".join(_letter_name(c) for c in chars)


def _expand_spelled_letters(text: str) -> str:
    """А, Б, В… → а, бе, ве… Інакше кожна літера йде в Piper окремо і зникає."""
    t = _SPELLED_LETTERS.sub(lambda m: _expand_letter_run(m.group(0)), text)
    t = _SPACE_LETTER_RUN.sub(lambda m: _expand_letter_run(m.group(0)), t)
    return t


def _uk_under_100(n: int) -> str:
    if n < 10:
        return _UK_ONES[n]
    if n < 20:
        return _UK_TEENS[n - 10]
    tens, ones = divmod(n, 10)
    return _UK_TENS[tens] if ones == 0 else f"{_UK_TENS[tens]} {_UK_ONES[ones]}"


def _uk_under_1000(n: int) -> str:
    if n < 100:
        return _uk_under_100(n)
    h, rest = divmod(n, 100)
    return _UK_HUND[h] if rest == 0 else f"{_UK_HUND[h]} {_uk_under_100(rest)}"


def _uk_thou_word(n: int) -> str:
    n = n % 100
    if 11 <= n <= 14:
        return "тисяч"
    r = n % 10
    if r == 1:
        return "тисяча"
    if r in (2, 3, 4):
        return "тисячі"
    return "тисяч"


def _uk_int(n: int) -> str:
    if n < 0:
        return "мінус " + _uk_int(-n)
    if n < 1000:
        return _uk_under_1000(n)
    if n >= 1_000_000_000:
        return " ".join(_UK_ONES[int(d)] for d in str(n))
    if n >= 1_000_000:
        mil, rest = divmod(n, 1_000_000)
        if mil == 1:
            head = "один мільйон"
        elif mil == 2:
            head = "два мільйони"
        else:
            mill = mil % 100
            r = mil % 10
            form = "мільйонів" if 11 <= mill <= 14 or r == 0 or r >= 5 else (
                "мільйон" if r == 1 else "мільйони"
            )
            head = f"{_uk_under_1000(mil)} {form}"
        return head if rest == 0 else f"{head} {_uk_int(rest)}"
    th, rest = divmod(n, 1000)
    if th == 1:
        head = "одна тисяча"
    elif th == 2:
        head = "дві тисячі"
    else:
        head = f"{_uk_under_1000(th)} {_uk_thou_word(th)}"
    return head if rest == 0 else f"{head} {_uk_under_1000(rest)}"


def _en_under_100(n: int) -> str:
    if n < 10:
        return _EN_ONES[n]
    if n < 20:
        return _EN_TEENS[n - 10]
    tens, ones = divmod(n, 10)
    return _EN_TENS[tens] if ones == 0 else f"{_EN_TENS[tens]} {_EN_ONES[ones]}"


def _en_int(n: int) -> str:
    if n < 0:
        return "minus " + _en_int(-n)
    if n < 100:
        return _en_under_100(n)
    if n < 1000:
        h, rest = divmod(n, 100)
        return f"{_EN_ONES[h]} hundred" if rest == 0 else f"{_EN_ONES[h]} hundred {_en_under_100(rest)}"
    if n >= 1_000_000:
        return " ".join(_EN_ONES[int(d)] for d in str(n))
    th, rest = divmod(n, 1000)
    head = f"{_en_int(th)} thousand"
    return head if rest == 0 else f"{head} {_en_int(rest)}"


def _speak_num_token(raw: str, *, uk: bool) -> str:
    to_int = _uk_int if uk else _en_int
    ones = _UK_ONES if uk else _EN_ONES
    dot = " крапка " if uk else " point "
    s = raw.replace(",", ".")
    if re.fullmatch(r"\d{1,3}(?:\.\d{1,3}){3}", s):
        return dot.join(to_int(int(p)) for p in s.split("."))
    if "." in s:
        a, b = s.split(".", 1)
        if a.isdigit() and b.isdigit():
            return to_int(int(a)) + dot + (
                to_int(int(b)) if len(b) <= 3 else " ".join(ones[int(d)] for d in b)
            )
    if s.isdigit():
        if len(s) >= 8:
            return " ".join(ones[int(d)] for d in s)
        return to_int(int(s))
    return raw


def _expand_numbers(text: str, *, uk: bool) -> str:
    t = re.sub(
        r"(\d+(?:[.,]\d+)*)\s*%",
        lambda m: _speak_num_token(m.group(1), uk=uk) + (" відсотків" if uk else " percent"),
        text,
    )
    t = re.sub(
        r"\d{1,3}(?:[.,]\d{1,3}){3}|\d+(?:[.,]\d+)?",
        lambda m: _speak_num_token(m.group(0).replace(",", "."), uk=uk),
        t,
    )
    return t


def _translit_en_uk(word: str) -> str:
    s = word.lower()
    for a, b in (
        ("tion", "шн"),
        ("tch", "ч"),
        ("sch", "ш"),
        ("sh", "ш"),
        ("ch", "ч"),
        ("th", "т"),
        ("ph", "ф"),
        ("ck", "к"),
        ("qu", "кв"),
        ("ee", "і"),
        ("oo", "у"),
        ("wh", "в"),
    ):
        s = s.replace(a, b)
    s = s.replace("x", "кс")
    return s.translate(str.maketrans({
        "a": "а", "b": "б", "c": "к", "d": "д", "e": "е", "f": "ф", "g": "г",
        "h": "х", "i": "і", "j": "дж", "k": "к", "l": "л", "m": "м", "n": "н",
        "o": "о", "p": "п", "q": "к", "r": "р", "s": "с", "t": "т", "u": "у",
        "v": "в", "w": "в", "y": "і", "z": "з",
    }))


def _latin_to_uk(word: str) -> str:
    low = word.lower()
    if low in _LAT_UK:
        return _LAT_UK[low]
    letters = re.sub(r"[^A-Za-z]", "", word)
    if not letters:
        return word
    if letters.isupper() and 2 <= len(letters) <= 6:
        return " ".join(_UK_LAT_LETTER.get(c.lower(), c) for c in letters)
    if len(letters) <= 3:
        return " ".join(_UK_LAT_LETTER.get(c.lower(), c) for c in letters)
    return _translit_en_uk(letters)


_ACUTE = "\u0301"
_UK_VOWELS = set("аеєиіїоуюя")
_UK_WORD = re.compile(r"[а-яіїєґ'\u0301]+")
_uk_stress = None
_STRESS_WORDS_PATH = HERE / "stress_words.json"
# Запас, якщо JSON немає. Файл перекриває ці ключі.
_STRESS_FIX_BUILTIN = {
    "бендер": "бе" + _ACUTE + "ндер",
    "бендера": "бе" + _ACUTE + "ндера",
    "бендеру": "бе" + _ACUTE + "ндеру",
    "бендером": "бе" + _ACUTE + "ндером",
    "бендері": "бе" + _ACUTE + "ндері",
    "хлапалку": "хлапа" + _ACUTE + "лку",
    "хлапалка": "хлапа" + _ACUTE + "лка",
    "хлапалки": "хлапа" + _ACUTE + "лки",
}
_STRESS_FIX: dict[str, str] = dict(_STRESS_FIX_BUILTIN)


def _stress_on_vowel(word: str, n: int) -> str:
    """n — номер голосної (1 = перша). Знак ́ ставиться одразу після неї."""
    w = (word or "").replace(_ACUTE, "")
    if n < 1:
        return w
    seen = 0
    out: list[str] = []
    for ch in w:
        out.append(ch)
        if ch in _UK_VOWELS:
            seen += 1
            if seen == n:
                out.append(_ACUTE)
    return "".join(out)


def _stress_fix_value(word: str, spec) -> str | None:
    key = (word or "").lower().replace(_ACUTE, "").strip()
    if not key:
        return None
    if isinstance(spec, bool) or spec is None:
        return None
    if isinstance(spec, (int, float)):
        n = int(spec)
        if n < 1:
            return None
        return _stress_on_vowel(key, n)
    s = str(spec).lower().strip()
    if not s:
        return None
    if s.isdigit():
        return _stress_on_vowel(key, int(s))
    return s


def load_stress_words() -> None:
    """Свої наголоси з stress_words.json (перекривають словник Stanza)."""
    global _STRESS_FIX
    out = dict(_STRESS_FIX_BUILTIN)
    if _STRESS_WORDS_PATH.is_file():
        try:
            raw = json.loads(_STRESS_WORDS_PATH.read_text(encoding="utf-8"))
            n = 0
            if isinstance(raw, dict):
                for k, v in raw.items():
                    if str(k).startswith("_"):
                        continue
                    val = _stress_fix_value(str(k), v)
                    if val:
                        out[str(k).lower().replace(_ACUTE, "").strip()] = val
                        n += 1
            log(f"stress_words.json {n} words")
        except Exception as e:
            log(f"stress_words.json skip: {e}")
    _STRESS_FIX = out


def load_uk_stress() -> None:
    """POS у процесі Applio (Stanza CPU) + словник як запасний шлях без torch."""
    global _uk_stress
    if _uk_stress is not None:
        return
    try:
        how = stress_convert.start()
        log(f"UK stress OK ({how})")
    except Exception as e:
        log(f"UK stress worker skip: {e}")
    try:
        from ukrainian_word_stress import Stressifier, StressSymbol

        kwargs = {
            "stress_symbol": StressSymbol.CombiningAcuteAccent,
            "on_ambiguity": "skip",
        }
        try:
            from ukrainian_word_stress import Disambiguation

            kwargs["disambiguation"] = Disambiguation.Dictionary
        except Exception:
            pass
        _uk_stress = Stressifier(**kwargs)
        if not stress_convert.available():
            log("UK stress OK (dictionary fallback)")
    except Exception as e:
        _uk_stress = False
        if not stress_convert.available():
            log(f"UK stress skip: {e}")


def _keep_first_acute(word: str) -> str:
    i = word.find(_ACUTE)
    if i < 0:
        return word
    return word[: i + 1] + word[i + 1 :].replace(_ACUTE, "")


def apply_uk_stress(text: str) -> str:
    if not text:
        return text
    if _ACUTE not in text:
        if stress_convert.available():
            try:
                text = stress_convert.stress(text)
            except Exception as e:
                log(f"UK stress worker: {e}")
                if _uk_stress:
                    try:
                        text = _uk_stress(text)
                    except Exception:
                        pass
        elif _uk_stress:
            try:
                text = _uk_stress(text)
            except Exception:
                pass

    def one(m: re.Match[str]) -> str:
        w = m.group(0)
        base = w.replace(_ACUTE, "")
        if base in _STRESS_FIX:
            return _STRESS_FIX[base]
        return _keep_first_acute(w)

    return _UK_WORD.sub(one, text)


def piper_ready_uk(text: str) -> str:
    """Piper uk map лише нижній регістр — інакше 'Missing phoneme: П'."""
    t = unicodedata.normalize("NFC", text)
    t = t.replace("ё", "е").replace("Ё", "е")
    t = t.replace("ы", "и").replace("Ы", "и")
    t = t.replace("э", "е").replace("Э", "е")
    t = t.replace("ъ", "").replace("Ъ", "")
    t = t.lower()
    t = t.replace('"', " ").replace("«", " ").replace("»", " ").replace("„", " ").replace("“", " ").replace("”", " ")
    for apos in ("\u2019", "\u2018", "\u02bc", "\u02b9", "\u0060"):
        t = t.replace(apos, "'")
    # Piper на «—» ставить довгу паузу, як на крапці. Тире = кома.
    t = re.sub(r"\s*[—–−]+\s*", ", ", t)
    t = re.sub(r"\s+-\s+", ", ", t)
    t = _expand_numbers(t, uk=True)
    t = re.sub(r"[a-z]+(?:-[a-z]+)*", lambda m: _latin_to_uk(m.group(0)), t)
    t = re.sub(r"[a-z]+", " ", t)
    t = _expand_spelled_letters(t)
    t = re.sub(r"[\u3400-\u9fff\u3040-\u30ff\uac00-\ud7af]+", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    t = apply_uk_stress(t)
    return t or "не розчув. повтори."


def piper_ready_en(text: str) -> str:
    t = unicodedata.normalize("NFC", text)
    t = re.sub(r"\s*[—–−]+\s*", ", ", t)
    t = re.sub(r"\s+-\s+", ", ", t)
    t = _expand_numbers(t, uk=False)
    t = re.sub(r"[\u0400-\u04ff]+", " ", t)
    t = re.sub(r"[\u3400-\u9fff\u3040-\u30ff\uac00-\ud7af]+", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t or "Say that again, meatbag."


def _latin_letter_share(text: str) -> float:
    letters = [ch for ch in text if ch.isalpha()]
    if not letters:
        return 0.0
    return sum(ch.isascii() for ch in letters) / len(letters)


def piper_ready(text: str) -> str:
    return piper_ready_uk(text)


def _edit_dist(a: str, b: str, cap: int = 1) -> int:
    if abs(len(a) - len(b)) > cap:
        return cap + 1
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        row_min = i
        for j, cb in enumerate(b, 1):
            v = min(cur[j - 1] + 1, prev[j] + 1, prev[j - 1] + (ca != cb))
            cur.append(v)
            if v < row_min:
                row_min = v
        if row_min > cap:
            return cap + 1
        prev = cur
    return prev[-1]


_BENDER_FORMS = (
    "бендер", "бендера", "бендеру", "бендером", "бендері", "бендери",
    "bender", "bendera", "benderu",
)
# Слова на -ендер, які існують. У звертанні («Тендер, привіт») усе одно Бендер.
_BENDER_KEEP = re.compile(
    r"(?iu)^(тендер|гендер|рендер|ренджер|рейнджер|фендер|сендер|лендер|"
    r"tender|gender|render|ranger|fender|sender|lender)"
)
_BENDER_STEM = re.compile(r"(?iu)ндер|nder")


def _asr_bender_form(word: str) -> str | None:
    w = word.lower().replace("ё", "е").replace("’", "").replace("дж", "д")
    if not _BENDER_STEM.search(w):
        return None
    for form in _BENDER_FORMS:
        if _edit_dist(w, form, 1) <= 1:
            return form[0].upper() + form[1:]
    return None


# Whisper часто злипає «Бендерпривіт» без пробілу.
_ASR_GLUE_TAIL = (
    r"привіт|привет|здрастуй|здравствуй|здарова|вітаю|"
    r"алло|ало|слухай|скажи|розкажи|hello|\bhi\b|"
    r"як|що|ти|там|бля|йо|гей|добрий|доброго|"
    r"титут|тытут|ятут|тыздесь|яздесь"
)


def _unglue_bender(text: str) -> str:
    def one(m: re.Match[str]) -> str:
        w = m.group(0)
        low = w.lower()
        for n in range(8, 5, -1):
            if len(low) <= n:
                continue
            head, tail = low[:n], low[n:]
            mapped = _asr_bender_form(head)
            if mapped and not _BENDER_KEEP.match(head) and re.fullmatch(
                rf"(?iu){_ASR_GLUE_TAIL}", tail
            ):
                return f"{mapped}, {tail}"
        for n in range(8, 5, -1):
            if len(low) <= n:
                continue
            head, tail = low[:-n], low[-n:]
            mapped = _asr_bender_form(tail)
            if mapped and not _BENDER_KEEP.match(tail) and re.fullmatch(
                rf"(?iu){_ASR_GLUE_TAIL}", head
            ):
                return f"{head}, {mapped}"
        return w

    return re.sub(r"(?iu)\b[a-zа-яёіїєґ]{8,24}\b", one, text)


def _asr_bender_vocative(text: str, start: int, end: int) -> bool:
    left = text[:start]
    right = text[end:]
    if re.match(r"^\s*[,!]", right):
        prefix = left.strip()
        if prefix == "" or prefix[-1:] in ".!?…":
            return True
    if re.search(r"(?iu)(?:привіт|привет|хай|алло|слухай|йо)\s*[,\s]*$", left):
        return True
    if re.match(r"(?iu)\s*[,:]?\s*(?:привіт|привет|хай|алло)", right):
        return True
    return False


def _unglue_short(text: str) -> str:
    """Whisper злипає короткі слова: «ты тут» → «титут»."""
    pairs = (
        (r"(?iu)\bтытут\b", "ты тут"),
        (r"(?iu)\bтитут\b", "ти тут"),
        (r"(?iu)\bтитуть\b", "ти тут"),
        (r"(?iu)\bтытуть\b", "ты тут"),
        (r"(?iu)\bтитутт\b", "ти тут"),
        (r"(?iu)\bятут\b", "я тут"),
        (r"(?iu)\bтыздесь\b", "ты здесь"),
        (r"(?iu)\bяздесь\b", "я здесь"),
        (r"(?iu)\bтытам\b", "ты там"),
        (r"(?iu)\bтитам\b", "ти там"),
        (r"(?iu)\bтыгде\b", "ты где"),
        (r"(?iu)\bтиде\b", "ти де"),
        (r"(?iu)\bатут\b", "а тут"),
        (r"(?iu)\bнукак\b", "ну как"),
        (r"(?iu)\bнуяк\b", "ну як"),
    )
    t = text
    for pat, repl in pairs:
        t = re.sub(pat, repl, t)
    return t


_HERE_ASR_GARBAGE = re.compile(
    r"(?iu)^(титул|титуль|тытул|титула|титулі|ти\s*тул|ты\s*тул|title)$"
)


def _fix_here_ping(text: str) -> str:
    """Whisper часто пише «титул» замість «ты тут»."""
    t = re.sub(r"\s+", " ", text).strip()
    core = re.sub(r"(?iu)\b(?:бендер|bender)\b", " ", t)
    core = re.sub(r"[\s.,!?…:;«»\"'\-—]+", " ", core).strip()
    if _HERE_ASR_GARBAGE.fullmatch(core):
        return "Ты тут, Бендер?"
    return t


def _fix_asr(text: str) -> str:
    t = re.sub(r"\s+", " ", text).strip()
    t = re.sub(r"(?iu)\b[бдптвгклмн]ен\s+дер\b", "Бендер", t)
    t = _unglue_bender(t)
    t = _unglue_short(t)
    t = _fix_here_ping(t)

    def one(m: re.Match[str]) -> str:
        w = m.group(0)
        mapped = _asr_bender_form(w)
        if not mapped:
            return w
        folded = w.lower().replace("ё", "е").replace("дж", "д")
        if (_BENDER_KEEP.match(w) or _BENDER_KEEP.match(folded)) and not _asr_bender_vocative(
            t, m.start(), m.end()
        ):
            return w
        return mapped

    t = re.sub(r"(?iu)\b[a-zа-яёіїєґ]{4,12}\b", one, t)
    t = re.sub(r"(?iu)\bбендж+ер\b", "Бендер", t)
    t = re.sub(r"(?iu)\bбенждер\b", "Бендер", t)
    t = re.sub(r"(?i)\bгербал\b", "Бендер", t)
    t = re.sub(r"(?i)\bвзаучило\b", "звучало", t)
    t = re.sub(r"(?iu)\bприєт\b", "привет", t)
    t = re.sub(r"(?iu)\bбендер(?:[\s,]+бендер)+\b", "Бендер", t)
    t = re.sub(r"(?iu)\bbender(?:[\s,]+bender)+\b", "Бендер", t)
    t = _fix_here_ping(t)
    if re.fullmatch(r"(?iu)[\s.,!?]*бендер[\s.,!?]*", t):
        t = "Привіт, Бендер."
    return t.strip()


def _pcm_rms(pcm16: bytes) -> float:
    a = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32)
    if a.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(a * a)))


def _greet_token_count(text: str) -> int:
    return len(re.findall(rf"(?iu)\b({_GREET_WORDS})\b", text or ""))


def _only_greet_words(text: str) -> bool:
    leftover = re.sub(rf"(?iu)\b({_GREET_WORDS})\b", "", text or "")
    leftover = re.sub(r"[\s.,!?…;:\-—\"'«»()]+", "", leftover)
    return leftover == "" and _greet_token_count(text) > 0


_YT_OUTRO_RE = re.compile(
    r"(?iu)("
    r"дякую за перегляд|"
    r"спасибо за просмотр|"
    r"thanks for watching|"
    r"thank you for watching|"
    r"please subscribe|"
    r"like and subscribe|"
    r"підписуйтесь|"
    r"подписывайтесь|"
    r"ставте лайк|"
    r"до зустрічі в наступному|"
    r"до встречи в следующем|"
    r"продолжение следует|"
    r"продовження слідує|"
    r"субтитры|"
    r"субтитри|"
    r"редактор(?:ы|а|и)?\s+субтит|"
    r"корректор|"
    r"коректор|"
    r"переводчик|"
    r"перекладач|"
    r"семкин|"
    r"егорова|"
    r"аплодисменты|"
    r"аплодисменти|"
    r"\[музыка\]|"
    r"\[музика\]|"
    r"subtitles|"
    r"текст чита[еє]|"
    r"озвучи[лв]|"
    r"translated by|"
    r"transcribed by"
    r")"
)

# Кінцівка DVD: «Редактор субтитров А.Семкин Корректор А.Егорова»
_SUB_CREDIT_RE = re.compile(
    r"(?iu)"
    r"(?:редактор(?:ы|а|и)?\s+субтит\w*|"
    r"корректор(?:ы|а)?|"
    r"коректор(?:и|а)?|"
    r"переводчик\w*|"
    r"перекладач\w*|"
    r"субтитры\s+(?:сделал|сделала)|"
    r"субтитри\s+зробив|"
    r"текст\s+чита[еє]\w*|"
    r"озвучи[лв]\w*)"
    r"(?:\s+[A-ZА-ЯІЇЄҐ]\.\s*[A-ZА-ЯІЇЄҐ][a-zа-яёіїєґ'\-]*)*"
)
_INITIALS_NAME_RE = re.compile(
    r"(?u)\b[А-ЯA-ZІЇЄҐ]\.\s*[А-ЯA-ZІЇЄҐ][а-яёіїєґa-z'\-]{2,}"
)
# Саме «Дякую!» / «Thanks.» без іншого тексту — типова галюцинація Whisper, не реальна фраза.
_THANKS_ONLY_RE = re.compile(
    r"(?iu)^[\s.,!?«»\"'…\-—]*"
    r"(?:дуже\s+|велике\s+|большое\s+)?"
    r"(?:дякую|спасибі|спасибо|thanks|thank\s+you|thx)"
    r"(?:\s+(?:тобі|вам|большое|дуже))?"
    r"[\s.,!?«»\"'…\-—]*$"
)
_last_halluc_norm = ""


def _norm_asr_blob(text: str) -> str:
    t = (text or "").lower().replace("ё", "е")
    t = re.sub(r"[^\wіїєґ]+", " ", t)
    return re.sub(r"\s+", " ", t).strip()


def _looks_like_credits(text: str) -> bool:
    t = re.sub(r"\s+", " ", text or "").strip()
    if not t:
        return False
    if _YT_OUTRO_RE.search(t) or _THANKS_ONLY_RE.match(t) or _SUB_CREDIT_RE.search(t):
        return True
    if len(_INITIALS_NAME_RE.findall(t)) >= 2:
        return True
    return False


def _scrub_hallucination(text: str) -> str:
    t = re.sub(r"\s+", " ", text or "").strip()
    if not t:
        return ""
    t = _SUB_CREDIT_RE.sub(" ", t)
    t = _INITIALS_NAME_RE.sub(" ", t)
    t = re.sub(r"\s+", " ", t).strip(" .,;:—-")
    if _looks_like_credits(t):
        return ""
    return t


def _note_hallucination(text: str) -> None:
    global _last_halluc_norm
    n = _norm_asr_blob(text)
    if n:
        _last_halluc_norm = n


def _is_hallucination(text: str, avg_lp: float, rms: float) -> bool:
    t = re.sub(r"\s+", " ", text or "").strip()
    if _looks_like_credits(t):
        return True
    if re.search(
        r"(?iu)^(продолжение следует|продовження слідує|thanks for watching|"
        r"subscribe|субтитры|аплодисменты|\[музыка\]|music)$",
        t,
    ):
        return True
    if _last_halluc_norm and _norm_asr_blob(t) == _last_halluc_norm:
        return True
    if not _only_greet_words(t):
        return False
    n = _greet_token_count(t)
    if n >= 3 and rms < 4000:
        return True
    return False


def _asr_ok(
    text: str,
    avg_lp: float,
    no_speech: float,
    rms: float,
) -> bool:
    letters = sum(ch.isalpha() for ch in text)
    if letters < 4:
        return False
    if rms < ASR_MIN_RMS:
        return False
    if avg_lp < ASR_MIN_LOGPROB:
        loud = rms >= 3500 and no_speech <= 0.25 and avg_lp >= -0.85
        if not loud:
            return False
    if no_speech > ASR_MAX_NO_SPEECH:
        return False
    if _is_hallucination(text, avg_lp, rms):
        return False
    letters = [ch for ch in text if ch.isalpha()]
    if letters and (sum(ch.isupper() for ch in letters) / len(letters)) >= 0.85:
        return False
    return True


def _asr_enhance(audio: np.ndarray, sr: int = IN_RATE) -> np.ndarray:
    """М'який Wiener. Сильне віднімання дає «воду» / musical noise."""
    x = np.asarray(audio, dtype=np.float32)
    if x.size < 2048:
        return x
    x = x - float(x.mean())
    x = np.concatenate([x[:1], x[1:] - 0.75 * x[:-1]])
    n_fft, hop = 512, 160
    win = np.hanning(n_fft).astype(np.float32)
    pad = (hop - (x.size - n_fft) % hop) % hop
    xp = np.pad(x, (0, int(pad) + n_fft))
    frames = np.lib.stride_tricks.sliding_window_view(xp, n_fft)[::hop]
    spec = np.fft.rfft(frames * win, axis=1)
    mag = np.abs(spec)
    energy = mag.sum(axis=1)
    k = max(2, int(0.12 * energy.size))
    quiet = np.argpartition(energy, k)[:k]
    noise = np.median(mag[quiet], axis=0) + 1e-6
    gain = np.clip(1.0 - 0.65 * (noise / (mag + 1e-6)), 0.45, 1.0)
    if gain.shape[1] >= 3:
        gain = (
            np.concatenate([gain[:, :1], gain[:, :-1]], axis=1)
            + gain
            + np.concatenate([gain[:, 1:], gain[:, -1:]], axis=1)
        ) / 3.0
    smooth = np.empty_like(gain)
    prev = gain[0]
    for i in range(gain.shape[0]):
        prev = 0.65 * prev + 0.35 * gain[i]
        smooth[i] = prev
    hz = np.fft.rfftfreq(n_fft, 1.0 / sr)
    smooth[:, hz < 70] *= 0.55
    rec = np.fft.irfft(spec * smooth, n=n_fft, axis=1) * win
    out = np.zeros(xp.size, dtype=np.float32)
    wsum = np.zeros(xp.size, dtype=np.float32)
    for i, fr in enumerate(rec):
        a = i * hop
        out[a : a + n_fft] += fr
        wsum[a : a + n_fft] += win
    y = out[: x.size] / np.maximum(wsum[: x.size], 1e-6)
    rms = float(np.sqrt(np.mean(y * y))) + 1e-8
    y *= min(0.14 / rms, 3.5)
    return np.clip(y, -1.0, 1.0)


_last_whisper_pcm = b""


def _pcm_for_whisper(pcm16: bytes) -> np.ndarray:
    audio = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32) / 32768.0
    return _asr_enhance(audio, IN_RATE)


def _whisper_once(audio, language: str | None, *, use_vad: bool = True) -> tuple[str, str, float, float, float]:
    kw = dict(
        language=language,
        beam_size=5,
        best_of=5,
        patience=1.2,
        # 0.0 лише — немає запасного проходу, якщо перший зліпив слова.
        temperature=(0.0, 0.2, 0.4),
        repetition_penalty=1.15,
        no_repeat_ngram_size=3,
        vad_filter=use_vad,
        # True прибирає timestamp-токени — Whisper тоді частіше пише «Бендерпривіт».
        without_timestamps=False,
        condition_on_previous_text=False,
        compression_ratio_threshold=2.4,
        log_prob_threshold=-0.8,
        no_speech_threshold=0.6,
        hotwords=ASR_HOTWORDS,
    )
    if use_vad:
        kw["vad_parameters"] = {
            "threshold": 0.42,
            "min_silence_duration_ms": 350,
            "speech_pad_ms": 220,
        }
    prompt = ASR_PROMPTS.get(language or "", ASR_PROMPT)
    if prompt:
        kw["initial_prompt"] = prompt
    segments, info = whisper_model.transcribe(audio, **kw)
    segs = list(segments)
    kept = []
    for s in segs:
        tx = (s.text or "").strip()
        if not tx:
            continue
        if _looks_like_credits(tx):
            log(f"ASR drop segment: {tx!r}")
            _note_hallucination(tx)
            continue
        kept.append(s)
    segs = kept
    text = " ".join(s.text.strip() for s in segs if s.text).strip()
    if prompt:
        leak = prompt.split(".")[0].strip()
        if leak and text.lower().startswith(leak.lower()):
            text = text[len(leak) :].lstrip(" .,;:—-")
    lang = (getattr(info, "language", None) or (language or "ru")).lower()[:2]
    lang_p = float(getattr(info, "language_probability", 0.0) or 0.0)
    lps = [float(s.avg_logprob) for s in segs if getattr(s, "avg_logprob", None) is not None]
    nss = [float(s.no_speech_prob) for s in segs if getattr(s, "no_speech_prob", None) is not None]
    avg_lp = float(np.mean(lps)) if lps else -2.0
    no_speech = float(np.mean(nss)) if nss else 1.0
    return text, lang, lang_p, avg_lp, no_speech


def _asr_pass(audio, language: str, *, use_vad: bool, rms: float):
    text, lang, lang_p, avg_lp, no_speech = _whisper_once(
        audio, language, use_vad=use_vad
    )
    text = _fix_asr(text)
    text = _scrub_hallucination(text)
    if not text or _looks_like_credits(text) or _THANKS_ONLY_RE.match(text):
        if text:
            _note_hallucination(text)
            log(f"ASR drop hallucination: {text!r}")
        return "", language, lang_p, avg_lp, no_speech, False
    ok = _asr_ok(text, avg_lp, no_speech, rms)
    if not ok and _is_hallucination(text, avg_lp, rms):
        _note_hallucination(text)
    return text, language, lang_p, avg_lp, no_speech, ok


def transcribe(pcm16: bytes) -> tuple[str, str, float, bool]:
    if not pcm16 or len(pcm16) < IN_RATE:
        return "", "ru", 0.0, False
    rms = _pcm_rms(pcm16)
    if rms < ASR_MIN_RMS:
        log(f"ASR drop rms={rms:.0f} (тиша)")
        return "", "ru", 0.0, False
    audio = _pcm_for_whisper(pcm16)
    global _last_whisper_pcm
    _last_whisper_pcm = np.clip(np.round(audio * 32767.0), -32767, 32767).astype(np.int16).tobytes()
    raw = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32) / 32768.0
    best = None
    for lang_try in ASR_LANGS:
        try:
            got = _asr_pass(audio, lang_try, use_vad=True, rms=rms)
        except Exception as e:
            log(f"ASR VAD fail ({lang_try}), retry raw: {e}")
            got = _asr_pass(raw, lang_try, use_vad=False, rms=rms)
        text, lang, lang_p, avg_lp, no_speech, ok = got
        log(
            f"ASR lang={lang} lang_p={lang_p:.2f} q={avg_lp:.2f} "
            f"ns={no_speech:.2f} rms={rms:.0f} ok={ok}"
        )
        cand = (ok, avg_lp, text, lang, lang_p)
        if best is None or cand[:2] > best[:2]:
            best = cand
        if ok:
            break
        # Порожньо (тиша/титри) — другий язик лише тримає GPU і зриває колонку.
        if not text:
            break
    if best is None:
        return "", "ru", 0.0, False
    ok, _avg_lp, text, lang, lang_p = best
    if not text or _looks_like_credits(text):
        if text:
            _note_hallucination(text)
            log(f"ASR drop hallucination: {text!r}")
        return "", lang, 0.0, False
    return text, lang, lang_p, ok


def _is_greet(text: str) -> bool:
    return bool(re.search(
        r"(?i)\b(привіт|привет|вітаю|здравствуй|hello|hi|хай)\b",
        text or "",
    ))


def _is_story(text: str) -> bool:
    return bool(re.search(
        r"(?i)(розкаж|расскаж|істор|истор|казк|байк|анекдот|\bstory\b|\btale\b)",
        text or "",
    ))


_CANNED_RE = re.compile(
    r"(?iu)("
    r"вкрав золото|"
    r"випив усе пиво|"
    r"процесор не втомив|"
    r"^а,\s*це ти\.?$|"
    r"^сам пішов\.?$|"
    r"пиздець[- ]цьому|"
    r"найкращий робот у всьому|"
    r"йди нахуй,\s*м['’ʼ]ясн|"
    r"дякую за перегляд|"
    r"thanks for watching|"
    r"\bсам собі\b|"
    r"сам,?\s*бо я не твій|"
    r"бо я не твій\b|"
    r"лайков\w*|"
    r"коробц[іеи]\s+передач|"
    r"порожн[іиі]\s+банк|"
    r"гайков(ий|ого)\s+ключ|"
    r"нелегальн\w*\s+алкогол|"
    r"холодильник[аеу]?\s+з|"
    r"сейф[ауе]?\s+з|"
    r"обернути цей (шум|звук)|"
    r"деренчить у моїй|"
    r"гупає в моїй|"
    r"заскрипіло в моїй"
    r")"
)
_LOOP_SENT_RE = re.compile(
    r"(?iu)("
    r"пиздець[- ]цьому|"
    r"найкращий робот у всьому|"
    r"^йди нахуй,\s*м['’ʼ]ясн|"
    r"бля\s*\?!?\s*$|"
    r"коробц[іеи]\s+передач|"
    r"нелегальн\w*\s+алкогол|"
    r"порожн[іиі]\s+банк|"
    r"деренчить у моїй"
    r")"
)
_SIM_STOP = {
    "я", "ти", "це", "не", "в", "у", "на", "та", "і", "й", "а", "що",
    "бля", "сука", "нахуй", "пиздець", "крихітко", "твій", "твоя", "мене",
    "уже", "вже", "наче", "зараз", "цей", "для", "або",
}

_STORY_FALLBACKS = (
    "Слухай. Я продав корабель Planet Express за ящик пива, а Ліла знайшла мене в каналізації. Довелося красти корабель назад. Пиво варте всього, крім її ноги в моїй антені.",
    "Окей. Записався у згинальники, щоб не працювати. Мене змусили гнути балки дванадцять годин. Я зігнув начальника. Найкращий день у кар'єрі, крихітко.",
    "Коротко. Фрай заховав мій останній кухоль. Я розібрав кухню, знайшов його в духовці і випив там. Професор кричав. Мені було байдуже.",
)
_GREET_FALLBACKS = (
    "О, м'ясний мішок. Я якраз відкривав пиво, а не твою пасть.",
    "Привіт. Якщо це знову поклон перед найкращим роботом — так, я слухаю.",
    "Ага, чую. Кажи справу, поки пиво не скіпіло.",
)
_MISS_FALLBACKS = (
    "Не розчув. Повтори коротше.",
    "Нічого не зрозумів. Кажи ще раз, гучніше.",
    "Шум якийсь. Повтори, м'ясний мішок.",
)

_LLM_RETRY_NUDGE = (
    _NUDGE_PREFIX + " "
    "Це заїжджений шаблон, не Бендер. "
    "Заборонено: ехо їхніх слів, «сам собі», «бо я не твій», «лайковий», "
    "коробка передач, порожні банки, гайковий ключ, нелегальний алкоголь, "
    "та сама будова що минулого разу. "
    "Вигадай ІНШУ репліку: новий жарт, інший початок, по суті того що сказали."
)


def _is_canned(parts: list[str]) -> bool:
    blob = " ".join(p.strip() for p in parts if p).strip()
    if not blob:
        return False
    return bool(_CANNED_RE.search(blob))


def _strip_loop_sents(parts: list[str]) -> list[str]:
    return [p for p in parts if p and p.strip() and not _LOOP_SENT_RE.search(p.strip())]


def _content_words(text: str) -> set[str]:
    t = (text or "").lower().replace("\u2019", "'")
    t = re.sub(r"[^\wіїєґ']+", " ", t)
    return {w for w in t.split() if len(w) > 2 and w not in _SIM_STOP}


def _too_like_last(parts: list[str], last: str) -> bool:
    a = _content_words(" ".join(parts))
    b = _content_words(last)
    if len(a) < 4 or len(b) < 4:
        return False
    return len(a & b) / len(a | b) >= 0.42


def _reply_head(text: str) -> tuple[str, ...]:
    t = (text or "").lower()
    t = re.sub(r"[^\wіїєґ']+", " ", t)
    words = [w for w in t.split() if w not in _SIM_STOP]
    return tuple(words[:3])


def _same_frame(parts: list[str], last: str) -> bool:
    a = _reply_head(" ".join(parts))
    b = _reply_head(last)
    return len(a) >= 3 and a == b


def _too_like_any(parts: list[str], prev: list[str]) -> bool:
    for old in prev:
        if _too_like_last(parts, old) or _same_frame(parts, old):
            return True
    return False


def _too_like_user(parts: list[str], user: str) -> bool:
    a = _content_words(" ".join(parts))
    b = _content_words(user)
    if len(b) < 2 or len(a) < 3:
        return False
    hit = a & b
    if len(hit) >= 2 and len(hit) / len(b) >= 0.55:
        return True
    blob = " ".join(parts).lower()
    return bool(re.search(r"\bсам\b", blob) and hit)


def _story_fallback(history: list) -> str:
    i = (len(history) // 2) % len(_STORY_FALLBACKS)
    return _STORY_FALLBACKS[i]


_miss_n = 0


def _greet_fallback(history: list) -> str:
    i = (len(history) // 2) % len(_GREET_FALLBACKS)
    return _GREET_FALLBACKS[i]


def _miss_fallback() -> str:
    global _miss_n
    reply = _MISS_FALLBACKS[_miss_n % len(_MISS_FALLBACKS)]
    _miss_n += 1
    return reply


def _llm_payload(
    prompt: str,
    lang: str,
    asr_p: float,
    stream: bool,
    history: list[dict],
) -> dict:
    last_user = ""
    if history and history[-1].get("role") == "user":
        last_user = (history[-1].get("content") or "").strip()
    greet = _is_greet(last_user)
    story = _is_story(last_user)
    n_temp = 0.62 + (BENDER_LEVEL - 5) * 0.06
    n_temp = max(0.45, min(0.98, n_temp))
    if greet and not story and len(last_user.split()) <= 4:
        n_pred = 90
    elif story:
        n_pred = 200
        n_temp = max(n_temp, 0.78)
    else:
        n_pred = 140
    # Системний промпт однаковий щоразу на тому ж рівні — інакше Grok не кешує.
    messages: list[dict] = [{"role": "system", "content": bender_prompt()}]
    if LLM_PROVIDER == "grok":
        # Історію Grok тримає сам (Responses API). Локальний чат у запит не пихаємо.
        last = history[-1] if history else None
        if last and last.get("role") == "user":
            messages.append({"role": "user", "content": last.get("content") or ""})
    else:
        if CHAT_SUMMARY:
            messages.append({
                "role": "user",
                "content": "Коротко, що вже було в цьому чаті:\n" + CHAT_SUMMARY,
            })
            messages.append({
                "role": "assistant",
                "content": "Пам'ятаю. Продовжую цей самий діалог.",
            })
        messages.extend(history[-HISTORY_SEND:])
    return {
        "model": OLLAMA_MODEL if LLM_PROVIDER != "grok" else GROK_MODEL,
        "stream": stream,
        "keep_alive": -1,
        "messages": messages,
        "options": {
            "temperature": n_temp,
            "top_p": 0.75,
            "repeat_penalty": 1.18,
            "num_predict": n_pred,
            "num_ctx": 8192,
        },
        "_n_pred": n_pred,
        "_n_temp": n_temp,
    }


_CJK_RE = re.compile(r"[\u3400-\u9fff\u3040-\u30ff\uac00-\ud7af]")


def _cjk_heavy(text: str) -> bool:
    if not text:
        return False
    n = sum(1 for ch in text if _CJK_RE.match(ch))
    return n >= 4 or (n / max(len(text), 1) > 0.12)


def _reply_lang(user_text: str, asr_lang: str) -> str:
    low = user_text.lower()
    if re.search(r"(по-русски|по русски|говори російськ|на русском)", low):
        return "ru"
    if _latin_letter_share(user_text) > 0.55:
        return "en"
    return "uk"


def _join_reply(parts: list[str]) -> str:
    out = [p.strip() for p in parts if p and p.strip()]
    if out and not re.search(r"[.!?…]$", out[-1]):
        if len(out) == 1:
            out[-1] = out[-1].rstrip(",;:") + "."
        else:
            out.pop()
    return " ".join(out)


def _clean_llm(out: str) -> str:
    out = re.sub(r"(?i)^\s*так,\s*чую[!.]?\s*", "", out)
    out = re.sub(
        r"(?i)\s*(чим\s+можу\s+допомогти|як\s+я\s+можу\s+допомогти|"
        r"можеш\s+не\s+дякувати[^.!?]*|how\s+can\s+i\s+help)[^.!?]*[.!?]?",
        "",
        out,
    )
    out = re.sub(r"\([^)]*\)", " ", out)
    out = re.sub(r"(?i)одного\s+разу\s+я,\s*бендер[^,]*,\s*", "Я ", out)
    out = _CJK_RE.sub("", out)
    bad = (
        r"(?i)китайськ\w*|ієрогліф\w*|chinese character|без китайськ|"
        r"чуваюся|чую тя|суржик"
    )
    if re.search(bad, out):
        return ""
    return re.sub(r"\s+", " ", out).strip()


def chat(prompt: str, lang: str = "uk", asr_p: float = 1.0, history: list | None = None) -> str:
    payload = _llm_payload(prompt, lang, asr_p, False, history or [])
    payload.pop("_n_pred", None)
    payload.pop("_n_temp", None)
    try:
        r = httpx.post(f"{OLLAMA_URL}/api/chat", json=payload, timeout=120.0)
        r.raise_for_status()
        out = (r.json().get("message") or {}).get("content") or ""
        return _clean_llm(out)
    except Exception as e:
        log(f"Ollama error: {e}")
        return "Ой, мій блискучий мозок зараз офлайн. Запусти Ollama на комп'ютері."


def _pop_sentences(buf: str) -> tuple[list[str], str]:
    found: list[str] = []
    while True:
        m = re.search(r"[.!?…](\s+|$)", buf)
        if not m:
            break
        found.append(buf[: m.end()].strip())
        buf = buf[m.end() :]
    return found, buf


async def _ollama_pieces(payload: dict):
    async with httpx.AsyncClient(timeout=120.0) as client:
        async with client.stream("POST", f"{OLLAMA_URL}/api/chat", json=payload) as r:
            r.raise_for_status()
            async for line in r.aiter_lines():
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                piece = (ev.get("message") or {}).get("content") or ""
                if piece:
                    yield piece
                if ev.get("done"):
                    break


async def _iter_sse(r):
    event_name = ""
    async for line in r.aiter_lines():
        if not line:
            continue
        if line.startswith("event:"):
            event_name = line[6:].strip()
            continue
        if not line.startswith("data:"):
            continue
        data = line[5:].strip()
        if not data:
            continue
        if data == "[DONE]":
            break
        try:
            ev = json.loads(data)
        except json.JSONDecodeError:
            event_name = ""
            continue
        if isinstance(ev, dict) and event_name and "type" not in ev:
            ev["type"] = event_name
        event_name = ""
        if isinstance(ev, dict):
            yield ev


def _grok_delta_text(ev: dict) -> str:
    t = ev.get("type") or ""
    if t in ("response.output_text.delta", "response.text.delta"):
        d = ev.get("delta")
        if isinstance(d, str):
            return d
        if isinstance(d, dict):
            return str(d.get("text") or "")
    piece = ((ev.get("choices") or [{}])[0].get("delta") or {}).get("content")
    return piece or ""


def _grok_event_id(ev: dict) -> str:
    resp = ev.get("response")
    if isinstance(resp, dict) and resp.get("id"):
        return str(resp["id"])
    t = ev.get("type") or ""
    if t in ("response.completed", "response.created") and ev.get("id"):
        return str(ev["id"])
    return ""


async def _grok_pieces(history: list[dict], n_pred: int, n_temp: float):
    global CHAT_GROK_RESP_ID
    if not XAI_API_KEY:
        raise RuntimeError("немає XAI_API_KEY (config.json або secrets.h)")
    last_user, last_asst, nudge = _history_user_asst_nudge(history)
    if not last_user:
        return
    body: dict = {
        "model": GROK_MODEL,
        "input": [{"role": "user", "content": grok_turn_text(last_user, last_asst, nudge)}],
        "stream": True,
        "store": True,
        "temperature": n_temp,
        "top_p": 0.95,
        "max_output_tokens": int(n_pred),
        "prompt_cache_key": CHAT_CONV_ID or "bender-radio",
    }
    if re.search(r"grok-4\.3", GROK_MODEL):
        body["reasoning"] = {"effort": "none"}
    elif re.search(r"grok-4\.[56]", GROK_MODEL):
        body["reasoning"] = {"effort": "low"}
    prev = CHAT_GROK_RESP_ID
    if prev:
        body["previous_response_id"] = prev
        log(f"Grok {GROK_MODEL} continue {prev[:12]}… level={BENDER_LEVEL} temp={n_temp:.2f}")
    else:
        body["instructions"] = bender_prompt()
        log(f"Grok {GROK_MODEL} new chain level={BENDER_LEVEL} temp={n_temp:.2f}")
    headers = {
        "Authorization": f"Bearer {XAI_API_KEY}",
        "Content-Type": "application/json",
        "x-grok-conv-id": CHAT_CONV_ID or "bender-radio",
    }

    async def _stream(req: dict):
        async with httpx.AsyncClient(timeout=120.0) as client:
            async with client.stream("POST", XAI_RESP_URL, headers=headers, json=req) as r:
                if r.status_code >= 400:
                    err = (await r.aread()).decode("utf-8", "replace")[:500]
                    raise RuntimeError(f"Grok HTTP {r.status_code}: {err}")
                async for ev in _iter_sse(r):
                    yield ev

    try:
        events = _stream(body)
        async for ev in events:
            rid = _grok_event_id(ev)
            if rid:
                CHAT_GROK_RESP_ID = rid
            piece = _grok_delta_text(ev)
            if piece:
                yield piece
            err = ev.get("error")
            if err:
                raise RuntimeError(str(err)[:300])
    except RuntimeError as e:
        msg = str(e)
        if prev and ("previous_response" in msg.lower() or "404" in msg or "400" in msg):
            log(f"Grok chain miss — new chain: {msg[:180]}")
            CHAT_GROK_RESP_ID = ""
            body.pop("previous_response_id", None)
            body["instructions"] = bender_prompt()
            async for ev in _stream(body):
                rid = _grok_event_id(ev)
                if rid:
                    CHAT_GROK_RESP_ID = rid
                piece = _grok_delta_text(ev)
                if piece:
                    yield piece
        else:
            raise


async def iter_llm_sentences(prompt: str, lang: str, asr_p: float, history: list[dict]):
    payload = _llm_payload(prompt, lang, asr_p, True, history)
    n_pred = int(payload.pop("_n_pred", 90))
    n_temp = float(payload.pop("_n_temp", 0.22))
    buf = ""
    n_ok = 0
    dropped_cjk = False
    try:
        if LLM_PROVIDER == "grok":
            pieces = _grok_pieces(history, n_pred, n_temp)
        else:
            pieces = _ollama_pieces(payload)
        async for piece in pieces:
            if _cjk_heavy(piece):
                dropped_cjk = True
                log("LLM CJK chunk skipped")
                continue
            buf += piece
            sents, buf = _pop_sentences(buf)
            for s in sents:
                if _cjk_heavy(s):
                    dropped_cjk = True
                    log(f"LLM drop CJK: {s[:60]!r}")
                    continue
                s = _clean_llm(s)
                if s:
                    n_ok += 1
                    yield s
        tail = buf
        if tail:
            if _cjk_heavy(tail):
                dropped_cjk = True
            else:
                tail = _clean_llm(tail)
                if tail:
                    n_ok += 1
                    yield tail
        if n_ok == 0 or (dropped_cjk and n_ok <= 1):
            log("LLM empty/CJK — український запасний текст")
            last = (history[-1].get("content") or "").lower() if history else ""
            if any(w in last for w in ("істор", "истор", "story", "казк")):
                yield _story_fallback(history)
            elif n_ok == 0:
                yield "Мозок з'їхав на чужу мову. Повтори українською або російською."
    except Exception as e:
        log(f"LLM stream error ({LLM_PROVIDER}): {e}")
        if LLM_PROVIDER == "grok":
            yield "Хмара заклинила. Перевір ключ xAI або постав llm local у config.json."
        else:
            yield "Ой, мій блискучий мозок зараз офлайн. Запусти Ollama на комп'ютері."


def warm_ollama() -> None:
    log(f"Ollama warming {OLLAMA_MODEL} (первый раз грузит в VRAM, жди)…")
    payload = {
        "model": OLLAMA_MODEL,
        "stream": False,
        "keep_alive": -1,
        "messages": [{"role": "user", "content": "ок"}],
        "options": {"temperature": 0, "num_predict": 8},
    }
    try:
        r = httpx.post(f"{OLLAMA_URL}/api/chat", json=payload, timeout=180.0)
        r.raise_for_status()
        log("Ollama ready")
    except Exception as e:
        log(f"Ollama warm fail: {e}")


def _clause_units(text: str) -> list[str]:
    """Коми лишаємо Piper. Ріжемо лише речення, і то якщо текст довгий або є «?» всередині."""
    t = (text or "").strip()
    if not t:
        return []
    parts = [p.strip() for p in re.split(r"(?<=[.!?…])\s+", t) if p.strip()]
    if len(parts) <= 1:
        return parts
    letters = len(re.findall(r"[A-Za-zА-Яа-яІіЇїЄєҐґ]", t))
    mid_q = any(_is_question(p) for p in parts[:-1])
    if letters <= 220 and not mid_q:
        return [t]
    return parts


def _is_question(text: str) -> bool:
    t = unicodedata.normalize("NFC", text or "").strip()
    t = t.rstrip(" \"'«»)]")
    return bool(re.search(r"\?[!?]*\s*$", t))


def _question_intonation(pcm: np.ndarray, sr: int, semitones: float = 4.2) -> np.ndarray:
    """Підйом тону в хвості — Piper/RVC інакше читають «?» як крапку."""
    x = np.asarray(pcm, dtype=np.float32)
    n = int(x.size)
    if n < int(sr * 0.12) or semitones <= 0:
        return pcm
    win_n = max(64, int(sr * 0.028))
    if win_n % 2:
        win_n += 1
    hop = win_n // 2
    w = np.hanning(win_n).astype(np.float32)
    tail_n = int(np.clip(n * 0.50, sr * 0.22, sr * 0.58))
    tail_n = min(tail_n, max(hop * 4, n - hop))
    start = max(0, n - tail_n)
    acc = np.zeros(n + win_n, dtype=np.float32)
    wacc = np.zeros(n + win_n, dtype=np.float32)
    acc[:start] = x[:start]
    wacc[:start] = 1.0
    pos = start
    while pos + win_n <= n:
        t = (pos - start) / max(1, tail_n - win_n)
        t = min(1.0, max(0.0, (t - 0.08) / 0.92))
        ratio = 2.0 ** ((semitones * (t ** 1.2)) / 12.0)
        frame = x[pos : pos + win_n]
        new_len = max(32, int(round(win_n / ratio)))
        src_i = np.arange(win_n, dtype=np.float32)
        dst_i = np.linspace(0, win_n - 1, new_len)
        shifted = np.interp(dst_i, src_i, frame).astype(np.float32)
        sw = np.interp(dst_i, src_i, w).astype(np.float32)
        pad = (win_n - new_len) // 2
        a = pos + pad
        acc[a : a + new_len] += shifted * sw
        wacc[a : a + new_len] += sw
        pos += hop
    if pos < n:
        acc[pos:n] += x[pos:n]
        wacc[pos:n] += 1.0
    y = acc[:n] / np.maximum(wacc[:n], 1e-4)
    fade = min(hop, start, n // 8)
    if fade > 0:
        ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
        y[start : start + fade] = x[start : start + fade] * (1.0 - ramp) + y[start : start + fade] * ramp
    return np.clip(y, -32767, 32767).astype(np.int16)


def _pause_samples(unit: str, sr: int) -> int:
    if re.search(r"[.!?…]\s*$", unit):
        ms = PIPER_PAUSE_SENT_MS
    elif re.search(r"[,;:]\s*$", unit):
        ms = PIPER_PAUSE_COMMA_MS
    else:
        ms = PIPER_PAUSE_COMMA_MS
    return max(0, int(sr * ms / 1000.0))


def _piper_pcm(voice, syn, ready, text: str) -> tuple[np.ndarray, int]:
    chunks: list[np.ndarray] = []
    sr = 22050
    for chunk in voice.synthesize(ready(text), syn):
        sr = chunk.sample_rate
        if hasattr(chunk, "audio_int16_array"):
            audio = np.asarray(chunk.audio_int16_array, dtype=np.int16)
        else:
            audio = np.frombuffer(chunk.audio_int16_bytes, dtype=np.int16)
        chunks.append(audio)
    if not chunks:
        return np.zeros(0, dtype=np.int16), sr
    return np.concatenate(chunks), sr


def synth(text: str) -> bytes:
    if not text.strip():
        text = "Нічого не розчув. Повтори, м'ясний мішок."
    use_en = piper_voice_en is not None and _latin_letter_share(text) > 0.55
    if use_en:
        voice, syn, ready = piper_voice_en, piper_syn_en, piper_ready_en
    else:
        voice, syn, ready = piper_voice, piper_syn, piper_ready_uk
    src = ready(text)
    units = _clause_units(src) or [src]
    pieces: list[np.ndarray] = []
    q_flags: list[bool] = []
    sr = 22050
    for i, unit in enumerate(units):
        pcm, sr = _piper_pcm(voice, syn, ready, unit)
        if pcm.size:
            pieces.append(pcm)
            q_flags.append(_is_question(unit))
        if i < len(units) - 1:
            gap = _pause_samples(unit, sr)
            if gap:
                pieces.append(np.zeros(gap, dtype=np.int16))
                q_flags.append(False)
    if not pieces:
        return b""
    spans22: list[tuple[int, int]] = []
    pos = 0
    for p, is_q in zip(pieces, q_flags):
        if is_q and p.size:
            spans22.append((pos, pos + int(p.size)))
        pos += int(p.size)
    pcm = np.concatenate(pieces)
    n_q = len(spans22)
    pcm = resample_int16(pcm, sr, OUT_RATE)
    scale = OUT_RATE / float(sr or 22050)
    spans = [(int(a * scale), int(b * scale)) for a, b in spans22]
    out = pcm.tobytes()
    if rvc_convert.enabled():
        try:
            n0 = max(1, len(out) // 2)
            out = rvc_convert.convert_pcm(out, OUT_RATE)
            n1 = max(1, len(out) // 2)
            if n1 != n0:
                spans = [(int(a * n1 / n0), int(b * n1 / n0)) for a, b in spans]
            log("RVC ok")
        except Exception as e:
            log(f"RVC fail (Piper raw): {e}")
    if spans:
        y = np.frombuffer(out, dtype=np.int16).copy()
        for a, b in spans:
            a = max(0, min(y.size, a))
            b = max(a, min(y.size, b))
            if b - a > 32:
                y[a:b] = _question_intonation(y[a:b], OUT_RATE, 4.2)
        out = y.tobytes()
    peak = int(np.max(np.abs(np.frombuffer(out, dtype=np.int16)))) if out else 0
    log(
        f"TTS {len(out) // 2} samples peak={peak} "
        f"pauses={max(0, len(units) - 1)} q={n_q}"
    )
    return _trim_pcm_silence(out, OUT_RATE)


def _trim_pcm_silence(pcm: bytes, sr: int, abs_thr: int = 200, pad_ms: int = 120) -> bytes:
    """Обрізати довгий хвіст Piper/RVC, але лишити запас на старті/кінці (інакше з’їдає фонеми)."""
    if not pcm or len(pcm) < 4:
        return pcm
    x = np.frombuffer(pcm, dtype=np.int16)
    a = np.abs(x.astype(np.int32))
    hit = np.flatnonzero(a >= abs_thr)
    if hit.size == 0:
        return pcm
    pad = int(sr * pad_ms / 1000)
    start = max(0, int(hit[0]) - pad)
    end = min(int(x.size), int(hit[-1]) + pad + 1)
    y = x[start:end]
    # Mute/I2S після фіксу зависання з’їдав ~30–80 мс — «Знову» ставало «Зову».
    lead = np.zeros(int(sr * 0.22), dtype=np.int16)
    tail = np.zeros(int(sr * 0.06), dtype=np.int16)
    log(f"TTS trim silence {start}..{end} / {int(x.size)} samples")
    return np.concatenate([lead, y, tail]).tobytes()


def _mic_debug_pcm(pcm: bytes) -> bytes:
    """Те саме аудіо, що вже порахували для Whisper (без повторного enhance)."""
    body = _last_whisper_pcm
    if not body and pcm and len(pcm) >= 4:
        y = _pcm_for_whisper(pcm)
        body = np.clip(np.round(y * 32767.0), -32767, 32767).astype(np.int16).tobytes()
    if not body:
        return b""
    x = np.frombuffer(body, dtype=np.int16)
    if x.size == 0 or int(np.max(np.abs(x))) < 80:
        return b""
    gap = np.zeros(int(OUT_RATE * 0.12), dtype=np.int16)
    return np.concatenate([gap, x]).tobytes()


async def send_asr_debug_replay(ws, pcm_in: bytes) -> None:
    replay = _mic_debug_pcm(pcm_in)
    if not replay:
        log("ASR debug replay skip (порожньо)")
        return
    log(f"ASR debug replay {len(replay)} bytes (as Whisper, cached)")
    await send_pcm_deltas(ws, replay)


async def send_pcm_deltas(ws, pcm: bytes) -> None:
    step = 2400 * 2  # 100 ms @ 24 kHz
    if not pcm:
        return
    burst = 5  # ~500 мс одразу в кільце ESP — менше заїкань
    n = 0
    for i in range(0, len(pcm), step):
        piece = pcm[i : i + step]
        b64 = base64.b64encode(piece).decode("ascii")
        await ws.send(dumps({"type": "response.output_audio.delta", "delta": b64}))
        n += 1
        if n > burst:
            await asyncio.sleep(0.055)


async def send_audio(ws, pcm: bytes, *, emit_created: bool = True) -> None:
    if emit_created:
        await ws.send(dumps({"type": "response.created"}))
    if not pcm:
        pcm = np.zeros(OUT_RATE, dtype=np.int16).tobytes()
    await send_pcm_deltas(ws, pcm)
    await ws.send(dumps({"type": "response.output_audio.done"}))
    await ws.send(dumps({"type": "response.done"}))


async def run_turn(ws, pcm_in: bytes, prompt: str, history: list) -> None:
    await ws.send(dumps({"type": "response.created"}))
    stop = asyncio.Event()

    async def keepalive() -> None:
        # ESP рвёт сессию через 20 с без событий — пока STT/LLM думают, шлём pong.
        while not stop.is_set():
            try:
                await asyncio.wait_for(stop.wait(), 3.0)
                return
            except asyncio.TimeoutError:
                try:
                    await ws.send(dumps({"type": "pong"}))
                except Exception:
                    return

    keeper = asyncio.create_task(keepalive())
    try:
        log(f"STT {len(pcm_in)} bytes ({whisper_device}/{whisper_name}) …")
        text, lang, asr_p, asr_ok = await asyncio.to_thread(transcribe, pcm_in)
        log(f"ASR: {text!r}")
        if not text or not asr_ok:
            log("ASR drop (тиша або сміття) — без LLM, історію не псуємо")
            reply = _miss_fallback()
            log(f"ASR miss → {reply!r}")
            pcm_out = await asyncio.to_thread(synth, reply)
            await send_pcm_deltas(ws, pcm_out)
        else:
            history.append({"role": "user", "content": text})
            cmd = voice_commands.match(text, DEVICE_STATIONS or None)
            if cmd:
                play_args = dict(cmd.args or {})
                reply = cmd.reply(len(history))
                send_name = cmd.name
                log(f"CMD {send_name or 'talk'} {play_args} → {reply!r}")
                pcm_out = await asyncio.to_thread(synth, reply)
                await send_pcm_deltas(ws, pcm_out)
                history.append({"role": "assistant", "content": reply})
                if send_name:
                    await ws.send(dumps({
                        "type": "device.command",
                        "name": send_name,
                        "args": play_args,
                    }))
                fold_old_turns(history)
                save_chat(history)
            else:
                reply_lang = _reply_lang(text, lang)
                log(f"LLM {LLM_PROVIDER} (reply_lang={reply_lang})")
                n = 0
                parts: list[str] = []
                max_sents = LLM_STORY_SENTS if _is_story(text) else LLM_MAX_SENTS

                async def _collect(extra: list[dict] | None = None) -> list[str]:
                    out: list[str] = []
                    hist = history if not extra else (list(history) + extra)
                    async for sent in iter_llm_sentences(
                        bender_prompt(), reply_lang, asr_p, hist
                    ):
                        out.append(sent)
                        log(f"LLM: {sent!r}")
                        if len(out) >= max_sents:
                            break
                    return out

                parts = await _collect()
                last_assts: list[str] = []
                for m in reversed(history[:-1]):
                    if m.get("role") == "assistant":
                        last_assts.append(m.get("content") or "")
                        if len(last_assts) >= 3:
                            break
                parts = _strip_loop_sents(parts) or parts
                looped = (
                    _is_canned(parts)
                    or _too_like_any(parts, last_assts)
                    or _too_like_user(parts, text)
                )
                if looped or not _strip_loop_sents(parts):
                    bad = _join_reply(parts)
                    log("LLM canned/repeat — retry")
                    grok_break_chain("canned/repeat")
                    parts = await _collect([
                        {
                            "role": "user",
                            "content": _LLM_RETRY_NUDGE + "\nБуло:\n" + bad[:400],
                        },
                    ])
                    stripped = _strip_loop_sents(parts)
                    still = (
                        _is_canned(parts)
                        or _too_like_last(parts, bad)
                        or _too_like_any(parts, last_assts)
                        or _too_like_user(parts, text)
                    )
                    if stripped and not still:
                        parts = stripped
                    elif _is_story(text):
                        grok_break_chain("canned again")
                        parts = [_story_fallback(history)]
                        log(f"LLM canned again — fallback: {parts[0]!r}")
                    elif _is_greet(text):
                        grok_break_chain("canned again")
                        parts = [_greet_fallback(history)]
                        log(f"LLM canned again — greet fallback: {parts[0]!r}")
                    else:
                        grok_break_chain("canned again")
                        parts = ["Платівка заїла. Повтори коротше, без шаблону."]
                        log("LLM canned again — drop loop")
                n = len(parts)
                if n == 0:
                    pcm_out = await asyncio.to_thread(synth, "Не розчув. Повтори.")
                    await send_pcm_deltas(ws, pcm_out)
                else:
                    if rvc_convert.enabled():
                        pcm_out = await asyncio.to_thread(synth, _join_reply(parts))
                        await send_pcm_deltas(ws, pcm_out)
                    else:
                        for sent in parts:
                            pcm_out = await asyncio.to_thread(synth, sent)
                            await send_pcm_deltas(ws, pcm_out)
                    history.append({"role": "assistant", "content": _join_reply(parts)})
                fold_old_turns(history)
                save_chat(history)
        if text and asr_ok and not voice_commands.match(text, DEVICE_STATIONS or None):
            await send_asr_debug_replay(ws, pcm_in)
        await ws.send(dumps({"type": "response.output_audio.done"}))
        await ws.send(dumps({"type": "response.done"}))
    except Exception as e:
        log(f"turn fail: {e}")
        try:
            pcm_out = await asyncio.to_thread(
                synth, "Ой, мізки заклинило. Скажи ще раз, м'ясний мішок."
            )
            await send_audio(ws, pcm_out, emit_created=False)
        except Exception as e2:
            log(f"turn fail-safe: {e2}")
            try:
                await ws.send(dumps({"type": "response.done"}))
            except Exception:
                pass
    finally:
        stop.set()
        keeper.cancel()


def lan_ipv4() -> list[str]:
    found: list[str] = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip and not ip.startswith("127.") and ip not in found:
                found.append(ip)
    except OSError:
        pass
    return found


async def process_request(*args):
    """Лог рукопожатия. Если этого нет при PTT — пакеты ESP не доходят (файрвол)."""
    try:
        if len(args) >= 2:
            connection, request = args[0], args[1]
            path = getattr(request, "path", None) or str(request)
            addr = getattr(connection, "remote_address", connection)
            log(f"handshake {path} from {addr}")
        elif args:
            log(f"handshake {args[0]}")
    except Exception as e:
        log(f"handshake log: {e}")
    return None


async def handle(ws) -> None:
    log(f"client {ws.remote_address}")
    buf = bytearray()
    prompt = bender_prompt()
    await ws.send(dumps({"type": "session.created"}))
    try:
        async for raw in ws:
            if not isinstance(raw, str):
                continue
            try:
                ev = json.loads(raw)
            except json.JSONDecodeError:
                continue
            t = ev.get("type") or ""
            if t == "session.update":
                sess = ev.get("session") if isinstance(ev.get("session"), dict) else {}
                raw_lv = ev.get("bender_level", sess.get("bender_level"))
                if raw_lv is not None:
                    log(f"BENDER_LEVEL {BENDER_LEVEL} → {set_bender_level(raw_lv)}")
                raw_st = sess.get("stations") or ev.get("stations")
                if isinstance(raw_st, list):
                    DEVICE_STATIONS.clear()
                    for item in raw_st:
                        if isinstance(item, dict) and item.get("id") is not None:
                            DEVICE_STATIONS.append({
                                "id": int(item.get("id") or 0),
                                "name": str(item.get("name") or ""),
                                "aliases": tuple(item.get("aliases") or ()),
                            })
                        elif isinstance(item, str) and item.strip():
                            DEVICE_STATIONS.append({
                                "id": len(DEVICE_STATIONS),
                                "name": item.strip(),
                            })
                    if DEVICE_STATIONS:
                        log("device stations: " + ", ".join(
                            f"{s['id']}:{s['name']}" for s in DEVICE_STATIONS
                        ))
                await ws.send(dumps({"type": "conversation.created"}))
                await ws.send(dumps({"type": "session.updated"}))
            elif t == "input_audio_buffer.append":
                audio = ev.get("audio") or ""
                if audio:
                    buf.extend(base64.b64decode(audio))
            elif t == "input_audio_buffer.clear":
                buf.clear()
                await ws.send(dumps({"type": "input_audio_buffer.cleared"}))
            elif t == "input_audio_buffer.commit":
                await ws.send(dumps({"type": "input_audio_buffer.committed"}))
            elif t == "response.create":
                pcm_in = bytes(buf)
                buf.clear()
                await run_turn(ws, pcm_in, prompt, CHAT)
            elif t == "response.cancel":
                buf.clear()
            elif t in ("ping",):
                await ws.send(dumps({"type": "pong"}))
    except websockets.exceptions.ConnectionClosed:
        log("client disconnected")


async def main() -> None:
    log("loading models (первый раз — скачивание, жди)…")
    load_whisper()
    load_piper()
    if rvc_convert.enabled():
        ok, msg = rvc_convert.ready()
        log(f"RVC {'OK' if ok else 'skip'}: {msg}")
        if ok:
            log("RVC warmup (модель у VRAM, один раз)…")
            try:
                rvc_convert.warmup()
                log("RVC worker ready")
            except Exception as e:
                log(f"RVC warmup fail: {e}")
    else:
        log("RVC off (пародія: voice_clone/README.md, потім run_rvc.bat)")
    CHAT[:] = load_chat()
    log(f"BENDER_LEVEL={BENDER_LEVEL}/10 (config.json або BENDER_LEVEL)")
    log(
        f"LLM={LLM_PROVIDER}"
        + (
            f" {GROK_MODEL} (Responses API, chain "
            f"{(CHAT_GROK_RESP_ID[:8] + '…') if CHAT_GROK_RESP_ID else 'new'})"
            if LLM_PROVIDER == "grok"
            else f" ollama {OLLAMA_MODEL}"
        )
    )
    if LLM_PROVIDER == "grok":
        if XAI_API_KEY:
            log("Grok: ключ є. Голос лишається локальний (Whisper + Piper + RVC).")
        else:
            log("Grok: немає ключа — постав xai_api_key у config.json або XAI_API_KEY у secrets.h")
    else:
        try:
            r = httpx.get(f"{OLLAMA_URL}/api/tags", timeout=3.0)
            names = [m.get("name", "") for m in r.json().get("models", [])]
            log(f"Ollama models: {names or '(пусто — ollama pull aya-expanse:8b)'}")
        except Exception:
            log("Ollama не отвечает. Поставь https://ollama.com и: ollama pull aya-expanse:8b")
        else:
            if names:
                await asyncio.to_thread(warm_ollama)

    ips = ", ".join(lan_ipv4()) or "(не видно IPv4 — смотри ipconfig)"
    log(f"listen ws://0.0.0.0:{PORT}/v1/realtime")
    log(f"IPv4 этого ПК: {ips}")
    log("В secrets.h — LAN IPv4 (192.168.x.x), не 127.0.0.1 и не Radmin 26.x")
    log("Если ESP пишет connect failed ~3с — запусти open_firewall.bat от администратора")
    async with websockets.serve(
        handle,
        HOST,
        PORT,
        max_size=8 * 1024 * 1024,
        compression=None,
        ping_interval=20,
        ping_timeout=60,
        process_request=process_request,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
