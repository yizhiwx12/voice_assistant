"""使用最小的电脑端 FreeRTOS 声明检查真实界面文本存储。

检查有界长回答保留、UTF-8 截断、诊断快照生命周期、模式和连接状态、
按采样游标驱动的字幕翻页，以及结合小型 LVGL 几何桩的真实 render() 和共用翻页路径。
真实字体度量、像素布局和 RTOS 并发仍需设备测试；本检查不使用硬件。
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("g++") or "C:/mingw64/bin/g++.exe")
    args = parser.parse_args()
    environment = os.environ.copy()
    environment["PATH"] = str(Path(args.compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="voice-ui-text-") as temporary:
        executable = Path(temporary) / ("ui_text_status.exe" if os.name == "nt" else "ui_text_status")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "tools/tests/ui_text_stubs"),
            "-I", str(ROOT / "main"), "-I", str(ROOT / "main/app"),
            str(ROOT / "tools/tests/ui_text_status.cpp"),
            "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)
        render_executable = Path(temporary) / ("ui_render.exe" if os.name == "nt" else "ui_render")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/ui_render_stubs"),
            "-I", str(ROOT / "tools/tests/ui_text_stubs"),
            "-I", str(ROOT / "tools/tests/firmware_stubs"),
            "-I", str(ROOT / "main"),
            str(ROOT / "main/display/voice_assistant_ui.cpp"),
            str(ROOT / "tools/tests/ui_render.cpp"), "-o", str(render_executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(render_executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
