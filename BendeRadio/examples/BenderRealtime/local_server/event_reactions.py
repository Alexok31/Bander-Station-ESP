"""Short event replies with monotonic, session-independent cooldowns. No LLM."""
import time
import random


class EventReactions:
    REPLIES = {
        "battery_low": ("Енергія закінчується. Підключи зарядку, поки я ще ввічливий.",
                        "Час мене підзарядити. На самому сарказмі далеко не поїдеш."),
        "charging": ("О-о-о, стру-у-ум!",
                     "Нарешті, а то я вже зголоднів!",
                     "Не пиво, але теж смачно."),
        "network_back": ("Зв'язок повернувся. Можете знову захоплюватися мною.",),
        "favorite_station": ("О, улюблена станція. Смак у тебе іноді буває.",),
        "shake": ("Гей, обережніше! Я робот, а не шейкер.",
                  "Не труси мене. Геній усередині й так ледве тримається."),
        "carried": ("Куди несемо мою величність? Сподіваюсь, не на роботу.",),
        "set_down": ("О, прибули. Паркування схвалюю.",),
    }
    COOLDOWN = {"battery_low": 1800, "charging": 600, "network_back": 900,
                "favorite_station": 900, "shake": 300, "carried": 600, "set_down": 600}

    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.last = {}
        self.last_any = None
        self.counts = {}
        self.last_charging_reply = None

    def choose(self, event: dict, profile: dict) -> str | None:
        name = event.get("name")
        if not isinstance(name, str) or name not in self.REPLIES or not profile.get("event_voice", True):
            return None
        if name == "favorite_station" and event.get("station") not in profile.get("favorites", []):
            return None
        now = self.clock()
        if self.last_any is not None and now - self.last_any < 120:
            return None
        if name in self.last and now - self.last[name] < self.COOLDOWN[name]:
            return None
        self.last_any = now
        self.last[name] = now
        if name == "charging":
            choices = [reply for reply in self.REPLIES[name] if reply != self.last_charging_reply]
            reply = random.choice(choices or self.REPLIES[name])
            self.last_charging_reply = reply
            return reply
        n = self.counts.get(name, 0)
        self.counts[name] = n + 1
        return self.REPLIES[name][n % len(self.REPLIES[name])]
