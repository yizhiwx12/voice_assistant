"""与传输协议无关的适配器能力说明。

streaming_input 表示可逐段输入，streaming_output 表示可逐段输出；
partial_results 表示识别过程有中间文字，不能用它代替最终识别结果。
这些标记描述当前实现，不代表账号、模型或云端资源已通过实机验证。
"""

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class ProviderCapabilities:
    streaming_input: bool = False
    streaming_output: bool = False
    partial_results: bool = False
    endpointing: str = "none"
    audio_format: str | None = None

    def to_dict(self):
        return asdict(self)


@dataclass(frozen=True)
class AdapterInfo:
    kind: str
    name: str
    implemented: bool
    modes: tuple[str, ...]
    capabilities: ProviderCapabilities
    note: str = ""

    def to_dict(self):
        return {
            "kind": self.kind,
            "adapter": self.name,
            "implemented": self.implemented,
            "modes": list(self.modes),
            "capabilities": self.capabilities.to_dict(),
            "note": self.note,
        }
