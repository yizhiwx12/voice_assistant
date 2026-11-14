"""用一个或多个 WAV 文件模拟 ESP32 音频传输，不实际播放音频。

使用安装了 server/requirements.txt 的 Python 环境运行。
输入为 16 kHz 单声道 PCM16 WAV；输出为服务端返回的 24 kHz 单声道 PCM16 WAV。
本工具与服务端共用原始 Opus 编解码及线上协议头。
写入 WAV 不能证明用户已听到回答，因此工具从不确认实际播放完成。
"""

import argparse
import asyncio
import json
import re
import sys
import time
import wave
from contextlib import suppress
from pathlib import Path
from urllib.parse import urlsplit

SERVER_DIR = Path(__file__).resolve().parents[1] / "server"
sys.path.insert(0, str(SERVER_DIR))

from audio.opus_codec import Decoder, Encoder  # noqa: E402
from transport.voice_protocol import AudioPacket, DOWNLINK, UPLINK, ProtocolError  # noqa: E402


def read_input(path):
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels() != 1 or source.getsampwidth() != 2
                or source.getframerate() != 16000 or source.getcomptype() != "NONE"):
            raise ValueError("Input WAV must be uncompressed mono PCM16 at 16000 Hz")
        count = source.getnframes()
        if not 0 < count <= 16000 * 30:
            raise ValueError("Input WAV duration must be greater than zero and at most 30 seconds")
        pcm = source.readframes(count)
        if len(pcm) != count * 2:
            raise ValueError("Input WAV contains incomplete samples")
        return pcm


def identity(event):
    return (event.get("session_epoch"), event.get("turn_id"), event.get("generation"))


