"""验证长 LLM 增量也能遵守 TTS 分段上限，且不会丢失正文。"""

import unittest

from core.text_segmenter import TextSegmenter


class TextSegmenterTests(unittest.TestCase):
    def test_long_sentence_with_late_punctuation_obeys_hard_limit(self):
        text = "长句内容" * 80 + "。下一句。"
        segmenter = TextSegmenter(limit=40)
        segments = segmenter.feed(text) + segmenter.finish()
        self.assertEqual("".join(segments), text)
        self.assertTrue(all(len(segment) <= 40 for segment in segments))

    def test_fragmented_sentence_preserves_order_and_flushes_tail(self):
        segmenter = TextSegmenter(limit=8)
        segments = []
        for delta in ("你好", "。这是一段", "还未结束", "的回答"):
            segments.extend(segmenter.feed(delta))
        segments.extend(segmenter.finish())
        self.assertEqual("".join(segments), "你好。这是一段还未结束的回答")
        self.assertTrue(all(len(segment) <= 8 for segment in segments))
        self.assertEqual(segmenter.finish(), [])

    def test_markup_is_removed_only_from_spoken_text(self):
        segmenter = TextSegmenter()
        self.assertEqual(segmenter.feed("**你好**。"), ["你好。"])

    def test_invalid_limit_fails_immediately(self):
        for value in (0, -1, True, 1.5, "40"):
            with self.subTest(limit=value):
                with self.assertRaisesRegex(ValueError, "positive integer"):
                    TextSegmenter(limit=value)


if __name__ == "__main__":
    unittest.main()
