import unittest
import json

from audio.subtitle_timeline import SubtitleAssembler, ResponseSubtitleAssembler
from providers.tts.base import SubtitleCue, SubtitleWord


def cue(text, *words):
    return SubtitleCue(text, tuple(SubtitleWord(*word) for word in words), 398)


class SubtitleTimelineTests(unittest.TestCase):
    def test_maps_utf8_offsets_and_preserves_spaces(self):
        assembler = SubtitleAssembler()
        result = assembler.feed(cue("你好 世界", ("你", 0, 100), ("好", 100, 200),
                                    ("世", 200, 300), ("界", 300, 400)))
        self.assertEqual(result[0]["text"], "你好 世界")
        self.assertEqual([word["text_byte_offset"] for word in result[0]["words"]], [0, 3, 7, 10])
        self.assertEqual(assembler.display_bytes.decode(), "你好 世界")

    def test_duplicate_cue_is_ignored_and_cumulative_cue_appends_suffix(self):
        assembler = SubtitleAssembler()
        first = assembler.feed(cue("你好", ("你", 0, 100), ("好", 100, 200)))
        self.assertEqual(first[0]["text"], "你好")
        self.assertEqual(assembler.feed(cue("你好", ("你", 0, 100), ("好", 100, 200))), [])
        second = assembler.feed(cue("你好世界", ("你", 0, 100), ("好", 100, 200),
                                      ("世", 200, 300), ("界", 300, 400)))
        self.assertEqual(second[0]["text"], "世界")
        self.assertEqual(second[0]["text_offset"], len("你好".encode()))
        self.assertEqual(second[0]["words"][0]["text_byte_offset"], len("你好".encode()))

    def test_late_words_for_existing_text_emit_metadata_without_text(self):
        assembler = SubtitleAssembler()
        assembler.feed(cue("你好", ("你", 0, 100)))
        result = assembler.feed(cue("你好", ("你", 0, 100), ("好", 100, 200)))
        self.assertEqual(result[0]["text"], "")
        self.assertEqual(result[0]["text_offset"], len("你好".encode()))
        self.assertEqual(result[0]["words"][0]["text_byte_offset"], len("你".encode()))

    def test_mismatch_and_time_regression_fail_closed(self):
        assembler = SubtitleAssembler()
        self.assertEqual(assembler.feed(cue("你好", ("坏", 0, 100))), [])
        self.assertEqual(assembler.failed_reason, "text_mismatch")
        self.assertEqual(assembler.feed(cue("你好", ("你", 0, 100))), [])
        assembler = SubtitleAssembler()
        assembler.feed(cue("你好", ("你", 100, 200)))
        self.assertEqual(assembler.feed(cue("世界", ("世", 90, 300))), [])
        self.assertEqual(assembler.failed_reason, "timeline_regressed")

    def test_display_and_batch_limits(self):
        assembler = SubtitleAssembler(max_display_bytes=3)
        self.assertEqual(assembler.feed(cue("你好", ("你", 0, 100), ("好", 100, 200))), [])
        self.assertEqual(assembler.failed_reason, "display_overflow")
        assembler = SubtitleAssembler(max_batch_words=1)
        result = assembler.feed(cue("你好", ("你", 0, 100), ("好", 100, 200)))
        self.assertEqual([item["text"] for item in result], ["你", "好"])
        self.assertEqual([item["sequence"] for item in result], [0, 1])

    def test_repeated_spoken_sentence_is_not_mistaken_for_duplicate(self):
        assembler = SubtitleAssembler()
        assembler.feed(cue("你好", ("你", 0, 100), ("好", 100, 200)))
        result = assembler.feed(cue("你好", ("你", 300, 400), ("好", 400, 500)))
        self.assertEqual(result[0]["text"], "你好")
        self.assertEqual(result[0]["text_offset"], 6)
        self.assertEqual(assembler.display_bytes.decode(), "你好你好")

    def test_late_metadata_only_batches_stay_bounded(self):
        assembler = SubtitleAssembler(max_batch_words=1)
        assembler.feed(cue("你好呀", ("你", 0, 100)))
        result = assembler.feed(cue("你好呀", ("你", 0, 100), ("好", 100, 200), ("呀", 200, 300)))
        self.assertEqual([item["text"] for item in result], ["", ""])
        self.assertTrue(all(len(item["words"]) == 1 for item in result))

    def test_changed_text_cannot_reuse_old_timing_keys(self):
        assembler = SubtitleAssembler()
        assembler.feed(cue("你好", ("你", 0, 100)))
        self.assertEqual(assembler.feed(cue("你坏", ("你", 0, 100), ("坏", 100, 200))), [])
        self.assertEqual(assembler.failed_reason, "cumulative_text_mismatch")

    def test_word_matching_cannot_skip_untimed_content_inside_sentence(self):
        assembler = SubtitleAssembler()
        self.assertEqual(assembler.feed(cue("你好世界", ("你", 0, 100), ("世", 100, 200))), [])
        self.assertEqual(assembler.failed_reason, "text_mismatch")


