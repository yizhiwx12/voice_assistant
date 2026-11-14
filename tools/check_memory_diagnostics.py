"""无需 ESP 硬件，检查真实堆诊断模块。

检查各内存能力对应的空闲值、最低值和最大块报告，以及碎片堆预算拒绝、
首个失败信息归属、并发回调和延迟日志。失败钩子及固定状态的 ESP-IDF
临界区内均不分配内存、不查询堆、不打印日志。电脑端自旋锁桩使用静态存储，
真实模块不依赖 C++ 原子操作始终无锁。
ESP32 的真实堆大小、DMA 和 Wi-Fi 行为仍需设备验证。
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
    with tempfile.TemporaryDirectory(prefix="memory-diagnostics-") as temporary:
        executable = Path(temporary) / ("memory_diagnostics.exe" if os.name == "nt" else "memory_diagnostics")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "tools/tests/memory_diagnostics_stubs"),
            "-I", str(ROOT / "main"), str(ROOT / "main/app/memory_diagnostics.cpp"),
            str(ROOT / "tools/tests/memory_diagnostics.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        scenarios = ("snapshot", "budget", "registration", "first-copy", "long-name", "null-name", "concurrent", "logging")
        for scenario in scenarios:
            subprocess.run([str(executable), scenario], check=True, env=environment, timeout=15)
    print(f"Memory diagnostics: {len(scenarios)} production scenarios passed")


if __name__ == "__main__":
    main()
