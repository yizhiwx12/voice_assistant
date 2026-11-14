"""检查真实时钟估计器对中断抖动与持续漂移的处理。"""
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
    with tempfile.TemporaryDirectory(prefix="reference-clock-jitter-") as temporary:
        executable = Path(temporary) / ("reference_clock_jitter.exe" if os.name == "nt" else "reference_clock_jitter")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-ffp-contract=off",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "tools/tests/audio_driver_stubs"),
            "-I", str(ROOT / "main/audio"), str(ROOT / "main/audio/reference_timeline.cpp"),
            str(ROOT / "tools/tests/reference_clock_jitter.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=20)


if __name__ == "__main__":
    main()
