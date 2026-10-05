"""Publish a local, clearly experimental model bundle and an evidence report."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',default='prototype-v3');args=parser.parse_args()
    folder=ROOT/'artifacts/wake-training'/args.run
    report=json.loads((folder/'report.json').read_text(encoding='utf-8'))
    config=json.loads((folder/'model.json').read_text(encoding='utf-8'))
    data=ROOT/'artifacts/wake-training'/report.get('dataset','dataset-v2')
    manifest=json.loads((data/'manifest.json').read_text(encoding='utf-8'))
    assert hashlib.sha256((folder/config['model']).read_bytes()).hexdigest()==config['sha256']
    if 'dataset_manifest_sha256' in report:
        assert hashlib.sha256((data/'manifest.json').read_bytes()).hexdigest()==report['dataset_manifest_sha256']
    heldout=json.loads((folder/'holdout.json').read_text(encoding='utf-8')) if (folder/'holdout.json').exists() else None
    background_comparison=json.loads((folder/'background-comparison.json').read_text(encoding='utf-8')) if (folder/'background-comparison.json').exists() else None
    real_sources=[r for r in manifest['sources'] if r['source']=='real']
    synthetic_sources=[r for r in manifest['sources'] if r['source']=='synthetic']
    negative_count=sum(r['label']=='negative' for r in real_sources)
    real_val=sum(r['split']=='validation' for r in real_sources)
    synthetic_val=sum(r['split']=='validation' for r in synthetic_sources)
    provenance={}
    for name in ['wake_training_audio.py','prepare_wake_training.py','generate_wake_synthetic.py',
                 'train_wake_prototype.py','requirements-wake-training.txt','wake-voice-sources.json']:
        provenance[name]=hashlib.sha256((ROOT/'agent-tools'/name).read_bytes()).hexdigest()
    provenance['dataset_manifest']=hashlib.sha256((data/'manifest.json').read_bytes()).hexdigest()
    provenance['feature_cache']=hashlib.sha256((data/'features.npz').read_bytes()).hexdigest()
    provenance['training_code_at_run']=report.get('training_code_sha256',{})
    (folder/'provenance.json').write_text(json.dumps(provenance,indent=2),encoding='utf-8')
    real=report['real_validation'];synthetic=report['synthetic_validation'];threshold=report['threshold']
    lines=[f'# Bender: {args.run}', '', '**Экспериментальная модель. На плату не установлена; для постоянного прослушивания пока не готова.**', '',
           'Фразы: «Эй, Бендер», «Привет, Бендер», «Бендер». Один бинарный выход означает любое из трёх обращений.', '',
           f'Файл: `bender.int8.tflite`, {report["model_bytes"]} байт, {report["parameters"]} параметров. Вход и выход INT8; float-тензоров в графе нет.', '',
           '## Данные и границы проверки', '',
           f'{len(real_sources)} исходных записей колонки: {len(real_sources)-negative_count} обращений и {negative_count} фрагментов речи/фона. Метки заданы пользователем; первоначальные 19 фоновых файлов исправлены по его указанию.',
           f'{len(synthetic_sources)} синтетических исходников: два голоса Piper (denis/dmitri), три обращения, обычная речь и похожие слова. Карточки исходных голосовых данных указывают CC0; URL и SHA-256 сохранены.',
           f'{manifest["training_windows"]} обучающих окон после преобразований. Validation: {real_val} реальных и {synthetic_val} синтетических исходников. Дополнительно зарезервировано test: {manifest.get("reserved_test_sources",0)}. Их аудио не используется как обучающий шум или для калибровки INT8.',
           'Реальная проверка: тот же пользователь и близкие сессии. Синтетическая: те же два голоса. Новых независимых дикторов и длинного фонового теста нет.',
           'На этой выборке выбирались веса и порог; это разработочная validation, а не независимый финальный test.', '',
           '## Результат INT8 в скользящем окне', '',
           f'Порог для приведённых чисел: {threshold:.4f}. Решение каждые 100 мс, требуются три положительных решения подряд. Проверяется полный звук записи без вырезания фразы.',
           f'- Реальные обращения: {real["detected"]} / {real["positive"]}.',
           f'- Реальный фон: {real["false_positive_clips"]} ложных вызовов на {real["negative"]} записях ({real["negative_seconds"]} с).',
           f'- Синтетические обращения: {synthetic["detected"]} / {synthetic["positive"]}.',
           f'- Синтетическая обычная речь: {synthetic["false_positive_clips"]} ложных вызовов на {synthetic["negative"]} записях.', '']
    for name,label in [('Эй, Бендер','hey_bender'),('Привет, Бендер','privet_bender'),('Бендер','bender')]:
        r=real['per_phrase'][label];lines.append(f'- «{name}»: {r["detected"]} / {r["total"]} реальных проверочных записей.')
    if heldout:
        h=heldout['metrics']
        lines += ['', '## Отложенные новые записи (test)', '',
                  'Модель и порог выбраны до этой проверки. Это новые исходные файлы того же пользователя; они не являются проверкой на других людях.',
                  f'Обращения: {h["detected"]}/{h["positive"]}. Ложное срабатывание на {h["false_positive_clips"]}/{h["negative"]} фоновых записей ({h["negative_seconds"]} секунд). Порог: {heldout["threshold"]}.',
                  f'По фразам: {json.dumps(h["per_phrase"],ensure_ascii=False)}.',
                  'Нельзя подбирать порог по этой проверке и продолжать считать её независимой.']
        if 'fresh_test' in heldout:
            fresh=heldout['fresh_test'];previous=heldout['previous_test_regression']
            lines += ['', 'Часть этого набора уже проверялась на прошлой модели и теперь служит проверкой регрессий.',
                      f'Ранее проверенные файлы: обращения {previous["detected"]}/{previous["positive"]}; ложные срабатывания {previous["false_positive_clips"]}/{previous["negative"]}.',
                      f'Новые файлы: обращения {fresh["detected"]}/{fresh["positive"]}; ложные срабатывания {fresh["false_positive_clips"]}/{fresh["negative"]}, фон {fresh["negative_seconds"]} секунд.']
    if background_comparison:
        lines += ['', '## Сравнение на одном новом фоне', '',
                  f'На {background_comparison["clips"]} одинаковых отложенных фоновых записях: прежняя модель — {background_comparison["baseline_false_positive_clips"]} ложных срабатываний, новая — {background_comparison["candidate_false_positive_clips"]}.',
                  'Пороги обеих моделей выбраны на validation до этой проверки. Проверяемые записи не использовались для обучения.']
    lines += ['', 'Если `recommended_threshold` равен null, не найден порог, одновременно дающий минимум 8/9 реальных, 24/30 синтетических обращений и ноль ложных вызовов на validation. Тогда приведённый порог 0,5 — диагностический, а не настройка для прошивки.',
              f'Результат этого ограничения: **{"пройден только на validation" if report["development_gate_passed"] else "не пройден"}**.',
              'Даже ноль ложных вызовов на 20 секундах реального фона не доказывает приемлемое число ошибок за час.', '',
              '## Подключение к плате', '',
              'Это собственный DS-CNN/log-mel прототип, **не совместимый напрямую с frontend microWakeWord или WakeNet**. Все коэффициенты и контрольный пример лежат в `frontend_reference.npz`; обработка описана в `model.json` и `wake_training_audio.py`.',
              'До интеграции нужны проверка DSP на ESP32-S3, замер памяти/времени/энергопотребления и тест одновременно с радио/AirPlay. Прошивка, I2S-логика, автоотключение и работающий AI-сервер этим экспериментом не изменены.',
              'Для общей модели потребуются настоящие записи других людей и больше независимого фона. Синтетика не заменяет эту проверку.', '',
              '## Воспроизведение', '',
              'См. `docs/wake-training.md`. В папке сохранены веса, Keras-модель, история обучения, вероятности по всем проверочным записям и контрольные суммы исходников.',
              'Первый прогон выявил ошибку границ фразы; второй — заметные ложные вызовы. Финальный выбор производится по скользящим окнам на validation, исходные записи между train/validation не переносятся.', '']
    (folder/'REPORT.md').write_text('\n'.join(lines),encoding='utf-8')
    # Bundle contains no user's voice recordings.
    with zipfile.ZipFile(folder/'bender-prototype.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for name in ['bender.int8.tflite','model.json','report.json','REPORT.md','provenance.json']:
            archive.write(folder/name,name)
        if heldout:archive.write(folder/'holdout.json','holdout.json')
        if background_comparison:archive.write(folder/'background-comparison.json','background-comparison.json')
        archive.write(data/'frontend_reference.npz','frontend_reference.npz')
        archive.write(ROOT/'agent-tools/wake_training_audio.py','wake_training_audio.py')
    with zipfile.ZipFile(folder/'bender-prototype.zip') as archive:assert archive.testzip() is None
    print(folder/'REPORT.md')


if __name__=='__main__':main()
