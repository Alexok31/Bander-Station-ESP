import re
import unittest
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE = (ROOT / 'BendeRadio/webui/index.html').read_text(encoding='utf-8')

class Forms(HTMLParser):
    def __init__(self):
        super().__init__()
        self.forms = []
        self.current = None
        self.ids = []
        self.resources = []
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if 'id' in a: self.ids.append(a['id'])
        if tag == 'form':
            assert self.current is None, 'nested form'
            self.current = {}
            self.forms.append(self.current)
        if self.current is not None and tag in ('input', 'textarea', 'select') and 'name' in a:
            self.current[a['name']] = a.get('value')
        if tag in ('script', 'img', 'link'):
            self.resources.append(a.get('src', a.get('href', '')))
    def handle_endtag(self, tag):
        if tag == 'form': self.current = None

class WebUiTemplateTests(unittest.TestCase):
    def setUp(self):
        self.doc = Forms()
        self.doc.feed(PAGE)
    def test_forms_are_independent(self):
        forms = {f['section']: set(f) for f in self.doc.forms}
        self.assertEqual(set(forms), {'radio', 'wifi', 'behavior', 'display', 'ai', 'character'})
        self.assertEqual(forms['radio'], {'section', 'stations'})
        self.assertEqual(forms['ai'], {'section', 'ai_ws'})
        self.assertEqual(forms['behavior'], {'section', 'motion_settings', 'wake_on_shake', 'shake_delta', 'calm_minutes'})
        self.assertNotIn('sta_ssid', forms['display'])
    def test_character_has_independent_complete_form(self):
        form = next(f for f in self.doc.forms if f['section'] == 'character')
        self.assertEqual(set(form), {'section', 'sarcasm', 'sociability', 'curiosity', 'stubbornness', 'warmth', 'roughness', 'profanity'})
        self.assertNotIn('В разработке', PAGE)
        classic = re.search(r'data-preset="classic" data-values="([0-9,]+)"', PAGE)[1]
        header = (ROOT / 'BendeRadio/CharacterSettings.h').read_text(encoding='utf-8')
        defaults = re.search(r'values\[count\] = \{([0-9, ]+)\}', header)[1].replace(' ', '')
        self.assertEqual(classic, defaults)
    def test_offline_assets_and_unique_ids(self):
        self.assertFalse(any(self.doc.resources))
        self.assertEqual(len(self.doc.ids), len(set(self.doc.ids)))
    def test_all_tokens_supplied_by_firmware(self):
        tokens = set(re.findall(r'\{\{([A-Z_]+)\}\}', PAGE))
        firmware = (ROOT / 'BendeRadio/WebUi.cpp').read_text(encoding='utf-8')
        supplied = set(re.findall(r'\{"([A-Z_]+)",', firmware))
        self.assertEqual(tokens, supplied)
    def test_embedded_page_is_current(self):
        embedded = (ROOT / 'BendeRadio/WebUiPage.h').read_text(encoding='utf-8')
        self.assertIn('R"BENDERUI(' + PAGE + ')BENDERUI"', embedded)

if __name__ == '__main__':
    unittest.main()
