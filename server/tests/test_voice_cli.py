"""命令行接口约定检查；仅用模拟配置，不访问云端或打开监听端口。"""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import voice_server
from core.config import EXAMPLE_CONFIG_PATH


class VoiceCliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.config_path = Path(self.directory.name) / "voice_config.json"
        self.secret_path = Path(self.directory.name) / "test_secrets.json"
        self.config = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
        self.secrets = {
            "DASHSCOPE_API_KEY": "test-asr-secret-must-not-print",
            "DASHSCOPE_WORKSPACE_ID": "test-workspace",
            "DEEPSEEK_API_KEY": "test-llm-secret-must-not-print",
            "VOLCENGINE_ASR_API_KEY": "test-volc-asr-must-not-print",
            "VOLCENGINE_TTS_API_KEY": "test-volc-tts-must-not-print",
        }
        self.write_files()
        self.environment = patch.dict(os.environ, {}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def write_files(self):
        self.config_path.write_text(json.dumps(self.config), encoding="utf-8")
        self.secret_path.write_text(json.dumps(self.secrets), encoding="utf-8")

    def invoke(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        args = ["--config", str(self.config_path), "--secrets", str(self.secret_path), *arguments]
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = voice_server.main(args)
        output, error = stdout.getvalue(), stderr.getvalue()
        for secret in self.secrets.values():
            # 工作空间是公开标识，但配置选择摘要也无需展示它。
            self.assertNotIn(secret, output + error)
        return code, output, error

    def no_runtime(self):
        stack = contextlib.ExitStack()
        stack.enter_context(patch.object(voice_server, "create_http_server", side_effect=AssertionError("must not bind")))
        stack.enter_context(patch.object(voice_server, "create_provider", side_effect=AssertionError("must not call providers")))
        return stack

    def test_list_options_never_loads_secrets_or_constructs_runtime(self):
        # 即使密钥文件被故意写成错误格式，也不能影响列出配置选项。
        self.secret_path.write_text("not-json-secret-must-not-print", encoding="utf-8")
        with self.no_runtime(), patch.object(voice_server, "load_secrets", side_effect=AssertionError("must not read secrets")):
            code, output, error = self.invoke("--list-options")
        self.assertEqual((code, error), (0, ""))
        options = json.loads(output)
        self.assertEqual(options["active_profile"], "legacy_http")
        self.assertIn("doubao_2", [item["id"] for item in options["models"]["asr"]])
        self.assertNotIn("credential_ref", output)

    def test_check_config_validates_without_binding_or_provider_construction(self):
        with self.no_runtime():
            code, output, error = self.invoke("--check-config")
        self.assertEqual((code, error), (0, ""))
        self.assertIn('"profile": "legacy_http"', output)
        self.assertIn('"id": "qwen_existing"', output)
        self.assertIn("Configuration valid", output)

    def test_llm_override_keeps_asr_and_disabled_tts(self):
        qwen = self.config["llm"]["qwen_default"]
        qwen.update({"base_url": "https://example.com/compatible-mode/v1", "model": "verified-test-qwen"})
        self.write_files()
        with self.no_runtime():
            code, output, error = self.invoke("--llm", "qwen_default", "--check-config")
        self.assertEqual((code, error), (0, ""))
        self.assertIn('"id": "qwen_default"', output)
        self.assertIn('"id": "qwen_existing"', output)
        self.assertIn('"id": "none"', output)

    def test_realtime_independent_tts_override_checks_only_chosen_credentials(self):
        self.config["tts"]["cosyvoice_default"].update({
            "model": "verified-cosyvoice", "voice": "verified-cosyvoice-voice", "endpoint": "wss://example.com/tts",
        })
        del self.secrets["VOLCENGINE_TTS_API_KEY"]
        self.write_files()
        with self.no_runtime():
            code, output, error = self.invoke("--profile", "realtime_default", "--mode", "realtime",
                                              "--tts", "cosyvoice_default", "--check-config")
        self.assertEqual((code, error), (0, ""))
        self.assertIn('"id": "doubao_2"', output)
        self.assertIn('"id": "deepseek_default"', output)
        self.assertIn('"id": "cosyvoice_default"', output)

    def test_unknown_model_and_incompatible_http_tts_fail_before_binding(self):
        for arguments, fragment in ((["--asr", "missing"], "Unknown asr"),
                                    (["--tts", "volc_default"], "two text lines"),
                                    (["--asr", "doubao_2"], "does not support 'http'")):
            with self.subTest(arguments=arguments), self.no_runtime():
                code, output, error = self.invoke(*arguments)
            self.assertEqual((code, output), (2, ""))
            self.assertIn(fragment, error)

    def test_strict_streaming_profile_rejects_qwen_batch_override(self):
        with self.no_runtime():
            code, output, error = self.invoke("--profile", "realtime_default", "--mode", "realtime",
                                              "--asr", "qwen_existing", "--check-config")
        self.assertEqual((code, output), (2, ""))
        self.assertIn("requires implemented streaming ASR", error)

    def test_environment_model_ref_and_key_precedence_do_not_mutate_other_process_settings(self):
        self.secrets["DEEPSEEK_MODEL"] = "file-model"
        self.write_files()
        with patch.dict(os.environ, {"DEEPSEEK_MODEL": "environment-model", "DEEPSEEK_API_KEY": ""}), self.no_runtime():
            code, _, error = self.invoke("--check-config")
            self.assertEqual(os.environ["DEEPSEEK_API_KEY"], "")
        self.assertEqual(code, 2)
        self.assertIn("DEEPSEEK_API_KEY", error)
        with patch.dict(os.environ, {"DEEPSEEK_MODEL": "environment-model"}), self.no_runtime():
            code, output, error = self.invoke("--check-config")
        self.assertEqual((code, error), (0, ""))
        self.assertIn('"model": "environment-model"', output)
        self.assertNotIn("DASHSCOPE_API_KEY", os.environ)

    def test_missing_selected_credentials_and_bad_secret_json_are_redacted(self):
        del self.secrets["DEEPSEEK_API_KEY"]
        self.write_files()
        with self.no_runtime():
            code, output, error = self.invoke("--check-config")
        self.assertEqual((code, output), (2, ""))
        self.assertIn("DEEPSEEK_API_KEY", error)
        self.secret_path.write_text('{"API_KEY":never-print-this-secret}', encoding="utf-8")
        with self.no_runtime():
            code, output, error = self.invoke("--check-config")
        self.assertEqual((code, output), (2, ""))
        self.assertIn("Invalid JSON", error)
        self.assertNotIn("never-print-this-secret", error)

    def test_unknown_cli_flags_and_bad_port_produce_usage_errors(self):
        for arguments in (["--made-up-option"], ["--port", "0"], ["--port", "65536"]):
            with self.subTest(arguments=arguments), self.no_runtime():
                output = io.StringIO()
                with contextlib.redirect_stderr(output), self.assertRaises(SystemExit) as error:
                    voice_server.main(arguments)
                self.assertEqual(error.exception.code, 2)
                self.assertIn("error:", output.getvalue())


if __name__ == "__main__":
    unittest.main()
