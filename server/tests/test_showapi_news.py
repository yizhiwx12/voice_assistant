"""ShowAPI 新闻适配器的离线契约、边界和取消检查。"""

import asyncio
import json
import unittest
from unittest.mock import patch

import aiohttp

from providers.news.showapi import (
    BASE_URL, MAX_CHANNELS, MAX_RESPONSE_BYTES, ShowApiNewsError,
    ShowApiNewsProvider,
)


def response(body=None):
    return {"showapi_res_code": 0, "showapi_res_error": "",
            "showapi_res_body": {"ret_code": 0, **(body or {})}}


def article(index=0, *, title=None, published="2026-11-14 10:00:00", **extra):
    return {"title": title or f"新闻{index}",
            "link": f"https://news.example.com/{index}?session=private-token#story",
            "pubDate": published, "source": "测试媒体", "channelName": "国内最新",
            "desc": "<p>正文摘要<script>secret()</script></p>", **extra}


class FakeResponse:
    def __init__(self, body=None, *, raw=None, status=200, gate=None, error=None):
        self.raw = json.dumps(response({"pagebean": {"contentlist": [article()]}}
                                       if body is None else body), ensure_ascii=False).encode() \
            if raw is None else raw
        self.status = status
        self.gate = gate
        self.error = error
        self.content = self
        self.started = asyncio.Event()
        self.reads = 0
        self.closed = False

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        self.closed = True

    async def iter_chunked(self, size):
        self.reads += 1
        self.started.set()
        if self.gate is not None:
            await self.gate.wait()
        if self.error is not None:
            raise self.error
        for offset in range(0, len(self.raw), size):
            yield self.raw[offset:offset + size]


class FakeSession:
    def __init__(self, http_response=None, *, error=None):
        self.response = http_response or FakeResponse()
        self.error = error
        self.calls = []

    def post(self, endpoint, **kwargs):
        self.calls.append((endpoint, kwargs))
        if self.error is not None:
            raise self.error
        return self.response


