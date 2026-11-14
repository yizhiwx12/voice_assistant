"""组装大小受限的 TTS 逐词时间信息，校验失败时停止自动跟随。

上游时间识别进度追赶音频时，可能重复发送累计字幕。本模块将其转成只追加
的显示批次，不根据网络到达时间或音频总时长推测播放进度。对齐失败时保留
音频和全文，并退出自动跟随。"""

import unicodedata
import json
from bisect import bisect_left

from providers.tts.base import SubtitleCue, SubtitleWord


class SubtitleAssembler:
    """把累计字幕或原文字幕转换为大小受限的 UTF-8 显示更新。"""

    def __init__(self, *, max_display_bytes=8191, max_batch_words=24,
                 max_batch_bytes=1024):
        self.max_display_bytes = max_display_bytes
        self.max_batch_words = max_batch_words
        self.max_batch_bytes = max_batch_bytes
        self._display = bytearray()
        self._seen = set()
        self._seen_cues = set()
        self._last_text = ""
        self._last_base = 0
        self._last_end_sample = -1
        self._sequence = 0
        self.failed_reason = None

    @property
    def display_bytes(self):
        return bytes(self._display)

    @property
    def covered_sample(self):
        return max(0, self._last_end_sample)

    def _fail(self, reason):
        if self.failed_reason is None:
            self.failed_reason = reason
        return []

    @staticmethod
    def _byte_offsets(text):
        offsets = [0]
        total = 0
        for character in text:
            total += len(character.encode("utf-8"))
            offsets.append(total)
        return offsets

    @staticmethod
    def _ignorable_gap(text):
        return all(character.isspace() or unicodedata.category(character).startswith("P")
                   for character in text)

    def feed(self, cue: SubtitleCue):
        """消费一条字幕，返回零个或多个显示批次。

        每批包含 sequence、text_offset 和 text，以及携带绝对 UTF-8 字节偏移
        和会话内样本范围的逐词记录。一旦遇到错误元数据或超出显示上限，
        组装器会保持失败状态，不再返回后续更新。"""
        if self.failed_reason is not None:
            return []
        if not isinstance(cue, SubtitleCue) or not cue.text or not cue.words:
            return self._fail("invalid_cue")
        text = cue.text
        try:
            text_bytes = text.encode("utf-8")
        except UnicodeError:
            return self._fail("invalid_utf8")
        offsets = self._byte_offsets(text)
        previous_text = self._last_text
        previous_bytes = previous_text.encode("utf-8")
        keys = set()
        mapped = []
        cursor = 0
        last_start = -1
        for word in cue.words:
            if not isinstance(word, SubtitleWord) or not isinstance(word.word, str) or not word.word:
                return self._fail("invalid_word")
            key = (word.word, word.start_sample, word.end_sample)
            if key in keys:
                return self._fail("duplicate_word")
            keys.add(key)
            if (type(word.start_sample) is not int or type(word.end_sample) is not int
                    or word.start_sample < 0 or word.end_sample <= word.start_sample):
                return self._fail("invalid_timing")
            # 本地文字必须按顺序包含这些词，避免厂商规范化后
            # 文字不匹配时显示错误内容。
            position = text.find(word.word, cursor)
            if position < 0 or not self._ignorable_gap(text[cursor:position]):
                return self._fail("text_mismatch")
            start_byte, end_byte = offsets[position], offsets[position + len(word.word)]
            cursor = position + len(word.word)
            if word.start_sample < last_start:
                return self._fail("timeline_regressed")
            last_start = word.start_sample
            mapped.append((word, key, start_byte, end_byte))

        unseen = [item for item in mapped if item[1] not in self._seen]
        # 完全相同的字幕可以忽略。同样文字若带有新的时间键，
        # 则按新出现的内容处理，不视为重复。
        if not unseen:
            return [] if cue.fingerprint() in self._seen_cues else self._fail("changed_duplicate_cue")
        if len(self._seen) + len(unseen) > 16000 or len(self._seen_cues) >= 1024:
            return self._fail("metadata_overflow")

        if any(word.start_sample < self._last_end_sample for word, *_ in unseen):
            return self._fail("timeline_regressed")

        has_seen_prefix = bool(mapped and mapped[0][1] in self._seen)
        cumulative = bool(previous_text and text.startswith(previous_text) and has_seen_prefix)
        if any(item[1] in self._seen for item in mapped) and not cumulative:
            return self._fail("cumulative_text_mismatch")
        prefix_bytes = len(previous_bytes) if cumulative else 0
        append_bytes = text_bytes[prefix_bytes:] if cumulative else text_bytes
        old_display_len = len(self._display)
        if old_display_len + len(append_bytes) > self.max_display_bytes:
            return self._fail("display_overflow")
        base = old_display_len
        cue_base = self._last_base if cumulative else base
        mapped_new = []
        for word, key, start_byte, end_byte in unseen:
            mapped_new.append((word, key, cue_base + start_byte, cue_base + end_byte))

        # 迟到的累计字幕可能为已显示的文字补充时间。保留这些词
        # 的偏移，同时发出空的文字增量。
        if not append_bytes:
            batches = [(mapped_new[index:index + self.max_batch_words], base)
                       for index in range(0, len(mapped_new), self.max_batch_words)]
        else:
            batches = []
        # 按上限分组。最后一组携带追加后缀中的结尾标点或
        # 空白，使显示内容完整保留 cue.text。
        cursor_abs = base
        append_end_abs = base + len(append_bytes)
        current = []
        current_end = cursor_abs
        for index, (word, key, start_abs, end_abs) in enumerate(mapped_new if append_bytes else []):
            candidate_end = max(cursor_abs, end_abs)
            if index == len(mapped_new) - 1:
                candidate_end = append_end_abs
            candidate_start = cursor_abs
            segment = bytes(append_bytes[candidate_start - base:candidate_end - base])
            if current and (len(current) >= self.max_batch_words or
                            len(segment) > self.max_batch_bytes):
                batches.append((current, current_end))
                current = []
                cursor_abs = current_end
                candidate_start = cursor_abs
                segment = bytes(append_bytes[candidate_start - base:candidate_end - base])
            if len(segment) > self.max_batch_bytes:
                return self._fail("batch_overflow")
            current.append((word, key, start_abs, end_abs))
            current_end = candidate_end
        if current:
            batches.append((current, current_end))

        # 所有校验成功后才提交状态。
        self._display.extend(append_bytes)
        self._seen.update(item[1] for item in mapped_new)
        self._seen_cues.add(cue.fingerprint())
        self._last_text = text
        self._last_base = cue_base
        self._last_end_sample = max(self._last_end_sample,
                                    max(item[0].end_sample for item in mapped_new))
        result = []
        for records, batch_end in batches:
            # text_offset 是当前输出游标；根据前一批偏移提取文字，
            # 避免保存第二份文字缓存。
            if result:
                text_offset = result[-1]["text_offset"] + len(result[-1]["text"].encode("utf-8"))
            else:
                text_offset = base
            text_end = batch_end
            text = bytes(self._display[text_offset:text_end]).decode("utf-8") if text_end > text_offset else ""
            result.append({
                "sequence": self._sequence,
                "text_offset": text_offset,
                "text": text,
                "words": [{"text": word.word, "start_sample": word.start_sample,
                           "end_sample": word.end_sample,
                           "text_byte_offset": start_abs}
                          for word, _, start_abs, _ in records],
                "covered_sample": max(word.end_sample for word, *_ in records),
            })
            self._sequence += 1
        return result


