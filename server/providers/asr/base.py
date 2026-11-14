"""兼容 HTTP 服务使用的同步批量识别接口。

流式 ASR 有独立的生命周期，由实时传输层接入；批量实现不能宣称提供中间结果。"""

from typing import Protocol


class BatchAsrProvider(Protocol):
    def recognize_wav(self, wav_data: bytes) -> str:
        """返回一轮完整 WAV 发言的全部识别文字。"""
        ...