class ResponseSubtitleTimelineTests(unittest.TestCase):
    def test_maps_fixed_markdown_and_unicode_without_replacing_the_answer(self):
        assembler = ResponseSubtitleAssembler("**你好**， `ＡＩ` 很好。")
        first = assembler.feed(cue("你好 ai", ("你好", 0, 200), ("ai", 200, 400)))
        self.assertEqual(first[0]["text"], "")
        self.assertEqual(first[0]["text_offset"], 0)
        self.assertEqual(first[0]["words"][0]["text_byte_offset"], 2)
        self.assertEqual(first[0]["words"][1]["text"], "ＡＩ")
        self.assertEqual(first[0]["words"][1]["text_byte_offset"], len("**你好**， `".encode()))
        later = assembler.feed(cue("很好。", ("很", 400, 500), ("好", 500, 600)))
        self.assertEqual(later[0]["sequence"], 1)
        self.assertEqual(later[0]["words"][0]["text_byte_offset"], len("**你好**， `ＡＩ` ".encode()))

    def test_late_cumulative_timing_reuses_existing_ranges_and_repeated_sentence_advances(self):
        assembler = ResponseSubtitleAssembler("你好。你好。")
        first = cue("你好。", ("你", 0, 100))
        self.assertEqual(assembler.feed(first)[0]["words"][0]["text_byte_offset"], 0)
        self.assertEqual(assembler.feed(first), [])
        late = assembler.feed(cue("你好。", ("你", 0, 100), ("好", 100, 200)))
        self.assertEqual(late[0]["words"][0]["text_byte_offset"], 3)
        next_sentence = assembler.feed(cue("你好。", ("你", 300, 400), ("好", 400, 500)))
        self.assertEqual([word["text_byte_offset"] for word in next_sentence[0]["words"]], [9, 12])

    def test_substantive_normalization_never_guesses_a_page(self):
        assembler = ResponseSubtitleAssembler("我有12个苹果")
        self.assertEqual(assembler.feed(cue("我有十二个苹果", ("我有十二个苹果", 0, 900))), [])
        self.assertEqual(assembler.failed_reason, "response_text_mismatch")
        self.assertEqual(assembler.feed(cue("12", ("12", 1000, 1100))), [])

    def test_nonpunctuation_content_cannot_be_skipped_to_match_a_later_duplicate(self):
        assembler = ResponseSubtitleAssembler("不要跳过，后来才说你好")
        self.assertEqual(assembler.feed(cue("你好", ("你好", 0, 100))), [])
        self.assertEqual(assembler.failed_reason, "response_text_mismatch")

    def test_emoji_and_retained_utf8_prefix_limit(self):
        assembler = ResponseSubtitleAssembler("😀你好", max_display_bytes=7)
        first = assembler.feed(cue("😀你", ("😀", 0, 100), ("你", 100, 200)))
        self.assertEqual([word["text_byte_offset"] for word in first[0]["words"]], [0, 4])
        self.assertEqual(assembler.feed(cue("好", ("好", 200, 300))), [])
        self.assertEqual(assembler.failed_reason, "display_overflow")

    def test_punctuation_only_cue_does_not_leave_a_sequence_gap(self):
        assembler = ResponseSubtitleAssembler("。你好")
        self.assertEqual(assembler.feed(cue("。", ("。", 0, 100))), [])
        first = assembler.feed(cue("你好", ("你好", 100, 200)))
        self.assertEqual(first[0]["sequence"], 0)
        self.assertEqual(first[0]["words"][0]["text_byte_offset"], 3)

    def test_mapped_formatting_gap_cannot_create_an_oversized_control_frame(self):
        assembler = ResponseSubtitleAssembler("你" + ('"' * 3000) + "好")
        self.assertEqual(assembler.feed(cue("你好", ("你好", 0, 100))), [])
        self.assertEqual(assembler.failed_reason, "mapped_word_overflow")

        text = ("你" + ('"' * 140) + "好") * 24
        assembler = ResponseSubtitleAssembler(text)
        words = tuple(("你好", index * 100, (index + 1) * 100) for index in range(24))
        result = assembler.feed(cue("你好" * 24, *words))
        self.assertGreater(len(result), 1)
        self.assertEqual([item["sequence"] for item in result], list(range(len(result))))
        self.assertEqual(sum(len(item["words"]) for item in result), 24)
        for batch in result:
            self.assertLess(len(json.dumps({"type": "audio.subtitle", "session_epoch": 4294967295,
                                           "turn_id": 4294967295, "generation": 4294967295,
                                           "stream_id": 4294967295, **batch}).encode()), 8192)


if __name__ == "__main__":
    unittest.main()
