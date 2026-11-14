"""使用真实观察器检查有界 D1 候选确认。

输入为处理后 PCM/VAD 测试样本，不依赖 RTOS、网络或硬件。
检查采样边界和生命周期，不验证声学回声抑制。
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
    with tempfile.TemporaryDirectory(prefix="barge-in-observer-") as temporary:
        executable = Path(temporary) / ("barge_in_observer.exe" if os.name == "nt" else "barge_in_observer")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main"),
            str(ROOT / "tools/tests/barge_in_observer.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
