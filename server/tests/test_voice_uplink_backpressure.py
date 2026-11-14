"""验证突发输入的有界接纳及诊断范围，不调用网络或云端。"""

import asyncio
import unittest
from contextlib import suppress
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from transport.voice_websocket import DeviceConnection, UplinkBacklogError
from transport.voice_protocol import AudioPacket, UPLINK


class UplinkBackpressureTests(unittest.IsolatedAsyncioTestCase):
    def connection(self):
        selection = SimpleNamespace(transport={"uplink": {"codec": "pcm_s16le"},
                                               "downlink": {"codec": "pcm_s16le"}})
        connection = DeviceConnection(SimpleNamespace(), None, {}, selection, None)
        connection.uplink = SimpleNamespace(id=1, feed=lambda value: value)
        return connection

    async def stop_worker(self, connection):
        if connection.worker:
            connection.worker.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await connection.worker

    async def fill_queue(self, connection):
        for sequence in range(connection.queue.maxsize):
            await connection._enqueue(("audio", sequence))

    async def test_burst_larger_than_queue_drains_in_order_without_growing_storage(self):
        connection = self.connection()
        feeding, release = asyncio.Event(), asyncio.Event()
        received = []

        async def feed(value):
            if not received:
                feeding.set()
                await release.wait()
            received.append(value)
            # sleep(0) 提供任务公平调度，同时避开 Windows 约 16 ms 的定时器下限。
            await asyncio.sleep(0)

        connection.pipeline.feed_audio = feed
        # 本测试只验证按序接纳数据，解码器直接返回输入；
        # 真实执行器和编解码耗时由本地回环测试覆盖。
        async def decode(function, value):
            return function(value)

        decoder_patch = patch("transport.voice_websocket.codec_call", side_effect=decode)
        decoder_patch.start()
        self.addCleanup(decoder_patch.stop)
        connection.worker = asyncio.create_task(connection._work())
        self.addAsyncCleanup(self.stop_worker, connection)

        async def burst():
            for sequence in range(80):
                await connection._enqueue(("audio", sequence))

        sender = asyncio.create_task(burst())
        try:
            await asyncio.wait_for(feeding.wait(), 1)
            while not connection.queue.full():
                await asyncio.sleep(0)
            self.assertFalse(sender.done(), "A full queue must wait for a brief burst")
            release.set()
            await asyncio.wait_for(sender, 3)
            await asyncio.wait_for(connection.queue.join(), 3)
        finally:
            sender.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await sender
        self.assertEqual(received, list(range(80)))
        self.assertEqual(connection.queue_peak, 25)
        self.assertEqual(connection.queue.maxsize, 25)
        self.assertEqual(len(connection.queue.audio_times), 0)
        self.assertIsNone(connection.first_failure)

    async def test_stalled_admission_times_out_and_leaves_no_late_putter(self):
        connection = self.connection()
        await self.fill_queue(connection)
        with self.assertRaises(UplinkBacklogError):
            await asyncio.wait_for(connection._enqueue(("audio", 25)), 1)
        self.assertEqual(connection.queue.qsize(), 25)
        connection.queue.get_nowait()
        connection.queue.task_done()
        await asyncio.sleep(0)
        self.assertEqual(connection.queue.qsize(), 24)
        self.assertGreater(connection._diagnostic_values()["enqueue_wait_max_ms"], 0)

    async def test_worker_failure_unblocks_full_queue_and_preserves_original_error(self):
        connection = self.connection()
        await self.fill_queue(connection)
        fail_worker = asyncio.Event()

        async def worker():
            await fail_worker.wait()
            raise RuntimeError("synthetic worker failure")

        connection.worker = asyncio.create_task(worker())
        sender = asyncio.create_task(connection._enqueue(("audio", 25)))
        await asyncio.sleep(0)
        fail_worker.set()
        with self.assertRaisesRegex(RuntimeError, "synthetic worker failure"):
            await asyncio.wait_for(sender, 1)
        connection.queue.get_nowait()
        connection.queue.task_done()
        await asyncio.sleep(0)
        self.assertEqual(connection.queue.qsize(), 24)

    async def test_continuously_slow_consumer_hits_age_bound_despite_freed_slots(self):
        connection = self.connection()
        received = []

        async def decode(function, value):
            return function(value)

        async def feed(value):
            received.append(value)
            await asyncio.sleep(0.02)

        connection.pipeline.feed_audio = feed
        # 只缩短音频年龄上限：每次输入仍能在实际的 100 ms 接纳预算内腾出
        # 一个槽位，但累积落后最终仍必须失败。
        with patch("transport.voice_websocket.UPLINK_MAX_QUEUE_AGE_SECONDS", 0.08), \
                patch("transport.voice_websocket.codec_call", side_effect=decode):
            connection.worker = asyncio.create_task(connection._work())
            try:
                with self.assertRaises(UplinkBacklogError):
                    for sequence in range(80):
                        await connection._enqueue(("audio", sequence))
            finally:
                await self.stop_worker(connection)
        self.assertGreater(len(received), 1)
        self.assertLess(len(received), 80)
        self.assertLessEqual(connection.queue.qsize(), 25)

    async def test_25ms_service_of_20ms_media_hits_default_lag_limit_with_small_queue(self):
        connection = self.connection()
        clock = [0.0]
        connection._media_now = lambda: clock[0]
        received = []

        async def decode(function, packet):
            return function(packet)

        async def feed(packet):
            received.append(packet.sequence)
            # 受控的服务时钟可以避开 Windows 低精度等待定时器。
            # 源时间戳仍表示每 20 ms 产生一帧。
            clock[0] += 0.025

        connection.pipeline.feed_audio = feed
        with patch("transport.voice_websocket.codec_call", side_effect=decode):
            connection.worker = asyncio.create_task(connection._work())
            try:
                with self.assertRaises(UplinkBacklogError):
                    for sequence in range(220):
                        packet = AudioPacket(UPLINK, 1, 1, 0, 0, sequence,
                                             sequence * 320, b"synthetic")
                        await connection._enqueue(("audio", packet))
            finally:
                await self.stop_worker(connection)
        # 每次接纳都有及时释放的槽位，仅检查年龄和队列是否满会一直放行；
        # 媒体时钟则能识别这种持续漂移。
        self.assertGreater(len(received), 100)
        self.assertLess(len(received), 200)
        self.assertLess(connection.queue_peak, 25)
        self.assertGreaterEqual(connection._diagnostic_values()["turn_media_lag_ms"], 750)

    async def test_worker_media_limit_still_applies_when_producer_has_stopped(self):
        connection = self.connection()
        clock = [0.0]
        connection._media_now = lambda: clock[0]
        received = []

        async def decode(function, packet):
            return function(packet)

        async def feed(packet):
            received.append(packet.sequence)
            clock[0] += 0.8

        for sequence in range(2):
            await connection._enqueue(("audio", AudioPacket(UPLINK, 1, 1, 0, 0,
                sequence, sequence * 320, b"synthetic")))
        connection.pipeline.feed_audio = feed
        with patch("transport.voice_websocket.codec_call", side_effect=decode):
            with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
                with self.assertRaises(UplinkBacklogError):
                    await connection._work()
        self.assertEqual(received, [0])
        self.assertIn("stage=media_lag", logs.output[0])
        self.assertGreaterEqual(connection._diagnostic_values()["worker_media_lag_ms"], 750)

    async def test_turn_baseline_reset_ignores_standby_and_queued_items_keep_old_baseline(self):
        connection = self.connection()
        clock = [1.0]
        connection._media_now = lambda: clock[0]
        await connection._enqueue(("start", 1))
        first = AudioPacket(UPLINK, 1, 1, 0, 0, 0, 0, b"synthetic")
        await connection._enqueue(("audio", first))
        # 只检查元数据而不消费它：即使新轮次改变了接纳基准，
        # 旧队列中的音频也必须保留自己的原始起点。
        connection.queue.get_nowait()
        connection.queue.task_done()
        old_audio = connection.queue.get_nowait()
        connection.queue.task_done()
        old_origin = old_audio[3]
        clock[0] += 79.0
        await connection._enqueue(("start", 2))
        next_packet = AudioPacket(UPLINK, 1, 1, 0, 0, 1, 320, b"synthetic")
        await connection._enqueue(("audio", next_packet))
        self.assertEqual(connection._diagnostic_values()["turn_media_lag_ms"], 0)
        self.assertEqual(connection._diagnostic_values()["turn_media_lag_max_ms"], 0)
        self.assertGreater(connection._media_lag_seconds(first, old_origin), 78)

    async def test_decoder_pause_is_checked_before_forwarding_pcm(self):
        connection = self.connection()
        clock = [0.0]
        connection._media_now = lambda: clock[0]

        async def decode(function, packet):
            clock[0] += 0.8
            return function(packet)

        packet = AudioPacket(UPLINK, 1, 1, 0, 0, 0, 0, b"synthetic")
        await connection._enqueue(("audio", packet))
        feed = connection.pipeline.feed_audio = AsyncMock()
        with patch("transport.voice_websocket.codec_call", side_effect=decode):
            with self.assertLogs("transport.voice_websocket", level="WARNING"):
                with self.assertRaises(UplinkBacklogError):
                    await connection._work()
        feed.assert_not_awaited()

    async def test_cancelled_admission_does_not_insert_audio_after_cancellation(self):
        connection = self.connection()
        await self.fill_queue(connection)
        sender = asyncio.create_task(connection._enqueue(("audio", 25)))
        await asyncio.sleep(0)
        sender.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await sender
        connection.queue.get_nowait()
        connection.queue.task_done()
        await asyncio.sleep(0)
        self.assertEqual(connection.queue.qsize(), 24)
        self.assertEqual(connection._stage_started.get("enqueue_wait"), None)

    async def test_expired_audio_aborts_before_decode_even_if_producer_has_stopped(self):
        connection = self.connection()
        feed = connection.pipeline.feed_audio = AsyncMock()
        now = asyncio.get_running_loop().time()
        connection.queue.put_nowait(("audio", b"expired", now - 1))
        with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
            with self.assertRaises(UplinkBacklogError):
                await connection._work()
        feed.assert_not_awaited()
        self.assertEqual(connection.first_failure["category"], "uplink_backlog")
        self.assertIn("stage=queue_age", logs.output[0])
        self.assertIn("worker_phase=queue_age", logs.output[0])
        self.assertEqual(connection.queue.qsize(), 0)

    async def test_old_control_does_not_count_as_old_audio_but_old_audio_blocks_admission(self):
        connection = self.connection()
        now = asyncio.get_running_loop().time()
        connection.queue.put_nowait(("stream_start", {}, now - 10))
        connection.queue.put_nowait(("audio", 0, now - 0.02))
        metrics = connection._diagnostic_values()
        # 较大的单调时间戳相减时，舍入结果可能略小于 20 ms。
        self.assertGreaterEqual(metrics["queue_oldest_audio_ms"], 20 - 1e-6)
        self.assertLess(metrics["queue_oldest_audio_ms"], 500)
        connection.queue.get_nowait()
        connection.queue.task_done()
        connection.queue.get_nowait()
        connection.queue.task_done()
        connection.queue.put_nowait(("audio", 0, now - 1))
        with self.assertRaises(UplinkBacklogError):
            await connection._enqueue(("audio", 1))

    async def test_new_turn_resets_arrival_scope_without_erasing_connection_history(self):
        connection = self.connection()
        await connection._enqueue(("start", 1))
        connection._record_audio_arrival(1.0)
        connection._record_audio_arrival(1.001)
        connection._record_audio_arrival(1.002)
        await connection._enqueue(("start", 2))
        connection._record_audio_arrival(80.0)
        metrics = connection._diagnostic_values()
        self.assertEqual(metrics["arrival_turn"], 2)
        self.assertEqual(metrics["turn_receive_frames"], 1)
        self.assertEqual(metrics["turn_receive_gap_max_ms"], 0)
        self.assertEqual(metrics["turn_receive_burst_peak"], 1)
        self.assertEqual(metrics["receive_frames"], 4)
        self.assertGreater(metrics["receive_gap_max_ms"], 78000)


if __name__ == "__main__":
    unittest.main()
