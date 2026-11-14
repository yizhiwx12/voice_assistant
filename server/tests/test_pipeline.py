import asyncio
import json
import unittest
from itertools import product
from types import SimpleNamespace

from core.pipeline import VoicePipeline
from core.session import ConversationSession
from providers.tts.base import SubtitleCue, SubtitleWord


def selection(asr="stream", llm="llm-a", tts="tts-a"):
    return SimpleNamespace(
        asr=SimpleNamespace(name=asr, adapter="qwen_batch" if asr == "batch" else "doubao_streaming"),
        llm=SimpleNamespace(name=llm, adapter="chat_completions"),
        tts=SimpleNamespace(name=tts, adapter="test_tts"),
    )


class FakeAsr:
    def __init__(self, text="问题", fail_start=False):
        self.text = text
        self.fail_start = fail_start
        self.closed = False
        self.audio = []
        self.recognitions = 0
        self.queue = asyncio.Queue()

    async def start(self):
        if self.fail_start:
            raise RuntimeError("start failed")
        self.queue.put_nowait(SimpleNamespace(kind="partial", text="问题中间结果"))
        self.queue.put_nowait(SimpleNamespace(kind="segment_final", text="问题片段"))

    async def feed_audio(self, data):
        self.audio.append(data)

    async def end_utterance(self):
        self.recognitions += 1
        return self.text

    def recognize_wav(self, data):
        self.audio.append(data)
        self.recognitions += 1
        return self.text

    async def recognize_wav_async(self, data, session):
        return self.recognize_wav(data)

    async def events(self):
        while True:
            yield await self.queue.get()

    async def close(self):
        self.closed = True


class FakeLlm:
    def __init__(self, name, *, gate=None, fail=False):
        self.name = name
        self.gate = gate
        self.fail = fail
        self.calls = 0
        self.closed = False
        self.entered_gate = asyncio.Event()

    async def stream(self, messages, session):
        self.calls += 1
        try:
            yield "第一句。"
            if self.gate:
                self.entered_gate.set()
                await self.gate.wait()
            if self.fail:
                raise RuntimeError("generation failed")
            yield "第二句。"
        finally:
            self.closed = True


class FakeTts:
    def __init__(self, name, *, fail_start=False, fail_append=False):
        self.name = name
        self.fail_start = fail_start
        self.fail_append = fail_append
        self.text = []
        self.queue = asyncio.Queue()
        self.closed = False
        self.cancelled = False

    async def start(self):
        if self.fail_start:
            raise RuntimeError("synthesis start failed")

    async def append_text(self, text):
        if self.fail_append:
            raise RuntimeError("synthesis append failed")
        self.text.append(text)
        self.queue.put_nowait(text.encode())

    async def finish(self):
        self.queue.put_nowait(None)

    async def audio(self):
        while True:
            chunk = await self.queue.get()
            if chunk is None:
                return
            yield chunk

    async def cancel(self):
        self.cancelled = True
        await self.close()

    async def close(self):
        self.closed = True


class Harness:
    def __init__(self, selected=None, *, text="问题", gate=None,
                 tts_fail_start=False, tts_fail_append=False, llm_fail=False):
        self.selected = selected or selection()
        self.asr = FakeAsr(text)
        self.llm = FakeLlm(self.selected.llm.name, gate=gate, fail=llm_fail)
        self.tts = FakeTts(self.selected.tts.name, fail_start=tts_fail_start,
                           fail_append=tts_fail_append)
        self.events = []
        self.audio = []
        self.created = []
        self.subtitle_modes = []
        self.pipeline = VoicePipeline(self.selected, {}, object(), self.emit,
                                      self.send_audio, provider_factory=self.factory)

    def factory(self, kind, entry, credentials, http):
        self.created.append((kind, entry.name))
        return getattr(self, kind)

    async def emit(self, value):
        self.events.append(value)

    async def send_audio(self, source, identity, *, subtitle_enabled=False):
        self.subtitle_modes.append(subtitle_enabled)
        async for chunk in source:
            self.audio.append((chunk, dict(identity)))

    async def run_turn(self, turn=1):
        await self.pipeline.start_utterance(turn)
        await self.pipeline.feed_audio(b"\x00\x00" * 320)
        await self.pipeline.end_utterance()
        if self.pipeline.response_task:
            await self.pipeline.response_task


