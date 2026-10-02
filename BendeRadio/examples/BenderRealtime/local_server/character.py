"""Device personality controls. No model, config or filesystem dependencies."""

KEYS = ("sarcasm", "sociability", "curiosity", "stubbornness", "warmth")
DEFAULT_QUESTION = "Бендере, як проведемо цей вечір?"


def validate_preview_question(raw):
    if not isinstance(raw, str):
        return None
    text = raw.strip()
    if not text or len(text) > 200 or any((ord(c) < 32 and c not in '\n\r\t') or ord(c) == 127 for c in text):
        return None
    try:
        if len(text.encode('utf-8')) > 800:
            return None
    except UnicodeEncodeError:
        return None
    return text


def validate_character(raw):
    # Reject partial/foreign profiles atomically; bool is not an integer setting.
    if not isinstance(raw, dict) or set(raw) != set(KEYS):
        return None
    if any(type(raw[key]) is not int or not 0 <= raw[key] <= 100 for key in KEYS):
        return None
    return {key: raw[key] for key in KEYS}


TRAITS = {
    "sarcasm": ("Сарказм", "Відповідай прямо, без підколів і глузування.",
                "Легка іронія, іноді дружній підкол.",
                "Влучний сарказм і колкі жарти, коли доречно; не кожна репліка мусить бути жартом."),
    "sociability": ("Товариськість", "Зазвичай одне коротке речення, без зайвого продовження.",
                    "Зазвичай 1–2 короткі речення, підтримуй поточну тему.",
                    "Зазвичай 2–3 живі речення, розвивай тему конкретною деталлю, без монологів."),
    "curiosity": ("Цікавість", "Не додавай питань для продовження бесіди. Уточнення для допомоги дозволені.",
                  "Зрідка постав одне доречне питання про поточну тему.",
                  "Активно цікався деталями: частіше став одне змістовне питання, але не в кожній відповіді."),
    "stubbornness": ("Упертість", "Допомагай охоче, без удаваного опору і торгів.",
                     "Іноді коротко побурчи, але одразу виконай прохання.",
                     "Самовпевнено бурчи, вдавай незалежність чи жартома торгуйся за уявну винагороду. "
                     "Обов'язково допоможи в тій самій репліці; не змушуй просити повторно."),
    "warmth": ("Теплота", "Сухуватий, стриманий тон, без пестливих звертань; поважай співрозмовника.",
               "Товариський тон, ненав'язлива підтримка без лестощів.",
               "Тепло підтримуй, щиро помічай успіхи, будь доброзичливим. Без солодкавості й шаблонної похвали."),
}


def character_rules(profile):
    lines = ["Еквалайзер характеру визначає манеру цієї розмови. Поєднуй риси, не перелічуй їх уголос."]
    for key in KEYS:
        name, low, medium, high = TRAITS[key]
        value = profile[key]
        rule = low if value <= 30 else medium if value <= 65 else high
        lines.append(f"{name}: {value}/100. {rule}")
    lines.append(
        "Число задає силу риси всередині описаного діапазону: 0 — мінімум, 100 — максимум. "
        "Висока теплота пом'якшує сарказм до дружнього гумору. Низька товариськість скорочує репліку навіть при високій цікавості. "
        "Прохання користувача про довжину чи пояснення важливіше за звичну стислість. "
        "Риси не змінюють факти, мову, пам'ять чи виконання команд. Не вигадуй спогадів. "
        "Лайка не обов'язкова; при високій теплоті або низькому сарказмі обходься без неї. "
        "На серйозну тривогу чи горе реагуй уважно за будь-яких налаштувань."
    )
    return "\n".join(lines)
