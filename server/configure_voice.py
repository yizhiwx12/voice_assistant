"""中文模型配置向导。运行：python server/configure_voice.py。

交互只负责编辑，校验复用 core.config，文件保存复用 core.config_editor。
不会启动服务、占用串口或调用云 API。--show 可只读查看设备组合。
"""

import sys

# 必须先于 core 导入执行，避免旧解释器在解析配置类型或异步能力时出错。
if sys.version_info[:2] < (3, 11):
    print("运行配置向导需要 Python 3.11 或更高版本（推荐 Python 3.12）；"
          f"当前为 Python {sys.version_info[0]}.{sys.version_info[1]}。"
          "请更换解释器并重新创建 .venv-voice。", file=sys.stderr)
    raise SystemExit(2)

import argparse
import copy
import getpass
import os
from pathlib import Path

from core.config import (
    ConfigurationError, DEFAULT_CONFIG_PATH, DEFAULT_SECRETS_PATH,
    apply_model_overrides, load_secrets, load_voice_config, referenced_settings,
    select_profile, validate_selection_credentials,
    DEFAULT_NEWS_OPTIONS, news_credential_status,
)
from core.config_editor import load_editable_config, read_document, save_document, validate_document, validate_structure
from core.provider_registry import get_adapter


KINDS = {"asr": "ASR 语音识别（把你说的话变成文字）",
         "llm": "LLM 聊天模型（理解问题并生成回答）",
         "tts": "TTS 语音合成（把回答读出来）"}
LABELS = {"qwen_batch": "通义千问，整句识别，不提供实时中间文字",
          "doubao_streaming": "豆包 2.0，流式识别，适合当前设备",
          "chat_completions": "兼容 Chat Completions 的聊天接口",
          "volcengine_bidirectional": "火山在线播报，需要对应音色权限",
          "cosyvoice_bidirectional": "阿里 CosyVoice，需要匹配模型和音色",
          "gpt_sovits": "本机曼波/GPT-SoVITS，需要另开 9880 引擎"}
CREDENTIAL_LABELS = {
    "DASHSCOPE_API_KEY": "阿里百炼 API Key（千问识别/聊天/CosyVoice）",
    "DASHSCOPE_WORKSPACE_ID": "阿里百炼 Workspace ID（业务空间 ID）",
    "DEEPSEEK_API_KEY": "DeepSeek 聊天服务 API Key",
    "VOLCENGINE_ASR_API_KEY": "豆包语音识别 API Key（火山平台）",
    "VOLCENGINE_TTS_API_KEY": "火山语音合成 API Key",
    "VOLCENGINE_ASR_APP_ID": "豆包识别 App ID",
    "VOLCENGINE_TTS_APP_ID": "火山合成 App ID",
    "ANYSEARCH_API_KEY": "AnySearch 联网搜索 Key（可不填，使用匿名额度）",
    "SHOWAPI_APP_KEY": "易源 ShowAPI 新闻 AppKey（需开通新闻 API 109）",
}
# 这里只描述编辑提示；允许的值及兼容性仍由运行时配置模块判断。
FIELDS = {
    "chat_completions": [("model", "模型 ID（按服务商控制台填写）", str),
                         ("base_url", "接口基础地址（HTTPS；完整接口地址使用 endpoint）", str),
                         ("endpoint", "完整聊天接口地址（优先于 base_url）", str),
                         ("max_tokens", "回答长度预算（1–65536 token，不等于中文字数）", int),
                         ("temperature", "随机程度（0–2；越低越稳定）", float),
                         ("enable_web_search", "允许按问题触发联网查询（true/false）", bool)],
    "doubao_streaming": [("resource_id", "识别资源（小时版 duration / 并发版 concurrent 的完整 ID）", str),
                         ("endpoint", "豆包流式接口地址（wss://）", str)],
    "qwen_batch": [("model", "通义识别模型 ID", str),
                   ("endpoint", "自定义完整识别地址（HTTPS；通常无需设置）", str)],
    "volcengine_bidirectional": [("voice", "音色 ID（不是音色的中文名称）", str),
                                ("resource_id", "合成资源 ID，例如 seed-tts-2.0", str),
                                ("endpoint", "火山双向合成地址（wss://）", str)],
    "cosyvoice_bidirectional": [("model", "合成模型 ID", str), ("voice", "音色 ID", str),
                               ("endpoint", "CosyVoice 接口地址（wss://）", str)],
    "gpt_sovits": [("endpoint", "本地合成地址，例如 http://127.0.0.1:9880/", str),
                   ("refer_wav_path", "引擎电脑上的参考音频路径（可用 C:/...）", str),
                   ("prompt_text", "参考音频中说的完整文字", str),
                   ("prompt_language", "参考音频语言，例如 zh", str),
                   ("text_language", "回答语言，例如 zh", str),
                   ("speed", "语速（0.1–4；1 为正常）", float),
                   ("first_segment_chars", "首段字数（8–200；默认 40）", int),
                   ("segment_chars", "后续段字数（8–200；默认 80）", int)],
}


