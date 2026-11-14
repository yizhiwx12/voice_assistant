"""万维易源新闻接口：频道、新闻列表、发布时间检索和热搜。

请求固定发往官方 HTTPS 域名，不跟随重定向；密钥只通过 appKey 查询参数
发送。只提取成功响应中的有限来源字段，不记录远端错误正文或带密钥的 URL。
官方契约：https://www.showapi.com/openapi/market/109.json
"""

import asyncio
from collections.abc import Mapping
from datetime import datetime
from html.parser import HTMLParser
import json
import math
import re
import unicodedata
from urllib.parse import urlsplit, urlunsplit

import aiohttp


BASE_URL = "https://route.showapi.com"
MAX_RESPONSE_BYTES = 512 * 1024
MAX_RESULTS = 5
MAX_CHANNELS = 100
_REASONS = frozenset({
    "missing_credentials", "invalid_options", "invalid_query", "http_status",
    "response_too_large", "invalid_json", "invalid_schema", "upstream_error",
    "empty_results", "timeout", "connection_failed",
})


def _code(value):
    """只接受整数或整数形式的字符串；布尔值不是状态码。"""
    if type(value) is str and re.fullmatch(r"-?\d{1,10}", value, flags=re.ASCII):
        value = int(value)
    return value if type(value) is int and -(2 ** 31) <= value < 2 ** 31 else None


class ShowApiNewsError(RuntimeError):
    """仅保留固定原因码和数值，避免异常信息携带请求或上游正文。"""

    def __init__(self, reason, http_status=0, upstream_code=0):
        self.reason = reason if reason in _REASONS else "invalid_schema"
        self.http_status = (http_status if type(http_status) is int
                            and 100 <= http_status <= 599 else 0)
        self.upstream_code = _code(upstream_code)
        if self.upstream_code is None:
            self.upstream_code = 0
        super().__init__(f"ShowAPI {self.reason} http_status={self.http_status} "
                         f"upstream_code={self.upstream_code}")


class _PlainText(HTMLParser):
    """只保留可见文本；脚本、样式和标签不进入模型上下文。"""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.hidden = 0
        self.parts = []

    def handle_starttag(self, tag, attrs):
        if tag in {"script", "style"}:
            self.hidden += 1
        elif not self.hidden:
            self.parts.append(" ")

    def handle_endtag(self, tag):
        if tag in {"script", "style"}:
            self.hidden = max(0, self.hidden - 1)
        elif not self.hidden:
            self.parts.append(" ")

    def handle_data(self, data):
        if not self.hidden:
            self.parts.append(data)


def _text(value, limit):
    if not isinstance(value, str):
        raise ShowApiNewsError("invalid_schema")
    parser = _PlainText()
    parser.feed(value)
    parser.close()
    plain = "".join(parser.parts)
    # 格式控制、不可见字符及换行均转为空格，避免控制终端或伪造字段边界。
    plain = "".join(" " if unicodedata.category(char).startswith("C") else char
                    for char in plain)
    return " ".join(plain.split())[:limit]


def _url(value):
    if (not isinstance(value, str) or not value or len(value) > 800
            or any(char.isspace() or unicodedata.category(char).startswith("C")
                   for char in value)
            or "\\" in value or re.search(r"%(?:0[0-9a-f]|1[0-9a-f]|7f)", value, re.I)):
        raise ShowApiNewsError("invalid_schema")
    try:
        parsed = urlsplit(value)
        valid = (parsed.scheme in {"http", "https"} and parsed.hostname
                 and parsed.username is None and parsed.password is None
                 and not any(char in parsed.hostname for char in "<>\"{}|^`"))
        parsed.port
        if not valid:
            raise ValueError
    except ValueError:
        raise ShowApiNewsError("invalid_schema") from None
    # 来源链接的查询串和片段可能带令牌，不交给 LLM，也不影响新闻正文的获取。
    return urlunsplit((parsed.scheme, parsed.netloc, parsed.path, "", ""))


def _body(value):
    if not isinstance(value, dict):
        raise ShowApiNewsError("invalid_schema")
    outer = _code(value.get("showapi_res_code"))
    if outer is None:
        raise ShowApiNewsError("invalid_schema")
    if outer != 0:
        raise ShowApiNewsError("upstream_error", upstream_code=outer)
    body = value.get("showapi_res_body")
    if not isinstance(body, dict):
        raise ShowApiNewsError("invalid_schema")
    business = _code(body.get("ret_code"))
    if business is None:
        raise ShowApiNewsError("invalid_schema")
    if business != 0:
        raise ShowApiNewsError("upstream_error", upstream_code=business)
    return body


