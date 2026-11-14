"""通过合成 DMA 事件检查真实 ICS-43434 接收时序和生命周期。

检查消费者控制的延迟启用 RX、可重试生命周期、启用前日志、
一致的 64 位启动/首次读取/首次溢出诊断、读取阶段和核心、驱动读取与回调延迟，
以及包含任务发布被抢占情况的有界首次溢出进度快照、采样/时间锚点、
已消费读取范围、溢出失效及可恢复关闭。
不测量真实 DMA 抖动、麦克风声学效果或物理 RX/TX 时钟偏差。
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
    with tempfile.TemporaryDirectory(prefix="microphone-timing-") as temporary:
        executable = Path(temporary) / ("microphone_timing.exe" if os.name == "nt" else "microphone_timing")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/microphone_timing_stubs"),
            "-I", str(ROOT / "main/audio"), str(ROOT / "main/audio/ics43434_microphone.cpp"),
            str(ROOT / "tools/tests/microphone_timing.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