def choose(prompt, items):
    """编号选择；空输入返回，不把错误输入当成默认选项。"""
    print("\n" + prompt)
    for index, (_, label) in enumerate(items, 1):
        print(f"  {index}. {label}")
    while True:
        answer = input("输入编号，直接回车返回：").strip()
        if not answer:
            return None
        if answer.isascii() and answer.isdigit() and 1 <= int(answer) <= len(items):
            return items[int(answer) - 1][0]
        print("请输入列表中的编号。")


def show_selection(data, profile):
    print(f"\n设备组合 profiles.{profile}：")
    settings = data["profiles"][profile]
    for kind, title in KINDS.items():
        name = settings[kind]
        options = data[kind].get(name, {})
        detail = options.get("model", options.get("voice", options.get("adapter", "不播报")))
        print(f"  {title}：{name} / {detail}")
    print("设备会显式请求此组合；修改后须重启 8000 服务。")
    news = {**DEFAULT_NEWS_OPTIONS, **data.get("news", {})}
    print(f"  新闻 ShowAPI：{'已开启' if news['enabled'] else '未开启'}，"
          f"默认频道={news['channel_name'] or '全部'}，最多 {news['max_results']} 条")


def edit_model(data):
    kind = choose("要编辑哪一层的参数？", list(KINDS.items()))
    if kind is None:
        return False
    name = choose("选择配置条目（可先填写尚未使用的模型）：", [(name, name) for name in data[kind]])
    if name is None:
        return False
    options = data[kind][name]
    fields = FIELDS[options["adapter"]] + [("timeout_s", "请求超时秒数（0.1–300）", float)]
    field = choose("选择参数；接口地址和模型 ID 请从对应服务商控制台核对：",
                   [(spec, f"{spec[0]}：{spec[1]}，当前={options.get(spec[0], '使用默认值')}") for spec in fields])
    if field is None:
        return False
    key, _, convert = field
    raw = input("新值（回车保留；输入 /default 删除该字段并恢复适配器默认）：").strip()
    if not raw:
        return False
    if raw == "/default":
        options.pop(key, None)
    else:
        if convert is bool:
            if raw.lower() not in ("true", "false"):
                raise ConfigurationError("开关只接受 true 或 false")
            value = raw.lower() == "true"
        else:
            value = convert(raw)
        options[key] = value
    if key == "model" and "model_ref" in options:
        options.pop("model_ref")
        print("此条目已改为直接使用 model，旧凭证文件中的模型名不再覆盖它。")
    return True


