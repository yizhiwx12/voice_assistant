"""把 LLM 的增量文字整理成有长度上限的 TTS 文本段。

先按句末标点切分；一句过长时也必须按 limit 切开，避免厂商单次请求过大。
这里的上限按字符计算，和设备控制帧的 UTF-8 字节上限是两回事。
"""

import re


class TextSegmenter:
    def __init__(self, limit=120):
        if type(limit) is not int or limit <= 0:
            raise ValueError("Text segment limit must be a positive integer")
        self.pending = ""
        self.limit = limit

    @staticmethod
    def spoken(text):
        # 适配器已排除推理与工具字段；播报前去掉简单格式标记，
        # 显示仍保留原始回答。
        return re.sub(r"[`*_#]", "", text).strip()

    def feed(self, text):
        self.pending += text
        result = []
        while self.pending:
            match = re.search(r"[。！？!?；;\n]", self.pending)
            # 标点可能在很长的增量末尾，仍要遵守单段硬上限。
            length = min(match.end(), self.limit) if match else (
                self.limit if len(self.pending) >= self.limit else 0)
            if not length:
                break
            segment = self.spoken(self.pending[:length])
            self.pending = self.pending[length:]
            if segment:
                result.append(segment)
        return result

    def finish(self):
        text = self.spoken(self.pending)
        self.pending = ""
        return [text] if text else []
