import asyncio
import gzip
import json
import struct
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from providers.tts.base import SubtitleCue, SubtitleUnavailable, SubtitleWord

from providers.tts.volcengine import (
    MAX_BUFFERED_PCM_BYTES, MAX_PAYLOAD_BYTES, VolcengineTtsProvider,
    decode_response, encode_request,
)


def response(event, scope="server-connection", payload=None, *, audio=False, compressed=False):
    body = payload if audio else json.dumps({} if payload is None else payload).encode()
    if compressed:
        body = gzip.compress(body)
    head = bytes((0x11, 0xB4 if audio else 0x94, (0 if audio else 0x10) | int(compressed), 0))
    encoded_id = scope.encode()
    return (head + struct.pack(">II", event, len(encoded_id)) + encoded_id
            + struct.pack(">I", len(body)) + body)


def request_fields(packet):
    event = struct.unpack_from(">I", packet, 4)[0]
    position = 8
    scope = None
    if event >= 100:
        length = struct.unpack_from(">I", packet, position)[0]
        position += 4
        scope = packet[position:position + length].decode()
        position += length
    length = struct.unpack_from(">I", packet, position)[0]
    return event, scope, json.loads(packet[position + 4:position + 4 + length])


class FakeWebSocket:
    def __init__(self, *, reject_session=False, wrong_session=False, finish_audio=True):
        self.queue = asyncio.Queue()
        self.sent = []
        self.closed = False
        self.reject_session = reject_session
        self.wrong_session = wrong_session
        self.finish_audio = finish_audio
        self.session_id = None

    def push(self, packet):
        self.queue.put_nowait(SimpleNamespace(type=2, data=packet))

    async def send_bytes(self, packet):
        self.sent.append(packet)
        event, scope, _ = request_fields(packet)
        if event == 1:
            self.push(response(50))
        elif event == 100:
            self.session_id = scope
            ack_scope = "wrong-session" if self.wrong_session else scope
            self.push(response(153 if self.reject_session else 150, ack_scope))
        elif event == 102 and self.finish_audio:
            self.push(response(350, scope))
            self.push(response(352, scope, b"\x01\x00" * 480, audio=True))
            self.push(response(351, scope))
            self.push(response(152, scope, {"status_code": 20000000}))
        elif event == 101:
            self.push(response(151, scope))
        elif event == 2:
            self.push(response(52))

    async def receive(self):
        item = await self.queue.get()
        if isinstance(item, Exception):
            raise item
        return item

    async def close(self):
        self.closed = True
        self.queue.put_nowait(SimpleNamespace(type=8, data=None))


class FakeSession:
    def __init__(self, socket=None):
        self.socket = socket or FakeWebSocket()
        self.calls = []

    async def ws_connect(self, endpoint, **kwargs):
        self.calls.append((endpoint, kwargs))
        return self.socket