def check_saved(config_path, secrets_path, profile, mode):
    config = load_voice_config(config_path, environ={})
    credentials = load_secrets(secrets_path, references=referenced_settings(config))
    config = apply_model_overrides(config, credentials)
    selected = select_profile(config, profile=profile, mode=mode)
    validate_selection_credentials(selected, credentials)
    status = news_credential_status(selected, credentials)
    if status == "missing_credentials":
        print("新闻功能尚未就绪：请用菜单 5 填写 " + selected.news["credential_ref"] + "。普通对话不受影响。")
    elif status == "ready":
        print("ShowAPI 新闻凭证已填写；账号权限及返回内容尚未联网验证。")
    for kind, entry in (("ASR", selected.asr), ("LLM", selected.llm), ("TTS", selected.tts)):
        print(f"{kind} 实际生效：{entry.name} / {entry.options.get('model', entry.options.get('voice', entry.adapter))}" if entry else f"{kind}：不播报")
    if mode == "realtime" and "opus" in (selected.transport["uplink"]["codec"], selected.transport["downlink"]["codec"]):
        from audio.opus_codec import native_version
        print("Opus：" + native_version())
    print("离线配置检查通过。没有调用云 API；模型权限、9880 引擎和设备仍需实际验证。")
    return selected


def edit_credentials(data, profile, path):
    refs = set()
    for kind in KINDS:
        options = data[kind].get(data["profiles"][profile][kind], {})
        refs.update(value for key, value in options.items() if key in {"credential_ref", "workspace_ref", "app_id_ref"})
    if data["llm"][data["profiles"][profile]["llm"]].get("enable_web_search"):
        refs.add("ANYSEARCH_API_KEY")
    news = {**DEFAULT_NEWS_OPTIONS, **data.get("news", {})}
    if news["enabled"]:
        refs.add(news["credential_ref"])
    name = choose("填写当前组合需要的凭证（不显示已有值；AnySearch Key 可选）：",
                  [(ref, f"{CREDENTIAL_LABELS.get(ref, ref)} / {ref}") for ref in sorted(refs)])
    if name is None:
        return
    # 无安全的交互终端时不退化为明文回显，用户可直接编辑本地凭证文件。
    if not sys.stdin.isatty():
        raise ConfigurationError("请在交互式 PowerShell 中填写密钥，或直接编辑本地 server_secrets.json")
    value = getpass.getpass("粘贴新值（输入不回显；回车保留已有值）：").strip()
    if not value:
        return
    secrets, original = read_document(path)
    secrets[name] = value
    _, backup = save_document(path, secrets, original=original)
    print("凭证已保存。" + (" 原文件已自动备份。" if backup else ""))
    if name in os.environ:
        print(f"提示：当前窗口设置了 {name} 环境变量，它会优先于文件；需要同步修改该环境变量。")


def edit_news(data):
    """新闻独立于聊天模型搜索；只编辑非敏感参数，AppKey 统一走密钥菜单。"""
    news = {**DEFAULT_NEWS_OPTIONS, **data.get("news", {})}
    field = choose("ShowAPI 新闻设置（Key 用主菜单 5 填写；需账号拥有 API 109 权限）：", [
        ("enabled", f"开启/关闭新闻功能，当前={news['enabled']}"),
        ("channel_name", f"默认频道名称，当前={news['channel_name'] or '全部'}"),
        ("max_results", f"每次最多条数（1–5），当前={news['max_results']}"),
        ("timeout_s", f"请求超时秒数（1–20），当前={news['timeout_s']}"),
    ])
    if field is None:
        return False
    value = input("输入新值（回车保留；开关填 true/false，频道填 /all 表示全部）：").strip()
    if not value:
        return False
    if field == "enabled":
        if value.lower() not in {"true", "false"}:
            raise ConfigurationError("新闻开关只接受 true 或 false")
        news[field] = value.lower() == "true"
    elif field == "channel_name":
        news[field] = "" if value == "/all" else value
    else:
        news[field] = int(value) if field == "max_results" else float(value)
    data["news"] = news
    return True


