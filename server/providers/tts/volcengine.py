"""火山 v3 双向 TTS，使用原生 PCM 输出。

每个适配器管理一个 WebSocket 和一个合成会话。云端 Opus/Ogg 容器不直接
进入设备传输；本适配器请求 24 kHz PCM。字幕异常不应中断音频。
协议参考：https://docs.volcengine.com/docs/6561/1329505"""

import asyncio
import contextlib
import json
import logging
import math
import struct
import uuid
import zlib
from collections import deque
from dataclasses import dataclass, replace

from providers.tts.base import SubtitleCue, SubtitleUnavailable, SubtitleWord


DEFAULT_ENDPOINT = "wss://openspeech.bytedance.com/api/v3/tts/bidirection"
MAX_PAYLOAD_BYTES = 256 * 1024
MAX_BUFFERED_PCM_BYTES = 24000 * 2 * 4
CANCEL_SEND_SECONDS = 0.1
CANCEL_CLOSE_SECONDS = 0.15
MAX_SUBTITLE_WORDS = 16000
MAX_CUE_WORDS = 2048
MAX_SUBTITLE_TEXT_BYTES = 64000
MAX_SUBTITLE_SECONDS = 600
MEDIA_FRAME_BYTES = 24000 * 2 // 50  # 24 kHz 单声道 PCM16 的 20 ms 音频。
_U32 = struct.Struct(">I")
_LOG = logging.getLogger(__name__)
# 固定本地字段沿用实时命令行已有的有限诊断输出队列。
# 不要开启厂商或根日志的详细输出，也不要同步写控制台。
_TIMING_LOG = logging.getLogger("transport.voice_websocket.tts")


def decode_subtitle(payload, event):
    """解析文档定义的 TTS2.0 原文逐词信息，不猜测事件编号。

    公开事件表和示例枚举没有给出 TTSSubtitle 的数值。只有已经允许的下行
    TTS JSON 事件包含完整的文档字段结构时，才解析为字幕。时间以合成会话
    起点为零，单位为秒，包含静音；音频包到达时间不参与映射。"""
    # 本适配器只为 TTS2.0 启用字幕：350/351 表示合成边界，
    # 逐词对齐通过独立的 TTSSubtitle 事件传来。边界事件可选的
    # words/phonemes 字段不是字幕结构，可能为空/null，
    # 也可能描述旧版 TTS1.0 的规范化文字格式。
    if event in (350, 351):
        return None
    if not isinstance(payload, dict) or "words" not in payload:
        return None
    text, words = payload.get("text"), payload["words"]
    # 异步对齐结果暂时为空不代表字幕失败；继续等待
    # 后续携带实际逐词时间戳的事件。
    if isinstance(words, list) and not words:
        return None
    if (not isinstance(text, str) or len(text.encode("utf-8")) > MAX_SUBTITLE_TEXT_BYTES
            or not isinstance(words, list) or not 1 <= len(words) <= MAX_CUE_WORDS):
        raise ValueError("invalid_schema")
    parsed = []
    last_start = -1
    for word in words:
        if not isinstance(word, dict):
            raise ValueError("invalid_word")
        value, start, end = word.get("word"), word.get("startTime"), word.get("endTime")
        if (not isinstance(value, str) or not value or len(value.encode("utf-8")) > 1024
                or isinstance(start, bool) or isinstance(end, bool)
                or not isinstance(start, (int, float)) or not isinstance(end, (int, float))
                or not math.isfinite(start) or not math.isfinite(end)
                or not 0 <= start < end <= MAX_SUBTITLE_SECONDS or start < last_start):
            raise ValueError("invalid_timing")
        parsed.append(SubtitleWord(value, round(start * 24000), round(end * 24000)))
        last_start = start
    return SubtitleCue(text, tuple(parsed), event)


@dataclass(frozen=True)
class VolcMessage:
    message_type: int
    event: int | None
    scope_id: str | None
    payload: bytes | dict
    error_code: int | None = None


