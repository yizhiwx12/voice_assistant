import asyncio
import json
import unittest
from unittest.mock import patch

import aiohttp

from providers.search.anysearch import (
    AnySearchError, AnySearchProvider, MAX_RESPONSE_BYTES, SEARCH_ENDPOINT,
)


def success(results=None):
    return {"code": 0, "data": {"results": results if results is not None else [
        {"title": "来源标题", "url": "https://example.com/news", "snippet": "报道摘要"},
    ]}}


class FakeResponse:
    def __init__(self, body=None, *, raw=None, status=200, gate=None, error=None):
        self.raw = (json.dumps(success() if body is None else body).encode("utf-8")
                    if raw is None else raw)
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
    def __init__(self, response, *, error=None):
        self.response = response
        self.error = error
        self.calls = []

    def post(self, endpoint, **kwargs):
        self.calls.append((endpoint, kwargs))
        if self.error is not None:
            raise self.error
        return self.response


class AnySearchTests(unittest.IsolatedAsyncioTestCase):
    async def test_anonymous_search_uses_fixed_endpoint_and_expected_payload(self):
        response = FakeResponse()
        session = FakeSession(response)
        result = await AnySearchProvider({}).search("  最新新闻  ", session)
        self.assertEqual(result, success()["data"]["results"])
        endpoint, request = session.calls[0]
        self.assertEqual(endpoint, SEARCH_ENDPOINT)
        self.assertEqual(request["json"], {"query": "最新新闻", "max_results": 5,
                                          "language": "zh-CN", "zone": "cn"})
        self.assertNotIn("Authorization", request["headers"])
        self.assertEqual(request["timeout"].total, 20)
        self.assertTrue(response.closed)

    async def test_authenticated_search_only_puts_key_in_authorization_header(self):
        session = FakeSession(FakeResponse())
        await AnySearchProvider({"ANYSEARCH_API_KEY": " private-key "}).search("新闻", session)
        request = session.calls[0][1]
        self.assertEqual(request["headers"]["Authorization"], "Bearer private-key")
        self.assertNotIn("private-key", json.dumps(request["json"]))

    async def test_result_count_and_fields_are_bounded(self):
        rows = [{"title": "甲" * 400, "url": "https://example.com/" + str(index),
                 "content": "乙" * 1500} for index in range(6)]
        rows[-1] = None  # 请求结果上限为五条，超出的结果不保留。
        result = await AnySearchProvider({}).search("新闻", FakeSession(FakeResponse(success(rows))))
        self.assertEqual(len(result), 5)
        self.assertTrue(all(len(item["title"]) == 300 and len(item["snippet"]) == 1200
                            for item in result))
        self.assertEqual(set(result[0]), {"title", "url", "snippet"})

    async def test_missing_optional_snippet_is_accepted(self):
        row = {"title": "只有标题", "url": "http://example.com/news"}
        result = await AnySearchProvider({}).search("新闻", FakeSession(FakeResponse(success([row]))))
        self.assertEqual(result[0]["snippet"], "")

    async def test_invalid_top_level_schema_and_upstream_code_are_safe_errors(self):
        cases = [([], "invalid_schema"), ({}, "invalid_schema"),
                 ({"code": False}, "invalid_schema"),
                 ({"code": 0, "data": []}, "invalid_schema"),
                 ({"code": 0, "data": {"results": {}}}, "invalid_schema"),
                 ({"code": -1, "message": "private-body"}, "upstream_error"),
                 (success([]), "empty_results")]
        cases.append((success([{"title": "", "url": "https://example.com"}]), "empty_results"))
        for body, reason in cases:
            with self.subTest(reason=reason, body=body):
                response = FakeResponse(body)
                with self.assertRaises(AnySearchError) as caught:
                    await AnySearchProvider({}).search("private-query", FakeSession(response))
                self.assertEqual(caught.exception.reason, reason)
                self.assertNotIn("private", str(caught.exception))
                self.assertTrue(response.closed)

    async def test_invalid_result_fields_and_urls_fail_closed(self):
        rows = [None, {}, {"title": None, "url": "https://example.com"},
                {"title": "a", "url": "https://example.com", "snippet": []}]
        rows.extend({"title": "a", "url": url} for url in (
            "file:///C:/private", "javascript:alert(1)", "https://user:password@example.com/",
            "https:///news", "https://example.com:bad/news", "https://example.com:65536/",
            "https://example.com/line\nbreak", "https://example.com/" + "a" * 800,
        ))
        for row in rows:
            with self.subTest(row=row):
                with self.assertRaises(AnySearchError) as caught:
                    await AnySearchProvider({}).search("新闻", FakeSession(FakeResponse(success([row]))))
                self.assertEqual(caught.exception.reason, "invalid_schema")

    async def test_non_200_body_is_never_read_or_exposed_including_quota_credentials(self):
        sensitive = b'{"message":"password=private-password api_key=private-key"}'
        for status in (401, 402, 429, 502):
            with self.subTest(status=status):
                response = FakeResponse(raw=sensitive, status=status)
                with self.assertRaises(AnySearchError) as caught:
                    await AnySearchProvider({"ANYSEARCH_API_KEY": "private-key"}).search(
                        "private-query", FakeSession(response))
                error = caught.exception
                self.assertEqual((error.reason, error.http_status), ("http_status", status))
                self.assertEqual(str(error), f"AnySearch http_status http_status={status}")
                self.assertEqual(response.reads, 0)
                self.assertTrue(response.closed)

    async def test_large_response_is_rejected_before_json_parse(self):
        response = FakeResponse(raw=b"x" * (MAX_RESPONSE_BYTES + 1))
        with self.assertRaises(AnySearchError) as caught:
            await AnySearchProvider({}).search("新闻", FakeSession(response))
        self.assertEqual(caught.exception.reason, "response_too_large")
        self.assertTrue(response.closed)

    async def test_invalid_json_and_utf8_are_safe(self):
        for raw in (b"private-not-json", b"\xff"):
            with self.subTest(raw=raw):
                with self.assertRaises(AnySearchError) as caught:
                    await AnySearchProvider({}).search("新闻", FakeSession(FakeResponse(raw=raw)))
                self.assertEqual(caught.exception.reason, "invalid_json")
                self.assertNotIn("private", str(caught.exception))

    async def test_timeout_releases_response_context(self):
        response = FakeResponse(gate=asyncio.Event())
        with patch("providers.search.anysearch.REQUEST_TIMEOUT_SECONDS", 0.01):
            with self.assertRaises(AnySearchError) as caught:
                await AnySearchProvider({}).search("新闻", FakeSession(response))
        self.assertEqual(caught.exception.reason, "timeout")
        self.assertTrue(response.closed)

    async def test_connection_error_never_exposes_vendor_exception(self):
        session = FakeSession(FakeResponse(), error=aiohttp.ClientConnectionError("private-key"))
        with self.assertRaises(AnySearchError) as caught:
            await AnySearchProvider({}).search("private-query", session)
        self.assertEqual(caught.exception.reason, "connection_failed")
        self.assertNotIn("private", str(caught.exception))
        self.assertTrue(caught.exception.__suppress_context__)

    async def test_cancellation_propagates_after_releasing_response(self):
        response = FakeResponse(gate=asyncio.Event())
        task = asyncio.create_task(AnySearchProvider({}).search("新闻", FakeSession(response)))
        await asyncio.wait_for(response.started.wait(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(response.closed)

    async def test_blank_query_does_not_make_request(self):
        session = FakeSession(FakeResponse())
        for query in ("", " \n", None):
            with self.subTest(query=query):
                with self.assertRaises(AnySearchError) as caught:
                    await AnySearchProvider({}).search(query, session)
                self.assertEqual(caught.exception.reason, "invalid_query")
        self.assertEqual(session.calls, [])


if __name__ == "__main__":
    unittest.main()
