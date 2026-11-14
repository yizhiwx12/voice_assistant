"""模型配置入口：读取 JSON → 校验 → 生成不可变快照 → 按 profile 选三层模型。

voice_config.json 只保存模型参数和凭证的名字，真正的密钥从独立文件/环境变量读取。
一个快照供本次启动及其会话共用；编辑配置后重启服务才生效。
"""

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from types import MappingProxyType
from typing import Mapping
from urllib.parse import urlsplit

from core.provider_registry import get_adapter, require_adapter


SERVER_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG_PATH = SERVER_DIR / "config" / "voice_config.json"
EXAMPLE_CONFIG_PATH = SERVER_DIR / "config" / "voice_config.example.json"
DEFAULT_SECRETS_PATH = SERVER_DIR / "config" / "server_secrets.json"
LEGACY_KEYS = frozenset({
    "DASHSCOPE_API_KEY", "DASHSCOPE_WORKSPACE_ID", "DASHSCOPE_ASR_MODEL",
    "DEEPSEEK_API_KEY", "DEEPSEEK_MODEL",
})
KNOWN_SECRET_KEYS = LEGACY_KEYS | frozenset({
    "VOLCENGINE_ASR_API_KEY", "VOLCENGINE_TTS_API_KEY",
    "VOLCENGINE_ASR_APP_ID", "VOLCENGINE_TTS_APP_ID",
    "ANYSEARCH_API_KEY", "SHOWAPI_APP_KEY",
})
DEFAULT_NEWS_OPTIONS = {
    "enabled": False,
    "credential_ref": "SHOWAPI_APP_KEY",
    "channel_name": "国内最新",
    "max_results": 3,
    "timeout_s": 10,
}
_REF_PATTERN = re.compile(r"^[A-Z][A-Z0-9_]*$")
_IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,79}$")
_MODEL_OPTIONS = {
    "qwen_batch": {"model", "model_ref", "workspace_ref", "endpoint", "timeout_s", "language_hints"},
    "doubao_streaming": {"model", "resource_id", "endpoint", "timeout_s", "chunk_duration_ms", "max_utterance_s", "enable_nonstream", "enable_ddc", "end_window_size"},
    "chat_completions": {"model", "model_ref", "base_url", "endpoint", "max_tokens", "timeout_s", "temperature", "top_p", "enable_web_search"},
    "volcengine_bidirectional": {"resource_id", "voice", "endpoint", "app_id_ref", "timeout_s"},
    "cosyvoice_bidirectional": {"model", "voice", "endpoint", "timeout_s"},
    "gpt_sovits": {
        "endpoint", "refer_wav_path", "prompt_text", "prompt_language",
        "text_language", "speed", "timeout_s", "max_audio_bytes",
        "first_segment_chars", "segment_chars",
    },
}


class ConfigurationError(ValueError):
    pass


