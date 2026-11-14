import copy
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from core.config import (
    EXAMPLE_CONFIG_PATH, ConfigurationError, list_options, load_secrets,
    load_voice_config, news_credential_status, referenced_settings,
    select_profile, validate_selection_credentials,
)
from core.provider_registry import ProviderUnavailableError, create_provider, get_adapter


class VoiceConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "voice_config.json"
        self.data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))

    def load(self, data=None, environ=None):
        self.path.write_text(json.dumps(self.data if data is None else data), encoding="utf-8")
        return load_voice_config(self.path, environ={} if environ is None else environ)

    def test_default_http_profile_needs_only_its_selected_providers(self):
        selected = select_profile(self.load())
        self.assertEqual(selected.profile, "legacy_http")
        self.assertEqual(selected.asr.name, "qwen_existing")
        self.assertIsNone(selected.tts)
        validate_selection_credentials(selected, {
            "DASHSCOPE_API_KEY": "test-asr", "DASHSCOPE_WORKSPACE_ID": "test-workspace",
            "DEEPSEEK_API_KEY": "test-llm",
        })

    def test_llm_override_is_independent_of_asr_and_tts(self):
        self.data["llm"]["qwen_default"]["base_url"] = "https://example.com/compatible-mode/v1"
        self.data["llm"]["qwen_default"]["model"] = "verified-test-model"
        selected = select_profile(self.load(), llm="qwen_default")
        self.assertEqual(selected.asr.name, "qwen_existing")
        self.assertEqual(selected.llm.name, "qwen_default")
        self.assertIsNone(selected.tts)

    def test_snapshot_is_deeply_immutable_and_does_not_reload_file(self):
        self.data["asr"]["qwen_existing"]["language_hints"] = ["zh"]
        snapshot = self.load()
        selected = select_profile(snapshot)
        self.path.write_text("{}", encoding="utf-8")
        self.assertEqual(selected.asr.options["model"], "qwen-audio-3.1-asr-flash")
        with self.assertRaises(TypeError):
            snapshot.profiles["legacy_http"]["llm"] = "different"
        with self.assertRaises(TypeError):
            selected.transport["uplink"]["codec"] = "pcm_s16le"
        with self.assertRaises(TypeError):
            selected.asr.options["language_hints"][0] = "en"

    def test_legacy_model_environment_refs_and_file_secret_precedence(self):
        secret_path = Path(self.directory.name) / "server_secrets.json"
        secret_path.write_text(json.dumps({
            "DASHSCOPE_API_KEY": "file-test", "DEEPSEEK_MODEL": "file-model",
            "CUSTOM_QWEN_KEY": "custom-file", "IGNORED": "should-not-load",
        }), encoding="utf-8")
        self.data["llm"]["qwen_default"]["credential_ref"] = "CUSTOM_QWEN_KEY"
        config = self.load()
        secrets = load_secrets(secret_path, environ={"DASHSCOPE_API_KEY": "env-test"},
                               references=referenced_settings(config))
        self.assertEqual(secrets["DASHSCOPE_API_KEY"], "env-test")
        self.assertEqual(secrets["CUSTOM_QWEN_KEY"], "custom-file")
        self.assertNotIn("IGNORED", secrets)
        updated = self.load(environ=secrets)
        self.assertEqual(updated.models["llm"]["deepseek_default"].options["model"], "file-model")
        with self.assertRaises(TypeError):
            secrets["DASHSCOPE_API_KEY"] = "new"

    def test_empty_environment_credential_never_falls_back_to_file_secret(self):
        path = Path(self.directory.name) / "server_secrets.json"
        path.write_text('{"DASHSCOPE_API_KEY":"file-value"}', encoding="utf-8")
        self.assertEqual(load_secrets(path, environ={"DASHSCOPE_API_KEY": ""})["DASHSCOPE_API_KEY"], "")

    def test_unknown_profiles_choices_and_unsupported_adapter_modes_rejected(self):
        config = self.load()
        with self.assertRaisesRegex(ConfigurationError, "Unknown profile"):
            select_profile(config, profile="missing")
        with self.assertRaisesRegex(ConfigurationError, "Unknown asr"):
            select_profile(config, asr="missing")
        with self.assertRaisesRegex(ProviderUnavailableError, "does not support 'http'"):
            select_profile(config, asr="doubao_2")
        with self.assertRaisesRegex(ConfigurationError, "streaming ASR"):
            select_profile(config, profile="realtime_default", asr="qwen_existing", tts="none")

    def test_http_never_silently_discards_requested_synthesis(self):
        with self.assertRaisesRegex(ConfigurationError, "two text lines"):
            select_profile(self.load(), tts="volc_default")

    def test_list_options_is_offline_and_marks_only_actual_capabilities(self):
        with patch.dict(os.environ, {}, clear=True):
            options = list_options(self.load())
        encoded = json.dumps(options)
        self.assertNotIn("credential_ref", encoded)
        self.assertNotIn("CUSTOM_QWEN_KEY", encoded)
        self.assertEqual(options["profiles"]["legacy_http"]["available_modes"], ["http"])
        # 标准 TTS 音色现在已有可用的项目默认值。列出选项
        # 仍为离线操作，不代表凭证已经配置完成。
        self.assertEqual(options["profiles"]["realtime_default"]["available_modes"], ["realtime"])
        asr = {item["id"]: item for item in options["models"]["asr"]}
        self.assertTrue(asr["doubao_2"]["implemented"])
        self.assertTrue(asr["doubao_2"]["capabilities"]["streaming_input"])
        self.assertTrue(asr["doubao_2"]["capabilities"]["partial_results"])
        self.assertFalse(asr["qwen_existing"]["capabilities"]["partial_results"])
        self.assertTrue(get_adapter("llm", "chat_completions").capabilities.streaming_output)

    def test_news_is_separate_default_off_and_has_no_credential_dependency_until_enabled(self):
        config = self.load()
        self.assertFalse(config.news["enabled"])
        self.assertNotIn("SHOWAPI_APP_KEY", referenced_settings(config))
        selected = select_profile(config, profile="realtime_default", mode="realtime")
        self.assertEqual(news_credential_status(selected, {}), "disabled")
        self.assertEqual(selected.to_dict()["news"], {
            "enabled": False, "provider": "showapi", "channel_name": "国内最新", "max_results": 3})

        self.data["news"]["enabled"] = True
        config = self.load()
        self.assertIn("SHOWAPI_APP_KEY", referenced_settings(config))
        selected = select_profile(config, profile="realtime_default", mode="realtime")
        self.assertEqual(news_credential_status(selected, {}), "missing_credentials")
        for example in ("YOUR_APPKEY", "paste-your-app-key", "<app-key>"):
            with self.subTest(example=example):
                self.assertEqual(news_credential_status(selected, {"SHOWAPI_APP_KEY": example}),
                                 "missing_credentials")
        self.assertEqual(news_credential_status(selected, {"SHOWAPI_APP_KEY": "local-test-key"}), "ready")
        # 新闻是可选能力；开启它也不会要求普通 ASR/LLM/TTS 启动必须有新闻 Key。
        validate_selection_credentials(selected, {
            "VOLCENGINE_ASR_API_KEY": "test-asr", "DEEPSEEK_API_KEY": "test-llm",
            "VOLCENGINE_TTS_API_KEY": "test-tts",
        })

    def test_news_settings_fail_closed_on_wrong_types_and_unbounded_values(self):
        for key, value in (("enabled", 1), ("enabled", "true"), ("max_results", True),
                           ("max_results", 6), ("timeout_s", 0), ("timeout_s", 21),
                           ("credential_ref", "showapi-key"), ("channel_name", 123)):
            with self.subTest(key=key, value=value):
                data = copy.deepcopy(self.data)
                data["news"][key] = value
                with self.assertRaises(ConfigurationError):
                    self.load(data)

    def test_realtime_profiles_accept_implemented_adapters_with_verified_settings(self):
        self.data["tts"]["volc_default"]["voice"] = "verified-test-voice"
        selected = select_profile(self.load(), profile="realtime_default", mode="realtime")
        self.assertEqual((selected.asr.name, selected.llm.name, selected.tts.name),
                         ("doubao_2", "deepseek_default", "volc_default"))
        validate_selection_credentials(selected, {
            "VOLCENGINE_ASR_API_KEY": "test-asr", "DEEPSEEK_API_KEY": "test-llm",
            "VOLCENGINE_TTS_API_KEY": "test-tts",
        })
        self.assertEqual(list_options(self.load())["profiles"]["realtime_default"]["available_modes"], ["realtime"])

    def test_compatible_realtime_profile_allows_batch_but_no_partial_claim(self):
        tts = self.data["tts"]["cosyvoice_default"]
        tts.update({"endpoint": "wss://example.com/tts", "voice": "verified-test-voice", "model": "verified-test-model"})
        selected = select_profile(self.load(), profile="qwen_compatible", mode="realtime")
        self.assertFalse(selected.require_streaming_asr)
        self.assertFalse(selected.to_dict()["asr"]["capabilities"]["partial_results"])

    def test_mambo_segment_limits_are_bounded_integers(self):
        for key in ("first_segment_chars", "segment_chars"):
            for invalid in (True, 7, 201, 40.5, "40"):
                with self.subTest(key=key, invalid=invalid):
                    data = copy.deepcopy(self.data)
                    data["tts"]["mambo_local"][key] = invalid
                    with self.assertRaisesRegex(ConfigurationError, key):
                        self.load(data)
        data = copy.deepcopy(self.data)
        data["tts"]["mambo_local"].update(first_segment_chars=8, segment_chars=200)
        selected = select_profile(self.load(data), profile="mambo_local", mode="realtime")
        self.assertEqual(selected.tts.options["first_segment_chars"], 8)
        self.assertEqual(selected.tts.options["segment_chars"], 200)

    def test_search_flag_is_boolean_and_optional_key_uses_existing_secret_loader(self):
        for invalid in (0, 1, "true", None):
            with self.subTest(value=invalid):
                data = copy.deepcopy(self.data)
                data["llm"]["deepseek_default"]["enable_web_search"] = invalid
                with self.assertRaisesRegex(ConfigurationError, "enable_web_search"):
                    self.load(data)
        path = Path(self.directory.name) / "search_secrets.json"
        path.write_text('{"ANYSEARCH_API_KEY":"test-file-key"}', encoding="utf-8")
        self.assertEqual(load_secrets(path, environ={})["ANYSEARCH_API_KEY"], "test-file-key")
        self.assertEqual(load_secrets(path, environ={"ANYSEARCH_API_KEY": ""})["ANYSEARCH_API_KEY"], "")

    def test_selected_credential_failure_discloses_names_only(self):
        selected = select_profile(self.load())
        with self.assertRaises(ConfigurationError) as context:
            validate_selection_credentials(selected, {"DASHSCOPE_API_KEY": "must-not-appear"})
        self.assertIn("DEEPSEEK_API_KEY", str(context.exception))
        self.assertIn("DASHSCOPE_WORKSPACE_ID", str(context.exception))
        self.assertNotIn("must-not-appear", str(context.exception))
        self.assertNotIn("VOLCENGINE", str(context.exception))

    def test_schema_validation_rejects_typos_invalid_transport_and_inline_secrets(self):
        for field, replacement in (("schema_version", True), ("transport", {}), ("active_profile", "missing")):
            with self.subTest(field=field):
                data = copy.deepcopy(self.data)
                data[field] = replacement
                with self.assertRaises(ConfigurationError):
                    self.load(data)
        data = copy.deepcopy(self.data)
        data["asr"]["qwen_existing"]["api_key"] = "secret-must-not-appear"
        with self.assertRaises(ConfigurationError) as context:
            self.load(data)
        self.assertNotIn("secret-must-not-appear", str(context.exception))
        data = copy.deepcopy(self.data)
        data["transport"]["uplink"]["bitrate_bps"] = "24000"
        with self.assertRaises(ConfigurationError):
            self.load(data)

    def test_placeholder_llm_fails_only_when_selected(self):
        config = self.load()
        select_profile(config)
        with self.assertRaisesRegex(ConfigurationError, "placeholder"):
            select_profile(config, llm="qwen_default")

    def test_registry_factory_uses_selected_model_snapshot(self):
        selected = select_profile(self.load())
        from providers.asr.qwen_batch import QwenBatchProvider
        from providers.llm.chat_completions import ChatCompletionsProvider
        self.assertIsInstance(create_provider("asr", selected.asr, {}), QwenBatchProvider)
        self.assertIsInstance(create_provider("llm", selected.llm, {}), ChatCompletionsProvider)

    def test_invalid_json_error_does_not_quote_secret_line(self):
        path = Path(self.directory.name) / "server_secrets.json"
        path.write_text('{"DASHSCOPE_API_KEY": secret-do-not-print}', encoding="utf-8")
        with self.assertRaises(ConfigurationError) as context:
            load_secrets(path, environ={})
        self.assertNotIn("secret-do-not-print", str(context.exception))


if __name__ == "__main__":
    unittest.main()
