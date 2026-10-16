"""Spoken Ukrainian durations after «через», without TTS number guessing."""

_SMALL = (
    "нуль", "одну", "дві", "три", "чотири", "п'ять", "шість", "сім", "вісім", "дев'ять",
    "десять", "одинадцять", "дванадцять", "тринадцять", "чотирнадцять", "п'ятнадцять",
    "шістнадцять", "сімнадцять", "вісімнадцять", "дев'ятнадцять",
)
_TENS = {20: "двадцять", 30: "тридцять", 40: "сорок", 50: "п'ятдесят"}


def _part(count: int, forms: tuple[str, str, str]) -> str:
    number = (_SMALL[count] if count < 20 else
              _TENS[count // 10 * 10] + (" " + _SMALL[count % 10] if count % 10 else ""))
    form = 2 if 11 <= count % 100 <= 14 else 0 if count % 10 == 1 else 1 if 2 <= count % 10 <= 4 else 2
    return f"{number} {forms[form]}"


def duration_after_through(seconds: int) -> str:
    """Timers support up to one day; retain exact seconds in spoken replies."""
    if type(seconds) is not int or not 0 <= seconds <= 86400:
        raise ValueError("Duration must be integer seconds within one day")
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    parts = [_part(n, forms) for n, forms in (
        (hours, ("годину", "години", "годин")),
        (minutes, ("хвилину", "хвилини", "хвилин")),
        (secs, ("секунду", "секунди", "секунд")),
    ) if n]
    if not parts:
        return "нуль секунд"
    return ", ".join(parts[:-1]) + " і " + parts[-1] if len(parts) > 1 else parts[0]