def encode_request(event, payload=None, *, session_id=None):
    """构造完整客户端 JSON 消息，标记为携带事件且不压缩。"""
    body = json.dumps({} if payload is None else payload, ensure_ascii=False,
                      separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_PAYLOAD_BYTES:
        raise RuntimeError("Volcengine TTS request exceeds the payload limit")
    packet = bytearray(b"\x11\x14\x10\x00")
    packet.extend(_U32.pack(event))
    if event not in (1, 2):
        if not session_id:
            raise RuntimeError("Volcengine TTS session identifier is required")
        encoded_id = session_id.encode("utf-8")
        if len(encoded_id) > 128:
            raise RuntimeError("Volcengine TTS session identifier is too long")
        packet.extend(_U32.pack(len(encoded_id)))
        packet.extend(encoded_id)
    packet.extend(_U32.pack(len(body)))
    packet.extend(body)
    return bytes(packet)


def decode_response(packet):
    """先校验二进制事件封包，再交出 PCM 或 JSON。"""
    if not isinstance(packet, bytes) or not 12 <= len(packet) <= MAX_PAYLOAD_BYTES + 512:
        raise RuntimeError("Invalid Volcengine TTS frame length")
    if packet[0] != 0x11 or packet[3] != 0:
        raise RuntimeError("Unsupported Volcengine TTS protocol header")
    message_type, flags = packet[1] >> 4, packet[1] & 15
    serialization, compression = packet[2] >> 4, packet[2] & 15
    if message_type not in (9, 11, 15) or compression not in (0, 1):
        raise RuntimeError("Unsupported Volcengine TTS frame format")
    if serialization not in (0, 1):
        raise RuntimeError("Unsupported Volcengine TTS serialization")
    position = 4

    def take_u32():
        nonlocal position
        if position + 4 > len(packet):
            raise RuntimeError("Truncated Volcengine TTS frame")
        result = _U32.unpack_from(packet, position)[0]
        position += 4
        return result

    def take_blob(limit):
        nonlocal position
        length = take_u32()
        if length > limit or position + length > len(packet):
            raise RuntimeError("Invalid Volcengine TTS field length")
        result = packet[position:position + length]
        position += length
        return result

    scope_id = None
    event = None
    error_code = None
    if message_type == 15:
        if flags != 0:
            raise RuntimeError("Unsupported Volcengine TTS error flags")
        error_code = take_u32()
    else:
        if flags != 4:
            raise RuntimeError("Volcengine TTS response is missing an event")
        event = take_u32()
        # 连接级服务端事件包含服务端分配的连接 ID；
        # 所有会话级和音频事件包含客户端分配的会话 ID。
        identifier = take_blob(256)
        try:
            scope_id = identifier.decode("utf-8")
        except UnicodeDecodeError:
            raise RuntimeError("Invalid Volcengine TTS scope identifier") from None
        if not scope_id:
            raise RuntimeError("Missing Volcengine TTS scope identifier")
    body = take_blob(MAX_PAYLOAD_BYTES)
    if position != len(packet):
        raise RuntimeError("Unexpected bytes after Volcengine TTS payload")
    if compression:
        try:
            decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
            body = decompressor.decompress(body, MAX_PAYLOAD_BYTES + 1)
            if len(body) > MAX_PAYLOAD_BYTES or not decompressor.eof or decompressor.unused_data:
                raise RuntimeError("Invalid or oversized Volcengine TTS compressed payload")
        except zlib.error:
            raise RuntimeError("Invalid Volcengine TTS compressed payload") from None
    if message_type == 15:
        # 不能保留或展示上游错误正文，其中可能回显密钥。
        return VolcMessage(message_type, None, None, {}, error_code)
    if message_type == 11:
        if event != 352 or serialization != 0:
            raise RuntimeError("Unsupported Volcengine TTS audio event")
        return VolcMessage(message_type, event, scope_id, body)
    if serialization != 1:
        raise RuntimeError("Volcengine TTS control payload must be JSON")
    try:
        result = json.loads(body)
    except (ValueError, UnicodeDecodeError):
        raise RuntimeError("Invalid Volcengine TTS JSON payload") from None
    if not isinstance(result, dict):
        raise RuntimeError("Volcengine TTS control payload must be an object")
    return VolcMessage(message_type, event, scope_id, result)


class VolcengineTtsProvider:
    sample_rate_hz = 24000
    channels = 1
    sample_width_bytes = 2
    codec = "pcm_s16le"
    capabilities = {"streaming_input": True, "streaming_output": True, "partial_results": False}

    def __init__(self, options, credentials, session):
        self.options = options
        self.credentials = credentials
        self.session = session
        self.session_id = str(uuid.uuid4())
        self.connect_id = str(uuid.uuid4())
        self._ws = None
        self._reader_task = None
        self._connection_started = None
        self._session_started = None
        self._connection_finished = None
        self._changed = asyncio.Event()
        self._space_available = asyncio.Event()
        self._chunks = deque()
        self._buffered = 0
        self._failure = None
        self._done = False
        self._started = False
        self._start_attempted = False
        self._finishing = False
        self._cancelled = False
        self._closed = False
        self._closing = False
        self._close_task = None
        self._cancel_requested = asyncio.Event()
        self._audio_claimed = False
        self.subtitles_enabled = (options.get("enable_subtitle", True) is True and
                                  options.get("resource_id", "seed-tts-2.0") == "seed-tts-2.0")
        self.subtitle_enabled = self.subtitles_enabled
        self._subtitle_words = set()
        self._subtitle_text_bytes = 0
        self._subtitle_cues = set()
        self._subtitle_failed = False
        self._subtitle_unavailable_pending = None
        self._subtitle_events = 0
        self._received_pcm_samples = 0
        self._last_subtitle_received_at = None
        self._audio_enqueue_wait_ms = 0.0
        self._audio_enqueue_wait_max_ms = 0.0
        self._send_lock = asyncio.Lock()
        self._timeout = float(options.get("timeout_s", 30))

    async def _send(self, event, payload=None):
        if self._ws is None or self._ws.closed:
            raise RuntimeError("Volcengine TTS connection is closed")
        packet = encode_request(event, payload, session_id=self.session_id if event >= 100 else None)
        try:
            async with self._send_lock:
                await asyncio.wait_for(self._ws.send_bytes(packet), self._timeout)
        except Exception:
            raise RuntimeError("Volcengine TTS send failed") from None

    def _headers(self):
        ref = self.options.get("credential_ref", "VOLCENGINE_TTS_API_KEY")
        key = self.credentials.get(ref)
        if not key:
            raise RuntimeError(f"Missing Volcengine TTS setting: {ref}")
        resource = self.options.get("resource_id", "seed-tts-2.0")
        if not resource or any(mark in resource for mark in ("<", ">", "\r", "\n")):
            raise RuntimeError("Volcengine TTS resource_id must be configured")
        headers = {"X-Api-Resource-Id": resource, "X-Api-Connect-Id": self.connect_id}
        app_ref = self.options.get("app_id_ref")
        if app_ref:
            app = self.credentials.get(app_ref)
            if not app:
                raise RuntimeError(f"Missing Volcengine TTS setting: {app_ref}")
            headers.update({"X-Api-App-Id": app, "X-Api-Access-Key": key})
        else:
            headers["X-Api-Key"] = key
        return headers

    async def start(self):
        if self._start_attempted or self._closed:
            raise RuntimeError("Volcengine TTS provider cannot be started again")
        self._start_attempted = True
        voice = self.options.get("voice")
        if not isinstance(voice, str) or not voice.strip() or "<" in voice or ">" in voice:
            raise RuntimeError("Volcengine TTS voice must be configured")
        headers = self._headers()
        loop = asyncio.get_running_loop()
        self._connection_started = loop.create_future()
        self._session_started = loop.create_future()
        self._connection_finished = loop.create_future()
        try:
            self._ws = await asyncio.wait_for(self.session.ws_connect(
                self.options.get("endpoint", DEFAULT_ENDPOINT), headers=headers,
                max_msg_size=MAX_PAYLOAD_BYTES + 512, heartbeat=20,
            ), self._timeout)
            if self._closed:
                await self._ws.close()
                raise RuntimeError("Volcengine TTS was closed while connecting")
            self._reader_task = asyncio.create_task(self._read())
            await self._send(1)
            await self._wait_ack(self._connection_started)
            await self._send(100, {
                "user": {"uid": "esp32-voice-assistant"},
                "event": 100,
                "namespace": "BidirectionalTTS",
                "req_params": {
                    "speaker": voice,
                    "audio_params": {"format": "pcm", "sample_rate": self.sample_rate_hz,
                                     **({"enable_subtitle": True} if self.subtitles_enabled else {})},
                },
            })
            await self._wait_ack(self._session_started)
            self._started = True
        except asyncio.CancelledError:
            await self.cancel()
            raise
        except Exception:
            await self.close()
            raise RuntimeError("Volcengine TTS session could not start") from None

    async def _wait_ack(self, future):
        try:
            await asyncio.wait_for(asyncio.shield(future), self._timeout)
        except TimeoutError:
            raise RuntimeError("Volcengine TTS acknowledgement timed out") from None
        if self._failure:
            raise RuntimeError(self._failure)

    async def append_text(self, text):
        if not self._started or self._finishing or self._closing or self._closed or self._done:
            raise RuntimeError("Volcengine TTS session is not accepting text")
        if not isinstance(text, str):
            raise TypeError("TTS text must be a string")
        if not text:
            return
        await self._send(200, {
            "user": {"uid": "esp32-voice-assistant"}, "event": 200,
            "namespace": "BidirectionalTTS", "req_params": {
                "text": text, "speaker": self.options["voice"],
                "audio_params": {"format": "pcm", "sample_rate": self.sample_rate_hz},
            },
        })

    async def finish(self):
        if self._finishing or self._closed or self._done:
            return
        if not self._started:
            raise RuntimeError("Volcengine TTS session has not started")
        self._finishing = True
        await self._send(102)

    def _end(self, error=None, *, discard=False):
        if error and not self._cancelled:
            self._failure = error
        self._done = True
        if discard:
            self._chunks.clear()
            self._buffered = 0
            self._subtitle_unavailable_pending = None
        for future in (self._connection_started, self._session_started):
            if future is not None and not future.done():
                future.set_result(None)
        self._changed.set()
        self._space_available.set()

    async def _enqueue_audio(self, chunk):
        # 硬件按实际时长播放，而合成服务可能一次生成数秒音频。
        # 此时暂停读取上游，以免丢弃 PCM，
        # 也不要拒绝只是消费较慢但仍正常工作的下游。
        if len(chunk) > MAX_BUFFERED_PCM_BYTES:
            raise RuntimeError("Volcengine TTS audio chunk exceeds four seconds")
        self._received_pcm_samples += len(chunk) // 2
        self._audio_enqueue_wait_ms = 0.0
        wait_started = None
        loop = asyncio.get_running_loop()
        try:
            while (not self._done and not self._closed and
                   (self._buffered + len(chunk) > MAX_BUFFERED_PCM_BYTES or len(self._chunks) >= 256)):
                if wait_started is None:
                    wait_started = loop.time()
                self._space_available.clear()
                try:
                    await asyncio.wait_for(self._space_available.wait(), self._timeout)
                except TimeoutError:
                    raise RuntimeError("Volcengine TTS audio consumer stalled") from None
        finally:
            if wait_started is not None:
                self._audio_enqueue_wait_ms = max(0.0, (loop.time() - wait_started) * 1000)
                self._audio_enqueue_wait_max_ms = max(
                    self._audio_enqueue_wait_max_ms, self._audio_enqueue_wait_ms)
                # 每个受阻 PCM 块最多记录一行，且等待至少达到一个媒体帧。
                # 只报告共享读取任务的停顿，不记录 PCM。
                if self._audio_enqueue_wait_ms >= 20.0:
                    _TIMING_LOG.info(
                        "tts_audio_backpressure source=%s wait_ms=%.1f wait_max_ms=%.1f "
                        "buffered_pcm_ms=%.1f received_pcm_samples=%d",
                        self.session_id, self._audio_enqueue_wait_ms,
                        self._audio_enqueue_wait_max_ms, self._buffered / 48.0,
                        self._received_pcm_samples)
        if self._done or self._closed:
            return
        self._chunks.append(chunk)
        self._buffered += len(chunk)
        self._changed.set()

    def _enqueue_subtitle(self, payload, event):
        if not self.subtitles_enabled or self._subtitle_failed or self._done or self._cancelled:
            return
        self._subtitle_events += 1
        received_at = asyncio.get_running_loop().time()
        try:
            cue = decode_subtitle(payload, event)
            if cue is None:
                return
            cue_key = cue.fingerprint()
            if cue_key in self._subtitle_cues:
                return
            word_keys = {(word.word, word.start_sample, word.end_sample) for word in cue.words}
            fresh_keys = word_keys - self._subtitle_words
            new_word_bytes = sum(len(key[0].encode("utf-8")) for key in fresh_keys)
            if (len(self._subtitle_words | word_keys) > MAX_SUBTITLE_WORDS
                    or self._subtitle_text_bytes + new_word_bytes > MAX_SUBTITLE_TEXT_BYTES
                    or len(self._subtitle_cues) >= 1024 or len(self._chunks) >= 256):
                raise ValueError("limit")
            self._subtitle_cues.add(cue_key)
            self._subtitle_words.update(word_keys)
            self._subtitle_text_bytes += new_word_bytes
            cue = replace(cue, received_at=received_at, source_id=self.session_id)
            self._chunks.append(cue)
            self._changed.set()
            gap_ms = (-1.0 if self._last_subtitle_received_at is None else
                      max(0.0, (received_at - self._last_subtitle_received_at) * 1000))
            self._last_subtitle_received_at = received_at
            _TIMING_LOG.info(
                "tts_subtitle_timing source=%s upstream_event=%d cue=%d words=%d "
                "received_pcm_samples=%d buffered_pcm_ms=%.1f rx_gap_ms=%.1f "
                "audio_enqueue_wait_ms=%.1f audio_enqueue_wait_max_ms=%.1f",
                self.session_id, event, len(self._subtitle_cues), len(cue.words),
                self._received_pcm_samples, self._buffered / 48.0, gap_ms,
                self._audio_enqueue_wait_ms, self._audio_enqueue_wait_max_ms)
        except ValueError as error:
            # 可选元数据不能破坏正常音频响应。下游有界等待
            # 会明确报告不可跟随，而不是猜测时间。
            self._subtitle_failed = True
            detail = str(error)
            if detail not in {"invalid_schema", "invalid_word", "invalid_timing", "limit"}:
                detail = "invalid_utf8"
            self._subtitle_unavailable_pending = SubtitleUnavailable(
                detail=detail, upstream_event=event, upstream_events=self._subtitle_events)
            self._changed.set()
            _LOG.warning("tts_subtitle_invalid event=%d reason=%s", event, detail)

    async def _read(self):
        try:
            while not self._closed:
                try:
                    message = await asyncio.wait_for(self._ws.receive(), self._timeout)
                except TimeoutError:
                    raise
                except Exception:
                    raise RuntimeError("Volcengine TTS receive failed") from None
                # aiohttp WSMsgType.BINARY == 2。其他意外消息类型视为错误；
                # 自动心跳处理使 ping/pong 不进入此循环。
                if message.type != 2:
                    if self._done:
                        return
                    raise RuntimeError("Volcengine TTS connection ended before completion")
                response = decode_response(message.data)
                if response.error_code is not None:
                    raise RuntimeError(f"Volcengine TTS upstream error code {response.error_code}")
                event = response.event
                if event in (50, 51, 52):
                    if event == 51:
                        raise RuntimeError("Volcengine TTS connection was rejected")
                    if event == 50:
                        if self._connection_started.done():
                            raise RuntimeError("Duplicate Volcengine TTS connection acknowledgement")
                        self._connection_started.set_result(None)
                    else:
                        if not self._done:
                            raise RuntimeError("Volcengine TTS connection finished unexpectedly")
                        self._connection_finished.set_result(None)
                        return
                    continue
                if response.scope_id != self.session_id:
                    raise RuntimeError("Volcengine TTS session identifier mismatch")
                if isinstance(response.payload, dict):
                    code = response.payload.get("status_code", 20000000)
                    if code not in (0, 20000000):
                        raise RuntimeError("Volcengine TTS upstream session failed")
                if event == 150:
                    if self._session_started.done():
                        raise RuntimeError("Duplicate Volcengine TTS session acknowledgement")
                    self._session_started.set_result(None)
                elif event == 153:
                    raise RuntimeError("Volcengine TTS session was rejected")
                elif event == 152:
                    if not self._finishing:
                        raise RuntimeError("Volcengine TTS finished before text input ended")
                    self._end()
                elif event == 151:
                    if not self._cancelled:
                        raise RuntimeError("Volcengine TTS session was cancelled upstream")
                    self._end(discard=True)
                elif event == 352:
                    if self._done or self._cancelled:
                        continue
                    if not self._session_started.done():
                        raise RuntimeError("Volcengine TTS audio arrived before session start")
                    chunk = response.payload
                    if len(chunk) % 2:
                        raise RuntimeError("Volcengine TTS returned incomplete PCM16 samples")
                    if chunk:
                        await self._enqueue_audio(chunk)
                elif response.message_type == 9 and 350 <= event <= 399:
                    self._enqueue_subtitle(response.payload, event)
                elif not (response.message_type == 9 and event == 154):
                    raise RuntimeError("Unexpected Volcengine TTS event")
        except asyncio.CancelledError:
            raise
        except TimeoutError:
            self._end("Volcengine TTS receive timed out", discard=True)
        except RuntimeError as exc:
            self._end(str(exc), discard=True)
        except Exception:
            self._end("Volcengine TTS receive failed", discard=True)

    async def media(self):
        """PCM 与元数据共用一个消费者，每次输出的媒体大小有上限。

        上游可能先返回数秒长的 PCM，再返回对应逐词时间事件。如果把整段
        PCM 一次交给下游，传输层就要等这段音频播完后才能看到时间事件。
        保持原有的有限队列，但将 PCM 按 20 ms 切片输出，让已经入队的字幕
        在切片之间优先于剩余音频尾部输出。"""
        if self._audio_claimed:
            raise RuntimeError("Volcengine TTS audio/media supports one consumer")
        self._audio_claimed = True
        while True:
            if self._subtitle_unavailable_pending:
                unavailable = self._subtitle_unavailable_pending
                self._subtitle_unavailable_pending = None
                yield unavailable
            while self._chunks:
                if self._subtitle_unavailable_pending:
                    unavailable = self._subtitle_unavailable_pending
                    self._subtitle_unavailable_pending = None
                    yield unavailable
                # 时间元数据是可选的，但一旦收到，
                # 就不能排在已缓存的 PCM 尾部之后等待。
                cue_index = next((index for index, item in enumerate(self._chunks)
                                  if isinstance(item, SubtitleCue)), None)
                if cue_index is not None:
                    self._chunks.rotate(-cue_index)
                    cue = self._chunks.popleft()
                    self._chunks.rotate(cue_index)
                    yield cue
                    continue

                chunk = self._chunks.popleft()
                if not isinstance(chunk, bytes):
                    raise RuntimeError("Volcengine TTS media queue contains invalid data")
                if len(chunk) > MEDIA_FRAME_BYTES:
                    head, tail = chunk[:MEDIA_FRAME_BYTES], chunk[MEDIA_FRAME_BYTES:]
                    self._chunks.appendleft(tail)
                    chunk = head
                self._buffered -= len(chunk)
                self._space_available.set()
                yield chunk
            if self._done:
                if self._failure:
                    raise RuntimeError(self._failure)
                return
            self._changed.clear()
            await self._changed.wait()

    async def audio(self):
        """兼容不消费字幕的调用者。"""
        async for item in self.media():
            if isinstance(item, bytes):
                yield item

    async def cancel(self):
        self._cancelled = True
        self._end(discard=True)
        # 同时唤醒已经开始的正常关闭等待：回答可能生成完毕时，
        # 设备上仍缓存着尚未播放的 PCM。
        self._cancel_requested.set()
        await self.close()

    async def close(self):
        if self._close_task is None:
            self._close_task = asyncio.create_task(self._shutdown())
        try:
            await asyncio.shield(self._close_task)
        except asyncio.CancelledError:
            self._cancelled = True
            self._end(discard=True)
            self._cancel_requested.set()
            await asyncio.shield(self._close_task)
            raise

    async def _until_cancel(self, operation):
        """抢占正常关闭等待，并等待该操作退出后再返回。"""
        task = asyncio.create_task(operation)
        wake = asyncio.create_task(self._cancel_requested.wait())
        try:
            await asyncio.wait((task, wake), return_when=asyncio.FIRST_COMPLETED)
            if not task.done():
                task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task
        finally:
            wake.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await wake

    async def _shutdown(self):
        graceful = self._done and not self._failure
        self._closing = True
        self._end(discard=True)

        async def finish_connection():
            await asyncio.wait_for(self._send(2), min(self._timeout, 1))
            if self._connection_finished is not None:
                await asyncio.wait_for(asyncio.shield(self._connection_finished), min(self._timeout, 1))

        if self._ws is not None and not self._ws.closed:
            if graceful and not self._cancelled:
                await self._until_cancel(finish_connection())
        # aiohttp 关闭会等待并行的接收操作退出。先停止并回收
        # 本模块的接收任务，再关闭连接，避免先消耗这段等待时间。
        task = self._reader_task
        if task is not None and task is not asyncio.current_task():
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task
        if self._ws is not None and not self._ws.closed:
            if self._cancelled:
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(self._send(101), CANCEL_SEND_SECONDS)
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(self._ws.close(), CANCEL_CLOSE_SECONDS)
            else:
                await self._until_cancel(asyncio.wait_for(self._ws.close(), 2))
                # 取消 aiohttp 关闭会中止传输。兼容包装层在被抢占后，
                # 可能仍需要最后一次有超时限制的关闭。
                if self._cancelled and not self._ws.closed:
                    with contextlib.suppress(Exception):
                        await asyncio.wait_for(self._ws.close(), CANCEL_CLOSE_SECONDS)
        self._closed = True
