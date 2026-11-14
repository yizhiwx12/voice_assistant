import json
import unittest
from datetime import datetime, timezone

from core.news_routing import (
    NEWS_INSTRUCTIONS, NewsRequest, is_news_discussion, news_request,
    with_news_results,
)
from core.realtime_info import SHANGHAI


class NewsRoutingTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 11, 14, 9, 30, tzinfo=SHANGHAI)
        self.options = {"enabled": True}

    def test_news_feature_requires_explicit_boolean_enable(self):
        for options in ({}, {"enabled": False}, {"enabled": 1}, {"enabled": "true"}, None):
            with self.subTest(options=options):
                self.assertIsNone(news_request("今天有什么新闻", options, self.now))

    def test_generic_broadcast_uses_default_channel_and_local_date_window(self):
        cases = ("今天有什么新闻", "播报三条今天的新闻")
        for query in cases:
            with self.subTest(query=query):
                result = news_request(query, self.options, self.now)
                self.assertEqual(result, NewsRequest(
                    channel_name="国内最新", begin_time="2026-11-14 00:00:00",
                    end_time="2026-11-14 09:30:00"))

    def test_categories_and_explicit_topics_are_separated(self):
        self.assertEqual(news_request("播报三条科技新闻", self.options, self.now),
                         NewsRequest(channel_name="科技最新"))
        self.assertEqual(news_request("听财经新闻", self.options, self.now),
                         NewsRequest(channel_name="财经最新"))
        self.assertEqual(news_request("听三条最新国际新闻", self.options, self.now),
                         NewsRequest(channel_name="国际最新"))
        self.assertEqual(news_request("英伟达最新新闻", self.options, self.now),
                         NewsRequest(keyword="英伟达"))
        self.assertEqual(news_request("今天有什么科技新闻", self.options, self.now),
                         NewsRequest(channel_name="科技最新", begin_time="2026-11-14 00:00:00",
                                     end_time="2026-11-14 09:30:00"))
        self.assertEqual(news_request("最近有什么突发新闻", self.options, self.now),
                         NewsRequest(channel_name="国内最新"))

    def test_channel_list_and_undated_hot_list_have_distinct_kinds(self):
        for query in ("有哪些新闻频道", "新闻频道都有哪些"):
            with self.subTest(query=query):
                self.assertEqual(news_request(query, self.options, self.now),
                                 NewsRequest(kind="channels"))
        self.assertEqual(news_request("看看热搜榜", self.options, self.now),
                         NewsRequest(kind="hot"))
        self.assertIsNone(news_request("今天的热搜榜", self.options, self.now))
        self.assertIsNone(news_request("昨天热搜", self.options, self.now))

    def test_relative_and_explicit_dates_are_shanghai_closed_intervals(self):
        self.assertEqual(news_request("昨天有什么新闻", self.options, self.now),
                         NewsRequest(channel_name="国内最新", begin_time="2026-11-13 00:00:00",
                                     end_time="2026-11-13 23:59:59"))
        self.assertEqual(news_request("前天科技新闻", self.options, self.now),
                         NewsRequest(channel_name="科技最新", begin_time="2026-11-12 00:00:00",
                                     end_time="2026-11-12 23:59:59"))
        utc_now = datetime(2026, 11, 14, 1, 30, tzinfo=timezone.utc)
        self.assertEqual(news_request("今天有什么新闻", self.options, utc_now).end_time,
                         "2026-11-14 09:30:00")

    def test_old_future_ambiguous_and_invalid_dates_fall_back(self):
        for query in ("明天有什么新闻", "大前天新闻", "2026年10月20日新闻", "上周有什么新闻",
                      "最近三天的新闻", "这两天科技新闻", "过去两天新闻",
                      "一个月前新闻", "2026年2月30日新闻", "上个月科技新闻"):
            with self.subTest(query=query):
                self.assertIsNone(news_request(query, self.options, self.now))
        self.assertIsNone(news_request("2026年10月30日新闻", self.options, self.now))

    def test_event_verification_and_uncertain_questions_fall_back(self):
        for query in ("某CEO卸任怎么回事", "某公司收购是真的吗", "昨天地震了吗",
                      "某公司新闻有什么后续", "这条新闻是否属实", "最近新闻怎么样",
                      "告诉我关于英伟达的消息", "今天几号，顺便告诉我科技新闻"):
            with self.subTest(query=query):
                self.assertIsNone(news_request(query, self.options, self.now))

    def test_concepts_and_news_writing_are_marked_as_discussion(self):
        for query in ("新闻是什么", "新闻专业学什么", "帮我写新闻稿", "新闻主播笑话"):
            with self.subTest(query=query):
                self.assertTrue(is_news_discussion(query))
                self.assertIsNone(news_request(query, self.options, self.now))
        for query in ("播报三条科技新闻", "英伟达最新新闻", "看看热搜",
                      "新闻主播今天去世了吗", "今天的新闻记者被捕了吗"):
            with self.subTest(query=query):
                self.assertFalse(is_news_discussion(query))

    def test_configured_default_channel_is_used_only_for_generic_broadcast(self):
        options = {"enabled": True, "channel_name": "国际最新"}
        self.assertEqual(news_request("最新新闻", options, self.now),
                         NewsRequest(channel_name="国际最新"))
        self.assertEqual(news_request("英伟达最新新闻", options, self.now),
                         NewsRequest(keyword="英伟达"))
        self.assertEqual(news_request("最新新闻", {"enabled": True, "channel_name": ""}, self.now),
                         NewsRequest(channel_name=""))

    def test_news_context_is_untrusted_user_data_and_does_not_mutate_input(self):
        messages = [{"role": "system", "content": "原有系统指令"},
                    {"role": "user", "content": "今天有什么新闻"}]
        original = [{**item} for item in messages]
        results = [{"title": "来源标题", "published_at": "2026-11-14 09:00",
                    "url": "https://example.com/news"}]
        enriched = with_news_results(messages, results)
        self.assertEqual(messages, original)
        self.assertEqual(enriched[-1], original[-1])
        self.assertEqual(enriched[1], {"role": "system", "content": NEWS_INSTRUCTIONS})
        external = next(item for item in enriched if item["role"] == "user" and
                        item["content"].lstrip().startswith("{") and
                        json.loads(item["content"]).get("external_news_results") is not None)
        self.assertEqual(external["role"], "user")
        self.assertIn("https://example.com/news", external["content"])
        self.assertIn("抓取时间和事件发生时间不同", NEWS_INSTRUCTIONS)
        self.assertFalse(any("https://example.com/news" in item["content"]
                             for item in enriched if item["role"] == "system"))


if __name__ == "__main__":
    unittest.main()
