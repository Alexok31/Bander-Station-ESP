import ast
import asyncio
import json
import re
import sys
import unittest
from contextlib import aclosing
from pathlib import Path
from types import SimpleNamespace

SERVER = Path(__file__).resolve().parents[1] / 'BendeRadio/examples/BenderRealtime/local_server'
sys.path.insert(0, str(SERVER))
from character import KEYS, validate_character, character_rules, validate_preview_question, DEFAULT_QUESTION


def server_namespace():
    # Use production protocol/prompt functions without loading speech models or secrets.
    names = {'set_device_character', 'grok_break_chain', 'bender_prompt',
             'grok_turn_text', '_llm_payload', 'handle', '_grok_pieces',
             'character_preview_text', 'run_character_preview'}
    tree = ast.parse((SERVER / 'server.py').read_text(encoding='utf-8'))
    nodes = [n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name in names]
    ns = dict(asyncio=asyncio, aclosing=aclosing, re=re,
              DEVICE_CHARACTER=None, CHAT_GROK_RESP_ID='old-chain', BENDER_LEVEL=10,
              validate_character=validate_character, character_rules=character_rules,
              validate_preview_question=validate_preview_question, DEFAULT_QUESTION=DEFAULT_QUESTION,
              PROMPT_TEMPLATE=(SERVER / 'bender_prompt.txt').read_text(encoding='utf-8'),
              bender_level_rules=lambda: 'LEGACY LEVEL TEN', log=lambda message: None,
              MEMORY=SimpleNamespace(context=lambda *args: '', profile=lambda *args: {'type': 'device.profile'}),
              DEVICE_STATIONS=[], _TURN_GUIDANCE='continue the conversation',
              _is_greet=lambda text: False, _is_story=lambda text: False,
              LLM_PROVIDER='ollama', OLLAMA_MODEL='test', GROK_MODEL='test',
              CHAT_SUMMARY='', HISTORY_SEND=10, json=json, dumps=json.dumps,
              websockets=SimpleNamespace(exceptions=SimpleNamespace(ConnectionClosed=ConnectionError)))
    exec(compile(ast.Module(body=nodes, type_ignores=[]), 'character-server', 'exec'), ns)
    return ns


