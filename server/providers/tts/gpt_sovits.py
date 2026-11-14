"""本地 GPT-SoVITS HTTP TTS，输出统一为 24 kHz 单声道 PCM16。

本地曼波使用旧版 POST / 接口，每次接受完整文字并返回一个完整 WAV。
finish() 后按大小受限的文本段顺序合成；播放当前段时最多预取下一段。"""

import asyncio
from array import array
import errno
from io import BytesIO
import logging
import math
import sys
import wave
from urllib.parse import urlsplit

from providers.tts.base import SubtitleCue, SubtitleWord


DEFAULT_ENDPOINT = "http://127.0.0.1:9880"
TARGET_SAMPLE_RATE = 24000
MAX_TEXT_CHARS = 16000
DEFAULT_MAX_AUDIO_BYTES = 32 * 1024 * 1024
_LOG = logging.getLogger("transport.voice_websocket.tts.gpt_sovits")


def _failure(reason, *, http_status=0):
    # 诊断只使用固定的本地原因分类和数字状态码。
    # 不能格式化输出上游异常、URL、文字、路径或响应正文。
    _LOG.warning("tts_failure adapter=gpt_sovits tts_reason=%s tts_http_status=%d",
                 reason, http_status)
    return RuntimeError(f"GPT-SoVITS {reason}")


