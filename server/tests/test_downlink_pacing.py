"""让音频发送贴近实际播放，避免控制消息受 TCP 队头阻塞影响。"""

import asyncio
import math
import struct
import unittest
from contextlib import suppress
from io import BytesIO
from types import SimpleNamespace
from unittest.mock import patch
import wave

from audio.opus_codec import CodecError, Decoder, Encoder, native_version
from providers.tts.base import SubtitleCue, SubtitleUnavailable, SubtitleWord
from providers.tts.gpt_sovits import GptSoVitsTtsProvider
from providers.tts.volcengine import VolcengineTtsProvider
from transport.voice_protocol import AudioPacket
from transport.voice_websocket import (
    DOWNLINK_BURST_SECONDS,
    DOWNLINK_FRAME_SECONDS,
    DeviceConnection,
    _DownlinkPacer,
    _subtitle_timing_metrics,
)
from tests.test_volcengine_tts import FakeSession, FakeWebSocket, response


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def clock(self):
        return self.now

    async def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class PausedClock(FakeClock):
    def __init__(self):
        super().__init__()
        self.sleeping = asyncio.Event()
        self.release = asyncio.Event()

    async def sleep(self, seconds):
        self.sleeping.set()
        await self.release.wait()
        await super().sleep(seconds)


class DownlinkPacingTests(unittest.IsolatedAsyncioTestCase):
    def test_subtitle_timing_uses_sent_content_not_received_pcm_or_padding(self):
        cue = SubtitleCue("private-body", (SubtitleWord("private-body", 240, 720),), 398,
                          received_at=10.125,
                          source_id="8313959b-ea47-4d18-921b-83edb99afac4")
        metrics = _subtitle_timing_metrics(
            cue, now=10.5, stream_started_at=10.0, previous_cue_at=10.25,
            sent_frames=2, pre_skip=312, source_samples=24000)
        self.assertEqual(metrics["source"], cue.source_id)
        self.assertEqual(metrics["elapsed_ms"], 500)
        self.assertEqual(metrics["gap_ms"], 250)
        self.assertEqual(metrics["provider_queue_ms"], 375)
        self.assertEqual(metrics["sent_content_samples"], 648)
        self.assertEqual(metrics["start_lead_ms"], -17)
        self.assertEqual(metrics["end_lead_ms"], 3)
        for frames, pre_skip, source_samples, expected in ((1, 0, 77, 77), (0, 312, 77, 0)):
            with self.subTest(frames=frames, pre_skip=pre_skip, source_samples=source_samples):
                result = _subtitle_timing_metrics(
                    cue, now=10.5, stream_started_at=10.0, previous_cue_at=None,
                    sent_frames=frames, pre_skip=pre_skip, source_samples=source_samples)
                self.assertEqual(result["sent_content_samples"], expected)
                self.assertEqual(result["gap_ms"], -1)

    def test_subtitle_timing_rejects_untrusted_source_and_invalid_local_timestamp(self):
        for received_at in (None, True, float("nan"), float("inf"), -1.0, 11.0, 10 ** 1000):
            with self.subTest(received_at=received_at):
                cue = SubtitleCue("private-body", (SubtitleWord("private-body", 0, 480),), 398,
                                  received_at=received_at, source_id="secret-key\nforged-log")
                result = _subtitle_timing_metrics(
                    cue, now=10.5, stream_started_at=10.0, previous_cue_at=None,
                    sent_frames=0, pre_skip=0, source_samples=0)
                self.assertEqual(result["source"], "none")
                self.assertEqual(result["provider_queue_ms"], -1)

    def test_cumulative_subtitle_timing_only_measures_new_batch_words(self):
        cumulative = SubtitleCue("old new", (
            SubtitleWord("old", 0, 480), SubtitleWord("new", 24000, 24480)), 398)
        fresh_words = [{"start_sample": 24000, "end_sample": 24480}]
        result = _subtitle_timing_metrics(
            cumulative, now=10.5, stream_started_at=10.0, previous_cue_at=None,
            sent_frames=25, pre_skip=0, source_samples=48000, fresh_words=fresh_words)
        self.assertEqual(result["first_start_sample"], 24000)
        self.assertEqual(result["end_sample"], 24480)
        self.assertEqual(result["start_lead_ms"], 500)
        self.assertIsNone(_subtitle_timing_metrics(
            cumulative, now=10.5, stream_started_at=10.0, previous_cue_at=None,
            sent_frames=25, pre_skip=0, source_samples=48000, fresh_words=[]))

    async def test_subtitle_diagnostics_are_per_cue_and_never_sent_as_protocol_fields(self):
        identity = {"session_epoch": 19, "turn_id": 1, "generation": 1}
        connection = self.connection("pcm_s16le", identity)
        source = "8313959b-ea47-4d18-921b-83edb99afac4"

        async def chunks():
            # 传输层已收到这个不足一帧的数据块，但记录第一条字幕时，
            # 它还不属于成功发出的音频包。
            yield bytes(77 * 2)
            yield SubtitleCue("private-first", (SubtitleWord("private-first", 240, 480),), 398,
                              received_at=asyncio.get_running_loop().time(), source_id=source)
            yield bytes(403 * 2)
            yield SubtitleCue("private-second", (SubtitleWord("private-second", 480, 960),), 398,
                              received_at=asyncio.get_running_loop().time(), source_id="bad\nsource")

        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            await connection.send_audio(chunks(), identity, subtitle_enabled=True)
        timing = [line for line in logs.output if "subtitle_timing " in line]
        self.assertEqual(len(timing), 2)
        self.assertIn("source=" + source + " cue=1 batches=1 words=1 new_words=1", timing[0])
        self.assertIn("sent_content_samples=0 start_lead_ms=10.0 end_lead_ms=20.0", timing[0])
        self.assertIn("source=none cue=2 batches=1 words=1 new_words=1", timing[1])
        self.assertIn("sent_content_samples=480 start_lead_ms=0.0 end_lead_ms=20.0", timing[1])
        self.assertNotIn("private-", " ".join(logs.output))
        self.assertNotIn("bad", " ".join(logs.output))
        subtitle_events = [event for event in connection.ws.json if event["type"] == "audio.subtitle"]
        self.assertEqual(len(subtitle_events), 2)
        allowed = {"type", "session_epoch", "turn_id", "generation", "stream_id", "sequence",
                   "text_offset", "text", "words", "covered_sample"}
        self.assertTrue(all(set(event) == allowed for event in subtitle_events))
        self.assertTrue(all("received_at" not in word and "source_id" not in word
                            for event in subtitle_events for word in event["words"]))

    async def test_fixed_response_text_uses_timing_only_batches_and_retains_full_answer_on_mismatch(self):
        identity = {"session_epoch": 19, "turn_id": 1, "generation": 1}
        for mismatch in (False, True):
            with self.subTest(mismatch=mismatch):
                connection = self.connection("pcm_s16le", identity)
                connection.pipeline.committed_answer = "**你好**，下一句。"

                async def chunks():
                    yield SubtitleCue("你好", (SubtitleWord("你好", 0, 480),), 398)
                    yield bytes(480 * 2)
                    yield SubtitleCue("不同文字" if mismatch else "下一句。",
                                      (SubtitleWord("不同文字" if mismatch else "下一句", 480, 960),), 398)
                    yield bytes(480 * 2)

                await connection.send_audio(chunks(), identity, subtitle_enabled=True)
                events = connection.ws.json
                self.assertEqual(events[0]["subtitle_mode"], "response_word_timestamps")
                batches = [item for item in events if item["type"] == "audio.subtitle"]
                self.assertEqual(len(batches), 1 if mismatch else 2)
                self.assertTrue(all(item["text"] == "" and item["text_offset"] == 0 for item in batches))
                self.assertEqual(batches[0]["words"][0]["text_byte_offset"], 2)
                if not mismatch:
                    self.assertEqual(batches[1]["words"][0]["text_byte_offset"], len("**你好**，".encode()))
                unavailable = [item for item in events if item["type"] == "audio.subtitle.unavailable"]
                self.assertEqual(len(unavailable), int(mismatch))
                if mismatch:
                    self.assertEqual(unavailable[0]["detail"], "response_text_mismatch")
                self.assertEqual(connection.pipeline.committed_answer, "**你好**，下一句。")
                self.assertEqual(events[-1]["type"], "audio.stream.end")

    async def test_sealing_a_turn_while_commit_waits_for_socket_lock_drops_commit(self):
        identity = {"session_epoch": 19, "turn_id": 1, "generation": 1}
        connection = self.connection("pcm_s16le", identity)
        await connection.send_lock.acquire()
        task = asyncio.create_task(connection.send_json({"type": "llm.done", **identity}))
        await asyncio.sleep(0)
        connection._sealed_turn = 1
        connection.send_lock.release()
        self.assertIs(await task, False)
        self.assertEqual(connection.ws.json, [])

    async def test_token_budget_limits_burst_after_idle_and_slow_writes(self):
        clock = FakeClock()
        pacer = _DownlinkPacer(clock=clock.clock, sleep=clock.sleep)

        sends = []
        for _ in range(25):
            await pacer.wait()
            sends.append(clock.now)
            # 缓慢的连接写入会消耗时间，但不能累计超过
            # 一个容量为 200 ms 的有限令牌桶所允许的后续音频。
            clock.now += 0.005

        capacity = int(DOWNLINK_BURST_SECONDS / DOWNLINK_FRAME_SECONDS)
        for index, sent_at in enumerate(sends, start=1):
            permitted = capacity + sent_at / DOWNLINK_FRAME_SECONDS + 1e-8
            self.assertLessEqual(index, permitted)

        # 生产者长时间停顿会补充令牌，但不会扩大令牌桶容量。
        clock.now += 30
        before = len(clock.sleeps)
        for _ in range(capacity + 1):
            await pacer.wait()
        self.assertEqual(len(clock.sleeps) - before, 1)
        self.assertAlmostEqual(clock.sleeps[-1], DOWNLINK_FRAME_SECONDS)

        # Windows 定时器可能延迟超过 15 ms 才唤醒。应按实际经过时间补充令牌，
        # 不能每次发送后重新等待完整一帧，
        # 否则听到的播放会变慢。额外等待可以允许少量追赶，
        # 但不能超过原来的令牌桶容量。
        coarse = FakeClock()

        async def coarse_sleep(seconds):
            coarse.now += seconds + 0.016

        paced = _DownlinkPacer(clock=coarse.clock, sleep=coarse_sleep)
        for index in range(110):
            await paced.wait()
            self.assertLessEqual(index + 1, capacity + coarse.now / DOWNLINK_FRAME_SECONDS + 1e-8)
        self.assertAlmostEqual(coarse.now, 2.0, delta=0.037)

    async def test_production_cancel_joins_pacing_wait_before_ack_without_tail(self):
        identity = {"session_epoch": 12, "turn_id": 2, "generation": 3}
        clock = PausedClock()
        pacer = _DownlinkPacer(clock=clock.clock, sleep=clock.sleep)
        connection = self.connection("pcm_s16le", identity)
        pcm = bytes(960 * 2 * 24)

        async def chunks():
            yield pcm

        with patch("transport.voice_websocket._DownlinkPacer", return_value=pacer):
            audio = asyncio.create_task(connection.send_audio(chunks(), identity))
            connection.pipeline.response_task = audio
            try:
                await asyncio.wait_for(clock.sleeping.wait(), 1)
                # 控制播放节奏的等待不能占用共享连接或控制消息锁。
                await asyncio.wait_for(connection.send_lock.acquire(), 0.05)
                connection.send_lock.release()
                original_send = connection.ws.send_json

                async def send_after_cleanup(event):
                    if event["type"] == "response.cancelled":
                        self.assertTrue(audio.done() and audio.cancelled())
                        self.assertIsNone(connection._playback_pending)
                    await original_send(event)

                connection.ws.send_json = send_after_cleanup
                # 验证真实 DeviceConnection 和 VoicePipeline 的
                # 取消流程，包括先回收旧任务再确认；
                # 不手动取消目标任务，也不伪造其确认消息。
                await asyncio.wait_for(connection._cancel_response(
                    {"type": "response.cancel", **identity}), 0.5)
                self.assertIsNone(connection.pipeline.response_task)
                self.assertTrue(audio.done() and audio.cancelled())
            finally:
                audio.cancel()
                with suppress(asyncio.CancelledError):
                    await audio
        self.assertEqual(len(connection.ws.binary), 10)
        self.assertEqual(connection.ws.json[-1]["type"], "response.cancelled")
        self.assertEqual(sum(event["type"] == "response.cancelled"
                             for event in connection.ws.json), 1)

    async def test_identity_change_during_wait_discards_frame_without_task_cancel(self):
        identity = {"session_epoch": 13, "turn_id": 2, "generation": 3}
        clock = PausedClock()
        pacer = _DownlinkPacer(clock=clock.clock, sleep=clock.sleep)
        connection = self.connection("pcm_s16le", identity)

        async def chunks():
            yield bytes(960 * 24)

        with patch("transport.voice_websocket._DownlinkPacer", return_value=pacer):
            audio = asyncio.create_task(connection.send_audio(chunks(), identity))
            try:
                await asyncio.wait_for(clock.sleeping.wait(), 1)
                connection.pipeline.response_identity = dict(identity, generation=4)
                clock.release.set()
                with self.assertRaises(asyncio.CancelledError):
                    await asyncio.wait_for(audio, 0.5)
                self.assertEqual(audio.cancelling(), 0)
            finally:
                audio.cancel()
                with suppress(asyncio.CancelledError):
                    await audio
        self.assertEqual(len(connection.ws.binary), 10)
        self.assertFalse(any(event["type"] == "audio.stream.end" for event in connection.ws.json))

    async def test_send_audio_preserves_frames_and_opus_pre_skip_with_pacing(self):
        for codec in ("pcm_s16le", "opus"):
            with self.subTest(codec=codec):
                if codec == "opus":
                    try:
                        native_version()
                    except CodecError as exc:
                        self.skipTest(str(exc))
                identity = {"session_epoch": 14, "turn_id": 1, "generation": 1}
                connection = self.connection(codec, identity)
                source_samples = 480 * 13 + 77
                pcm = struct.pack("<" + "h" * source_samples, *(
                    int(9000 * math.sin(2 * math.pi * 440 * index / 24000))
                    for index in range(source_samples)
                ))

                async def chunks():
                    for offset in range(0, len(pcm), 138):
                        yield pcm[offset:offset + 138]

                await connection.send_audio(chunks(), identity)
                packets = [AudioPacket.unpack(value) for value in connection.ws.binary]
                start = next(event for event in connection.ws.json
                             if event.get("type") == "audio.stream.start")
                end = next(event for event in connection.ws.json
                           if event.get("type") == "audio.stream.end")
                self.assertTrue(packets)
                self.assertEqual([packet.sequence for packet in packets], list(range(len(packets))))
                self.assertEqual([packet.sample_offset for packet in packets],
                                 [index * 480 for index in range(len(packets))])
                expected_pre_skip = 0
                if codec == "opus":
                    probe = Encoder(24000, 24000)
                    expected_pre_skip = probe.pre_skip
                    probe.close()
                self.assertEqual(start["pre_skip_samples"], expected_pre_skip)
                self.assertEqual(end["valid_end_sample"], start["pre_skip_samples"] + source_samples)
                if codec == "pcm_s16le":
                    joined = b"".join(packet.payload for packet in packets)
                    self.assertEqual(joined[:len(pcm)], pcm)
                    self.assertEqual(joined[len(pcm):], bytes(len(joined) - len(pcm)))
                else:
                    decoder = Decoder(24000)
                    try:
                        joined = b"".join(decoder.decode(packet.payload) for packet in packets)
                    finally:
                        decoder.close()
                    trimmed = joined[start["pre_skip_samples"] * 2:end["valid_end_sample"] * 2]
                    self.assertEqual(len(trimmed), len(pcm))
                    values = struct.unpack("<" + "h" * source_samples, trimmed)
                    self.assertGreater(math.sqrt(sum(value * value for value in values) / len(values)), 1000)

    async def test_mambo_first_segment_is_sent_while_second_synthesis_waits(self):
        identity = {"session_epoch": 20, "turn_id": 1, "generation": 1}
        connection = self.connection("pcm_s16le", identity)
        clock = FakeClock()
        pacer = _DownlinkPacer(clock=clock.clock, sleep=clock.sleep)
        second_requested = asyncio.Event()
        release_second = asyncio.Event()
        first_packet_sent = asyncio.Event()
        requests = []
        # 不足整帧的音频段必须连续拼接，段间不能补零。
        segment_pcm = (struct.pack("<h", 1111) * (480 * 13 + 77),
                       struct.pack("<h", -2222) * 499)

        class Response:
            status = 200

            def __init__(self, pcm, gate=None):
                output = BytesIO()
                with wave.open(output, "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(24000)
                    wav.writeframes(pcm)
                self.body = output.getvalue()
                self.content = self
                self.gate = gate

            async def __aenter__(self):
                if self.gate is not None:
                    second_requested.set()
                    await self.gate.wait()
                return self

            async def __aexit__(self, *args):
                return False

            async def iter_chunked(self, size):
                yield self.body

        class Session:
            def post(self, endpoint, *, json, timeout):
                index = len(requests)
                requests.append(dict(json))
                return Response(segment_pcm[index], release_second if index == 1 else None)

        provider = GptSoVitsTtsProvider(
            {"first_segment_chars": 8, "segment_chars": 8}, {}, Session())
        self.addAsyncCleanup(provider.close)
        await provider.start()
        await provider.append_text("第一段测试内容。第二段测试内容。")
        await provider.finish()
        original_send = connection.ws.send_bytes

        async def observe_packet(packet):
            await original_send(packet)
            first_packet_sent.set()

        connection.ws.send_bytes = observe_packet
        with patch("transport.voice_websocket._DownlinkPacer", return_value=pacer):
            sending = asyncio.create_task(connection.send_audio(provider.audio(), identity))
            try:
                await asyncio.wait_for(asyncio.gather(
                    first_packet_sent.wait(), second_requested.wait()), 1)
                self.assertFalse(sending.done())
                self.assertTrue(connection.ws.binary)
                self.assertEqual([event["type"] for event in connection.ws.json],
                                 ["audio.stream.start"])
                self.assertEqual(connection._playback_pending, identity)
                self.assertTrue(math.isinf(connection._playback_deadline))
                release_second.set()
                await asyncio.wait_for(sending, 1)
            finally:
                release_second.set()
                if not sending.done():
                    sending.cancel()
                with suppress(asyncio.CancelledError):
                    await sending

        self.assertEqual([request["text"] for request in requests],
                         ["第一段测试内容。", "第二段测试内容。"])
        self.assertEqual([event["type"] for event in connection.ws.json],
                         ["audio.stream.start", "audio.stream.end"])
        start, end = connection.ws.json
        self.assertEqual(start["subtitle_mode"], "none")
        packets = [AudioPacket.unpack(value) for value in connection.ws.binary]
        self.assertEqual([packet.sequence for packet in packets], list(range(len(packets))))
        self.assertEqual([packet.sample_offset for packet in packets],
                         [index * 480 for index in range(len(packets))])
        self.assertTrue(all(packet.stream_id == start["stream_id"] for packet in packets))
        self.assertEqual(end["stream_id"], start["stream_id"])
        pcm = b"".join(segment_pcm)
        self.assertEqual(end["valid_end_sample"], len(pcm) // 2)
        joined = b"".join(packet.payload for packet in packets)
        self.assertEqual(joined[:len(pcm)], pcm)
        self.assertEqual(joined[len(pcm):], bytes(len(joined) - len(pcm)))

    async def test_late_subtitle_overtakes_paced_multi_second_provider_pcm(self):
        identity = {"session_epoch": 15, "turn_id": 1, "generation": 1}
        connection = self.connection("pcm_s16le", identity)
        provider = VolcengineTtsProvider({}, {}, None)
        pcm = b"\x01\x00" * (24000 * 4)
        await provider._enqueue_audio(pcm)
        clock = PausedClock()
        subtitle_at_packet = []
        original_json = connection.ws.send_json

        async def observe_json(event):
            if event["type"] == "audio.subtitle":
                subtitle_at_packet.append(len(connection.ws.binary))
            await original_json(event)

        connection.ws.send_json = observe_json
        with patch("transport.voice_websocket._DownlinkPacer",
                   return_value=_DownlinkPacer(clock=clock.clock, sleep=clock.sleep)):
            task = asyncio.create_task(connection.send_audio(provider.media(), identity,
                                                             subtitle_enabled=True))
            try:
                await asyncio.wait_for(clock.sleeping.wait(), 1)
                # 上游元数据到达时，传输层仍有同一 PCM 帧中
                # 接近四秒的音频等待按节奏发送。
                provider._enqueue_subtitle({"text": "你好", "words": [
                    {"word": "你", "startTime": 0.02, "endTime": 0.2}]}, 398)
                provider._end()
                clock.release.set()
                await asyncio.wait_for(task, 1)
            finally:
                task.cancel()
                with suppress(asyncio.CancelledError):
                    await task
        self.assertEqual(subtitle_at_packet, [11])
        self.assertEqual(len(connection.ws.binary), 200)
        self.assertEqual(b"".join(AudioPacket.unpack(value).payload
                                  for value in connection.ws.binary), pcm)

    async def test_optional_subtitle_failure_is_immediate_once_and_audio_continues(self):
        for failure in (SubtitleUnavailable(),
                        SubtitleCue("好", (SubtitleWord("坏", 240, 480),), 398)):
            with self.subTest(failure=type(failure).__name__):
                identity = {"session_epoch": 16, "turn_id": 1, "generation": 1}
                connection = self.connection("pcm_s16le", identity)
                event_positions = []
                original_json = connection.ws.send_json

                async def observe_json(event):
                    if event["type"] == "audio.subtitle.unavailable":
                        event_positions.append(len(connection.ws.binary))
                    await original_json(event)

                connection.ws.send_json = observe_json

                async def chunks():
                    yield SubtitleCue("你", (SubtitleWord("你", 0, 240),), 398)
                    yield bytes(960)
                    yield failure
                    yield failure
                    yield SubtitleCue("好", (SubtitleWord("好", 240, 480),), 398)
                    yield bytes(960)

                with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
                    await connection.send_audio(chunks(), identity, subtitle_enabled=True)
                self.assertEqual(event_positions, [1])
                self.assertEqual(len(connection.ws.binary), 2)
                self.assertEqual(sum(event["type"] == "audio.subtitle"
                                     for event in connection.ws.json), 1)
                self.assertEqual(connection.ws.json[-1]["type"], "audio.stream.end")
                unavailable = next(event for event in connection.ws.json
                                   if event["type"] == "audio.subtitle.unavailable")
                expected_reason = "upstream_invalid" if isinstance(failure, SubtitleUnavailable) else "assembler_invalid"
                self.assertEqual(unavailable["reason"], expected_reason)
                self.assertTrue(any("status=unavailable reason=" + expected_reason in line
                                    for line in logs.output))
                self.assertTrue(any("status=following" in line for line in logs.output))
                self.assertNotIn("坏", " ".join(logs.output))

    async def test_missing_subtitles_fall_back_only_when_enabled_and_audio_is_unchanged(self):
        for enabled in (True, False):
            with self.subTest(enabled=enabled):
                identity = {"session_epoch": 17, "turn_id": 1, "generation": 1}
                connection = self.connection("pcm_s16le", identity)

                async def chunks():
                    yield b"\x01\x00" * 480

                await connection.send_audio(chunks(), identity, subtitle_enabled=enabled)
                types = [event["type"] for event in connection.ws.json]
                self.assertEqual(types.count("audio.subtitle.unavailable"), int(enabled))
                if enabled:
                    unavailable = next(event for event in connection.ws.json
                                       if event["type"] == "audio.subtitle.unavailable")
                    self.assertEqual(unavailable["reason"], "no_cues")
                self.assertEqual(len(connection.ws.binary), 1)
                self.assertEqual(AudioPacket.unpack(connection.ws.binary[0]).payload,
                                 b"\x01\x00" * 480)

    async def test_boundary_empty_words_then_provider_cue_reaches_device_before_pcm(self):
        identity = {"session_epoch": 18, "turn_id": 1, "generation": 1}
        connection = self.connection("pcm_s16le", identity)
        socket = FakeWebSocket(finish_audio=False)
        provider = VolcengineTtsProvider(
            {"voice": "test-voice", "resource_id": "seed-tts-2.0", "timeout_s": 1},
            {"VOLCENGINE_TTS_API_KEY": "mock-key"}, FakeSession(socket))
        self.addAsyncCleanup(provider.close)
        await provider.start()
        socket.push(response(350, provider.session_id, {"text": "你好", "words": [], "phonemes": []}))
        socket.push(response(350, provider.session_id, {"words": None}))
        socket.push(response(350, provider.session_id, {"words": [{"not_subtitle_word": True}]}))
        # TTS2.0 的逐词对齐可能在适配器队列中排在一个音频帧之后；
        # 传输层仍须在这段 PCM 按节奏发送期间下发该信息。
        pcm = b"\x01\x00" * 480
        socket.push(response(352, provider.session_id, pcm, audio=True))
        socket.push(response(398, provider.session_id, {"text": "你好", "words": [
            {"word": "你", "startTime": 0, "endTime": 0.01},
            {"word": "好", "startTime": 0.01, "endTime": 0.02}]}))
        socket.push(response(351, provider.session_id, {"words": [], "phonemes": []}))
        await provider.finish()
        socket.push(response(152, provider.session_id))
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            await connection.send_audio(provider.media(), identity, subtitle_enabled=provider.subtitle_enabled)
        types = [event["type"] for event in connection.ws.json]
        self.assertEqual(types, ["audio.stream.start", "audio.subtitle", "audio.stream.end"])
        batch = connection.ws.json[1]
        self.assertEqual(batch["text"], "你好")
        self.assertEqual([word["text_byte_offset"] for word in batch["words"]], [0, 3])
        self.assertEqual([word["start_sample"] for word in batch["words"]], [0, 240])
        self.assertEqual(connection.ws.json[0]["subtitle_mode"], "word_timestamps")
        self.assertEqual(AudioPacket.unpack(connection.ws.binary[0]).payload, pcm)
        self.assertFalse(provider._subtitle_failed)
        self.assertTrue(any("status=following" in line and "upstream_event=398" in line
                            and "cues=1 words=2 batches=1" in line for line in logs.output))

    @staticmethod
    def connection(codec, identity):
        class Socket:
            def __init__(self):
                self.json = []
                self.binary = []

            async def send_json(self, event):
                self.json.append(dict(event))

            async def send_bytes(self, packet):
                self.binary.append(packet)

        selected = SimpleNamespace(transport={
            "uplink": {"codec": "pcm_s16le"},
            "downlink": {"codec": codec, "bitrate_bps": 24000},
            "vbr": True,
        })
        connection = DeviceConnection(Socket(), None, {}, selected, None)
        connection.pipeline.response_identity = identity
        return connection


if __name__ == "__main__":
    unittest.main()
