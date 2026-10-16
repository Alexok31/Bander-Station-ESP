import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] /
                       "BendeRadio" / "examples" / "BenderRealtime" / "local_server"))
import planner_intent
import voice_commands
from uk_time import duration_after_through


class PlannerIntentTest(unittest.TestCase):
    def test_spoken_duration_uses_feminine_numbers_and_correct_endings(self):
        cases = {
            1: 'одну секунду', 2: 'дві секунди', 5: "п'ять секунд",
            11: 'одинадцять секунд', 14: 'чотирнадцять секунд',
            21: 'двадцять одну секунду', 22: 'двадцять дві секунди',
            60: 'одну хвилину', 120: 'дві хвилини', 300: "п'ять хвилин",
            660: 'одинадцять хвилин', 1260: 'двадцять одну хвилину',
            1320: 'двадцять дві хвилини', 1500: "двадцять п'ять хвилин",
            3600: 'одну годину', 7200: 'дві години', 39600: 'одинадцять годин',
            75600: 'двадцять одну годину', 86400: 'двадцять чотири години',
            3662: 'одну годину, одну хвилину і дві секунди',
        }
        for seconds, expected in cases.items():
            with self.subTest(seconds=seconds):
                self.assertEqual(duration_after_through(seconds), expected)

    def test_both_command_paths_and_list_use_spoken_duration(self):
        intent = {'kind': 'relative', 'seconds': 60, 'text': 'вимкнути плиту'}
        self.assertIn('Через одну хвилину', planner_intent.command(intent)[2])
        cmd = voice_commands.match('Нагадай через одну хвилину вимкнути плиту')
        self.assertIn('Через одну хвилину', cmd.reply(0))
        reply = voice_commands.reminder_reply_segments('all', [
            {'text': 'вимкнути плиту', 'remaining_seconds': 21}], True)
        self.assertIn('Через двадцять одну секунду', reply[-1])

    def test_model_handles_non_template_phrasing_and_seconds(self):
        result = planner_intent.parse(json.dumps({
            "kind": "relative", "seconds": 30, "text": "выключить телевизор"}))
        self.assertEqual(planner_intent.command(result)[0:2],
                         ("reminder.add", {"seconds": 30, "text": "выключить телевизор"}))

    def test_missing_task_asks_followup(self):
        result = planner_intent.parse('{"kind":"relative","seconds":60,"text":""}')
        self.assertEqual(planner_intent.command(result), (None, {}, "Що саме нагадати?"))
        self.assertTrue(planner_intent.candidate("выключить телевизор", pending=True))

    def test_invalid_model_output_cannot_schedule(self):
        for raw in ('{"kind":"relative","seconds":0,"text":"тест"}',
                    '{"kind":"relative","seconds":999999,"text":"тест"}',
                    '{"kind":"relative","seconds":true,"text":"тест"}',
                    '{"kind":"daily","hour":25,"minute":0,"text":"тест"}',
                    json.dumps({"kind": "daily", "hour": 9, "minute": 0, "text": "ж" * 100}),
                    'no json'):
            self.assertIsNone(planner_intent.parse(raw))

    def test_list_and_non_planner(self):
        self.assertTrue(planner_intent.candidate("Поставь таймер на пять минут"))
        self.assertTrue(planner_intent.candidate("Постав таймер на дві хвилини"))
        self.assertEqual(planner_intent.parse('{"kind":"list","scope":"today"}'),
                         {"kind": "list", "scope": "today"})
        self.assertEqual(planner_intent.parse('{"kind":"none"}'), {"kind": "none"})
        self.assertFalse(planner_intent.candidate("как дела?"))


if __name__ == "__main__":
    unittest.main()
