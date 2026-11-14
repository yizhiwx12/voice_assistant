import asyncio
import json
import unittest
from types import SimpleNamespace

try:
    import aiohttp
except ImportError:
    aiohttp = None

from providers.llm.chat_completions import ChatCompletionsProvider, _sse_events
from providers.asr.qwen_batch import QwenBatchProvider
from providers.tts.cosyvoice import CosyVoiceTtsProvider


class FakeContent:
    def __init__(self, chunks, block=False):
        self.chunks = chunks
        self.block = block
        self.blocked = asyncio.Event()

    async def iter_any(self):
        for chunk in self.chunks:
            yield chunk
        if self.block:
            self.blocked.set()
            await asyncio.Event().wait()


class FakeHttpResponse:
    def __init__(self, chunks, block=False):
        self.status = 200
        self.content = FakeContent(chunks, block)
        self.closed = False

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        self.closed = True

    async def read(self):
        data = bytearray()
        async for chunk in self.content.iter_any():
            data.extend(chunk)
        return bytes(data)


class FakeSession:
    def __init__(self, response=None, websocket=None):
        self.response = response
        self.websocket = websocket
        self.calls = []

    def post(self, endpoint, **kwargs):
        self.calls.append((endpoint, kwargs))
        return self.response

    async def ws_connect(self, endpoint, **kwargs):
        self.calls.append((endpoint, kwargs))
        return self.websocket


class FakeWebSocket:
    def __init__(self):
        self.messages = asyncio.Queue()
        self.sent = []
        self.closed = False
        self.provider = None

    async def send_json(self, value):
        self.sent.append(value)
        if value["header"]["action"] == "run-task":
            self.event("task-started", value["header"]["task_id"])

    def event(self, event, task_id=None, **header):
        self.messages.put_nowait(SimpleNamespace(
            type=aiohttp.WSMsgType.TEXT,
            data=json.dumps({"header": {
                "event": event, "task_id": task_id or self.provider.task_id,
                **header,
            }}),
        ))

    def binary(self, pcm):
        self.messages.put_nowait(SimpleNamespace(type=aiohttp.WSMsgType.BINARY, data=pcm))

    async def receive(self, timeout):
        return await asyncio.wait_for(self.messages.get(), timeout)

    async def close(self):
        self.closed = True


def sse_event(delta=None, finish=None, **extra):
    body = {"choices": [{"index": 0, "delta": delta or {}, "finish_reason": finish}]}
    body.update(extra)
    return "data: " + json.dumps(body, ensure_ascii=False) + "\r\n\r\n"


class SseParserTests(unittest.IsolatedAsyncioTestCase):
    async def test_line_endings_and_multiline_events_survive_every_byte_split(self):
        wire = ":注释\r\ndata: 第一行\r\ndata: 第二行\r\n\r\ndata: 末行".encode()
        # 每个位置都切一次，覆盖拆开的 CRLF 和中文 UTF-8 字节。
        for split in range(len(wire) + 1):
            with self.subTest(split=split):
                content = FakeContent([wire[:split], wire[split:]])
                self.assertEqual([event async for event in _sse_events(content)],
                                 ["第一行\n第二行", "末行"])

    async def test_lone_cr_and_lf_both_delimit_lines(self):
        wire = b"data: first\r\rdata: second\n\ndata: third\r\n\r\n"
        content = FakeContent([wire[index:index + 1] for index in range(len(wire))])
        self.assertEqual([event async for event in _sse_events(content)],
                         ["first", "second", "third"])

    async def test_line_limit_counts_utf8_bytes_before_line_ending(self):
        # 11 个字符但 21 字节，旧解析器按字符数检查会错误放行。
        content = FakeContent(["data: 汉字汉字汉".encode()])
        with self.assertRaisesRegex(RuntimeError, "line is too large"):
            _ = [event async for event in _sse_events(content, max_event_bytes=16)]

    async def test_multiline_event_limit_includes_joined_newlines(self):
        content = FakeContent([b"data: a\ndata: b\ndata: c\ndata: d\ndata: e\n\n"])
        with self.assertRaisesRegex(RuntimeError, "event is too large"):
            _ = [event async for event in _sse_events(content, max_event_bytes=8)]

    async def test_long_fragmented_event_is_lossless(self):
        expected = "汉字和English" * 20000
        wire = ("data: " + expected + "\n\n").encode()
        content = FakeContent([wire[index:index + 997] for index in range(0, len(wire), 997)])
        self.assertEqual([event async for event in _sse_events(content)], [expected])

    async def test_incomplete_utf8_at_eof_is_rejected(self):
        with self.assertRaises(UnicodeDecodeError):
            _ = [event async for event in _sse_events(FakeContent([b"data: \xe6\xb1"]))]


