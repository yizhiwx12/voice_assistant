import asyncio
import errno
from io import BytesIO
from types import SimpleNamespace
import wave
import unittest

import aiohttp

from core.config import EXAMPLE_CONFIG_PATH, load_voice_config, select_profile
from core.provider_registry import create_realtime_provider
from audio.subtitle_timeline import ResponseSubtitleAssembler
from providers.tts.base import SubtitleCue
from providers.tts.gpt_sovits import GptSoVitsTtsProvider, _decode_wav, _split_text


def wav_bytes(rate=32000, channels=1, frames=320, width=2, amplitude=1000):
    samples = [amplitude if index % 2 else -amplitude for index in range(frames)]
    raw = bytearray()
    for sample in samples:
        for _ in range(channels):
            raw.extend(int(sample).to_bytes(width, "little", signed=True))
    output = BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(width)
        wav.setframerate(rate)
        wav.writeframes(raw)
    return output.getvalue()


class FakeContent:
    def __init__(self, body):
        self.body = body

    async def iter_chunked(self, size):
        for offset in range(0, len(self.body), 7):
            yield self.body[offset:offset + 7]


class FakeResponse:
    def __init__(self, body, status=200):
        self.content = FakeContent(body)
        self.status = status

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        return False


class FakeSession:
    def __init__(self, body=b"", *, status=200, error=None):
        self.body = body
        self.status = status
        self.error = error
        self.payload = None

    def post(self, endpoint, *, json, timeout):
        self.endpoint = endpoint
        self.payload = json
        if self.error is not None:
            raise self.error
        return FakeResponse(self.body, self.status)


class ControlledResponse(FakeResponse):
    def __init__(self, session, index):
        super().__init__(b"")
        self.session = session
        self.index = index
        self.content = self
        self.body = wav_bytes(rate=24000, frames=2000, amplitude=1000 + index * 100)

    async def __aenter__(self):
        self.session.active += 1
        self.session.max_active = max(self.session.active, self.session.max_active)
        return self

    async def __aexit__(self, *args):
        self.session.active -= 1
        self.session.closed[self.index].set()

    async def iter_chunked(self, size):
        self.session.started[self.index].set()
        if self.index in self.session.gates:
            await self.session.gates[self.index].wait()
        if self.index in self.session.errors:
            raise self.session.errors[self.index]
        yield self.body


class ControlledSession:
    def __init__(self, *, gates=None, errors=None):
        self.gates = gates or {}
        self.errors = errors or {}
        self.started = [asyncio.Event() for _ in range(10)]
        self.closed = [asyncio.Event() for _ in range(10)]
        self.calls = []
        self.responses = []
        self.timeouts = []
        self.active = self.max_active = 0

    def post(self, endpoint, *, json, timeout):
        response = ControlledResponse(self, len(self.calls))
        self.calls.append(json)
        self.responses.append(response)
        self.timeouts.append(timeout.total)
        return response


