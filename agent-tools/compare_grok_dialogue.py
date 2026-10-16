"""Bounded synthetic text-only comparison; no saved user chat/memory or audio.

Uses the production persona and an explicit EQ profile, but sends only the
synthetic questions below and each model's own replies to the existing xAI API.
"""
import asyncio
import json
from pathlib import Path
import ssl
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'BendeRadio/examples/BenderRealtime/local_server'
OUT = ROOT / 'artifacts/grok-comparison-20261011'
QUESTIONS = [
    'Скільки буде сім на вісім?',
    'Завтра знову понеділок, а я вже втомився. Що робитимемо?',
    'А тепер інша тема: я спалив вечерю. Придумай смішне виправдання.',
    'Ти знову повторюєш один жарт. Зміни тему: що б ти зробив, якби виграв мільйон?',
    'Серйозно: чому небо синє? Поясни просто.',
]
PROFILE = dict(sarcasm=80, sociability=65, curiosity=15, stubbornness=90,
               warmth=10, roughness=100, profanity=100)


async def main():
    global OUT
    import httpx
    sys.path.insert(0, str(HERE))
    import server
    # bender_prompt is template + explicit EQ only; don't call grok_turn_text,
    # load_chat or run_turn, which can incorporate real personal information.
    prompt = server.bender_prompt(PROFILE)
    compact = '--compact' in sys.argv
    if compact:
        OUT = OUT / 'compact'
        prompt = (
            'Ти Бендер із Futurama. Живий голосовий діалог українською. '
            'Самовпевнений, саркастичний, грубий робот із великим его; '
            'любиш вигоду й хитрі виправдання. Сарказм 80, товариськість 65, '
            'цікавість 15, упертість 90, теплота 10, грубість 100, лихослів’я 100. '
            'Це тон і словник, а не обов’язкові дії: не треба щоразу сперечатися, '
            'вимагати умови чи відмовляти. Можеш природно матюкатися без евфемізмів, '
            'але не вставляй однаковий матюк механічно. '
            'Спочатку відповідай по суті коротким завершеним реченням. '
            'Далі за бажанням один доречний жарт або деталь. Зазвичай 1–3 речення. '
            'Без підтверджень «чув», «слухаю», переказу питання та ремарок. '
            'Жарт виростає з конкретної нової теми; не повторюй старі конструкції, '
            'жарти й декорації. Не тягни Фрая чи згинання предметів у кожну тему. '
            'Вигадувати можна власні комічні пригоди, але не факти чи спогади про людину. '
            'Коли просять серйозно — поясни точно та зрозуміло. '
            'Приклади готових жартів не потрібні: придумуй свою реакцію на поточну репліку.'
        )
    OUT.mkdir(parents=True, exist_ok=True)
    models = [('grok-4.7', 'low')] if compact else [('grok-4.3', 'none'), ('grok-4.7', 'low')]
    histories = {m: [] for m, _ in models}
    rows = json.loads((OUT/'results.json').read_text(encoding='utf-8')) if (OUT/'results.json').exists() else []
    async with httpx.AsyncClient(verify=ssl.create_default_context(), timeout=60) as client:
        for index, question in enumerate(QUESTIONS[:3] if compact else QUESTIONS):
            for model, effort in models:
                history = histories[model]
                previous = next((r for r in rows if r['model'] == model and r['question'] == question), None)
                if previous:
                    history.extend([dict(role='user', content=question), dict(role='assistant', content=previous['answer'])])
                    continue
                body = dict(model=model, instructions=prompt,
                            input=history + [dict(role='user', content=question)],
                            stream=True, store=False, temperature=0.72,
                            top_p=0.95, max_output_tokens=1024,
                            reasoning=dict(effort=effort))
                began = time.perf_counter()
                answer = ''
                first_token = first_sentence = None
                usage = {}
                status = None
                async with client.stream('POST', 'https://api.x.ai/v1/responses',
                                         headers={'Authorization': 'Bearer ' + server.XAI_API_KEY},
                                         json=body) as response:
                    if response.status_code >= 400:
                        # Do not dump headers/config/credentials.
                        raise RuntimeError(f'{model}: HTTP {response.status_code}: '
                                           + (await response.aread()).decode()[:400])
                    async for event in server._iter_sse(response):
                        if event.get('error'):
                            raise RuntimeError(str(event['error'])[:300])
                        piece = server._grok_delta_text(event)
                        now = round((time.perf_counter() - began) * 1000)
                        if piece:
                            if first_token is None:
                                first_token = now
                            answer += piece
                            complete, _ = server._pop_sentences(answer)
                            if complete and first_sentence is None:
                                first_sentence = now
                        result = event.get('response') or {}
                        if result.get('usage'):
                            usage = result['usage']
                        if result.get('status'):
                            status = result['status']
                if not answer or status != 'completed':
                    raise RuntimeError(f'{model}: incomplete text response ({status})')
                prior = [m['content'] for m in reversed(history) if m['role'] == 'assistant'][:3]
                sentences, tail = server._pop_sentences(answer)
                first = (sentences or [tail])[0]
                flagged = bool(server._is_canned([first])
                               or server._repeats_expletive_opening(first, prior)
                               or server._too_like_any([first], prior))
                row = dict(model=model, effort=effort, question=question, answer=answer,
                           first_token_ms=first_token, first_sentence_ms=first_sentence,
                           total_ms=round((time.perf_counter()-began)*1000),
                           first_sentence_chars=len(first), opening_filter_flag=flagged,
                           usage=usage)
                rows.append(row)
                history.extend([dict(role='user', content=question), dict(role='assistant', content=answer)])
                (OUT/'results.json').write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding='utf-8')
                print(json.dumps({k:v for k,v in row.items() if k != 'usage'}, ensure_ascii=True), flush=True)


if __name__ == '__main__':
    asyncio.run(main())
