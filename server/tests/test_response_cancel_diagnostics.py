"""用取消时序区分清理与连接等待，不包含内容正文。"""

import asyncio
import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock

from tests.test_pipeline import selection
from transport.voice_websocket import DeviceConnection


class ResponseCancelDiagnosticsTests(unittest.IsolatedAsyncioTestCase):
    def connection(self, *, active=True):
        selected = selection()
        selected.transport = {"uplink": {"codec": "pcm_s16le"},
                              "downlink": {"codec": "pcm_s16le"}}
        ws = SimpleNamespace(send_json=AsyncMock())
        connection = DeviceConnection(ws, None, {}, selected, None)
        identity = {"session_epoch": 1234, "turn_id": 3, "generation": 2}
        task = asyncio.get_running_loop().create_future() if active else None
        connection.pipeline = SimpleNamespace(response_identity=identity,
            response_task=task, cancel_response=AsyncMock(return_value=False))
        return connection, {"type": "response.cancel", **identity}

    async def test_active_timing_separates_cleanup_and_actual_ack_write(self):
        connection, request = self.connection()
        cleanup_entered, cleanup_release = asyncio.Event(), asyncio.Event()
        socket_entered, socket_release = asyncio.Event(), asyncio.Event()

        async def cancel():
            cleanup_entered.set()
            await cleanup_release.wait()
            await connection.send_json({"type": "response.cancelled", **connection.pipeline.response_identity})
            return True

        async def send(event):
            socket_entered.set()
            await socket_release.wait()

        connection.pipeline.cancel_response.side_effect = cancel
        connection.ws.send_json.side_effect = send
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            cancel_task = asyncio.create_task(connection._cancel_response(request))
            try:
                await asyncio.wait_for(cleanup_entered.wait(), 1)
                self.assertIn("phase=received mode=active", "\n".join(logs.output))
                self.assertNotIn("phase=cleanup_complete", "\n".join(logs.output))
                connection.ws.send_json.assert_not_awaited()
                cleanup_release.set()
                await asyncio.wait_for(socket_entered.wait(), 1)
                self.assertIn("phase=cleanup_complete mode=active", "\n".join(logs.output))
                self.assertNotIn("phase=ack_sent", "\n".join(logs.output))
                socket_release.set()
                await asyncio.wait_for(cancel_task, 1)
            finally:
                cleanup_release.set()
                socket_release.set()
                await cancel_task
        messages = [record.getMessage() for record in logs.records]
        self.assertEqual([message.split("phase=", 1)[1].split()[0] for message in messages],
                         ["received", "cleanup_complete", "ack_sent"])
        self.assertTrue(all("epoch=1234 turn=3 generation=2" in message for message in messages))
        self.assertTrue(all("elapsed_ms=" in message and "send_ms=" in message for message in messages))
        connection.ws.send_json.assert_awaited_once()
        self.assertIsNone(connection._response_cancel_diagnostic)

    async def test_completed_response_is_idempotent_and_acknowledged_once(self):
        connection, request = self.connection(active=False)
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            await connection._cancel_response(request)
        self.assertEqual(len(logs.records), 3)
        self.assertTrue(all("mode=idempotent" in message for message in logs.output))
        self.assertIn("phase=ack_sent", logs.output[-1])
        connection.ws.send_json.assert_awaited_once_with({"type": "response.cancelled",
            **connection.pipeline.response_identity})

    async def test_stale_malformed_identity_is_rejected_without_exposing_values(self):
        connection, request = self.connection()
        request.update(turn_id={"private-key": "private-payload"}, generation="private-secret")
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            await connection._cancel_response(request)
        self.assertEqual(len(logs.records), 2)
        self.assertIn("phase=rejected mode=stale epoch=1234 turn=0 generation=0", logs.output[-1])
        self.assertNotIn("private", "\n".join(logs.output))
        connection.pipeline.cancel_response.assert_not_awaited()
        connection.ws.send_json.assert_not_awaited()

    async def test_socket_failure_never_logs_ack_sent_or_exception_contents(self):
        connection, request = self.connection(active=False)
        connection.ws.send_json.side_effect = OSError("private-socket-and-credential")
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            with self.assertRaises(OSError):
                await connection._cancel_response(request)
        messages = "\n".join(logs.output)
        self.assertIn("phase=cleanup_complete", messages)
        self.assertIn("phase=failed", messages)
        self.assertIn("stage=ack_write", messages)
        self.assertNotIn("phase=ack_sent", messages)
        self.assertNotIn("private", messages)
        self.assertIsNone(connection._response_cancel_diagnostic)

    async def test_cleanup_failure_reports_cleanup_and_sends_no_ack(self):
        connection, request = self.connection()
        connection.pipeline.cancel_response.side_effect = RuntimeError("private-upstream-body")
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            with self.assertRaises(RuntimeError):
                await connection._cancel_response(request)
        messages = "\n".join(logs.output)
        self.assertIn("phase=failed", messages)
        self.assertIn("stage=cleanup", messages)
        self.assertNotIn("phase=cleanup_complete", messages)
        self.assertNotIn("private", messages)
        connection.ws.send_json.assert_not_awaited()
        self.assertIsNone(connection._response_cancel_diagnostic)

    async def test_downlink_lock_wait_is_not_reported_as_cleanup(self):
        connection, request = self.connection(active=False)
        await connection.send_lock.acquire()
        with self.assertLogs("transport.voice_websocket", level="INFO") as logs:
            task = asyncio.create_task(connection._cancel_response(request))
            try:
                await asyncio.sleep(0)
                self.assertIn("phase=cleanup_complete", "\n".join(logs.output))
                self.assertNotIn("phase=ack_sent", "\n".join(logs.output))
                connection.ws.send_json.assert_not_awaited()
            finally:
                connection.send_lock.release()
                await asyncio.wait_for(task, 1)
        self.assertIn("phase=ack_sent", logs.output[-1])

    async def test_ack_from_other_path_is_not_attributed_to_response_cancel(self):
        connection, request = self.connection(active=False)
        with self.assertNoLogs("transport.voice_websocket", level="INFO"):
            await connection.send_json({"type": "response.cancelled", **connection.pipeline.response_identity})
        connection.ws.send_json.assert_awaited_once()


if __name__ == "__main__":
    unittest.main()