@unittest.skipUnless(aiohttp, "requires aiohttp (server/requirements.txt)")
class LlmStreamTests(unittest.IsolatedAsyncioTestCase):
    def provider(self, **options):
        return ChatCompletionsProvider({"model": "test-model", **options}, {"DEEPSEEK_API_KEY": "test-key"})

    async def test_async_nonstreaming_keeps_old_request_behavior(self):
        response = FakeHttpResponse([json.dumps({"choices": [{"message": {"content": "完整回答"}}]}).encode()])
        session = FakeSession(response)
        self.assertEqual(await self.provider().generate_async([], session), "完整回答")
        self.assertIs(session.calls[0][1]["json"]["stream"], False)
        self.assertEqual(session.calls[0][1]["json"]["max_tokens"], 128)
        self.assertTrue(response.closed)

    async def test_async_nonstreaming_cancellation_closes_response(self):
        response = FakeHttpResponse([], block=True)
        task = asyncio.create_task(self.provider().generate_async([], FakeSession(response)))
        await response.content.blocked.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(response.closed)

    async def test_fragmented_utf8_only_yields_answer_content(self):
        wire = (":keepalive\r\n\r\n" +
                sse_event({"role": "assistant", "reasoning_content": "private thinking"}) +
                sse_event({"content": "你好"}) +
                sse_event({"tool_calls": [{"arguments": "private tool"}]}) +
                sse_event({"content": "。"}) + sse_event(finish="stop") +
                "data: [DONE]\r\n\r\n").encode("utf-8")
        response = FakeHttpResponse([wire[i:i+1] for i in range(len(wire))])
        session = FakeSession(response)
        self.assertEqual([value async for value in self.provider().stream([], session)], ["你好", "。"])
        self.assertTrue(response.closed)
        self.assertTrue(session.calls[0][1]["json"]["stream"])
        self.assertEqual(session.calls[0][1]["json"]["max_tokens"], 1024)

    async def test_explicit_token_budget_applies_to_both_request_modes(self):
        provider = self.provider(max_tokens=384)
        response = FakeHttpResponse([json.dumps({"choices": [{"message": {"content": "完整回答"}}]}).encode()])
        session = FakeSession(response)
        self.assertEqual(await provider.generate_async([], session), "完整回答")
        self.assertEqual(session.calls[0][1]["json"]["max_tokens"], 384)

        response = FakeHttpResponse([sse_event({"content": "完整回答"}, finish="stop").encode()])
        session = FakeSession(response)
        self.assertEqual([value async for value in provider.stream([], session)], ["完整回答"])
        self.assertEqual(session.calls[0][1]["json"]["max_tokens"], 384)

    async def test_done_marker_accepts_complete_text_without_finish_reason(self):
        wire = sse_event({"content": "完整回答"}) + "data: [DONE]\r\n\r\n"
        response = FakeHttpResponse([wire.encode()])
        self.assertEqual([value async for value in self.provider().stream([], FakeSession(response))], ["完整回答"])
        self.assertTrue(response.closed)

    async def test_token_limit_rejects_reasoning_only_and_partial_answer(self):
        for answer in (None, "尚未完整的回答"):
            with self.subTest(answer=answer):
                wire = sse_event({"reasoning_content": "private thinking"})
                if answer:
                    wire += sse_event({"content": answer})
                wire += sse_event(finish="length") + "data: [DONE]\r\n\r\n"
                response = FakeHttpResponse([wire.encode()])
                received = []
                with self.assertRaisesRegex(RuntimeError, "token limit before completion") as error:
                    async for value in self.provider().stream([], FakeSession(response)):
                        received.append(value)
                self.assertEqual(received, [answer] if answer else [])
                self.assertNotIn("private thinking", str(error.exception))
                self.assertTrue(response.closed)

    async def test_nonstop_finish_reasons_fail_without_exposing_upstream_value(self):
        for reason in ("content_filter", "tool_calls", "function_call", "private-upstream-value"):
            with self.subTest(reason=reason):
                wire = sse_event({"content": "partial"}) + sse_event(finish=reason) + "data: [DONE]\r\n\r\n"
                response = FakeHttpResponse([wire.encode()])
                with self.assertRaisesRegex(RuntimeError, "did not finish with a complete answer") as error:
                    _ = [value async for value in self.provider().stream([], FakeSession(response))]
                self.assertNotIn(reason, str(error.exception))
                self.assertTrue(response.closed)

    async def test_eof_without_done_or_finish_is_rejected(self):
        response = FakeHttpResponse([sse_event({"content": "partial"}).encode()])
        with self.assertRaisesRegex(RuntimeError, "before completion"):
            _ = [value async for value in self.provider().stream([], FakeSession(response))]
        self.assertTrue(response.closed)

    async def test_finish_reason_allows_eof_without_done(self):
        response = FakeHttpResponse([sse_event({"content": "done"}, finish="stop").encode()])
        self.assertEqual([value async for value in self.provider().stream([], FakeSession(response))], ["done"])

    async def test_upstream_error_body_is_not_exposed(self):
        response = FakeHttpResponse([b'data: {"error":{"message":"secret-token"}}\n\n'])
        with self.assertRaises(RuntimeError) as error:
            _ = [value async for value in self.provider().stream([], FakeSession(response))]
        self.assertNotIn("secret-token", str(error.exception))

    async def test_cancellation_closes_http_response(self):
        response = FakeHttpResponse([], block=True)

        async def drain():
            return [value async for value in self.provider().stream([], FakeSession(response))]

        task = asyncio.create_task(drain())
        await response.content.blocked.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(response.closed)


