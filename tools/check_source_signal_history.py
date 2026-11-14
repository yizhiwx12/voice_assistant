"""在电脑端检查有界原始/直流/参考统计的发布和过期。

测试真实的纯数值历史，不验证 AEC DSP，
也不验证 AFE 输入和输出采样之间的声学校准对齐。
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
    with tempfile.TemporaryDirectory(prefix="source-signal-history-") as temporary:
        executable = Path(temporary) / ("source_signal_history.exe" if os.name == "nt" else "source_signal_history")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main/audio"),
            str(ROOT / "tools/tests/source_signal_history.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
