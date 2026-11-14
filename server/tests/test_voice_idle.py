"""离线验证待机策略回归，不访问服务商或凭证。"""

import asyncio
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

from core.config import ConfigurationError, EXAMPLE_CONFIG_PATH, load_voice_config, select_profile
from transport.voice_websocket import DeviceConnection, _DownlinkPacer


class IdleConfigurationTests(unittest.TestCase):
    def test_optional_standby_timeout_defaults_to_disabled_and_is_bounded(self):
        data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "voice_config.json"
            data["transport"].pop("idle_timeout_seconds", None)
            path.write_text(json.dumps(data), encoding="utf-8")
            self.assertEqual(load_voice_config(path, environ={}).transport["idle_timeout_seconds"], 0)
            for seconds in (0, 1, 90, 86400):
                with self.subTest(valid=seconds):
                    data["transport"]["idle_timeout_seconds"] = seconds
                    path.write_text(json.dumps(data), encoding="utf-8")
                    self.assertEqual(load_voice_config(path, environ={}).transport["idle_timeout_seconds"], seconds)
            for seconds in (True, -1, 86401, 1.5, "90", None):
                with self.subTest(invalid=seconds):
                    data["transport"]["idle_timeout_seconds"] = seconds
                    path.write_text(json.dumps(data), encoding="utf-8")
                    with self.assertRaisesRegex(ConfigurationError, "idle_timeout_seconds"):
                        load_voice_config(path, environ={})


class InMemoryDevice:
    def __init__(self):
        self.closed = False
        self.ready = asyncio.Event()
        self.finished = asyncio.Event()
        self.incoming = asyncio.Queue()
        self.events = []
        self.closed_at = None
        self.close_code = None

    async def send_json(self, event):
        self.events.append(event)
        if event["type"] == "session.ready":
            self.ready.set()

    async def send_bytes(self, packet):
        pass

    async def receive(self):
        return await self.incoming.get()

    async def close(self, *, code, message):
        self.closed = True
        self.close_code = code
        self.closed_at = asyncio.get_running_loop().time()
        self.finished.set()


class StandbyPolicyTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_websocket_idle_closes_normally_despite_ping_pong(self):
        import aiohttp
        from aiohttp.test_utils import TestClient, TestServer
        from tests.test_voice_websocket import SimulatedProviders
        import voice_server

        data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
        data["transport"]["idle_timeout_seconds"] = 1
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "voice_config.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            config = load_voice_config(path, environ={})
        selected = select_profile(config, profile="realtime_default", mode="realtime")
        credentials = {name: "synthetic" for name in (
            "VOLCENGINE_ASR_API_KEY", "VOLCENGINE_TTS_API_KEY", "DEEPSEEK_API_KEY")}
        app = voice_server.create_realtime_app(config, credentials, selected,
                                              provider_factory=SimulatedProviders())
        async with TestClient(TestServer(app)) as client:
            async with client.ws_connect("/ws/voice") as ws:
                await ws.send_json({"type": "session.start", "version": 1,
                                    "uplink_codec": "pcm_s16le", "downlink_codec": "pcm_s16le"})
                ready = await ws.receive_json(timeout=1)
                self.assertEqual(ready["type"], "session.ready")
                began = asyncio.get_running_loop().time()

                async def ping():
                    for _ in range(4):
                        await asyncio.sleep(0.2)
                        await ws.ping()

                heartbeats = asyncio.create_task(ping())
                event = await ws.receive_json(timeout=2)
                await heartbeats
                self.assertEqual((event["type"], event["reason"]), ("session.closed", "idle_timeout"))
                self.assertEqual(event["session_epoch"], ready["session_epoch"])
                self.assertGreaterEqual(asyncio.get_running_loop().time() - began, 0.90)
                closed = await ws.receive(timeout=1)
                self.assertEqual(closed.type, aiohttp.WSMsgType.CLOSE)
                self.assertEqual(ws.close_code, 1000)

    def connection(self, seconds):
        selection = SimpleNamespace(transport={
            "idle_timeout_seconds": seconds,
            "uplink": {"codec": "pcm_s16le"}, "downlink": {"codec": "pcm_s16le"},
        }, to_dict=lambda: {})
        device = InMemoryDevice()
        connection = DeviceConnection(device, None, {}, selection, None,
                                      provider_factory=lambda *args: self.fail("Unexpected provider access"))
        return connection, device

    async def start(self, connection, device):
        task = asyncio.create_task(connection.run({}))

        async def cleanup():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

        self.addAsyncCleanup(cleanup)
        await asyncio.wait_for(device.ready.wait(), 1)
        return task

    async def test_default_standby_has_no_application_age_limit(self):
        connection, device = self.connection(0)
        task = await self.start(connection, device)
        connection.last_activity -= 3600
        connection._activity_changed.set()
        await asyncio.sleep(0.05)
        self.assertIsNone(connection._idle_wait_seconds())
        self.assertFalse(device.closed)
        self.assertFalse(task.done())
        self.assertIsNone(connection.first_failure)

    async def test_receive_exception_still_closes_pipeline_worker_and_probe(self):
        connection, device = self.connection(0)

        async def failed_receive():
            raise RuntimeError("synthetic receive failure")

        device.receive = failed_receive
        connection.pipeline.close = close = AsyncMock()
        with self.assertLogs("transport.voice_websocket", level="WARNING"):
            with self.assertRaisesRegex(RuntimeError, "synthetic receive"):
                await connection.run({})
        close.assert_awaited_once_with()
        self.assertTrue(connection.worker.done())
        self.assertTrue(connection._loop_probe.done())

    async def test_downlink_activity_restarts_only_remaining_idle_budget(self):
        connection, device = self.connection(1)
        task = await self.start(connection, device)
        await asyncio.sleep(0.60)
        await connection.send_json({"type": "llm.delta", "text": "synthetic"})
        refreshed = asyncio.get_running_loop().time()
        await asyncio.sleep(0.55)
        self.assertFalse(device.closed, "Original connection deadline must not close an active answer")
        await asyncio.wait_for(device.finished.wait(), 1)
        await task
        self.assertGreaterEqual(device.closed_at - refreshed, 0.90)
        self.assertLess(device.closed_at - refreshed, 1.40, "Do not restart a second complete idle period")
        self.assertEqual(device.events[-1]["type"], "session.closed")
        self.assertEqual(device.close_code, 1000)
        self.assertIsNone(connection.first_failure)

    async def test_silent_cloud_work_gets_full_standby_budget_after_finishing(self):
        connection, device = self.connection(1)
        gate = asyncio.Event()
        connection.pipeline.response_task = asyncio.create_task(gate.wait())
        task = await self.start(connection, device)
        await asyncio.sleep(1.15)
        self.assertFalse(device.closed, "Cloud work without deltas is still busy")
        finished = asyncio.get_running_loop().time()
        gate.set()
        await asyncio.sleep(0.40)
        self.assertFalse(device.closed, "Busy completion must get a complete idle budget")
        await asyncio.wait_for(device.finished.wait(), 1)
        await task
        self.assertGreaterEqual(device.closed_at - finished, 0.90)
        self.assertLess(device.closed_at - finished, 1.40)

    async def test_recording_and_asr_finalization_are_not_standby(self):
        connection, _ = self.connection(1)
        connection.last_activity -= 600
        for phase in ("recording", "finishing"):
            with self.subTest(phase=phase):
                setattr(connection.pipeline, phase, True)
                self.assertGreater(connection._idle_wait_seconds(), 0)
                setattr(connection.pipeline, phase, False)
                self.assertGreater(connection._idle_wait_seconds(), 0.90)

    async def test_playback_ack_releases_busy_and_old_clients_have_finite_grace(self):
        import aiohttp

        connection, device = self.connection(1)
        identity = connection.pipeline.session.identity()
        connection.pipeline.response_identity = identity
        connection._playback_pending = dict(identity)
        connection._playback_deadline = asyncio.get_running_loop().time() + 10
        task = await self.start(connection, device)
        await asyncio.sleep(1.15)
        self.assertFalse(device.closed, "Buffered device playback must be protected")
        acknowledged = asyncio.get_running_loop().time()
        await device.incoming.put(SimpleNamespace(type=aiohttp.WSMsgType.TEXT,
            data=json.dumps({"type": "playback.done", **identity})))
        await asyncio.sleep(0.40)
        self.assertFalse(device.closed)
        await asyncio.wait_for(device.finished.wait(), 1)
        await task
        self.assertIsNone(connection._playback_pending)
        self.assertGreaterEqual(device.closed_at - acknowledged, 0.90)

        legacy, _ = self.connection(1)
        legacy._playback_pending = dict(legacy.pipeline.session.identity())
        legacy._playback_deadline = asyncio.get_running_loop().time() + 0.01
        legacy.last_activity -= 600
        self.assertGreater(legacy._idle_wait_seconds(), 0)
        await asyncio.sleep(0.02)
        self.assertGreater(legacy._idle_wait_seconds(), 0.90)
        self.assertIsNone(legacy._playback_pending)

    async def test_audio_duration_sets_finite_playback_guard_and_failure_clears_it(self):
        connection, _ = self.connection(1)
        identity = connection.pipeline.session.identity()
        connection.pipeline.response_identity = identity

        async def audio():
            yield b"\0" * 960

        before = asyncio.get_running_loop().time()
        await connection.send_audio(audio(), identity)
        self.assertEqual(connection._playback_pending, identity)
        self.assertGreaterEqual(connection._playback_deadline, before + 5.02)
        self.assertLess(connection._playback_deadline, before + 5.20)

        async def failed_audio():
            yield b"\0" * 960
            raise RuntimeError("synthetic provider failure")

        with self.assertRaisesRegex(RuntimeError, "synthetic"):
            await connection.send_audio(failed_audio(), identity)
        self.assertIsNone(connection._playback_pending)

    async def test_paced_audio_guard_does_not_restart_the_entire_answer_duration(self):
        connection, _ = self.connection(1)
        identity = connection.pipeline.session.identity()
        connection.pipeline.response_identity = identity
        clock = SimpleNamespace(now=0.0)

        async def sleep(seconds):
            clock.now += seconds

        pacer = _DownlinkPacer(clock=lambda: clock.now, sleep=sleep)
        # 只替换本模块引用的 asyncio 对象，保留实际运行中的
        # 事件循环和 wait_for 任务调度。
        timing = SimpleNamespace(**vars(asyncio))
        timing.get_running_loop = lambda: SimpleNamespace(time=lambda: clock.now)

        async def audio():
            for _ in range(250):
                yield bytes(960)

        with patch("transport.voice_websocket.asyncio", timing), \
             patch("transport.voice_websocket._DownlinkPacer", return_value=pacer):
            await connection.send_audio(audio(), identity)
        self.assertAlmostEqual(clock.now, 4.8)
        self.assertAlmostEqual(connection._playback_deadline, 10.0)
        self.assertLess(connection._playback_deadline - clock.now, 5.21)

    async def test_playback_guard_rebases_after_provider_and_socket_stalls(self):
        for source in ("provider", "socket"):
            with self.subTest(stall=source):
                connection, device = self.connection(1)
                identity = connection.pipeline.session.identity()
                connection.pipeline.response_identity = identity
                clock = SimpleNamespace(now=0.0)

                async def sleep(seconds):
                    clock.now += seconds

                pacer = _DownlinkPacer(clock=lambda: clock.now, sleep=sleep)
                timing = SimpleNamespace(**vars(asyncio))
                timing.get_running_loop = lambda: SimpleNamespace(time=lambda: clock.now)
                writes = 0

                async def send_bytes(packet):
                    nonlocal writes
                    writes += 1
                    if source == "socket" and writes == 2:
                        clock.now += 10

                device.send_bytes = send_bytes

                async def audio():
                    yield bytes(960)
                    if source == "provider":
                        clock.now += 10
                    yield bytes(960)

                with patch("transport.voice_websocket.asyncio", timing), \
                     patch("transport.voice_websocket._DownlinkPacer", return_value=pacer):
                    await connection.send_audio(audio(), identity)
                self.assertAlmostEqual(clock.now, 10.0)
                self.assertAlmostEqual(connection._playback_deadline, 15.02)

    async def test_opus_encoder_closes_when_initial_stream_message_fails_or_is_cancelled(self):
        for cancel in (False, True):
            with self.subTest(cancel=cancel):
                connection, device = self.connection(1)
                connection.selection.transport["downlink"].update(codec="opus", bitrate_bps=32000)
                connection.selection.transport["vbr"] = True
                connection.downlink_codec = "opus"
                identity = connection.pipeline.session.identity()
                connection.pipeline.response_identity = identity
                encoder = SimpleNamespace(pre_skip=312, close=Mock())
                sending = asyncio.Event()

                async def send_json(event):
                    sending.set()
                    if cancel:
                        await asyncio.Event().wait()
                    raise OSError("synthetic initial transport failure")

                async def unread_audio():
                    self.fail("Audio must not be consumed before stream start is sent")
                    yield b""

                device.send_json = send_json
                with patch("transport.voice_websocket.Encoder", return_value=encoder):
                    task = asyncio.create_task(connection.send_audio(unread_audio(), identity))
                    await asyncio.wait_for(sending.wait(), 1)
                    if cancel:
                        task.cancel()
                        with self.assertRaises(asyncio.CancelledError):
                            await task
                    else:
                        with self.assertRaisesRegex(OSError, "synthetic initial"):
                            await task
                encoder.close.assert_called_once_with()
                self.assertIsNone(connection._playback_pending)


if __name__ == "__main__":
    unittest.main()