class OutputStream:
    """保留最后一帧解码数据，直到服务端给出 valid_end_sample。"""
    def __init__(self, event, epoch):
        if (event.get("session_epoch") != epoch or event.get("sample_rate_hz") != 24000
                or event.get("channels") != 1 or event.get("frame_duration_ms") != 20):
            raise ProtocolError("Unexpected downlink format or session")
        self.identity = identity(event)
        self.stream_id = event.get("stream_id")
        self.codec = event.get("codec")
        self.skip = event.get("pre_skip_samples", 0)
        if (self.codec not in ("opus", "pcm_s16le") or type(self.skip) is not int
                or not 0 <= self.skip <= 960 or (self.codec == "pcm_s16le" and self.skip)):
            raise ProtocolError("Unexpected downlink codec or pre-skip")
        self.decoder = Decoder(24000) if self.codec == "opus" else None
        self.sequence = 0
        self.offset = 0
        self.pending = b""
        self.pending_offset = 0
        self.emitted_until = 0

    def _take(self, end):
        start = max(self.skip, self.pending_offset, self.emitted_until)
        stop = min(end, self.pending_offset + len(self.pending) // 2)
        self.emitted_until = max(self.emitted_until, stop)
        if stop <= start:
            return b""
        return self.pending[(start - self.pending_offset) * 2:(stop - self.pending_offset) * 2]

    def feed(self, packet):
        if (packet.kind != DOWNLINK or packet.stream_id != self.stream_id
                or (packet.epoch, packet.turn_id, packet.generation) != self.identity
                or packet.sequence != self.sequence or packet.sample_offset != self.offset):
            raise ProtocolError("Downlink packet identity or sequence mismatch")
        if self.decoder:
            pcm = self.decoder.decode(packet.payload)
        elif len(packet.payload) == 960:
            pcm = packet.payload
        else:
            raise ProtocolError("PCM downlink frame must contain 480 samples")
        ready = self._take(self.offset)
        self.pending = pcm
        self.pending_offset = self.offset
        self.offset += 480
        self.sequence += 1
        return ready

    def finish(self, event):
        valid_end = event.get("valid_end_sample")
        if (identity(event) != self.identity or event.get("stream_id") != self.stream_id
                or type(valid_end) is not int
                or not max(self.skip, self.emitted_until) <= valid_end <= self.offset):
            raise ProtocolError("Invalid downlink valid_end_sample")
        return self._take(valid_end)

    def close(self):
        if self.decoder:
            self.decoder.close()


class TurnReport:
    """客户端观察到的时间，不代表云端内部延迟或设备播放延迟。"""
    def __init__(self, turn, codec, samples):
        self.origin = time.monotonic()
        self.data = {
            "turn_id": turn, "codec": codec, "input_samples_16000_hz": samples,
            "asr_text": "", "answer_text": "", "status": "pending", "reason": None,
            "timestamps_ms": {}, "latencies_ms": {}, "errors": [],
            "uplink": {"packets": 0, "payload_bytes": 0, "wire_bytes": 0},
            "downlink": {"packets": 0, "payload_bytes": 0, "wire_bytes": 0,
                         "discarded_cancelled_packets": 0, "discarded_cancelled_wire_bytes": 0,
                         "late_previous_turn_packets": 0},
        }

    def mark(self, name, *, replace=False):
        stamps = self.data["timestamps_ms"]
        if replace or name not in stamps:
            stamps[name] = round((time.monotonic() - self.origin) * 1000, 3)

    def packet(self, direction, payload_length):
        values = self.data[direction]
        values["packets"] += 1
        values["payload_bytes"] += payload_length
        values["wire_bytes"] += payload_length + 32

    def finish(self, pcm):
        self.data["output_samples_24000_hz"] = len(pcm) // 2
        stamps = self.data["timestamps_ms"]
        pairs = {
            "input_commit_to_asr_final": ("input_committed", "asr_final"),
            "last_uplink_packet_to_asr_final": ("uplink_last_packet_sent", "asr_final"),
            "asr_final_to_llm_first_delta": ("asr_final", "llm_first_delta"),
            "llm_first_delta_to_first_audio": ("llm_first_delta", "downlink_first_packet"),
            "input_commit_to_first_audio": ("input_committed", "downlink_first_packet"),
            "cancel_request_to_ack": ("cancel_requested", "cancel_ack"),
        }
        self.data["latencies_ms"] = {
            name: round(stamps[end] - stamps[start], 3)
            for name, (start, end) in pairs.items() if start in stamps and end in stamps
        }


class Receiver:
    def __init__(self, ws, epoch, cancel_after_ms=None, *, turn=1, report=None,
                 cancelled=None, cancel_trigger="response-start"):
        self.ws = ws
        self.epoch = epoch
        self.turn = turn
        self.report = report or TurnReport(turn, "unknown", 0)
        self.cancel_after_ms = cancel_after_ms
        self.cancel_trigger = cancel_trigger
        self.streams = {}
        self.cancelled = cancelled if cancelled is not None else set()
        self.pcm = bytearray()
        self.cancel_task = None
        self.response_done = False
        self.completed = asyncio.Event()
        self.speech_started = asyncio.Event()
        self.cancel_requested = False

    def append_pcm(self, data):
        if len(self.pcm) + len(data) > 24000 * 2 * 180:
            raise ProtocolError("Returned audio exceeds the 180 second recording limit")
        self.pcm.extend(data)

    async def cancel_later(self, target):
        await asyncio.sleep(self.cancel_after_ms / 1000)
        if self.response_done:
            return
        await self.cancel(target)

    async def cancel(self, target):
        # 发送前先在本地封闭，避免迟到的数据包进入解码器。
        self.cancel_requested = True
        self.cancelled.add(target)
        for stream_id, stream in list(self.streams.items()):
            if stream.identity == target:
                stream.close()
                del self.streams[stream_id]
        self.report.mark("cancel_requested")
        await self.ws.send_json({"type": "response.cancel", "session_epoch": target[0],
                                 "turn_id": target[1], "generation": target[2]})

    async def schedule_cancel(self, trigger, target):
        if (self.cancel_after_ms is None or self.cancel_trigger != trigger
                or self.cancel_task is not None or self.cancel_requested):
            return
        if self.cancel_after_ms == 0:
            await self.cancel(target)
        else:
            self.cancel_task = asyncio.create_task(self.cancel_later(target))

    def seal_streams(self):
        for stream in self.streams.values():
            stream.close()
        self.streams.clear()

    async def run(self):
        import aiohttp

        try:
            async for message in self.ws:
                if message.type == aiohttp.WSMsgType.BINARY:
                    packet = AudioPacket.unpack(message.data)
                    if packet.epoch != self.epoch:
                        raise ProtocolError("Audio packet has the wrong session epoch")
                    if (packet.epoch, packet.turn_id, packet.generation) in self.cancelled:
                        values = self.report.data["downlink"]
                        if packet.turn_id < self.turn:
                            values["late_previous_turn_packets"] += 1
                            continue
                        self.report.packet("downlink", len(packet.payload))
                        values["discarded_cancelled_packets"] += 1
                        values["discarded_cancelled_wire_bytes"] += len(message.data)
                        continue
                    if packet.turn_id != self.turn:
                        raise ProtocolError("Audio packet belongs to another turn")
                    self.report.packet("downlink", len(packet.payload))
                    stream = self.streams.get(packet.stream_id)
                    if stream is None:
                        raise ProtocolError("Audio packet arrived without stream start")
                    self.report.mark("downlink_first_packet")
                    pcm = stream.feed(packet)
                    if pcm:
                        self.report.mark("downlink_first_decoded_pcm")
                    self.append_pcm(pcm)
                    await self.schedule_cancel("first-audio", stream.identity)
                elif message.type == aiohttp.WSMsgType.TEXT:
                    event = json.loads(message.data)
                    if not isinstance(event, dict):
                        raise ProtocolError("Server control message must be an object")
                    kind = event.get("type")
                    if (event.get("session_epoch") == self.epoch
                            and type(event.get("turn_id")) is int
                            and event["turn_id"] < self.turn):
                        continue
                    if kind == "error":
                        # 只保留已知错误码，不保留任意上游文字。
                        code = event.get("code")
                        allowed = {"tts_failed", "response_failed", "asr_events_failed",
                                   "asr_final_failed", "voice_session_failed"}
                        code = code if code in allowed else "unknown_server_error"
                        self.report.data["errors"].append(code)
                        self.report.data.update(status="failed", reason=code)
                        print("Server error:", code, flush=True)
                        if code not in ("tts_failed", "response_failed"):
                            raise RuntimeError("Server rejected the voice session")
                        if code == "response_failed":
                            self.response_done = True
                            self.seal_streams()
                    elif event.get("session_epoch") != self.epoch:
                        raise ProtocolError("Server control message has the wrong session epoch")
                    elif event.get("turn_id", self.turn) < self.turn:
                        # 下一轮开始时，队列中可能已有旧取消 ACK。
                        # 它绝不能结束新回合。
                        continue
                    elif event.get("turn_id", self.turn) != self.turn:
                        raise ProtocolError("Control message belongs to another turn")
                    elif kind == "speech.started":
                        self.report.mark("speech_started")
                        self.speech_started.set()
                    elif kind == "audio.stream.start":
                        if identity(event) not in self.cancelled:
                            stream_id = event.get("stream_id")
                            if self.streams or stream_id in self.streams:
                                raise ProtocolError("Overlapping downlink streams")
                            self.streams[stream_id] = OutputStream(event, self.epoch)
                            self.report.mark("downlink_stream_started")
                    elif kind == "audio.stream.end":
                        if identity(event) not in self.cancelled:
                            stream = self.streams.pop(event.get("stream_id"), None)
                            if stream is None:
                                raise ProtocolError("Unknown downlink stream end")
                            try:
                                self.append_pcm(stream.finish(event))
                                self.report.mark("downlink_stream_ended")
                            finally:
                                stream.close()
                    elif kind in ("asr.utterance_final", "llm.delta"):
                        if identity(event) not in self.cancelled:
                            label = "ASR" if kind == "asr.utterance_final" else "LLM"
                            text = event.get("text", "")
                            if not isinstance(text, str):
                                raise ProtocolError("Server text must be a string")
                            self.report.mark("asr_final" if label == "ASR" else "llm_first_delta")
                            if label == "ASR":
                                self.report.data["asr_text"] = text
                            else:
                                self.report.data["answer_text"] += text
                            print(f"{label}: {text}", flush=True)
                    elif kind in ("asr.partial", "asr.segment_final"):
                        self.report.mark("asr_first_partial")
                    elif kind == "response.started":
                        self.report.data["response_generation"] = event.get("generation")
                        self.report.mark("response_started")
                        await self.schedule_cancel("response-start", identity(event))
                    elif kind in ("response.done", "response.cancelled"):
                        if kind == "response.done":
                            self.report.mark("response_done")
                            self.response_done = not self.cancel_requested
                            reason = event.get("reason")
                            if reason != "complete":
                                safe_reason = reason if reason in ("tts_failed", "empty_utterance") else "unknown_response_reason"
                                self.report.data.update(status="failed", reason=safe_reason)
                            elif not self.cancel_requested and not self.report.data["errors"]:
                                missing = []
                                if not self.report.data["asr_text"].strip():
                                    missing.append("asr_text")
                                if not self.report.data["answer_text"].strip():
                                    missing.append("answer_text")
                                if not self.pcm:
                                    missing.append("returned_audio")
                                if self.streams or "downlink_stream_ended" not in self.report.data["timestamps_ms"]:
                                    missing.append("audio_stream_end")
                                if missing:
                                    self.report.data.update(status="failed", reason="incomplete_response",
                                                            missing=missing)
                                    self.seal_streams()
                                else:
                                    self.report.data.update(status="complete", reason="complete")
                            if reason != "complete" and not self.cancel_requested:
                                self.seal_streams()
                        else:
                            if not self.cancel_requested or identity(event) not in self.cancelled:
                                raise ProtocolError("Unexpected cancellation ACK")
                            self.report.mark("cancel_ack")
                            self.response_done = True
                            if not self.report.data["errors"]:
                                self.report.data.update(status="cancelled", reason="requested")
                        if kind == "response.cancelled" or event.get("reason") == "tts_failed":
                            self.cancelled.add(identity(event))
                            self.seal_streams()
                    if self.response_done and not self.streams:
                        self.completed.set()
                        return
                elif message.type == aiohttp.WSMsgType.ERROR:
                    raise RuntimeError("Voice WebSocket connection failed")
            if not self.completed.is_set():
                raise RuntimeError("Voice WebSocket closed before response completion")
        finally:
            if self.cancel_task:
                self.cancel_task.cancel()
                with suppress(asyncio.CancelledError):
                    await self.cancel_task
            self.seal_streams()


async def send_input(ws, pcm, epoch, codec, *, turn=1, stream_id=1,
                     started=None, report=None):
    encoder = Encoder(16000, 24000) if codec == "opus" else None
    try:
        if report:
            report.mark("speech_start_sent")
        await ws.send_json({"type": "speech.start", "session_epoch": epoch, "turn_id": turn})
        if started:
            # 云端 ASR 启动可能超过输入队列有界的 500 毫秒容量。
            # 先等待就绪，不提前发送已有 WAV。
            await asyncio.wait_for(started.wait(), 30)
        await ws.send_json({"type": "audio.stream.start", "session_epoch": epoch,
                            "stream_id": stream_id, "codec": codec, "sample_rate_hz": 16000,
                            "channels": 1, "frame_duration_ms": 20,
                            "pre_skip_samples": encoder.pre_skip if encoder else 0})
        sequence = 0
        deadline = asyncio.get_running_loop().time()

        async def send(packet):
            nonlocal sequence, deadline
            delay = deadline - asyncio.get_running_loop().time()
            if delay > 0:
                await asyncio.sleep(delay)
            await ws.send_bytes(AudioPacket(UPLINK, epoch, stream_id, 0, 0, sequence,
                                            sequence * 320, packet).pack())
            if report:
                report.packet("uplink", len(packet))
                report.mark("uplink_first_packet_sent")
                report.mark("uplink_last_packet_sent", replace=True)
            sequence += 1
            deadline += 0.02

        for offset in range(0, len(pcm), 640):
            frame = pcm[offset:offset + 640]
            packets = encoder.feed(frame) if encoder else [frame.ljust(640, b"\0")]
            for packet in packets:
                await send(packet)
        if encoder:
            for packet in encoder.finish():
                await send(packet)
        valid_end = (encoder.pre_skip if encoder else 0) + len(pcm) // 2
        await ws.send_json({"type": "audio.stream.end", "session_epoch": epoch,
                            "stream_id": stream_id, "valid_end_sample": valid_end})
        await ws.send_json({"type": "speech.end", "session_epoch": epoch, "turn_id": turn})
        if report:
            report.mark("input_committed")
    finally:
        if encoder:
            encoder.close()


def output_path(base, turn, count):
    return base if count == 1 else base.with_name(f"{base.stem}_turn{turn:02d}{base.suffix}")


def write_wav(path, pcm):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(24000)
        output.writeframes(pcm)


def safe_selection(value):
    """只复制模型标识，不包含 URL、凭证或任意 JSON。"""
    if not isinstance(value, dict):
        return {}

    def token(text):
        return isinstance(text, str) and re.fullmatch(r"[A-Za-z0-9_.-]{1,160}", text)

    result = {}
    if token(value.get("profile")):
        result["profile"] = value["profile"]
    for kind in ("asr", "llm", "tts"):
        entry = value.get(kind)
        if isinstance(entry, dict):
            result[kind] = {key: entry[key] for key in
                            ("id", "adapter", "model", "resource_id", "voice")
                            if token(entry.get(key))}
    return result


async def run(args):
    """返回检查报告；即使上游或传输检查失败，也将报告持久化。"""
    import aiohttp

    paths = [args.input] if isinstance(args.input, Path) else args.input
    if not 1 <= len(paths) <= 1000:
        raise ValueError("Provide one to 1000 input WAV files")
    # 打开依赖云端的会话前，先校验文件。
    inputs = [read_input(path) for path in paths]
    parsed_url = urlsplit(args.url)
    if (parsed_url.scheme not in ("ws", "wss") or not parsed_url.hostname
            or parsed_url.username or parsed_url.password or parsed_url.query or parsed_url.fragment):
        raise ValueError("--url must be a ws:// or wss:// URL without credentials, query or fragment")
    first = {"type": "session.start", "version": 1,
             "uplink_codec": args.codec, "downlink_codec": args.codec}
    for field in ("profile", "asr", "llm", "tts"):
        value = getattr(args, field, None)
        if value is not None:
            first[field] = value
    report_path = getattr(args, "report", None) or args.output.with_suffix(".json")
    result = {
        "schema_version": 1, "status": "pending", "codec": args.codec,
        "playback_performed": False, "playback_acknowledged": False,
        "timing_scope": "Client monotonic receipt/send times; no cloud internal or playback timings",
        "wire_bytes_scope": "Audio payload plus 32-byte application header; excludes WebSocket/TCP/TLS/IP",
        "turns": [],
    }
    failed = False
    cancelled = set()
    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=None)) as session:
            async with session.ws_connect(args.url, heartbeat=20, max_msg_size=16384) as ws:
                await ws.send_json(first)
                ready = await ws.receive_json(timeout=15)
                epoch = ready.get("session_epoch") if isinstance(ready, dict) else None
                if (not isinstance(ready, dict) or ready.get("type") != "session.ready"
                        or type(epoch) is not int or not 0 < epoch <= 0xFFFFFFFF):
                    raise ProtocolError("Server did not accept session.start")
                if ready.get("uplink_codec") != args.codec or ready.get("downlink_codec") != args.codec:
                    raise ProtocolError("Server codec differs from requested format")
                result["session_epoch"] = epoch
                result["selection"] = safe_selection(ready.get("selection"))
                for turn, pcm in enumerate(inputs, 1):
                    report = TurnReport(turn, args.codec, len(pcm) // 2)
                    result["turns"].append(report.data)
                    cancel_after = (getattr(args, "cancel_after_ms", None)
                                    if turn == getattr(args, "cancel_turn", 1) else None)
                    receiver = Receiver(ws, epoch, cancel_after, turn=turn, report=report,
                                        cancelled=cancelled,
                                        cancel_trigger=getattr(args, "cancel_trigger", "response-start"))
                    receive_task = asyncio.create_task(receiver.run())
                    send_task = asyncio.create_task(send_input(
                        ws, pcm, epoch, args.codec, turn=turn, stream_id=turn,
                        started=receiver.speech_started, report=report))
                    try:
                        async with asyncio.timeout(180):
                            done, _ = await asyncio.wait((send_task, receive_task),
                                                        return_when=asyncio.FIRST_COMPLETED)
                            if receive_task in done:
                                await receive_task
                                if not send_task.done():
                                    raise ProtocolError("Server completed a response before input finished")
                            await send_task
                            await receive_task
                        if cancel_after is not None and "cancel_ack" not in report.data["timestamps_ms"]:
                            report.data.update(status="failed", reason="requested_cancel_not_observed")
                        failed |= report.data["status"] == "failed"
                    except Exception:
                        if report.data["status"] != "failed":
                            report.data.update(status="failed", reason="transport_or_protocol_failure")
                        raise
                    finally:
                        for task in (send_task, receive_task):
                            task.cancel()
                        for task in (send_task, receive_task):
                            with suppress(asyncio.CancelledError, Exception):
                                await task
                        report.finish(receiver.pcm)
                        path = output_path(args.output, turn, len(inputs))
                        write_wav(path, receiver.pcm)
                        report.data["output_file"] = path.name
                        print(f"Turn {turn}: {report.data['status']}; saved "
                              f"{len(receiver.pcm) // 2} samples at 24000 Hz", flush=True)
    except asyncio.CancelledError:
        failed = True
        result["error"] = "client_cancelled"
        raise
    except Exception:
        failed = True
        result["error"] = "voice_session_failed"
    finally:
        result["status"] = "failed" if failed else "passed"
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if failed:
        raise RuntimeError("Voice acceptance failed; inspect the JSON report") from None
    return result


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, nargs="+",
                        help="Mono PCM16/16 kHz WAV files (up to 30 seconds each), sent as consecutive turns")
    parser.add_argument("--url", default="ws://127.0.0.1:8000/ws/voice")
    for field in ("profile", "asr", "llm", "tts"):
        parser.add_argument("--" + field, help="Explicit server model/profile selection")
    parser.add_argument("--codec", choices=("opus", "pcm_s16le"), default="opus")
    parser.add_argument("--output", type=Path, default=SERVER_DIR / "data" / "voice_client_reply.wav")
    parser.add_argument("--report", type=Path, help="JSON report path; default is --output with .json suffix")
    parser.add_argument("--cancel-after-ms", type=int, help="Cancel this many milliseconds after the selected trigger")
    parser.add_argument("--cancel-turn", type=int, default=1, help="1-based turn to cancel; default 1")
    parser.add_argument("--cancel-trigger", choices=("response-start", "first-audio"), default="response-start",
                        help="Start the cancellation timer after response.started or the first audio packet")
    return parser


def main():
    parser = argument_parser()
    args = parser.parse_args()
    if args.cancel_after_ms is not None and args.cancel_after_ms < 0:
        parser.error("--cancel-after-ms must be nonnegative")
    if not 1 <= args.cancel_turn <= len(args.input):
        parser.error("--cancel-turn must select one of the input WAV files")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        raise SystemExit(130) from None
    except Exception:
        # 网络库异常载荷可能包含完整 URL。
        raise SystemExit("Voice client failed; check the JSON report, server, dependencies and input format") from None


if __name__ == "__main__":
    main()
