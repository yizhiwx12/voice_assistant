import io
import json
import unittest
from urllib.error import HTTPError, URLError

from providers.asr.qwen_batch import QwenBatchProvider
from providers.llm.chat_completions import ChatCompletionsProvider


class FakeResponse:
    def __init__(self, value):
        self.data = json.dumps(value, ensure_ascii=False).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def read(self):
        return self.data


class ProviderAdapterTests(unittest.TestCase):
    def test_qwen_uses_explicit_endpoint_and_referenced_credentials(self):
        captured = {}

        def opener(request, timeout):
            captured["request"] = request
            captured["timeout"] = timeout
            return FakeResponse({"output": {"text": "  完整\n识别结果  "}})

        provider = QwenBatchProvider(
            {
                "endpoint": "https://asr.example.test/generation",
                "model": "configured-asr",
                "credential_ref": "OTHER_ASR_KEY",
                "language_hints": ("en",),
                "timeout_s": 10,
            },
            {"OTHER_ASR_KEY": "test-asr-key"},
            opener=opener,
        )
        self.assertEqual(provider.recognize_wav(b"complete-wav"), "完整 识别结果")
        request = captured["request"]
        self.assertEqual(request.full_url, "https://asr.example.test/generation")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-asr-key")
        payload = json.loads(request.data)
        self.assertEqual(payload["model"], "configured-asr")
        self.assertEqual(payload["parameters"]["language_hints"], ["en"])
        self.assertEqual(captured["timeout"], 10)

    def test_chat_adapter_keeps_full_answer_and_excludes_reasoning_content(self):
        captured = {}
        full_answer = "很长的回答。" * 200 + "\n第二段"

        def opener(request, timeout):
            captured["request"] = request
            return FakeResponse({"choices": [{"message": {
                "content": full_answer,
                "reasoning_content": "must not become answer",
            }}]})

        provider = ChatCompletionsProvider(
            {
                "base_url": "https://llm.example.test/compatible-mode/v1/",
                "model": "configured-qwen",
                "credential_ref": "QWEN_KEY",
                "max_tokens": 1024,
                "temperature": 0.2,
            },
            {"QWEN_KEY": "test-llm-key"},
            opener=opener,
        )
        answer = provider.generate([{"role": "user", "content": "问题"}])
        self.assertEqual(answer, full_answer)
        request = captured["request"]
        self.assertEqual(request.full_url,
                         "https://llm.example.test/compatible-mode/v1/chat/completions")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-llm-key")
        payload = json.loads(request.data)
        self.assertEqual(payload["model"], "configured-qwen")
        self.assertEqual(payload["max_tokens"], 1024)
        self.assertEqual(payload["temperature"], 0.2)
        self.assertIs(payload["stream"], False)

    def test_upstream_http_error_does_not_expose_response_body_or_key(self):
        secret = "never-print-this-key"

        def opener(request, timeout):
            raise HTTPError(request.full_url, 401, "Unauthorized", {},
                            io.BytesIO(secret.encode()))

        provider = ChatCompletionsProvider({}, {"DEEPSEEK_API_KEY": secret}, opener)
        with self.assertRaisesRegex(RuntimeError, "HTTP 401") as error:
            provider.generate([{"role": "user", "content": "问题"}])
        self.assertNotIn(secret, str(error.exception))

    def test_upstream_connection_error_does_not_expose_reason(self):
        secret = "private-upstream-url-token"

        def opener(request, timeout):
            raise URLError(secret)

        provider = QwenBatchProvider(
            {}, {"DASHSCOPE_API_KEY": "key", "DASHSCOPE_WORKSPACE_ID": "workspace"}, opener
        )
        with self.assertRaisesRegex(RuntimeError, "connection failed") as error:
            provider.recognize_wav(b"wav")
        self.assertNotIn(secret, str(error.exception))

    def test_workspace_cannot_change_default_hostname(self):
        provider = QwenBatchProvider(
            {},
            {"DASHSCOPE_API_KEY": "key", "DASHSCOPE_WORKSPACE_ID": "bad.host/path"},
            opener=lambda *args, **kwargs: self.fail("must validate before networking"),
        )
        with self.assertRaisesRegex(RuntimeError, "workspace ID"):
            provider.recognize_wav(b"wav")

    def test_reasoning_only_answer_is_rejected(self):
        provider = ChatCompletionsProvider(
            {}, {"DEEPSEEK_API_KEY": "key"},
            opener=lambda *args, **kwargs: FakeResponse({"choices": [{"message": {
                "content": None, "reasoning_content": "thinking without final answer",
            }}]}),
        )
        with self.assertRaisesRegex(RuntimeError, "non-text content"):
            provider.generate([{"role": "user", "content": "问题"}])

    def test_missing_selected_credential_fails_before_networking(self):
        provider = ChatCompletionsProvider(
            {"credential_ref": "SELECTED_MODEL_KEY"}, {},
            opener=lambda *args, **kwargs: self.fail("must validate before networking"),
        )
        with self.assertRaisesRegex(RuntimeError, "SELECTED_MODEL_KEY"):
            provider.generate([{"role": "user", "content": "问题"}])


if __name__ == "__main__":
    unittest.main()