@unittest.skipUnless(aiohttp, "requires aiohttp (server/requirements.txt)")
class QwenBatchAsyncTests(unittest.IsolatedAsyncioTestCase):
    def provider(self):
        return QwenBatchProvider({}, {"DASHSCOPE_API_KEY": "test-key",
                                     "DASHSCOPE_WORKSPACE_ID": "test-workspace"})

    async def test_async_batch_reuses_wav_request_and_transcript_parser(self):
        response = FakeHttpResponse([json.dumps({"output": {"text": " 完整\n结果 "}}).encode()])
        session = FakeSession(response)
        self.assertEqual(await self.provider().recognize_wav_async(b"wav-data", session), "完整 结果")
        payload = session.calls[0][1]["json"]
        self.assertTrue(payload["input"]["messages"][0]["content"][0]["input_audio"]["data"].startswith("data:audio/wav;base64,"))
        self.assertTrue(response.closed)

    async def test_async_batch_cancellation_closes_response(self):
        response = FakeHttpResponse([], block=True)
        task = asyncio.create_task(self.provider().recognize_wav_async(b"wav", FakeSession(response)))
        await response.content.blocked.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(response.closed)


@unittest.skipUnless(aiohttp, "requires aiohttp (server/requirements.txt)")
class CosyVoiceStreamTests(unittest.IsolatedAsyncioTestCase):
    def provider(self):
        ws = FakeWebSocket()
        provider = CosyVoiceTtsProvider(
            {"endpoint": "wss://tts.example.test/inference", "model": "test-cosyvoice",
             "voice": "test-voice", "timeout_s": 1},
            {"DASHSCOPE_API_KEY": "test-key"}, FakeSession(websocket=ws),
        )
        ws.provider = provider
        return provider, ws

    async def test_task_lifecycle_and_fragmented_pcm_samples(self):
        provider, ws = self.provider()
        await provider.start()
        await provider.append_text("你好。")
        await provider.append_text("第二句。")
        await provider.finish()
        ws.event("task-failed", task_id="other-task", error_message="irrelevant")
        ws.binary(b"\x01")
        ws.binary(b"\x00\x02\x00")
        ws.event("task-finished")
        self.assertEqual(b"".join([chunk async for chunk in provider.audio()]), b"\x01\x00\x02\x00")
        self.assertTrue(ws.closed)
        self.assertEqual([item["header"]["action"] for item in ws.sent],
                         ["run-task", "continue-task", "continue-task", "finish-task"])
        self.assertEqual({item["header"]["task_id"] for item in ws.sent}, {provider.task_id})
        parameters = ws.sent[0]["payload"]["parameters"]
        self.assertEqual(parameters["format"], "pcm")
        self.assertEqual(parameters["sample_rate"], 24000)
        self.assertIs(parameters["enable_ssml"], False)

    async def test_task_failure_is_sanitized_and_closes_socket(self):
        provider, ws = self.provider()
        await provider.start()
        await provider.finish()
        ws.event("task-failed", error_message="secret-token")
        with self.assertRaisesRegex(RuntimeError, "synthesis failed") as error:
            _ = [chunk async for chunk in provider.audio()]
        self.assertNotIn("secret-token", str(error.exception))
        self.assertTrue(ws.closed)

    async def test_audio_cancellation_closes_socket(self):
        provider, ws = self.provider()
        await provider.start()
        task = asyncio.create_task(anext(provider.audio()))
        await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(ws.closed)
        await provider.cancel()
        await provider.close()

    async def test_incomplete_pcm_tail_fails(self):
        provider, ws = self.provider()
        await provider.start()
        await provider.finish()
        ws.binary(b"\x01")
        ws.event("task-finished")
        with self.assertRaisesRegex(RuntimeError, "incomplete PCM"):
            _ = [chunk async for chunk in provider.audio()]
        self.assertTrue(ws.closed)

    async def test_audio_is_pull_based_without_prefetch_when_consumer_pauses(self):
        provider, ws = self.provider()
        await provider.start()
        await provider.finish()
        ws.binary(b"\x01\x00" * 480)
        ws.binary(b"\x02\x00" * 480)
        ws.event("task-finished")
        reader = provider.audio()
        self.assertEqual(await anext(reader), b"\x01\x00" * 480)
        # 播放忙碌时，不允许后台任务预读下一个云端数据包。
        # 消费者暂停会直接向上游 WebSocket 读取者传递背压。
        await asyncio.sleep(0)
        self.assertEqual(ws.messages.qsize(), 2)
        self.assertEqual([chunk async for chunk in reader], [b"\x02\x00" * 480])


if __name__ == "__main__":
    unittest.main()