class VoicePipelineTests(unittest.IsolatedAsyncioTestCase):
    async def test_tts_subtitle_capability_selects_media_iterator_and_transport_flag(self):
        cue = SubtitleCue("第一句。", (SubtitleWord("第", 0, 240),), 398)

        class SubtitleTts(FakeTts):
            subtitle_enabled = True

            async def audio(self):
                raise AssertionError("Subtitle TTS must use its media iterator")
                yield  # 使兼容方法仍保持异步迭代器类型。

            async def media(self):
                yield cue
                async for part in super().audio():
                    yield part

        for subtitles in (False, True):
            with self.subTest(subtitles=subtitles):
                h = Harness()
                if subtitles:
                    h.tts = SubtitleTts("subtitle-tts")
                try:
                    await h.run_turn()
                    self.assertEqual(h.subtitle_modes, [subtitles])
                    self.assertEqual(h.events[-1]["reason"], "complete")
                    emitted_cues = [item for item, _ in h.audio if isinstance(item, SubtitleCue)]
                    self.assertEqual(emitted_cues, [cue] if subtitles else [])
                    self.assertTrue(any(isinstance(item, bytes) for item, _ in h.audio))
                finally:
                    await h.pipeline.close()

    async def test_response_started_is_emitted_before_audio_transport_starts(self):
        h = Harness()
        order = []
        emit_entered = asyncio.Event()
        release_emit = asyncio.Event()

        async def emit(event):
            if event["type"] == "response.started":
                # 通过真实的任务调度切换，模拟 response.started 到达设备前
                # 发生的 WebSocket 背压。
                emit_entered.set()
                await release_emit.wait()
            h.events.append(event)
            order.append(("event", event["type"]))

        async def send_audio(source, identity, *, subtitle_enabled=False):
            order.append(("audio", "start"))
            async for chunk in source:
                h.audio.append((chunk, dict(identity)))

        h.pipeline.emit_callback = emit
        h.pipeline.send_audio = send_audio
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(emit_entered.wait(), 1)
            await asyncio.sleep(0)
            self.assertNotIn(("audio", "start"), order)
            release_emit.set()
            await asyncio.wait_for(h.pipeline.response_task, 1)
            started = order.index(("event", "response.started"))
            transport = order.index(("audio", "start"))
            self.assertLess(started, transport)
        finally:
            release_emit.set()
            await h.pipeline.close()

    async def test_complete_answer_precedes_tts_append_backpressure(self):
        h = Harness()
        append_entered = asyncio.Event()
        release_append = asyncio.Event()
        second_delta_ready = asyncio.Event()

        async def append(text):
            append_entered.set()
            await release_append.wait()
            h.tts.text.append(text)

        h.tts.append_text = append

        async def stream(messages, session):
            yield "第一句。"
            second_delta_ready.set()
            yield "第二句。"

        h.llm.stream = stream
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.feed_audio(b"\x00\x00" * 320)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(append_entered.wait(), 1)
            # 合成背压不能再阻止后续正文提交：
            # 完整回答在合成开始前已经提交。
            await asyncio.sleep(0)
            self.assertTrue(second_delta_ready.is_set())
            self.assertEqual(
                [event["text"] for event in h.events if event["type"] == "llm.delta"],
                ["第一句。", "第二句。"],
            )
            self.assertEqual(sum(event["type"] == "llm.done" for event in h.events), 1)
            self.assertEqual(h.pipeline.committed_answer, "第一句。第二句。")
            release_append.set()
            await asyncio.wait_for(h.pipeline.response_task, 1)
            self.assertTrue(second_delta_ready.is_set())
            self.assertEqual(
                "".join(event["text"] for event in h.events if event["type"] == "llm.delta"),
                "第一句。第二句。",
            )
        finally:
            release_append.set()
            await h.pipeline.close()

    async def test_cancel_during_response_started_backpressure_never_starts_audio(self):
        h = Harness()
        emit_entered = asyncio.Event()

        async def emit(event):
            if event["type"] == "response.started":
                emit_entered.set()
                await asyncio.Event().wait()
            h.events.append(event)

        h.pipeline.emit_callback = emit
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(emit_entered.wait(), 1)
            await asyncio.wait_for(h.pipeline.cancel_response(), 1)
            self.assertEqual(h.subtitle_modes, [])
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertEqual(h.llm.calls, 0)
            self.assertEqual(
                sum(event["type"] == "response.cancelled" for event in h.events), 1)
        finally:
            await h.pipeline.close()

    async def test_llm_done_commit_backpressure_prevents_any_tts_or_audio_start(self):
        h = Harness()
        commit_entered, release_commit = asyncio.Event(), asyncio.Event()
        order = []
        original_factory = h.pipeline.factory

        def factory(kind, *args):
            order.append(("factory", kind))
            return original_factory(kind, *args)

        async def emit(event):
            if event["type"] == "llm.done":
                commit_entered.set()
                await release_commit.wait()
            h.events.append(event)
            order.append(("event", event["type"]))

        async def send_audio(source, identity, *, subtitle_enabled=False):
            order.append(("audio", "start"))
            async for chunk in source:
                h.audio.append((chunk, dict(identity)))

        h.pipeline.factory = factory
        h.pipeline.emit_callback = emit
        h.pipeline.send_audio = send_audio
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(commit_entered.wait(), 1)
            self.assertTrue(h.llm.closed)
            self.assertEqual("".join(e["text"] for e in h.events if e["type"] == "llm.delta"),
                             "第一句。第二句。")
            self.assertNotIn(("factory", "tts"), order)
            self.assertNotIn(("audio", "start"), order)
            release_commit.set()
            await asyncio.wait_for(h.pipeline.response_task, 1)
            done = order.index(("event", "llm.done"))
            self.assertLess(done, order.index(("factory", "tts")))
            self.assertLess(done, order.index(("audio", "start")))
        finally:
            release_commit.set()
            await h.pipeline.close()

    async def test_cancel_during_complete_text_commit_never_starts_tts(self):
        h = Harness()
        commit_entered = asyncio.Event()

        async def emit(event):
            if event["type"] == "llm.done":
                commit_entered.set()
                await asyncio.Event().wait()
            h.events.append(event)

        h.pipeline.emit_callback = emit
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(commit_entered.wait(), 1)
            await asyncio.wait_for(h.pipeline.cancel_response(), 1)
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertEqual(h.subtitle_modes, [])
            self.assertIsNone(h.pipeline.committed_answer)
            self.assertEqual(sum(e["type"] == "response.cancelled" for e in h.events), 1)
            self.assertTrue(h.pipeline.session.history[-1]["interrupted"])
        finally:
            await h.pipeline.close()

    async def test_large_provider_delta_is_losslessly_split_under_control_frame_bound(self):
        h = Harness()
        # 覆盖四字节 UTF-8、JSON 转义和控制字符。两种
        # ensure_ascii 传输模式都必须满足设备解析器上限。
        answer = ('语\U0001f642"\\\n' * 1800) + "结束。"

        async def stream(messages, session):
            yield answer

        h.llm.stream = stream
        try:
            await h.run_turn()
            deltas = [event for event in h.events if event["type"] == "llm.delta"]
            self.assertGreater(len(deltas), 1)
            self.assertEqual("".join(event["text"] for event in deltas), answer)
            for event in h.events:
                for ensure_ascii in (True, False):
                    wire = json.dumps(event, ensure_ascii=ensure_ascii).encode("utf-8")
                    self.assertLess(len(wire), 8192)
            kinds = [event["type"] for event in h.events]
            self.assertEqual(kinds.count("llm.done"), 1)
            self.assertGreater(kinds.index("llm.done"), max(
                index for index, kind in enumerate(kinds) if kind == "llm.delta"))
            self.assertEqual(h.pipeline.committed_answer, answer)
            self.assertEqual(h.events[-1]["reason"], "complete")
        finally:
            await h.pipeline.close()

    async def test_all_eight_provider_combinations_keep_independent_selections(self):
        for asr, llm, tts in product(("batch", "stream"), ("llm-a", "llm-b"), ("tts-a", "tts-b")):
            with self.subTest(asr=asr, llm=llm, tts=tts):
                h = Harness(selection(asr, llm, tts))
                try:
                    await h.run_turn()
                    self.assertCountEqual(h.created, [("asr", asr), ("tts", tts), ("llm", llm)])
                    self.assertEqual(h.asr.recognitions, 1)
                    self.assertEqual(h.llm.calls, 1)
                    self.assertEqual("".join(h.tts.text), "第一句。第二句。")
                    self.assertEqual(h.events[-1]["type"], "response.done")
                    self.assertEqual(h.events[-1]["reason"], "complete")
                    self.assertTrue(h.asr.closed)
                    self.assertTrue(h.tts.closed)
                finally:
                    await h.pipeline.close()

    async def test_partial_and_segment_final_do_not_invoke_llm(self):
        h = Harness()
        try:
            await h.pipeline.start_utterance(1)
            # 给独立的上游事件任务一次发布机会。
            for _ in range(3):
                await asyncio.sleep(0)
            types = [event["type"] for event in h.events]
            self.assertIn("asr.partial", types)
            self.assertIn("asr.segment_final", types)
            self.assertNotIn("asr.utterance_final", types)
            self.assertEqual(h.llm.calls, 0)
        finally:
            await h.pipeline.close()

    async def test_repeated_end_submits_one_llm_request(self):
        h = Harness()
        try:
            await h.run_turn()
            await h.pipeline.end_utterance()
            await h.pipeline.end_utterance()
            self.assertEqual(h.asr.recognitions, 1)
            self.assertEqual(h.llm.calls, 1)
            self.assertEqual(sum(event["type"] == "asr.utterance_final" for event in h.events), 1)
        finally:
            await h.pipeline.close()

    async def test_empty_utterance_does_not_create_llm_or_tts(self):
        h = Harness(text=" ")
        try:
            await h.run_turn()
            self.assertEqual(h.created, [("asr", "stream")])
            self.assertEqual(h.events[-1]["reason"], "empty_utterance")
        finally:
            await h.pipeline.close()

    async def test_history_eviction_does_not_skip_provider_cleanup(self):
        # ASR 允许较长转写；问题刚好占满预算，追加回答会淘汰整对历史。
        h = Harness(text="问" * 32000)
        try:
            await h.run_turn()
            self.assertEqual(h.pipeline.session.history, [])
            self.assertTrue(h.tts.closed)
            self.assertTrue(h.llm.closed)
            self.assertIsNone(h.pipeline.tts)
            self.assertEqual(h.events[-1]["reason"], "complete")
            # 清理结束后，同一连接可以继续下一轮。
            h.asr = FakeAsr("下一问")
            h.llm = FakeLlm("test-llm")
            h.tts = FakeTts("test-tts")
            await h.run_turn(turn=2)
            self.assertEqual(h.pipeline.session.history[0]["content"], "下一问")
            self.assertTrue(h.tts.closed)
        finally:
            await h.pipeline.close()

    async def test_tts_start_or_append_failure_preserves_text_answer(self):
        for failure in ("start", "append"):
            with self.subTest(failure=failure):
                h = Harness(tts_fail_start=failure == "start", tts_fail_append=failure == "append")
                try:
                    await h.run_turn()
                    self.assertEqual("".join(e["text"] for e in h.events if e["type"] == "llm.delta"),
                                     "第一句。第二句。")
                    self.assertEqual(h.pipeline.session.history[-1]["content"], "第一句。第二句。")
                    self.assertEqual(h.events[-1]["reason"], "tts_failed")
                    self.assertTrue(h.tts.closed)
                finally:
                    await h.pipeline.close()

    async def test_cancel_awaits_old_generation_before_next_turn(self):
        gate = asyncio.Event()
        h = Harness(gate=gate)
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await h.llm.entered_gate.wait()
            old = dict(h.pipeline.response_identity)
            await h.pipeline.start_utterance(2)
            boundary = len(h.events)
            gate.set()
            for _ in range(3):
                await asyncio.sleep(0)
            self.assertFalse(any(e["type"] == "llm.delta" for e in h.events[boundary:]))
            self.assertTrue(h.llm.closed)
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertEqual(h.subtitle_modes, [])
            self.assertFalse(any(e["type"] == "llm.done" for e in h.events))
            cancelled = [e for e in h.events if e["type"] == "response.cancelled"]
            self.assertEqual(len(cancelled), 1)
            self.assertEqual({key: cancelled[0][key] for key in old}, old)
            assistant = [item for item in h.pipeline.session.history if item["role"] == "assistant"]
            self.assertTrue(assistant[-1]["interrupted"])
            self.assertFalse(assistant[-1]["playback_confirmed"])
        finally:
            await h.pipeline.close()

    async def test_cancel_awaits_llm_finalization_without_starting_tts(self):
        h = Harness()
        close_entered = asyncio.Event()
        close_gate = asyncio.Event()

        async def stream(messages, http):
            try:
                yield "第一句。"
                yield "第二句。"
            finally:
                close_entered.set()
                # 模拟适配器等待自己的未完成请求退出。
                # 即使调用者取消生成，适配器仍负责完成这项等待。
                try:
                    await close_gate.wait()
                except asyncio.CancelledError:
                    await close_gate.wait()
                    raise
                finally:
                    h.llm.closed = True

        # 保留强引用，避免测试依靠垃圾回收触发的终结流程而误通过。
        retained_stream = stream([], None)
        h.llm.stream = lambda messages, http: retained_stream

        cancel_task = None
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(close_entered.wait(), 1)
            cancel_task = asyncio.create_task(h.pipeline.cancel_response())
            await asyncio.sleep(0)
            self.assertFalse(cancel_task.done())
            self.assertFalse(h.llm.closed)
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertFalse(any(event["type"] == "llm.done" for event in h.events))
            self.assertFalse(any(event["type"] == "response.cancelled" for event in h.events))
            close_gate.set()
            await cancel_task
            self.assertTrue(h.llm.closed)
            self.assertEqual(h.subtitle_modes, [])
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertEqual(sum(event["type"] == "response.cancelled" for event in h.events), 1)
            self.assertEqual(h.pipeline.answer, "第一句。第二句。")
        finally:
            close_gate.set()
            if cancel_task:
                await cancel_task
            await retained_stream.aclose()
            await h.pipeline.close()

    async def test_cancel_after_response_done_preempts_graceful_tts_close_before_ack(self):
        h = Harness()
        close_entered, release_close = asyncio.Event(), asyncio.Event()
        close_finished = asyncio.Event()

        async def graceful_close():
            close_entered.set()
            await release_close.wait()
            h.tts.closed = True
            close_finished.set()

        async def cancel():
            h.tts.cancelled = True
            release_close.set()
            await graceful_close()

        h.tts.close = graceful_close
        h.tts.cancel = cancel
        cancel_task = None
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(close_entered.wait(), 1)
            self.assertTrue(any(event["type"] == "response.done" for event in h.events))
            self.assertFalse(h.pipeline.response_task.done())
            self.assertFalse(close_finished.is_set())
            cancel_task = asyncio.create_task(h.pipeline.cancel_response())
            await asyncio.wait_for(asyncio.shield(cancel_task), 0.5)
            self.assertTrue(h.tts.cancelled)
            self.assertTrue(close_finished.is_set())
            self.assertIsNone(h.pipeline.tts)
            self.assertIsNone(h.pipeline.response_task)
            self.assertEqual(sum(event["type"] == "response.cancelled" for event in h.events), 1)
            self.assertTrue(h.pipeline.session.history[-1]["interrupted"])
        finally:
            release_close.set()
            if cancel_task:
                await cancel_task
            await h.pipeline.close()

    async def test_cancel_preempts_audio_iterator_close_before_joining_audio_task(self):
        h = Harness()
        audio_entered, close_entered = asyncio.Event(), asyncio.Event()
        release_close, audio_closed = asyncio.Event(), asyncio.Event()
        append_entered = asyncio.Event()

        async def audio():
            try:
                audio_entered.set()
                while True:
                    yield await h.tts.queue.get()
            finally:
                close_entered.set()
                await release_close.wait()
                audio_closed.set()

        async def cancel():
            h.tts.cancelled = True
            release_close.set()
            h.tts.closed = True

        async def append(text):
            append_entered.set()
            await asyncio.Event().wait()

        h.tts.audio = audio
        h.tts.cancel = cancel
        h.tts.append_text = append
        cancel_task = None
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(append_entered.wait(), 1)
            await asyncio.wait_for(audio_entered.wait(), 1)
            cancel_task = asyncio.create_task(h.pipeline.cancel_response())
            await asyncio.wait_for(asyncio.shield(cancel_task), 0.5)
            self.assertTrue(close_entered.is_set())
            self.assertTrue(audio_closed.is_set())
            self.assertTrue(h.tts.closed)
            self.assertTrue(h.llm.closed)
            self.assertEqual(sum(event["type"] == "response.cancelled" for event in h.events), 1)
        finally:
            release_close.set()
            if cancel_task:
                await cancel_task
            await h.pipeline.close()

    async def test_cancel_while_awaiting_audio_tail_requests_fast_close_before_join(self):
        h = Harness()
        audio_waiting, audio_closed = asyncio.Event(), asyncio.Event()
        release_close = asyncio.Event()

        async def audio():
            try:
                yield b"\x00\x00"
                audio_waiting.set()
                await asyncio.Event().wait()
            finally:
                await release_close.wait()
                audio_closed.set()

        async def cancel():
            h.tts.cancelled = True
            release_close.set()
            h.tts.closed = True

        h.tts.audio = audio
        h.tts.cancel = cancel
        cancel_task = None
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await asyncio.wait_for(audio_waiting.wait(), 1)
            self.assertTrue(h.llm.closed)
            self.assertFalse(any(event["type"] == "response.done" for event in h.events))
            cancel_task = asyncio.create_task(h.pipeline.cancel_response())
            await asyncio.wait_for(asyncio.shield(cancel_task), 0.5)
            self.assertTrue(audio_closed.is_set())
            self.assertTrue(h.tts.closed)
            self.assertEqual(sum(event["type"] == "response.cancelled" for event in h.events), 1)
        finally:
            release_close.set()
            if cancel_task:
                await cancel_task
            await h.pipeline.close()

    async def test_llm_failure_never_starts_tts_and_marks_partial_history_interrupted(self):
        h = Harness(llm_fail=True)
        try:
            await h.run_turn()
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertEqual(h.subtitle_modes, [])
            self.assertTrue(h.llm.closed)
            self.assertTrue(h.pipeline.session.history[-1]["interrupted"])
            self.assertIn("response_failed", [e.get("code") for e in h.events])
        finally:
            await h.pipeline.close()

    async def test_batch_audio_limit_and_odd_samples_are_rejected(self):
        h = Harness(selection(asr="batch"))
        try:
            await h.pipeline.start_utterance(1)
            with self.assertRaisesRegex(ValueError, "PCM16"):
                await h.pipeline.feed_audio(b"\x00")
            with self.assertRaisesRegex(ValueError, "30 seconds"):
                await h.pipeline.feed_audio(b"\x00" * 960002)
        finally:
            await h.pipeline.close()

    async def test_asr_start_failure_clears_capture_and_closes_provider(self):
        h = Harness()
        h.asr.fail_start = True
        with self.assertRaisesRegex(RuntimeError, "start failed"):
            await h.pipeline.start_utterance(1)
        self.assertFalse(h.pipeline.recording)
        self.assertTrue(h.asr.closed)
        self.assertIsNone(h.pipeline.asr)
        await h.pipeline.close()

    async def test_llm_factory_failure_is_contained_before_tts_is_created(self):
        h = Harness()
        original = h.pipeline.factory

        def factory(kind, *args):
            if kind == "llm":
                raise RuntimeError("factory failed")
            return original(kind, *args)

        h.pipeline.factory = factory
        try:
            await h.run_turn()
            self.assertIn("response_failed", [event.get("code") for event in h.events])
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
        finally:
            await h.pipeline.close()

    async def test_tts_factory_failure_preserves_full_llm_text(self):
        h = Harness()
        original = h.pipeline.factory

        def factory(kind, *args):
            if kind == "tts":
                raise RuntimeError("factory failed")
            return original(kind, *args)

        h.pipeline.factory = factory
        try:
            await h.run_turn()
            self.assertEqual("".join(e["text"] for e in h.events if e["type"] == "llm.delta"),
                             "第一句。第二句。")
            self.assertEqual(h.events[-1]["reason"], "tts_failed")
        finally:
            await h.pipeline.close()

    async def test_tts_tail_append_failure_preserves_full_llm_text(self):
        h = Harness(tts_fail_append=True)

        async def stream(messages, session):
            yield "不含句号的尾部回答"

        h.llm.stream = stream
        try:
            await h.run_turn()
            self.assertEqual("".join(e["text"] for e in h.events if e["type"] == "llm.delta"),
                             "不含句号的尾部回答")
            self.assertEqual(h.events[-1]["reason"], "tts_failed")
            self.assertTrue(h.tts.closed)
        finally:
            await h.pipeline.close()

    async def test_next_utterance_is_rejected_while_previous_asr_finishes(self):
        h = Harness()
        gate = asyncio.Event()
        entered = asyncio.Event()

        async def finish_asr():
            entered.set()
            await gate.wait()
            return "问题"

        h.asr.end_utterance = finish_asr
        try:
            await h.pipeline.start_utterance(1)
            ending = asyncio.create_task(h.pipeline.end_utterance())
            await entered.wait()
            with self.assertRaises(ValueError):
                await h.pipeline.start_utterance(2)
            self.assertEqual(h.pipeline.session.turn, 1)
            gate.set()
            await ending
            await h.pipeline.response_task
        finally:
            await h.pipeline.close()

    async def test_transport_dropping_a_sealed_text_commit_never_starts_tts(self):
        h = Harness()

        async def emit(event):
            if event["type"] == "llm.done":
                return False
            h.events.append(event)

        h.pipeline.emit_callback = emit
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            with self.assertRaises(asyncio.CancelledError):
                await h.pipeline.response_task
            self.assertNotIn(("tts", h.selected.tts.name), h.created)
            self.assertEqual(h.audio, [])
            self.assertIsNone(h.pipeline.committed_answer)
            self.assertTrue(h.pipeline.session.history[-1]["interrupted"])
        finally:
            await h.pipeline.close()

    async def test_tts_audio_failure_preserves_text_and_reports_completion(self):
        h = Harness()

        async def send_audio(source, identity, *, subtitle_enabled=False):
            raise RuntimeError("audio failed")

        h.pipeline.send_audio = send_audio
        try:
            await h.run_turn()
            self.assertEqual("".join(e["text"] for e in h.events if e["type"] == "llm.delta"),
                             "第一句。第二句。")
            self.assertEqual(h.events[-1]["type"], "response.done")
            self.assertEqual(h.events[-1]["reason"], "tts_failed")
        finally:
            await h.pipeline.close()

    async def test_close_resets_capture_and_releases_buffer(self):
        h = Harness(selection(asr="batch"))
        await h.pipeline.start_utterance(1)
        await h.pipeline.feed_audio(b"\x00\x00" * 20)
        await h.pipeline.close()
        self.assertFalse(h.pipeline.recording)
        self.assertFalse(h.pipeline.finishing)
        self.assertEqual(h.pipeline.pcm, b"")
        self.assertTrue(h.asr.closed)

    async def test_playback_confirmation_before_history_append_is_preserved(self):
        h = Harness()

        async def emit(event):
            await h.emit(event)
            if event["type"] == "response.done":
                h.pipeline.confirm_playback({key: event[key] for key in (
                    "session_epoch", "turn_id", "generation")})

        h.pipeline.emit_callback = emit
        try:
            await h.run_turn()
            assistant = h.pipeline.session.history[-1]
            self.assertTrue(assistant["playback_confirmed"])
            await h.pipeline.start_utterance(2)
            self.assertFalse(assistant["interrupted"])
        finally:
            await h.pipeline.close()

    async def test_next_turn_during_tts_cleanup_preserves_cleanup_and_history(self):
        h = Harness()
        close_entered = asyncio.Event()
        close_gate = asyncio.Event()

        async def close():
            close_entered.set()
            await close_gate.wait()
            h.tts.closed = True

        h.tts.close = close
        try:
            await h.pipeline.start_utterance(1)
            await h.pipeline.end_utterance()
            await close_entered.wait()
            next_turn = asyncio.create_task(h.pipeline.start_utterance(2))
            for _ in range(3):
                await asyncio.sleep(0)
            close_gate.set()
            await next_turn
            self.assertTrue(h.tts.closed)
            assistant = [item for item in h.pipeline.session.history if item["role"] == "assistant"]
            self.assertEqual(assistant[-1]["content"], "第一句。第二句。")
            self.assertTrue(assistant[-1]["interrupted"])
        finally:
            close_gate.set()
            await h.pipeline.close()

    def test_history_bounded_and_interrupted_text_is_explicit(self):
        session = ConversationSession(selection())
        for turn in range(20):
            session.add_history("user", f"question-{turn}")
            session.add_history("assistant", "answer" * 1000, interrupted=True)
        self.assertLessEqual(len(session.history), 12)
        self.assertLessEqual(sum(len(item["content"]) for item in session.history), 32000)
        self.assertEqual(session.history[0]["role"], "user")
        self.assertIn("被打断", session.messages()[-1]["content"])


if __name__ == "__main__":
    unittest.main()
