"""保留上游 ASR 故障原因的可观测性，不记录响应正文。"""

import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

from providers.asr.doubao_streaming import DoubaoAsrError, DoubaoStreamingAsrProvider
from transport.voice_websocket import DeviceConnection


class AsrFailureDiagnosticsTests(unittest.IsolatedAsyncioTestCase):
    def connection(self):
        selection = SimpleNamespace(transport={"uplink": {"codec": "pcm_s16le"},
                                               "downlink": {"codec": "pcm_s16le"}})
        ws = SimpleNamespace(send_json=AsyncMock())
        return DeviceConnection(ws, None, {}, selection, None)

    async def test_direct_failure_preserves_numeric_cause_after_provider_cleanup(self):
        connection = self.connection()
        secret = "secret-key-remote-response-and-audio"
        # 最终识别流程重新抛出异常前，可能已经清除了 pipeline.asr。
        error = DoubaoAsrError(secret, reason="upstream_rejected", upstream_code=45000001)
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            connection.record_failure("asr_final", error)
            first = dict(connection.first_failure)
            connection.record_failure("session_route", RuntimeError(secret))
        output = "\n".join(logs.output)
        self.assertIn("asr_reason=upstream_rejected asr_upstream_code=45000001", output)
        self.assertNotIn(secret, output)
        self.assertEqual(connection.first_failure, first)
        self.assertEqual(first["asr_reason"], "upstream_rejected")
        self.assertEqual(first["asr_upstream_code"], 45000001)

    async def test_reader_error_is_captured_before_feed_or_finalization_raises(self):
        connection = self.connection()
        provider = DoubaoStreamingAsrProvider({}, {})
        provider._error = DoubaoAsrError("private-message", reason="receive_timeout")
        provider._done.set()
        connection.pipeline.asr = provider
        with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
            await connection.pipeline._partials(connection.pipeline.session.turn)
        self.assertIn("asr_reason=receive_timeout", logs.output[0])
        self.assertNotIn("private-message", logs.output[0])
        self.assertEqual(connection.first_failure["asr_reason"], "receive_timeout")
        event = connection.ws.send_json.await_args.args[0]
        self.assertEqual(event["code"], "asr_events_failed")
        self.assertNotIn("asr_reason", event, "Provider diagnostics belong in local logs")

    async def test_current_failure_takes_precedence_over_reader_snapshot(self):
        connection = self.connection()
        provider = DoubaoStreamingAsrProvider({}, {})
        provider._error = DoubaoAsrError("private-reader", reason="receive_timeout")
        connection.pipeline.asr = provider
        error = DoubaoAsrError("private-send", reason="send_failed")
        with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
            connection.record_failure("asr_feed", error)
        self.assertIn("asr_reason=send_failed", logs.output[0])
        self.assertEqual(connection.first_failure["asr_reason"], "send_failed")

    async def test_untrusted_exception_and_provider_cannot_inject_log_fields(self):
        connection = self.connection()
        private = "private-address-and-key"
        method = Mock(return_value={"asr_reason": private})
        connection.pipeline.asr = SimpleNamespace(failure_diagnostics=method)
        error = RuntimeError(private)
        error.diagnostics = method
        with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
            connection.record_failure("asr_feed", error)
        method.assert_not_called()
        self.assertIn("asr_reason=none asr_upstream_code=0 asr_upstream_close_code=0", logs.output[0])
        self.assertNotIn(private, logs.output[0])

    async def test_upstream_and_device_close_codes_remain_distinct(self):
        connection = self.connection()
        error = DoubaoAsrError("remote-close-text", reason="upstream_closed",
                               upstream_close_code=1011)
        with self.assertLogs("transport.voice_websocket", level="WARNING") as logs:
            connection.record_failure("asr_feed", error, close_code=1008)
        self.assertIn("close_code=1008", logs.output[0])
        self.assertIn("asr_upstream_close_code=1011", logs.output[0])
        self.assertNotIn("remote-close-text", logs.output[0])


if __name__ == "__main__":
    unittest.main()
