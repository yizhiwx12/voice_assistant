"""豆包流式 ASR 2.0，通过 V3 二进制 API 发送 16 kHz 单声道 PCM16。

数据格式和请求头依据官方双向流式识别文档及公开 Python 协议示例
（核对于 2026-11-09）。每个实例只处理一轮发言，分句完成不等于用户说完；
结束由设备显式提交。异常中不包含请求、音频正文或远端错误描述。"""

import asyncio
import contextlib
import gzip
import io
import json
import struct
import uuid
from dataclasses import dataclass
from urllib.parse import urlsplit

from providers._http import require_credential


DEFAULT_ENDPOINT = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
MAX_RESPONSE_BYTES = 1_048_576


class DoubaoAsrError(RuntimeError):
    """使用机器可读的诊断字段，不记录上游文字。"""
    REASONS = frozenset({
        "none", "unknown", "invalid_response", "upstream_rejected",
        "upstream_closed", "unexpected_frame", "receive_timeout", "input_timeout",
        "receive_failed", "send_timeout", "send_failed", "connection_failed",
        "input_not_open", "duration_limit", "final_timeout", "consumer_timeout",
        "response_processing_failed", "canceled", "closed_before_final",
    })

    def __init__(self, message, *, reason="unknown", upstream_code=0, upstream_close_code=0):
        super().__init__(message)
        self.reason = reason
        self.upstream_code = upstream_code
        self.upstream_close_code = upstream_close_code

    def diagnostics(self):
        # 在输出边界再次校验，避免调用者修改属性后，
        # 把固定字段诊断变成任意远端内容。
        reason = self.reason if type(self.reason) is str and self.reason in self.REASONS else "unknown"
        code = self.upstream_code
        close = self.upstream_close_code
        return {
            "asr_reason": reason,
            "asr_upstream_code": code if type(code) is int and 0 <= code <= 0xFFFF_FFFF else 0,
            "asr_upstream_close_code": close if type(close) is int and 0 <= close <= 0xFFFF else 0,
        }


class _AudioPacer:
    """按样本时钟控制发送节奏，追赶速度最多为正常速度的 1 / 0.9 倍。

    网络发送完成的时刻不能作为下一包音频的截止时间。迟到的数据包可适度
    追赶，但停顿后不能突发发送积压数据。可注入的时钟和等待函数也支持
    长时间、确定性的传输模拟。"""
    def __init__(self, clock=None, sleep=asyncio.sleep):
        self.clock = clock or (lambda: asyncio.get_running_loop().time())
        self.sleep = sleep
        self.origin = None
        self.samples = 0
        self.last_started = None
        self.last_duration = 0.0
        self.packets = 0
        self.max_lag_ms = 0.0
        self.max_send_ms = 0.0

    async def wait(self):
        now = self.clock()
        if self.origin is None:
            self.origin = now
        audio_deadline = self.origin + self.samples / 16000
        deadline = audio_deadline
        if self.last_started is not None:
            deadline = max(deadline, self.last_started + self.last_duration * 0.9)
        while deadline > now:
            await self.sleep(deadline - now)
            now = self.clock()
        self.max_lag_ms = max(self.max_lag_ms, (now - audio_deadline) * 1000)
        return now

    def sent(self, started, sample_count):
        self.last_started = started
        self.last_duration = sample_count / 16000
        self.samples += sample_count
        self.packets += 1
        self.max_send_ms = max(self.max_send_ms, (self.clock() - started) * 1000)

    def metrics(self):
        return {"packets": self.packets, "samples": self.samples,
                "max_lag_ms": round(self.max_lag_ms, 1),
                "max_send_ms": round(self.max_send_ms, 1)}


@dataclass(frozen=True)
class AsrEvent:
    kind: str
    text: str
    segment_id: str | None = None


@dataclass(frozen=True)
class ServerResponse:
    sequence: int | None
    is_last: bool
    event: int | None
    payload: dict


def encode_request(payload: bytes, sequence: int, *, audio=False, last=False) -> bytes:
    """序列化一个 gzip 载荷；正常序号为正，音频结束时序号为负。"""
    if not 1 <= sequence <= 0x7FFF_FFFF or (last and not audio):
        raise ValueError("Invalid Doubao request sequence or terminal flag")
    body = gzip.compress(payload, mtime=0)
    # 官方 ASR 音频请求头仍将序列化类型标记为 JSON；
    # 消息类型用于区分原始 PCM 载荷与完整 JSON 请求。
    header = bytes((0x11, (0x20 if audio else 0x10) | (3 if last else 1), 0x11, 0))
    return header + struct.pack(">iI", -sequence if last else sequence, len(body)) + body


