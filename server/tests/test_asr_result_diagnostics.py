"""追踪静音、最终识别结果与解码输入，不导出内容。"""

import asyncio
import struct
import unittest
from contextlib import suppress
from types import SimpleNamespace
from unittest.mock import AsyncMock

from audio.levels import PcmLevelMeter
from audio.opus_codec import Encoder
from tests.test_pipeline import selection
from tests.test_voice_silent_utterance import SilentUtteranceHarness
from transport.voice_protocol import AudioPacket, UPLINK
from transport.voice_websocket import DeviceConnection


class LevelMeterTests(unittest.TestCase):
    def test_empty_and_dc_are_distinct_from_varying_audio(self):
        meter = PcmLevelMeter()
        meter.feed(b"")
        self.assertEqual(set(meter.metrics().values()), {0})
        meter.feed(struct.pack("<4h", *([1000] * 4)))
        self.assertEqual(meter.metrics()["pcm_rms"], 1000)
        self.assertEqual(meter.metrics()["pcm_ac_rms"], 0)
        meter = PcmLevelMeter()
        meter.feed(struct.pack("<4h", 1000, -1000, 1000, -1000))
        self.assertEqual(meter.metrics()["pcm_rms"], 1000)
        self.assertEqual(meter.metrics()["pcm_ac_rms"], 1000)

    def test_little_endian_clipping_and_chunked_aggregation(self):
        pcm = struct.pack("<6h", -32768, 32767, -2000, 2000, 0, 0)
        whole, chunks = PcmLevelMeter(), PcmLevelMeter()
        whole.feed(pcm)
        chunks.feed(pcm[:4])
        chunks.feed(pcm[4:])
        for key in ("pcm_samples", "pcm_rms", "pcm_ac_rms", "pcm_peak", "pcm_clipped"):
            self.assertEqual(whole.metrics()[key], chunks.metrics()[key])
        self.assertEqual(whole.metrics()["pcm_clipped"], 2)
        self.assertEqual(whole.metrics()["pcm_peak"], 32768)
        with self.assertRaises(ValueError):
            whole.feed(b"x")


class AsrResultDiagnosticsTests(unittest.IsolatedAsyncioTestCase):
    async def connection(self, final_texts):
        harness = SilentUtteranceHarness(final_texts, timeout=0.1)
        selected = selection()
        selected.transport = {"uplink": {"codec": "pcm_s16le"},
                              "downlink": {"codec": "pcm_s16le"}}
        ws = SimpleNamespace(send_json=AsyncMock(), close=AsyncMock())
        connection = DeviceConnection(ws, None, {}, selected, None, provider_factory=harness.factory)

        async def consume_audio(source, identity):
            async for _ in source:
                pass

        connection.pipeline.send_audio = consume_audio
        connection.worker = asyncio.create_task(connection._work())

        async def cleanup():
            connection.worker.cancel()
            with suppress(asyncio.CancelledError):
                await connection.worker
            await connection.pipeline.close()
            if connection.uplink:
                connection.uplink.close()

        self.addAsyncCleanup(cleanup)
        return connection, harness

    async def admit(self, connection, item):
        await connection._enqueue(item)
        await asyncio.wait_for(connection.queue.join(), 1)

    async def record(self, connection, turn, pcm, *, codec="pcm_s16le", continuous_end=False):
        await self.admit(connection, ("start", turn))
        if codec == "opus":
            encoder = Encoder(16000, 24000)
            self.addCleanup(encoder.close)
            payloads = encoder.feed(pcm) + encoder.finish()
            pre_skip = encoder.pre_skip
        else:
            pre_skip = 0
            padded = pcm + bytes((-len(pcm)) % 640)
            payloads = [padded[i:i + 640] for i in range(0, len(padded), 640)]
        await self.admit(connection, ("stream_start", {
            "stream_id": turn, "codec": codec, "sample_rate_hz": 16000, "channels": 1,
            "frame_duration_ms": 20, "pre_skip_samples": pre_skip,
        }))
        for index, payload in enumerate(payloads):
            packet = AudioPacket(UPLINK, connection.pipeline.session.epoch, turn, turn,
                                 0, index, index * 320, payload)
            await self.admit(connection, ("audio", packet))
        valid_end = pre_skip + len(pcm) // 2
        if not continuous_end:
            await self.admit(connection, ("stream_end", {"stream_id": turn, "valid_end_sample": valid_end}))
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            await self.admit(connection, ("end", {"turn_id": turn, "sample_offset": valid_end}))
            await asyncio.wait_for(connection.finish_task, 1)
        if connection.pipeline.response_task:
            await asyncio.wait_for(connection.pipeline.response_task, 1)
        return "\n".join(logs.output)

    async def test_empty_final_is_logged_and_skips_llm_without_retaining_text(self):
        connection, harness = await self.connection([""])
        log = await self.record(connection, 1, bytes(443 * 2))
        self.assertIn("outcome=empty final_chars=0", log)
        self.assertIn("device_partials=0 device_segments=0 device_finals=1", log)
        self.assertIn("provider_responses=2 provider_nonempty=0", log)
        self.assertIn("provider_final=1 provider_final_chars=0", log)
        self.assertIn("pcm_samples=443 pcm_rms=0 pcm_ac_rms=0 pcm_peak=0", log)
        self.assertEqual(harness.created, ["asr"])
        self.assertEqual(connection.ws.send_json.await_args.args[0]["reason"], "empty_utterance")

    async def test_each_turn_resets_levels_and_final_counters_without_logging_text(self):
        private_text = "private-transcript-never-log"
        connection, _ = await self.connection(["", private_text])
        await self.record(connection, 1, struct.pack("<640h", *([12000, -12000] * 320)))
        log = await self.record(connection, 2, struct.pack("<443h", *([1000] * 443)), continuous_end=True)
        self.assertIn("turn=2 outcome=text final_chars=28", log)
        self.assertIn("device_partials=0 device_segments=0 device_finals=1", log)
        self.assertIn("pcm_samples=443 pcm_rms=1000 pcm_ac_rms=0 pcm_peak=1000", log)
        self.assertNotIn(private_text, log)

    async def test_opus_levels_count_only_valid_decoded_samples_after_lookahead(self):
        connection, _ = await self.connection([""])
        connection.uplink_codec = "opus"
        pcm = struct.pack("<653h", *([6000, -6000] * 326 + [6000]))
        log = await self.record(connection, 1, pcm, codec="opus")
        self.assertIn("pcm_samples=653", log)
        self.assertGreater(connection._pcm_levels.metrics()["pcm_ac_rms"], 100)

    async def test_missing_final_is_failed_instead_of_empty(self):
        connection, _ = await self.connection([None])
        log = await self.record(connection, 1, bytes(640))
        self.assertIn("outcome=failed final_chars=0", log)
        self.assertIn("device_partials=0 device_segments=0 device_finals=0", log)
        self.assertIn("provider_final=0 provider_final_chars=0", log)
        connection.ws.close.assert_awaited_once()

    async def test_failed_device_write_is_not_counted_as_delivered_final(self):
        connection, _ = await self.connection([""])

        async def send(event):
            if event["type"] == "asr.utterance_final":
                raise OSError("private-socket-error")

        connection.ws.send_json.side_effect = send
        log = await self.record(connection, 1, bytes(640))
        self.assertIn("outcome=failed final_chars=0", log)
        self.assertIn("device_finals=0", log)
        self.assertIn("provider_final=1 provider_final_chars=0", log)
        self.assertNotIn("private-socket-error", log)


if __name__ == "__main__":
    unittest.main()
