"""Conservative farewell recognition; no speech models or cloud calls."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] /
                       'BendeRadio/examples/BenderRealtime/local_server'))
import voice_commands


class FarewellTests(unittest.TestCase):
    def test_wake_prefix_keeps_immediate_question_and_only_trims_at_start(self):
        cases = {
            'Привет, Бендер! Как дела?': 'Как дела?',
            'Привіт Бендер, котра година?': 'котра година?',
            'Эй, Бендер, пока': 'пока',
            'Бендер як справи': 'як справи',
            'ПриветБендер как дела?': 'как дела?',
            'Бендер': '', 'Привіт, Бендере!': '', 'Эй Бендер!': '',
            'Расскажи про Бендера': 'Расскажи про Бендера',
            'Как сказать привет Бендер?': 'Как сказать привет Бендер?',
            'Привет, как дела?': 'Привет, как дела?',
            'Бендеровский район': 'Бендеровский район',
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(voice_commands.wake_question(text), expected)

    def test_short_farewells_and_address(self):
        for text in ('Пока!', 'пока-пока', 'До свидания.', 'Ну всё, Бендер, пока!',
                     'До побачення, Бендере!', 'Бувай', 'До зустрічі',
                     'На добраніч!', 'Спокойной ночи', 'Спасибо, пока, Бендер',
                     'Дякую, бувай!', 'Ладно, давай, до встречи.'):
            with self.subTest(text=text):
                command = voice_commands.match(text)
                self.assertIsNotNone(command)
                self.assertEqual(command.name, 'conversation.end')
                self.assertTrue(command.replies)

    def test_ordinary_speech_and_quoted_goodbyes_do_not_end_conversation(self):
        for text in ('Пока не знаю', 'Я пока думаю', 'Пока расскажи про Марс',
                     'Не говори пока', 'Как сказать до свидания по-украински?',
                     'Запомни слово пока', 'Пока включи радио', 'Мне сказали пока',
                     'До встречи осталось два дня', 'Расскажи сказку на добраніч',
                     'Бендер', '', 'Спасибо', 'Все', 'Давай', 'Покажи погоду'):
            with self.subTest(text=text):
                command = voice_commands.match(text)
                self.assertFalse(command and command.name == 'conversation.end')


if __name__ == '__main__':
    unittest.main()
