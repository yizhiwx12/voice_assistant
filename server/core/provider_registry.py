"""登记已实现适配器的能力，并按配置选择创建厂商实例。

配置中的条目名称可以自定，adapter 必须匹配这里的实现名。新增厂商时
需同时登记能力、接入工厂并补离线契约测试；创建实例本身不发起请求。
"""

from core.events import AdapterInfo, ProviderCapabilities


class ProviderUnavailableError(ValueError):
    pass


_ADAPTERS = {
    ("asr", "qwen_batch"): AdapterInfo(
        "asr", "qwen_batch", True, ("http", "realtime"),
        ProviderCapabilities(endpointing="explicit_end", audio_format="wav_pcm16_mono_16000"),
        "Batch recognition only; no partial results.",
    ),
    ("asr", "doubao_streaming"): AdapterInfo(
        "asr", "doubao_streaming", True, ("realtime",),
        ProviderCapabilities(streaming_input=True, partial_results=True,
                             endpointing="explicit_end", audio_format="pcm16_mono_16000"),
        "Implemented; account/resource cloud validation is still required.",
    ),
    ("llm", "chat_completions"): AdapterInfo(
        "llm", "chat_completions", True, ("http", "realtime"), ProviderCapabilities(streaming_output=True),
        "HTTP and SSE implemented; selected model/region must be verified.",
    ),
    ("tts", "volcengine_bidirectional"): AdapterInfo(
        "tts", "volcengine_bidirectional", True, ("realtime",),
        ProviderCapabilities(streaming_input=True, streaming_output=True, audio_format="pcm16_mono_24000"),
        "Implemented with offline tests; account/voice cloud validation is still required.",
    ),
    ("tts", "cosyvoice_bidirectional"): AdapterInfo(
        "tts", "cosyvoice_bidirectional", True, ("realtime",),
        ProviderCapabilities(streaming_input=True, streaming_output=True, audio_format="pcm16_mono_24000"),
        "Implemented with offline tests; account/model/voice cloud validation is still required.",
    ),
    ("tts", "gpt_sovits"): AdapterInfo(
        "tts", "gpt_sovits", True, ("realtime",),
        ProviderCapabilities(audio_format="pcm16_mono_24000"),
        "Local GPT-SoVITS HTTP API; finished text is synthesized in bounded segments with one-segment prefetch, converted to 24 kHz PCM16.",
    ),
}


def get_adapter(kind, name):
    try:
        return _ADAPTERS[(kind, name)]
    except KeyError:
        raise ProviderUnavailableError(f"Unknown {kind} adapter '{name}'") from None


def list_adapters():
    return tuple(_ADAPTERS.values())


def require_adapter(kind, name, mode):
    info = get_adapter(kind, name)
    if not info.implemented:
        raise ProviderUnavailableError(f"{kind} adapter '{name}' is planned but not implemented (P2)")
    if mode not in info.modes:
        raise ProviderUnavailableError(f"{kind} adapter '{name}' does not support '{mode}' mode yet")
    return info


def create_provider(kind, entry, credentials, opener=None):
    """创建选中的已实现适配器实例，不调用其 API。"""
    require_adapter(kind, entry.adapter, "http")
    kwargs = {} if opener is None else {"opener": opener}
    if (kind, entry.adapter) == ("asr", "qwen_batch"):
        from providers.asr.qwen_batch import QwenBatchProvider
        return QwenBatchProvider(entry.options, credentials, **kwargs)
    if (kind, entry.adapter) == ("llm", "chat_completions"):
        from providers.llm.chat_completions import ChatCompletionsProvider
        return ChatCompletionsProvider(entry.options, credentials, **kwargs)
    raise ProviderUnavailableError(f"No factory for {kind} adapter '{entry.adapter}'")


def create_realtime_provider(kind, entry, credentials, session):
    """ASR、LLM、TTS 分别创建，所以三者可按 profile 自由组合。"""
    require_adapter(kind, entry.adapter, "realtime")
    if entry.adapter == "qwen_batch":
        from providers.asr.qwen_batch import QwenBatchProvider
        return QwenBatchProvider(entry.options, credentials)
    if entry.adapter == "doubao_streaming":
        from providers.asr.doubao_streaming import DoubaoStreamingAsrProvider
        return DoubaoStreamingAsrProvider(entry.options, credentials, session)
    if entry.adapter == "chat_completions":
        from providers.llm.chat_completions import ChatCompletionsProvider
        return ChatCompletionsProvider(entry.options, credentials)
    if entry.adapter == "volcengine_bidirectional":
        from providers.tts.volcengine import VolcengineTtsProvider
        return VolcengineTtsProvider(entry.options, credentials, session)
    if entry.adapter == "cosyvoice_bidirectional":
        from providers.tts.cosyvoice import CosyVoiceTtsProvider
        return CosyVoiceTtsProvider(entry.options, credentials, session)
    if entry.adapter == "gpt_sovits":
        from providers.tts.gpt_sovits import GptSoVitsTtsProvider
        return GptSoVitsTtsProvider(entry.options, credentials, session)
    raise ProviderUnavailableError("No realtime provider factory for selected adapter")