def _split_text(text, first_limit=40, segment_limit=80):
    """保持文字顺序，优先自然断句，并遵守首段和后续段的硬上限。"""
    for limit in (first_limit, segment_limit):
        if type(limit) is not int or not 8 <= limit <= 200:
            raise _failure("invalid_segment_limit")
    pending = text.strip()
    segments = []
    while pending:
        limit = segment_limit if segments else first_limit
        end = min(limit, len(pending))
        if len(pending) > limit:
            minimum = min(16, limit // 2)
            # 避免为简短的开场语气词单独发起请求。
            sentences = [index + 1 for index, char in enumerate(pending[:limit])
                         if char in "。！？!?；;\n" and index + 1 >= minimum]
            pauses = [index + 1 for index, char in enumerate(pending[:limit])
                      if (char in "，,、：:" or char.isspace()) and index + 1 >= minimum]
            end = sentences[0] if sentences else (pauses[-1] if pauses else limit)
            while end < limit and pending[end] in '”’」』")':
                end += 1
        segment = pending[:end].strip()
        pending = pending[end:].lstrip()
        if segment:
            segments.append(segment)
    return segments


def _resample_pcm16(samples, source_rate, target_rate=TARGET_SAMPLE_RATE):
    """对有符号单声道样本做线性重采样，不引入可选依赖。"""
    if source_rate == target_rate:
        return samples
    if source_rate <= 0 or target_rate <= 0:
        raise RuntimeError("GPT-SoVITS returned an invalid sample rate")
    source = array("h")
    source.frombytes(samples)
    if sys.byteorder == "big":
        source.byteswap()
    if not source:
        return b""
    if source_rate != target_rate and source.itemsize != 2:
        raise RuntimeError("Unsupported PCM sample width")
    count = max(1, round(len(source) * target_rate / source_rate))
    result = array("h")
    scale = source_rate / target_rate
    last = len(source) - 1
    for index in range(count):
        position = index * scale
        left = min(int(position), last)
        right = min(left + 1, last)
        fraction = position - left
        value = round(source[left] + (source[right] - source[left]) * fraction)
        result.append(max(-32768, min(32767, value)))
    return result.tobytes()


def _decode_wav(body, max_audio_bytes, max_pcm_bytes=None):
    if len(body) > max_audio_bytes:
        raise _failure("audio_too_large")
    try:
        with wave.open(BytesIO(body), "rb") as wav:
            channels = wav.getnchannels()
            width = wav.getsampwidth()
            rate = wav.getframerate()
            frames = wav.getnframes()
            if channels not in (1, 2) or width != 2 or rate <= 0 or frames <= 0:
                raise _failure("unsupported_wav")
            raw = wav.readframes(frames)
    except (wave.Error, EOFError):
        raise _failure("invalid_wav") from None
    if len(raw) != frames * 2 * channels:
        raise _failure("truncated_wav")
    output_bytes = max(1, round(frames * TARGET_SAMPLE_RATE / rate)) * 2
    if max_pcm_bytes is not None and output_bytes > max_pcm_bytes:
        raise _failure("audio_too_large")
    if channels == 2:
        stereo = array("h")
        stereo.frombytes(raw)
        if sys.byteorder == "big":
            stereo.byteswap()
        mono = array("h")
        for left, right in zip(stereo[::2], stereo[1::2]):
            mono.append(max(-32768, min(32767, round((left + right) / 2))))
        raw = mono.tobytes()
    return _resample_pcm16(raw, rate)


class GptSoVitsTtsProvider:
    """顺序请求大小受限的 WAV，并提供文本段级播放时间信息。"""

    codec = "pcm_s16le"
    sample_rate_hz = TARGET_SAMPLE_RATE
    channels = 1
    sample_width_bytes = 2
    capabilities = {"streaming_input": False, "streaming_output": False,
                    "partial_results": False}
    subtitle_enabled = True

    def __init__(self, options, credentials, session):
        self.options = options
        self.credentials = credentials
        self.session = session
        self.endpoint = str(options.get("endpoint", DEFAULT_ENDPOINT)).rstrip("/")
        self._text = []
        self._started = False
        self._finished = asyncio.Event()
        self._closed = False
        self._reading = False
        self._cancel_requested = False
        self._pending_task = None
        self._close_task = None
        self._deadline = None
        self._received_bytes = 0
        self._pcm_bytes = 0

    async def start(self):
        if self._started or self._closed:
            raise RuntimeError("GPT-SoVITS task cannot be started again")
        parsed = urlsplit(self.endpoint)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            raise _failure("invalid_endpoint")
        if parsed.username or parsed.password:
            raise _failure("endpoint_credentials")
        self._started = True

    async def append_text(self, text):
        if not self._started or self._finished.is_set() or self._closed:
            raise RuntimeError("GPT-SoVITS task does not accept text")
        if not isinstance(text, str):
            raise TypeError("GPT-SoVITS text must be a string")
        if text:
            if sum(len(item) for item in self._text) + len(text) > MAX_TEXT_CHARS:
                raise RuntimeError("GPT-SoVITS text exceeds the limit")
            self._text.append(text)

    async def finish(self):
        if not self._started:
            raise RuntimeError("GPT-SoVITS task has not started")
        self._finished.set()

    async def _read_response(self, response):
        limit = int(self.options.get("max_audio_bytes", DEFAULT_MAX_AUDIO_BYTES))
        if limit <= 0:
            raise _failure("invalid_audio_limit")
        if response.status != 200:
            raise _failure("http_status", http_status=response.status)
        body = bytearray()
        async for chunk in response.content.iter_chunked(64 * 1024):
            self._received_bytes += len(chunk)
            if self._received_bytes > limit:
                raise _failure("audio_too_large")
            body.extend(chunk)
        pcm = _decode_wav(bytes(body), limit, limit - self._pcm_bytes)
        self._pcm_bytes += len(pcm)
        return pcm

    async def _synthesize(self, text, index):
        import aiohttp

        if self._closed:
            raise asyncio.CancelledError()
        payload = {
            "text": text,
            "text_language": self.options.get("text_language", "zh"),
            "speed": float(self.options.get("speed", 1.0)),
        }
        for key in ("refer_wav_path", "prompt_text", "prompt_language"):
            value = self.options.get(key)
            if value:
                payload[key] = value
        timeout = float(self.options.get("timeout_s", 120))
        if not math.isfinite(timeout) or timeout <= 0:
            raise _failure("invalid_timeout")
        loop = asyncio.get_running_loop()
        started = loop.time()
        if self._deadline is None:
            self._deadline = started + timeout
        remaining = self._deadline - started
        if remaining <= 0:
            raise _failure("timeout")
        try:
            request_timeout = aiohttp.ClientTimeout(total=remaining)
            async with asyncio.timeout_at(self._deadline):
                async with self.session.post(
                        self.endpoint + "/", json=payload, timeout=request_timeout) as response:
                    pcm = await self._read_response(response)
            _LOG.info("tts_segment adapter=gpt_sovits index=%d text_chars=%d request_ms=%.1f "
                      "audio_ms=%.1f response_bytes_total=%d pcm_bytes_total=%d",
                      index, len(text), (loop.time() - started) * 1000,
                      len(pcm) * 1000 / (TARGET_SAMPLE_RATE * 2),
                      self._received_bytes, self._pcm_bytes)
            return pcm
        except asyncio.CancelledError:
            raise
        except asyncio.TimeoutError:
            raise _failure("timeout") from None
        except aiohttp.ClientConnectorError as exc:
            refused = (getattr(exc.os_error, "errno", None) in {errno.ECONNREFUSED, 10061}
                       or getattr(exc.os_error, "winerror", None) == 10061)
            raise _failure("connection_refused" if refused else "connection_failed") from None
        except aiohttp.ClientConnectionError:
            raise _failure("connection_failed") from None
        except aiohttp.ClientError:
            raise _failure("http_client_failed") from None

    async def media(self):
        """先产出每个文本段的样本范围，再产出统一格式的 PCM。

        曼波不提供逐词对齐。每个合成段对应一个时间范围；设备保留正常的
        DMA 播放时钟，在该范围内估算进度，不能将其视为逐字对齐精度。"""
        if not self._started or self._closed or self._reading:
            raise RuntimeError("GPT-SoVITS audio reader is unavailable")
        self._reading = True
        try:
            await self._finished.wait()
            if self._closed:
                return
            segments = _split_text("".join(self._text),
                                   self.options.get("first_segment_chars", 40),
                                   self.options.get("segment_chars", 80))
            self._text.clear()
            if not segments:
                return
            self._pending_task = asyncio.create_task(self._synthesize(segments[0], 1))
            sample_offset = 0
            for index in range(len(segments)):
                pcm = await self._pending_task
                self._pending_task = None
                if self._closed:
                    return
                if index + 1 < len(segments):
                    self._pending_task = asyncio.create_task(
                        self._synthesize(segments[index + 1], index + 2))
                sample_end = sample_offset + len(pcm) // 2
                if pcm and sample_end > sample_offset:
                    yield SubtitleCue(
                        text=segments[index],
                        words=(SubtitleWord(segments[index], sample_offset, sample_end),),
                        upstream_event=index + 1,
                    )
                sample_offset = sample_end
                # 下游按播放节奏消费，自然形成背压。当前 PCM 段消费完之前，
                # 不会创建第三个合成请求。
                for offset in range(0, len(pcm), 1920):
                    if self._closed:
                        return
                    yield pcm[offset:offset + 1920]
        finally:
            await self.close()

    async def audio(self):
        """兼容仅消费 PCM、不读取元数据的调用者。"""
        media = self.media()
        try:
            async for item in media:
                if isinstance(item, bytes):
                    yield item
        finally:
            await media.aclose()

    async def cancel(self):
        self._cancel_requested = True
        self._finished.set()
        await self.close()

    async def close(self):
        self._closed = True
        self._finished.set()
        self._text.clear()
        if self._close_task is None:
            self._close_task = asyncio.create_task(self._join_pending())
        try:
            await asyncio.shield(self._close_task)
        except asyncio.CancelledError:
            # 保留取消状态，但必须先释放请求再返回。
            await asyncio.shield(self._close_task)
            raise

    async def _join_pending(self):
        task = self._pending_task
        if task is not None:
            if not task.done() and not task.cancelling():
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            self._pending_task = None