class ResponseSubtitleAssembler:
    """把厂商时间信息对齐到一份已经提交且不可变的回答正文。

    仅忽略格式、标点、空白以及 Unicode 展示形式和大小写差异。对于实质性
    规范化（如 12 → 十二），明确拒绝映射，不猜测页码。普通组装器仍负责
    字幕校验、累计更新和重复抑制。"""

    def __init__(self, text, *, max_display_bytes=8191):
        self._text = text.encode("utf-8")
        self._tokens = self._tokenize(text)
        self._cursor = 0
        self._source_tokens = []
        self._source_starts = []
        self._source_bytes = 0
        self._sequence = 0
        self._assembler = SubtitleAssembler()
        self.max_display_bytes = max_display_bytes
        self.failed_reason = None

    @staticmethod
    def _tokenize(text):
        tokens = []
        position = 0
        for character in text:
            end = position + len(character.encode("utf-8"))
            for normalized in unicodedata.normalize("NFKC", character).casefold():
                if not (normalized.isspace() or normalized == "`" or
                        unicodedata.category(normalized).startswith("P")):
                    tokens.append((normalized, position, end))
            position = end
        return tokens

    @property
    def covered_sample(self):
        return self._assembler.covered_sample

    def feed(self, cue):
        if self.failed_reason is not None:
            return []
        batches = self._assembler.feed(cue)
        if self._assembler.failed_reason:
            self.failed_reason = self._assembler.failed_reason
            return []
        # 每次追加厂商文字后只扩展一次对齐映射，包括无时间信息的后缀。
        # 迟到的累计词复用这些不可变范围。
        appended = self._assembler.display_bytes[self._source_bytes:].decode("utf-8")
        for character, source_start, source_end in self._tokenize(appended):
            if self._cursor >= len(self._tokens) or character != self._tokens[self._cursor][0]:
                self.failed_reason = "response_text_mismatch"
                return []
            _, start, end = self._tokens[self._cursor]
            self._source_tokens.append((self._source_bytes + source_start,
                                        self._source_bytes + source_end, start, end))
            self._source_starts.append(self._source_bytes + source_start)
            self._cursor += 1
        self._source_bytes = len(self._assembler.display_bytes)
        result = []
        for batch in batches:
            words = []
            for word in batch["words"]:
                start = word["text_byte_offset"]
                end = start + len(word["text"].encode("utf-8"))
                spans = self._source_tokens[bisect_left(self._source_starts, start):
                                            bisect_left(self._source_starts, end)]
                if not spans:
                    continue  # 只有标点的事件不会触发翻页。
                first, last = spans[0][2], spans[-1][3]
                if last > self.max_display_bytes:
                    self.failed_reason = "display_overflow"
                    return []
                mapped = {**word, "text": self._text[first:last].decode("utf-8"),
                          "text_byte_offset": first}
                # 格式间隙可能显著扩大映射后的原文范围。可选元数据
                # 必须小于设备控制消息上限，
                # 并计入最坏情况下 ASCII JSON 转义和身份字段的开销。
                if len(json.dumps(mapped).encode("utf-8")) > 5500:
                    self.failed_reason = "mapped_word_overflow"
                    return []
                if words and len(json.dumps(words + [mapped]).encode("utf-8")) > 5500:
                    result.append({**batch, "sequence": self._sequence, "text": "",
                                   "text_offset": 0, "words": words,
                                   "covered_sample": max(item["end_sample"] for item in words)})
                    self._sequence += 1
                    words = []
                words.append(mapped)
            if words:
                result.append({**batch, "sequence": self._sequence,
                               "text": "", "text_offset": 0, "words": words,
                               "covered_sample": max(item["end_sample"] for item in words)})
                self._sequence += 1
        return result