def main(argv=None):
    # Windows 重定向/工具捕获时 Python 可能选用系统代码页，统一输出 UTF-8 中文。
    for stream in (sys.stdout, sys.stderr):
        if sys.platform == "win32" and hasattr(stream, "reconfigure") and not stream.isatty():
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="语音助手中文配置向导（不启动服务、不调用模型）")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH, help="业务配置文件")
    parser.add_argument("--secrets", type=Path, default=DEFAULT_SECRETS_PATH, help="本地凭证文件")
    parser.add_argument("--profile", default="realtime_default", help="设备请求的组合名称（默认 realtime_default）")
    parser.add_argument("--mode", choices=("http", "realtime"), default="realtime")
    parser.add_argument("--show", action="store_true", help="只查看组合，不写文件、不读取密钥")
    parser.add_argument("--check", action="store_true", help="只执行离线配置/凭证/Opus 检查")
    args = parser.parse_args(argv)
    try:
        data, original = load_editable_config(args.config)
        validate_structure(data)
        if args.profile not in data.get("profiles", {}):
            raise ConfigurationError("组合不存在，请核对 --profile 和 voice_config.json")
        if args.check:
            if not args.config.exists():
                raise ConfigurationError("本地配置尚未保存，请先运行交互向导选择模型")
            check_saved(args.config, args.secrets, args.profile, args.mode)
            return 0
        if args.show:
            show_selection(data, args.profile)
            return 0
        print("语音助手配置向导：1 识别 → 2 聊天 → 3 播报")
        print(f"配置文件：{args.config}\n凭证文件：{args.secrets}")
        if original is None:
            print("本地配置不存在，正在使用模板；首次保存才创建本地文件，不会覆盖现有密钥。")
        show_selection(data, args.profile)
        while True:
            action = choose("选择操作", [("asr", "选择语音识别 ASR"), ("llm", "选择聊天模型 LLM"),
                ("tts", "选择播报声音 TTS"), ("edit", "编辑模型 ID / 音色 / 地址 / 语速等参数"),
                ("keys", "填写当前组合的密钥"), ("check", "检查已保存配置"), ("show", "查看当前组合"),
                ("news", "新闻 API 设置（易源 ShowAPI）")])
            if action is None:
                break
            try:
                if action == "show":
                    show_selection(data, args.profile)
                    continue
                if action == "keys":
                    edit_credentials(data, args.profile, args.secrets)
                    continue
                if action == "check":
                    if original is None:
                        raise ConfigurationError("请先选择模型并保存配置")
                    check_saved(args.config, args.secrets, args.profile, args.mode)
                    continue
                candidate = copy.deepcopy(data)
                if action == "news":
                    if not edit_news(candidate):
                        continue
                elif action == "edit":
                    if not edit_model(candidate):
                        continue
                else:
                    entries = [(name, f"{name} — {LABELS.get(options['adapter'], options['adapter'])}")
                               for name, options in candidate[action].items()
                               if args.mode in get_adapter(action, options["adapter"]).modes]
                    if action == "tts" and args.mode == "http":
                        entries = [("none", "不播报（HTTP 只返回文字）")]
                    name = choose(KINDS[action], entries)
                    if name is None:
                        continue
                    candidate["profiles"][args.profile][action] = name
                    if action == "asr":
                        caps = get_adapter("asr", candidate["asr"][name]["adapter"]).capabilities
                        candidate["profiles"][args.profile]["require_streaming_asr"] = bool(caps.streaming_input and caps.partial_results)
                        if not caps.partial_results:
                            print("该识别器在一轮结束后才出文字，已同步关闭本组合的强制流式识别要求。")
                validate_document(candidate, profile=args.profile, mode=args.mode)
                original, backup = save_document(args.config, candidate, original=original)
                data = candidate
                print("配置已保存；重启 8000 服务后生效。" + (" 原文件已自动备份。" if backup else ""))
                print("建议接着填写密钥并执行“检查已保存配置”。")
            except (ConfigurationError, ValueError, OSError, ImportError) as exc:
                print(f"未完成：{exc}")
        print("已退出。启动方式见 server/README.md 的“新手配置”。")
        return 0
    except (ConfigurationError, ValueError, OSError, ImportError) as exc:
        print(f"配置错误：{exc}", file=sys.stderr)
        return 2
    except (EOFError, KeyboardInterrupt):
        print("\n已退出；只保留此前明确提示已保存的修改。")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
