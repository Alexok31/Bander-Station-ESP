"""Validate model-extracted planner intents before they reach the device."""

from __future__ import annotations

import json
import re
from uk_time import duration_after_through


def candidate(text: str, pending: bool = False) -> bool:
    if pending:
        return True
    return bool(re.search(r"(?iu)напом|нагад|напомин|план|заплан|щодня|каждый день|таймер|timer", text))


def parse(raw: str) -> dict | None:
    raw = raw.strip()
    if raw.startswith("```"):
        raw = re.sub(r"^```(?:json)?\s*|\s*```$", "", raw, flags=re.I).strip()
    try:
        value = json.loads(raw)
    except (ValueError, TypeError):
        return None
    if not isinstance(value, dict):
        return None
    kind = value.get("kind")
    if kind == "none":
        return {"kind": "none"}
    if kind == "list":
        return {"kind": "list", "scope": "today" if value.get("scope") == "today" else "all"}
    if kind == "incomplete":
        return {"kind": "incomplete"}
    if kind not in ("relative", "daily"):
        return None
    task = value.get("text")
    if task is None:
        task = ""
    if not isinstance(task, str):
        return None
    task = task.strip(" \t\r\n.,!?;:—–-")
    if len(task.encode("utf-8")) >= 160:
        return None
    if kind == "relative":
        seconds = value.get("seconds")
        if isinstance(seconds, bool) or not isinstance(seconds, int) or not 5 <= seconds <= 86400:
            return None
        return {"kind": kind, "seconds": seconds, "text": task}
    hour, minute = value.get("hour"), value.get("minute")
    if (isinstance(hour, bool) or isinstance(minute, bool) or
            not isinstance(hour, int) or not isinstance(minute, int) or
            not 0 <= hour <= 23 or not 0 <= minute <= 59):
        return None
    return {"kind": kind, "hour": hour, "minute": minute, "text": task}


def command(intent: dict) -> tuple[str | None, dict, str]:
    kind, task = intent["kind"], intent.get("text", "")
    if kind == "relative":
        seconds = intent["seconds"]
        if not task:
            return None, {}, "Що саме нагадати?"
        when = "через " + duration_after_through(seconds)
        return "reminder.add", {"seconds": seconds, "text": task}, f"Записав. {when.capitalize()} нагадаю: {task}."
    if kind == "daily":
        if not task:
            return None, {}, "Що саме нагадувати щодня?"
        hour, minute = intent["hour"], intent["minute"]
        return "reminder.daily.add", {"hour": hour, "minute": minute, "text": task}, f"Записав. Щодня о {hour:02d}:{minute:02d} нагадаю: {task}."
    raise ValueError("Only schedulable intents have a command")
