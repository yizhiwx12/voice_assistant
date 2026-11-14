"""入口行为回归：同一配置快照、空识别和空回答；不监听端口或访问云端。"""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

import voice_server
from core.config import EXAMPLE_CONFIG_PATH, load_voice_config, select_profile
from transport.app_state import PROVIDER_HTTP


class RealtimeHttpConsistencyTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        config = load_voice_config(EXAMPLE_CONFIG_PATH, environ={})
        selection = select_profile(config, profile="realtime_default", mode="realtime")
        app = voice_server.create_realtime_app(config, {}, selection)
        app[PROVIDER_HTTP] = object()
        self.handler = next(
            route.handler for route in app.router.routes()
            if route.method == "POST" and route.resource.canonical == "/audio"
        )
        self.request = SimpleNamespace(
            content_length=640, read=AsyncMock(return_value=bytes(640)), app=app,
        )

    async def test_whitespace_recognition_uses_retry_prompt_without_calling_llm(self):
        with patch("providers.asr.qwen_batch.QwenBatchProvider.recognize_wav_async",
                   AsyncMock(return_value=" \t\r\n ")), \
             patch("providers.llm.chat_completions.ChatCompletionsProvider.generate_async",
                   AsyncMock()) as llm, \
             patch.object(voice_server, "_save_legacy_audio") as save_audio:
            response = await self.handler(self.request)
        self.assertEqual(response.status, 200)
        self.assertEqual(response.text, "未识别到语音\n请靠近麦克风后重试。")
        llm.assert_not_awaited()
        save_audio.assert_called_once_with(bytes(640))

    async def test_empty_or_whitespace_answer_is_reported_as_upstream_failure(self):
        for answer in ("", " \r\n\t "):
            with self.subTest(answer=repr(answer)), \
                 patch("providers.asr.qwen_batch.QwenBatchProvider.recognize_wav_async",
                       AsyncMock(return_value="  你好\r\n  ")), \
                 patch("providers.llm.chat_completions.ChatCompletionsProvider.generate_async",
                       AsyncMock(return_value=answer)) as llm, \
                 patch.object(voice_server, "_save_legacy_audio"):
                response = await self.handler(self.request)
                self.assertEqual(response.status, 502)
                self.assertEqual(response.text, "DeepSeek request failed")
                self.assertEqual(llm.await_args.args[0][1]["content"], "你好")


class StartupSnapshotTests(unittest.TestCase):
    def test_realtime_start_loads_business_file_once_and_uses_validated_snapshot(self):
        data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
        credentials = {
            "VOLCENGINE_ASR_API_KEY": "synthetic-asr",
            "VOLCENGINE_TTS_API_KEY": "synthetic-tts",
            "DEEPSEEK_API_KEY": "synthetic-llm",
            "DEEPSEEK_MODEL": "synthetic-model-override",
        }
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "voice_config.json"
            secrets_path = Path(directory) / "server_secrets.json"
            config_path.write_text(json.dumps(data), encoding="utf-8")
            secrets_path.write_text(json.dumps(credentials), encoding="utf-8")

            def load_then_edit(*args, **kwargs):
                snapshot = load_voice_config(*args, **kwargs)
                # 模拟用户在启动过程中保存下一版配置；当前实例必须继续使用已校验快照。
                config_path.write_text("{}", encoding="utf-8")
                return snapshot

            app = object()
            diagnostic_handler = Mock()
            with patch.dict(os.environ, {}, clear=True), \
                 patch.object(voice_server, "load_voice_config", side_effect=load_then_edit) as loader, \
                 patch.object(voice_server, "create_realtime_app", return_value=app) as create_app, \
                 patch.object(voice_server, "_configure_realtime_logging", return_value=diagnostic_handler), \
                 patch("aiohttp.web.run_app") as runtime, \
                 contextlib.redirect_stdout(io.StringIO()) as stdout:
                result = voice_server.main([
                    "--config", str(config_path), "--secrets", str(secrets_path),
                    "--mode", "realtime", "--profile", "realtime_default",
                ])

        self.assertEqual(result, 0)
        loader.assert_called_once_with(config_path, environ={})
        create_app.assert_called_once()
        config, resolved_credentials, selection = create_app.call_args.args
        self.assertIs(selection.llm, config.models["llm"]["deepseek_default"])
        self.assertEqual(selection.llm.options["model"], "synthetic-model-override")
        self.assertEqual(resolved_credentials["DEEPSEEK_MODEL"], "synthetic-model-override")
        self.assertIn('"model": "synthetic-model-override"', stdout.getvalue())
        runtime.assert_called_once_with(app, host="0.0.0.0", port=8000, access_log=None)
        diagnostic_handler.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
