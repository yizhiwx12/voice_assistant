"""本机回环 PC 设备模拟；模拟服务商不连接云端。"""

import asyncio
import importlib.util
import io
import json
import math
import struct
import tempfile
import time
import unittest
import wave
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from audio.opus_codec import CodecError, Decoder, Encoder, native_version
from core.config import load_voice_config, select_profile
from transport.voice_protocol import AudioPacket, DOWNLINK, UPLINK, ProtocolError
from transport.voice_websocket import DeviceConnection, UplinkBacklogError
import voice_server


HAS_AIOHTTP = importlib.util.find_spec("aiohttp") is not None


def tone(rate, count, start=0):
    return struct.pack("<" + "h" * count, *(
        int(9000 * math.sin(2 * math.pi * 510 * (index + start) / rate))
        for index in range(count)
    ))


class SimulatedAsr:
    def __init__(self):
        self.pcm = bytearray()
        self.recognitions = 0
        self.closed = False
        self.events_queue = asyncio.Queue()

    async def start(self):
        self.events_queue.put_nowait(SimpleNamespace(kind="partial", text="\u4f60"))

    async def feed_audio(self, data):
        self.pcm.extend(data)

    async def end_utterance(self):
        self.recognitions += 1
        return "\u4f60\u597d"

    async def recognize_wav_async(self, data, session):
        with wave.open(io.BytesIO(data), "rb") as wav:
            self.pcm.extend(wav.readframes(wav.getnframes()))
        return await self.end_utterance()

    async def events(self):
        while True:
            yield await self.events_queue.get()

    async def close(self):
        self.closed = True


class SimulatedLlm:
    def __init__(self, gate=None):
        self.gate = gate
        self.calls = 0
        self.closed = False

    async def stream(self, messages, session):
        self.calls += 1
        try:
            yield "\u7b2c\u4e00\u53e5\u3002"
            if self.gate:
                await self.gate.wait()
            yield "\u7b2c\u4e8c\u53e5\u3002"
        finally:
            self.closed = True


