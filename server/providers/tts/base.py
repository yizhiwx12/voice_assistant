"""双向流式语音合成适配器共用的接口约定。

每个回答音频流创建一个适配器。start() 打开上游连接并等待确认；append_text()
发送确定的文字；finish() 提交文字结束标记，不等待音频全部输出。并行运行
audio()，持续接收 24 kHz 单声道、小端 PCM16，直到合成结束。每个适配器
只能有一个 audio() 读取者。cancel()/close() 可重复调用并负责关闭上游连接；
调用者还需取消并等待自己的读取任务。异常使用固定的安全描述，不透传上游载荷。"""

from dataclasses import dataclass, field
import hashlib
import struct
from typing import AsyncIterator, Protocol


@dataclass(frozen=True)
class SubtitleWord:
    """与会话内未压缩 24 kHz PCM 样本坐标对齐的原始文字。"""

    word: str
    start_sample: int
    end_sample: int


@dataclass(frozen=True)
class SubtitleCue:
    text: str
    words: tuple[SubtitleWord, ...]
    upstream_event: int
    # 仅供进程内诊断使用；这两个值不参与身份判断、
    # 文字组装、相等性比较或下行协议。
    received_at: float | None = field(default=None, compare=False, repr=False)
    source_id: str | None = field(default=None, compare=False, repr=False)

    @property
    def end_sample(self) -> int:
        return max((word.end_sample for word in self.words), default=0)

    def fingerprint(self) -> bytes:
        """生成长度受限的去重键，不保留重复的累计文字。"""
        digest = hashlib.sha256(self.text.encode("utf-8"))
        for word in self.words:
            encoded = word.word.encode("utf-8")
            digest.update(struct.pack(">QQI", word.start_sample, word.end_sample, len(encoded)))
            digest.update(encoded)
        return digest.digest()


@dataclass(frozen=True)
class SubtitleUnavailable:
    """可选逐词时间信息失效；音频可以继续播放并退出自动跟随。"""

    reason: str = "upstream_invalid"
    detail: str = ""
    upstream_event: int = 0
    upstream_events: int = 0


class StreamingTtsProvider(Protocol):
    codec: str
    sample_rate_hz: int
    channels: int
    sample_width_bytes: int

    async def start(self) -> None: ...
    async def append_text(self, text: str) -> None: ...
    async def finish(self) -> None: ...
    async def cancel(self) -> None: ...
    async def close(self) -> None: ...
    def audio(self) -> AsyncIterator[bytes]: ...