def freeze(value):
    if isinstance(value, Mapping):
        return MappingProxyType({key: freeze(item) for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(freeze(item) for item in value)
    return value


def _read_json(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except UnicodeError:
        raise ConfigurationError(f"{path.name} must be saved as UTF-8 (请用 UTF-8 编码保存)") from None
    except OSError as exc:
        raise ConfigurationError(f"Unable to read {path.name}") from exc
    except json.JSONDecodeError as exc:
        # 不要引用错误的 JSON 原文行，凭证文件中可能含真实密钥。
        raise ConfigurationError(f"Invalid JSON in {path.name} at line {exc.lineno}, column {exc.colno} (检查英文双引号、逗号；JSON 不支持注释)") from None
    if not isinstance(value, dict):
        raise ConfigurationError(f"{path.name} must contain a JSON object")
    return value


def load_secrets(path=None, *, environ=None, references=()):
    """只解析允许的引用名；环境变量显式设置时优先，即使值为空。"""
    path = Path(path) if path is not None else DEFAULT_SECRETS_PATH
    values = _read_json(path) if path.exists() else {}
    environment = os.environ if environ is None else environ
    allowed = KNOWN_SECRET_KEYS | frozenset(references)
    resolved = {}
    for key in allowed:
        if not isinstance(key, str) or not _REF_PATTERN.fullmatch(key):
            raise ConfigurationError("Credential references must be uppercase environment names")
        value = environment[key] if key in environment else values.get(key)
        if isinstance(value, str):
            resolved[key] = value.strip()
    return freeze(resolved)


def load_local_config(config_path=None):
    """供 audio_test_server 原始入口使用的兼容配置加载器。"""
    path = Path(config_path) if config_path is not None else DEFAULT_SECRETS_PATH
    if not path.exists():
        return {}
    try:
        values = _read_json(path)
    except ConfigurationError as exc:
        raise RuntimeError(str(exc)) from exc
    accepted = {}
    for key in LEGACY_KEYS:
        value = values.get(key)
        if isinstance(value, str) and value.strip():
            accepted[key] = value.strip()
            os.environ.setdefault(key, value.strip())
    return accepted


@dataclass(frozen=True)
class ModelConfig:
    name: str
    kind: str
    adapter: str
    options: Mapping


@dataclass(frozen=True)
class VoiceConfig:
    schema_version: int
    active_profile: str
    transport: Mapping
    models: Mapping
    profiles: Mapping
    news: Mapping = field(default_factory=lambda: MappingProxyType({}))


@dataclass(frozen=True)
class Selection:
    profile: str
    mode: str
    asr: ModelConfig
    llm: ModelConfig
    tts: ModelConfig | None
    require_streaming_asr: bool
    transport: Mapping
    news: Mapping = field(default_factory=lambda: MappingProxyType({}))

    def to_dict(self):
        """可安全公开的配置摘要，不含凭证或任意扩展参数。"""
        def describe(entry):
            if entry is None:
                return {"id": "none", "adapter": None}
            info = get_adapter(entry.kind, entry.adapter)
            result = {
                "id": entry.name, "adapter": entry.adapter,
                "capabilities": info.capabilities.to_dict(),
            }
            for key in ("model", "voice", "resource_id"):
                if key in entry.options:
                    result[key] = entry.options[key]
            return result
        return {
            "profile": self.profile, "mode": self.mode,
            "require_streaming_asr": self.require_streaming_asr,
            "asr": describe(self.asr), "llm": describe(self.llm), "tts": describe(self.tts),
            "news": {"enabled": self.news.get("enabled", False), "provider": "showapi",
                     "channel_name": self.news.get("channel_name", "国内最新"),
                     "max_results": self.news.get("max_results", 3)},
        }


def _object(value, label):
    if not isinstance(value, dict):
        raise ConfigurationError(f"{label} must be an object")
    return value


def _text(value, label):
    if not isinstance(value, str) or not value.strip():
        raise ConfigurationError(f"{label} must be a nonempty string")
    return value.strip()


def _identifier(value, label):
    value = _text(value, label)
    if not _IDENTIFIER_PATTERN.fullmatch(value):
        raise ConfigurationError(f"{label} must be a simple identifier (up to 80 characters)")
    return value


def _validate_transport(value):
    value = _object(value, "transport")
    allowed = {"frame_duration_ms", "channels", "vbr", "dtx", "allow_pcm_fallback", "idle_timeout_seconds",
               "fallback_codec", "uplink", "downlink"}
    if set(value) - allowed:
        raise ConfigurationError("Unknown transport setting")
    idle_timeout = value.get("idle_timeout_seconds", 0)
    if type(idle_timeout) is not int or not 0 <= idle_timeout <= 86400:
        raise ConfigurationError("transport.idle_timeout_seconds must be an integer between 0 and 86400 (0 disables idle closing)")
    value = {**value, "idle_timeout_seconds": idle_timeout}
    if type(value.get("frame_duration_ms")) is not int or value["frame_duration_ms"] != 20:
        raise ConfigurationError("transport.frame_duration_ms must be 20")
    if type(value.get("channels")) is not int or value["channels"] != 1:
        raise ConfigurationError("transport.channels must be 1")
    for key in ("vbr", "dtx", "allow_pcm_fallback"):
        if type(value.get(key)) is not bool:
            raise ConfigurationError(f"transport.{key} must be a boolean")
    if value["dtx"]:
        raise ConfigurationError("transport.dtx must be false for the initial protocol")
    if value.get("fallback_codec") != "pcm_s16le":
        raise ConfigurationError("transport.fallback_codec must be pcm_s16le")
    for direction, rate in (("uplink", 16000), ("downlink", 24000)):
        stream = _object(value.get(direction), f"transport.{direction}")
        if set(stream) - {"codec", "sample_rate_hz", "bitrate_bps"}:
            raise ConfigurationError(f"Unknown transport.{direction} setting")
        if stream.get("codec") not in ("opus", "pcm_s16le"):
            raise ConfigurationError(f"transport.{direction}.codec must be opus or pcm_s16le")
        if type(stream.get("sample_rate_hz")) is not int or stream["sample_rate_hz"] != rate:
            raise ConfigurationError(f"transport.{direction}.sample_rate_hz must be {rate}")
        if type(stream.get("bitrate_bps")) is not int or not 6000 <= stream["bitrate_bps"] <= 128000:
            raise ConfigurationError(f"transport.{direction}.bitrate_bps must be between 6000 and 128000")
    return freeze(value)


def load_voice_config(path=None, *, environ=None):
    """读取后冻结；磁盘文件变化不会修改当前生效的模型选择。"""
    source = Path(path) if path is not None else DEFAULT_CONFIG_PATH
    if path is None and not source.exists():
        source = EXAMPLE_CONFIG_PATH
    data = _read_json(source)
    return parse_voice_config(data, environ=environ)


def _validate_news(value):
    """新闻能力独立于聊天模型的联网开关；密钥只保留引用名。"""
    value = _object(value, "news")
    if set(value) - set(DEFAULT_NEWS_OPTIONS):
        raise ConfigurationError("Unknown news setting (新闻配置只接受 enabled/credential_ref/channel_name/max_results/timeout_s)")
    options = {**DEFAULT_NEWS_OPTIONS, **value}
    if type(options["enabled"]) is not bool:
        raise ConfigurationError("news.enabled must be a boolean (请填写 true 或 false)")
    ref = options["credential_ref"]
    if not isinstance(ref, str) or not _REF_PATTERN.fullmatch(ref):
        raise ConfigurationError("news.credential_ref must name an uppercase environment variable")
    channel = options["channel_name"]
    if not isinstance(channel, str) or len(channel) > 80 or any(ord(char) < 32 for char in channel):
        raise ConfigurationError("news.channel_name must be a string of up to 80 characters")
    options["channel_name"] = channel.strip()
    if type(options["max_results"]) is not int or not 1 <= options["max_results"] <= 5:
        raise ConfigurationError("news.max_results must be an integer between 1 and 5")
    if type(options["timeout_s"]) not in (int, float) or not 1 <= options["timeout_s"] <= 20:
        raise ConfigurationError("news.timeout_s must be between 1 and 20")
    return freeze(options)


def parse_voice_config(data, *, environ=None):
    """文件加载和交互编辑共用的纯内存校验，不修改传入的可编辑对象。"""
    data = _object(data, "voice configuration")
    if set(data) - {"schema_version", "active_profile", "transport", "asr", "llm", "tts", "profiles", "news"}:
        raise ConfigurationError("Unknown top-level voice configuration setting")
    if type(data.get("schema_version")) is not int or data["schema_version"] != 1:
        raise ConfigurationError("schema_version must be 1")
    environment = os.environ if environ is None else environ
    models = {}
    for kind in ("asr", "llm", "tts"):
        entries = _object(data.get(kind), kind)
        models[kind] = {}
        for name, options in entries.items():
            _identifier(name, f"{kind} model name")
            if kind == "tts" and name == "none":
                raise ConfigurationError("tts name 'none' is reserved for disabled synthesis")
            options = dict(_object(options, f"{kind}.{name}"))
            adapter = _text(options.get("adapter"), f"{kind}.{name}.adapter")
            get_adapter(kind, adapter)
            if any(key.lower() in {"api_key", "token", "secret", "password", "credentials"} for key in options):
                raise ConfigurationError(f"{kind}.{name} must use credential_ref instead of inline secrets")
            if set(options) - (_MODEL_OPTIONS[adapter] | {"adapter", "credential_ref"}):
                raise ConfigurationError(f"Unknown option in {kind}.{name} (请核对该适配器支持的字段)")
            for key in ("model", "voice", "resource_id", "endpoint", "base_url"):
                if key in options:
                    options[key] = _text(options[key], f"{kind}.{name}.{key}")
            for key, low, high in (("timeout_s", 0.1, 300), ("temperature", 0, 2), ("top_p", 0.0001, 1)):
                if key in options and (type(options[key]) not in (int, float) or not low <= options[key] <= high):
                    raise ConfigurationError(f"{kind}.{name}.{key} must be between {low} and {high}")
            if "max_tokens" in options and (type(options["max_tokens"]) is not int or not 1 <= options["max_tokens"] <= 65536):
                raise ConfigurationError(f"{kind}.{name}.max_tokens must be an integer between 1 and 65536")
            if "enable_web_search" in options and type(options["enable_web_search"]) is not bool:
                raise ConfigurationError(f"{kind}.{name}.enable_web_search must be a boolean")
            # 把厂商原先在联网后才发现的参数错误前移到离线配置检查。
            if adapter == "doubao_streaming":
                if options.get("model") != "bigmodel":
                    raise ConfigurationError(f"{kind}.{name}.model must be bigmodel")
                if "chunk_duration_ms" in options and (type(options["chunk_duration_ms"]) is not int or options["chunk_duration_ms"] not in (100, 200)):
                    raise ConfigurationError(f"{kind}.{name}.chunk_duration_ms must be 100 or 200")
                for key in ("enable_nonstream", "enable_ddc"):
                    if key in options and type(options[key]) is not bool:
                        raise ConfigurationError(f"{kind}.{name}.{key} must be a boolean")
                if "end_window_size" in options and (type(options["end_window_size"]) is not int or not 300 <= options["end_window_size"] <= 5000):
                    raise ConfigurationError(f"{kind}.{name}.end_window_size must be an integer between 300 and 5000")
                if "max_utterance_s" in options and (type(options["max_utterance_s"]) not in (int, float) or not 0 < options["max_utterance_s"] <= 300):
                    raise ConfigurationError(f"{kind}.{name}.max_utterance_s must be between 0 and 300 (exclusive of 0)")
            if "language_hints" in options:
                hints = options["language_hints"]
                if not isinstance(hints, list) or not hints or any(not isinstance(hint, str) or not hint.strip() for hint in hints):
                    raise ConfigurationError(f"{kind}.{name}.language_hints must be a nonempty string list")
            for key, ref in options.items():
                if key.endswith("_ref"):
                    if not isinstance(ref, str) or not _REF_PATTERN.fullmatch(ref):
                        raise ConfigurationError(f"{kind}.{name}.{key} must name an uppercase environment variable")
            # 本地 GPT-SoVITS HTTP API 没有身份验证层。
            if "credential_ref" not in options and adapter != "gpt_sovits":
                raise ConfigurationError(f"{kind}.{name}.credential_ref is required")
            if adapter in {"qwen_batch", "chat_completions", "cosyvoice_bidirectional", "doubao_streaming"}:
                _text(options.get("model"), f"{kind}.{name}.model")
            if adapter == "qwen_batch" and not options.get("endpoint"):
                _text(options.get("workspace_ref"), f"{kind}.{name}.workspace_ref")
            if adapter == "chat_completions" and not options.get("endpoint"):
                _text(options.get("base_url"), f"{kind}.{name}.base_url")
            if adapter == "gpt_sovits":
                _text(options.get("refer_wav_path"), f"{kind}.{name}.refer_wav_path")
                _text(options.get("prompt_text"), f"{kind}.{name}.prompt_text")
                _text(options.get("prompt_language", "zh"), f"{kind}.{name}.prompt_language")
                _text(options.get("text_language", "zh"), f"{kind}.{name}.text_language")
                speed = options.get("speed", 1.0)
                if type(speed) not in (int, float) or not 0.1 <= speed <= 4:
                    raise ConfigurationError(f"{kind}.{name}.speed must be between 0.1 and 4")
                for key in ("first_segment_chars", "segment_chars"):
                    if key in options and (type(options[key]) is not int or not 8 <= options[key] <= 200):
                        raise ConfigurationError(f"{kind}.{name}.{key} must be an integer between 8 and 200")
                max_audio_bytes = options.get("max_audio_bytes", 16 * 1024 * 1024)
                if type(max_audio_bytes) is not int or not 1 <= max_audio_bytes <= 128 * 1024 * 1024:
                    raise ConfigurationError(f"{kind}.{name}.max_audio_bytes must be between 1 and 134217728")
            models[kind][name] = ModelConfig(name, kind, adapter, freeze(options))
    profiles = _object(data.get("profiles"), "profiles")
    if not profiles:
        raise ConfigurationError("At least one profile is required")
    for name, profile in profiles.items():
        _identifier(name, "profile name")
        _object(profile, f"profiles.{name}")
        if set(profile) - {"asr", "llm", "tts", "require_streaming_asr"}:
            raise ConfigurationError(f"Unknown setting in profile '{name}'")
        if type(profile.get("require_streaming_asr", False)) is not bool:
            raise ConfigurationError(f"profiles.{name}.require_streaming_asr must be a boolean")
        for kind in ("asr", "llm", "tts"):
            choice = profile.get(kind)
            if kind == "tts" and choice == "none":
                continue
            if not isinstance(choice, str) or choice not in models[kind]:
                raise ConfigurationError(f"Profile '{name}' references an unknown {kind} choice")
    active = _text(data.get("active_profile"), "active_profile")
    if active not in profiles:
        raise ConfigurationError("active_profile does not name a configured profile")
    config = VoiceConfig(1, active, _validate_transport(data.get("transport")), freeze(models), freeze(profiles),
                         _validate_news(data.get("news", {})))
    return apply_model_overrides(config, environment)


def apply_model_overrides(config, settings):
    """在同一快照上应用旧 model_ref 覆盖，避免为取密钥而反复重读磁盘。"""
    models = {}
    for kind, entries in config.models.items():
        models[kind] = {}
        for name, entry in entries.items():
            ref = entry.options.get("model_ref")
            override = settings.get(ref) if ref else None
            if isinstance(override, str) and override.strip():
                entry = ModelConfig(name, kind, entry.adapter, freeze({**entry.options, "model": override.strip()}))
            models[kind][name] = entry
    return VoiceConfig(config.schema_version, config.active_profile, config.transport, freeze(models), config.profiles, config.news)


def referenced_settings(config):
    return frozenset(
        ref for entries in config.models.values() for entry in entries.values()
        for key, ref in entry.options.items() if key.endswith("_ref")
    ) | frozenset({config.news["credential_ref"]} if config.news.get("enabled") else ())


def _validate_selected_options(entry):
    for key in ("model", "voice", "resource_id", "endpoint", "base_url"):
        value = entry.options.get(key)
        if isinstance(value, str) and ("<" in value or ">" in value):
            raise ConfigurationError(f"{entry.kind}.{entry.name}.{key} still contains an example placeholder")
    for key in ("endpoint", "base_url"):
        if key in entry.options:
            try:
                parsed = urlsplit(entry.options[key])
                port = parsed.port  # urlsplit 在访问 .port 时才检查数字和范围。
            except ValueError:
                raise ConfigurationError(f"{entry.kind}.{entry.name}.{key} has an invalid URL or port (端口必须是 1–65535 的整数)") from None
            if port is not None and not 1 <= port <= 65535:
                raise ConfigurationError(f"{entry.kind}.{entry.name}.{key} port must be between 1 and 65535")
            if entry.adapter == "gpt_sovits":
                valid_schemes = {"http", "https"}
            else:
                valid_schemes = {"https"} if entry.adapter in {"qwen_batch", "chat_completions"} else {"wss"}
            if parsed.scheme not in valid_schemes or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
                schemes = "/".join(sorted(valid_schemes)).upper()
                raise ConfigurationError(f"{entry.kind}.{entry.name}.{key} must be an {schemes} URL without credentials")
    # 数值范围已在加载时统一校验，选中模型时不再使用另一套不同上限。
    if entry.adapter in {"doubao_streaming", "volcengine_bidirectional"}:
        _text(entry.options.get("resource_id"), f"{entry.kind}.{entry.name}.resource_id")
    if entry.adapter == "doubao_streaming" and entry.options["resource_id"] not in {"volc.seedasr.sauc.duration", "volc.seedasr.sauc.concurrent"}:
        raise ConfigurationError(f"asr.{entry.name}.resource_id must select volc.seedasr.sauc.duration or volc.seedasr.sauc.concurrent")
    if entry.kind == "tts" and entry.adapter != "gpt_sovits":
        _text(entry.options.get("voice"), f"tts.{entry.name}.voice")


def select_profile(config, *, profile=None, asr=None, llm=None, tts=None, mode="http", require_available=True):
    if mode not in ("http", "realtime"):
        raise ConfigurationError("mode must be http or realtime")
    name = config.active_profile if profile is None else profile
    if name not in config.profiles:
        raise ConfigurationError(f"Unknown profile '{name}'")
    settings = config.profiles[name]
    selected = {}
    for kind, override in (("asr", asr), ("llm", llm), ("tts", tts)):
        choice = settings[kind] if override is None else override
        if kind == "tts" and choice == "none":
            selected[kind] = None
        else:
            if choice not in config.models[kind]:
                raise ConfigurationError(f"Unknown {kind} choice '{choice}'")
            selected[kind] = config.models[kind][choice]
    if settings.get("require_streaming_asr", False):
        capability = get_adapter("asr", selected["asr"].adapter).capabilities
        if not capability.streaming_input or not capability.partial_results:
            raise ConfigurationError(f"Profile '{name}' requires implemented streaming ASR with partial results")
    if mode == "http" and selected["tts"] is not None:
        raise ConfigurationError("HTTP /audio returns two text lines; select --tts none or use realtime mode")
    if mode == "realtime" and selected["tts"] is None:
        raise ConfigurationError("Realtime mode requires a TTS choice")
    if require_available:
        for entry in selected.values():
            if entry is not None:
                require_adapter(entry.kind, entry.adapter, mode)
                _validate_selected_options(entry)
    return Selection(name, mode, selected["asr"], selected["llm"], selected["tts"],
                     settings.get("require_streaming_asr", False), config.transport, config.news)


def news_credential_status(selection, secrets):
    """新闻为可选能力：缺少 Key 时给出明确提示，不阻止普通语音对话启动。"""
    news = getattr(selection, "news", {})
    if getattr(selection, "mode", None) != "realtime" or not news.get("enabled", False):
        return "disabled"
    ref = news.get("credential_ref", "SHOWAPI_APP_KEY")
    value = secrets.get(ref, "")
    placeholder = value.strip().lower().replace("-", "_").replace(" ", "_") if isinstance(value, str) else ""
    if (not placeholder or len(value) > 256 or any(char in value for char in "<>{}\r\n")
            or placeholder.startswith(("paste_your_", "your_", "请填写", "填写你的", "替换为"))
            or placeholder in {"your_appkey", "your_app_key", "your_key", "your_api_key",
                               "replace_me", "change_me", "changeme", "example", "demo"}):
        return "missing_credentials"
    return "ready"


def validate_selection_credentials(selection, secrets):
    """只检查选中的服务商；失败信息只返回配置名，不返回配置值。"""
    missing = set()
    for entry in (selection.asr, selection.llm, selection.tts):
        if entry is None:
            continue
        for key in ("credential_ref", "workspace_ref", "app_id_ref"):
            ref = entry.options.get(key)
            value = secrets.get(ref, "")
            if ref and (not value or value.startswith("paste-your-") or ("<" in value and ">" in value)):
                missing.add(ref)
    if missing:
        raise ConfigurationError("Missing selected-provider settings: " + ", ".join(sorted(missing)) + " (请填写真实凭证；示例占位文字不能使用)")


def list_options(config):
    """说明已支持的能力，不检查凭证或联系服务商。"""
    models = {}
    for kind, entries in config.models.items():
        models[kind] = []
        for entry in entries.values():
            info = get_adapter(kind, entry.adapter)
            public = {"id": entry.name, **info.to_dict()}
            for key in ("model", "voice", "resource_id"):
                if key in entry.options:
                    public[key] = entry.options[key]
            models[kind].append(public)
    profiles = {}
    for name, settings in config.profiles.items():
        available = []
        issues = {}
        for mode in ("http", "realtime"):
            try:
                select_profile(config, profile=name, mode=mode)
                available.append(mode)
            except ValueError as exc:
                issues[mode] = str(exc)
        profiles[name] = {**dict(settings), "available_modes": available, "issues": issues}
    return {"schema_version": config.schema_version, "active_profile": config.active_profile,
            "models": models, "profiles": profiles,
            "news": {"enabled": config.news.get("enabled", False), "provider": "showapi",
                     "channel_name": config.news.get("channel_name", "国内最新"),
                     "max_results": config.news.get("max_results", 3)}}
