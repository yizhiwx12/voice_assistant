"""设备 WebSocket 传输及有界队列；厂商协议只在服务端处理。"""

import asyncio
import json
import logging
import math
import uuid
from collections import deque
from contextlib import suppress

from audio.levels import PcmLevelMeter
from audio.opus_codec import Decoder, Encoder, CodecError, native_version
from audio.subtitle_timeline import SubtitleAssembler, ResponseSubtitleAssembler
from providers.tts.base import SubtitleCue, SubtitleUnavailable
from core.config import select_profile, validate_selection_credentials
from core.pipeline import VoicePipeline
from providers.asr.doubao_streaming import DoubaoAsrError, DoubaoStreamingAsrProvider
from transport.voice_protocol import AudioPacket, DOWNLINK, UPLINK, ProtocolError, uint32


LOGGER = logging.getLogger(__name__)

# 允许短时投递突发，但不增加固定音频存储。
# 这些是应用层预算，不是端到端 TCP 延迟承诺。
UPLINK_ADMISSION_WAIT_SECONDS = 0.1
UPLINK_MAX_QUEUE_AGE_SECONDS = 0.75
CANCEL_CLEANUP_SECONDS = 5.0
DOWNLINK_FRAME_SECONDS = 0.02
DOWNLINK_BURST_SECONDS = 0.2


def _subtitle_timing_metrics(cue, *, now, stream_started_at, previous_cue_at,
                             sent_frames, pre_skip, source_samples, fresh_words=None):
    """记录本地投递时序，不把它当作设备播放进度的估计值。"""
    source = "none"
    if isinstance(cue.source_id, str):
        try:
            parsed = uuid.UUID(cue.source_id)
            if str(parsed) == cue.source_id:
                source = cue.source_id
        except ValueError:
            pass
    received_at = cue.received_at
    valid_received_at = (type(received_at) in (float, int) and
                         0 <= received_at <= now and math.isfinite(received_at))
    sent_content_samples = min(source_samples, max(0, sent_frames * 480 - pre_skip))
    words = cue.words if fresh_words is None else tuple(fresh_words)
    if not words:
        return None
    first_start = min(word["start_sample"] if isinstance(word, dict) else word.start_sample
                      for word in words)
    last_end = max(word["end_sample"] if isinstance(word, dict) else word.end_sample
                   for word in words)
    return {
        "source": source,
        "elapsed_ms": max(0.0, (now - stream_started_at) * 1000),
        "gap_ms": (-1.0 if previous_cue_at is None else
                   max(0.0, (now - previous_cue_at) * 1000)),
        "provider_queue_ms": (max(0.0, (now - received_at) * 1000)
                              if valid_received_at else -1.0),
        "first_start_sample": first_start,
        "end_sample": last_end,
        "sent_content_samples": sent_content_samples,
        "start_lead_ms": (first_start - sent_content_samples) / 24.0,
        "end_lead_ms": (last_end - sent_content_samples) / 24.0,
    }


class _DownlinkPacer:
    """限制共享 TCP 流中排在控制消息之前的待播放音频。

    可写连接可能在设备播放前就收下整段合成回答，因此仅靠 TCP 背压无法
    让后续取消确认靠近实际听到的位置。允许少量预缓冲，随后按媒体速率发送；
    生产者或连接停顿后重新确定基准，避免累计时间换来无限制突发发送。
    """
    def __init__(self, *, clock=None, sleep=None):
        self._clock = clock or asyncio.get_running_loop().time
        self._sleep = sleep or asyncio.sleep
        self._capacity = DOWNLINK_BURST_SECONDS / DOWNLINK_FRAME_SECONDS
        self._tokens = self._capacity
        self._updated = self._clock()

    async def wait(self):
        while True:
            now = self._clock()
            self._tokens = min(self._capacity,
                self._tokens + max(0.0, now - self._updated) / DOWNLINK_FRAME_SECONDS)
            self._updated = now
            if self._tokens >= 1 - 1e-9:
                self._tokens = max(0.0, self._tokens - 1)
                return
            # 此等待必须放在 DeviceConnection.send_lock 之外，
            # 让 response.cancelled 等控制消息能立即写出。
            await self._sleep((1 - self._tokens) * DOWNLINK_FRAME_SECONDS)


class UplinkBacklogError(ProtocolError):
    pass


class UplinkQueue(asyncio.Queue):
    """为有界音频队列同时保留有界时间戳元数据。"""
    def __init__(self):
        super().__init__(maxsize=25)  # 最多保留 500 ms，按每帧 20 ms 计。
        self.audio_times = deque()

    def _put(self, item):
        super()._put(item)
        if item[0] == "audio":
            self.audio_times.append(item[2])

    def _get(self):
        item = super()._get()
        if item[0] == "audio":
            self.audio_times.popleft()
        return item

    def oldest_audio_age(self, now):
        return max(0.0, now - self.audio_times[0]) if self.audio_times else 0.0


async def codec_call(function, *args):
    """取消时先等待本机编解码任务结束，再释放其状态。"""
    task = asyncio.create_task(asyncio.to_thread(function, *args))
    try:
        return await asyncio.shield(task)
    except asyncio.CancelledError:
        with suppress(Exception):
            await task
        raise


