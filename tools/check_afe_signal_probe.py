"""检查合成 AFE 探测信号的生成、资源归属、节奏和失败路径。

电脑端使用替代前端，验证有界测试驱动和真实确认观察器。
此检查不运行 ESP-SR 的 DSP，也不验证声学回声抑制效果。
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
    with tempfile.TemporaryDirectory(prefix="afe-signal-probe-") as temporary:
        executable = Path(temporary) / ("afe_signal_probe.exe" if os.name == "nt" else "afe_signal_probe")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++",
            "-DCONFIG_VOICE_AEC_OBSERVATION_ENABLED=1",
            "-DCONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED=1",
            "-I", str(ROOT / "tools/tests/local_frontend_stubs"),
            "-I", str(ROOT / "main"), "-I", str(ROOT / "main/audio"),
            str(ROOT / "tools/tests/afe_signal_probe.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