class VolcProtocolTests(unittest.TestCase):
    def test_local_subtitle_diagnostics_do_not_change_identity_or_repr(self):
        original = SubtitleCue("private-subtitle-body", (
            SubtitleWord("private-subtitle-body", 0, 480),), 398)
        tagged = SubtitleCue(original.text, original.words, original.upstream_event,
                             received_at=123.456,
                             source_id="8313959b-ea47-4d18-921b-83edb99afac4")
        self.assertEqual(tagged, original)
        self.assertEqual(tagged.fingerprint(), original.fingerprint())
        self.assertNotIn("received_at", repr(tagged))
        self.assertNotIn("8313959b", repr(tagged))

    def test_control_request_bytes_match_documented_layout(self):
        self.assertEqual(encode_request(1), bytes.fromhex("11 14 10 00 00 00 00 01 00 00 00 02 7b 7d"))
        event, scope, payload = request_fields(encode_request(102, session_id="abc"))
        self.assertEqual((event, scope, payload), (102, "abc", {}))

    def test_response_connection_session_and_raw_pcm_envelopes(self):
        self.assertEqual(decode_response(response(50)).scope_id, "server-connection")
        audio = decode_response(response(352, "session", b"\x04\x00" * 20, audio=True))
        self.assertEqual((audio.message_type, audio.event, audio.scope_id), (11, 352, "session"))
        self.assertEqual(audio.payload, b"\x04\x00" * 20)

    def test_gzip_and_malformed_frames_are_bounded(self):
        audio = decode_response(response(352, "session", b"\x00\x00" * 50, audio=True, compressed=True))
        self.assertEqual(len(audio.payload), 100)
        for packet in (b"x", response(150) + b"x", response(150)[:-2], b"\x11\x90\x10\x00" + b"\x00" * 8,
                       response(352, "s", b"a" * (MAX_PAYLOAD_BYTES + 1), audio=True, compressed=True)):
            with self.subTest(size=len(packet)):
                with self.assertRaises(RuntimeError):
                    decode_response(packet)

    def test_error_frame_discards_upstream_error_body(self):
        body = b'{"message":"secret-token-must-not-escape"}'
        packet = b"\x11\xf0\x10\x00" + struct.pack(">II", 45000001, len(body)) + body
        result = decode_response(packet)
        self.assertEqual(result.error_code, 45000001)
        self.assertEqual(result.payload, {})
        self.assertNotIn("secret-token", repr(result))


