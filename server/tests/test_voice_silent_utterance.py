"""使用静默的本机上游验证真实 ASR 适配器与流水线边界。"""

import asyncio
import unittest

from core.pipeline import VoicePipeline
from providers.asr.doubao_streaming import DoubaoAsrError, DoubaoStreamingAsrProvider
from tests.test_doubao_streaming import FakeSession, FakeSocket, unpack_request
from tests.test_pipeline import FakeLlm, FakeTts, selection


class FinalOnlySocket(FakeSocket):
    """只有收到真正负序号的结束帧后，才返回识别数据。"""

    def __init__(self, text):
        super().__init__(final_on_end=False)
        self.final_text = text

    async def send_bytes(self, packet):
        await super().send_bytes(packet)
        message_type, flags, sequence, _ = unpack_request(packet)
        if message_type == 2 and flags == 3 and self.final_text is not None:
            await self.push({"result": {"text": self.final_text}},
                            sequence=-abs(sequence), last=True)


class SilentUtteranceHarness:
    def __init__(self, final_texts, *, timeout=0.25):
        self.timeout = timeout
        self.final_texts = iter(final_texts)
        self.events = []
        self.audio = []
        self.providers = []
        self.sockets = []
        self.created = []
        self.llm = FakeLlm("test-llm")
        self.tts = FakeTts("test-tts")
        self.pipeline = VoicePipeline(selection(), {}, None, self.emit, self.send_audio,
                                      provider_factory=self.factory)

    def factory(self, kind, entry, credentials, http):
        self.created.append(kind)
        if kind == "asr":
            socket = FinalOnlySocket(next(self.final_texts))
            provider = DoubaoStreamingAsrProvider(
                {"timeout_s": self.timeout}, {"VOLCENGINE_ASR_API_KEY": "synthetic-key"},
                FakeSession(socket),
            )
            self.providers.append(provider)
            self.sockets.append(socket)
            return provider
        return getattr(self, kind)

    async def emit(self, event):
        self.events.append(event)

    async def send_audio(self, chunks, identity, *, subtitle_enabled=False):
        async for chunk in chunks:
            self.audio.append((chunk, dict(identity)))

    async def record(self, frames):
        # 即使上游连续多个读取预算都保持静默，真实样本节奏也应使每次
        # 成功发送上游 PCM 的间隔接近 100 ms。
        for _ in range(frames):
            await self.pipeline.feed_audio(bytes(640))
            await asyncio.sleep(0.02)


class SilentUtterancePipelineTests(unittest.IsolatedAsyncioTestCase):
    def harness(self, texts, **kwargs):
        harness = SilentUtteranceHarness(texts, **kwargs)
        self.addAsyncCleanup(harness.pipeline.close)
        return harness

    async def record_long_silence(self, harness, turn):
        await harness.pipeline.start_utterance(turn)
        started = asyncio.get_running_loop().time()
        await harness.record(35)
        self.assertGreater(asyncio.get_running_loop().time() - started,
                           harness.timeout * 2)
        self.assertTrue(harness.pipeline.recording)
        self.assertFalse(harness.providers[-1]._reader.done())
        self.assertFalse(any(event["type"] in ("error", "asr.utterance_final", "response.done")
                             for event in harness.events if event["turn_id"] == turn))

    async def test_silent_recording_outlives_response_timeout_and_commits_only_on_final(self):
        harness = self.harness([""])
        await self.record_long_silence(harness, 1)
        await harness.pipeline.end_utterance()
        finals = [event for event in harness.events if event["type"] == "asr.utterance_final"]
        self.assertEqual([event["text"] for event in finals], [""])
        self.assertEqual(harness.events[-1]["type"], "response.done")
        self.assertEqual(harness.events[-1]["reason"], "empty_utterance")
        self.assertEqual(harness.created, ["asr"])
        self.assertEqual(harness.pipeline.session.history, [])
        self.assertIsNone(harness.pipeline.response_task)
        audio = [unpack_request(packet) for _, packet in harness.sockets[0].sent
                 if unpack_request(packet)[0] == 2]
        self.assertEqual(b"".join(packet[3] for packet in audio), bytes(35 * 640))
        self.assertEqual(sum(packet[1] == 3 for packet in audio), 1)
        self.assertTrue(harness.sockets[0].closed)

    async def test_same_session_accepts_a_spoken_turn_after_empty_silence(self):
        harness = self.harness(["", "next question"])
        epoch = harness.pipeline.session.epoch
        await self.record_long_silence(harness, 1)
        await harness.pipeline.end_utterance()
        await harness.pipeline.start_utterance(2)
        await harness.record(5)
        await harness.pipeline.end_utterance()
        await asyncio.wait_for(harness.pipeline.response_task, 1)
        self.assertEqual(harness.pipeline.session.epoch, epoch)
        self.assertEqual(harness.pipeline.session.turn, 2)
        self.assertEqual(harness.created.count("asr"), 2)
        self.assertEqual(harness.created.count("llm"), 1)
        self.assertEqual(harness.created.count("tts"), 1)
        self.assertEqual(harness.llm.calls, 1)
        self.assertEqual([item["content"] for item in harness.pipeline.session.history
                          if item["role"] == "user"], ["next question"])
        self.assertEqual([(event["turn_id"], event["reason"]) for event in harness.events
                          if event["type"] == "response.done"],
                         [(1, "empty_utterance"), (2, "complete")])
        self.assertFalse(any(event["type"] == "error" for event in harness.events))
        self.assertTrue(harness.audio)
        self.assertTrue(all(identity["turn_id"] == 2 for _, identity in harness.audio))
        self.assertTrue(all(socket.closed for socket in harness.sockets))

    async def test_missing_final_is_not_converted_to_an_empty_success(self):
        harness = self.harness([None], timeout=0.1)
        await harness.pipeline.start_utterance(1)
        await harness.pipeline.feed_audio(bytes(640))
        with self.assertRaises(DoubaoAsrError) as caught:
            await asyncio.wait_for(harness.pipeline.end_utterance(), 1)
        self.assertEqual(caught.exception.diagnostics()["asr_reason"], "final_timeout")
        self.assertFalse(any(event["type"] in ("asr.utterance_final", "response.done")
                             for event in harness.events))
        self.assertEqual(harness.created, ["asr"])
        self.assertEqual(harness.pipeline.session.history, [])
        self.assertTrue(harness.sockets[0].closed)


if __name__ == "__main__":
    unittest.main()
