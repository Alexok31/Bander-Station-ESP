"""Голосові команди Бендера. Нову дію = рядок у COMMANDS + обробник на ESP.

Розпізнавання на ПК (надійніше за Grok-tool-call). ESP лише виконує device.command
після фрази, коли колонку вже віддали радіо.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# Вбудовані імена — як у firmware. ESP може прислати свій список у session.update.
STATIONS_DEFAULT: list[dict] = [
    {
        "id": 0,
        "name": "Majestic Jukebox",
        "aliases": ("majestic", "jukebox", "джаз", "джазу", "меджестік", "меджестик"),
    },
    {
        "id": 1,
        "name": "Radio1 Rock",
        "aliases": ("rock", "рок", "radio1", "радіо1", "радио1"),
    },
]

_ON = re.compile(
    r"(?iu)("
    r"(?:включ|увімкн|ввімкн|вмикн|запуст|постав|крутан)\w*"
    r".{0,20}"
    r"(?:радіо|радио|музик|музык|станц)|"
    r"(?:радіо|радио|музик|музык)"
    r".{0,12}"
    r"(?:включ|увімкн|ввімкн|вмикн|запуст)"
    r")"
)
_OFF = re.compile(
    r"(?iu)("
    r"(?:виключ|вимкн|выключ|зупини|заткни|зупини|стоп)\w*"
    r".{0,20}"
    r"(?:радіо|радио|музик|музык|станц)|"
    r"(?:радіо|радио)"
    r".{0,12}"
    r"(?:виключ|вимкн|выключ|стоп)"
    r")"
)
_NEXT = re.compile(
    r"(?iu)("
    r"(?:наступн|следующ|інш|друг)\w*.{0,12}(?:станц|радіо|радио)|"
    r"(?:станц|радіо|радио).{0,12}(?:наступн|следующ)|"
    r"\bnext\b"
    r")"
)
_PREV = re.compile(
    r"(?iu)("
    r"(?:попередн|предыдущ|минул)\w*.{0,12}(?:станц|радіо|радио)|"
    r"\bprev(?:ious)?\b"
    r")"
)
_PLAY = re.compile(
    r"(?iu)(?:включ|увімкн|ввімкн|вмикн|запуст|постав|грай|играй|слуха|слушай|крутан)"
)
_SUGGEST = re.compile(
    r"(?iu)("
    r"порад\w*|посовет\w*|запропону\w*|"
    r"як[аіу]\s+станц|какая\s+станц|які\s+станц|какие\s+станц|"
    r"що\s+(?:ввімк|увімк|включ)|что\s+(?:включ|поставить)|"
    r"яке\s+радіо|какое\s+радио|"
    r"список\s+станц|які\s+є"
    r")"
)
_HERE_CORE = re.compile(
    r"(?iu)^(?:ну\s+|эй\s+|гей\s+|ау\s+|алло\s+|ало\s+)*"
    r"(?:ты|ти)?\s*"
    r"(?:тут|здесь|там|на\s+(?:зв.?язку|связи)|титул|титуль|тытул|title)"
    r"(?:\s+(?:ли|же))?$"
)


def _is_here_ping(text: str) -> bool:
    core = re.sub(r"(?iu)\b(?:бендер|bender)\b", " ", text)
    core = re.sub(r"[\s.,!?…:;«»\"'\-—]+", " ", core).strip()
    if not core:
        return False
    return bool(_HERE_CORE.fullmatch(core))


@dataclass
class VoiceCommand:
    """name=None — лише репліка, ESP нічого не робить."""

    name: str | None
    args: dict = field(default_factory=dict)
    replies: tuple[str, ...] = ()
    skip_llm: bool = True

    def reply(self, n: int = 0) -> str:
        if not self.replies:
            return "Зробив."
        return self.replies[n % len(self.replies)]


def _norm(text: str) -> str:
    t = (text or "").lower().replace("ё", "е")
    t = t.replace("’", "'").replace("`", "'")
    t = re.sub(r"\s+", " ", t).strip()
    return t


def catalog(stations: list[dict] | None) -> list[dict]:
    by_id: dict[int, dict] = {}
    for s in STATIONS_DEFAULT:
        i = int(s["id"])
        by_id[i] = {
            "id": i,
            "name": s.get("name") or "",
            "aliases": tuple(s.get("aliases") or ()),
        }
    for s in stations or []:
        try:
            i = int(s.get("id"))
        except (TypeError, ValueError):
            continue
        cur = by_id.get(i, {"id": i, "name": "", "aliases": ()})
        if s.get("name"):
            cur["name"] = str(s["name"])
        extra = tuple(str(a) for a in (s.get("aliases") or ()) if a)
        if extra:
            cur["aliases"] = tuple(dict.fromkeys(tuple(cur.get("aliases") or ()) + extra))
        by_id[i] = cur
    return [by_id[k] for k in sorted(by_id)]


def find_station(text: str, stations: list[dict] | None) -> dict | None:
    t = _norm(text)
    best = None
    best_len = 0
    for st in catalog(stations):
        names = [str(st.get("name") or "")]
        names.extend(str(a) for a in (st.get("aliases") or ()))
        for alias in names:
            a = _norm(alias)
            if len(a) >= 3 and a in t and len(a) > best_len:
                best = st
                best_len = len(a)
    return best


def _list_stations(stations: list[dict] | None) -> str:
    names = [str(s.get("name") or f"станція {s.get('id')}") for s in catalog(stations)]
    if not names:
        return "ніяких"
    if len(names) == 1:
        return names[0]
    return ", ".join(names[:-1]) + " і " + names[-1]


def match(text: str, stations: list[dict] | None = None) -> VoiceCommand | None:
    t = _norm(text)
    if len(t) < 6:
        return None

    st = find_station(t, stations)
    listed = _list_stations(stations)
    st_name = str((st or {}).get("name") or "цю станцію")

    if _OFF.search(t):
        return VoiceCommand(
            name="radio.off",
            replies=(
                "Добре. Радіо мовчить. Нарешті тиша для моїх схем.",
                "Вимкнув. Можеш сам наспівувати, м'ясний мішок.",
                "Стоп. Я і так краще за будь-який ефір.",
            ),
        )
    if _NEXT.search(t):
        return VoiceCommand(
            name="radio.next",
            replies=(
                "Ок, наступна. Якщо знову джаз — звинувачуй Фрая.",
                "Листаю. Сподіваюсь, там є щось гучніше за твоє пирхання.",
            ),
        )
    if _PREV.search(t):
        return VoiceCommand(
            name="radio.prev",
            replies=(
                "Повернув назад. Ностальгія — для м'яса, але хай буде.",
                "Попередня. Не звикайся, що я виконую забаганки.",
            ),
        )
    if st is not None and _PLAY.search(t):
        return VoiceCommand(
            name="radio.station",
            args={"station": int(st["id"]), "title": st_name},
            replies=(
                f"Окей. Ставлю {st_name}. Не аплодуй, я і так геній.",
                f"{st_name}. І не смій крутити ручку, поки я говорю.",
                f"Лови {st_name}. Краще за твої плейлисти, крихітко.",
            ),
        )
    if _ON.search(t):
        return VoiceCommand(
            name="radio.on",
            replies=(
                "Вмикаю радіо. Не любиш — кажи станцію.",
                "Радіо крутиться. Можеш підтанцьовувати, я не дивлюсь.",
                "Ефір пішов. Вимкну сам як набридне.",
            ),
        )
    if _SUGGEST.search(t):
        return VoiceCommand(
            name=None,
            replies=(
                f"У мене є {listed}. Кажи яку вмикати — і я зроблю вигляд, ніби це була моя ідея.",
                f"Станції: {listed}. Рок чи джаз, м'ясний мішок. Тільки швидко.",
                f"Можу поставити {listed}. Вибирай, поки я не поставив рекламу страхування.",
            ),
        )
    if _is_here_ping(t):
        return VoiceCommand(
            name=None,
            replies=(
                "Так, я тут. Куди я подінусь з цієї банки.",
                "На місці. Можеш не перевіряти кожні п'ять секунд.",
                "Чую. Говори, поки я не вирішив, що ти фоновий шум.",
            ),
        )
    return None
