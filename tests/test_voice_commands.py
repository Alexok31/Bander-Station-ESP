"""Conservative farewell recognition; no speech models or cloud calls."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] /
                       'BendeRadio/examples/BenderRealtime/local_server'))
import voice_commands


class FarewellTests(unittest.TestCase):
    def test_validated_wake_requires_invocation_at_start(self):
        valid = {
            'Привет, Бендер! Как дела?': 'Как дела?',
            'Привіт Бендер': '',
            'Эй, Бендер, пока': 'пока',
            'Бендер расскажи историю': 'расскажи историю',
            'ПриветБендер, ты тут?': 'ты тут?',
        }
        for phrase, question in valid.items():
            with self.subTest(phrase=phrase):
                self.assertEqual(voice_commands.validated_wake_question(phrase), question)
        for phrase in ('', 'щелчок', 'Бам-бам', 'бардак', 'Привет, блендер',
                       'Расскажи про Бендера', 'Как дела, Бендер?', 'Привет всем'):
            with self.subTest(phrase=phrase):
                self.assertIsNone(voice_commands.validated_wake_question(phrase))

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


class ReminderTests(unittest.TestCase):
    def test_daily_plan_and_spoken_today_list(self):
        command = voice_commands.match('Напоминай мне каждый день в 09:00 принять лекарство')
        self.assertEqual(command.name, 'reminder.daily.add')
        self.assertEqual(command.args, {'hour': 9, 'minute': 0, 'text': 'принять лекарство'})
        spoken = voice_commands.match('Нагадай мені щодня о дев\'ять ранку випити ліки')
        self.assertEqual(spoken.args, {'hour': 9, 'minute': 0, 'text': 'випити ліки'})
        evening = voice_commands.match('Напоминай каждый день в девять вечера выключить телевизор')
        self.assertEqual(evening.args['hour'], 21)
        hours = voice_commands.match('Напоминай каждый день в девять часов утра принять лекарство')
        self.assertEqual(hours.args, {'hour': 9, 'minute': 0, 'text': 'принять лекарство'})
        self.assertEqual(voice_commands.reminder_query_scope('Есть какие-то у меня планы на сегодня?'), 'today')
        self.assertEqual(voice_commands.reminder_query_scope('Какие напоминания активны сегодня?'), 'today')
        self.assertEqual(voice_commands.reminder_query_scope('Какие у меня активные напоминания?'), 'all')
        entries = [
            {'daily': True, 'today': True, 'minute_of_day': 540, 'remaining_seconds': 3600,
             'text': 'принять лекарство'},
            {'daily': False, 'today': False, 'remaining_seconds': 90000, 'text': 'снять чайник'},
        ]
        replies = voice_commands.reminder_reply_segments('today', entries, True)
        self.assertEqual(replies[1], 'Щодня о 09:00: принять лекарство.')
        self.assertEqual(len(replies), 2)
        self.assertIn('Годинник', voice_commands.reminder_reply_segments('today', entries, False)[0])

    def test_relative_reminders_in_both_languages(self):
        cases = (
            ('Напомни мне через 20 минут снять чайник', 20, 'снять чайник'),
            ('Бендер, нагадай мені через 2 години вимкнути плиту', 120, 'вимкнути плиту'),
            ('Напомни мне выключить телевизор через минуту', 1, 'выключить телевизор'),
            ('Напомню через три минуты выключить телевизор', 3, 'выключить телевизор'),
            ('Нагадай через одну хвилину вимкнути плиту', 1, 'вимкнути плиту'),
        )
        for phrase, minutes, task in cases:
            with self.subTest(phrase=phrase):
                command = voice_commands.match(phrase)
                self.assertEqual(command.name, 'reminder.add')
                self.assertEqual(command.args, {'minutes': minutes, 'text': task})

    def test_invalid_or_quoted_reminder_does_not_schedule(self):
        for phrase in ('Напомни через 0 минут снять чайник',
                       'Напомни через 1500 минут снять чайник',
                       'Напомни через 10 минут',
                       'Напомни снять чайник через много минут',
                       'Как сказать «напомни через 10 минут»?'):
            with self.subTest(phrase=phrase):
                command = voice_commands.match(phrase)
                self.assertFalse(command and command.name == 'reminder.add')


if __name__ == '__main__':
    unittest.main()
