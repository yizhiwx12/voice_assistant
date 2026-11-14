"""兼容 Chat Completions 的 LLM 适配器，支持普通请求与 SSE 流式回答。

模型、地址及凭证引用来自所选配置项；仅把回答 content 交给后续 TTS，
不把 reasoning_content、工具调用或上游错误正文读出来。
"""

import asyncio
import json
import re
from collections.abc import Mapping, Sequence
from urllib.request import Request, urlopen

from providers._http import request_json, require_credential


LEGACY_SYSTEM_PROMPT = (
    "你是桌面语音助手。请用简体中文简短回答，控制在约40个汉字以内。"
    "不要使用 Markdown、表情符号或换行。"
)


class ChatCompletionsProvider:
    capabilities = {
        "streaming_input": False,
        "streaming_output": True,
        "partial_results": False,
    }

    def __init__(self, options: Mapping, credentials: Mapping, opener=None):
        self.options = options
        self.credentials = credentials
        self.opener = opener if opener is not None else urlopen

    def _endpoint(self):
        endpoint = self.options.get("endpoint")
        if not endpoint:
            base_url = self.options.get("base_url", "https://api.deepseek.com")
            endpoint = f"{base_url.rstrip('/')}/chat/completions"
        return endpoint

    def _payload(self, messages, *, stream):
        payload = {
            "model": self.options.get("model", "deepseek-flash"),
            "messages": [dict(message) for message in messages],
            # 流式模型的预算可能同时用于推理和语音回答；
            # 原 HTTP 模式的默认预算单独保留。
            "max_tokens": self.options.get("max_tokens", 1024 if stream else 128),
            "stream": stream,
        }
        # 仅转发明确支持的内容生成参数。
        for key in ("temperature", "top_p"):
            if key in self.options:
                payload[key] = self.options[key]
        return payload

    def generate(self, messages: Sequence[Mapping[str, str]]) -> str:
        api_key = require_credential(
            self.credentials, self.options.get("credential_ref", "DEEPSEEK_API_KEY")
        )
        request = Request(
            self._endpoint(),
            data=json.dumps(self._payload(messages, stream=False), ensure_ascii=False).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        result = request_json(
            request,
            opener=self.opener,
            timeout=self.options.get("timeout_s", 45),
            label="Chat Completions",
        )
        return self._answer(result)

    @staticmethod
    def _answer(result):
        try:
            answer = result["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError):
            raise RuntimeError("Chat Completions returned an unexpected response") from None
        if not isinstance(answer, str):
            raise RuntimeError("Chat Completions returned non-text content")
        if not answer.strip():
            raise RuntimeError("Chat Completions returned an empty answer")
        # 界面或兼容接口的字节限制不能截短对话或后续 TTS 正文。
        return answer

    async def generate_async(self, messages, session):
        """保留原 HTTP 接口约定的可取消非流式请求。"""
        import aiohttp

        api_key = require_credential(
            self.credentials, self.options.get("credential_ref", "DEEPSEEK_API_KEY")
        )
        try:
            async with session.post(
                self._endpoint(), json=self._payload(messages, stream=False),
                headers={"Authorization": f"Bearer {api_key}"},
                timeout=aiohttp.ClientTimeout(total=self.options.get("timeout_s", 45)),
            ) as response:
                if response.status != 200:
                    raise RuntimeError(f"Chat Completions HTTP {response.status}")
                try:
                    result = json.loads(await response.read())
                except (UnicodeDecodeError, json.JSONDecodeError):
                    raise RuntimeError("Chat Completions returned invalid JSON") from None
        except (aiohttp.ClientError, asyncio.TimeoutError):
            raise RuntimeError("Chat Completions connection failed") from None
        return self._answer(result)

    async def stream(self, messages, session):
        """仅产出回答正文；共享 aiohttp 会话由调用者管理。

        HTTP 响应的生命周期跟随本迭代器，包括取消和 aclose()。到达 EOF 前
        必须收到 stop 完成原因或 SSE [DONE] 标记。推理与工具调用增量
        不会作为语音文字返回。"""
        import aiohttp

        api_key = require_credential(
            self.credentials, self.options.get("credential_ref", "DEEPSEEK_API_KEY")
        )
        timeout_s = self.options.get("timeout_s", 45)
        complete = False
        emitted_text = False
        try:
            async with session.post(
                self._endpoint(),
                json=self._payload(messages, stream=True),
                headers={"Authorization": f"Bearer {api_key}",
                         "Accept": "text/event-stream"},
                timeout=aiohttp.ClientTimeout(total=None, sock_connect=timeout_s,
                                              sock_read=timeout_s),
            ) as response:
                if response.status != 200:
                    raise RuntimeError(f"Chat Completions HTTP {response.status}")
                async for data in _sse_events(response.content):
                    if data.strip() == "[DONE]":
                        complete = True
                        break
                    try:
                        event = json.loads(data)
                    except json.JSONDecodeError:
                        raise RuntimeError("Chat Completions returned invalid SSE JSON") from None
                    if not isinstance(event, dict) or "error" in event:
                        raise RuntimeError("Chat Completions stream failed")
                    choices = event.get("choices")
                    if not isinstance(choices, list):
                        raise RuntimeError("Chat Completions returned invalid SSE choices")
                    if not choices:  # 仅含用量统计的事件没有回答增量。
                        continue
                    choice = next((item for item in choices
                                   if isinstance(item, dict) and item.get("index", 0) == 0), None)
                    if choice is None:
                        continue
                    delta = choice.get("delta", {})
                    if not isinstance(delta, dict):
                        raise RuntimeError("Chat Completions returned invalid SSE delta")
                    finish_reason = choice.get("finish_reason")
                    if finish_reason == "length":
                        raise RuntimeError("Chat Completions stream reached its token limit before completion")
                    if finish_reason is not None and finish_reason != "stop":
                        # 不能回显上游任意字段值，也不能将被过滤或工具调用响应
                        # 视为已经完整生成的语音回答。
                        raise RuntimeError("Chat Completions stream did not finish with a complete answer")
                    text = delta.get("content")
                    if text is not None and not isinstance(text, str):
                        raise RuntimeError("Chat Completions returned non-text SSE content")
                    if text:
                        if complete:
                            raise RuntimeError("Chat Completions sent text after completion")
                        emitted_text = True
                        yield text
                    if finish_reason == "stop":
                        complete = True
        except (aiohttp.ClientError, asyncio.TimeoutError, UnicodeDecodeError):
            raise RuntimeError("Chat Completions stream connection failed") from None
        if not complete:
            raise RuntimeError("Chat Completions stream ended before completion")
        if not emitted_text:
            raise RuntimeError("Chat Completions returned an empty answer")


async def _sse_events(content, max_event_bytes=1_048_576):
    """按网络块解析 SSE；完整行才解码，兼容拆开的 UTF-8 与 CRLF。

bytearray 避免长行逐字拼接带来的重复复制；行与事件都按实际 UTF-8
字节数限流。一个网络块可以包含多行，也可能只包含半个汉字。
"""
    line = bytearray()
    data = []
    event_bytes = 0
    previous_cr = False

    def accept_line(value):
        nonlocal event_bytes
        if not value:
            result = "\n".join(data) if data else None
            data.clear()
            event_bytes = 0
            return result
        if value.startswith("data:"):
            value = value[5:]
            if value.startswith(" "):
                value = value[1:]
            # 多个 data 行会用换行连接，连接符也属于事件占用。
            event_bytes += len(value.encode("utf-8")) + bool(data)
            if event_bytes > max_event_bytes:
                raise RuntimeError("Chat Completions SSE event is too large")
            data.append(value)
        return None

    def append_line(fragment):
        if len(line) + len(fragment) > max_event_bytes:
            raise RuntimeError("Chat Completions SSE line is too large")
        line.extend(fragment)

    async for chunk in content.iter_any():
        start = 0
        # 仅扫描 SSE 规定的 CR/LF，不把 Unicode 其他分隔符误作换行。
        for match in re.finditer(rb"[\r\n]", chunk):
            if match.start() > start:
                append_line(chunk[start:match.start()])
                previous_cr = False
            delimiter = match.group()
            if delimiter == b"\n" and previous_cr:
                previous_cr = False
            else:
                previous_cr = delimiter == b"\r"
                event = accept_line(line.decode("utf-8"))
                line.clear()
                if event is not None:
                    yield event
            start = match.end()
        if start < len(chunk):
            append_line(chunk[start:])
            previous_cr = False
    if line:
        accept_line(line.decode("utf-8"))
    if data:
        yield "\n".join(data)
