"""在电脑端使用真实按键手势检查应用启动和主循环。

桩实现替代依赖边界和 GPIO 触点，不替代应用事件分派。
默认关闭串口支持，也不提供串口头文件；不使用硬件。
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
    with tempfile.TemporaryDirectory(prefix="voice-device-controls-") as temporary:
        executable = Path(temporary) / ("device_controls.exe" if os.name == "nt" else "device_controls")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/device_controls_stubs"),
            "-I", str(ROOT / "tools/tests/local_buttons_stubs"),
            "-I", str(ROOT / "tools/tests/ui_render_stubs"),
            "-I", str(ROOT / "tools/tests/ui_text_stubs"), "-I", str(ROOT / "main"),
            str(ROOT / "main/app/voice_assistant_app.cpp"),
            str(ROOT / "main/app/local_buttons.cpp"),
            str(ROOT / "tools/tests/device_controls.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)
        invalid_executable = Path(temporary) / ("device_controls_invalid.exe" if os.name == "nt" else "device_controls_invalid")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-DDEVICE_TEST_INVALID_BUTTON_GPIO",
            "-DCONFIG_VOICE_BUTTON_FIRST_GPIO=4",
            "-I", str(ROOT / "tools/tests/device_controls_stubs"),
            "-I", str(ROOT / "tools/tests/local_buttons_stubs"),
            "-I", str(ROOT / "tools/tests/ui_render_stubs"),
            "-I", str(ROOT / "tools/tests/ui_text_stubs"), "-I", str(ROOT / "main"),
            str(ROOT / "main/app/voice_assistant_app.cpp"),
            str(ROOT / "main/app/local_buttons.cpp"),
            str(ROOT / "tools/tests/device_controls.cpp"), "-o", str(invalid_executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(invalid_executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