class ShowApiNewsTests(unittest.IsolatedAsyncioTestCase):
    def provider(self, options=None, credentials=None):
        selected_credentials = ({"SHOWAPI_APP_KEY": "test-app-key"}
                                if credentials is None else credentials)
        return ShowApiNewsProvider(
            {"credential_ref": "SHOWAPI_APP_KEY", "channel_name": "国内最新",
             "max_results": 3, "timeout_s": 10, **(options or {})},
            selected_credentials,
        )

    async def test_channels_uses_fixed_endpoint_and_normalizes_response(self):
        http_response = FakeResponse({"totalNum": 2, "channelList": [
            {"channelId": "chn-1", "name": "<b>国内最新</b>"},
            {"channelId": "chn-2", "name": "国际最新"},
        ]})
        session = FakeSession(http_response)
        result = await self.provider().channels(session)
        self.assertEqual(result, [{"id": "chn-1", "name": "国内最新"},
                                  {"id": "chn-2", "name": "国际最新"}])
        endpoint, request = session.calls[0]
        self.assertEqual(endpoint, BASE_URL + "/109-34")
        self.assertEqual(request["params"], {"appKey": "test-app-key"})
        self.assertEqual(request["data"], {})
        self.assertEqual(request["headers"]["Content-Type"],
                         "application/x-www-form-urlencoded")
        self.assertFalse(request["allow_redirects"])
        self.assertEqual(request["timeout"].total, 10)
        self.assertTrue(http_response.closed)

    async def test_channels_and_news_results_obey_configured_caps(self):
        channels = [{"channelId": f"chn-{index}", "name": f"频道{index}"}
                    for index in range(MAX_CHANNELS + 1)]
        session = FakeSession(FakeResponse({"channelList": channels}))
        self.assertEqual(len(await self.provider().channels(session)), MAX_CHANNELS)

        rows = [article(index) for index in range(20)]
        session = FakeSession(FakeResponse({"pagebean": {"contentlist": rows}}))
        self.assertEqual(len(await self.provider({"max_results": 2}).search("", session)), 2)

    async def test_search_uses_109_35_and_explicit_empty_channel_means_all_channels(self):
        session = FakeSession(FakeResponse({"ret_code": "0", "pagebean": {
            "contentlist": [article(1)]}}))
        result = await self.provider().search("  足球  ", session, channel_name="")
        self.assertEqual(result[0]["title"], "新闻1")
        endpoint, request = session.calls[0]
        self.assertEqual(endpoint, BASE_URL + "/109-35")
        self.assertEqual(request["data"], {"page": "1", "maxResult": "20", "title": "足球"})
        self.assertNotIn("channelName", request["data"])

    async def test_search_uses_default_channel_but_never_uses_or_for_empty_override(self):
        for chosen, expected in ((None, "默认频道"), ("", None), ("体育最新", "体育最新")):
            with self.subTest(chosen=chosen):
                session = FakeSession()
                provider = self.provider({"channel_name": "默认频道"})
                await provider.search("", session, channel_name=chosen)
                data = session.calls[0][1]["data"]
                self.assertEqual(data.get("channelName"), expected)
                self.assertNotIn("title", data)

    async def test_time_search_uses_109_36_and_sorts_explicit_dates_newest_first(self):
        older = article(1, published="2026-11-13 08:00:00")
        newer = article(2, published="2026-11-14 11:00:00")
        session = FakeSession(FakeResponse({"pagebean": {"contentlist": [older, newer]}}))
        results = await self.provider().search(
            "", session, channel_name="国内最新", begin_time="2026-11-13 00:00:00",
            end_time="2026-11-14 12:00:00")
        self.assertEqual([item["title"] for item in results], ["新闻2", "新闻1"])
        endpoint, request = session.calls[0]
        self.assertEqual(endpoint, BASE_URL + "/109-36")
        self.assertEqual(request["data"], {
            "page": "1", "maxResult": "20", "channelName": "国内最新",
            "beginTime": "2026-11-13 00:00:00", "endTime": "2026-11-14 12:00:00"})

    async def test_single_time_bound_is_allowed_and_never_synthesizes_other_bound(self):
        session = FakeSession()
        await self.provider().search("标题", session, begin_time="2026-11-14 00:00:00")
        self.assertEqual(session.calls[0][0], BASE_URL + "/109-36")
        self.assertNotIn("endTime", session.calls[0][1]["data"])

    async def test_invalid_dates_and_reverse_range_are_rejected_before_network(self):
        for begin, end in (("today", None), ("2026-02-30 00:00:00", None),
                           ("2026-11-15 00:00:00", "2026-11-14 00:00:00")):
            with self.subTest(begin=begin, end=end):
                session = FakeSession()
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider().search("", session, begin_time=begin, end_time=end)
                self.assertEqual(caught.exception.reason, "invalid_query")
                self.assertEqual(session.calls, [])

    async def test_hot_combines_hot_and_pinned_rows_without_claiming_a_date(self):
        session = FakeSession(FakeResponse({"ret_code": "0", "hot": [
            {"hot_research": "热门话题", "desc": "摘要", "link": "https://news.example.com/hot"}],
            "top": [{"top": "置顶标题", "link": "http://news.example.com/top"}]}))
        results = await self.provider({"max_results": 2}).hot(session)
        self.assertEqual([item["title"] for item in results], ["热门话题", "置顶标题"])
        self.assertEqual([item["published_at"] for item in results], ["", ""])
        self.assertEqual(session.calls[0][0], BASE_URL + "/109-37")
        self.assertEqual(session.calls[0][1]["data"], {})

    async def test_source_fields_are_bounded_html_cleaned_and_tracking_removed(self):
        row = article(title="<b>新闻\x00标题</b>" * 50, desc="<p>正文</p><style>hidden</style>摘要",
                      source="媒体" * 100, channelName="频道" * 100)
        session = FakeSession(FakeResponse({"pagebean": {"contentlist": [row]}}))
        result = (await self.provider().search("", session))[0]
        self.assertTrue(result["title"].startswith("新闻 标题"))
        self.assertLessEqual(len(result["title"]), 300)
        self.assertEqual(result["snippet"], "正文 摘要")
        self.assertLessEqual(len(result["snippet"]), 1200)
        self.assertLessEqual(len(result["source"]), 100)
        self.assertLessEqual(len(result["channel"]), 80)
        self.assertLessEqual(len(result["published_at"]), 40)
        self.assertEqual(result["url"], "https://news.example.com/0")
        self.assertNotIn("private-token", json.dumps(result))

    async def test_unknown_published_time_is_retained_but_not_guessed_or_sorted_as_current(self):
        unknown = article(1, published="昨天")
        known = article(2, published="2026-11-13 23:00:00")
        session = FakeSession(FakeResponse({"pagebean": {"contentlist": [unknown, known]}}))
        results = await self.provider().search("", session)
        self.assertEqual(results[0]["published_at"], known["pubDate"])
        self.assertEqual(results[1]["published_at"], "昨天")

    async def test_invalid_urls_fail_closed(self):
        for url in ("javascript:alert(1)", "file:///private", "https://user:pass@example.com/",
                    "https:///missing-host", "https://example.com:bad", "https://example.com:65536",
                    "https://example.com/\\\\evil.test", "https://example.com/%0dHeader: x",
                    "https://example.com/" + "x" * 800):
            with self.subTest(url=url):
                session = FakeSession(FakeResponse({"pagebean": {"contentlist": [
                    article(1, link=url)]}}))
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider().search("", session)
                self.assertEqual(caught.exception.reason, "invalid_schema")

    async def test_both_status_layers_accept_numeric_zero_and_hide_nonzero_codes(self):
        cases = [({"showapi_res_code": False, "showapi_res_body": {"ret_code": 0}},
                  "invalid_schema", 0),
                 ({"showapi_res_code": "401", "showapi_res_body": {"ret_code": 0}},
                  "upstream_error", 401),
                 ({"showapi_res_code": 0, "showapi_res_body": {"ret_code": "-7"}},
                  "upstream_error", -7),
                 ({"showapi_res_code": 0, "showapi_res_body": {"ret_code": False}},
                  "invalid_schema", 0)]
        for body, reason, code in cases:
            with self.subTest(body=body):
                session = FakeSession(FakeResponse(raw=json.dumps(body).encode()))
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider().search("隐私查询", session)
                self.assertEqual(caught.exception.reason, reason)
                self.assertEqual(caught.exception.upstream_code, code)
                self.assertNotIn("隐私", str(caught.exception))

    async def test_non_200_and_redirect_bodies_are_never_read(self):
        sensitive = b'{"message":"private-app-key"}'
        for status in (301, 401, 429, 500):
            with self.subTest(status=status):
                http_response = FakeResponse(raw=sensitive, status=status)
                session = FakeSession(http_response)
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider().channels(session)
                self.assertEqual((caught.exception.reason, caught.exception.http_status),
                                 ("http_status", status))
                self.assertEqual(http_response.reads, 0)
                self.assertTrue(http_response.closed)
                self.assertNotIn("private-app-key", str(caught.exception))

    async def test_app_key_is_only_in_query_and_redirects_are_disabled(self):
        session = FakeSession(FakeResponse({"channelList": [
            {"channelId": "chn-1", "name": "国内最新"}]}))
        await self.provider().channels(session)
        endpoint, request = session.calls[0]
        self.assertEqual(endpoint, BASE_URL + "/109-34")
        self.assertEqual(request["params"], {"appKey": "test-app-key"})
        self.assertNotIn("test-app-key", json.dumps(request["data"]))
        self.assertFalse(request["allow_redirects"])

    async def test_missing_or_example_key_fails_before_network(self):
        for credentials in ({}, {"SHOWAPI_APP_KEY": "YOUR_APPKEY"},
                            {"SHOWAPI_APP_KEY": "your-appKey"},
                            {"SHOWAPI_APP_KEY": "paste-your-appkey"},
                            {"SHOWAPI_APP_KEY": "<paste-key-here>"},
                            {"SHOWAPI_APP_KEY": "replace_me"}):
            with self.subTest(credentials=credentials):
                session = FakeSession()
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider(credentials=credentials).channels(session)
                self.assertEqual(caught.exception.reason, "missing_credentials")
                self.assertEqual(session.calls, [])

    async def test_invalid_options_fail_before_network(self):
        for options in ({"max_results": 0}, {"max_results": 6}, {"timeout_s": 0},
                        {"timeout_s": 21}, {"timeout_s": True}):
            with self.subTest(options=options):
                session = FakeSession()
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider(options).channels(session)
                self.assertEqual(caught.exception.reason, "invalid_options")
                self.assertEqual(session.calls, [])

    async def test_invalid_and_empty_response_shapes_fail_safely(self):
        cases = [({"channelList": {}}, "invalid_schema"),
                 ({"channelList": []}, "empty_results"),
                 ({"pagebean": {"contentlist": []}}, "empty_results"),
                 ({"pagebean": {"contentlist": [None]}}, "invalid_schema"),
                 ({"pagebean": {"contentlist": [{"title": ""}]}}, "invalid_schema"),
                 ({"hot": [], "top": []}, "empty_results"),
                 ({"hot": {}, "top": []}, "invalid_schema")]
        for body, reason in cases:
            with self.subTest(body=body):
                session = FakeSession(FakeResponse(body))
                call = self.provider().channels(session) if "channelList" in body else (
                    self.provider().hot(session) if "hot" in body else self.provider().search("", session))
                with self.assertRaises(ShowApiNewsError) as caught:
                    await call
                self.assertEqual(caught.exception.reason, reason)

    async def test_large_and_malformed_responses_are_rejected_and_closed(self):
        for raw, reason in ((b"x" * (MAX_RESPONSE_BYTES + 1), "response_too_large"),
                            (b"not-json", "invalid_json"), (b"\xff", "invalid_json")):
            with self.subTest(reason=reason):
                http_response = FakeResponse(raw=raw)
                with self.assertRaises(ShowApiNewsError) as caught:
                    await self.provider().channels(FakeSession(http_response))
                self.assertEqual(caught.exception.reason, reason)
                self.assertTrue(http_response.closed)

    async def test_cancel_releases_response_and_propagates_cancellation(self):
        http_response = FakeResponse(gate=asyncio.Event())
        task = asyncio.create_task(self.provider().channels(FakeSession(http_response)))
        await http_response.started.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(http_response.closed)

    async def test_timeout_and_connection_errors_are_sanitized(self):
        http_response = FakeResponse(gate=asyncio.Event())
        provider = self.provider({"timeout_s": 1})
        with self.assertRaises(ShowApiNewsError) as caught:
            await provider.channels(FakeSession(http_response))
        self.assertEqual(caught.exception.reason, "timeout")
        self.assertTrue(http_response.closed)

        session = FakeSession(error=aiohttp.ClientConnectionError("private-app-key"))
        with self.assertRaises(ShowApiNewsError) as caught:
            await self.provider().channels(session)
        self.assertEqual(caught.exception.reason, "connection_failed")
        self.assertNotIn("private-app-key", str(caught.exception))
        self.assertTrue(caught.exception.__suppress_context__)


if __name__ == "__main__":
    unittest.main()