class GptSoVitsProviderTests(unittest.IsolatedAsyncioTestCase):
    async def prepared(self, session, text="甲" * 160, **options):
        provider = GptSoVitsTtsProvider(options, {}, session)
        self.addAsyncCleanup(provider.close)
        await provider.start()
        await provider.append_text(text)
        await provider.finish()
        return provider

    def test_split_text_has_hard_limits_without_losing_or_reordering_text(self):
        texts = (
            "甲" * 241,
            "好的。" + "这里是比较完整的一句话，" * 20 + "结束。",
            "This is an English sentence, with spaces! " * 10,
            "你好！\n" * 50,
            "甲" * 38 + "。”" + "乙" * 90,
        )
        for text in texts:
            with self.subTest(text=text[:20]):
                segments = _split_text(text)
                self.assertLessEqual(len(segments[0]), 40)
                self.assertTrue(all(0 < len(segment) <= 80 for segment in segments[1:]))
                self.assertEqual("".join("".join(segments).split()), "".join(text.split()))
        self.assertGreater(len(_split_text(texts[1])[0]), len("好的。"))
        self.assertEqual(_split_text(" \n\t"), [])
        self.assertEqual([len(segment) for segment in _split_text("甲" * 33, 8, 12)], [8, 12, 12, 1])

    async def test_first_audio_precedes_second_response_and_lookahead_stays_one(self):
        gate = asyncio.Event()
        session = ControlledSession(gates={1: gate})
        provider = await self.prepared(session)
        reader = provider.audio()
        self.addAsyncCleanup(reader.aclose)
        first = await asyncio.wait_for(anext(reader), 1)
        self.assertEqual(len(first), 1920)
        await asyncio.wait_for(session.started[1].wait(), 1)
        self.assertFalse(session.closed[1].is_set())
        self.assertEqual(len(session.calls), 2)
        gate.set()
        await asyncio.wait_for(session.closed[1].wait(), 1)
        # 即使下一段已经完整预取，只要消费者仍持有当前段的第一个数据块，
        # 就不能触发第三个 HTTP 请求。
        self.assertEqual(len(session.calls), 2)
        chunks = [first] + [chunk async for chunk in reader]
        self.assertEqual([len(call["text"]) for call in session.calls], [40, 80, 40])
        self.assertEqual("".join(call["text"] for call in session.calls), "甲" * 160)
        expected = b"".join(response.body[44:] for response in session.responses)
        self.assertEqual(b"".join(chunks), expected)
        self.assertTrue(all(len(chunk) <= 1920 and len(chunk) % 2 == 0 for chunk in chunks))
        self.assertEqual(session.max_active, 1)
        self.assertEqual(session.active, 0)

    async def test_cancel_during_first_request_joins_http_and_preserves_cancellation(self):
        session = ControlledSession(gates={0: asyncio.Event()})
        provider = await self.prepared(session)
        reader = provider.audio()
        reading = asyncio.create_task(anext(reader))
        await asyncio.wait_for(session.started[0].wait(), 1)
        await asyncio.wait_for(asyncio.gather(provider.cancel(), provider.close()), 1)
        with self.assertRaises(asyncio.CancelledError):
            await reading
        self.assertTrue(session.closed[0].is_set())
        self.assertIsNone(provider._pending_task)
        self.assertEqual(len(session.calls), 1)

    async def test_reader_task_cancellation_closes_request_before_returning(self):
        session = ControlledSession(gates={0: asyncio.Event()})
        provider = await self.prepared(session)
        reader = provider.audio()
        reading = asyncio.create_task(anext(reader))
        await asyncio.wait_for(session.started[0].wait(), 1)
        reading.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await asyncio.wait_for(reading, 1)
        self.assertTrue(session.closed[0].is_set())
        self.assertIsNone(provider._pending_task)
        self.assertTrue(provider._close_task.done())

    async def test_close_or_cancel_while_yielded_releases_prefetch(self):
        for operation in ("aclose", "cancel"):
            with self.subTest(operation=operation):
                session = ControlledSession(gates={1: asyncio.Event()})
                provider = await self.prepared(session)
                reader = provider.audio()
                await asyncio.wait_for(anext(reader), 1)
                await asyncio.wait_for(session.started[1].wait(), 1)
                await asyncio.wait_for(reader.aclose() if operation == "aclose" else provider.cancel(), 1)
                await reader.aclose()
                self.assertTrue(session.closed[1].is_set())
                self.assertIsNone(provider._pending_task)
                self.assertEqual(len(session.calls), 2)
                self.assertEqual(session.active, 0)

    async def test_response_limit_is_cumulative_across_wav_requests(self):
        session = ControlledSession()
        # 每个响应为 4044 字节，两个响应合计会超出本轮预算。
        provider = await self.prepared(session, max_audio_bytes=6000)
        with self.assertLogs("transport.voice_websocket", level="WARNING"):
            with self.assertRaisesRegex(RuntimeError, "audio_too_large"):
                _ = [chunk async for chunk in provider.audio()]
        self.assertEqual(len(session.calls), 2)
        self.assertEqual(session.active, 0)
        self.assertIsNone(provider._pending_task)

    async def test_failed_lookahead_is_raised_after_current_segment_and_retrieved_on_close(self):
        for consume in (True, False):
            with self.subTest(consume=consume):
                session = ControlledSession(errors={1: aiohttp.ClientPayloadError("private-error")})
                provider = await self.prepared(session)
                reader = provider.audio()
                with self.assertLogs("transport.voice_websocket", level="WARNING"):
                    first = await anext(reader)
                    await asyncio.wait_for(session.closed[1].wait(), 1)
                pending = provider._pending_task
                self.assertTrue(pending.done())
                if consume:
                    chunks = [first]
                    with self.assertRaisesRegex(RuntimeError, "http_client_failed"):
                        async for chunk in reader:
                            chunks.append(chunk)
                    self.assertEqual(b"".join(chunks), session.responses[0].body[44:])
                else:
                    await reader.aclose()
                self.assertIsNone(provider._pending_task)
                self.assertFalse(pending._log_traceback)
                self.assertEqual(len(session.calls), 2)

    async def test_timeout_budget_does_not_reset_for_later_segments(self):
        gate = asyncio.Event()
        session = ControlledSession(gates={1: gate})
        provider = await self.prepared(session, timeout_s=0.1)
        reader = provider.audio()
        await anext(reader)
        await asyncio.wait_for(session.started[1].wait(), 1)
        self.assertLessEqual(session.timeouts[1], session.timeouts[0])
        self.assertGreater(session.timeouts[1], 0)
        with self.assertLogs("transport.voice_websocket", level="WARNING"):
            with self.assertRaisesRegex(RuntimeError, "timeout"):
                _ = [chunk async for chunk in reader]
        self.assertTrue(session.closed[1].is_set())
        self.assertIsNone(provider._pending_task)

    async def test_segment_timing_logs_contain_only_fixed_fields_and_counts(self):
        private = "private-tts-data-do-not-log"
        provider = await self.prepared(FakeSession(wav_bytes()), private,
                                       refer_wav_path=private, prompt_text=private)
        with self.assertLogs("transport.voice_websocket", level="INFO") as captured:
            _ = [chunk async for chunk in provider.audio()]
        self.assertEqual(len(captured.records), 1)
        self.assertRegex(captured.records[0].getMessage(),
                         r"^tts_segment adapter=gpt_sovits index=1 text_chars=27 request_ms=\d+\.\d "
                         r"audio_ms=10\.0 response_bytes_total=684 pcm_bytes_total=480$")
        self.assertNotIn(private, "\n".join(captured.output))

    def test_decode_rejects_pcm_expansion_before_resampling(self):
        with self.assertLogs("transport.voice_websocket", level="WARNING"):
            with self.assertRaisesRegex(RuntimeError, "audio_too_large"):
                _decode_wav(wav_bytes(rate=1, frames=2), 100000, max_pcm_bytes=1000)

    async def test_failure_diagnostics_distinguish_causes_without_private_data(self):
        private = "private-tts-data-do-not-log"
        connector = SimpleNamespace(host=private, port=9880, ssl=False)
        refused = aiohttp.ClientConnectorError(
            connector, ConnectionRefusedError(errno.ECONNREFUSED, private))
        cases = (
            ("connection_refused", FakeSession(error=refused), {}, 0),
            ("timeout", FakeSession(error=asyncio.TimeoutError(private)), {}, 0),
            ("connection_failed", FakeSession(error=aiohttp.ClientConnectionError(private)), {}, 0),
            ("http_client_failed", FakeSession(error=aiohttp.ClientPayloadError(private)), {}, 0),
            ("http_status", FakeSession(private.encode(), status=503), {}, 503),
            ("invalid_wav", FakeSession(private.encode()), {}, 0),
            ("unsupported_wav", FakeSession(wav_bytes(width=4)), {}, 0),
            ("truncated_wav", FakeSession(wav_bytes()[:-1]), {}, 0),
            ("audio_too_large", FakeSession(wav_bytes()), {"max_audio_bytes": 64}, 0),
        )
        for reason, session, overrides, status in cases:
            with self.subTest(reason=reason):
                provider = GptSoVitsTtsProvider({
                    "endpoint": f"http://{private}.invalid:9880",
                    "refer_wav_path": f"C:/private/{private}.wav",
                    "prompt_text": private,
                    **overrides,
                }, {}, session)
                await provider.start()
                await provider.append_text(private)
                await provider.finish()
                with self.assertLogs("transport.voice_websocket", level="WARNING") as captured:
                    with self.assertRaises(RuntimeError) as raised:
                        _ = [chunk async for chunk in provider.audio()]
                self.assertEqual(len(captured.records), 1)
                self.assertEqual(captured.records[0].getMessage(),
                                 f"tts_failure adapter=gpt_sovits tts_reason={reason} tts_http_status={status}")
                self.assertNotIn(private, "\n".join(captured.output))
                self.assertNotIn(private, str(raised.exception))
                self.assertIsNone(captured.records[0].exc_info)

    async def test_selected_mambo_profile_synthesizes_through_registry(self):
        config = load_voice_config(EXAMPLE_CONFIG_PATH, environ={})
        selection = select_profile(config, profile="mambo_local", mode="realtime")
        session = FakeSession(wav_bytes(rate=32000, frames=320))
        provider = create_realtime_provider("tts", selection.tts, {}, session)

        await provider.start()
        await provider.append_text("你好，曼波！")
        await provider.finish()
        pcm = b"".join([chunk async for chunk in provider.audio()])

        self.assertEqual(session.endpoint, "http://127.0.0.1:9880/")
        self.assertEqual(session.payload["text"], "你好，曼波！")
        self.assertEqual(session.payload["refer_wav_path"], selection.tts.options["refer_wav_path"])
        self.assertEqual(session.payload["prompt_text"], selection.tts.options["prompt_text"])
        self.assertEqual(session.payload["prompt_language"], "zh")
        self.assertEqual((provider.codec, provider.sample_rate_hz, provider.channels),
                         ("pcm_s16le", 24000, 1))
        # 32 kHz 源音频的十毫秒应转换为 240 个 PCM16 样本。
        self.assertEqual(len(pcm), 480)
        self.assertEqual(pcm[:6], b"\x18\xfc\x4d\x01\x4d\x01")

    async def test_media_emits_segment_sample_ranges_mapped_to_response_text(self):
        text = "甲" * 60
        session = FakeSession(wav_bytes(rate=32000, frames=320))
        provider = await self.prepared(session, text)

        items = [item async for item in provider.media()]
        cues = [item for item in items if isinstance(item, SubtitleCue)]
        pcm = b"".join(item for item in items if isinstance(item, bytes))

        self.assertTrue(provider.subtitle_enabled)
        self.assertEqual([cue.text for cue in cues], ["甲" * 40, "甲" * 20])
        self.assertEqual([(cue.words[0].start_sample, cue.words[0].end_sample)
                          for cue in cues], [(0, 240), (240, 480)])
        self.assertEqual(len(pcm), 960)

        assembler = ResponseSubtitleAssembler(text)
        mapped = [word for cue in cues for batch in assembler.feed(cue)
                  for word in batch["words"]]
        self.assertIsNone(assembler.failed_reason)
        self.assertEqual([word["text_byte_offset"] for word in mapped], [0, 120])
        self.assertEqual([word["text"] for word in mapped], ["甲" * 40, "甲" * 20])

    async def test_buffers_text_and_resamples_wav(self):
        session = FakeSession(wav_bytes(rate=32000, frames=320))
        provider = GptSoVitsTtsProvider({
            "endpoint": "http://127.0.0.1:9880",
            "refer_wav_path": r"C:\models\refer.wav",
            "prompt_text": "参考文本",
        }, {}, session)
        await provider.start()
        await provider.append_text("你好")
        await provider.append_text("，曼波！")
        await provider.finish()
        chunks = [chunk async for chunk in provider.audio()]
        self.assertEqual(len(b"".join(chunks)), 480)
        self.assertEqual(session.endpoint, "http://127.0.0.1:9880/")
        self.assertEqual(session.payload["text"], "你好，曼波！")
        self.assertEqual(session.payload["refer_wav_path"], r"C:\models\refer.wav")
        self.assertEqual(session.payload["text_language"], "zh")

    async def test_cancel_before_audio_avoids_request(self):
        session = FakeSession(wav_bytes())
        provider = GptSoVitsTtsProvider({"endpoint": "http://127.0.0.1:9880"}, {}, session)
        await provider.start()
        await provider.append_text("不会发送")
        reader = provider.audio()
        task = asyncio.create_task(reader.__anext__())
        await asyncio.sleep(0)
        await provider.cancel()
        with self.assertRaises(StopAsyncIteration):
            await task
        self.assertIsNone(getattr(session, "payload", None))

    def test_stereo_wav_is_mixed_to_mono(self):
        pcm = _decode_wav(wav_bytes(rate=24000, channels=2, frames=20), 100000)
        self.assertEqual(len(pcm), 40)


if __name__ == "__main__":
    unittest.main()