class InputStream:
    """解码连续音频流，保留最后一帧以便按有效终点裁剪。"""
    def __init__(self, stream_id, codec, pre_skip):
        self.id = uint32(stream_id, "stream_id", nonzero=True)
        if codec not in ("opus", "pcm_s16le"):
            raise ProtocolError("Unsupported uplink codec")
        if type(pre_skip) is not int or not 0 <= pre_skip <= 640 or (codec == "pcm_s16le" and pre_skip):
            raise ProtocolError("Invalid uplink pre_skip_samples")
        self.codec = codec
        self.skip = pre_skip
        self.decoder = Decoder(16000) if codec == "opus" else None
        self.sequence = 0
        self.offset = 0
        self.pending = b""
        self.pending_offset = 0
        self.emitted_until = 0

    def _take(self, end):
        start = max(self.skip, self.pending_offset, self.emitted_until)
        stop = min(end, self.pending_offset + len(self.pending) // 2)
        result = self.pending[(start - self.pending_offset) * 2:(stop - self.pending_offset) * 2] if stop > start else b""
        self.emitted_until = max(self.emitted_until, stop)
        return result

    def feed(self, packet):
        if packet.stream_id != self.id or packet.sequence != self.sequence or packet.sample_offset != self.offset:
            raise ProtocolError("Audio stream sequence or sample offset mismatch")
        if self.decoder:
            pcm = self.decoder.decode(packet.payload)
        else:
            if len(packet.payload) != 640:
                raise ProtocolError("PCM uplink frame must contain 320 samples")
            pcm = packet.payload
        ready = self._take(self.offset)
        self.pending = pcm
        self.pending_offset = self.offset
        self.offset += 320
        self.sequence += 1
        return ready

    def flush(self, valid_end):
        if type(valid_end) is not int or not max(self.skip, self.emitted_until) <= valid_end <= self.offset:
            raise ProtocolError("Invalid decoded valid_end_sample")
        return self._take(valid_end)

    def close(self):
        if self.decoder:
            self.decoder.close()


class DeviceConnection:
    def __init__(self, ws, config, credentials, selection, http_session, *, provider_factory=None):
        self.ws = ws
        self.config = config
        self.credentials = credentials
        self.selection = selection
        self.send_lock = asyncio.Lock()
        self.uplink = None
        self.used_streams = set()
        self._discarded_streams = set()  # 容量受 used_streams 的 1000 个编号上限约束。
        self._sealed_turn = 0
        self.downstream_id = 0x80000000
        self.downlink_codec = selection.transport["downlink"]["codec"]
        self.uplink_codec = selection.transport["uplink"]["codec"]
        self.queue = UplinkQueue()
        self.pipeline = VoicePipeline(selection, credentials, http_session, self.send_json,
                                      self.send_audio, provider_factory=provider_factory)
        self.worker = None
        self.finish_task = None
        self.speech_frames = 0
        self.last_activity = asyncio.get_running_loop().time()
        self._activity_changed = asyncio.Event()
        self._idle_was_busy = False
        self._playback_pending = None
        self._playback_deadline = 0.0
        self.queue_peak = 0
        self.first_failure = None
        self._pacing = {}
        self._reset_recognition_diagnostics()
        # 记录数值型阶段诊断，并将其保留在连接上，
        # 以便队列满时指出当前占用工作任务的操作，
        # 同时不暴露音频或服务商数据。
        self._stage_last = {}
        self._stage_max = {}
        self._stage_started = {}
        self._loop_probe = None
        self._loop_deadline = None
        self._loop_lag_ms = 0.0
        self._loop_lag_max_ms = 0.0
        self._audio_received_at = None
        self._receive_gap_ms = 0.0
        self._receive_gap_max_ms = 0.0
        self._receive_burst = 0
        self._receive_burst_peak = 0
        self._receive_frames = 0
        self._worker_phase = "waiting"
        self._arrival_turn = 0
        self._media_origin = None
        self._worker_media_lag_ms = 0.0
        self._reset_turn_arrivals()
        self._response_cancel_diagnostic = None

    def _log_response_cancel(self, phase, diagnostic, *, send_ms=0.0, stage="none"):
        # 只记录固定的数值身份和时序；格式错误的控制值、
        # 服务商异常、识别正文及凭证均不能写入日志。
        target = diagnostic["target"]
        safe_id = lambda key: target[key] if type(target.get(key)) is int and 0 <= target[key] <= 0xffffffff else 0
        LOGGER.info("voice_response_cancel phase=%s mode=%s epoch=%d turn=%d generation=%d "
                    "elapsed_ms=%.1f send_ms=%.1f stage=%s", phase, diagnostic["mode"],
                    safe_id("session_epoch"), safe_id("turn_id"), safe_id("generation"),
                    (asyncio.get_running_loop().time() - diagnostic["started"]) * 1000,
                    send_ms, stage)

    def _response_cancel_cleanup_complete(self, diagnostic):
        if not diagnostic["cleanup_complete"]:
            diagnostic["cleanup_complete"] = True
            self._log_response_cancel("cleanup_complete", diagnostic)

    async def _cancel_response(self, event):
        target = {key: event.get(key) for key in ("session_epoch", "turn_id", "generation")}
        matched = target == self.pipeline.response_identity
        task = self.pipeline.response_task
        diagnostic = {"target": target, "started": asyncio.get_running_loop().time(),
                      "mode": "stale" if not matched else "active" if task and not task.done() else "idempotent",
                      "cleanup_complete": False, "ack_sent": False}
        self._log_response_cancel("received", diagnostic)
        if not matched:
            self._log_response_cancel("rejected", diagnostic)
            return
        self._response_cancel_diagnostic = diagnostic
        try:
            self._playback_pending = None
            # 正在执行的取消在清理后通过下方 send_json 发出确认。
            # 回答已完成时，也确认此幂等请求。
            if not await self.pipeline.cancel_response():
                self._response_cancel_cleanup_complete(diagnostic)
                await self.send_json({"type": "response.cancelled", **target})
        except BaseException:
            self._log_response_cancel("failed", diagnostic,
                                      stage="ack_write" if diagnostic["cleanup_complete"] else "cleanup")
            raise
        finally:
            self._response_cancel_diagnostic = None

    def _reset_recognition_diagnostics(self):
        self._pcm_levels = PcmLevelMeter()
        self._asr_outcome = "no_final"
        self._device_partial_events = 0
        self._device_segment_events = 0
        self._device_final_events = 0
        self._asr_final_chars = 0

    async def _feed_pipeline_audio(self, pcm):
        recording = self.pipeline.recording
        await self.pipeline.feed_audio(pcm)
        if recording:
            # 统计实际送入 ASR、已解码并裁掉前瞻部分的 PCM，
            # 包括最后尾段；这里不保留样本。
            self._pcm_levels.feed(pcm)

    def _reset_turn_arrivals(self):
        self._turn_audio_received_at = None
        self._turn_receive_gap_ms = 0.0
        self._turn_receive_gap_max_ms = 0.0
        self._turn_receive_burst = 0
        self._turn_receive_burst_peak = 0
        self._turn_receive_frames = 0
        self._turn_media_lag_ms = 0.0
        self._turn_media_lag_max_ms = 0.0

    def _media_now(self):
        return asyncio.get_running_loop().time()

    def _media_lag_seconds(self, packet, origin):
        if origin is None:
            return 0.0
        # 设备样本偏移量表示时长，并非远端墙钟。
        # 这里测量首帧接收之后不断增长的滞后；
        # 首帧之前的延迟不在可观测基准内。
        started, first_offset = origin
        return max(0.0, self._media_now() - started - (packet.sample_offset - first_offset) / 16000)

    async def _monitor_loop(self, interval=0.1):
        """使用固定存储的调度探针，不检查负载内容或任务。"""
        loop = asyncio.get_running_loop()
        try:
            while True:
                self._loop_deadline = loop.time() + interval
                await asyncio.sleep(interval)
                self._loop_lag_ms = max(0.0, (loop.time() - self._loop_deadline) * 1000)
                self._loop_lag_max_ms = max(self._loop_lag_max_ms, self._loop_lag_ms)
        finally:
            self._loop_deadline = None

    def _record_audio_arrival(self, now):
        if self._audio_received_at is not None:
            self._receive_gap_ms = (now - self._audio_received_at) * 1000
            self._receive_gap_max_ms = max(self._receive_gap_max_ms, self._receive_gap_ms)
            # 连续到达间隔小于 20 ms 帧长的一半时，判为投递突发；
            # 前面的长间隔仍单独保留。
            self._receive_burst = self._receive_burst + 1 if self._receive_gap_ms < 10 else 1
        else:
            self._receive_burst = 1
        self._audio_received_at = now
        self._receive_frames += 1
        self._receive_burst_peak = max(self._receive_burst_peak, self._receive_burst)
        if self._turn_audio_received_at is not None:
            self._turn_receive_gap_ms = (now - self._turn_audio_received_at) * 1000
            self._turn_receive_gap_max_ms = max(self._turn_receive_gap_max_ms, self._turn_receive_gap_ms)
            self._turn_receive_burst = self._turn_receive_burst + 1 if self._turn_receive_gap_ms < 10 else 1
        else:
            self._turn_receive_burst = 1
        self._turn_audio_received_at = now
        self._turn_receive_frames += 1
        self._turn_receive_burst_peak = max(self._turn_receive_burst_peak, self._turn_receive_burst)

    def _stage_begin(self, name):
        self._stage_started[name] = asyncio.get_running_loop().time()

    def _stage_end(self, name):
        started = self._stage_started.pop(name, None)
        if started is None:
            return
        elapsed = (asyncio.get_running_loop().time() - started) * 1000
        self._stage_last[name] = elapsed
        self._stage_max[name] = max(self._stage_max.get(name, 0.0), elapsed)

    def _stage_value(self, name):
        started = self._stage_started.get(name)
        if started is not None:
            return (asyncio.get_running_loop().time() - started) * 1000
        return self._stage_last.get(name, 0.0)

    def _diagnostic_values(self):
        pacing = self._capture_pacing()
        loop_overdue_ms = (max(0.0, (asyncio.get_running_loop().time() - self._loop_deadline) * 1000)
                           if self._loop_deadline is not None else 0.0)
        return {
            "queue_oldest_audio_ms": self.queue.oldest_audio_age(asyncio.get_running_loop().time()) * 1000,
            "enqueue_wait_ms": self._stage_value("enqueue_wait"),
            "enqueue_wait_max_ms": self._stage_max.get("enqueue_wait", 0.0),
            "worker_phase": self._worker_phase,
            "worker_media_lag_ms": self._worker_media_lag_ms,
            "queue_wait_ms": self._stage_value("queue_wait"),
            "queue_wait_max_ms": self._stage_max.get("queue_wait", 0.0),
            "decode_ms": self._stage_value("decode"),
            "decode_max_ms": self._stage_max.get("decode", 0.0),
            "feed_ms": self._stage_value("asr_feed"),
            "feed_max_ms": self._stage_max.get("asr_feed", 0.0),
            "feed_inflight_ms": ((asyncio.get_running_loop().time() - self._stage_started["asr_feed"]) * 1000
                                 if "asr_feed" in self._stage_started else 0.0),
            "asr_inflight": pacing.get("send_inflight", 0),
            "asr_inflight_ms": pacing.get("send_inflight_ms", 0.0),
            "asr_inflight_max_ms": pacing.get("max_send_inflight_ms", 0.0),
            "loop_lag_ms": max(self._loop_lag_ms, loop_overdue_ms),
            "loop_lag_max_ms": max(self._loop_lag_max_ms, loop_overdue_ms),
            "receive_gap_ms": self._receive_gap_ms,
            "receive_gap_max_ms": self._receive_gap_max_ms,
            "receive_burst": self._receive_burst,
            "receive_burst_peak": self._receive_burst_peak,
            "receive_frames": self._receive_frames,
            "arrival_turn": self._arrival_turn,
            "turn_receive_frames": self._turn_receive_frames,
            "turn_receive_gap_ms": self._turn_receive_gap_ms,
            "turn_receive_gap_max_ms": self._turn_receive_gap_max_ms,
            "turn_receive_burst": self._turn_receive_burst,
            "turn_receive_burst_peak": self._turn_receive_burst_peak,
            "turn_media_lag_ms": self._turn_media_lag_ms,
            "turn_media_lag_max_ms": self._turn_media_lag_max_ms,
        }

    def _capture_pacing(self, asr=None):
        asr = asr if asr is not None else self.pipeline.asr
        if asr is not None and hasattr(asr, "pacing_metrics"):
            self._pacing = asr.pacing_metrics()
        return self._pacing

    def _asr_failure_diagnostics(self, exc):
        # 只有这个已知适配器提供固定原因码和有界数值；
        # 不检查任意异常文本或远端响应对象。
        if isinstance(exc, DoubaoAsrError):
            return exc.diagnostics()
        if isinstance(self.pipeline.asr, DoubaoStreamingAsrProvider):
            return self.pipeline.asr.failure_diagnostics()
        return {"asr_reason": "none", "asr_upstream_code": 0, "asr_upstream_close_code": 0}

    def record_failure(self, stage, exc=None, *, category=None, close_code=0):
        """只使用固定分类与数值；异常文本可能包含秘密。"""
        if category is None:
            category = ("uplink_backlog" if isinstance(exc, UplinkBacklogError) else
                        "codec_failed" if isinstance(exc, CodecError) else
                        "protocol_invalid" if isinstance(exc, (ProtocolError, ValueError, TypeError)) else
                        "operation_timeout" if isinstance(exc, TimeoutError) else
                        "network_failed" if isinstance(exc, OSError) else "provider_failed")
        now_ms = int(asyncio.get_running_loop().time() * 1000)
        first = self.first_failure is None
        asr_failure = self._asr_failure_diagnostics(exc)
        if first:
            self.first_failure = {"stage": stage, "category": category, "time_ms": now_ms,
                                  **asr_failure}
        pacing = self._capture_pacing()
        diagnostics = self._diagnostic_values()
        log = LOGGER.warning if first else LOGGER.info
        log("voice_failure first=%d time_ms=%d stage=%s category=%s epoch=%d turn=%d stream=%d "
            "queue=%d queue_peak=%d close_code=%d asr_lag_ms=%.1f asr_send_ms=%.1f "
            "queue_wait_ms=%.1f queue_wait_max_ms=%.1f decode_ms=%.1f decode_max_ms=%.1f "
            "feed_ms=%.1f feed_max_ms=%.1f feed_inflight_ms=%.1f asr_inflight=%d "
            "asr_inflight_ms=%.1f asr_inflight_max_ms=%.1f loop_lag_ms=%.1f loop_lag_max_ms=%.1f "
            "receive_gap_ms=%.1f receive_gap_max_ms=%.1f receive_burst=%d receive_burst_peak=%d "
            "receive_frames=%d metrics_scope=connection queue_oldest_audio_ms=%.1f "
            "enqueue_wait_ms=%.1f enqueue_wait_max_ms=%.1f worker_phase=%s arrival_turn=%d "
            "turn_receive_frames=%d turn_receive_gap_ms=%.1f turn_receive_gap_max_ms=%.1f "
            "turn_receive_burst=%d turn_receive_burst_peak=%d worker_media_lag_ms=%.1f "
            "turn_media_lag_ms=%.1f turn_media_lag_max_ms=%.1f "
            "asr_reason=%s asr_upstream_code=%d asr_upstream_close_code=%d",
            int(first), now_ms, stage, category, self.pipeline.session.epoch, self.pipeline.session.turn,
            self.uplink.id if self.uplink else 0, self.queue.qsize(), self.queue_peak, close_code,
            pacing.get("max_lag_ms", 0), pacing.get("max_send_ms", 0),
            diagnostics["queue_wait_ms"], diagnostics["queue_wait_max_ms"],
            diagnostics["decode_ms"], diagnostics["decode_max_ms"], diagnostics["feed_ms"],
            diagnostics["feed_max_ms"], diagnostics["feed_inflight_ms"], diagnostics["asr_inflight"],
            diagnostics["asr_inflight_ms"], diagnostics["asr_inflight_max_ms"], diagnostics["loop_lag_ms"],
            diagnostics["loop_lag_max_ms"], diagnostics["receive_gap_ms"], diagnostics["receive_gap_max_ms"],
            diagnostics["receive_burst"], diagnostics["receive_burst_peak"], diagnostics["receive_frames"],
            diagnostics["queue_oldest_audio_ms"], diagnostics["enqueue_wait_ms"], diagnostics["enqueue_wait_max_ms"],
            diagnostics["worker_phase"], diagnostics["arrival_turn"], diagnostics["turn_receive_frames"],
            diagnostics["turn_receive_gap_ms"], diagnostics["turn_receive_gap_max_ms"],
            diagnostics["turn_receive_burst"], diagnostics["turn_receive_burst_peak"],
            diagnostics["worker_media_lag_ms"], diagnostics["turn_media_lag_ms"],
            diagnostics["turn_media_lag_max_ms"], asr_failure["asr_reason"],
            asr_failure["asr_upstream_code"], asr_failure["asr_upstream_close_code"])

    def _check_queue_age(self, now):
        if self.queue.oldest_audio_age(now) >= UPLINK_MAX_QUEUE_AGE_SECONDS:
            raise UplinkBacklogError("Queued uplink audio exceeded its age budget")

    async def _check_worker(self):
        if self.worker is not None and self.worker.done():
            await self.worker  # 保留工作任务的原始失败原因。
            raise ProtocolError("Uplink worker stopped")

    async def _enqueue(self, item):
        loop = asyncio.get_running_loop()
        now = loop.time()
        if item[0] == "start":
            self._arrival_turn = item[1]
            self._reset_turn_arrivals()
        if item[0] in ("start", "stream_start"):
            self._media_origin = None
        origin = None
        if item[0] == "audio":
            self._record_audio_arrival(now)
            if isinstance(item[1], AudioPacket):
                if self._media_origin is None:
                    self._media_origin = (self._media_now(), item[1].sample_offset)
                origin = self._media_origin
                self._turn_media_lag_ms = self._media_lag_seconds(item[1], origin) * 1000
                self._turn_media_lag_max_ms = max(self._turn_media_lag_max_ms, self._turn_media_lag_ms)
                if self._turn_media_lag_ms >= UPLINK_MAX_QUEUE_AGE_SECONDS * 1000:
                    raise UplinkBacklogError("Uplink media timeline exceeded its lag budget")
        await self._check_worker()
        self._check_queue_age(now)
        pending = None
        try:
            try:
                self.queue.put_nowait((item[0], item[1], now, origin))
            except asyncio.QueueFull:
                # 最多只有一个待接纳项：run() 等入队完成才读取下一条消息。
                # 取消不能遗留等待入队的任务。
                self._stage_begin("enqueue_wait")
                pending = asyncio.create_task(self.queue.put((item[0], item[1], now, origin)))
                watched = (pending, self.worker) if self.worker is not None else (pending,)
                done, _ = await asyncio.wait(watched, timeout=UPLINK_ADMISSION_WAIT_SECONDS,
                                             return_when=asyncio.FIRST_COMPLETED)
                await self._check_worker()
                if pending not in done:
                    raise UplinkBacklogError("Uplink admission exceeded its wait budget")
                await pending
            self.queue_peak = max(self.queue_peak, self.queue.qsize())
            # 读取 WebSocket 缓冲和 put_nowait 都可能不让出调度；
            # 每次接纳后给解码与输入任务一次执行机会。
            await asyncio.sleep(0)
            await self._check_worker()
            self._check_queue_age(loop.time())
        finally:
            if pending is not None:
                if not pending.done():
                    pending.cancel()
                with suppress(asyncio.CancelledError):
                    await pending
                self._stage_end("enqueue_wait")

    async def send_json(self, event):
        if event.get("type") == "error" and event.get("code") in (
                "asr_events_failed", "asr_final_failed", "tts_failed", "response_failed"):
            self.record_failure("pipeline_event", category=event["code"])
        cancel_diagnostic = self._response_cancel_diagnostic
        if (event.get("type") == "response.cancelled" and cancel_diagnostic is not None
                and {key: event.get(key) for key in ("session_epoch", "turn_id", "generation")}
                == cancel_diagnostic["target"]):
            # 流水线只在旧回答清理完成后发出消息，
            # 将此边界与下行锁或连接等待分开统计。
            self._response_cancel_cleanup_complete(cancel_diagnostic)
            cancel_send_started = asyncio.get_running_loop().time()
        else:
            cancel_diagnostic = None
        async with self.send_lock:
            if (event.get("type", "").startswith("asr.") or
                    event.get("type") in ("speech.started", "response.started", "llm.delta", "llm.done", "response.done")):
                if 0 < event.get("turn_id", 0) <= self._sealed_turn:
                    return False
            await asyncio.wait_for(self.ws.send_json(event), 5)
            if cancel_diagnostic is not None:
                cancel_diagnostic["ack_sent"] = True
                self._log_response_cancel("ack_sent", cancel_diagnostic,
                    send_ms=(asyncio.get_running_loop().time() - cancel_send_started) * 1000)
            # 只统计成功写给设备的结果，不记录正文。
            # 即使音频传输正常，最终识别结果也可能为空。
            if event.get("type") == "asr.partial":
                self._device_partial_events += 1
            elif event.get("type") == "asr.segment_final":
                self._device_segment_events += 1
            elif event.get("type") == "asr.utterance_final":
                self._device_final_events += 1
                text = event.get("text", "")
                self._asr_final_chars = len(text)
                self._asr_outcome = "text" if text.strip() else "empty"
            self.last_activity = asyncio.get_running_loop().time()
            self._activity_changed.set()

    async def send_packet(self, packet):
        async with self.send_lock:
            await asyncio.wait_for(self.ws.send_bytes(packet.pack()), 5)
            self.last_activity = asyncio.get_running_loop().time()
            self._activity_changed.set()

    def _idle_wait_seconds(self):
        """只有明确待机才算空闲；云端处理和缓冲播放期间均为忙碌。"""
        seconds = self.selection.transport.get("idle_timeout_seconds", 0)
        if not seconds:
            return None
        now = asyncio.get_running_loop().time()
        if self._playback_pending is not None and self._playback_deadline <= now:
            # 旧 PC 客户端可能不发 playback.done；预留完整样本播放时长
            # 与宽限期后，再开始完整的空闲时间预算。
            self._playback_pending = None
        tasks = (self.finish_task, self.pipeline.response_task)
        busy = (self.pipeline.recording or self.pipeline.finishing or not self.queue.empty()
                or any(task and not task.done() for task in tasks)
                or self._playback_pending is not None)
        if busy:
            self._idle_was_busy = True
            # 同时留意队列已排空或缺失播放确认的情况。
            return 1.0
        if self._idle_was_busy:
            self.last_activity = now
            self._idle_was_busy = False
        return max(0.0, seconds - (now - self.last_activity))

    async def send_audio(self, chunks, identity, *, subtitle_enabled=False):
        self.downstream_id += 1
        stream_id = self.downstream_id
        codec = self.downlink_codec
        encoder = Encoder(24000, self.selection.transport["downlink"]["bitrate_bps"],
                          vbr=self.selection.transport["vbr"]) if codec == "opus" else None
        pre_skip = encoder.pre_skip if encoder else 0
        sequence = 0
        samples = 0
        pending = bytearray()
        playback_finished = False
        loop = asyncio.get_running_loop()
        estimated_playback_end = 0.0
        pacer = _DownlinkPacer()
        # 流水线在进入本方法前已提交完整回答；
        # 旧版直接调用或诊断调用仍保留只追加字幕的模式。
        response_text = getattr(self.pipeline, "committed_answer", None)
        fixed_text = isinstance(response_text, str)
        subtitle_assembler = ((ResponseSubtitleAssembler(response_text) if fixed_text else SubtitleAssembler())
                              if subtitle_enabled else None)
        subtitle_sent = False
        subtitle_unavailable = False
        subtitle_cues = 0
        subtitle_words = 0
        subtitle_batches = 0
        stream_started_at = loop.time()
        previous_subtitle_at = None

        async def unavailable(reason, *, detail="none", upstream_event=0, upstream_events=0):
            nonlocal subtitle_unavailable
            if subtitle_unavailable:
                return
            await self.send_json({"type": "audio.subtitle.unavailable", **identity,
                                  "stream_id": stream_id, "reason": reason, "detail": detail})
            subtitle_unavailable = True
            LOGGER.warning(
                "subtitle_state status=unavailable reason=%s detail=%s epoch=%d turn=%d "
                "generation=%d stream=%d upstream_event=%d upstream_events=%d cues=%d words=%d "
                "batches=%d pcm_samples=%d",
                reason, detail, identity["session_epoch"], identity["turn_id"],
                identity["generation"], stream_id, upstream_event, upstream_events,
                subtitle_cues, subtitle_words, subtitle_batches, samples)

        async def send(payload):
            nonlocal sequence, estimated_playback_end
            await pacer.wait()
            # 每个包发送前都再次检查，包括编解码器缓存尾段；
            # 速率控制等待期间可能收到取消或新回答。
            if identity != self.pipeline.response_identity:
                raise asyncio.CancelledError()
            await self.send_packet(AudioPacket(DOWNLINK, identity["session_epoch"], stream_id,
                identity["turn_id"], identity["generation"], sequence, sequence * 480, payload))
            sequence += 1
            # 只统计成功发送的帧；服务商或连接停顿可能耗尽播放缓冲。
            # 不要把这段空闲时间累计到后续帧，
            # 否则会误以为新帧已被播放。
            estimated_playback_end = max(estimated_playback_end, loop.time()) + DOWNLINK_FRAME_SECONDS

        try:
            await self.send_json({"type": "audio.stream.start", **identity, "stream_id": stream_id,
                                  "codec": codec, "sample_rate_hz": 24000, "channels": 1,
                                  "frame_duration_ms": 20, "pre_skip_samples": pre_skip,
                                  "subtitle_mode": ("response_word_timestamps" if fixed_text else "word_timestamps")
                                  if subtitle_enabled else "none"})
            self._playback_pending = dict(identity)
            self._playback_deadline = float("inf")
            async for chunk in chunks:
                if identity != self.pipeline.response_identity:
                    raise asyncio.CancelledError()
                if isinstance(chunk, SubtitleUnavailable):
                    if subtitle_enabled and not subtitle_unavailable:
                        detail = chunk.detail if chunk.detail in {
                            "invalid_schema", "invalid_word", "invalid_timing", "invalid_utf8", "limit"
                        } else "none"
                        await unavailable("upstream_invalid", detail=detail,
                                          upstream_event=chunk.upstream_event,
                                          upstream_events=chunk.upstream_events)
                    continue
                if isinstance(chunk, SubtitleCue):
                    if subtitle_assembler is None or subtitle_unavailable:
                        continue
                    subtitle_cues += 1
                    subtitle_words += len(chunk.words)
                    batches = subtitle_assembler.feed(chunk)
                    if subtitle_assembler.failed_reason is not None:
                        await unavailable("assembler_invalid", detail=subtitle_assembler.failed_reason,
                                          upstream_event=chunk.upstream_event)
                        continue
                    if not batches:
                        # 完全重复的字幕片段没有新增显示词，
                        # 不算新的投递间隔。
                        continue
                    cue_at = loop.time()
                    fresh_words = [word for batch in batches for word in batch["words"]]
                    timing = _subtitle_timing_metrics(
                        chunk, now=cue_at, stream_started_at=stream_started_at,
                        previous_cue_at=previous_subtitle_at, sent_frames=sequence,
                        pre_skip=pre_skip, source_samples=samples,
                        fresh_words=fresh_words)
                    previous_subtitle_at = cue_at
                    subtitle_send_started = loop.time()
                    for batch in batches:
                        await self.send_json({"type": "audio.subtitle", **identity,
                                              "stream_id": stream_id, **batch})
                        subtitle_batches += 1
                    LOGGER.info(
                        "subtitle_timing epoch=%d turn=%d generation=%d stream=%d source=%s "
                        "cue=%d batches=%d words=%d new_words=%d elapsed_ms=%.1f gap_ms=%.1f "
                        "provider_queue_ms=%.1f first_start_sample=%d end_sample=%d "
                        "sent_content_samples=%d start_lead_ms=%.1f end_lead_ms=%.1f send_ms=%.1f",
                        identity["session_epoch"], identity["turn_id"], identity["generation"],
                        stream_id, timing["source"], subtitle_cues, len(batches), len(chunk.words),
                        len(fresh_words),
                        timing["elapsed_ms"], timing["gap_ms"], timing["provider_queue_ms"],
                        timing["first_start_sample"], timing["end_sample"],
                        timing["sent_content_samples"], timing["start_lead_ms"],
                        timing["end_lead_ms"], max(0.0, (loop.time() - subtitle_send_started) * 1000))
                    if batches and not subtitle_sent:
                        LOGGER.info(
                            "subtitle_state status=following epoch=%d turn=%d generation=%d stream=%d "
                            "upstream_event=%d cues=%d words=%d batches=%d pcm_samples=%d",
                            identity["session_epoch"], identity["turn_id"], identity["generation"],
                            stream_id, chunk.upstream_event, subtitle_cues, subtitle_words,
                            subtitle_batches, samples)
                        subtitle_sent = True
                    continue
                if len(chunk) % 2 or len(chunk) > 192000:
                    raise ProtocolError("Invalid TTS PCM chunk")
                samples += len(chunk) // 2
                pending.extend(chunk)
                while len(pending) >= 960:
                    frame = bytes(pending[:960])
                    del pending[:960]
                    packets = await codec_call(encoder.feed, frame) if encoder else [frame]
                    for packet in packets:
                        await send(packet)
            if encoder:
                if pending:
                    for packet in await codec_call(encoder.feed, bytes(pending)):
                        await send(packet)
                for packet in await codec_call(encoder.finish):
                    await send(packet)
            elif pending:
                await send(bytes(pending).ljust(960, b"\0"))
            if not samples:
                raise ProtocolError("TTS returned no audio")
            if subtitle_enabled and not subtitle_sent and not subtitle_unavailable:
                await unavailable("no_cues")
            if self._playback_pending == identity:
                # 发送已贴近实时；若这里重新等待一整段媒体时长，
                # 待机会多延迟整段回答的时间。
                # 此后备路径兼容不发 playback.done 的客户端；
                # 实际声学与网络完成仍需设备确认。
                self._playback_deadline = max(estimated_playback_end, loop.time()) + 5.0
            await self.send_json({"type": "audio.stream.end", **identity, "stream_id": stream_id,
                                  "valid_end_sample": pre_skip + samples})
            playback_finished = True
        finally:
            if not playback_finished and self._playback_pending == identity:
                self._playback_pending = None
                self._activity_changed.set()
            if encoder:
                encoder.close()

    async def _work(self):
        while True:
            self._worker_phase = "waiting"
            kind, value, enqueued_at, *timing = await self.queue.get()
            self._stage_started["queue_wait"] = enqueued_at
            self._stage_end("queue_wait")
            self._worker_phase = stage = "uplink_control"
            try:
                if kind == "audio" and asyncio.get_running_loop().time() - enqueued_at >= UPLINK_MAX_QUEUE_AGE_SECONDS:
                    self._worker_phase = stage = "queue_age"
                    raise UplinkBacklogError("Queued uplink audio exceeded its age budget")
                if kind == "audio":
                    self._worker_media_lag_ms = self._media_lag_seconds(value, timing[0] if timing else None) * 1000
                    if self._worker_media_lag_ms >= UPLINK_MAX_QUEUE_AGE_SECONDS * 1000:
                        self._worker_phase = stage = "media_lag"
                        raise UplinkBacklogError("Uplink consumption exceeded its media lag budget")
                if kind == "start":
                    self._worker_phase = stage = "asr_start"
                    if self.finish_task:
                        if not self.finish_task.done():
                            self.finish_task.cancel()
                        with suppress(asyncio.CancelledError):
                            await self.finish_task
                        self.finish_task = None
                    self._pacing = {}
                    self._reset_recognition_diagnostics()
                    await self.pipeline.start_utterance(value)
                    self.speech_frames = 0
                elif kind == "audio":
                    if not self.uplink:
                        raise ProtocolError("Start an uplink stream before audio")
                    self._worker_phase = stage = "uplink_decode"
                    self._stage_begin("decode")
                    try:
                        pcm = await codec_call(self.uplink.feed, value)
                    finally:
                        self._stage_end("decode")
                    # 解码器或执行器出队后的停顿可能耗尽剩余预算，
                    # 不能继续转发这些过期 PCM。
                    self._worker_media_lag_ms = self._media_lag_seconds(value, timing[0] if timing else None) * 1000
                    if self._worker_media_lag_ms >= UPLINK_MAX_QUEUE_AGE_SECONDS * 1000:
                        self._worker_phase = stage = "media_lag"
                        raise UplinkBacklogError("Uplink decoding exceeded its media lag budget")
                    if asyncio.get_running_loop().time() - enqueued_at >= UPLINK_MAX_QUEUE_AGE_SECONDS:
                        self._worker_phase = stage = "queue_age"
                        raise UplinkBacklogError("Uplink decoding exceeded its queue age budget")
                    if self.pipeline.recording:
                        self.speech_frames += 1
                        if self.speech_frames > 1502:  # 包含 Opus 前瞻与尾部刷新样本。
                            raise ProtocolError("Utterance exceeds 30 seconds")
                    self._worker_phase = stage = "asr_feed"
                    self._stage_begin("asr_feed")
                    try:
                        await self._feed_pipeline_audio(pcm)
                    finally:
                        self._stage_end("asr_feed")
                elif kind == "stream_start":
                    if self.uplink:
                        raise ProtocolError("Close the previous uplink stream first")
                    stream_id = uint32(value.get("stream_id"), "stream_id", nonzero=True)
                    if stream_id in self.used_streams or len(self.used_streams) >= 1000:
                        raise ProtocolError("Use a new stream_id (reconnect after 1000 streams)")
                    codec = value.get("codec")
                    if codec != self.uplink_codec:
                        raise ProtocolError("Uplink codec differs from handshake")
                    if (value.get("sample_rate_hz") != 16000 or value.get("channels") != 1
                            or value.get("frame_duration_ms") != 20):
                        raise ProtocolError("Uplink format must be mono 16 kHz / 20 ms")
                    self.uplink = InputStream(stream_id, codec, value.get("pre_skip_samples", 0))
                    self.used_streams.add(stream_id)
                elif kind == "stream_end":
                    if not self.uplink or value.get("stream_id") != self.uplink.id:
                        raise ProtocolError("Unknown uplink stream end")
                    self._worker_phase = stage = "asr_feed_tail"
                    self._stage_begin("asr_feed")
                    try:
                        await self._feed_pipeline_audio(self.uplink.flush(value.get("valid_end_sample")))
                    finally:
                        self._stage_end("asr_feed")
                    self.uplink.close()
                    self.uplink = None
                elif kind == "end":
                    if value["turn_id"] < self.pipeline.session.turn:
                        continue  # 这是被新一轮替代后才到达的旧发言端点。
                    if value["turn_id"] != self.pipeline.session.turn:
                        raise ProtocolError("speech.end turn_id mismatch")
                    if value["turn_id"] <= self._sealed_turn or not self.pipeline.recording:
                        continue  # 本轮所有端点共用一个结束处理任务。
                    if self.uplink:
                        # 连续流须先收到足够承载编解码前瞻的包，
                        # 才能提供准确样本边界。
                        self._worker_phase = stage = "asr_feed_tail"
                        self._stage_begin("asr_feed")
                        try:
                            await self._feed_pipeline_audio(self.uplink.flush(value.get("sample_offset")))
                        finally:
                            self._stage_end("asr_feed")
                    if self.finish_task is None or self.finish_task.done():
                        self.finish_task = asyncio.create_task(self._finish_utterance())
                        # 先让状态从 recording 转到 finishing，
                        # 再接纳同一上行流的后续静音包。
                        await asyncio.sleep(0)
            except Exception as exc:
                self.record_failure(stage, exc)
                raise
            finally:
                self.queue.task_done()
                self._worker_phase = "waiting"

    def _discard_stream(self, stream_id):
        stream_id = uint32(stream_id, "stream_id", nonzero=True)
        if stream_id not in self.used_streams and len(self.used_streams) >= 1000:
            raise ProtocolError("Reconnect after 1000 streams")
        self.used_streams.add(stream_id)
        self._discarded_streams.add(stream_id)

    async def _cancel_utterance(self, turn):
        latest = max(self._arrival_turn, self.pipeline.session.turn)
        if turn > latest:
            raise ProtocolError("speech.cancel turn_id mismatch")
        if turn == latest and turn > self._sealed_turn:
            # 先设置边界再等待：取消清理过程中，
            # 服务商可能刚好完成握手或返回最终结果。
            self._sealed_turn = turn
            self.pipeline.seal_utterance(turn)
            if self.worker:
                self.worker.cancel()
                with suppress(asyncio.CancelledError):
                    await self.worker
            if self.finish_task:
                self.finish_task.cancel()
                with suppress(asyncio.CancelledError):
                    await self.finish_task
                self.finish_task = None
            # 本轮取消之前接纳的所有内容都属于本轮或其连续流。
            # 这里运行在唯一的 WebSocket 读取任务中，
            # 因此不会并发接纳新输入。
            while not self.queue.empty():
                item = self.queue.get_nowait()
                if item[0] == "stream_start":
                    self._discard_stream(item[1].get("stream_id"))
                self.queue.task_done()
            if self.uplink:
                self._discard_stream(self.uplink.id)
                self.uplink.close()
                self.uplink = None
            await self.pipeline.cancel_utterance(turn)
            self._playback_pending = None
            self._media_origin = None
            self._stage_started.clear()
            self.worker = asyncio.create_task(self._work())
        # 重复或过期取消只确认所请求的编号，
        # 不影响新一轮的录音、回答、解码器或队列。
        await self.send_json({"type": "speech.cancelled", "session_epoch": self.pipeline.session.epoch,
                              "turn_id": turn, "generation": self.pipeline.session.generation})

    async def _finish_utterance(self):
        asr = self.pipeline.asr
        try:
            await self.pipeline.end_utterance()
        except asyncio.CancelledError:
            self._asr_outcome = "canceled"
            raise
        except Exception as exc:
            self._asr_outcome = "failed"
            self._capture_pacing(asr)
            self.record_failure("asr_final", exc)
            await self.send_json({"type": "error", **self.pipeline.session.identity(),
                                  "code": "asr_final_failed"})
            await self.ws.close(code=1011, message=b"ASR finalization failed")
        finally:
            pacing = self._capture_pacing(asr)
            diagnostics = self._diagnostic_values()
            LOGGER.info("voice_asr_summary epoch=%d turn=%d packets=%d samples=%d lag_ms=%.1f "
                        "send_ms=%.1f queue_peak=%d queue_wait_ms=%.1f queue_wait_max_ms=%.1f "
                        "decode_ms=%.1f decode_max_ms=%.1f feed_ms=%.1f feed_max_ms=%.1f "
                        "feed_inflight_ms=%.1f asr_inflight=%d asr_inflight_ms=%.1f "
                        "asr_inflight_max_ms=%.1f loop_lag_ms=%.1f loop_lag_max_ms=%.1f "
                        "receive_gap_ms=%.1f receive_gap_max_ms=%.1f receive_burst=%d "
                        "receive_burst_peak=%d receive_frames=%d metrics_scope=connection "
                        "queue_oldest_audio_ms=%.1f enqueue_wait_ms=%.1f enqueue_wait_max_ms=%.1f "
                        "worker_phase=%s arrival_turn=%d turn_receive_frames=%d turn_receive_gap_ms=%.1f "
                        "turn_receive_gap_max_ms=%.1f turn_receive_burst=%d turn_receive_burst_peak=%d "
                        "worker_media_lag_ms=%.1f turn_media_lag_ms=%.1f turn_media_lag_max_ms=%.1f", self.pipeline.session.epoch,
                        self.pipeline.session.turn, pacing.get("packets", 0), pacing.get("samples", 0),
                        pacing.get("max_lag_ms", 0), pacing.get("max_send_ms", 0), self.queue_peak,
                        diagnostics["queue_wait_ms"], diagnostics["queue_wait_max_ms"],
                        diagnostics["decode_ms"], diagnostics["decode_max_ms"], diagnostics["feed_ms"],
                        diagnostics["feed_max_ms"], diagnostics["feed_inflight_ms"], diagnostics["asr_inflight"],
                        diagnostics["asr_inflight_ms"], diagnostics["asr_inflight_max_ms"], diagnostics["loop_lag_ms"],
                        diagnostics["loop_lag_max_ms"], diagnostics["receive_gap_ms"], diagnostics["receive_gap_max_ms"],
                        diagnostics["receive_burst"], diagnostics["receive_burst_peak"], diagnostics["receive_frames"],
                        diagnostics["queue_oldest_audio_ms"], diagnostics["enqueue_wait_ms"],
                        diagnostics["enqueue_wait_max_ms"], diagnostics["worker_phase"], diagnostics["arrival_turn"],
                        diagnostics["turn_receive_frames"], diagnostics["turn_receive_gap_ms"],
                        diagnostics["turn_receive_gap_max_ms"], diagnostics["turn_receive_burst"],
                        diagnostics["turn_receive_burst_peak"], diagnostics["worker_media_lag_ms"],
                        diagnostics["turn_media_lag_ms"], diagnostics["turn_media_lag_max_ms"])
            recognition = (asr.recognition_metrics()
                           if isinstance(asr, DoubaoStreamingAsrProvider) else {})
            levels = self._pcm_levels.metrics()
            LOGGER.info("voice_asr_result epoch=%d turn=%d outcome=%s final_chars=%d "
                        "device_partials=%d device_segments=%d device_finals=%d "
                        "provider_responses=%d provider_nonempty=%d provider_partials=%d "
                        "provider_segments=%d provider_final=%d provider_final_chars=%d "
                        "pcm_samples=%d pcm_rms=%d pcm_ac_rms=%d pcm_peak=%d "
                        "pcm_clipped=%d pcm_frame_rms_max=%d", self.pipeline.session.epoch,
                        self.pipeline.session.turn, self._asr_outcome, self._asr_final_chars,
                        self._device_partial_events, self._device_segment_events, self._device_final_events,
                        recognition.get("responses", 0), recognition.get("nonempty_responses", 0),
                        recognition.get("partial_events", 0), recognition.get("segment_events", 0),
                        recognition.get("final_received", 0), recognition.get("final_chars", 0),
                        levels["pcm_samples"], levels["pcm_rms"], levels["pcm_ac_rms"],
                        levels["pcm_peak"], levels["pcm_clipped"], levels["pcm_frame_rms_max"])

    async def run(self, first):
        import aiohttp
        for direction in ("uplink", "downlink"):
            requested = first.get(direction + "_codec", self.selection.transport[direction]["codec"])
            configured = self.selection.transport[direction]["codec"]
            if requested != configured:
                if requested != "pcm_s16le" or not self.selection.transport["allow_pcm_fallback"]:
                    raise ProtocolError("Unsupported codec negotiation")
            setattr(self, direction + "_codec", requested)
        if "opus" in (self.uplink_codec, self.downlink_codec):
            native_version()  # 开始采集前明确报错，不静默退回 PCM。
        await self.send_json({"type": "session.ready", **self.pipeline.session.identity(),
                              "selection": self.selection.to_dict(),
                              "uplink_codec": self.uplink_codec, "downlink_codec": self.downlink_codec,
                              "uplink_sample_rate_hz": 16000, "downlink_sample_rate_hz": 24000,
                              "frame_duration_ms": 20})
        self.worker = asyncio.create_task(self._work())
        self._loop_probe = asyncio.create_task(self._monitor_loop())
        receive = None
        activity = None
        try:
            while not self.ws.closed:
                if self.finish_task and self.finish_task.done():
                    await self.finish_task
                    self.finish_task = None
                if receive is None:
                    receive = asyncio.create_task(self.ws.receive())
                waiting = {receive, self.worker}
                if self.selection.transport.get("idle_timeout_seconds", 0):
                    self._activity_changed.clear()
                    activity = asyncio.create_task(self._activity_changed.wait())
                    waiting.add(activity)
                # 无输出的云端操作完成时立即唤醒，
                # 让忙碌结束后的待机获得完整空闲预算。
                waiting.update(task for task in (self.finish_task, self.pipeline.response_task)
                               if task is not None and not task.done())
                timeout = self._idle_wait_seconds()
                done, _ = await asyncio.wait(waiting, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
                if activity is not None:
                    activity.cancel()
                    with suppress(asyncio.CancelledError):
                        await activity
                    activity = None
                if self.worker in done:
                    receive.cancel()
                    with suppress(asyncio.CancelledError):
                        await receive
                    await self.worker  # 立即上报工作任务失败。
                if receive not in done:
                    if self._idle_wait_seconds() == 0:
                        LOGGER.info("voice_session_idle epoch=%d turn=%d idle_timeout_seconds=%d",
                                    self.pipeline.session.epoch, self.pipeline.session.turn,
                                    self.selection.transport.get("idle_timeout_seconds", 0))
                        await self.send_json({"type": "session.closed", **self.pipeline.session.identity(),
                                              "reason": "idle_timeout"})
                        await self.ws.close(code=1000, message=b"Voice session idle")
                        return
                    continue
                completed_receive, receive = receive, None
                message = completed_receive.result()
                self.last_activity = asyncio.get_running_loop().time()
                if message.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                    close_code = message.data if message.type == aiohttp.WSMsgType.CLOSE and type(message.data) is int else self.ws.close_code
                    close_code = close_code if type(close_code) is int else 0
                    LOGGER.info("voice_ws_closed epoch=%d turn=%d close_code=%d",
                                self.pipeline.session.epoch, self.pipeline.session.turn, close_code)
                    if message.type == aiohttp.WSMsgType.ERROR or self.pipeline.recording:
                        self.record_failure("device_socket", category="device_transport_closed", close_code=close_code)
                    break
                if message.type == aiohttp.WSMsgType.BINARY:
                    packet = AudioPacket.unpack(message.data)
                    if packet.kind != UPLINK or packet.epoch != self.pipeline.session.epoch:
                        raise ProtocolError("Wrong audio direction or session_epoch")
                    # 上行独立于回答的生成编号，
                    # 即使正在取消回答，也使用值为零的 turn/generation 字段。
                    if packet.turn_id or packet.generation:
                        raise ProtocolError("Continuous uplink uses turn_id=0 and generation=0")
                    if packet.stream_id in self._discarded_streams:
                        continue
                    item = ("audio", packet)
                elif message.type == aiohttp.WSMsgType.TEXT:
                    if len(message.data.encode("utf-8")) > 8192:
                        raise ProtocolError("Control message too large")
                    event = json.loads(message.data)
                    if not isinstance(event, dict) or event.get("session_epoch") != self.pipeline.session.epoch:
                        raise ProtocolError("Wrong control session_epoch")
                    kind = event.get("type")
                    if kind == "speech.cancel":
                        turn = uint32(event.get("turn_id"), "turn_id", nonzero=True)
                        # 服务商关闭停滞时，不能无限占用唯一设备读取任务，
                        # 也不能阻塞自动 PONG。
                        # 超时后关闭本会话，不复用
                        # 尚未确认清理完成的状态。
                        async with asyncio.timeout(CANCEL_CLEANUP_SECONDS):
                            await self._cancel_utterance(turn)
                        continue
                    if kind == "response.cancel":
                        await self._cancel_response(event)
                        continue
                    if kind == "playback.done":
                        target = {key: event.get(key) for key in ("session_epoch", "turn_id", "generation")}
                        self.pipeline.confirm_playback(target)
                        if target == self._playback_pending:
                            self._playback_pending = None
                        continue
                    if kind == "speech.start":
                        turn = uint32(event.get("turn_id"), "turn_id", nonzero=True)
                        if turn <= max(self._arrival_turn, self.pipeline.session.turn, self._sealed_turn):
                            raise ProtocolError("turn_id must increase monotonically")
                        # 立即取消，不排在已入队音频
                        # 或尚未收到的上游结束确认之后等待。
                        await self.pipeline.cancel_response()
                        self._playback_pending = None
                        item = ("start", turn)
                    elif kind == "speech.end":
                        turn = uint32(event.get("turn_id"), "turn_id", nonzero=True)
                        if turn <= self._sealed_turn:
                            continue
                        item = ("end", event)
                    elif kind == "audio.stream.start":
                        if event.get("stream_id") in self._discarded_streams:
                            continue
                        if self._arrival_turn and self._arrival_turn <= self._sealed_turn:
                            self._discard_stream(event.get("stream_id"))
                            continue
                        item = ("stream_start", event)
                    elif kind == "audio.stream.end":
                        if event.get("stream_id") in self._discarded_streams:
                            continue
                        item = ("stream_end", event)
                    else:
                        raise ProtocolError("Unsupported control event")
                else:
                    continue
                await self._enqueue(item)
        except Exception as exc:
            self.record_failure("device_receive", exc)
            raise
        finally:
            if activity is not None:
                activity.cancel()
                with suppress(asyncio.CancelledError):
                    await activity
            if receive is not None:
                receive.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await receive
            if self._loop_probe:
                self._loop_probe.cancel()
                with suppress(asyncio.CancelledError):
                    await self._loop_probe
            if self.worker:
                self.worker.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await self.worker
            if self.finish_task:
                self.finish_task.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await self.finish_task
            with suppress(Exception):
                await self.pipeline.close()
            if self.uplink:
                self.uplink.close()


def make_voice_route(config, credentials, default_selection, *, provider_factory=None):
    async def route(request):
        from aiohttp import web
        from transport.app_state import CONNECTIONS, PROVIDER_HTTP
        ws = web.WebSocketResponse(heartbeat=20, max_msg_size=16384)
        connections = request.app[CONNECTIONS]
        if connections.active >= 4:
            raise web.HTTPServiceUnavailable(text="Voice session capacity reached")
        connections.active += 1
        connections.sockets.add(ws)
        connection = None
        try:
            await ws.prepare(request)
            first = await ws.receive_json(timeout=15)
            if not isinstance(first, dict) or first.get("type") != "session.start" or first.get("version") != 1:
                raise ProtocolError("Expected version 1 session.start")
            inherit = "profile" not in first
            selection = select_profile(config, profile=first.get("profile", default_selection.profile),
                asr=first.get("asr", default_selection.asr.name if inherit else None),
                llm=first.get("llm", default_selection.llm.name if inherit else None),
                tts=first.get("tts", default_selection.tts.name if inherit else None), mode="realtime")
            validate_selection_credentials(selection, credentials)
            connection = DeviceConnection(ws, config, credentials, selection,
                request.app[PROVIDER_HTTP], provider_factory=provider_factory)
            await connection.run(first)
        except (ValueError, RuntimeError, TimeoutError, TypeError, OSError) as exc:
            if connection:
                connection.record_failure("session_route", exc)
            else:
                # 握手错误没有可信身份；不得打印请求、URL、
                # 异常文本或请求的服务商编号。
                LOGGER.warning("voice_failure first=1 stage=handshake category=configuration_or_protocol")
            if not ws.closed:
                with suppress(Exception):
                    code = "uplink_backlog" if isinstance(exc, UplinkBacklogError) else "voice_session_failed"
                    await ws.send_json({"type": "error", "code": code})
                    await ws.close(code=1008, message=b"Voice protocol or provider failure")
        finally:
            connections.active -= 1
            connections.sockets.discard(ws)
        return ws
    return route