class CharacterTests(unittest.TestCase):
    def test_legacy_profile_retains_traits_and_adds_new_defaults(self):
        old = dict(zip(KEYS[:5], (3, 7, 11, 91, 100)))
        self.assertEqual(validate_character(old), {**old, 'roughness': 45, 'profanity': 35})
        self.assertIsNone(validate_character({**old, 'profanity': 0}))

    def test_profanity_and_roughness_are_independent(self):
        harsh = dict.fromkeys(KEYS, 100)
        harsh['profanity'] = 0
        rules = character_rules(harsh)
        self.assertIn('Грубість: 100/100. Різкий', rules)
        self.assertIn('Без мату і лайки', rules)
        friendly = {**harsh, 'roughness': 0, 'profanity': 100}
        rules = character_rules(friendly)
        self.assertIn("Грубість: 0/100. М'який", rules)
        self.assertIn("Лихослів'я: 100/100. У КОЖНІЙ", rules)
        self.assertIn('1–2 справжні матерні слова', rules)
        self.assertNotIn('Без мату і лайки', rules)

    def test_extremes_differ_from_merely_high_or_low_values(self):
        for key in KEYS:
            profiles = [{**dict.fromkeys(KEYS, 50), key: value} for value in (0, 25, 75, 100)]
            # Compare actual instructions excluding the numeric label.
            instructions = [character_rules(p).splitlines()[KEYS.index(key) + 1].split('/100. ', 1)[1]
                            for p in profiles]
            self.assertNotEqual(instructions[0], instructions[1], key)
            self.assertNotEqual(instructions[2], instructions[3], key)

    def test_maximum_profanity_is_not_satisfied_by_euphemisms(self):
        profile = dict.fromkeys(KEYS, 0)
        profile.update(profanity=100, warmth=100)
        prompt = server_namespace()['bender_prompt'](profile)
        self.assertIn('вони НЕ виконують цю настройку', prompt)
        self.assertIn('«блядь»', prompt)
        self.assertIn('при низькій грубості лайся на ситуацію', prompt)
        self.assertIn('крайніх значень мають пріоритет', prompt)

    def test_preview_question_limits_and_unicode(self):
        for bad in (None, 42, [], '', ' \n\t', 'a' * 201, 'аб' * 101, 'hello\x00world', 'a\x7fb', '\ud800'):
            self.assertIsNone(validate_preview_question(bad))
        for good in ('ї' * 200, '🙂' * 200, 'Що робити?', '<тест> & "цитата"', 'One\nTwo'):
            self.assertEqual(validate_preview_question(good), good)
        self.assertEqual(validate_preview_question('  Привіт!  '), 'Привіт!')

    def test_validation_is_atomic_and_strict(self):
        good = dict.fromkeys(KEYS, 50)
        for raw in (None, [], {}, {**good, 'foreign': 1}, {k: 50 for k in KEYS[:-1]}):
            self.assertIsNone(validate_character(raw))
        for bad in (True, False, '50', 50.0, -1, 101, None):
            self.assertIsNone(validate_character({**good, 'warmth': bad}))
        for boundary in (0, 100):
            self.assertEqual(validate_character(dict.fromkeys(KEYS, boundary)), dict.fromkeys(KEYS, boundary))
        copied = validate_character(good)
        good['warmth'] = 0
        self.assertEqual(copied['warmth'], 50)

    def test_changed_profile_resets_chain_but_reconnect_and_invalid_do_not(self):
        ns = server_namespace()
        good = dict.fromkeys(KEYS, 50)
        self.assertTrue(ns['set_device_character'](good))
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], '')
        ns['CHAT_GROK_RESP_ID'] = 'new-chain'
        ns['set_device_character'](good)
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], 'new-chain')
        self.assertFalse(ns['set_device_character']({'warmth': 100}))
        self.assertEqual(ns['DEVICE_CHARACTER'], good)
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], 'new-chain')
        ns['set_device_character']({**good, 'warmth': 100})
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], '')

    def test_eq_removes_conflicting_legacy_style_from_prompt_and_turn(self):
        ns = server_namespace()
        self.assertIn('LEGACY LEVEL TEN', ns['bender_prompt']())
        ns['set_device_character'](dict(zip(KEYS, (20, 65, 55, 15, 90, 10, 0))))
        prompt = ns['bender_prompt']()
        self.assertNotIn('LEGACY LEVEL TEN', prompt)
        self.assertNotIn('Нахабний, егоїстичний, цинічний', prompt)
        self.assertNotRegex(prompt, r'\{[A-Z_]+\}')
        self.assertIn('без іронічного перевертання сенсу', prompt)
        self.assertIn('Теплота: 90/100', prompt)
        turn = ns['grok_turn_text']('Привіт')
        self.assertNotIn('BENDER_LEVEL', turn)
        self.assertNotIn('Різкість і мат', turn)
        for required in ('Не вигадуй спогади', 'За замовчуванням українська', 'перше речення дає відповідь'):
            self.assertIn(required, prompt)

    def test_each_slider_changes_real_model_payload(self):
        ns = server_namespace()
        profile = dict.fromkeys(KEYS, 0)
        history = [{'role': 'user', 'content': 'Як справи?'}]
        ns['set_device_character'](profile)
        baseline = ns['_llm_payload']('stale prompt', 'uk', 1, True, history)['messages'][0]['content']
        for key in KEYS:
            ns['set_device_character']({**profile, key: 100})
            payload = ns['_llm_payload']('stale prompt', 'uk', 1, True, history)
            self.assertNotEqual(payload['messages'][0]['content'], baseline)
            self.assertEqual(payload['messages'][-1], history[-1])


class CharacterProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_preview_uses_unsaved_traits_without_changing_conversation(self):
        ns = server_namespace()
        saved = dict.fromkeys(KEYS, 50)
        ns['DEVICE_CHARACTER'] = saved.copy()
        ns['CHAT'] = [{'role': 'user', 'content': 'Existing conversation'}]
        payloads = []
        async def pieces(payload):
            payloads.append(payload)
            yield 'Ходімо гуляти.'
        ns.update(_ollama_pieces=pieces, _clean_llm=lambda text: text,
                  _pop_sentences=lambda text: ([text], ''))
        question = 'Чому роботи такі вперті?'
        out = await ns['character_preview_text'](dict.fromkeys(KEYS, 90), question)
        self.assertEqual(out, 'Ходімо гуляти.')
        self.assertIn('Теплота: 90/100', payloads[0]['messages'][0]['content'])
        self.assertEqual(len(payloads[0]['messages']), 2)
        self.assertEqual(payloads[0]['messages'][1], {'role': 'user', 'content': question})
        self.assertEqual(ns['DEVICE_CHARACTER'], saved)
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], 'old-chain')
        self.assertEqual(ns['CHAT'], [{'role': 'user', 'content': 'Existing conversation'}])

    async def test_grok_preview_never_uses_or_updates_remote_conversation(self):
        ns = server_namespace()
        requests = []
        class Stream:
            def raise_for_status(self): pass
            async def __aenter__(self): return self
            async def __aexit__(self, *args): pass
        class Client(Stream):
            def stream(self, method, url, headers, json):
                requests.append(json)
                return Stream()
        async def events(response):
            yield {'id': 'preview-only', 'text': 'Пішли на прогулянку.'}
        ns.update(LLM_PROVIDER='grok', XAI_API_KEY='fake', XAI_RESP_URL='unused',
                  _httpx_async=lambda **kwargs: Client(), _iter_sse=events,
                  _grok_delta_text=lambda event: event['text'], _clean_llm=lambda text: text,
                  _pop_sentences=lambda text: ([text], ''))
        await ns['character_preview_text'](dict.fromkeys(KEYS, 100), 'Порадь, як відпочити.')
        self.assertFalse(requests[0]['store'])
        self.assertNotIn('previous_response_id', requests[0])
        self.assertIn('Теплота: 100/100', requests[0]['instructions'])
        self.assertEqual(requests[0]['input'], [{'role': 'user', 'content': 'Порадь, як відпочити.'}])
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], 'old-chain')
        self.assertIsNone(ns['DEVICE_CHARACTER'])

    async def test_preview_protocol_finishes_on_success_invalid_profile_and_tts_failure(self):
        for case in ('ok', 'invalid', 'empty_audio', 'model_error', 'invalid_question', 'custom_question'):
            with self.subTest(case=case):
                ns = server_namespace()
                wire, spoken, generated = [], [], []
                async def generate(profile, question):
                    generated.append((profile, question))
                    if case == 'model_error': raise RuntimeError('offline')
                    return 'Пішли гуляти.'
                async def send(raw): wire.append(json.loads(raw))
                async def pcm(ws, value): spoken.append(value)
                ns.update(character_preview_text=generate, send_pcm_deltas=pcm,
                          synth=lambda text: b'' if case == 'empty_audio' else b'pcm')
                profile = {'warmth': 100} if case == 'invalid' else dict.fromkeys(KEYS, 50)
                event = {'character': profile, 'request_id': 7}
                if case == 'invalid_question': event['question'] = ''
                if case == 'custom_question': event['question'] = '  Як справи?  '
                await ns['run_character_preview'](SimpleNamespace(send=send), event)
                success = case in ('ok', 'custom_question')
                self.assertEqual(wire[-3], {'type': 'character.preview.result', 'request_id': 7, 'ok': success})
                self.assertEqual(wire[-1]['type'], 'response.done')
                self.assertEqual(bool(spoken), success)
                self.assertEqual(bool(generated), case not in ('invalid', 'invalid_question'))
                if generated:
                    self.assertEqual(generated[0][1], 'Як справи?' if case == 'custom_question' else DEFAULT_QUESTION)
                self.assertEqual(ns['CHAT_GROK_RESP_ID'], 'old-chain')

    async def test_grok_receives_new_style_with_history_then_reuses_chain(self):
        ns = server_namespace()
        requests = []
        class Stream:
            status_code = 200
            async def __aenter__(self): return self
            async def __aexit__(self, *args): pass
        class Client(Stream):
            def stream(self, method, url, headers, json):
                requests.append(json)
                return Stream()
        async def events(response):
            yield {'id': 'fresh-chain', 'text': 'Слухаю.'}
        ns.update(re=re, aclosing=aclosing, XAI_API_KEY='fake', XAI_RESP_URL='unused',
                  CHAT_CONV_ID='test', _httpx_async=Client, _iter_sse=events,
                  _grok_event_id=lambda ev: ev['id'], _grok_delta_text=lambda ev: ev['text'],
                  _history_user_asst_nudge=lambda history: (history[-1]['content'], '', ''))
        profile = dict.fromkeys(KEYS, 20)
        ns['set_device_character'](profile)
        history = [{'role': 'user', 'content': 'Розкажи про Марс.'},
                   {'role': 'assistant', 'content': 'Марс — планета.'},
                   {'role': 'user', 'content': 'А далі?'}]
        self.assertEqual([part async for part in ns['_grok_pieces'](history, 140, .68)], ['Слухаю.'])
        self.assertEqual(requests[0]['instructions'], ns['bender_prompt']())
        self.assertEqual(requests[0]['input'][:2], history[:2])
        self.assertNotIn('previous_response_id', requests[0])
        ns['set_device_character'](profile)  # Reconnect with the same device settings.
        _ = [part async for part in ns['_grok_pieces'](history, 140, .68)]
        self.assertEqual(requests[1]['previous_response_id'], 'fresh-chain')
        self.assertEqual(requests[1]['instructions'], ns['bender_prompt']())
        self.assertEqual(len(requests[1]['input']), 1)

    async def test_session_update_applies_complete_profile_and_keeps_ack(self):
        ns = server_namespace()
        profile = dict(zip(KEYS, (80, 55, 40, 70, 35, 45, 35)))
        wire = []
        class Socket:
            remote_address = 'test'
            async def send(self, raw): wire.append(json.loads(raw))
            async def __aiter__(self):
                yield json.dumps({'type': 'session.update', 'session': {'character': profile}})
                yield json.dumps({'type': 'session.update', 'session': {'character': {'warmth': 101}}})
        await ns['handle'](Socket())
        self.assertEqual(ns['DEVICE_CHARACTER'], profile)
        self.assertEqual(ns['CHAT_GROK_RESP_ID'], '')
        self.assertEqual(sum(m['type'] == 'session.updated' for m in wire), 2)


if __name__ == '__main__': unittest.main()
