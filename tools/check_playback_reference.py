"""使用有界合成 DMA 事件检查真实播放参考模块。

检查数值、连续性标记、内存类别和降级行为。
不执行真实中断、DMA 时序测量、声学播放或硬件内存测量。
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
    with tempfile.TemporaryDirectory(prefix="playback-reference-") as temporary:
        executable = Path(temporary) / ("playback_reference.exe" if os.name == "nt" else "playback_reference")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/audio_driver_stubs"),
            "-I", str(ROOT / "main/audio"), str(ROOT / "main/audio/playback_reference.cpp"),
            str(ROOT / "tools/tests/playback_reference.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