def parse_response(packet: bytes) -> ServerResponse:
    """解析有大小限制的 V3 服务端帧，包括可选的序号和事件字段。"""
    if len(packet) < 8 or len(packet) > MAX_RESPONSE_BYTES:
        raise DoubaoAsrError("Doubao ASR returned an invalid frame length", reason="invalid_response")
    version, words = packet[0] >> 4, packet[0] & 15
    message_type, flags = packet[1] >> 4, packet[1] & 15
    serialization, compression = packet[2] >> 4, packet[2] & 15
    offset = words * 4
    if version != 1 or words < 1 or offset > len(packet) or flags & 8:
        raise DoubaoAsrError("Doubao ASR returned an unsupported frame header", reason="invalid_response")
    if message_type not in (9, 15) or compression not in (0, 1):
        raise DoubaoAsrError("Doubao ASR returned an unsupported frame type", reason="invalid_response")

    def number(signed=False):
        nonlocal offset
        if len(packet) - offset < 4:
            raise DoubaoAsrError("Doubao ASR returned a truncated frame", reason="invalid_response")
        value = struct.unpack_from(">i" if signed else ">I", packet, offset)[0]
        offset += 4
        return value

    sequence = number(True) if flags & 1 else None
    event = number(True) if flags & 4 else None
    code = number() if message_type == 15 else 0
    size = number()
    if size != len(packet) - offset:
        raise DoubaoAsrError("Doubao ASR returned an invalid payload length", reason="invalid_response")
    if message_type == 15:
        raise DoubaoAsrError(f"Doubao ASR rejected the request (code {code})",
                             reason="upstream_rejected", upstream_code=code)
    payload = packet[offset:]
    if compression:
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(payload)) as source:
                payload = source.read(MAX_RESPONSE_BYTES + 1)
        except (OSError, EOFError, ValueError):
            raise DoubaoAsrError("Doubao ASR returned invalid compressed data", reason="invalid_response") from None
    if len(payload) > MAX_RESPONSE_BYTES or serialization != 1:
        raise DoubaoAsrError("Doubao ASR returned an unsupported payload", reason="invalid_response")
    try:
        result = json.loads(payload)
    except (ValueError, UnicodeError):
        raise DoubaoAsrError("Doubao ASR returned invalid JSON", reason="invalid_response") from None
    if not isinstance(result, dict):
        raise DoubaoAsrError("Doubao ASR returned an invalid response object", reason="invalid_response")
    body_code = result.get("code", 0)
    if type(body_code) is not int or body_code not in (0, 1000, 20000000):
        # 上游响应正文中只有数字状态码可以安全地用于诊断。
        safe_code = body_code if type(body_code) is int and 0 <= body_code <= 0xFFFF_FFFF else 0
        suffix = f" (code {safe_code})" if safe_code else ""
        raise DoubaoAsrError("Doubao ASR rejected the request" + suffix,
                             reason="upstream_rejected", upstream_code=safe_code)
    return ServerResponse(sequence, bool(flags & 2) or (sequence is not None and sequence < 0), event, result)


