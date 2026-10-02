"""Opt-in live text check using the production preview path; no TTS or conversation writes.

python agent-tools/check_character_model.py --live
Sends six short synthetic questions to the configured provider (billable for Grok).
Prints only profiles/replies; never configuration, credentials, or provider error bodies.
"""
import argparse
import ast
import asyncio
from contextlib import aclosing
from functools import partial
import json
import os
from pathlib import Path
import re
import sys
import ssl

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'BendeRadio/examples/BenderRealtime/local_server'
sys.path.insert(0, str(HERE))
from character import KEYS, character_rules, validate_preview_question, DEFAULT_QUESTION


def production_namespace():
    import httpx
    names = {'_secrets_h_key', 'load_config', 'bender_prompt', 'character_preview_text',
             '_iter_sse', '_grok_delta_text', '_ollama_pieces', '_clean_llm', '_pop_sentences'}
    tree = ast.parse((HERE / 'server.py').read_text(encoding='utf-8'))
    nodes = [node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
             and node.name in names]
    ns = dict(HERE=HERE, CONFIG_PATH=HERE/'config.json', os=os, re=re, json=json,
              DEVICE_CHARACTER=None, BENDER_LEVEL=5, character_rules=character_rules,
              validate_preview_question=validate_preview_question, DEFAULT_QUESTION=DEFAULT_QUESTION,
              PROMPT_TEMPLATE=(HERE/'bender_prompt.txt').read_text(encoding='utf-8'),
              aclosing=aclosing, _httpx_async=partial(httpx.AsyncClient, verify=ssl.create_default_context()),
              _CJK_RE=re.compile(r'[\u3400-\u9fff\u3040-\u30ff\uac00-\ud7af]'),
              XAI_RESP_URL='https://api.x.ai/v1/responses',
              OLLAMA_URL=os.environ.get('OLLAMA_URL', 'http://127.0.0.1:11434'))
    exec(compile(ast.Module(body=nodes, type_ignores=[]), 'live-character-check', 'exec'), ns)
    cfg = ns['load_config']()
    ns.update(LLM_PROVIDER=cfg['llm'], GROK_MODEL=cfg['grok_model'],
              OLLAMA_MODEL=cfg['ollama_model'], XAI_API_KEY=cfg['xai_api_key'])
    return ns


async def main(selected_cases=None):
    ns = production_namespace()
    base = dict(zip(KEYS, (0, 50, 0, 0, 70, 0, 0)))
    question = 'Бендере, порадь, як провести вечір після нудного робочого дня.'
    cases = [
        ('soft_clean', base, question),
        ('rough_clean', {**base, 'roughness': 100}, question),
        ('soft_swearing', {**base, 'profanity': 100}, question),
        ('soft_swearing_repeat', {**base, 'profanity': 100}, 'Я нарешті закінчив свій проєкт! Як тобі така новина?'),
        ('terse', {**base, 'sociability': 0, 'warmth': 0}, question),
        ('curious_talkative', {**base, 'sociability': 100, 'curiosity': 100}, question),
    ]
    if selected_cases:
        cases = [case for case in cases if case[0] in selected_cases]
        if len(cases) != len(set(selected_cases)):
            raise SystemExit('Unknown case name; no requests sent')
    out = []
    for name, profile, text in cases:
        try:
            reply = await asyncio.wait_for(ns['character_preview_text'](profile, text), timeout=35)
            record = dict(case=name, profile=profile, question=text, reply=reply)
        except Exception as error:
            record = dict(case=name, error=type(error).__name__)
        out.append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)
        if 'error' in record:
            break
    path = ROOT/('artifacts/character-model-check-selected.json' if selected_cases else 'artifacts/character-model-check.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, ensure_ascii=False, indent=2), encoding='utf-8')
    if any('error' in record for record in out):
        raise SystemExit(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live', action='store_true', help='Send six model requests; may incur API charges')
    parser.add_argument('--case', action='append', help='Run only named cases (repeatable)')
    args = parser.parse_args()
    if not args.live:
        parser.error('--live is required; no model requests sent')
    asyncio.run(main(args.case))