class VolcengineTtsTests(unittest.IsolatedAsyncioTestCase):
    def provider(self, socket=None, **overrides):
        session = FakeSession(socket)
        options = {"voice": "test-voice", "resource_id": "seed-tts-2.0", "timeout_s": 1, **overrides}
        provider = VolcengineTtsProvider(options, {"VOLCENGINE_TTS_API_KEY": "mock-key", "APP": "mock-app"}, session)
        self.addAsyncCleanup(provider.close)
        return provider, session

    async def test_start_append_finish_and_pcm_audio(self):
        provider, session = self.provider()
        await provider.start()
        await provider.append_text("Hello")
        await provider.finish()
        await provider.finish()
        chunks = [chunk async for chunk in provider.audio()]
        self.assertEqual(chunks, [b"\x01\x00" * 480])
        endpoint, kwargs = session.calls[0]
        self.assertEqual(endpoint, "wss://openspeech.bytedance.com/api/v3/tts/bidirection")
        self.assertEqual(kwargs["headers"]["X-Api-Key"], "mock-key")
        self.assertNotIn("X-Api-App-Id", kwargs["headers"])
        fields = [request_fields(packet) for packet in session.socket.sent]
        self.assertEqual([field[0] for field in fields], [1, 100, 200, 102])
        self.assertEqual(fields[1][2]["req_params"]["audio_params"],
                         {"format": "pcm", "sample_rate": 24000, "enable_subtitle": True})
        self.assertEqual(fields[2][2]["req_params"]["text"], "Hello")
        await provider.close()
        await provider.close()
        self.assertTrue(session.socket.closed)
        self.assertEqual(request_fields(session.socket.sent[-1])[0], 2)

    async def test_subtitles_can_be_disabled_and_do_not_enable_legacy_timestamp(self):
        for overrides in ({"enable_subtitle": False}, {"resource_id": "seed-tts-1.0"}):
            with self.subTest(overrides=overrides):
                provider, session = self.provider(**overrides)
                await provider.start()
                params = request_fields(session.socket.sent[1])[2]["req_params"]["audio_params"]
                self.assertEqual(params, {"format": "pcm", "sample_rate": 24000})
                self.assertFalse(provider.subtitle_enabled)

    async def test_subtitle_timing_records_receipt_gap_and_buffer_without_text_or_credentials(self):
        provider, _ = self.provider()
        await provider._enqueue_audio(bytes(960))
        loop_clock = SimpleNamespace(time=iter((10.0, 10.25)).__next__)
        with self.assertLogs("transport.voice_websocket.tts", level="INFO") as logs:
            with patch("providers.tts.volcengine.asyncio.get_running_loop", return_value=loop_clock):
                provider._enqueue_subtitle({"text": "private-first", "words": [
                    {"word": "private-first", "startTime": 0, "endTime": 0.02}]}, 398)
                provider._enqueue_subtitle({"text": "private-second", "words": [
                    {"word": "private-second", "startTime": 0.02, "endTime": 0.04}]}, 398)
        lines = " ".join(logs.output)
        self.assertIn("source=" + provider.session_id, lines)
        self.assertIn("cue=1 words=1 received_pcm_samples=480 buffered_pcm_ms=20.0 rx_gap_ms=-1.0", lines)
        self.assertIn("cue=2 words=1 received_pcm_samples=480 buffered_pcm_ms=20.0 rx_gap_ms=250.0", lines)
        self.assertIn("audio_enqueue_wait_ms=0.0 audio_enqueue_wait_max_ms=0.0", lines)
        self.assertNotIn("private-", lines)
        self.assertNotIn("mock-key", lines)
        self.assertNotIn("mock-app", lines)
        cues = [item for item in provider._chunks if isinstance(item, SubtitleCue)]
        self.assertEqual([item.received_at for item in cues], [10.0, 10.25])
        self.assertTrue(all(item.source_id == provider.session_id for item in cues))

    async def test_audio_backpressure_timing_measures_shared_reader_wait(self):
        provider, _ = self.provider()
        await provider._enqueue_audio(bytes(MAX_BUFFERED_PCM_BYTES))
        wait_entered = asyncio.Event()

        class ObservedSpaceAvailable(asyncio.Event):
            async def wait(self):
                wait_entered.set()
                return await super().wait()

        provider._space_available = ObservedSpaceAvailable()
        clock = SimpleNamespace(now=10.0)
        clock.time = lambda: clock.now
        with self.assertLogs("transport.voice_websocket.tts", level="INFO") as logs:
            # 只替换适配器拿到的诊断时钟，调度和 Event 仍使用真实事件循环。
            with patch("providers.tts.volcengine.asyncio.get_running_loop", return_value=clock):
                pending = asyncio.create_task(provider._enqueue_audio(bytes(960)))
                try:
                    await asyncio.wait_for(wait_entered.wait(), 1)
                    self.assertFalse(pending.done())
                    # 确认生产者已因队列满阻塞，再推进时钟并让真实消费腾出容量。
                    clock.now = 10.125
                    media = provider.media()
                    try:
                        self.assertEqual(len(await anext(media)), 960)
                        await asyncio.wait_for(pending, 1)
                    finally:
                        await media.aclose()
                finally:
                    pending.cancel()
                    await asyncio.gather(pending, return_exceptions=True)
        self.assertGreaterEqual(provider._audio_enqueue_wait_ms, 20)
        self.assertEqual(provider._audio_enqueue_wait_ms, 125.0)
        self.assertEqual(provider._audio_enqueue_wait_max_ms, provider._audio_enqueue_wait_ms)
        self.assertEqual(provider._received_pcm_samples, MAX_BUFFERED_PCM_BYTES // 2 + 480)
        line = " ".join(logs.output)
        self.assertIn("tts_audio_backpressure source=" + provider.session_id, line)
        self.assertIn("wait_ms=125.0 wait_max_ms=125.0", line)
        self.assertIn("received_pcm_samples=96480", line)
        self.assertNotIn("mock-key", line)

    async def test_media_preserves_late_subtitle_and_session_relative_sample_clock(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        payload = {"text": "2019年。", "words": [
            {"word": "2019", "startTime": 0.585, "endTime": 0.615, "confidence": 0.1},
            {"word": "年。", "startTime": 6.2150416, "endTime": 6.3550415, "confidence": 0.8},
        ]}
        # 398 是人为构造的下行 TTS 事件，不表示
        # 上游未公开的 TTSSubtitle 事件数值就是这个编号。
        session.socket.push(response(352, provider.session_id, b"\x01\x00" * 480, audio=True))
        session.socket.push(response(398, provider.session_id, payload))
        await provider.finish()
        session.socket.push(response(152, provider.session_id))
        items = [item async for item in provider.media()]
        self.assertEqual(items[0], SubtitleCue("2019年。", (
            SubtitleWord("2019", 14040, 14760), SubtitleWord("年。", 149161, 152521)), 398))
        self.assertEqual(items[1], b"\x01\x00" * 480)
        self.assertEqual(items[0].end_sample, 152521)
        with self.assertRaisesRegex(RuntimeError, "one consumer"):
            await anext(provider.audio())

    async def test_subtitle_can_overtake_remaining_audio_of_a_large_chunk(self):
        provider, _ = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        pcm = b"\x01\x00" * (24000 * 4)
        await provider._enqueue_audio(pcm)
        media = provider.media()
        self.addAsyncCleanup(media.aclose)
        first = await anext(media)
        # 传输层按节奏消费每个输出缓冲。若在此一次持有完整的四秒
        # 音频块，随后到达的字幕就会在这段播放期间不可见。
        self.assertLessEqual(len(first), 960)
        self.assertEqual(provider._buffered, len(pcm) - len(first))
        provider._enqueue_subtitle({"text": "你好", "words": [
            {"word": "你", "startTime": 0.01, "endTime": 0.1}]}, 398)
        self.assertIsInstance(await anext(media), SubtitleCue)
        await provider.finish()
        provider._end()
        remainder = b"".join([item async for item in media])
        self.assertEqual(first + remainder, pcm)
        self.assertEqual(provider._buffered, 0)

    async def test_cancel_discards_remainder_of_partially_consumed_pcm_chunk(self):
        provider, _ = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        await provider._enqueue_audio(b"\x01\x00" * (24000 * 4))
        media = provider.media()
        self.addAsyncCleanup(media.aclose)
        self.assertEqual(len(await anext(media)), 960)
        await provider.cancel()
        self.assertEqual([item async for item in media], [])
        self.assertEqual(provider._buffered, 0)

    async def test_duplicate_cue_is_ignored_but_cumulative_text_is_not_truncated(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        first = {"text": "你好", "words": [{"word": "你", "startTime": 0.1, "endTime": 0.2}]}
        second = {"text": "你好", "words": first["words"] + [
            {"word": "好", "startTime": 0.2, "endTime": 0.3}]}
        for payload in (first, first, second):
            session.socket.push(response(398, provider.session_id, payload))
        await provider.finish()
        session.socket.push(response(152, provider.session_id))
        items = [item async for item in provider.media()]
        self.assertEqual([len(cue.words) for cue in items], [1, 2])
        self.assertEqual([cue.text for cue in items], ["你好", "你好"])

    async def test_invalid_optional_subtitle_does_not_break_audio_or_expose_text(self):
        for bad_time in (float("nan"), float("inf"), -1, True, "0.1"):
            with self.subTest(bad_time=bad_time):
                provider, session = self.provider(FakeWebSocket(finish_audio=False))
                await provider.start()
                session.socket.push(response(398, provider.session_id, {"text": "secret-text", "words": [
                    {"word": "secret-text", "startTime": bad_time, "endTime": 1}]}))
                session.socket.push(response(352, provider.session_id, b"\x00\x00", audio=True))
                await provider.finish()
                session.socket.push(response(152, provider.session_id))
                with self.assertLogs("providers.tts.volcengine", level="WARNING") as logs:
                    self.assertEqual([item async for item in provider.media()],
                                     [SubtitleUnavailable(detail="invalid_timing", upstream_event=398,
                                                          upstream_events=1), b"\x00\x00"])
                self.assertNotIn("secret-text", " ".join(logs.output))

    async def test_invalid_subtitle_after_valid_cue_reports_one_failure_and_keeps_audio(self):
        provider, _ = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        provider._enqueue_subtitle({"text": "好", "words": [
            {"word": "好", "startTime": 0, "endTime": 0.1}]}, 398)
        media = provider.media()
        self.addAsyncCleanup(media.aclose)
        self.assertIsInstance(await anext(media), SubtitleCue)
        with self.assertLogs("providers.tts.volcengine", level="WARNING"):
            provider._enqueue_subtitle({"text": "好", "words": [
                {"word": "好", "startTime": False, "endTime": 0.1}]}, 398)
        await provider._enqueue_audio(b"\x01\x00" * 480)
        provider._end()
        self.assertEqual([item async for item in media],
                         [SubtitleUnavailable(detail="invalid_timing", upstream_event=398,
                                              upstream_events=2), b"\x01\x00" * 480])

    async def test_empty_word_control_events_do_not_disable_later_subtitles(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        # 边界事件可以完全省略文字。空对齐结果不代表时间线损坏，
        # 异步字幕事件自身返回空结果时也一样。
        boundary_payloads = ({"words": [], "phonemes": []}, {"words": None},
                             {"text": None, "words": [{"not_subtitle_word": True}]},
                             {"words": "not_a_subtitle_array"})
        for event, payload in [(event, payload) for event in (350, 351)
                               for payload in boundary_payloads] + [
                                   (398, {"text": "你好", "words": []})]:
            session.socket.push(response(event, provider.session_id, payload))
        session.socket.push(response(398, provider.session_id, {"text": "你好", "words": [
            {"word": "你", "startTime": 0, "endTime": 0.1},
            {"word": "好", "startTime": 0.1, "endTime": 0.2}]}))
        session.socket.push(response(352, provider.session_id, b"\x01\x00" * 480, audio=True))
        await provider.finish()
        session.socket.push(response(152, provider.session_id))
        items = [item async for item in provider.media()]
        self.assertFalse(provider._subtitle_failed)
        self.assertEqual(provider._subtitle_events, 10)
        self.assertEqual(items, [SubtitleCue("你好", (
            SubtitleWord("你", 0, 2400), SubtitleWord("好", 2400, 4800)), 398), b"\x01\x00" * 480])

    async def test_audio_only_consumer_discards_subtitles_and_cancel_clears_both(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        payload = {"text": "好", "words": [{"word": "好", "startTime": 0, "endTime": 0.1}]}
        session.socket.push(response(398, provider.session_id, payload))
        session.socket.push(response(352, provider.session_id, b"\x00\x00", audio=True))
        await provider.finish()
        session.socket.push(response(152, provider.session_id))
        self.assertEqual([item async for item in provider.audio()], [b"\x00\x00"])
        other, other_session = self.provider(FakeWebSocket(finish_audio=False))
        await other.start()
        other._enqueue_subtitle(payload, 398)
        await other.cancel()
        self.assertEqual([item async for item in other.media()], [])

    async def test_optional_legacy_auth_and_unique_session_ids(self):
        first, session = self.provider(app_id_ref="APP")
        second, _ = self.provider()
        self.assertNotEqual(first.session_id, second.session_id)
        await first.start()
        headers = session.calls[0][1]["headers"]
        self.assertEqual(headers["X-Api-App-Id"], "mock-app")
        self.assertEqual(headers["X-Api-Access-Key"], "mock-key")
        self.assertNotIn("X-Api-Key", headers)

    async def test_cancel_discards_queued_audio_without_waiting_remote_ack(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        session.socket.push(response(352, provider.session_id, b"\x00\x00" * 100, audio=True))
        await asyncio.sleep(0)
        await provider.cancel()
        await provider.cancel()
        self.assertEqual([chunk async for chunk in provider.audio()], [])
        self.assertIn(101, [request_fields(p)[0] for p in session.socket.sent])
        self.assertTrue(session.socket.closed)
        with self.assertRaises(RuntimeError):
            await provider.append_text("late")

    async def test_wrong_session_and_rejected_session_close_socket(self):
        for kwargs in ({"wrong_session": True}, {"reject_session": True}):
            with self.subTest(kwargs=kwargs):
                provider, session = self.provider(FakeWebSocket(**kwargs))
                with self.assertRaisesRegex(RuntimeError, "could not start"):
                    await provider.start()
                self.assertTrue(session.socket.closed)

    async def test_transport_failure_never_exposes_exception_or_secret(self):
        provider, session = self.provider()
        await provider.start()
        session.socket.queue.put_nowait(RuntimeError("secret-token-should-not-escape"))
        with self.assertRaises(RuntimeError) as error:
            async for _ in provider.audio():
                pass
        self.assertEqual(str(error.exception), "Volcengine TTS receive failed")

    async def test_odd_pcm_and_oversized_chunk_fail_without_playing_partial_audio(self):
        for chunk in (b"x", b"\x00" * (MAX_BUFFERED_PCM_BYTES + 2)):
            with self.subTest(length=len(chunk)):
                provider, session = self.provider()
                await provider.start()
                session.socket.push(response(352, provider.session_id, chunk, audio=True))
                with self.assertRaises(RuntimeError):
                    async for _ in provider.audio():
                        self.fail("Invalid audio must not be emitted")

    async def test_full_audio_queue_backpressures_until_consumer_drains(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        first = b"\x01\x00" * (MAX_BUFFERED_PCM_BYTES // 2)
        second = b"\x02\x00" * 480
        session.socket.push(response(352, provider.session_id, first, audio=True))
        session.socket.push(response(352, provider.session_id, second, audio=True))
        await provider.finish()
        session.socket.push(response(152, provider.session_id))
        # 等待读取任务取出第二个音频块，但仍无法将其入队。
        # 此时队列是已满而非失败，且大小仍受到限制。
        async with asyncio.timeout(1):
            while provider._buffered != MAX_BUFFERED_PCM_BYTES or session.socket.queue.qsize() != 1:
                await asyncio.sleep(0)
        self.assertIsNone(provider._failure)
        self.assertFalse(provider._done)
        self.assertEqual(b"".join([chunk async for chunk in provider.audio()]), first + second)
        self.assertLessEqual(provider._buffered, MAX_BUFFERED_PCM_BYTES)

    async def test_known_backpressure_delays_unread_subtitle_behind_second_full_pcm_chunk(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        full = bytes(MAX_BUFFERED_PCM_BYTES)
        session.socket.push(response(352, provider.session_id, full, audio=True))
        session.socket.push(response(352, provider.session_id, full, audio=True))
        session.socket.push(response(398, provider.session_id, {"text": "word", "words": [
            {"word": "word", "startTime": 0, "endTime": 0.02}]}))
        async with asyncio.timeout(1):
            while provider._buffered != MAX_BUFFERED_PCM_BYTES or session.socket.queue.qsize() != 1:
                await asyncio.sleep(0)
        # 这里记录当前共享读取任务的限制，不表示已经修复：
        # 仍在连接中尚未读取的字幕不能越过一个正在等待入队的完整 PCM 块。
        # 将来若调整缓冲策略，需要相应更新此断言。
        self.assertFalse(any(isinstance(item, SubtitleCue) for item in provider._chunks))
        consumed_samples = 0
        media = provider.media()
        try:
            async with asyncio.timeout(1):
                while consumed_samples < MAX_BUFFERED_PCM_BYTES // 2:
                    item = await anext(media)
                    self.assertIsInstance(item, bytes)
                    consumed_samples += len(item) // 2
                    await asyncio.sleep(0)
                self.assertEqual(consumed_samples, 96000)
                # 只有第一个完整音频块全部消费后，待入队的完整块才有足够空间；
                # 随后才让读取任务接收下一个事件。
                while not any(isinstance(item, SubtitleCue) for item in provider._chunks):
                    await asyncio.sleep(0)
                self.assertIsInstance(await anext(media), SubtitleCue)
        finally:
            await media.aclose()

    async def test_cancel_unblocks_full_audio_queue_and_discards_pending_chunk(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False))
        await provider.start()
        full = b"\x00\x00" * (MAX_BUFFERED_PCM_BYTES // 2)
        session.socket.push(response(352, provider.session_id, full, audio=True))
        session.socket.push(response(352, provider.session_id, b"\x02\x00", audio=True))
        async with asyncio.timeout(1):
            while provider._buffered != MAX_BUFFERED_PCM_BYTES or not session.socket.queue.empty():
                await asyncio.sleep(0)
        await asyncio.wait_for(provider.cancel(), 0.5)
        self.assertEqual([chunk async for chunk in provider.audio()], [])
        self.assertTrue(provider._reader_task.done())
        self.assertEqual(provider._buffered, 0)

    async def test_full_audio_queue_stall_has_bounded_timeout(self):
        provider, session = self.provider(FakeWebSocket(finish_audio=False), timeout_s=0.05)
        await provider.start()
        full = b"\x00\x00" * (MAX_BUFFERED_PCM_BYTES // 2)
        session.socket.push(response(352, provider.session_id, full, audio=True))
        session.socket.push(response(352, provider.session_id, b"\x02\x00", audio=True))
        await asyncio.wait_for(provider._reader_task, 1)
        with self.assertRaisesRegex(RuntimeError, "consumer stalled"):
            _ = [chunk async for chunk in provider.audio()]
        self.assertEqual(provider._buffered, 0)

    async def test_timeout_unblocks_audio_consumer(self):
        provider, _ = self.provider(timeout_s=0.2)
        await provider.start()
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            await asyncio.wait_for(anext(provider.audio()), 1)

    async def test_requires_voice_and_selected_credentials_before_network(self):
        for options, credentials in (({}, {"VOLCENGINE_TTS_API_KEY": "key"}), ({"voice": "test"}, {})):
            session = FakeSession()
            provider = VolcengineTtsProvider(options, credentials, session)
            with self.assertRaises(RuntimeError):
                await provider.start()
            self.assertEqual(session.calls, [])

    async def test_real_aiohttp_socket_against_local_protocol_fixture(self):
        try:
            import aiohttp
            from aiohttp import web
        except ImportError:
            self.skipTest("aiohttp is installed in the service virtual environment")
        events = []

        async def fixture(request):
            self.assertEqual(request.headers["X-Api-Key"], "local-mock-only")
            socket = web.WebSocketResponse()
            await socket.prepare(request)
            async for message in socket:
                event, scope, payload = request_fields(message.data)
                events.append(event)
                if event == 1:
                    await socket.send_bytes(response(50))
                elif event == 100:
                    self.assertEqual(payload["req_params"]["audio_params"]["format"], "pcm")
                    await socket.send_bytes(response(150, scope))
                elif event == 102:
                    await socket.send_bytes(response(352, scope, b"\x12\x00" * 480, audio=True))
                    await socket.send_bytes(response(152, scope))
                elif event == 2:
                    await socket.send_bytes(response(52))
                    break
            await socket.close()
            return socket

        app = web.Application()
        app.router.add_get("/tts", fixture)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        async with aiohttp.ClientSession() as session:
            provider = VolcengineTtsProvider(
                {"endpoint": f"ws://127.0.0.1:{port}/tts", "voice": "mock-voice", "timeout_s": 2},
                {"VOLCENGINE_TTS_API_KEY": "local-mock-only"}, session,
            )
            try:
                await provider.start()
                await provider.append_text("Local fixture")
                await provider.finish()
                self.assertEqual([part async for part in provider.audio()], [b"\x12\x00" * 480])
            finally:
                await provider.close()
        self.assertEqual(events, [1, 100, 200, 102, 2])


if __name__ == "__main__":
    unittest.main()
