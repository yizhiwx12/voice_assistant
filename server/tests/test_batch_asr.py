import base64
import io
import json
import os
import tempfile
import threading
import unittest
import wave
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import audio_test_server


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False

    def read(self):
        return self.payload


class ConfigTests(unittest.TestCase):
    def test_local_config_preserves_environment_precedence_and_ignores_unknown_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "server_secrets.json"
            config_path.write_text(json.dumps({
                "DASHSCOPE_API_KEY": "file-key",
                "DASHSCOPE_WORKSPACE_ID": "test-workspace",
                "DASHSCOPE_ASR_MODEL": "test-asr-model",
                "UNUSED_PIPELINE_SETTING": "unused",
                "UNUSED_AUDIO_SETTING": "unused",
            }), encoding="utf-8")

            with patch.dict(os.environ, {"DASHSCOPE_API_KEY": "environment-key"}, clear=True):
                audio_test_server.load_local_config(config_path)

                self.assertEqual(os.environ["DASHSCOPE_API_KEY"], "environment-key")
                self.assertEqual(os.environ["DASHSCOPE_WORKSPACE_ID"], "test-workspace")
                self.assertEqual(os.environ["DASHSCOPE_ASR_MODEL"], "test-asr-model")
                self.assertNotIn("UNUSED_PIPELINE_SETTING", os.environ)
                self.assertNotIn("UNUSED_AUDIO_SETTING", os.environ)


class AudioFormatAndProviderTests(unittest.TestCase):
    def test_pcm16_is_wrapped_as_mono_16khz_wav(self):
        pcm = b"\x01\x00\x02\x00" * 80

        with wave.open(io.BytesIO(audio_test_server.pcm16_to_wav(pcm)), "rb") as wav_file:
            self.assertEqual(wav_file.getnchannels(), 1)
            self.assertEqual(wav_file.getsampwidth(), 2)
            self.assertEqual(wav_file.getframerate(), 16000)
            self.assertEqual(wav_file.readframes(wav_file.getnframes()), pcm)

    def test_qwen_batch_request_contains_wav_audio_and_chinese_hint(self):
        captured = {}

        def fake_urlopen(request, timeout):
            captured["request"] = request
            return FakeResponse(json.dumps({"output": {"text": "今天天气如何？"}}).encode())

        pcm = b"\x10\x00\x20\x00" * 20
        wav_data = audio_test_server.pcm16_to_wav(pcm)
        with patch.dict(os.environ, {
            "DASHSCOPE_API_KEY": "test-key",
            "DASHSCOPE_WORKSPACE_ID": "test-workspace",
            "DASHSCOPE_ASR_MODEL": "test-asr-model",
        }, clear=True), patch.object(audio_test_server, "urlopen", side_effect=fake_urlopen):
            transcript = audio_test_server.recognize_qwen(wav_data)

        request = captured["request"]
        payload = json.loads(request.data.decode("utf-8"))
        encoded_audio = payload["input"]["messages"][0]["content"][0]["input_audio"]["data"]
        self.assertEqual(transcript, "今天天气如何？")
        self.assertIn("test-workspace.cn-beijing.maas.aliyuncs.com", request.full_url)
        self.assertEqual(request.get_header("Authorization"), "Bearer test-key")
        self.assertEqual(payload["model"], "test-asr-model")
        self.assertEqual(payload["parameters"]["language_hints"], ["zh"])
        self.assertEqual(base64.b64decode(encoded_audio.split(",", 1)[1]), wav_data)

    def test_deepseek_request_returns_clean_single_line_text(self):
        captured = {}

        def fake_urlopen(request, timeout):
            captured["request"] = request
            return FakeResponse(json.dumps({
                "choices": [{"message": {"content": "  你好，\n今天注意休息。  "}}]
            }, ensure_ascii=False).encode("utf-8"))

        with patch.dict(os.environ, {
            "DEEPSEEK_API_KEY": "test-key",
            "DEEPSEEK_MODEL": "test-chat-model",
        }, clear=True), patch.object(audio_test_server, "urlopen", side_effect=fake_urlopen):
            answer = audio_test_server.ask_deepseek("你好")

        payload = json.loads(captured["request"].data.decode("utf-8"))
        self.assertEqual(answer, "你好， 今天注意休息。")
        self.assertEqual(payload["model"], "test-chat-model")
        self.assertEqual(payload["messages"][1]["content"], "你好")


class HttpEndpointTests(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), audio_test_server.Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_ping_and_audio_endpoint_run_batch_asr_then_deepseek(self):
        with urlopen(f"{self.base_url}/ping", timeout=2) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b"OK")

        pcm = b"\x00\x00" * 100
        request = Request(f"{self.base_url}/audio", data=pcm, method="POST")
        with patch.object(audio_test_server, "recognize_qwen", return_value="测试问题") as asr, \
             patch.object(audio_test_server, "ask_deepseek", return_value="测试回答") as llm, \
             patch.object(Path, "write_bytes") as save_audio:
            with urlopen(request, timeout=2) as response:
                body = response.read().decode("utf-8")

        self.assertEqual(body, "测试问题\n测试回答")
        asr.assert_called_once()
        llm.assert_called_once_with("测试问题")
        save_audio.assert_called_once_with(pcm)

    def test_empty_asr_result_returns_retry_prompt_without_calling_deepseek(self):
        request = Request(f"{self.base_url}/audio", data=b"\x00\x00" * 20, method="POST")
        with patch.object(audio_test_server, "recognize_qwen", return_value=""), \
             patch.object(audio_test_server, "ask_deepseek") as llm, \
             patch.object(Path, "write_bytes"):
            with urlopen(request, timeout=2) as response:
                body = response.read().decode("utf-8")

        self.assertEqual(body, "未识别到语音\n请靠近麦克风后重试。")
        llm.assert_not_called()

    def test_audio_endpoint_rejects_odd_pcm16_body(self):
        request = Request(f"{self.base_url}/audio", data=b"\x00", method="POST")

        with self.assertRaises(HTTPError) as error:
            urlopen(request, timeout=2)

        self.assertEqual(error.exception.code, 400)


if __name__ == "__main__":
    unittest.main()
