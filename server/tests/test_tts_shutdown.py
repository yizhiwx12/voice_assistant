"""取消时必须在 D2 确认时间预算内释放上游资源。"""

import asyncio
import unittest

from providers.tts.cosyvoice import CosyVoiceTtsProvider
from providers.tts.volcengine import VolcengineTtsProvider
from tests.test_volcengine_tts import FakeSession, FakeWebSocket, request_fields, response


class SlowSocket(FakeWebSocket):
    def __init__(self, *, blocked_event=None, block_close=False):
        super().__init__(finish_audio=False)
        self.blocked_event = blocked_event
        self.block_close = block_close
        self.operation_entered = asyncio.Event()
        self.close_cancelled = False

    async def send_bytes(self, packet):
        if request_fields(packet)[0] == self.blocked_event:
            self.operation_entered.set()
            await asyncio.Event().wait()
        await super().send_bytes(packet)

    async def close(self):
        if self.block_close:
            self.operation_entered.set()
            try:
                await asyncio.Event().wait()
            except asyncio.CancelledError:
                # aiohttp 的关闭握手被取消时，也会关闭底层响应；
                # 这个替身模拟该公开行为。
                self.close_cancelled = True
                self.closed = True
                raise
        await super().close()


class TtsShutdownTests(unittest.IsolatedAsyncioTestCase):
    async def volc(self, socket):
        provider = VolcengineTtsProvider(
            {"voice": "mock", "timeout_s": 30},
            {"VOLCENGINE_TTS_API_KEY": "mock"}, FakeSession(socket))
        await provider.start()
        self.addAsyncCleanup(provider.close)
        return provider

    async def test_cancel_bounds_stalled_cancel_send_and_joins_reader(self):
        socket = SlowSocket(blocked_event=101)
        provider = await self.volc(socket)
        await asyncio.wait_for(provider.cancel(), 0.7)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._reader_task.done())
        self.assertTrue(provider._close_task.done())
        self.assertEqual([chunk async for chunk in provider.audio()], [])

    async def test_cancel_bounds_stalled_close_without_background_cleanup(self):
        socket = SlowSocket(block_close=True)
        provider = await self.volc(socket)
        await asyncio.wait_for(provider.cancel(), 0.7)
        self.assertTrue(socket.close_cancelled)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._reader_task.done())
        self.assertTrue(provider._close_task.done())

    async def test_cancel_preempts_graceful_send_with_multiple_close_waiters(self):
        socket = SlowSocket(blocked_event=2)
        provider = await self.volc(socket)
        provider._end()
        closing = asyncio.create_task(provider.close())
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        await asyncio.wait_for(asyncio.gather(provider.cancel(), provider.cancel(), closing), 0.7)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._reader_task.done())
        self.assertTrue(provider._close_task.done())

    async def test_cancel_preempts_graceful_remote_ack_wait(self):
        socket = SlowSocket()
        original_send = socket.send_bytes

        async def no_finish_ack(packet):
            if request_fields(packet)[0] == 2:
                socket.operation_entered.set()
                return
            await original_send(packet)

        socket.send_bytes = no_finish_ack
        provider = await self.volc(socket)
        provider._end()
        closing = asyncio.create_task(provider.close())
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        await asyncio.wait_for(asyncio.gather(provider.cancel(), closing), 0.7)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._reader_task.done())
        self.assertTrue(provider._close_task.done())

    async def test_cancel_preempts_graceful_websocket_handshake(self):
        socket = SlowSocket(block_close=True)
        provider = await self.volc(socket)
        provider._end()
        closing = asyncio.create_task(provider.close())
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        await asyncio.wait_for(asyncio.gather(provider.cancel(), closing), 0.7)
        self.assertTrue(socket.close_cancelled)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._close_task.done())

    async def test_cosyvoice_cancel_preempts_graceful_close(self):
        socket = SlowSocket(block_close=True)
        provider = CosyVoiceTtsProvider({"timeout_s": 45}, {}, FakeSession(socket))
        provider._ws = socket
        closing = asyncio.create_task(provider.close())
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        await asyncio.wait_for(asyncio.gather(provider.cancel(), closing), 0.7)
        self.assertTrue(socket.close_cancelled)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._close_task.done())

    async def test_cancelled_close_waiter_propagates_only_after_cleanup(self):
        for kind in ("volcengine", "cosyvoice"):
            with self.subTest(provider=kind):
                socket = SlowSocket(block_close=True)
                if kind == "volcengine":
                    provider = await self.volc(socket)
                    provider._end()
                else:
                    provider = CosyVoiceTtsProvider({"timeout_s": 45}, {}, FakeSession(socket))
                    provider._ws = socket
                closing = asyncio.create_task(provider.close())
                await asyncio.wait_for(socket.operation_entered.wait(), 1)
                closing.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await asyncio.wait_for(closing, 0.7)
                self.assertTrue(socket.closed)
                self.assertTrue(provider._close_task.done())
                if kind == "volcengine":
                    self.assertTrue(provider._reader_task.done())

    async def test_cancel_mid_volcengine_start_uses_fast_joined_close(self):
        socket = SlowSocket(blocked_event=1, block_close=True)
        provider = VolcengineTtsProvider(
            {"voice": "mock", "timeout_s": 30},
            {"VOLCENGINE_TTS_API_KEY": "mock"}, FakeSession(socket))
        starting = asyncio.create_task(provider.start())
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        starting.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await asyncio.wait_for(starting, 0.7)
        self.assertTrue(provider._cancelled)
        self.assertTrue(socket.closed)
        self.assertTrue(provider._reader_task.done())
        self.assertTrue(provider._close_task.done())

    async def test_cancel_mid_cosyvoice_start_ack_uses_fast_joined_close(self):
        socket = SlowSocket(block_close=True)

        async def send_json(value):
            pass

        async def receive(timeout):
            socket.operation_entered.set()
            await asyncio.Event().wait()

        socket.send_json, socket.receive = send_json, receive
        provider = CosyVoiceTtsProvider(
            {"endpoint": "ws://mock", "model": "mock", "voice": "mock", "timeout_s": 45},
            {"DASHSCOPE_API_KEY": "mock"}, FakeSession(socket))
        starting = asyncio.create_task(provider.start())
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        starting.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await asyncio.wait_for(starting, 0.7)
        self.assertTrue(provider._cancel_requested.is_set())
        self.assertTrue(socket.closed)
        self.assertTrue(provider._close_task.done())

    async def test_cancel_mid_cosyvoice_append_uses_fast_joined_close(self):
        socket = SlowSocket(block_close=True)

        async def send_json(value):
            socket.operation_entered.set()
            await asyncio.Event().wait()

        socket.send_json = send_json
        provider = CosyVoiceTtsProvider({"timeout_s": 45}, {}, FakeSession(socket))
        provider._ws, provider._started = socket, True
        appending = asyncio.create_task(provider.append_text("mock"))
        await asyncio.wait_for(socket.operation_entered.wait(), 1)
        appending.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await asyncio.wait_for(appending, 0.7)
        self.assertTrue(provider._cancel_requested.is_set())
        self.assertTrue(socket.closed)
        self.assertTrue(provider._close_task.done())

    async def test_real_aiohttp_cancel_closes_without_remote_close_reply(self):
        import aiohttp
        from aiohttp import web

        connected, release = asyncio.Event(), asyncio.Event()
        server_socket = None

        async def fixture(request):
            nonlocal server_socket
            server_socket = web.WebSocketResponse(autoclose=False)
            await server_socket.prepare(request)
            # 只完成适配器握手，然后故意停止接收：
            # 不发送远端取消、连接结束或关闭确认。
            for reply_event in (50, 150):
                packet = await server_socket.receive()
                _, scope, _ = request_fields(packet.data)
                await server_socket.send_bytes(response(reply_event, scope or "server"))
            connected.set()
            await release.wait()
            return server_socket

        app = web.Application()
        app.router.add_get("/tts", fixture)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        try:
            async with aiohttp.ClientSession() as session:
                provider = VolcengineTtsProvider(
                    {"voice": "mock", "timeout_s": 30, "endpoint": f"ws://127.0.0.1:{port}/tts"},
                    {"VOLCENGINE_TTS_API_KEY": "mock"}, session)
                await provider.start()
                await connected.wait()
                await asyncio.wait_for(provider.cancel(), 0.7)
                self.assertTrue(provider._ws.closed)
                self.assertTrue(provider._reader_task.done())
                self.assertTrue(provider._close_task.done())
        finally:
            release.set()
            if server_socket is not None:
                await server_socket.close()
            await runner.cleanup()
