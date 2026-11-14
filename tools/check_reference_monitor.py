"""无需 ESP 硬件，检查真实参考音频重采样、时间线和监测器。

通过合成的已完成 DMA 块验证参考音频交接和显式无效区间。
此检查不验证中断调度、声学延迟、AEC 或插话效果。
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
    with tempfile.TemporaryDirectory(prefix="reference-monitor-") as temporary:
        executable = Path(temporary) / ("reference_monitor.exe" if os.name == "nt" else "reference_monitor")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-ffp-contract=off",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/audio_driver_stubs"),
            "-I", str(ROOT / "main/audio"),
            str(ROOT / "main/audio/reference_resampler.cpp"),
            str(ROOT / "main/audio/reference_timeline.cpp"),
            str(ROOT / "main/audio/acoustic_delay_probe.cpp"),
            str(ROOT / "main/audio/reference_monitor.cpp"),
            str(ROOT / "tools/tests/reference_monitor.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
