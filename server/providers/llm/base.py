"""实现阶段 P1 提供的同步文本生成接口。"""

from collections.abc import Mapping, Sequence
from typing import Protocol


class TextLlmProvider(Protocol):
    def generate(self, messages: Sequence[Mapping[str, str]]) -> str:
        """返回回答正文，不施加传输协议特定的长度限制。"""
        ...
