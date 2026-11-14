"""在不依赖 RTOS、网络或硬件的情况下检查自动语音回合采样边界。

检查真实代码中的调用者自有 PCM 环、启动静音门槛、预录、端点、时长上限、
显式结束、启动缓存和旧历史隔离。
不测量麦克风声学效果或真实云端握手延迟。
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
    with tempfile.TemporaryDirectory(prefix="automatic-utterance-") as temporary:
        executable = Path(temporary) / ("automatic_utterance.exe" if os.name == "nt" else "automatic_utterance")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main"),
            str(ROOT / "tools/tests/automatic_utterance.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
