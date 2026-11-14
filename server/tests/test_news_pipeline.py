"""ShowAPI 新闻在真实流水线中的路由、上下文、取消结果和降级行为。"""

import unittest
from unittest.mock import AsyncMock, patch

from tests.test_pipeline import Harness, selection
from tests.test_realtime_info import CapturingLlm


class NewsPipelineTests(unittest.IsolatedAsyncioTestCase):
    def harness(self, query, *, enabled=True, web_search=True, key=True):
        selected = selection()
        selected.mode = "realtime"
        selected.llm.options = {"enable_web_search": web_search}
        selected.news = {"enabled": enabled, "credential_ref": "SHOWAPI_APP_KEY",
                         "channel_name": "国内最新", "max_results": 3, "timeout_s": 10}
        harness = Harness(selected, text=query)
        harness.llm = CapturingLlm()
        harness.pipeline.credentials = {"SHOWAPI_APP_KEY": "synthetic-key"} if key else {}
        self.addAsyncCleanup(harness.pipeline.close)
        return harness

    async def test_bulletin_results_enter_only_current_llm_context_and_tts(self):
        harness = self.harness("今天有什么新闻")
        sources = [{"title": "本地报道", "url": "https://news.example/story",
                    "snippet": "来源摘要", "source": "测试媒体",
                    "published_at": "2026-11-14 09:00:00", "channel": "国内最新"}]
        with patch("core.pipeline.ShowApiNewsProvider") as provider, \
                patch("core.pipeline.AnySearchProvider") as anysearch:
            provider.return_value.search = AsyncMock(return_value=sources)
            await harness.run_turn()
            request = harness.llm.requests[-1]
            news_context = next(item for item in request if sources[0]["url"] in item["content"])
            self.assertEqual(news_context["role"], "user")
            self.assertIn("external_news_results", news_context["content"])
            self.assertEqual(request[-1]["content"], harness.asr.text)
            self.assertFalse(any(sources[0]["url"] in item["content"]
                                 for item in harness.pipeline.session.history))
            self.assertEqual("".join(harness.tts.text), harness.pipeline.answer)
            self.assertLess(
                next(i for i, event in enumerate(harness.events) if event["type"] == "llm.done"),
                next(i for i, event in enumerate(harness.events) if event["type"] == "response.done"),
            )
        provider.return_value.search.assert_awaited_once()
        anysearch.assert_not_called()

    async def test_channel_list_is_directly_spoken_without_llm_or_search(self):
        harness = self.harness("有哪些新闻频道")
        with patch("core.pipeline.ShowApiNewsProvider") as provider, \
                patch("core.pipeline.AnySearchProvider") as anysearch:
            provider.return_value.channels = AsyncMock(return_value=[
                {"id": "cn", "name": "国内最新"}, {"id": "tech", "name": "科技最新"}])
            await harness.run_turn()
        self.assertIn("国内最新、科技最新", harness.pipeline.answer)
        self.assertEqual(harness.llm.calls, 0)
        self.assertEqual("".join(harness.tts.text), harness.pipeline.answer)
        anysearch.assert_not_called()

    async def test_explicit_category_bulletin_without_news_keyword_still_uses_provider(self):
        harness = self.harness("听科技资讯")
        sources = [{"title": "科技报道", "url": "https://news.example/tech", "snippet": "摘要"}]
        with patch("core.pipeline.ShowApiNewsProvider") as news:
            news.return_value.search = AsyncMock(return_value=sources)
            await harness.run_turn()
        news.return_value.search.assert_awaited_once()
        self.assertIn("external_news_results", str(harness.llm.requests[-1]))

    async def test_missing_news_key_fails_closed_but_normal_chat_still_runs(self):
        harness = self.harness("今天有什么新闻", key=False)
        with patch("core.pipeline.ShowApiNewsProvider") as provider, \
                patch("core.pipeline.AnySearchProvider") as anysearch:
            await harness.run_turn()
            self.assertIn("尚未配置有效 AppKey", harness.pipeline.answer)
            self.assertEqual(harness.llm.calls, 0)
            harness.asr.text = "你好"
            await harness.run_turn(2)
        provider.assert_not_called()
        anysearch.assert_not_called()
        self.assertEqual(harness.llm.calls, 1)

    async def test_provider_failure_does_not_fall_back_to_llm_guess(self):
        harness = self.harness("今天有什么科技新闻")
        with patch("core.pipeline.ShowApiNewsProvider") as provider, \
                patch("core.pipeline.AnySearchProvider") as anysearch:
            provider.return_value.search = AsyncMock(side_effect=RuntimeError("private upstream body"))
            await harness.run_turn()
        self.assertIn("新闻服务暂时不可用", harness.pipeline.answer)
        self.assertEqual(harness.llm.calls, 0)
        anysearch.assert_not_called()

    async def test_concrete_event_verification_stays_on_existing_search_path(self):
        harness = self.harness("新闻主播今天去世了吗", enabled=True)
        search_results = [{"title": "事件核实", "snippet": "搜索来源", "url": "https://search.example/1"}]
        with patch("core.pipeline.ShowApiNewsProvider") as news, \
                patch("core.pipeline.AnySearchProvider") as search:
            search.return_value.search = AsyncMock(return_value=search_results)
            await harness.run_turn()
        news.assert_not_called()
        search.return_value.search.assert_awaited_once()
        self.assertIn("external_search_results", str(harness.llm.requests[-1]))

    async def test_news_concepts_and_writing_requests_remain_ordinary_chat(self):
        for query in ("新闻是什么", "新闻专业学什么", "帮我写新闻稿", "新闻主播笑话"):
            with self.subTest(query=query):
                harness = self.harness(query)
                with patch("core.pipeline.ShowApiNewsProvider") as news, \
                        patch("core.pipeline.AnySearchProvider") as search:
                    await harness.run_turn()
                news.assert_not_called()
                search.assert_not_called()
                self.assertEqual(harness.llm.calls, 1)

    async def test_search_disabled_remains_independent_from_news_feature(self):
        harness = self.harness("今天有什么新闻", web_search=False)
        sources = [{"title": "报道", "url": "https://news.example/1", "snippet": "摘要",
                    "source": "媒体", "published_at": "", "channel": "国内最新"}]
        with patch("core.pipeline.ShowApiNewsProvider") as news, \
                patch("core.pipeline.AnySearchProvider") as search:
            news.return_value.search = AsyncMock(return_value=sources)
            await harness.run_turn()
        news.return_value.search.assert_awaited_once()
        search.assert_not_called()
        self.assertIn("external_news_results", str(harness.llm.requests[-1]))

    async def test_news_cancel_joins_request_before_next_turn(self):
        import asyncio

        harness = self.harness("今天有什么新闻")
        entered, released = asyncio.Event(), asyncio.Event()

        async def wait(*args, **kwargs):
            entered.set()
            try:
                await asyncio.Event().wait()
            finally:
                released.set()

        with patch("core.pipeline.ShowApiNewsProvider") as provider:
            provider.return_value.search = AsyncMock(side_effect=wait)
            await harness.pipeline.start_utterance(1)
            await harness.pipeline.end_utterance()
            await asyncio.wait_for(entered.wait(), 1)
            await asyncio.wait_for(harness.pipeline.cancel_response(), 1)
            self.assertTrue(released.is_set())
            self.assertEqual(harness.llm.calls, 0)
            self.assertFalse(any(event["type"] == "llm.done" for event in harness.events))
            harness.asr.text = "你好"
            await harness.run_turn(2)
        self.assertEqual(harness.llm.calls, 1)
        self.assertEqual(harness.events[-1]["reason"], "complete")


if __name__ == "__main__":
    unittest.main()
