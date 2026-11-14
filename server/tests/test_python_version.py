"""旧解释器必须在导入业务、读取配置或启动网络前收到明确提示。"""

import builtins
import contextlib
import io
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch


SERVER_DIR = Path(__file__).resolve().parents[1]


class PythonVersionTests(unittest.TestCase):
    def test_old_python_exits_before_config_import_or_io(self):
        original_import = builtins.__import__
        for filename in ("voice_server.py", "configure_voice.py"):
            for version in ((3, 9, 20, "final", 0), (3, 10, 16, "final", 0)):
                with self.subTest(entrypoint=filename, version=version):
                    imported = []

                    def guarded_import(name, *args, **kwargs):
                        imported.append(name)
                        if name.split(".")[0] in {"core", "providers", "transport", "aiohttp"}:
                            raise AssertionError("旧解释器不能加载业务模块")
                        return original_import(name, *args, **kwargs)

                    error, output = io.StringIO(), io.StringIO()
                    entrypoint = str(SERVER_DIR / filename)
                    with patch.object(sys, "version_info", version), \
                         patch("builtins.__import__", side_effect=guarded_import), \
                         patch.object(Path, "read_text", side_effect=AssertionError("不能读取配置或凭证")), \
                         patch.object(Path, "read_bytes", side_effect=AssertionError("不能读取配置或凭证")), \
                         patch("socket.socket", side_effect=AssertionError("不能创建网络连接")), \
                         contextlib.redirect_stderr(error), contextlib.redirect_stdout(output):
                        with self.assertRaises(SystemExit) as caught:
                            runpy.run_path(entrypoint, run_name="__main__")
                    self.assertEqual(caught.exception.code, 2)
                    self.assertIn("Python 3.11 或更高版本", error.getvalue())
                    self.assertIn("推荐 Python 3.12", error.getvalue())
                    self.assertIn(f"当前为 Python {version[0]}.{version[1]}", error.getvalue())
                    self.assertNotIn("Traceback", error.getvalue())
                    self.assertEqual(output.getvalue(), "")
                    self.assertTrue(all(name.split(".")[0] not in {
                        "core", "providers", "transport", "aiohttp"} for name in imported))

    def test_minimum_supported_version_can_show_help_without_config_or_network(self):
        for filename in ("voice_server.py", "configure_voice.py"):
            with self.subTest(entrypoint=filename):
                output, error = io.StringIO(), io.StringIO()
                entrypoint = str(SERVER_DIR / filename)
                with patch.object(sys, "version_info", (3, 11, 0, "final", 0)), \
                     patch.object(sys, "argv", [entrypoint, "--help"]), \
                     patch.object(Path, "read_text", side_effect=AssertionError("帮助不读取配置")), \
                     patch.object(Path, "read_bytes", side_effect=AssertionError("帮助不读取凭证")), \
                     patch("socket.socket", side_effect=AssertionError("帮助不创建网络连接")), \
                     contextlib.redirect_stderr(error), contextlib.redirect_stdout(output):
                    with self.assertRaises(SystemExit) as caught:
                        runpy.run_path(entrypoint, run_name="__main__")
                self.assertEqual(caught.exception.code, 0)
                self.assertIn("--help", output.getvalue())
                self.assertEqual(error.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
