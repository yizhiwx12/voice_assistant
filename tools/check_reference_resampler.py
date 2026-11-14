"""检查真实播放参考重采样器的 24 kHz 到 16 kHz 转换。"""
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
    with tempfile.TemporaryDirectory(prefix="reference-resampler-") as temporary:
        executable = Path(temporary) / ("reference_resampler.exe" if os.name == "nt" else "reference_resampler")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-ffp-contract=off",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main/audio"),
            str(ROOT / "main/audio/reference_resampler.cpp"),
            str(ROOT / "tools/tests/reference_resampler.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