class DoubaoStreamingAsrProvider:
    capabilities = {
        "streaming_input": True,
        "streaming_output": True,
        "partial_results": True,
        "endpointing": "explicit_end",
        "audio_format": "pcm_s16le_mono_16000",
    }

    def __init__(self, options, credentials, session=None):
        self.options = options
        self.credentials = credentials
        self._session = session
        self._owns_session = session is None
        self._socket = None
        self._reader = None
        self._started = False
        self._closed = False
        self._ending = False
        self._done = asyncio.Event()
        self._send_lock = asyncio.Lock()
        self._events = asyncio.Queue(maxsize=64)
        self._error = None
        self._sequence = 1
        self._pending = bytearray()
        self._text = ""
        self._seen_segments = set()
        self._final_emitted = False
        self._response_count = 0
        self._nonempty_response_count = 0
        self._partial_event_count = 0
        self._segment_event_count = 0
        self._final_received = False
        self._final_chars = 0
        self._send_inflight_started = None
        self._max_send_inflight_ms = 0.0
        self._input_bytes = 0
        self._input_started_at = None
        self._last_audio_sent_at = None
        self._pacer = _AudioPacer()
        self._timeout = self._positive_number("timeout_s", 30, maximum=300)
        self._max_seconds = self._positive_number("max_utterance_s", 60, maximum=300)
        self._chunk_ms = options.get("chunk_duration_ms", 100)
        if type(self._chunk_ms) is not int or self._chunk_ms not in (100, 200):
            raise ValueError("Doubao ASR chunk_duration_ms must be 100 or 200")
        self._chunk_bytes = self._chunk_ms * 32
        if options.get("model", "bigmodel") != "bigmodel":
            raise ValueError("Doubao streaming ASR requires model 'bigmodel'")
        endpoint = options.get("endpoint", DEFAULT_ENDPOINT)
        parsed = urlsplit(endpoint)
        if parsed.scheme != "wss" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
            raise ValueError("Doubao ASR endpoint must be a secure WebSocket URL")
        self._endpoint = endpoint

    def _positive_number(self, name, default, maximum):
        value = self.options.get(name, default)
        if type(value) not in (int, float) or not 0 < value <= maximum:
            raise ValueError(f"Doubao ASR {name} is outside the supported range")
        return value

    async def start(self):
        if self._started or self._closed:
            raise DoubaoAsrError("Doubao ASR instance cannot be restarted", reason="input_not_open")
        key = require_credential(self.credentials, self.options.get("credential_ref", "VOLCENGINE_ASR_API_KEY"))
        resource = self.options.get("resource_id", "volc.seedasr.sauc.duration")
        if resource not in ("volc.seedasr.sauc.duration", "volc.seedasr.sauc.concurrent"):
            raise ValueError("Doubao ASR 2.0 resource_id must select duration or concurrent")
        try:
            if self._session is None:
                import aiohttp
                self._session = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=None, sock_connect=self._timeout))
            self._socket = await asyncio.wait_for(self._session.ws_connect(
                self._endpoint,
                headers={"X-Api-Key": key, "X-Api-Resource-Id": resource, "X-Api-Request-Id": str(uuid.uuid4())},
                heartbeat=20,
                max_msg_size=MAX_RESPONSE_BYTES,
            ), self._timeout)
            request_options = {
                "model_name": "bigmodel", "enable_itn": True,
                "enable_punc": True, "show_utterances": True,
                "result_type": "full",
            }
            for name in ("enable_nonstream", "enable_ddc"):
                if name in self.options:
                    if type(self.options[name]) is not bool:
                        raise ValueError(f"Doubao ASR {name} must be a boolean")
                    request_options[name] = self.options[name]
            if "end_window_size" in self.options:
                value = self.options["end_window_size"]
                if type(value) is not int or not 300 <= value <= 5000:
                    raise ValueError("Doubao ASR end_window_size must be 300..5000 ms")
                request_options["end_window_size"] = value
            payload = {
                "user": {"uid": "voice-assistant"},
                "audio": {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
                "request": request_options,
            }
            await self._send(encode_request(json.dumps(payload).encode(), self._sequence))
            self._sequence += 1
            response = await self._receive_response()
            await self._handle_response(response)
            if self._done.is_set():
                raise DoubaoAsrError("Doubao ASR ended before audio input", reason="input_not_open")
            self._started = True
            self._input_started_at = asyncio.get_running_loop().time()
            self._reader = asyncio.create_task(self._read_responses(), name="doubao-asr-reader")
        except asyncio.CancelledError:
            await self.cancel()
            raise
        except (ValueError, DoubaoAsrError):
            await self.close()
            raise
        except Exception:
            await self.close()
            raise DoubaoAsrError("Doubao ASR connection or handshake failed", reason="connection_failed") from None

    async def _send(self, packet):
        started = asyncio.get_running_loop().time()
        self._send_inflight_started = started
        try:
            await asyncio.wait_for(self._socket.send_bytes(packet), self._timeout)
        except asyncio.CancelledError:
            raise
        except TimeoutError:
            raise DoubaoAsrError("Doubao ASR send failed or timed out", reason="send_timeout") from None
        except Exception:
            raise DoubaoAsrError("Doubao ASR send failed or timed out", reason="send_failed") from None
        finally:
            elapsed = (asyncio.get_running_loop().time() - started) * 1000
            self._send_inflight_started = None
            self._max_send_inflight_ms = max(self._max_send_inflight_ms, elapsed)

    async def _receive_message(self):
        if not self._started:
            # 必须收到初始响应才能完成握手。
            return await asyncio.wait_for(self._socket.receive(), self._timeout)

        # 背景噪声或静音可能不会产生任何 ASR 识别消息。
        # 音频持续成功发送时，保持同一个接收操作；
        # 不要在每次活动截止时间到达时取消并重启 aiohttp.receive()。
        receive = asyncio.create_task(self._socket.receive())
        try:
            while True:
                if self._ending:
                    # 发送音频结束标记有独立的发送超时。发送成功后，
                    # end_utterance() 为最终结果提供一份完整的新等待预算。
                    return await receive
                reference = (self._last_audio_sent_at if self._last_audio_sent_at is not None
                             else self._input_started_at)
                remaining = reference + self._timeout - asyncio.get_running_loop().time()
                if remaining <= 0:
                    raise DoubaoAsrError("Doubao ASR audio input stalled", reason="input_timeout")
                done, _ = await asyncio.wait((receive,), timeout=remaining)
                if receive in done:
                    return receive.result()
                # 接收等待期间，音频发送可能已经延后了截止时间。
        finally:
            if not receive.done():
                receive.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await receive

    async def _receive_response(self):
        try:
            message = await self._receive_message()
        except asyncio.CancelledError:
            raise
        except DoubaoAsrError:
            raise
        except TimeoutError:
            raise DoubaoAsrError("Doubao ASR receive failed or timed out", reason="receive_timeout") from None
        except Exception:
            raise DoubaoAsrError("Doubao ASR receive failed or timed out", reason="receive_failed") from None
        # aiohttp.WSMsgType.BINARY == 2；在此直接使用常量，
        # 使仅解析协议的测试或导入无需安装 HTTP 客户端。
        if message.type != 2:
            # CLOSE/CLOSING/CLOSED 不携带识别数据。不能透传其自由文本原因，
            # 也不能透传 ERROR.data，因为它可能是异常对象。
            if message.type in (8, 256, 257):
                close = message.data if message.type == 8 else getattr(self._socket, "close_code", 0)
                raise DoubaoAsrError("Doubao ASR connection closed or returned a non-binary frame",
                                     reason="upstream_closed", upstream_close_code=close)
            reason = "receive_failed" if message.type == 258 else "unexpected_frame"
            raise DoubaoAsrError("Doubao ASR connection closed or returned a non-binary frame", reason=reason)
        response = parse_response(message.data)
        self._response_count += 1
        return response

    async def feed_audio(self, pcm):
        if not isinstance(pcm, bytes) or len(pcm) % 2 or len(pcm) > 32000:
            raise ValueError("Doubao ASR input must be even-length PCM16, at most one second per call")
        async with self._send_lock:
            self._require_input_open()
            if self._input_bytes + len(pcm) > self._max_seconds * 32000:
                raise DoubaoAsrError("Doubao ASR utterance duration limit exceeded", reason="duration_limit")
            self._input_bytes += len(pcm)
            self._pending.extend(pcm)
            while len(self._pending) >= self._chunk_bytes:
                chunk = bytes(self._pending[:self._chunk_bytes])
                del self._pending[:self._chunk_bytes]
                await self._send_audio(chunk, last=False)

    def _require_input_open(self):
        if self._error:
            raise self._error
        if not self._started or self._closed or self._ending or self._done.is_set():
            raise DoubaoAsrError("Doubao ASR input stream is not open", reason="input_not_open")

    async def _send_audio(self, pcm, *, last):
        started = await self._pacer.wait()
        if self._error:
            raise self._error
        if self._closed or self._done.is_set():
            raise DoubaoAsrError("Doubao ASR input stream is not open", reason="input_not_open")
        await self._send(encode_request(pcm, self._sequence, audio=True, last=last))
        if pcm and not last:
            self._last_audio_sent_at = asyncio.get_running_loop().time()
        self._sequence += 1
        self._pacer.sent(started, len(pcm) // 2)

    def pacing_metrics(self):
        """仅返回数字形式的传输指标，不包含音频或识别文字。"""
        metrics = self._pacer.metrics()
        started = self._send_inflight_started
        elapsed = ((asyncio.get_running_loop().time() - started) * 1000
                   if started is not None else 0.0)
        metrics.update({
            "send_inflight": int(started is not None),
            "send_inflight_ms": elapsed,
            "max_send_inflight_ms": max(self._max_send_inflight_ms, elapsed),
        })
        return metrics

    def failure_diagnostics(self):
        """读取任务的错误可能先于下一次音频输入到达事件消费者。"""
        if isinstance(self._error, DoubaoAsrError):
            return self._error.diagnostics()
        return {"asr_reason": "none", "asr_upstream_code": 0, "asr_upstream_close_code": 0}

    def recognition_metrics(self):
        """提供固定的数字识别进度，不包含转写文字或载荷。

        响应数包括成功解析的握手。非空响应数统计聚合结果文字，包含重复结果；
        事件数则统计修订和分句去重后成功入队的次数。即使最终结果为空，
        也能区分已经收到最终结果与尚未收到最终结果。"""
        return {
            "responses": self._response_count,
            "nonempty_responses": self._nonempty_response_count,
            "partial_events": self._partial_event_count,
            "segment_events": self._segment_event_count,
            "final_received": int(self._final_received),
            "final_chars": self._final_chars,
        }

    async def end_utterance(self):
        async with self._send_lock:
            if self._error:
                raise self._error
            if not self._started or self._closed:
                raise DoubaoAsrError("Doubao ASR input stream is not open", reason="input_not_open")
            if not self._ending and not self._done.is_set():
                self._require_input_open()
                self._ending = True
                tail = bytes(self._pending)
                self._pending.clear()
                await self._send_audio(tail, last=True)
        try:
            await asyncio.wait_for(self._done.wait(), self._timeout)
        except TimeoutError:
            await self.cancel()
            raise DoubaoAsrError("Doubao ASR final result timed out", reason="final_timeout") from None
        if self._error:
            raise self._error
        return self._text

    async def _emit(self, event):
        try:
            await asyncio.wait_for(self._events.put(event), self._timeout)
        except TimeoutError:
            raise DoubaoAsrError("Doubao ASR event consumer is too slow", reason="consumer_timeout") from None
        if event.kind == "partial":
            self._partial_event_count += 1
        elif event.kind == "segment_final":
            self._segment_event_count += 1

    async def _handle_response(self, response):
        payload = response.payload
        result = payload.get("result", {})
        results = result if isinstance(result, list) else [result]
        if not all(isinstance(item, dict) for item in results):
            raise DoubaoAsrError("Doubao ASR returned an invalid recognition result", reason="invalid_response")
        texts = [item.get("text", "") for item in results]
        if not all(isinstance(text, str) for text in texts):
            raise DoubaoAsrError("Doubao ASR returned non-text recognition", reason="invalid_response")
        text = "".join(texts)
        if len(text) > 32768:
            raise DoubaoAsrError("Doubao ASR transcript limit exceeded", reason="invalid_response")
        if text:
            self._nonempty_response_count += 1
        if any("text" in item for item in results) and text != self._text:
            self._text = text
            if not response.is_last:
                await self._emit(AsrEvent("partial", text))
        for item in results:
            utterances = item.get("utterances", [])
            if not isinstance(utterances, list):
                raise DoubaoAsrError("Doubao ASR returned invalid utterances", reason="invalid_response")
            for utterance in utterances:
                if not isinstance(utterance, dict) or utterance.get("definite") is not True:
                    continue
                segment_text = utterance.get("text", "")
                if not isinstance(segment_text, str) or len(segment_text) > 32768:
                    raise DoubaoAsrError("Doubao ASR returned invalid segment text", reason="invalid_response")
                segment_id = f"{utterance.get('start_time', '')}:{utterance.get('end_time', '')}"
                identity = (segment_id, segment_text)
                if identity not in self._seen_segments:
                    if len(self._seen_segments) >= 1024:
                        raise DoubaoAsrError("Doubao ASR segment limit exceeded", reason="invalid_response")
                    self._seen_segments.add(identity)
                    await self._emit(AsrEvent("segment_final", segment_text, segment_id))
        if response.is_last and not self._final_emitted:
            self._final_received = True
            self._final_chars = len(self._text)
            self._final_emitted = True
            await self._emit(AsrEvent("utterance_final", self._text))
            self._done.set()

    async def _read_responses(self):
        try:
            while not self._done.is_set():
                await self._handle_response(await self._receive_response())
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._error = exc if isinstance(exc, DoubaoAsrError) else DoubaoAsrError(
                "Doubao ASR response processing failed", reason="response_processing_failed")
            self._done.set()

    async def events(self):
        """单次消费并与音频处理并行运行；向上传递已脱敏的错误。"""
        while not self._done.is_set() or not self._events.empty():
            try:
                event = await asyncio.wait_for(self._events.get(), 0.1)
            except TimeoutError:
                continue
            yield event
        if self._error:
            raise self._error

    async def cancel(self):
        if not self._done.is_set():
            self._error = DoubaoAsrError("Doubao ASR canceled", reason="canceled")
        await self.close()

    async def close(self):
        if self._closed:
            return
        self._closed = True
        if self._started and not self._done.is_set() and self._error is None:
            self._error = DoubaoAsrError("Doubao ASR closed before final result", reason="closed_before_final")
        self._pending.clear()
        self._done.set()
        if self._reader and self._reader is not asyncio.current_task():
            self._reader.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._reader
        if self._socket:
            with contextlib.suppress(Exception):
                await asyncio.wait_for(self._socket.close(), self._timeout)
        if self._owns_session and self._session:
            with contextlib.suppress(Exception):
                await asyncio.wait_for(self._session.close(), self._timeout)
