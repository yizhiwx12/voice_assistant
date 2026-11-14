import asyncio
import gzip
import json
import math
import struct
import unittest
from types import SimpleNamespace

from providers.asr.doubao_streaming import (
    _AudioPacer,
    DEFAULT_ENDPOINT,
    MAX_RESPONSE_BYTES,
    DoubaoAsrError,
    DoubaoStreamingAsrProvider,
    encode_request,
    parse_response,
)


def response_packet(payload, *, sequence=1, last=False, event=None, compressed=True):
    flags = (1 if sequence is not None else 0) | (2 if last else 0) | (4 if event is not None else 0)
    packet = bytes((0x11, 0x90 | flags, 0x10 | int(compressed), 0))
    if sequence is not None:
        packet += struct.pack(">i", sequence)
    if event is not None:
        packet += struct.pack(">i", event)
    payload = json.dumps(payload, ensure_ascii=False).encode()
    if compressed:
        payload = gzip.compress(payload)
    return packet + struct.pack(">I", len(payload)) + payload


def unpack_request(packet):
    sequence, size = struct.unpack_from(">iI", packet, 4)
    if size != len(packet) - 12:
        raise AssertionError("Bad outbound payload length")
    return packet[1] >> 4, packet[1] & 15, sequence, gzip.decompress(packet[12:])


class ProtocolTests(unittest.TestCase):
    def test_error_diagnostics_only_export_fixed_reasons_and_bounded_integers(self):
        secret = "secret-key-and-audio"
        error = DoubaoAsrError(secret, reason=secret, upstream_code=secret, upstream_close_code=secret)
        self.assertEqual(error.diagnostics(), {
            "asr_reason": "unknown", "asr_upstream_code": 0, "asr_upstream_close_code": 0,
        })
        for invalid in (True, False, -1, 2 ** 40, 1.5, {}, [secret]):
            error.reason = invalid
            error.upstream_code = invalid
            error.upstream_close_code = invalid
            self.assertEqual(error.diagnostics(), {
                "asr_reason": "unknown", "asr_upstream_code": 0, "asr_upstream_close_code": 0,
            })

    def test_binary_and_json_rejections_keep_only_numeric_status_codes(self):
        secret = "secret-key-and-audio"
        body = secret.encode()
        binary = bytes((0x11, 0xF0, 0x10, 0)) + struct.pack(">II", 45000001, len(body)) + body
        for packet in (binary, response_packet({"code": 45000001, "message": secret})):
            with self.assertRaises(DoubaoAsrError) as caught:
                parse_response(packet)
            self.assertEqual(caught.exception.diagnostics(), {
                "asr_reason": "upstream_rejected", "asr_upstream_code": 45000001,
                "asr_upstream_close_code": 0,
            })
            self.assertNotIn(secret, str(caught.exception))
        for code in (secret, True, False, 0.0, -1, 2 ** 40, [secret], {"secret": secret}):
            with self.subTest(code=code), self.assertRaises(DoubaoAsrError) as caught:
                parse_response(response_packet({"code": code, "message": secret}))
            self.assertEqual(caught.exception.diagnostics()["asr_upstream_code"], 0)
            self.assertNotIn(secret, str(caught.exception))

    def test_requests_match_official_big_endian_sequence_and_gzip_protocol(self):
        message_type, flags, sequence, data = unpack_request(encode_request(b'{"audio":{}}', 1))
        self.assertEqual((message_type, flags, sequence), (1, 1, 1))
        self.assertEqual(data, b'{"audio":{}}')
        pcm = b"\x01\x00" * 30
        message_type, flags, sequence, data = unpack_request(encode_request(pcm, 3, audio=True, last=True))
        self.assertEqual((message_type, flags, sequence), (2, 3, -3))
        self.assertEqual(data, pcm)

    def test_response_optional_sequence_event_and_last_flag(self):
        for sequence, last, event in ((3, False, None), (-4, True, 1), (None, True, None)):
            with self.subTest(sequence=sequence):
                payload = {"result": {"text": "\u4f60\u597d"}}
                parsed = parse_response(response_packet(payload, sequence=sequence, last=last, event=event))
                self.assertEqual(parsed.sequence, sequence)
                self.assertEqual(parsed.is_last, last)
                self.assertEqual(parsed.event, event)
                self.assertEqual(parsed.payload, payload)

    def test_negative_sequence_marks_final_even_without_terminal_flag(self):
        parsed = parse_response(response_packet({"result": {}}, sequence=-3))
        self.assertTrue(parsed.is_last)

    def test_uncompressed_json_and_error_redaction(self):
        self.assertEqual(parse_response(response_packet({"result": {}}, compressed=False)).payload, {"result": {}})
        secret = b"remote-secret-audio-and-key"
        error = bytes((0x11, 0xF0, 0x10, 0)) + struct.pack(">II", 45000001, len(secret)) + secret
        with self.assertRaisesRegex(DoubaoAsrError, "45000001") as caught:
            parse_response(error)
        self.assertNotIn(secret.decode(), str(caught.exception))
        with self.assertRaisesRegex(DoubaoAsrError, "123") as caught:
            parse_response(response_packet({"code": 123, "message": secret.decode()}))
        self.assertNotIn(secret.decode(), str(caught.exception))

    def test_rejects_truncation_bad_compression_and_oversized_inflation(self):
        valid = response_packet({"result": {"text": "abc"}})
        for packet in (b"", valid[:6], valid[:-1], b"\x21" + valid[1:], valid + b"x"):
            with self.subTest(length=len(packet)), self.assertRaises(DoubaoAsrError):
                parse_response(packet)
        bad_zip = bytes((0x11, 0x90, 0x11, 0)) + struct.pack(">I", 3) + b"bad"
        with self.assertRaisesRegex(DoubaoAsrError, "compressed"):
            parse_response(bad_zip)
        large = response_packet({"result": {"text": "x" * MAX_RESPONSE_BYTES}})
        with self.assertRaisesRegex(DoubaoAsrError, "payload"):
            parse_response(large)


