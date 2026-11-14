"""输出暂停不能阻塞语音连接、健康检查或关闭流程。

所有连接仅用于本机测试，服务商为模拟实现，配置来自示例。
不读取或修改凭证及用户正在运行的服务。
"""

import asyncio
import contextlib
import io
import json
import logging
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import weakref

from core.config import EXAMPLE_CONFIG_PATH, load_voice_config, select_profile
from transport.diagnostic_logging import BoundedDiagnosticHandler
import voice_server


class BlockingStream:
    def __init__(self):
        self.entered = threading.Event()
        self.release = threading.Event()
        self.lines = []

    def write(self, text):
        self.entered.set()
        if not self.release.wait(5):
            raise RuntimeError("Synthetic terminal remained paused")
        self.lines.append(text)

    def flush(self):
        pass


class ForbiddenStream:
    def write(self, text):
        raise AssertionError("Unexpected fallback to synchronous stderr")

    def flush(self):
        raise AssertionError("Unexpected fallback flush")


def wait_until(predicate, timeout=1):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("Diagnostic worker did not reach expected state")
        time.sleep(0.005)


class DiagnosticHandlerTests(unittest.TestCase):
    def logger(self, stream, **options):
        handler = BoundedDiagnosticHandler(stream, **options)
        self.addCleanup(handler.close)
        logger = logging.Logger("transport.voice_websocket", logging.INFO)
        logger.addHandler(handler)
        return logger, handler

    def test_full_queue_is_bounded_and_recovers_without_producer_stderr(self):
        stream = BlockingStream()
        logger, handler = self.logger(stream, capacity=3)
        try:
            logger.info("voice_test first=1")
            self.assertTrue(stream.entered.wait(1))
            with patch("sys.stderr", ForbiddenStream()), patch.object(
                    handler, "handleError", side_effect=AssertionError("Must never use handleError")):
                for index in range(1000):
                    logger.info("voice_test index=%d", index)
                stats = handler.stats()
                self.assertEqual((stats.queued, stats.capacity, stats.accepted), (3, 3, 4))
                self.assertEqual(stats.dropped, 997)
            stream.release.set()
            self.assertTrue(handler.stop(1))
            text = "".join(stream.lines)
            self.assertIn("diagnostic_log_dropped count=997 total=997", text)
            self.assertEqual(handler.stats().written, 4)
        finally:
            stream.release.set()
            handler.stop(1)

    def test_logging_shutdown_is_bounded_even_while_writer_is_paused(self):
        stream = BlockingStream()
        logger, handler = self.logger(stream, capacity=2, stop_timeout=0.03)
        try:
            logger.info("voice_test blocked=1")
            self.assertTrue(stream.entered.wait(1))
            began = time.monotonic()
            logging.shutdown([weakref.ref(handler)])
            self.assertLess(time.monotonic() - began, 0.4)
            stats = handler.stats()
            self.assertFalse(stats.accepting)
            self.assertTrue(stats.writer_alive)
            logger.info("voice_test after_stop=1")
            self.assertEqual(handler.stats().accepted, 1)
        finally:
            stream.release.set()
            self.assertTrue(handler.stop(1))

    def test_sink_exception_never_falls_back_to_stderr(self):
        class FailingStream:
            def write(self, text):
                raise OSError("Synthetic output failure")
            def flush(self):
                raise AssertionError("Should not flush after failed write")

        with patch("sys.stderr", ForbiddenStream()):
            logger, handler = self.logger(FailingStream())
            logger.info("voice_test failure=1")
            wait_until(lambda: handler.stats().sink_failures > 0)
            self.assertEqual(handler.stats().dropped, 1)
            self.assertTrue(handler.stop(1))

    def test_queued_lines_own_formatted_text_and_have_a_fixed_size_limit(self):
        class MutableValue:
            value = "before"
            def __str__(self):
                return self.value

        stream = BlockingStream()
        logger, handler = self.logger(stream, capacity=2, max_line_chars=128)
        value = MutableValue()
        try:
            logger.info("voice_test gate=1")
            self.assertTrue(stream.entered.wait(1))
            logger.info("voice_test value=%s", value)
            value.value = "after"
            logger.info("voice_test value=%s", "x" * 10000)
            stream.release.set()
            self.assertTrue(handler.stop(1))
            self.assertIn("value=before", "".join(stream.lines))
            self.assertNotIn("value=after", "".join(stream.lines))
            self.assertTrue(all(len(line.rstrip("\n")) <= 128 for line in stream.lines))
            self.assertIn("[truncated]", stream.lines[-1])
        finally:
            stream.release.set()
            handler.stop(1)

    def test_invalid_format_is_counted_without_logging_another_error(self):
        stream = io.StringIO()
        logger, handler = self.logger(stream)
        with patch.object(handler, "handleError", side_effect=AssertionError("Must not recurse")):
            logger.info("voice_test numeric=%d", "invalid-number")
        self.assertEqual(handler.stats().dropped, 1)
        self.assertTrue(handler.stop(1))


class DiagnosticLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.logger = logging.getLogger("transport.voice_websocket")
        self.original = (list(self.logger.handlers), self.logger.level, self.logger.propagate)
        self.logger.handlers = []
        self.addCleanup(self.restore)

    def restore(self):
        for handler in self.logger.handlers:
            if isinstance(handler, BoundedDiagnosticHandler):
                handler.close()
        self.logger.handlers, self.logger.level, self.logger.propagate = self.original

    def test_configuration_is_idempotent_and_replaces_synchronous_handler(self):
        old = logging.StreamHandler(ForbiddenStream())
        self.logger.addHandler(old)
        stream = io.StringIO()
        handler = voice_server._configure_realtime_logging(stream=stream)
        self.assertEqual(self.logger.handlers, [handler])
        self.assertIs(voice_server._configure_realtime_logging(), handler)
        self.logger.info("voice_test lifecycle=1")
        self.assertTrue(handler.stop(1))
        self.assertIn("voice_test lifecycle=1", stream.getvalue())
        old.close()

    def test_provider_timing_reaches_existing_bounded_sink_under_standard_configuration(self):
        from providers.tts.volcengine import _TIMING_LOG

        stream = io.StringIO()
        handler = voice_server._configure_realtime_logging(stream=stream)
        self.assertEqual(_TIMING_LOG.handlers, [])
        self.assertTrue(_TIMING_LOG.isEnabledFor(logging.INFO))
        _TIMING_LOG.info("tts_subtitle_timing source=%s cue=%d words=%d",
                         "8313959b-ea47-4d18-921b-83edb99afac4", 1, 13)
        self.assertTrue(handler.stop(1))
        output = stream.getvalue()
        self.assertIn("INFO transport.voice_websocket.tts tts_subtitle_timing", output)
        self.assertIn("cue=1 words=13", output)
        self.assertEqual(handler.stats().accepted, 1)

    def test_main_always_detaches_and_stops_handler_when_runtime_start_fails(self):
        stream = io.StringIO()
        handler = voice_server._configure_realtime_logging(stream=stream)
        selection = SimpleNamespace(to_dict=lambda: {"mode": "realtime"})
        with patch.object(voice_server, "resolve_configuration", return_value=(object(), selection, {})), \
             patch.object(voice_server, "create_realtime_app", return_value=object()), \
             patch("aiohttp.web.run_app", side_effect=OSError("Synthetic bind failure")), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(voice_server.main(["--mode", "realtime"]), 1)
        self.assertNotIn(handler, self.logger.handlers)
        self.assertFalse(handler.stats().accepting)
        self.assertFalse(handler.stats().writer_alive)

    def test_main_detaches_and_stops_handler_after_normal_runtime_shutdown(self):
        handler = voice_server._configure_realtime_logging(stream=io.StringIO())
        selection = SimpleNamespace(to_dict=lambda: {"mode": "realtime"})
        with patch.object(voice_server, "resolve_configuration", return_value=(object(), selection, {})), \
             patch.object(voice_server, "create_realtime_app", return_value=object()), \
             patch("aiohttp.web.run_app") as runtime, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(voice_server.main(["--mode", "realtime"]), 0)
        runtime.assert_called_once()
        self.assertNotIn(handler, self.logger.handlers)
        self.assertFalse(handler.stats().accepting)
        self.assertFalse(handler.stats().writer_alive)


class DiagnosticSocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_paused_output_keeps_http_ping_websocket_pong_and_new_upgrade_responsive(self):
        import aiohttp
        from aiohttp.test_utils import TestClient, TestServer
        from tests.test_voice_websocket import SimulatedProviders

        logger = logging.getLogger("transport.voice_websocket")
        original = (list(logger.handlers), logger.level, logger.propagate)
        logger.handlers = []
        stream = BlockingStream()
        handler = voice_server._configure_realtime_logging(stream=stream, capacity=3)
        providers = SimulatedProviders()
        try:
            data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
            data["transport"]["idle_timeout_seconds"] = 0
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "voice_config.json"
                path.write_text(json.dumps(data), encoding="utf-8")
                config = load_voice_config(path, environ={})
            selected = select_profile(config, profile="realtime_default", mode="realtime")
            credentials = {name: "synthetic" for name in (
                "VOLCENGINE_ASR_API_KEY", "VOLCENGINE_TTS_API_KEY", "DEEPSEEK_API_KEY")}
            app = voice_server.create_realtime_app(config, credentials, selected, provider_factory=providers)
            async with TestClient(TestServer(app)) as client:
                async with client.ws_connect("/ws/voice", autoping=False) as first:
                    start = {"type": "session.start", "version": 1,
                             "uplink_codec": "pcm_s16le", "downlink_codec": "pcm_s16le"}
                    await first.send_json(start)
                    ready = await first.receive_json(timeout=1)
                    self.assertEqual(ready["type"], "session.ready")
                    logger.info("voice_diagnostic_probe epoch=%d", ready["session_epoch"])
                    self.assertTrue(await asyncio.to_thread(stream.entered.wait, 1))
                    for index in range(100):
                        logger.info("voice_diagnostic_probe index=%d", index)
                    self.assertEqual(handler.stats().queued, 3)
                    self.assertGreater(handler.stats().dropped, 0)

                    async with asyncio.timeout(1):
                        async with client.get("/ping") as response:
                            self.assertEqual((response.status, await response.text()), (200, "OK"))
                        await first.ping(b"output-paused")
                        pong = await first.receive(timeout=1)
                        self.assertEqual((pong.type, pong.data), (aiohttp.WSMsgType.PONG, b"output-paused"))
                        async with client.ws_connect("/ws/voice", autoping=False) as second:
                            await second.send_json(start)
                            second_ready = await second.receive_json(timeout=1)
                            self.assertEqual(second_ready["type"], "session.ready")
                            self.assertNotEqual(second_ready["session_epoch"], ready["session_epoch"])
                    self.assertFalse(stream.release.is_set())
                    self.assertEqual(providers.created, [])
        finally:
            stream.release.set()
            self.assertTrue(await asyncio.to_thread(handler.stop, 1))
            handler.close()
            logger.handlers, logger.level, logger.propagate = original
        self.assertIn("diagnostic_log_dropped count=", "".join(stream.lines))


if __name__ == "__main__":
    unittest.main()
