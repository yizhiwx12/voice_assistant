"""无需硬件，验证真实双按键去抖、手势及 GPIO 配置。

使用 GPIO 桩检查引脚占用、有效电平和失败处理。
此检查不确认底板接线、电气电平、物理抖动或目标设备时序。
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
    with tempfile.TemporaryDirectory(prefix="voice-local-buttons-") as temporary:
        executable = Path(temporary) / ("local_buttons.exe" if os.name == "nt" else "local_buttons")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/local_buttons_stubs"),
            "-I", str(ROOT / "tools/tests/ui_render_stubs"), "-I", str(ROOT / "main"),
            str(ROOT / "main/app/local_buttons.cpp"),
            str(ROOT / "tools/tests/local_buttons.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