def _published(value):
    """只识别官方明确的发布时间格式；未知时间保持原样，不猜测日期。"""
    try:
        return datetime.strptime(value, "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return None


def _source(item, *, hot=False, pinned=False):
    if not isinstance(item, dict):
        raise ShowApiNewsError("invalid_schema")
    title_key = "top" if pinned else ("hot_research" if hot else "title")
    title = _text(item.get(title_key), 300)
    result = {"title": title, "url": _url(item.get("link")),
              "snippet": _text(item.get("desc", ""), 1200),
              "source": _text(item.get("source", ""), 100),
              "published_at": "" if hot else _text(item.get("pubDate", ""), 40),
              "channel": _text(item.get("channelName", ""), 80)}
    if not title:
        raise ShowApiNewsError("invalid_schema")
    return result


class ShowApiNewsProvider:
    def __init__(self, options, credentials):
        self.options = options
        self.credentials = credentials

    def _settings(self):
        if not isinstance(self.options, Mapping) or not isinstance(self.credentials, Mapping):
            raise ShowApiNewsError("invalid_options")
        reference = self.options.get("credential_ref", "SHOWAPI_APP_KEY")
        if not isinstance(reference, str) or not reference:
            raise ShowApiNewsError("invalid_options")
        key = self.credentials.get(reference)
        if not isinstance(key, str):
            raise ShowApiNewsError("missing_credentials")
        key = key.strip()
        placeholder = key.lower().replace("-", "_").replace(" ", "_")
        if (not key or len(key) > 256 or any(char in key for char in "<>{}\r\n")
                or placeholder in {"your_appkey", "your_app_key", "your_key", "your_api_key",
                                   "replace_me", "change_me", "changeme", "example", "demo"}
                or placeholder.startswith(("paste_your_", "your_", "请填写", "填写你的", "替换为"))):
            raise ShowApiNewsError("missing_credentials")
        count = self.options.get("max_results", 3)
        timeout = self.options.get("timeout_s", 10)
        if (type(count) is not int or not 1 <= count <= MAX_RESULTS
                or type(timeout) not in (int, float) or not math.isfinite(timeout)
                or not 1 <= timeout <= 20):
            raise ShowApiNewsError("invalid_options")
        return key, count, timeout

    async def _request(self, point, data, session):
        key, _, timeout = self._settings()
        try:
            async with asyncio.timeout(timeout):
                async with session.post(
                        BASE_URL + point, params={"appKey": key}, data=data,
                        headers={"Content-Type": "application/x-www-form-urlencoded"},
                        allow_redirects=False,
                        timeout=aiohttp.ClientTimeout(total=timeout)) as response:
                    if response.status != 200:
                        # HTTP 错误和重定向正文可能包含密钥，不读取也不跟随。
                        raise ShowApiNewsError("http_status", response.status)
                    raw = bytearray()
                    async for chunk in response.content.iter_chunked(16 * 1024):
                        if len(raw) + len(chunk) > MAX_RESPONSE_BYTES:
                            raise ShowApiNewsError("response_too_large")
                        raw.extend(chunk)
                    try:
                        value = json.loads(raw.decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
                        raise ShowApiNewsError("invalid_json") from None
                    return _body(value)
        except (asyncio.TimeoutError, TimeoutError):
            raise ShowApiNewsError("timeout") from None
        except (aiohttp.ClientError, OSError, ValueError):
            raise ShowApiNewsError("connection_failed") from None

    async def channels(self, session):
        """查询最多一百个频道；频道编号不能当作新闻内容使用。"""
        body = await self._request("/109-34", {}, session)
        rows = body.get("channelList")
        if not isinstance(rows, list):
            raise ShowApiNewsError("invalid_schema")
        results = []
        for item in rows[:MAX_CHANNELS]:
            if not isinstance(item, dict):
                raise ShowApiNewsError("invalid_schema")
            identifier, name = _text(item.get("channelId"), 100), _text(item.get("name"), 80)
            if not identifier or not name:
                raise ShowApiNewsError("invalid_schema")
            results.append({"id": identifier, "name": name})
        if not results:
            raise ShowApiNewsError("empty_results")
        return results

    async def search(self, keyword, session, *, channel_name=None, begin_time=None, end_time=None):
        """查询频道/关键词新闻；显式空频道表示跨频道，带时间则使用 109-36。"""
        _, count, _ = self._settings()
        if not isinstance(keyword, str) or len(keyword) > 300:
            raise ShowApiNewsError("invalid_query")
        channel = self.options.get("channel_name", "国内最新") if channel_name is None else channel_name
        if not isinstance(channel, str) or len(channel) > 80:
            raise ShowApiNewsError("invalid_query")
        data = {"page": "1", "maxResult": "20"}
        if keyword.strip():
            data["title"] = keyword.strip()
        if channel.strip():
            data["channelName"] = channel.strip()
        point = "/109-35"
        if begin_time is not None or end_time is not None:
            point = "/109-36"
            dates = []
            for name, value in (("beginTime", begin_time), ("endTime", end_time)):
                if value is not None:
                    if not isinstance(value, str) or not re.fullmatch(
                            r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", value, flags=re.ASCII):
                        raise ShowApiNewsError("invalid_query")
                    parsed = _published(value)
                    if parsed is None:
                        raise ShowApiNewsError("invalid_query")
                    data[name] = value
                    dates.append(parsed)
            if len(dates) == 2 and dates[0] > dates[1]:
                raise ShowApiNewsError("invalid_query")
        body = await self._request(point, data, session)
        page = body.get("pagebean")
        if not isinstance(page, dict) or not isinstance(page.get("contentlist"), list):
            raise ShowApiNewsError("invalid_schema")
        # 只处理官方单页上限内的数据；明确时间从新到旧，未知时间仍不补造。
        results = [_source(item) for item in page["contentlist"][:100]]
        results.sort(key=lambda item: _published(item["published_at"]) or datetime.min, reverse=True)
        if not results:
            raise ShowApiNewsError("empty_results")
        return results[:count]

    async def hot(self, session):
        """读取置顶与热搜标题；接口没有发布时间，返回的日期字段保持为空。"""
        _, count, _ = self._settings()
        body = await self._request("/109-37", {}, session)
        hot, pinned = body.get("hot"), body.get("top", [])
        if not isinstance(hot, list) or not isinstance(pinned, list):
            raise ShowApiNewsError("invalid_schema")
        results = [_source(item, hot=True) for item in hot[:count]]
        if len(results) < count:
            results.extend(_source(item, hot=True, pinned=True)
                           for item in pinned[:count - len(results)])
        if not results:
            raise ShowApiNewsError("empty_results")
        return results
