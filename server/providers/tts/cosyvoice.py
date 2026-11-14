"""CosyVoice 双向 WebSocket TTS，输出统一为 24 kHz 单声道 PCM16。

先等待任务确认，再逐段发送文字，结束输入后继续读取剩余音频。配置中的
model、voice 与地区 endpoint 必须匹配账号资源；取消会关闭本轮连接。
协议：https://help.aliyun.com/zh/model-studio/cosyvoice-websocket-api"""

import asyncio
from contextlib import suppress
import json
import uuid

from providers._http import require_credential


CANCEL_CLOSE_SECONDS = 0.15


class CosyVoiceTtsProvider:
    codec = "pcm_s16le"
    sample_rate_hz = 24000
    channels = 1
    sample_width_bytes = 2

    def __init__(self, options, credentials, session):
        self.options = options
        self.credentials = credentials
        self.session = session
        self.task_id = uuid.uuid4().hex
        self._ws = None
        self._started = False
        self._finished_input = False
        self._closed = False
        self._close_task = None
        self._cancel_requested = asyncio.Event()
        self._reading = False
        self._timeout_s = options.get("timeout_s", 45)

    async def start(self):
        import aiohttp

        if self._started or self._closed:
            raise RuntimeError("CosyVoice task cannot be started again")
        key = require_credential(
            self.credentials, self.options.get("credential_ref", "DASHSCOPE_API_KEY")
        )
        for field in ("endpoint", "model", "voice"):
            if not self.options.get(field):
                raise RuntimeError(f"CosyVoice {field} is not configured")
        try:
            async with asyncio.timeout(self._timeout_s):
                self._ws = await self.session.ws_connect(
                    self.options["endpoint"],
                    headers={"Authorization": f"Bearer {key}"},
                    heartbeat=20,
                    max_msg_size=1_048_576,
                )
                await self._send("run-task", {
                    "task_group": "audio",
                    "task": "tts",
                    "function": "SpeechSynthesizer",
                    "model": self.options["model"],
                    "parameters": {
                        "text_type": "PlainText",
                        "voice": self.options["voice"],
                        "format": "pcm",
                        "sample_rate": self.sample_rate_hz,
                        "volume": 50,
                        "rate": 1,
                        "pitch": 1,
                        "enable_ssml": False,
                    },
                    "input": {},
                })
                while True:
                    message = await self._receive()
                    if isinstance(message, bytes):
                        raise RuntimeError("CosyVoice sent audio before task acknowledgement")
                    if message == "task-started":
                        self._started = True
                        return
                    if message == "task-finished":
                        raise RuntimeError("CosyVoice task finished before acknowledgement")
        except asyncio.CancelledError:
            await self.cancel()
            raise
        except (aiohttp.ClientError, asyncio.TimeoutError):
            await self.close()
            raise RuntimeError("CosyVoice connection failed") from None
        except Exception:
            await self.close()
            raise

    async def _send(self, action, payload):
        if self._ws is None or self._closed:
            raise RuntimeError("CosyVoice task is closed")
        try:
            async with asyncio.timeout(self._timeout_s):
                await self._ws.send_json({
                    "header": {"action": action, "task_id": self.task_id,
                               "streaming": "duplex"},
                    "payload": payload,
                })
        except asyncio.CancelledError:
            await self.cancel()
            raise
        except Exception:
            await self.close()
            raise RuntimeError("CosyVoice send failed") from None

    async def append_text(self, text):
        if not self._started or self._finished_input:
            raise RuntimeError("CosyVoice task does not accept text")
        if not isinstance(text, str):
            raise TypeError("CosyVoice text must be a string")
        if text:
            await self._send("continue-task", {"input": {"text": text}})

    async def finish(self):
        if not self._started:
            raise RuntimeError("CosyVoice task has not started")
        if not self._finished_input:
            await self._send("finish-task", {"input": {}})
            self._finished_input = True

    async def _receive(self):
        import aiohttp

        message = await self._ws.receive(timeout=self._timeout_s)
        if message.type == aiohttp.WSMsgType.BINARY:
            return bytes(message.data)
        if message.type != aiohttp.WSMsgType.TEXT:
            raise RuntimeError("CosyVoice stream ended before task completion")
        try:
            body = json.loads(message.data)
            header = body["header"]
            if not isinstance(header, dict):
                raise ValueError
            if header.get("task_id") != self.task_id:
                return None
            event = header.get("event")
            if event == "task-failed":
                raise RuntimeError("CosyVoice synthesis failed")
            return event
        except (json.JSONDecodeError, KeyError, TypeError, ValueError):
            raise RuntimeError("CosyVoice returned an invalid event") from None

    async def audio(self):
        import aiohttp

        if not self._started or self._closed or self._reading:
            raise RuntimeError("CosyVoice audio reader is unavailable")
        self._reading = True
        tail = b""
        try:
            while not self._closed:
                event = await self._receive()
                if isinstance(event, bytes):
                    pcm = tail + event
                    size = len(pcm) & ~1
                    if size:
                        yield pcm[:size]
                    tail = pcm[size:]
                elif event == "task-finished":
                    if not self._finished_input:
                        raise RuntimeError("CosyVoice finished before end of text")
                    if tail:
                        raise RuntimeError("CosyVoice returned an incomplete PCM sample")
                    return
        except (aiohttp.ClientError, asyncio.TimeoutError):
            raise RuntimeError("CosyVoice stream connection failed") from None
        finally:
            await self.close()

    async def cancel(self):
        self._cancel_requested.set()
        await self.close()

    async def close(self):
        if self._close_task is None:
            self._close_task = asyncio.create_task(self._shutdown())
        try:
            await asyncio.shield(self._close_task)
        except asyncio.CancelledError:
            self._cancel_requested.set()
            await asyncio.shield(self._close_task)
            raise

    async def _shutdown(self):
        self._closed = True
        if self._ws is not None:
            if self._cancel_requested.is_set():
                with suppress(Exception):
                    await asyncio.wait_for(self._ws.close(), CANCEL_CLOSE_SECONDS)
                return
            task = asyncio.create_task(asyncio.wait_for(self._ws.close(), min(self._timeout_s, 5)))
            wake = asyncio.create_task(self._cancel_requested.wait())
            try:
                await asyncio.wait((task, wake), return_when=asyncio.FIRST_COMPLETED)
                if not task.done():
                    task.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await task
                if self._cancel_requested.is_set() and not self._ws.closed:
                    with suppress(Exception):
                        await asyncio.wait_for(self._ws.close(), CANCEL_CLOSE_SECONDS)
            finally:
                wake.cancel()
                with suppress(asyncio.CancelledError):
                    await wake
