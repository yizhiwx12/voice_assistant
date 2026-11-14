"""通过本机 WebSocket 验证 PC 验收客户端，不访问云端。"""

import asyncio
import importlib.util
import json
import tempfile
import unittest
import wave
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from aiohttp import web
from aiohttp.test_utils import TestServer

from audio.opus_codec import native_version
from core.config import load_voice_config, select_profile
from tests.test_voice_websocket import SimulatedProviders, tone
from transport.voice_protocol import AudioPacket, DOWNLINK
import voice_server


CLIENT_PATH = Path(__file__).resolve().parents[2] / "tools" / "voice_client.py"
SPEC = importlib.util.spec_from_file_location("acceptance_voice_client", CLIENT_PATH)
client_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(client_tool)


class VoiceClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.input = self.root / "input.wav"
        with wave.open(str(self.input), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(tone(16000, 711))

    async def start_server(self, app):
        server = TestServer(app)
        await server.start_server()
        self.addAsyncCleanup(server.close)
        return str(server.make_url("/ws/voice")).replace("http://", "ws://", 1)

    async def make_server(self, providers):
        config = load_voice_config(Path(__file__).parents[1] / "config" / "voice_config.example.json", environ={})
        selected = select_profile(config, profile="realtime_default", mode="realtime")
        credentials = {name: "synthetic-test-value" for name in (
            "DASHSCOPE_API_KEY", "DASHSCOPE_WORKSPACE_ID", "DEEPSEEK_API_KEY",
            "VOLCENGINE_ASR_API_KEY", "VOLCENGINE_TTS_API_KEY",
        )}
        app = voice_server.create_realtime_app(config, credentials, selected,
                                                provider_factory=providers)
        return await self.start_server(app)

    def args(self, url, *, count=1, codec="pcm_s16le", extra=()):
        return client_tool.argument_parser().parse_args([
            *([str(self.input)] * count), "--url", url, "--codec", codec,
            "--output", str(self.root / "reply.wav"), *extra,
        ])

    async def execute(self, args):
        with redirect_stdout(StringIO()):
            return await client_tool.run(args)

    def test_selection_report_keeps_identifiers_and_excludes_arbitrary_fields(self):
        value = {"profile": "realtime_default", "endpoint": "wss://secret.invalid",
                 "asr": {"id": "doubao_2", "adapter": "doubao_streaming", "model": "bigmodel",
                         "resource_id": "volc.seedasr.sauc.duration", "credential": "secret",
                         "endpoint": "wss://secret.invalid"},
                 "llm": {"model": {"token": "secret"}, "id": "deepseek_default"},
                 "tts": {"voice": "https://secret.invalid", "adapter": "volcengine_bidirectional"}}
        result = client_tool.safe_selection(value)
        self.assertEqual(result["profile"], "realtime_default")
        self.assertEqual(result["asr"]["resource_id"], "volc.seedasr.sauc.duration")
        self.assertEqual(result["llm"], {"id": "deepseek_default"})
        self.assertEqual(result["tts"], {"adapter": "volcengine_bidirectional"})
        self.assertNotIn("secret", json.dumps(result))

    async def test_two_turns_one_connection_preserves_pcm_tail_and_metrics(self):
        providers = SimulatedProviders()
        url = await self.make_server(providers)
        result = await self.execute(self.args(url, count=2))
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["selection"]["profile"], "realtime_default")
        self.assertEqual(result["selection"]["asr"]["id"], "doubao_2")
        self.assertEqual([turn["turn_id"] for turn in result["turns"]], [1, 2])
        self.assertEqual(len(providers.asr), 2)
        for index, turn in enumerate(result["turns"], 1):
            self.assertEqual(turn["status"], "complete")
            self.assertEqual(turn["asr_text"], "你好")
            self.assertEqual(turn["answer_text"], "第一句。第二句。")
            self.assertEqual(len(providers.asr[index - 1].pcm), 711 * 2)
            self.assertIn("input_commit_to_asr_final", turn["latencies_ms"])
            self.assertLessEqual(turn["timestamps_ms"]["speech_started"],
                                 turn["timestamps_ms"]["uplink_first_packet_sent"])
            uplink = turn["uplink"]
            self.assertEqual(uplink, {"packets": 3, "payload_bytes": 1920, "wire_bytes": 2016})
            with wave.open(str(self.root / f"reply_turn{index:02d}.wav"), "rb") as wav:
                self.assertEqual(wav.getframerate(), 24000)
                self.assertEqual(wav.readframes(wav.getnframes()), bytes(providers.tts[index - 1].pcm))
        persisted = json.loads((self.root / "reply.json").read_text(encoding="utf-8"))
        self.assertFalse(persisted["playback_performed"])
        self.assertFalse(persisted["playback_acknowledged"])
        self.assertNotIn(url, json.dumps(persisted))

    async def test_native_opus_cancel_after_audio_then_next_turn_recovers(self):
        native_version()  # 项目运行环境必须提供原生 Opus 库。
        providers = SimulatedProviders(hold_first_response=True)
        url = await self.make_server(providers)
        result = await self.execute(self.args(url, count=2, codec="opus", extra=(
            "--cancel-turn", "1", "--cancel-trigger", "first-audio", "--cancel-after-ms", "0",
        )))
        first, second = result["turns"]
        self.assertEqual((first["status"], second["status"]), ("cancelled", "complete"))
        self.assertIn("cancel_request_to_ack", first["latencies_ms"])
        self.assertIn("downlink_first_packet", first["timestamps_ms"])
        self.assertTrue(providers.tts[0].cancelled)
        self.assertEqual(second["output_samples_24000_hz"], len(providers.tts[1].pcm) // 2)
        self.assertEqual([len(asr.pcm) for asr in providers.asr], [1422, 1422])

    async def test_tts_and_llm_errors_fail_and_persist_safe_report(self):
        for failure in ("tts", "llm"):
            with self.subTest(failure=failure):
                providers = SimulatedProviders()

                def factory(kind, entry, credentials, session):
                    provider = providers(kind, entry, credentials, session)
                    if kind == failure:
                        async def broken(*args):
                            raise RuntimeError("secret-token https://secret.invalid/path")
                        if kind == "tts":
                            provider.start = broken
                        else:
                            async def broken_stream(*args):
                                await broken()
                                yield "unreachable"
                            provider.stream = broken_stream
                    return provider

                url = await self.make_server(factory)
                with self.assertRaises(RuntimeError):
                    await self.execute(self.args(url))
                raw = (self.root / "reply.json").read_text(encoding="utf-8")
                result = json.loads(raw)
                self.assertEqual(result["status"], "failed")
                self.assertEqual(result["turns"][0]["status"], "failed")
                self.assertNotIn("secret", raw)
                self.assertNotIn(url, raw)

    async def test_late_cancelled_packets_never_decode_or_complete_next_turn(self):
        observed = []
        epoch = 104

        async def route(request):
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            await ws.receive_json()
            await ws.send_json({"type": "session.ready", "session_epoch": epoch,
                                "uplink_codec": "pcm_s16le", "downlink_codec": "pcm_s16le"})
            for turn in (1, 2):
                event = await ws.receive_json()
                observed.append(event)
                self.assertEqual((event["type"], event["turn_id"]), ("speech.start", turn))
                if turn == 1:
                    # 云端握手通过 speech.started 确认之前，
                    # 不能收到音频流或音频数据。
                    with self.assertRaises(TimeoutError):
                        await ws.receive(timeout=0.03)
                await ws.send_json({"type": "speech.started", "session_epoch": epoch,
                                    "turn_id": turn, "generation": turn})
                while True:
                    message = await ws.receive()
                    if message.type == web.WSMsgType.TEXT:
                        event = json.loads(message.data)
                        observed.append(event)
                        if event["type"] == "speech.end":
                            break
                ident = {"session_epoch": epoch, "turn_id": turn, "generation": turn}
                await ws.send_json({"type": "asr.utterance_final", **ident, "text": "问题"})
                await ws.send_json({"type": "response.started", **ident})
                await ws.send_json({"type": "llm.delta", **ident, "text": "回答。"})
                await ws.send_json({"type": "audio.stream.start", **ident, "stream_id": 100 + turn,
                                    "codec": "pcm_s16le", "sample_rate_hz": 24000,
                                    "channels": 1, "frame_duration_ms": 20, "pre_skip_samples": 0})
                await ws.send_bytes(AudioPacket(DOWNLINK, epoch, 100 + turn, turn, turn,
                                                0, 0, bytes(960)).pack())
                if turn == 1:
                    cancel = await ws.receive_json()
                    observed.append(cancel)
                    self.assertEqual(cancel["type"], "response.cancel")
                    # 此载荷故意使用无效 PCM；若它到达解码器或
                    # 音频流校验器，客户端就会失败。
                    late = AudioPacket(DOWNLINK, epoch, 101, 1, 1, 99, 999, b"x").pack()
                    await ws.send_bytes(late)
                    await ws.send_json({"type": "response.cancelled", **ident})
                    await ws.send_json({"type": "response.cancelled", **ident})
                    await ws.send_bytes(late)
                else:
                    await ws.send_bytes(AudioPacket(DOWNLINK, epoch, 102, 2, 2,
                                                    1, 480, bytes(960)).pack())
                    await ws.send_json({"type": "audio.stream.end", **ident,
                                        "stream_id": 102, "valid_end_sample": 711})
                    await ws.send_json({"type": "response.done", **ident, "reason": "complete"})
            await ws.close()
            return ws

        app = web.Application()
        app.router.add_get("/ws/voice", route)
        url = await self.start_server(app)
        result = await self.execute(self.args(url, count=2, extra=(
            "--cancel-trigger", "first-audio", "--cancel-after-ms", "0",
        )))
        first, second = result["turns"]
        self.assertEqual(first["downlink"]["discarded_cancelled_packets"], 1)
        self.assertEqual(second["downlink"]["late_previous_turn_packets"], 1)
        self.assertEqual(second["downlink"]["packets"], 2)
        self.assertEqual(second["output_samples_24000_hz"], 711)
        self.assertTrue(all(item["type"] != "playback.done" for item in observed))
        starts = [item for item in observed if item["type"] == "audio.stream.start"]
        self.assertEqual([item["stream_id"] for item in starts], [1, 2])

    async def test_malformed_complete_never_passes_acceptance(self):
        for missing in ("asr_text", "answer_text", "returned_audio", "audio_stream_end", "unknown_reason"):
            with self.subTest(missing=missing):
                async def route(request):
                    ws = web.WebSocketResponse()
                    await ws.prepare(request)
                    await ws.receive_json()
                    epoch = 105
                    ident = {"session_epoch": epoch, "turn_id": 1, "generation": 1}
                    await ws.send_json({"type": "session.ready", "session_epoch": epoch,
                                        "uplink_codec": "pcm_s16le", "downlink_codec": "pcm_s16le"})
                    await ws.receive_json()
                    await ws.send_json({"type": "speech.started", **ident})
                    while True:
                        message = await ws.receive()
                        if message.type == web.WSMsgType.TEXT and json.loads(message.data)["type"] == "speech.end":
                            break
                    await ws.send_json({"type": "asr.utterance_final", **ident,
                                        "text": " " if missing == "asr_text" else "问题"})
                    await ws.send_json({"type": "response.started", **ident})
                    await ws.send_json({"type": "llm.delta", **ident,
                                        "text": " " if missing == "answer_text" else "回答。"})
                    await ws.send_json({"type": "audio.stream.start", **ident, "stream_id": 101,
                                        "codec": "pcm_s16le", "sample_rate_hz": 24000,
                                        "channels": 1, "frame_duration_ms": 20, "pre_skip_samples": 0})
                    if missing != "returned_audio":
                        await ws.send_bytes(AudioPacket(DOWNLINK, epoch, 101, 1, 1,
                                                        0, 0, bytes(960)).pack())
                    if missing != "audio_stream_end":
                        await ws.send_json({"type": "audio.stream.end", **ident, "stream_id": 101,
                                            "valid_end_sample": 0 if missing == "returned_audio" else 480})
                    await ws.send_json({"type": "response.done", **ident,
                                        "reason": "unexpected" if missing == "unknown_reason" else "complete"})
                    await ws.close()
                    return ws

                app = web.Application()
                app.router.add_get("/ws/voice", route)
                url = await self.start_server(app)
                with self.assertRaises(RuntimeError):
                    await self.execute(self.args(url))
                result = json.loads((self.root / "reply.json").read_text(encoding="utf-8"))
                self.assertEqual(result["status"], "failed")
                turn = result["turns"][0]
                self.assertEqual(turn["status"], "failed")
                if missing == "unknown_reason":
                    self.assertEqual(turn["reason"], "unknown_response_reason")
                else:
                    self.assertEqual(turn["reason"], "incomplete_response")
                    self.assertIn(missing, turn["missing"])


if __name__ == "__main__":
    unittest.main()
