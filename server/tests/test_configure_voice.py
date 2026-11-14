"""新手从真实向导入口完成配置；只在临时目录保存，不访问云或本地凭证。"""

import contextlib
import copy
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import configure_voice
from core.config import ConfigurationError, EXAMPLE_CONFIG_PATH, load_voice_config, select_profile, validate_selection_credentials
from core.config_editor import read_document, save_document, validate_document


class ConfigurationEditorTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "config" / "voice_config.json"
        self.secrets = self.path.with_name("server_secrets.json")
        self.data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
        self.environment = patch.dict(os.environ, {}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def invoke(self, answers=(), *flags):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch("builtins.input", side_effect=answers), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = configure_voice.main(["--config", str(self.path), "--secrets", str(self.secrets), *flags])
        return code, stdout.getvalue(), stderr.getvalue()

    def test_show_is_read_only_and_does_not_read_credentials(self):
        with patch.object(configure_voice, "load_secrets", side_effect=AssertionError("no secrets")):
            code, output, error = self.invoke((), "--show")
        self.assertEqual((code, error), (0, ""))
        self.assertIn("realtime_default", output)
        self.assertIn("doubao_2", output)
        self.assertFalse(self.path.exists())
        self.assertFalse(self.secrets.exists())

    def test_news_menu_enables_independent_feature_without_changing_models(self):
        code, output, error = self.invoke(["8", "1", "true", ""])
        self.assertEqual((code, error), (0, ""))
        saved = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertTrue(saved["news"]["enabled"])
        self.assertEqual(saved["profiles"], self.data["profiles"])
        self.assertEqual(saved["llm"], self.data["llm"])
        self.assertFalse(self.secrets.exists())
        self.assertIn("配置已保存", output)

    def test_enabled_news_key_is_available_in_private_credentials_menu(self):
        self.data["news"]["enabled"] = True
        self.path.parent.mkdir()
        self.path.write_text(json.dumps(self.data), encoding="utf-8")
        with patch.object(configure_voice.sys.stdin, "isatty", return_value=True), \
                patch.object(configure_voice.getpass, "getpass", return_value="private-showapi-key"):
            code, output, error = self.invoke(["5", "3", ""])
        self.assertEqual((code, error), (0, ""))
        saved = json.loads(self.secrets.read_text(encoding="utf-8"))
        self.assertEqual(saved["SHOWAPI_APP_KEY"], "private-showapi-key")
        self.assertNotIn("private-showapi-key", output + error)

    def test_asr_switch_updates_actual_device_profile_and_streaming_requirement(self):
        code, output, error = self.invoke(["1", "1", ""])
        self.assertEqual((code, error), (0, ""))
        selection = select_profile(load_voice_config(self.path, environ={}), profile="realtime_default", mode="realtime")
        self.assertEqual(selection.asr.name, "qwen_existing")
        self.assertFalse(selection.require_streaming_asr)
        self.assertEqual(selection.tts.name, "volc_default")
        self.assertIn("重启", output)
        self.assertFalse(self.secrets.exists())

    def test_invalid_provider_selection_is_not_saved(self):
        code, output, error = self.invoke(["2", "2", ""])
        self.assertEqual((code, error), (0, ""))
        self.assertIn("placeholder", output)
        self.assertFalse(self.path.exists())

    def test_model_edit_removes_legacy_override_and_keeps_other_configuration(self):
        code, _, error = self.invoke(["4", "2", "1", "1", "my-chat-model", ""])
        self.assertEqual((code, error), (0, ""))
        saved = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(saved["llm"]["deepseek_default"]["model"], "my-chat-model")
        self.assertNotIn("model_ref", saved["llm"]["deepseek_default"])
        self.assertEqual(saved["transport"], self.data["transport"])
        self.assertEqual(saved["asr"], self.data["asr"])

    def test_key_edit_preserves_unselected_credentials_and_does_not_echo_values(self):
        self.secrets.parent.mkdir(parents=True)
        self.secrets.write_text('{"CUSTOM_UNUSED":"keep-private"}', encoding="utf-8")
        with patch.object(configure_voice.sys.stdin, "isatty", return_value=True), patch.object(configure_voice.getpass, "getpass", return_value="private-new-key"):
            code, output, error = self.invoke(["5", "1", ""])
        self.assertEqual((code, error), (0, ""))
        saved = json.loads(self.secrets.read_text(encoding="utf-8"))
        self.assertEqual(saved["CUSTOM_UNUSED"], "keep-private")
        self.assertIn("private-new-key", saved.values())
        self.assertNotIn("private-new-key", output)
        self.assertNotIn("keep-private", output)
        self.assertFalse(self.path.exists())
        self.assertEqual(len(list((self.path.parent / "backups").glob("server_secrets.*.json"))), 1)

    def test_atomic_save_retains_exact_original_backup_and_unknown_fields(self):
        original, backup = save_document(self.path, {"note": "中文"}, original=None)
        self.assertIsNone(backup)
        data, loaded = read_document(self.path)
        self.assertEqual(original, loaded)
        data["other"] = 42
        new, backup = save_document(self.path, data, original=loaded)
        self.assertEqual(backup.read_bytes(), original)
        self.assertEqual(self.path.read_bytes(), new)
        self.assertEqual(json.loads(new), {"note": "中文", "other": 42})

    def test_external_edit_is_preserved_and_never_overwritten(self):
        original, _ = save_document(self.path, {"version": 1}, original=None)
        self.path.write_text('{"version":2}', encoding="utf-8")
        with self.assertRaisesRegex(ConfigurationError, "其他窗口"):
            save_document(self.path, {"version": 3}, original=original)
        self.assertEqual(json.loads(self.path.read_text()), {"version": 2})

    def test_edit_during_temp_file_creation_is_preserved_with_exact_original_backup(self):
        original, _ = save_document(self.path, {"version": 1}, original=None)
        create_temp = tempfile.mkstemp

        def edit_then_create(*args, **kwargs):
            self.path.write_text('{"version":2}', encoding="utf-8")
            return create_temp(*args, **kwargs)

        with patch("core.config_editor.tempfile.mkstemp", side_effect=edit_then_create):
            with self.assertRaisesRegex(ConfigurationError, "其他窗口"):
                save_document(self.path, {"version": 3}, original=original)
        self.assertEqual(json.loads(self.path.read_text()), {"version": 2})
        self.assertEqual(next((self.path.parent / "backups").glob("*.json")).read_bytes(), original)
        self.assertFalse(list(self.path.parent.glob(".voice-config-*")))

    def test_missing_profile_field_is_reported_without_traceback_or_rewriting(self):
        del self.data["profiles"]["realtime_default"]["llm"]
        self.path.parent.mkdir()
        self.path.write_text(json.dumps(self.data), encoding="utf-8")
        original = self.path.read_bytes()
        code, output, error = self.invoke((), "--show")
        self.assertEqual(code, 2)
        self.assertIn("llm", error)
        self.assertNotIn("Traceback", output + error)
        self.assertEqual(self.path.read_bytes(), original)

    def test_invalid_url_port_fails_before_saving(self):
        for port in ("not-a-port", "0", "65536"):
            with self.subTest(port=port):
                self.data["llm"]["deepseek_default"]["base_url"] = f"https://example.com:{port}/v1"
                with self.assertRaisesRegex(ConfigurationError, "port"):
                    validate_document(self.data, profile="realtime_default")

    def test_replace_failure_retains_previous_config_and_cleans_temporary_file(self):
        original, _ = save_document(self.path, {"version": 1}, original=None)
        with patch("core.config_editor.os.replace", side_effect=OSError("simulated replace failure")):
            with self.assertRaises(OSError):
                save_document(self.path, {"version": 2}, original=original)
        self.assertEqual(self.path.read_bytes(), original)
        self.assertFalse(list(self.path.parent.glob(".voice-config-*")))

    def test_check_missing_custom_config_does_not_silently_check_default(self):
        with patch.object(configure_voice, "load_secrets", side_effect=AssertionError("no secrets")):
            code, _, error = self.invoke((), "--check")
        self.assertEqual(code, 2)
        self.assertIn("尚未保存", error)

    def test_placeholder_credentials_are_rejected_without_disclosure(self):
        selection = validate_document(self.data, profile="realtime_default")
        secrets = {"VOLCENGINE_ASR_API_KEY": "paste-your-volcengine-asr-api-key-here",
                   "VOLCENGINE_TTS_API_KEY": "test-tts", "DEEPSEEK_API_KEY": "test-llm"}
        with self.assertRaises(ConfigurationError) as result:
            validate_selection_credentials(selection, secrets)
        self.assertIn("VOLCENGINE_ASR_API_KEY", str(result.exception))
        self.assertNotIn("paste-your-", str(result.exception))

    def test_doubao_mistakes_fail_before_provider_construction(self):
        for key, bad in (("chunk_duration_ms", 20), ("enable_ddc", "true"), ("enable_nonstream", 1),
                         ("end_window_size", 50), ("max_utterance_s", True), ("model", "wrong"),
                         ("resource_id", "wrong")):
            with self.subTest(key=key):
                data = copy.deepcopy(self.data)
                data["asr"]["doubao_2"][key] = bad
                with self.assertRaisesRegex(ConfigurationError, key):
                    validate_document(data, profile="realtime_default")

    def test_max_tokens_has_same_range_at_load_and_selection(self):
        self.data["llm"]["deepseek_default"]["max_tokens"] = 65536
        selected = validate_document(self.data, profile="realtime_default")
        self.assertEqual(selected.llm.options["max_tokens"], 65536)

    def test_bad_encoding_fails_without_displaying_document_content(self):
        self.path.parent.mkdir()
        self.path.write_bytes(b'\xffprivate-value')
        with self.assertRaises(ConfigurationError) as result:
            load_voice_config(self.path)
        self.assertIn("UTF-8", str(result.exception))
        self.assertNotIn("private-value", str(result.exception))


if __name__ == "__main__":
    unittest.main()