class SimulatedTts:
    samples_per_sentence = 1103

    def __init__(self, gate=None):
        self.gate = gate
        self.pcm = bytearray()
        self.queue = asyncio.Queue()
        self.closed = False
        self.cancelled = False

    async def start(self):
        pass

    async def append_text(self, text):
        samples = tone(24000, self.samples_per_sentence, len(self.pcm) // 2)
        self.pcm.extend(samples)
        # 故意避开设备帧边界拆分，验证末尾补零。
        await self.queue.put(samples[:182])
        await self.queue.put(samples[182:])
        if self.gate:
            # 在合成输出首批可播放样本后暂停。TTS 开始前全文
            # 已经提交，因此用 LLM 阻塞门控会使这些取消测试
            # 在任何音频流建立之前就停止。
            await self.gate.wait()

    async def finish(self):
        await self.queue.put(None)

    async def audio(self):
        while (value := await self.queue.get()) is not None:
            yield value

    async def cancel(self):
        self.cancelled = True
        await self.close()

    async def close(self):
        self.closed = True


class SimulatedProviders:
    def __init__(self, *, hold_first_response=False):
        self.created = []
        self.asr = []
        self.llm = []
        self.tts = []
        self.first_gate = asyncio.Event() if hold_first_response else None

    def __call__(self, kind, entry, credentials, session):
        self.created.append((kind, entry.name))
        if kind == "asr":
            result = SimulatedAsr()
            self.asr.append(result)
        elif kind == "llm":
            result = SimulatedLlm()
            self.llm.append(result)
        else:
            result = SimulatedTts(self.first_gate if not self.tts else None)
            self.tts.append(result)
        return result


@unittest.skipUnless(HAS_AIOHTTP, "Install server/requirements.txt for loopback WebSocket tests")
class VoiceWebSocketTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestClient, TestServer
        self.TestClient, self.TestServer = TestClient, TestServer
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.config_data = json.loads((Path(__file__).parents[1] / "config" / "voice_config.example.json").read_text(encoding="utf-8"))
        self.config_data["llm"]["qwen_default"].update(
            base_url="https://configured-only.invalid/compatible-mode/v1", model="synthetic-qwen")
        self.config_data["tts"]["volc_default"]["voice"] = "synthetic-volc-voice"
        self.config_data["tts"]["cosyvoice_default"].update(
            endpoint="wss://configured-only.invalid/cosyvoice", model="synthetic-cosyvoice", voice="synthetic-cosy-voice")
        self.credentials = {name: "synthetic-test-value" for name in (
            "DASHSCOPE_API_KEY", "DASHSCOPE_WORKSPACE_ID", "DEEPSEEK_API_KEY",
            "VOLCENGINE_ASR_API_KEY", "VOLCENGINE_TTS_API_KEY",
        )}
        self.providers = SimulatedProviders()
        self.observed = []

    async def make_client(self, *, llm=None):
        path = Path(self.directory.name) / "voice_config.json"
        path.write_text(json.dumps(self.config_data), encoding="utf-8")
        config = load_voice_config(path, environ={})
        selected = select_profile(config, profile="realtime_default", llm=llm, mode="realtime")
        app = voice_server.create_realtime_app(config, self.credentials, selected, provider_factory=self.providers)
        client = self.TestClient(self.TestServer(app))
        await client.start_server()
        self.addAsyncCleanup(client.close)
        return client

    async def connect(self, client, *, codec="pcm_s16le", **options):
        ws = await client.ws_connect("/ws/voice")
        self.addAsyncCleanup(ws.close)
        await ws.send_json({"type": "session.start", "version": 1,
                            "uplink_codec": codec, "downlink_codec": codec, **options})
        ready = await self.until(ws, "session.ready")
        return ws, ready

    async def receive(self, ws):
        import aiohttp
        message = await ws.receive(timeout=3)
        if message.type == aiohttp.WSMsgType.BINARY:
            value = AudioPacket.unpack(message.data)
        elif message.type == aiohttp.WSMsgType.TEXT:
            value = json.loads(message.data)
        else:
            self.fail(f"WebSocket closed unexpectedly (message type {message.type})")
        self.observed.append(value)
        return value

    async def until(self, ws, kind):
        for _ in range(100):
            value = await self.receive(ws)
            if isinstance(value, dict):
                if value.get("type") == "error" and kind != "error":
                    self.fail(f"Unexpected session error: {value.get('code')}")
                if value.get("type") == kind:
                    return value
        self.fail(f"Event {kind} did not arrive")

    async def start_stream(self, ws, epoch, *, codec="pcm_s16le", pre_skip=0, stream_id=1):
        await ws.send_json({"type": "audio.stream.start", "session_epoch": epoch,
                            "stream_id": stream_id, "codec": codec, "sample_rate_hz": 16000,
                            "channels": 1, "frame_duration_ms": 20, "pre_skip_samples": pre_skip})

    async def start_turn(self, ws, epoch, turn=1):
        await ws.send_json({"type": "speech.start", "session_epoch": epoch, "turn_id": turn})
        return await self.until(ws, "speech.started")

    async def send_frames(self, ws, epoch, payloads, *, start_sequence=0, stream_id=1):
        for index, payload in enumerate(payloads, start_sequence):
            await ws.send_bytes(AudioPacket(UPLINK, epoch, stream_id, 0, 0, index, index * 320, payload).pack())
            # 让有限的服务端解码工作任务获得执行机会；这里是设备
            # 模拟器，不是积压压力或洪泛测试。
            await asyncio.sleep(0.002)

    async def test_pcm_device_roundtrip_fallback_tail_and_duplicate_end(self):
        client = await self.make_client()
        with patch("transport.voice_websocket.native_version", side_effect=CodecError("native unavailable")):
            ws, ready = await self.connect(client)
        self.assertEqual((ready["uplink_codec"], ready["downlink_codec"]), ("pcm_s16le", "pcm_s16le"))
        self.assertEqual((ready["uplink_sample_rate_hz"], ready["downlink_sample_rate_hz"]), (16000, 24000))
        epoch = ready["session_epoch"]
        await self.start_stream(ws, epoch)
        await self.start_turn(ws, epoch)
        source = tone(16000, 320 * 5)
        await self.send_frames(ws, epoch, [source[index:index + 640] for index in range(0, len(source), 640)])
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch, "stream_id": 1, "valid_end_sample": 1600})
        ending = {"type": "speech.end", "session_epoch": epoch, "turn_id": 1}
        await ws.send_json(ending)
        await ws.send_json(ending)
        done = await self.until(ws, "response.done")
        self.assertEqual(done["reason"], "complete")
        self.assertEqual(bytes(self.providers.asr[0].pcm), source)
        self.assertEqual(self.providers.asr[0].recognitions, 1)
        self.assertEqual(sum(item.calls for item in self.providers.llm), 1)
        self.assertEqual(sum(isinstance(item, dict) and item.get("type") == "asr.utterance_final" for item in self.observed), 1)
        starts = [item for item in self.observed if isinstance(item, dict) and item.get("type") == "audio.stream.start"]
        ends = [item for item in self.observed if isinstance(item, dict) and item.get("type") == "audio.stream.end"]
        packets = [item for item in self.observed if isinstance(item, AudioPacket)]
        committed = next(item for item in self.observed
                         if isinstance(item, dict) and item.get("type") == "llm.done")
        self.assertEqual("".join(item["text"] for item in self.observed
                                 if isinstance(item, dict) and item.get("type") == "llm.delta"),
                         "第一句。第二句。")
        self.assertLess(self.observed.index(committed), self.observed.index(starts[0]))
        self.assertLess(self.observed.index(committed), self.observed.index(packets[0]))
        self.assertEqual(starts[0]["sample_rate_hz"], 24000)
        self.assertEqual(starts[0]["pre_skip_samples"], 0)
        self.assertTrue(packets)
        self.assertTrue(all(item.kind == DOWNLINK and len(item.payload) == 960 for item in packets))
        self.assertEqual([item.sequence for item in packets], list(range(len(packets))))
        self.assertEqual([item.sample_offset for item in packets], [480 * i for i in range(len(packets))])
        valid_samples = ends[0]["valid_end_sample"]
        self.assertEqual(valid_samples, len(self.providers.tts[0].pcm) // 2)
        self.assertEqual(b"".join(item.payload for item in packets)[:valid_samples * 2], bytes(self.providers.tts[0].pcm))

    async def test_native_opus_device_roundtrip_preskip_and_valid_end(self):
        try:
            native_version()
        except CodecError as exc:
            self.skipTest(str(exc))
        client = await self.make_client()
        ws, ready = await self.connect(client, codec="opus")
        epoch = ready["session_epoch"]
        encoder = Encoder(16000, 24000)
        decoder = Decoder(24000)
        self.addCleanup(encoder.close)
        self.addCleanup(decoder.close)
        source_samples = 320 * 4 + 71
        source = tone(16000, source_samples)
        payloads = encoder.feed(source) + encoder.finish()
        await self.start_stream(ws, epoch, codec="opus", pre_skip=encoder.pre_skip)
        await self.start_turn(ws, epoch)
        await self.send_frames(ws, epoch, payloads)
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch, "stream_id": 1,
                            "valid_end_sample": encoder.pre_skip + source_samples})
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1})
        done = await self.until(ws, "response.done")
        self.assertEqual(done["reason"], "complete")
        captured = bytes(self.providers.asr[0].pcm)
        self.assertEqual(len(captured), source_samples * 2)
        self.assertGreater(max(abs(value) for value in struct.unpack("<" + "h" * source_samples, captured)), 1000)
        start = next(item for item in self.observed if isinstance(item, dict) and item.get("type") == "audio.stream.start")
        end = next(item for item in self.observed if isinstance(item, dict) and item.get("type") == "audio.stream.end")
        packets = [item for item in self.observed if isinstance(item, AudioPacket)]
        decoded = b"".join(decoder.decode(item.payload) for item in packets)
        trimmed = decoded[start["pre_skip_samples"] * 2:end["valid_end_sample"] * 2]
        self.assertEqual(len(trimmed), len(self.providers.tts[0].pcm))
        values = struct.unpack("<" + "h" * (len(trimmed) // 2), trimmed)
        self.assertGreater(math.sqrt(sum(value * value for value in values) / len(values)), 1000)
        self.assertTrue(all(item.epoch == epoch and item.turn_id == 1 and item.generation == 1 for item in packets))

    async def test_explicit_profile_replaces_startup_overrides(self):
        client = await self.make_client(llm="qwen_default")
        ws, ready = await self.connect(client)
        self.assertEqual(ready["selection"]["llm"]["id"], "qwen_default")
        await ws.close()
        ws, ready = await self.connect(client, profile="qwen_compatible")
        selected = ready["selection"]
        self.assertEqual(selected["asr"]["id"], "qwen_existing")
        self.assertEqual(selected["llm"]["id"], "deepseek_default")
        self.assertEqual(selected["tts"]["id"], "cosyvoice_default")
        self.assertEqual(self.providers.created, [])

    async def test_realtime_app_preserves_legacy_http_routes_and_text_limits(self):
        client = await self.make_client(llm="qwen_default")
        async with client.get("/ping") as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(await response.text(), "OK")
        transcript = "\u6d4b" * 50
        answer = "\u7b54" * 60
        asr = AsyncMock(return_value=transcript)
        llm = AsyncMock(return_value=answer)
        with patch("providers.asr.qwen_batch.QwenBatchProvider.recognize_wav_async", asr), \
             patch("providers.llm.chat_completions.ChatCompletionsProvider.generate_async", llm), \
             patch("voice_server._save_legacy_audio") as save_audio:
            async with client.post("/audio", data=bytes(640)) as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(await response.text(), f"{transcript[:32]}\n{answer[:40]}")
            save_audio.assert_called_once_with(bytes(640))
        asr.assert_awaited_once()
        llm.assert_awaited_once()
        self.assertEqual(llm.await_args.args[0][1]["content"], transcript)
        self.assertEqual(self.providers.created, [], "Legacy HTTP does not choose the WebSocket profile providers")
        with patch("providers.asr.qwen_batch.QwenBatchProvider.recognize_wav_async", AsyncMock(return_value="")), \
             patch("providers.llm.chat_completions.ChatCompletionsProvider.generate_async", AsyncMock()) as llm, \
             patch("voice_server._save_legacy_audio"):
            async with client.post("/audio", data=bytes(640)) as response:
                self.assertEqual(await response.text(), "\u672a\u8bc6\u522b\u5230\u8bed\u97f3\n\u8bf7\u9760\u8fd1\u9ea6\u514b\u98ce\u540e\u91cd\u8bd5\u3002")
            llm.assert_not_awaited()
        async with client.post("/audio", data=b"\0") as response:
            self.assertEqual(response.status, 400)

    async def test_fallback_disallowed_and_bad_epoch_or_header_are_rejected(self):
        self.config_data["transport"]["allow_pcm_fallback"] = False
        client = await self.make_client()
        ws = await client.ws_connect("/ws/voice")
        self.addAsyncCleanup(ws.close)
        await ws.send_json({"type": "session.start", "version": 1, "uplink_codec": "pcm_s16le", "downlink_codec": "pcm_s16le"})
        error = await self.until(ws, "error")
        self.assertEqual(error["code"], "voice_session_failed")
        self.config_data["transport"]["allow_pcm_fallback"] = True
        client = await self.make_client()
        for malformed in ("epoch", "header"):
            with self.subTest(malformed=malformed):
                ws, ready = await self.connect(client)
                packet = AudioPacket(UPLINK, ready["session_epoch"] ^ 1, 1, 0, 0, 0, 0, bytes(640)).pack()
                if malformed == "header":
                    packet = bytes((2,)) + packet[1:]
                await ws.send_bytes(packet)
                self.assertEqual((await self.until(ws, "error"))["code"], "voice_session_failed")
                await ws.close()

    async def test_response_cancel_blocks_old_audio_and_keeps_uplink_stream(self):
        self.providers = SimulatedProviders(hold_first_response=True)
        client = await self.make_client()
        ws, ready = await self.connect(client)
        epoch = ready["session_epoch"]
        await self.start_stream(ws, epoch)
        await self.start_turn(ws, epoch)
        await self.send_frames(ws, epoch, [tone(16000, 320)] * 4)
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1, "sample_offset": 1280})
        while True:
            value = await self.receive(ws)
            if isinstance(value, AudioPacket):
                old = value
                break
        await ws.send_json({"type": "response.cancel", "session_epoch": epoch, "turn_id": old.turn_id, "generation": old.generation})
        await self.until(ws, "response.cancelled")
        cancellation_boundary = len(self.observed)
        self.providers.first_gate.set()
        # 取消回答期间持续输入的音频继续推进，无需重新发送
        # stream.start，也不重置序号或已解码样本偏移。
        await self.send_frames(ws, epoch, [bytes(640)], start_sequence=4)
        await self.start_turn(ws, epoch, turn=2)
        await self.send_frames(ws, epoch, [tone(16000, 320)] * 4, start_sequence=5)
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 2, "sample_offset": 2880})
        done = await self.until(ws, "response.done")
        self.assertEqual(done["turn_id"], 2)
        self.assertEqual(done["reason"], "complete")
        cancelled = [value for value in self.observed
                     if isinstance(value, dict) and value.get("type") == "response.cancelled"]
        self.assertEqual(len(cancelled), 1, "One cancel command must produce one acknowledgement")
        later_audio = [value for value in self.observed[cancellation_boundary:] if isinstance(value, AudioPacket)]
        self.assertTrue(later_audio)
        self.assertTrue(all((value.turn_id, value.generation) != (old.turn_id, old.generation) for value in later_audio))
        self.assertTrue(self.providers.tts[0].cancelled)
        self.assertTrue(self.providers.llm[0].closed)
        self.assertEqual(len(self.providers.asr), 2)
        self.assertEqual([item.recognitions for item in self.providers.asr], [1, 1])

    async def test_barge_in_cancel_waits_for_cleanup_before_ack_and_new_stream(self):
        factory = SimulatedProviders(hold_first_response=True)
        cleanup_entered, cleanup_gate = asyncio.Event(), asyncio.Event()

        def providers(kind, *args):
            provider = factory(kind, *args)
            if kind == "tts" and len(factory.tts) == 1:
                original_close = provider.close

                async def blocked_close():
                    cleanup_entered.set()
                    await cleanup_gate.wait()
                    await original_close()

                provider.close = blocked_close
            return provider

        self.providers = providers
        client = await self.make_client()
        ws, ready = await self.connect(client)
        epoch = ready["session_epoch"]
        await self.start_stream(ws, epoch)
        await self.start_turn(ws, epoch)
        await self.send_frames(ws, epoch, [tone(16000, 320)] * 4)
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch,
                            "stream_id": 1, "valid_end_sample": 1280})
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1})
        while not isinstance(old := await self.receive(ws), AudioPacket):
            pass

        cancel = {"type": "response.cancel", "session_epoch": epoch,
                  "turn_id": old.turn_id, "generation": old.generation}
        receive_cancel = None
        try:
            await ws.send_json(cancel)
            await asyncio.wait_for(cleanup_entered.wait(), 1)
            # 即使客户端提前流水发送开始消息，只要旧一代回答仍在回收上游
            # 资源，就不能开始新的 ASR。
            await ws.send_json({"type": "speech.start", "session_epoch": epoch, "turn_id": 2})
            receive_cancel = asyncio.create_task(self.until(ws, "response.cancelled"))
            await asyncio.sleep(0.03)
            self.assertFalse(receive_cancel.done())
            self.assertEqual(len(factory.asr), 1)
            self.assertTrue(factory.llm[0].closed)
            self.assertFalse(factory.tts[0].closed)

            cleanup_gate.set()
            acknowledgement = await asyncio.wait_for(receive_cancel, 1)
            self.assertEqual({key: acknowledgement[key] for key in cancel if key != "type"},
                             {key: cancel[key] for key in cancel if key != "type"})
            self.assertTrue(factory.tts[0].closed)
            self.assertTrue(factory.tts[0].cancelled)
            started = await self.until(ws, "speech.started")
            self.assertEqual(started["turn_id"], 2)
            acknowledgement_boundary = self.observed.index(acknowledgement)

            # 固件会关闭每条上行音频流，并以序号和样本偏移均为零打开下一条；
            # 取消操作不能恢复第 1 条音频流。
            await self.start_stream(ws, epoch, stream_id=2)
            source = tone(16000, 320 * 4)
            await self.send_frames(ws, epoch,
                                   [source[index:index + 640] for index in range(0, len(source), 640)],
                                   stream_id=2)
            await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch,
                                "stream_id": 2, "valid_end_sample": 1280})
            await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 2})
            self.assertEqual((await self.until(ws, "response.done"))["turn_id"], 2)
            self.assertEqual(len(factory.asr), 2)
            self.assertEqual(bytes(factory.asr[1].pcm), source)
            self.assertEqual([provider.recognitions for provider in factory.asr], [1, 1])
            later_audio = [item for item in self.observed[acknowledgement_boundary + 1:]
                           if isinstance(item, AudioPacket)]
            self.assertTrue(later_audio)
            self.assertTrue(all(item.turn_id == 2 and item.generation != old.generation
                                for item in later_audio))
            self.assertEqual(sum(isinstance(item, dict) and item.get("type") == "response.cancelled"
                                 for item in self.observed), 1)
        finally:
            cleanup_gate.set()
            if receive_cancel is not None and not receive_cancel.done():
                receive_cancel.cancel()
                try:
                    await receive_cancel
                except asyncio.CancelledError:
                    pass

    async def test_response_cancel_duplicate_ack_and_old_target_leave_new_turn_intact(self):
        factory = SimulatedProviders(hold_first_response=True)
        second_gate = asyncio.Event()

        def providers(kind, *args):
            provider = factory(kind, *args)
            if kind == "tts" and len(factory.tts) == 2:
                provider.gate = second_gate
            return provider

        self.providers = providers
        client = await self.make_client()
        ws, ready = await self.connect(client)
        epoch = ready["session_epoch"]
        await self.start_stream(ws, epoch)
        await self.start_turn(ws, epoch)
        await self.send_frames(ws, epoch, [tone(16000, 320)] * 4)
        await ws.send_json({"type": "speech.end", "session_epoch": epoch,
                            "turn_id": 1, "sample_offset": 1280})
        while not isinstance(old := await self.receive(ws), AudioPacket):
            pass
        cancel = {"type": "response.cancel", "session_epoch": epoch,
                  "turn_id": old.turn_id, "generation": old.generation}
        await ws.send_json(cancel)
        first_ack = await self.until(ws, "response.cancelled")
        await ws.send_json(cancel)
        second_ack = await self.until(ws, "response.cancelled")
        self.assertEqual(second_ack, first_ack)
        await self.start_turn(ws, epoch, turn=2)
        await self.send_frames(ws, epoch, [tone(16000, 320)] * 4, start_sequence=4)
        await ws.send_json({"type": "speech.end", "session_epoch": epoch,
                            "turn_id": 2, "sample_offset": 2560})
        while not isinstance(new := await self.receive(ws), AudioPacket):
            pass
        # 当较新的回答仍在运行时，针对过期目标的请求既不能取消
        # 新一代回答，也不能为旧目标伪造确认消息。
        boundary = len(self.observed)
        try:
            await ws.send_json(cancel)
            await ws.send_json({"type": "response.cancel", "session_epoch": epoch,
                                "turn_id": new.turn_id, "generation": new.generation + 1})
            await asyncio.sleep(0.03)
            self.assertFalse(factory.tts[1].cancelled)
            self.assertTrue(factory.llm[1].closed)
            self.assertFalse(factory.tts[1].closed)
            second_gate.set()
            done = await self.until(ws, "response.done")
            self.assertEqual((done["turn_id"], done["reason"]), (2, "complete"))
            later = self.observed[boundary:]
            self.assertFalse(any(isinstance(item, dict) and item.get("type") == "response.cancelled"
                                 for item in later))
            await self.start_turn(ws, epoch, turn=3)
            self.assertEqual(len(factory.asr), 3)
            self.assertEqual([provider.recognitions for provider in factory.asr], [1, 1, 0])
        finally:
            second_gate.set()

    async def test_stalled_upstream_closes_bounded_uplink_and_releases_provider(self):
        gate, closed = asyncio.Event(), asyncio.Event()
        factory = self.providers

        def providers(kind, *args):
            provider = factory(kind, *args)
            if kind == "asr":
                async def blocked_feed(pcm):
                    await gate.wait()

                async def close():
                    provider.closed = True
                    closed.set()

                provider.feed_audio, provider.close = blocked_feed, close
            return provider

        self.providers = providers
        client = await self.make_client()
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            ws, ready = await self.connect(client)
            epoch = ready["session_epoch"]
            await self.start_stream(ws, epoch)
            await self.start_turn(ws, epoch)
            for sequence in range(40):
                await ws.send_bytes(AudioPacket(UPLINK, epoch, 1, 0, 0, sequence, sequence * 320, bytes(640)).pack())
            self.assertEqual((await self.until(ws, "error"))["code"], "uplink_backlog")
            await ws.receive(timeout=3)
            self.assertEqual(ws.close_code, 1008)
            await asyncio.wait_for(closed.wait(), 3)
        output = "\n".join(logs.output)
        self.assertIn("category=uplink_backlog", output)
        self.assertIn("queue=25 queue_peak=25", output)
        self.assertIn("queue_wait_ms=", output)
        self.assertIn("feed_inflight_ms=", output)
        self.assertEqual(output.count("first=1"), 1)
        self.assertTrue(factory.asr[0].closed)

    async def test_unpaced_delivery_burst_completes_without_dropping_audio(self):
        client = await self.make_client()
        ws, ready = await self.connect(client)
        epoch = ready["session_epoch"]
        await self.start_stream(ws, epoch)
        await self.start_turn(ws, epoch)
        payloads = [tone(16000, 320, index * 320) for index in range(64)]
        # 故意省略模拟器平时每帧的等待：这里模拟多次已缓存的网络交付，
        # 不是源端缓慢生成音频。
        for sequence, pcm in enumerate(payloads):
            await ws.send_bytes(AudioPacket(UPLINK, epoch, 1, 0, 0, sequence,
                                           sequence * 320, pcm).pack())
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch,
                            "stream_id": 1, "valid_end_sample": 64 * 320})
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1})
        await self.until(ws, "response.done")
        self.assertEqual(bytes(self.providers.asr[0].pcm), b"".join(payloads))
        self.assertFalse(ws.closed)

    async def test_queue_diagnostics_separate_wait_decode_and_running_feed(self):
        selected = SimpleNamespace(transport={"uplink": {"codec": "pcm_s16le"},
                                              "downlink": {"codec": "pcm_s16le"}})
        connection = DeviceConnection(SimpleNamespace(), None, {}, selected, None,
                                      provider_factory=self.providers)
        gate, feeding = asyncio.Event(), asyncio.Event()
        connection.uplink = SimpleNamespace(id=1, feed=lambda packet: bytes(640))

        async def decode(function, value):
            await asyncio.sleep(0.02)
            return function(value)

        async def feed(pcm):
            feeding.set()
            await gate.wait()

        connection.pipeline.feed_audio = feed
        await connection._enqueue(("audio", object()))
        await asyncio.sleep(0.02)
        with patch("transport.voice_websocket.codec_call", side_effect=decode):
            worker = asyncio.create_task(connection._work())
            try:
                await asyncio.wait_for(feeding.wait(), 1)
                await asyncio.sleep(0.02)
                metrics = connection._diagnostic_values()
                self.assertGreaterEqual(metrics["queue_wait_ms"], 10)
                self.assertGreaterEqual(metrics["decode_ms"], 10)
                self.assertGreaterEqual(metrics["feed_inflight_ms"], 10)
                self.assertEqual(metrics["feed_max_ms"], 0, "Running calls are separate from completed maxima")
                with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
                    connection.record_failure("device_receive", UplinkBacklogError())
                self.assertIn("feed_inflight_ms=", logs.output[0])
                gate.set()
                await asyncio.wait_for(connection.queue.join(), 1)
                completed = connection._diagnostic_values()
                self.assertEqual(completed["feed_inflight_ms"], 0)
                self.assertGreaterEqual(completed["feed_max_ms"], 10)
            finally:
                worker.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await worker

    async def test_audio_arrival_metrics_distinguish_gap_then_burst(self):
        selected = SimpleNamespace(transport={"uplink": {"codec": "pcm_s16le"},
                                              "downlink": {"codec": "pcm_s16le"}})
        connection = DeviceConnection(SimpleNamespace(), None, {}, selected, None,
                                      provider_factory=self.providers)
        for when in (1.0, 1.02, 1.30, 1.301, 1.302):
            connection._record_audio_arrival(when)
        metrics = connection._diagnostic_values()
        self.assertAlmostEqual(metrics["receive_gap_ms"], 1)
        self.assertAlmostEqual(metrics["receive_gap_max_ms"], 280)
        self.assertEqual(metrics["receive_burst"], 3)
        self.assertEqual(metrics["receive_burst_peak"], 3)
        self.assertEqual(metrics["receive_frames"], 5)

    async def test_loop_probe_records_blocked_event_loop(self):
        selected = SimpleNamespace(transport={"uplink": {"codec": "pcm_s16le"},
                                              "downlink": {"codec": "pcm_s16le"}})
        connection = DeviceConnection(SimpleNamespace(), None, {}, selected, None,
                                      provider_factory=self.providers)
        probe = asyncio.create_task(connection._monitor_loop(interval=0.01))
        try:
            await asyncio.sleep(0)  # 让探针先建立第一个截止时间。
            time.sleep(0.06)  # 模拟同步回调阻塞 asyncio。
            self.assertGreater(connection._diagnostic_values()["loop_lag_max_ms"], 20)
            await asyncio.sleep(0.02)
            self.assertGreater(connection._diagnostic_values()["loop_lag_max_ms"], 20)
        finally:
            probe.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await probe

    async def test_idle_timeout_is_normal_close_and_stops_probe(self):
        selected = SimpleNamespace(transport={"idle_timeout_seconds": 1, "uplink": {"codec": "pcm_s16le"},
                                              "downlink": {"codec": "pcm_s16le"}}, to_dict=lambda: {})
        ws = SimpleNamespace(closed=False, send_json=AsyncMock(), receive=AsyncMock(), close=AsyncMock())
        connection = DeviceConnection(ws, None, {}, selected, None, provider_factory=self.providers)

        async def idle_wait(*args, **kwargs):
            connection.last_activity -= 2
            return set(), set()

        with patch("transport.voice_websocket.asyncio.wait", side_effect=idle_wait):
            with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
                await connection.run({})
        self.assertIsNone(connection.first_failure)
        self.assertIn("voice_session_idle", logs.output[0])
        event = ws.send_json.await_args_list[-1].args[0]
        self.assertEqual((event["type"], event["reason"]), ("session.closed", "idle_timeout"))
        self.assertEqual(event["session_epoch"], connection.pipeline.session.epoch)
        ws.close.assert_awaited_once_with(code=1000, message=b"Voice session idle")
        self.assertTrue(connection._loop_probe.done())
        self.assertTrue(connection.worker.done())

    async def test_error_events_latch_first_failure_and_redact_later_exception_text(self):
        # ASR 事件读取任务可能在 feed_audio 抛错前先报告失败；其
        # 首份诊断必须保留，不能被后续发送或清理异常覆盖。
        selected = SimpleNamespace(transport={"uplink": {"codec": "pcm_s16le"},
                                              "downlink": {"codec": "pcm_s16le"}})
        ws = SimpleNamespace(send_json=AsyncMock())
        connection = DeviceConnection(ws, None, {}, selected, None, provider_factory=self.providers)
        secret = "upstream-secret-key-audio-and-address"
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            await connection.send_json({"type": "error", "code": "asr_events_failed"})
            first = dict(connection.first_failure)
            connection.record_failure("asr_feed", RuntimeError(secret))
            connection.record_failure("session_route", UplinkBacklogError(secret))
        self.assertEqual(connection.first_failure, first)
        output = "\n".join(logs.output)
        self.assertEqual(output.count("first=1"), 1)
        self.assertIn("category=asr_events_failed", output)
        self.assertIn("first=0", output)
        self.assertNotIn(secret, output)


if __name__ == "__main__":
    unittest.main()