class FakeSocket:
    def __init__(self, *, final_on_end=True):
        self.messages = asyncio.Queue()
        self.sent = []
        self.closed = False
        self.final_on_end = final_on_end
        self.messages.put_nowait(SimpleNamespace(type=2, data=response_packet({})))

    async def send_bytes(self, packet):
        self.sent.append((asyncio.get_running_loop().time(), packet))
        message_type, flags, sequence, pcm = unpack_request(packet)
        if message_type == 2 and flags == 3 and self.final_on_end:
            await self.push({"result": {"text": "\u5b8c\u6574\u7ed3\u679c"}}, sequence=-abs(sequence), last=True)

    async def receive(self):
        return await self.messages.get()

    async def close(self):
        self.closed = True

    async def push(self, payload, **kwargs):
        await self.messages.put(SimpleNamespace(type=2, data=response_packet(payload, **kwargs)))


class FakeSession:
    def __init__(self, socket):
        self.socket = socket
        self.closed = False
        self.calls = []

    async def ws_connect(self, endpoint, **kwargs):
        self.calls.append((endpoint, kwargs))
        return self.socket

    async def close(self):
        self.closed = True


class LifecycleTests(unittest.IsolatedAsyncioTestCase):
    def provider(self, **options):
        socket = FakeSocket()
        session = FakeSession(socket)
        provider = DoubaoStreamingAsrProvider(
            {"model": "bigmodel", "resource_id": "volc.seedasr.sauc.duration", **options},
            {"VOLCENGINE_ASR_API_KEY": "test-key"}, session,
        )
        self.addAsyncCleanup(provider.close)
        return provider, socket, session

    async def test_handshake_headers_pcm_format_and_model_resource(self):
        provider, socket, session = self.provider()
        await provider.start()
        endpoint, options = session.calls[0]
        self.assertEqual(endpoint, DEFAULT_ENDPOINT)
        self.assertEqual(options["headers"]["X-Api-Key"], "test-key")
        self.assertEqual(options["headers"]["X-Api-Resource-Id"], "volc.seedasr.sauc.duration")
        self.assertIn("X-Api-Request-Id", options["headers"])
        payload = json.loads(unpack_request(socket.sent[0][1])[3])
        self.assertEqual(payload["audio"], {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1})
        self.assertEqual(payload["request"]["model_name"], "bigmodel")
        self.assertTrue(payload["request"]["show_utterances"])
        self.assertEqual(payload["request"]["result_type"], "full")

    async def test_recognition_metrics_count_only_parsed_normal_protocol_responses(self):
        secret = "private-transcript-and-key"
        bad_packets = (
            response_packet({"code": 45000001, "message": secret}),
            response_packet({"result": {"text": secret}})[:-1],
        )
        for packet in bad_packets:
            with self.subTest(packet_length=len(packet)):
                provider, socket, _ = self.provider()
                expected = {
                    "responses": 0, "nonempty_responses": 0,
                    "partial_events": 0, "segment_events": 0,
                    "final_received": 0, "final_chars": 0,
                }
                self.assertEqual(provider.recognition_metrics(), expected)
                await provider.start()
                expected["responses"] = 1  # 成功完成协议握手。
                await socket.messages.put(SimpleNamespace(type=2, data=packet))
                await asyncio.wait_for(provider._reader, 1)
                self.assertEqual(provider.recognition_metrics(), expected)
                self.assertNotIn(secret, json.dumps(provider.recognition_metrics()))
                self.assertTrue(all(type(value) is int for value in provider.recognition_metrics().values()))

    async def test_list_results_count_responses_separately_from_deduplicated_events(self):
        provider, socket, _ = self.provider()
        socket.final_on_end = False
        await provider.start()

        async def collect():
            return [event async for event in provider.events()]

        collected = asyncio.create_task(collect())
        first = {"definite": True, "text": "one", "start_time": 0, "end_time": 100}
        second = {"definite": True, "text": "two", "start_time": 100, "end_time": 200}
        result = [{"text": "one", "utterances": [first]}, {"text": "two", "utterances": [second]}]
        await socket.push({"result": result})
        await socket.push({"result": result})
        await socket.push({"result": [{"text": "final", "utterances": [first, second]}]}, last=True)
        events = await asyncio.wait_for(collected, 1)
        self.assertEqual([event.kind for event in events], [
            "partial", "segment_final", "segment_final", "utterance_final",
        ])
        self.assertEqual(events[0].text, "onetwo")
        self.assertEqual(await provider.end_utterance(), "final")
        metrics = provider.recognition_metrics()
        self.assertEqual(metrics, {
            "responses": 4, "nonempty_responses": 3,
            "partial_events": 1, "segment_events": 2,
            "final_received": 1, "final_chars": 5,
        })
        metrics["responses"] = 999
        self.assertEqual(provider.recognition_metrics()["responses"], 4, "Snapshots cannot mutate provider state")

    async def test_empty_final_is_distinct_from_no_final_for_object_and_list_results(self):
        for result in ({"text": ""}, []):
            with self.subTest(result_type=type(result)):
                provider, socket, _ = self.provider()
                await provider.start()
                self.assertEqual(provider.recognition_metrics()["final_received"], 0)
                await socket.push({"result": result}, last=True)
                events = [event async for event in provider.events()]
                self.assertEqual([(event.kind, event.text) for event in events], [("utterance_final", "")])
                self.assertEqual(await provider.end_utterance(), "")
                self.assertEqual(provider.recognition_metrics(), {
                    "responses": 2, "nonempty_responses": 0,
                    "partial_events": 0, "segment_events": 0,
                    "final_received": 1, "final_chars": 0,
                })

    async def test_recognition_event_counters_require_successful_queue_publication(self):
        provider, _, _ = self.provider(timeout_s=0.01)
        provider._events = asyncio.Queue(maxsize=1)
        await provider._emit(SimpleNamespace(kind="partial", text="private"))
        with self.assertRaises(DoubaoAsrError) as caught:
            await provider._emit(SimpleNamespace(kind="segment_final", text="private"))
        self.assertEqual(caught.exception.reason, "consumer_timeout")
        self.assertEqual(provider.recognition_metrics()["partial_events"], 1)
        self.assertEqual(provider.recognition_metrics()["segment_events"], 0)
        self.assertNotIn("private", json.dumps(provider.recognition_metrics()))

    async def test_continuous_audio_keeps_one_receive_alive_without_recognition_responses(self):
        provider, socket, _ = self.provider(timeout_s=0.25)
        original_receive = socket.receive
        receive_calls = 0
        receive_cancellations = 0

        async def receive():
            nonlocal receive_calls, receive_cancellations
            receive_calls += 1
            try:
                return await original_receive()
            except asyncio.CancelledError:
                receive_cancellations += 1
                raise

        socket.receive = receive
        await provider.start()
        started = asyncio.get_running_loop().time()
        for _ in range(35):
            await provider.feed_audio(bytes(640))
            await asyncio.sleep(0.02)
        self.assertGreater(asyncio.get_running_loop().time() - started, 2 * provider._timeout)
        self.assertFalse(provider._reader.done())
        self.assertEqual(receive_calls, 2, "One handshake read and one uninterrupted background read")
        self.assertEqual(receive_cancellations, 0)
        self.assertEqual(provider.failure_diagnostics()["asr_reason"], "none")
        await provider.end_utterance()

    async def test_unsent_partial_audio_and_empty_feeds_do_not_hide_stalled_input(self):
        provider, socket, _ = self.provider(timeout_s=0.08)
        await provider.start()
        await provider.feed_audio(bytes(2))  # 这里只是缓存音频，并未成功向上游发送 PCM。
        for _ in range(3):
            await provider.feed_audio(b"")
            await asyncio.sleep(0.01)
        await asyncio.wait_for(provider._reader, 1)
        with self.assertRaises(DoubaoAsrError) as feed_error:
            await provider.feed_audio(bytes(640))
        self.assertGreater(provider._input_bytes, 0)
        self.assertEqual(provider.failure_diagnostics()["asr_reason"], "input_timeout")
        self.assertIs(feed_error.exception, provider._error)
        with self.assertRaises(DoubaoAsrError) as events_error:
            async for _ in provider.events():
                pass
        self.assertIs(events_error.exception, feed_error.exception)
        self.assertFalse(socket.closed, "The transport owns cleanup after this diagnosed failure")

    async def test_responses_do_not_replace_successful_audio_progress(self):
        provider, socket, _ = self.provider(timeout_s=0.15)
        await provider.start()
        await provider.feed_audio(bytes(3200))
        for _ in range(3):
            await asyncio.sleep(0.03)
            await socket.push({})
            await asyncio.sleep(0)
            self.assertFalse(provider._reader.done())
        await asyncio.wait_for(provider._reader, 1)
        self.assertEqual(provider.failure_diagnostics()["asr_reason"], "input_timeout")

    async def test_missing_handshake_response_still_has_a_fixed_timeout(self):
        provider, socket, _ = self.provider(timeout_s=0.05)
        socket.messages.get_nowait()
        with self.assertRaises(DoubaoAsrError) as caught:
            await provider.start()
        self.assertEqual(caught.exception.diagnostics()["asr_reason"], "receive_timeout")
        self.assertTrue(socket.closed)

    async def test_eof_send_and_final_result_have_budgets_independent_of_old_receive_deadline(self):
        provider, socket, _ = self.provider(timeout_s=0.3)
        await provider.start()
        await provider.feed_audio(bytes(3200))
        original_send = socket.send_bytes
        socket.final_on_end = False
        final_tasks = []

        async def delayed_final(sequence):
            await asyncio.sleep(0.21)
            await socket.push({"result": {"text": "delayed final"}}, sequence=-abs(sequence), last=True)

        async def send(packet):
            _, flags, sequence, _ = unpack_request(packet)
            if flags == 3:
                await asyncio.sleep(0.21)
            await original_send(packet)
            if flags == 3:
                final_tasks.append(asyncio.create_task(delayed_final(sequence)))

        socket.send_bytes = send
        await asyncio.sleep(0.21)
        try:
            # 音频结束标记的发送跨过先前截止时间，随后最终结果在独立预算内到达。
            self.assertEqual(await provider.end_utterance(), "delayed final")
            self.assertEqual(provider.failure_diagnostics()["asr_reason"], "none")
        finally:
            for task in final_tasks:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

    async def test_close_and_error_frames_never_export_their_remote_text(self):
        secret = "secret-key-and-audio"
        for kind, data, reason, close in ((8, 1008, "upstream_closed", 1008),
                                          (8, secret, "upstream_closed", 0),
                                          (257, None, "upstream_closed", 1001),
                                          (258, RuntimeError(secret), "receive_failed", 0),
                                          (1, secret, "unexpected_frame", 0)):
            with self.subTest(kind=kind, data_type=type(data)):
                provider, socket, _ = self.provider()
                await provider.start()
                socket.close_code = 1001
                await socket.messages.put(SimpleNamespace(type=kind, data=data, extra=secret))
                await asyncio.wait_for(provider._reader, 1)
                self.assertEqual(provider.failure_diagnostics(), {
                    "asr_reason": reason, "asr_upstream_code": 0, "asr_upstream_close_code": close,
                })
                self.assertNotIn(secret, str(provider._error))

    async def test_send_failure_and_timeout_have_distinct_fixed_reasons(self):
        secret = "secret-key-and-audio"
        for exception, reason in ((TimeoutError(secret), "send_timeout"),
                                  (OSError(secret), "send_failed")):
            with self.subTest(reason=reason):
                provider, socket, _ = self.provider()
                await provider.start()

                async def failing_send(packet):
                    raise exception

                socket.send_bytes = failing_send
                with self.assertRaises(DoubaoAsrError) as caught:
                    await provider._send(b"test")
                self.assertEqual(caught.exception.diagnostics()["asr_reason"], reason)
                self.assertNotIn(secret, str(caught.exception))

    async def test_stream_revisions_segment_dedup_and_one_utterance_final(self):
        provider, socket, session = self.provider()
        await provider.start()

        async def collect():
            return [event async for event in provider.events()]

        collected = asyncio.create_task(collect())
        segment = {"definite": True, "text": "\u7b2c\u4e00\u53e5", "start_time": 0, "end_time": 200}
        await socket.push({"result": {"text": "\u7b2c", "utterances": []}})
        await socket.push({"result": {"text": "\u7b2c\u4e00\u53e5", "utterances": [segment]}})
        await socket.push({"result": {"text": "\u7b2c\u4e00\u53e5", "utterances": [segment]}})
        await asyncio.sleep(0.02)
        self.assertFalse(provider._done.is_set(), "A definite segment must not end the utterance")
        for _ in range(7):
            await provider.feed_audio(b"\x01\x00" * 320)
        transcript = await provider.end_utterance()
        self.assertEqual(await provider.end_utterance(), transcript)
        events = await collected
        self.assertEqual(transcript, "\u5b8c\u6574\u7ed3\u679c")
        self.assertEqual([event.kind for event in events], ["partial", "partial", "segment_final", "utterance_final"])
        self.assertEqual(events[-1].text, transcript)
        requests = [unpack_request(packet) for _, packet in socket.sent]
        self.assertEqual([req[2] for req in requests], [1, 2, -3])
        self.assertEqual(len(requests[1][3]), 3200)
        self.assertEqual(len(requests[2][3]), 1280)
        await provider.close()
        self.assertTrue(socket.closed)
        self.assertFalse(session.closed, "Shared HTTP session remains owned by its caller")

    async def test_exact_chunk_multiple_sends_terminal_empty_packet(self):
        provider, socket, _ = self.provider()
        await provider.start()
        await provider.feed_audio(b"\x00\x00" * 1600)
        await provider.end_utterance()
        audio = [unpack_request(packet) for _, packet in socket.sent][1:]
        self.assertEqual([(item[1], len(item[3])) for item in audio], [(1, 3200), (3, 0)])

    async def test_pacing_does_not_burst_buffered_audio(self):
        provider, socket, _ = self.provider()
        await provider.start()
        await provider.feed_audio(b"\x00\x00" * 4800)
        times = [when for when, packet in socket.sent if unpack_request(packet)[0] == 2]
        self.assertEqual(len(times), 3)
        self.assertGreaterEqual(times[1] - times[0], 0.09)
        self.assertGreaterEqual(times[2] - times[1], 0.09)

    async def test_cancel_stops_reader_and_rejects_new_audio(self):
        provider, socket, _ = self.provider()
        await provider.start()
        await provider.cancel()
        await provider.cancel()
        self.assertTrue(socket.closed)
        self.assertTrue(provider._reader.done())
        with self.assertRaisesRegex(DoubaoAsrError, "canceled"):
            await provider.feed_audio(b"\x00\x00")
        with self.assertRaisesRegex(DoubaoAsrError, "canceled"):
            await provider.end_utterance()

    async def test_cancel_during_pacing_prevents_queued_audio_send(self):
        provider, socket, _ = self.provider()
        await provider.start()
        feeding = asyncio.create_task(provider.feed_audio(b"\x00\x00" * 3200))
        await asyncio.sleep(0.02)
        await provider.cancel()
        with self.assertRaisesRegex(DoubaoAsrError, "canceled"):
            await feeding
        audio_requests = [packet for _, packet in socket.sent if unpack_request(packet)[0] == 2]
        self.assertEqual(len(audio_requests), 1)

    async def test_missing_final_times_out_and_closes(self):
        provider, socket, _ = self.provider(timeout_s=0.05)
        socket.final_on_end = False
        await provider.start()
        with self.assertRaisesRegex(DoubaoAsrError, "timed out"):
            await provider.end_utterance()
        self.assertTrue(socket.closed)

    async def test_error_is_propagated_without_upstream_text(self):
        provider, socket, _ = self.provider()
        await provider.start()
        await socket.push({"code": 45000001, "message": "secret-key-and-audio"})
        with self.assertRaisesRegex(DoubaoAsrError, "45000001") as caught:
            async for _ in provider.events():
                pass
        self.assertNotIn("secret", str(caught.exception))

    async def test_final_empty_text_retracts_partial_and_end_before_start_fails(self):
        provider, socket, _ = self.provider()
        with self.assertRaisesRegex(DoubaoAsrError, "not open"):
            await provider.end_utterance()
        await provider.start()
        await socket.push({"result": {"text": "revise-me"}})
        await socket.push({"result": {"text": ""}}, last=True)
        events = [event async for event in provider.events()]
        self.assertEqual(events[-1].kind, "utterance_final")
        self.assertEqual(events[-1].text, "")
        self.assertEqual(await provider.end_utterance(), "")
        self.assertEqual(provider.recognition_metrics(), {
            "responses": 3, "nonempty_responses": 1,
            "partial_events": 1, "segment_events": 0,
            "final_received": 1, "final_chars": 0,
        })

    async def test_closing_early_does_not_commit_partial_text(self):
        provider, socket, _ = self.provider()
        await provider.start()
        await socket.push({"result": {"text": "unconfirmed"}})
        await provider.close()
        with self.assertRaisesRegex(DoubaoAsrError, "before final"):
            await provider.end_utterance()

    async def test_bounded_input_rejects_invalid_pcm_and_overlong_utterance(self):
        provider, _, _ = self.provider(max_utterance_s=0.01)
        await provider.start()
        for pcm in (b"x", b"\x00" * 32002, bytearray(b"\x00\x00")):
            with self.subTest(type=type(pcm)), self.assertRaises(ValueError):
                await provider.feed_audio(pcm)
        with self.assertRaisesRegex(DoubaoAsrError, "duration"):
            await provider.feed_audio(b"\x00\x00" * 320)

    async def test_rejects_non_20_model_and_bad_chunk_configuration(self):
        for options in ({"model": "anything"}, {"chunk_duration_ms": 20}, {"endpoint": "ws://example.test"}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.provider(**options)

    async def test_send_metrics_report_inflight_and_completed_duration(self):
        provider, socket, _ = self.provider(timeout_s=1)
        await provider.start()
        gate = asyncio.Event()
        started = asyncio.Event()

        async def blocked_send(packet):
            started.set()
            await gate.wait()

        socket.send_bytes = blocked_send
        task = asyncio.create_task(provider._send(b"audio"))
        await asyncio.wait_for(started.wait(), 1)
        metrics = provider.pacing_metrics()
        self.assertEqual(metrics["send_inflight"], 1)
        self.assertGreaterEqual(metrics["send_inflight_ms"], 0)
        await asyncio.sleep(0.02)
        self.assertGreater(provider.pacing_metrics()["send_inflight_ms"], 0)
        gate.set()
        await task
        metrics = provider.pacing_metrics()
        self.assertEqual(metrics["send_inflight"], 0)
        self.assertEqual(metrics["send_inflight_ms"], 0)
        self.assertGreater(metrics["max_send_inflight_ms"], 0)

    async def test_cancelled_send_clears_inflight_metric(self):
        provider, socket, _ = self.provider(timeout_s=1)
        await provider.start()
        started = asyncio.Event()

        async def blocked_send(packet):
            started.set()
            await asyncio.Event().wait()

        socket.send_bytes = blocked_send
        task = asyncio.create_task(provider._send(b"audio"))
        await asyncio.wait_for(started.wait(), 1)
        self.assertEqual(provider.pacing_metrics()["send_inflight"], 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(provider.pacing_metrics()["send_inflight"], 0)


class SimulatedClock:
    """即使消费者正在等待或发送，也按实时速率投递输入。"""
    def __init__(self, seconds=30):
        self.now = 0.0
        self.queue = asyncio.Queue(maxsize=25)
        self.next_frame = 1
        self.frames = int(seconds / 0.02)
        self.peak = 0

    def time(self):
        return self.now

    def advance(self, duration):
        self.now += duration
        while self.next_frame <= self.frames and self.next_frame * 0.02 <= self.now + 1e-9:
            self.queue.put_nowait(bytes(640))
            self.peak = max(self.peak, self.queue.qsize())
            self.next_frame += 1

    async def sleep(self, duration):
        # Windows 单调时钟精度较低，唤醒也有抖动，但发送节奏不能持续漂移。
        quantum = 0.015625
        self.advance(max(1, math.ceil((duration - 1e-12) / quantum)) * quantum)


class PacingRegressionTests(unittest.IsolatedAsyncioTestCase):
    def provider(self, clock, send_delay):
        provider = DoubaoStreamingAsrProvider({}, {})
        provider._started = True
        provider._pacer = _AudioPacer(clock.time, clock.sleep)
        sent = []

        async def send(packet):
            request = unpack_request(packet)
            sent.append((clock.time(), request))
            clock.advance(send_delay(len(sent)))
            if request[1] == 3:
                provider._done.set()

        provider._send = send
        self.addAsyncCleanup(provider.close)
        return provider, sent

    async def drain(self, provider, clock):
        while clock.next_frame <= clock.frames or not clock.queue.empty():
            if clock.queue.empty():
                clock.advance(max(0, clock.next_frame * 0.02 - clock.time()))
            await provider.feed_audio(clock.queue.get_nowait())
            clock.queue.task_done()

    async def test_30_seconds_of_20ms_input_has_no_cumulative_send_or_timer_drift(self):
        clock = SimulatedClock()
        # 改变正常发送开销，并注入一次较长的临时停顿。
        provider, sent = self.provider(clock, lambda count: 0.12 if count % 47 == 0 else 0.014 + (count % 3) * 0.003)
        await self.drain(provider, clock)
        await provider.end_utterance()
        self.assertEqual(provider.pacing_metrics()["samples"], 30 * 16000)
        self.assertEqual(b"".join(item[3] for _, item in sent), bytes(30 * 32000))
        self.assertLessEqual(clock.peak, 10, "Normal overhead must not grow the 500 ms queue")
        self.assertLess(clock.time(), 30.5)
        times = [when for when, request in sent if request[1] != 3]
        self.assertTrue(all(b - a >= 0.09 - 1e-9 for a, b in zip(times, times[1:])))
        self.assertGreater(provider.pacing_metrics()["max_send_ms"], 100)

    async def test_genuinely_slow_connection_still_hits_bounded_queue_and_releases(self):
        clock = SimulatedClock()
        provider, sent = self.provider(clock, lambda _: 0.14)
        with self.assertRaises(asyncio.QueueFull):
            await self.drain(provider, clock)
        self.assertEqual(clock.queue.qsize(), 25)
        self.assertLess(clock.time(), 5)
        await provider.cancel()
        self.assertTrue(provider._closed)
        with self.assertRaises(DoubaoAsrError):
            await provider.feed_audio(bytes(640))

    async def test_partial_tail_preserves_samples_and_a_new_round_resets_clock(self):
        clock = SimulatedClock(seconds=0.14)
        provider, sent = self.provider(clock, lambda _: 0.014)
        await self.drain(provider, clock)
        await provider.end_utterance()
        self.assertEqual([(request[1], len(request[3])) for _, request in sent], [(1, 3200), (3, 1280)])
        self.assertEqual(provider.pacing_metrics()["samples"], 2240)
        await provider.close()
        # 新的适配器管理下一轮发言，不继承上一轮的截止时间。
        clock.advance(3)
        fresh, new_sent = self.provider(clock, lambda _: 0.014)
        before = clock.time()
        await fresh.feed_audio(bytes(3200))
        self.assertEqual(new_sent[0][0], before)
        self.assertEqual(fresh.pacing_metrics()["samples"], 1600)

    async def test_input_pause_cannot_release_buffered_audio_as_a_burst(self):
        clock = SimulatedClock(seconds=0)
        provider, sent = self.provider(clock, lambda _: 0.014)
        await provider.feed_audio(bytes(3200))
        clock.advance(2)
        await provider.feed_audio(bytes(9600))
        times = [when for when, _ in sent]
        self.assertTrue(all(b - a >= 0.09 - 1e-9 for a, b in zip(times, times[1:])))


if __name__ == "__main__":
    unittest.main()
