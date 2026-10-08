"""Opt-in synthetic multi-turn check of the production dialogue path, without audio.

Uses the configured provider (billable), never reads chat/memory or writes them.
Run with --live; results contain only synthetic questions and model replies.
"""
import argparse
import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
import uuid

from check_character_model import production_namespace, HERE, ROOT


def dialogue_namespace():
    ns = production_namespace()
    from speech_pipeline import TurnTiming
    functions = {
        '_grok_pieces', '_grok_event_id', 'grok_turn_text', 'grok_break_chain',
        '_history_user_asst_nudge', '_llm_payload', 'iter_llm_sentences',
        '_iter_checked_reply', '_is_greet', '_is_story', '_cjk_heavy',
        '_is_canned', '_strip_loop_sents', '_reply_words', '_too_like_last',
        '_too_like_any', '_too_like_user',
        '_join_reply', '_story_fallback', '_greet_fallback', '_trim_reply_filler',
        '_model_context', '_is_social_checkin',
        '_repeats_expletive_opening',
    }
    constants = {'_NUDGE_PREFIX', '_LLM_RETRY_NUDGE', '_CANNED_RE',
                 '_LOOP_SENT_RE', '_STORY_FALLBACKS', '_GREET_FALLBACKS',
                 '_ACK_OPENING_RE', '_EMPTY_INVITATION_RE'}
    tree = ast.parse((HERE/'server.py').read_text(encoding='utf-8'))
    nodes = [n for n in tree.body if
             isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name in functions
             or isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in constants for t in n.targets)]
    ns.update(TurnTiming=TurnTiming, CHAT_GROK_RESP_ID='', CHAT_CONV_ID=str(uuid.uuid4()),
              CHAT_SUMMARY='', HISTORY_SEND=16, DEVICE_STATIONS=[],
              MEMORY=SimpleNamespace(context=lambda *args: ''), LLM_MAX_SENTS=3, LLM_STORY_SENTS=5,
              log=lambda message: None)
    exec(compile(ast.Module(body=nodes, type_ignores=[]), 'dialogue-check', 'exec'), ns)
    ns['DEVICE_CHARACTER'] = dict(zip(
        ('sarcasm', 'sociability', 'curiosity', 'stubbornness', 'warmth', 'roughness', 'profanity'),
        (80, 65, 15, 90, 10, 100, 100)))
    return ns


async def main(label, turns, scenario, clean, stale_history):
    ns = dialogue_namespace()
    if clean:
        ns['DEVICE_CHARACTER'].update(roughness=0, profanity=0, warmth=70)
    history, results = [], []
    if stale_history:
        for i in range(40):
            history.extend([
                {'role': 'user', 'content': 'Как дела?' if i % 2 == 0 else 'Что делаешь?'},
                {'role': 'assistant', 'content': 'Гаразд, чув. Нормально. Кажи, що треба.'}])
    questions = ['Як настрій?', 'Що робиш?', 'А тепер почисть унітаз.', 'Ти що, ганчірка, Бендер?']
    if scenario == 'mixed':
        questions = ['Мені нудно, вигадай нам заняття.', 'І що будемо робити далі?',
                     'Скільки буде сім помножити на вісім? Відповідай лише числом.',
                     'У мене важкий день, усе валиться з рук.']
    elif scenario == 'goodnight':
        questions = ['Пожелай пацанам спокойной ночи.', 'А теперь Сане, Андрюхе и Боде.',
                     'Придумай другое пожелание, смешное.', 'А для тех, кто завтра работает?']
    for question in questions[:turns]:
        history.append({'role': 'user', 'content': question})
        try:
            async def collect():
                return [s async for s in ns['_iter_checked_reply'](
                    question, 'uk', 1., history, ns['TurnTiming'](lambda message: None))]
            reply = ' '.join(await asyncio.wait_for(collect(), 50))
            history.append({'role': 'assistant', 'content': reply})
            result = dict(question=question, reply=reply)
        except Exception as error:
            result = dict(question=question, error=type(error).__name__)
        results.append(result)
        print(json.dumps(result, ensure_ascii=False), flush=True)
        if 'error' in result:
            break
    suffix = '-' + scenario if scenario != 'banter' else ''
    suffix += '-clean' if clean else ''
    suffix += '-stale' if stale_history else ''
    output = ROOT/'artifacts'/f'dialogue-check-{label}{suffix}.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    if any('error' in r for r in results):
        raise SystemExit(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live', action='store_true')
    parser.add_argument('--label', choices=('before', 'after'), default='after')
    parser.add_argument('--turns', type=int, choices=range(1, 5), default=4)
    parser.add_argument('--scenario', choices=('banter', 'mixed', 'goodnight'), default='banter')
    parser.add_argument('--clean', action='store_true', help='Use zero roughness/profanity and warmth=70')
    parser.add_argument('--stale-history', action='store_true', help='Seed synthetic repetitive history from the reported failure')
    args = parser.parse_args()
    if not args.live:
        parser.error('--live is required; no requests sent')
    asyncio.run(main(args.label, args.turns, args.scenario, args.clean, args.stale_history))
