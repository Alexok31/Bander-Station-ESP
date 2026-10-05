"""Generate local augmentation sources, not replacements for real test speakers."""
import hashlib
import json
from pathlib import Path
import random
import wave
import onnxruntime
from piper import PiperVoice, SynthesisConfig
from piper.config import PiperConfig

ROOT = Path(__file__).resolve().parents[1] / 'artifacts/wake-training'
PHRASES = {'hey_bender': ['Эй, Бендер.', 'Эй, Бендер!', 'Эй, Бендер?'],
           'privet_bender': ['Привет, Бендер.', 'Привет, Бендер!', 'Привет, Бендер?'],
           'bender': ['Бендер.', 'Бендер!', 'Бендер?']}
NEGATIVE = [
    'Привет, как твои дела?', 'Эй, ты меня слышишь?', 'Привет, Андрей.', 'Добрый вечер.',
    'Включи музыку.', 'Сделай потише.', 'Хочу послушать радио.', 'Поставь следующую песню.',
    'Выключи колонку.', 'Мне пора идти домой.', 'Сегодня хорошая погода.', 'Какая завтра температура?',
    'Не забудь зарядить телефон.', 'Поставь чайник.', 'Давай поговорим.', 'Я немного устал.',
    'Где мои ключи?', 'Закрой дверь.', 'Открой окно.', 'Кто там пришёл?',
    'Ты купил хлеб?', 'Пойдём гулять.', 'Расскажи смешную историю.', 'Сколько сейчас времени?',
    'Мне нужен блендер.', 'Блендер стоит на кухне.', 'Перемешай суп блендером.', 'Новый тендер.',
    'Бенедикт пришёл домой.', 'Это бенефис актёра.', 'Гендерное равенство.', 'Красивый рендер.',
    'Бренды одежды.', 'Где наш менеджер?', 'Лендер и тендер.', 'Берег моря.',
    'Береги себя.', 'Белый медведь.', 'Бенгальский огонь.', 'Бензин закончился.',
    'Календарь на стене.', 'Обед готов.', 'Компьютер выключен.', 'Отличная идея.',
    'Не понимаю, что ты сказал.', 'Повтори ещё раз.', 'Говори громче.', 'Тихо, ребёнок спит.',
    'Завтра поедем за город.', 'Сейчас я работаю.', 'Почему пропала сеть?', 'Зарядка подключена.',
    'Какой процент батареи?', 'Я слушаю подкаст.', 'Настрой эквалайзер.', 'Убавь басы.',
    'Прибавь громкость.', 'Очень интересный фильм.', 'Наконец-то пятница.', 'Давай поедим.',
    'Эй, привет!', 'Пока, до встречи.', 'Алло, здравствуйте.', 'Это просто проверка микрофона.',
    'У мене все добре.', 'Увімкни радіо.', 'Я вже вдома.', 'Зараз будемо вечеряти.',
    'Дякую за допомогу.', 'Привіт, друже.', 'Дуже гарна музика.', 'На добраніч.',
    'Сто один, двести, триста.', 'Раз, два, три, четыре.', 'Нет, не надо.', 'Хорошо, договорились.',
    'Что ты думаешь об этом?', 'Музыка играет очень тихо.', 'Чашка стоит на столе.', 'Я скоро вернусь.',
]


def main():
    output = ROOT / 'synthetic'
    output.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261004)
    records = []
    for name in ('denis', 'dmitri'):
        model = ROOT / 'voices' / f'ru_RU-{name}-medium.onnx'
        config = json.loads(model.with_suffix('.onnx.json').read_text(encoding='utf-8'))
        options = onnxruntime.SessionOptions()
        options.intra_op_num_threads = 2
        options.inter_op_num_threads = 1
        voice = PiperVoice(session=onnxruntime.InferenceSession(str(model), sess_options=options,
                           providers=['CPUExecutionProvider']), config=PiperConfig.from_dict(config))
        tasks = [(label, i, texts[i % len(texts)]) for label, texts in PHRASES.items() for i in range(30)]
        tasks += [('negative', i, text) for i, text in enumerate(NEGATIVE)]
        for number, (label, i, text) in enumerate(tasks):
            ident = f'{name}_{label}_{i:03d}'
            target = output / f'{ident}.wav'
            settings = dict(length_scale=round(rng.uniform(.8, 1.22), 4),
                            noise_scale=round(rng.uniform(.45, .85), 4), noise_w_scale=round(rng.uniform(.6, .9), 4))
            if not target.exists():
                with wave.open(str(target), 'wb') as wav:
                    voice.synthesize_wav(text, wav, syn_config=SynthesisConfig(**settings))
            records.append(dict(id=ident, label=label, text=text, file=target.name, source='synthetic',
                                voice=name, config=settings, sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
            if (number + 1) % 25 == 0:
                print(f'{name}: {number+1}/{len(tasks)}', flush=True)
    (output / 'manifest.json').write_text(json.dumps({'items': records, 'seed':20261004,
        'note':'Two synthetic voices; not independent real speakers. Retain WAVs for exact reproduction.'},
        ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'Generated {len(records)} sources', flush=True)


if __name__ == '__main__':
    main()
