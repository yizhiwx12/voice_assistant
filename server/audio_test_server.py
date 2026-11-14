"""原始 HTTP 入口，作为兼容包装保留。

云端协议实现在 providers/。本模块保留供现有脚本和测试使用的公共函数，
以及可被替换的调用位置。
"""

import os
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.request import urlopen

from audio.formats import SAMPLE_RATE_HZ, pcm16_to_wav
from core.config import load_local_config
from core.text_utils import clean_single_line, truncate_utf8
from providers.asr.qwen_batch import QwenBatchProvider
from providers.llm.chat_completions import ChatCompletionsProvider, LEGACY_SYSTEM_PROMPT
from transport.http_routes import make_handler

MAX_AUDIO_BYTES = 1_000_000
MAX_TRANSCRIPT_BYTES = 96
MAX_REPLY_BYTES = 120
SERVER_DIR = Path(__file__).resolve().parent


def recognize_qwen(wav_data):
    provider = QwenBatchProvider(
        {
            "model": os.environ.get("DASHSCOPE_ASR_MODEL", "qwen-audio-3.1-asr-flash"),
            "credential_ref": "DASHSCOPE_API_KEY",
            "workspace_ref": "DASHSCOPE_WORKSPACE_ID",
        },
        os.environ,
        opener=urlopen,
    )
    return provider.recognize_wav(wav_data)


def ask_deepseek(transcript):
    provider = ChatCompletionsProvider(
        {
            "model": os.environ.get("DEEPSEEK_MODEL", "deepseek-flash"),
            "base_url": "https://api.deepseek.com",
            "credential_ref": "DEEPSEEK_API_KEY",
            "max_tokens": 128,
        },
        os.environ,
        opener=urlopen,
    )
    answer = provider.generate([
        {"role": "system", "content": LEGACY_SYSTEM_PROMPT},
        {"role": "user", "content": transcript},
    ])
    return truncate_utf8(clean_single_line(answer), MAX_REPLY_BYTES)


Handler = make_handler(
    lambda wav: recognize_qwen(wav),
    lambda transcript: ask_deepseek(transcript),
    data_directory=SERVER_DIR / "data",
)


def main():
    try:
        load_local_config()
    except RuntimeError as exc:
        raise SystemExit(str(exc)) from exc

    api_key = os.environ.get("DASHSCOPE_API_KEY")
    workspace_id = os.environ.get("DASHSCOPE_WORKSPACE_ID")
    if not api_key:
        print("Warning: DASHSCOPE_API_KEY is not set; ASR requests will fail")
    if not workspace_id:
        print("Warning: DASHSCOPE_WORKSPACE_ID is not set; ASR requests will fail")
    if api_key and workspace_id:
        model = os.environ.get("DASHSCOPE_ASR_MODEL", "qwen-audio-3.1-asr-flash")
        print(f"HTTP batch ASR ready with model '{model}'")

    if not os.environ.get("DEEPSEEK_API_KEY"):
        print("Warning: DEEPSEEK_API_KEY is not set; /audio replies will fail")
    else:
        model = os.environ.get("DEEPSEEK_MODEL", "deepseek-flash")
        print(f"DeepSeek ready with model '{model}'")

    server = ThreadingHTTPServer(("0.0.0.0", 8000), Handler)
    print("Voice assistant server ready at http://0.0.0.0:8000", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
