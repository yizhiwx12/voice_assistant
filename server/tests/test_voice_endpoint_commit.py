"""通过真实业务路由的本机回环检查端点、手动结束与取消的竞态。"""

import asyncio
import unittest
from unittest.mock import patch

from transport.voice_protocol import AudioPacket, UPLINK
from tests import test_voice_websocket as device


@unittest.skipUnless(device.HAS_AIOHTTP, "Install server/requirements.txt for loopback tests")
class EndpointCommitTests(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = device.VoiceWebSocketTests.asyncSetUp
    make_client = device.VoiceWebSocketTests.make_client
    connect = device.VoiceWebSocketTests.connect
    receive = device.VoiceWebSocketTests.receive
    until = device.VoiceWebSocketTests.until
    start_stream = device.VoiceWebSocketTests.start_stream
    start_turn = device.VoiceWebSocketTests.start_turn
    send_frames = device.VoiceWebSocketTests.send_frames

    async def end(self, ws, epoch, turn):
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch,
                            "stream_id": turn, "valid_end_sample": 640})
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": turn})

    async def next_spoken_turn(self, ws, epoch, turn=2):
        await self.start_turn(ws, epoch, turn)
        await self.start_stream(ws, epoch, stream_id=turn)
        await self.send_frames(ws, epoch, [device.tone(16000, 320)] * 2, stream_id=turn)
        await self.end(ws, epoch, turn)
        done = await self.until(ws, "response.done")
        self.assertEqual((done["turn_id"], done["reason"]), (turn, "complete"))

    def delay_first_provider(self, operation):
        factory = self.providers
        entered, gate = asyncio.Event(), asyncio.Event()

        def create(kind, *args):
            provider = factory(kind, *args)
            if kind == "asr" and len(factory.asr) == 1:
                original = getattr(provider, operation)

                async def delayed(*values):
                    entered.set()
                    await gate.wait()
                    return await original(*values)

                setattr(provider, operation, delayed)
            return provider

        self.providers = create
        return factory, entered, gate

    async def test_competing_endpoints_and_late_old_end_submit_each_turn_once(self):
        providers, entered, gate = self.delay_first_provider("end_utterance")
        ws, ready = await self.connect(await self.make_client())
        epoch = ready["session_epoch"]
        await self.start_turn(ws, epoch)
        await self.start_stream(ws, epoch)
        await self.send_frames(ws, epoch, [device.tone(16000, 320)] * 2)
        await self.end(ws, epoch, 1)
        await asyncio.wait_for(entered.wait(), 1)
        # 本地 VAD、手动 s 命令和时长保护可能在云端尚未返回
        # 最终结果时同时触发，但它们共用同一个最终提交流程。
        for _ in range(3):
            await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1})
        self.assertEqual(providers.llm, [])
        gate.set()
        await self.until(ws, "response.done")
        await self.start_turn(ws, epoch, 2)
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1})
        await self.start_stream(ws, epoch, stream_id=2)
        await self.send_frames(ws, epoch, [device.tone(16000, 320)] * 2, stream_id=2)
        await self.end(ws, epoch, 2)
        await self.until(ws, "response.done")
        self.assertEqual([item.recognitions for item in providers.asr], [1, 1])
        self.assertEqual([item.calls for item in providers.llm], [1, 1])
        self.assertEqual(sum(isinstance(item, dict) and item.get("type") == "asr.utterance_final"
                             for item in self.observed), 2)
        self.assertFalse(ws.closed)

    async def test_cancel_during_handshake_fences_late_stream_and_allows_next_turn(self):
        providers, entered, _ = self.delay_first_provider("start")
        ws, ready = await self.connect(await self.make_client())
        epoch = ready["session_epoch"]
        await ws.send_json({"type": "speech.start", "session_epoch": epoch, "turn_id": 1})
        await asyncio.wait_for(entered.wait(), 1)
        await ws.send_json({"type": "speech.cancel", "session_epoch": epoch, "turn_id": 1})
        acknowledgement = await self.until(ws, "speech.cancelled")
        self.assertEqual(acknowledgement["turn_id"], 1)
        self.assertTrue(providers.asr[0].closed)
        self.assertEqual(providers.asr[0].recognitions, 0)
        self.assertEqual(providers.llm, [])
        # 延迟到达的音频流开始消息及其数据包不能恢复已取消的输入。
        await self.start_stream(ws, epoch)
        await ws.send_bytes(AudioPacket(UPLINK, epoch, 1, 0, 0, 0, 0, bytes(640)).pack())
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch,
                            "stream_id": 1, "valid_end_sample": 320})
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": 1})
        await self.next_spoken_turn(ws, epoch)
        self.assertEqual([item.recognitions for item in providers.asr], [0, 1])
        self.assertEqual(sum(item.calls for item in providers.llm), 1)
        self.assertFalse(any(isinstance(item, dict) and item.get("turn_id") == 1 and
                             item.get("type") in ("speech.started", "asr.utterance_final", "response.done")
                             for item in self.observed))

    async def test_cancel_pending_final_does_not_commit_and_duplicate_is_idempotent(self):
        providers, entered, _ = self.delay_first_provider("end_utterance")
        ws, ready = await self.connect(await self.make_client())
        epoch = ready["session_epoch"]
        await self.start_turn(ws, epoch)
        await self.start_stream(ws, epoch)
        await self.send_frames(ws, epoch, [device.tone(16000, 320)] * 2)
        await self.end(ws, epoch, 1)
        await asyncio.wait_for(entered.wait(), 1)
        cancel = {"type": "speech.cancel", "session_epoch": epoch, "turn_id": 1}
        await ws.send_json(cancel)
        await self.until(ws, "speech.cancelled")
        await ws.send_json(cancel)
        await self.until(ws, "speech.cancelled")
        self.assertTrue(providers.asr[0].closed)
        self.assertEqual(providers.llm, [])
        await self.next_spoken_turn(ws, epoch)
        self.assertEqual([item.recognitions for item in providers.asr], [0, 1])
        self.assertEqual(sum(item.calls for item in providers.llm), 1)
        self.assertFalse(any(isinstance(item, dict) and item.get("turn_id") == 1 and
                             item.get("type") in ("asr.utterance_final", "response.done")
                             for item in self.observed))

    async def test_cancel_seals_pending_stream_start_and_rejects_future_turn(self):
        providers, entered, _ = self.delay_first_provider("start")
        ws, ready = await self.connect(await self.make_client())
        epoch = ready["session_epoch"]
        await ws.send_json({"type": "speech.start", "session_epoch": epoch, "turn_id": 1})
        await asyncio.wait_for(entered.wait(), 1)
        await self.start_stream(ws, epoch)
        await ws.send_bytes(AudioPacket(UPLINK, epoch, 1, 0, 0, 0, 0, bytes(640)).pack())
        await ws.send_json({"type": "speech.cancel", "session_epoch": epoch, "turn_id": 1})
        await self.until(ws, "speech.cancelled")
        self.assertEqual(providers.asr[0].recognitions, 0)
        await self.next_spoken_turn(ws, epoch)
        self.assertEqual(sum(item.calls for item in providers.llm), 1)
        await ws.send_json({"type": "speech.cancel", "session_epoch": epoch, "turn_id": 3})
        self.assertEqual((await self.until(ws, "error"))["code"], "voice_session_failed")

    async def test_cancel_running_feed_discards_queue_and_old_cancel_spares_new_turn(self):
        providers, entered, _ = self.delay_first_provider("feed_audio")
        ws, ready = await self.connect(await self.make_client())
        epoch = ready["session_epoch"]
        await self.start_turn(ws, epoch)
        await self.start_stream(ws, epoch)
        await self.send_frames(ws, epoch, [device.tone(16000, 320)] * 8)
        await asyncio.wait_for(entered.wait(), 1)
        cancel = {"type": "speech.cancel", "session_epoch": epoch, "turn_id": 1}
        await ws.send_json(cancel)
        await self.until(ws, "speech.cancelled")
        self.assertTrue(providers.asr[0].closed)
        self.assertEqual(providers.asr[0].pcm, b"")
        await self.start_turn(ws, epoch, 2)
        await self.start_stream(ws, epoch)  # 旧轮次的开始消息在新轮次之后延迟到达。
        await ws.send_bytes(AudioPacket(UPLINK, epoch, 1, 0, 0, 99, 99 * 320, bytes(640)).pack())
        await self.start_stream(ws, epoch, stream_id=2)
        await ws.send_json(cancel)
        acknowledgement = await self.until(ws, "speech.cancelled")
        self.assertEqual(acknowledgement["turn_id"], 1)
        await self.send_frames(ws, epoch, [device.tone(16000, 320)] * 2, stream_id=2)
        await self.end(ws, epoch, 2)
        self.assertEqual((await self.until(ws, "response.done"))["turn_id"], 2)
        self.assertEqual([item.recognitions for item in providers.asr], [0, 1])
        self.assertEqual(sum(item.calls for item in providers.llm), 1)

    async def test_stalled_cancel_cleanup_is_a_bounded_real_failure(self):
        providers = self.providers
        closing = asyncio.Event()

        def create(kind, *args):
            provider = providers(kind, *args)
            if kind == "asr":
                async def close():
                    closing.set()
                    await asyncio.Event().wait()

                provider.close = close
            return provider

        self.providers = create
        ws, ready = await self.connect(await self.make_client())
        epoch = ready["session_epoch"]
        await self.start_turn(ws, epoch)
        with patch("transport.voice_websocket.CANCEL_CLEANUP_SECONDS", 0.05):
            await ws.send_json({"type": "speech.cancel", "session_epoch": epoch, "turn_id": 1})
            await asyncio.wait_for(closing.wait(), 1)
            error = await self.until(ws, "error")
        self.assertEqual(error["code"], "voice_session_failed")
        self.assertFalse(any(isinstance(item, dict) and item.get("type") == "speech.cancelled"
                             for item in self.observed))
        self.assertEqual(providers.asr[0].recognitions, 0)
        self.assertEqual(providers.llm, [])


if __name__ == "__main__":
    unittest.main()
