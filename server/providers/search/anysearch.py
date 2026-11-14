"""为语音流水线提供大小受限、可取消的 AnySearch REST 检索。

只向调用者返回成功响应中的来源资料。错误正文可能包含配额服务新生成的
凭证，因此不读取错误正文，也不将其放入日志或 LLM 上下文。"""

import asyncio
import json
from urllib.parse import urlsplit

import aiohttp


SEARCH_ENDPOINT = "https://api.anysearch.com/v1/search"
REQUEST_TIMEOUT_SECONDS = 20
MAX_RESPONSE_BYTES = 512 * 1024
MAX_RESULTS = 5
_REASONS = frozenset({
    "invalid_query", "http_status", "response_too_large", "invalid_json",
    "invalid_schema", "upstream_error", "empty_results", "timeout",
    "connection_failed",
})


class AnySearchError(RuntimeError):
    """安全的诊断字段；不保存厂商响应正文或请求文字。"""

    def __init__(self, reason, http_status=0):
        self.reason = reason if reason in _REASONS else "invalid_schema"
        self.http_status = (http_status if type(http_status) is int
                            and 100 <= http_status <= 599 else 0)
        super().__init__(f"AnySearch {self.reason} http_status={self.http_status}")


def _results(body):
    if not isinstance(body, dict) or type(body.get("code")) is not int:
        raise AnySearchError("invalid_schema")
    if body["code"] != 0:
        raise AnySearchError("upstream_error")
    data = body.get("data")
    if not isinstance(data, dict) or not isinstance(data.get("results"), list):
        raise AnySearchError("invalid_schema")
    results = []
    for item in data["results"][:MAX_RESULTS]:
        if not isinstance(item, dict):
            raise AnySearchError("invalid_schema")
        title, url = item.get("title"), item.get("url")
        snippet = item.get("snippet", item.get("content", ""))
        if not all(isinstance(value, str) for value in (title, url, snippet)):
            raise AnySearchError("invalid_schema")
        url = url.strip()
        try:
            parsed = urlsplit(url)
            valid_url = (parsed.scheme in {"http", "https"} and parsed.hostname
                         and parsed.username is None and parsed.password is None
                         and len(url) <= 800 and not any(ord(char) < 33 for char in url))
            # 读取端口属性时，也会拒绝格式错误或超出范围的端口。
            parsed.port
        except ValueError:
            valid_url = False
        if not valid_url:
            raise AnySearchError("invalid_schema")
        if not title.strip() and not snippet.strip():
            continue
        results.append({"title": title[:300], "url": url, "snippet": snippet[:1200]})
    if not results:
        raise AnySearchError("empty_results")
    return results


class AnySearchProvider:
    def __init__(self, credentials):
        self.credentials = credentials

    async def search(self, query, session):
        if not isinstance(query, str) or not query.strip():
            raise AnySearchError("invalid_query")
        headers = {"Content-Type": "application/json"}
        api_key = self.credentials.get("ANYSEARCH_API_KEY")
        if isinstance(api_key, str) and api_key.strip():
            headers["Authorization"] = "Bearer " + api_key.strip()
        payload = {"query": query.strip(), "max_results": MAX_RESULTS,
                   "language": "zh-CN", "zone": "cn"}
        try:
            async with asyncio.timeout(REQUEST_TIMEOUT_SECONDS):
                async with session.post(
                        SEARCH_ENDPOINT, json=payload, headers=headers,
                        timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT_SECONDS)) as response:
                    if response.status != 200:
                        raise AnySearchError("http_status", response.status)
                    raw = bytearray()
                    async for chunk in response.content.iter_chunked(16 * 1024):
                        if len(raw) + len(chunk) > MAX_RESPONSE_BYTES:
                            raise AnySearchError("response_too_large")
                        raw.extend(chunk)
                    try:
                        body = json.loads(raw.decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
                        raise AnySearchError("invalid_json") from None
                    return _results(body)
        except (asyncio.TimeoutError, TimeoutError):
            raise AnySearchError("timeout") from None
        except (aiohttp.ClientError, OSError, ValueError):
            raise AnySearchError("connection_failed") from None
