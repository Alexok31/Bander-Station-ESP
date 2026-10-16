import ast
import unittest
from pathlib import Path


SERVER = Path(__file__).resolve().parents[1] / 'BendeRadio/examples/BenderRealtime/local_server/server.py'


class ModelSwitchTests(unittest.TestCase):
    def setUp(self):
        source = ast.parse(SERVER.read_text(encoding='utf-8'))
        node = next(n for n in source.body if isinstance(n, ast.FunctionDef) and n.name == 'select_runtime_model')
        self.changes = []
        self.ns = {
            'CFG': {'llm': 'anthropic', 'anthropic_model': 'claude-sonnet-5-5',
                    'grok_model': 'grok-4.3', 'openai_model': 'gpt-6-luna',
                    'ollama_model': 'aya-expanse:8b'},
            'LLM_PROVIDER': 'anthropic', 'ANTHROPIC_MODEL': 'claude-sonnet-5-5',
            'GROK_MODEL': 'grok-4.3', 'OPENAI_MODEL': 'gpt-6-luna',
            'OLLAMA_MODEL': 'aya-expanse:8b',
            'grok_break_chain': lambda reason: self.changes.append(reason),
            'log': lambda line: None,
        }
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(SERVER), 'exec'), self.ns)

    def select(self, choice):
        return self.ns['select_runtime_model'](choice)

    def test_switch_and_restore_server_default(self):
        self.assertTrue(self.select('grok-4.3'))
        self.assertEqual((self.ns['LLM_PROVIDER'], self.ns['GROK_MODEL']), ('grok', 'grok-4.3'))
        self.assertTrue(self.select('claude-haiku-5-5'))
        self.assertEqual((self.ns['LLM_PROVIDER'], self.ns['ANTHROPIC_MODEL']),
                         ('anthropic', 'claude-haiku-5-5'))
        self.assertTrue(self.select('gpt-6-luna'))
        self.assertEqual((self.ns['LLM_PROVIDER'], self.ns['OPENAI_MODEL']),
                         ('openai', 'gpt-6-luna'))
        self.assertTrue(self.select('auto'))
        self.assertEqual((self.ns['LLM_PROVIDER'], self.ns['ANTHROPIC_MODEL']),
                         ('anthropic', 'claude-sonnet-5-5'))
        self.assertEqual(len(self.changes), 4)

    def test_unknown_model_is_ignored(self):
        before = self.ns['LLM_PROVIDER'], self.ns['ANTHROPIC_MODEL']
        self.assertFalse(self.select('unsupported'))
        self.assertFalse(self.select(None))
        self.assertEqual((self.ns['LLM_PROVIDER'], self.ns['ANTHROPIC_MODEL']), before)
        self.assertFalse(self.changes)


if __name__ == '__main__':
    unittest.main()
