"""检查实际使用的有界声学延迟相关性观察器。

使用合成数组验证延迟方向，以及对增益、直流偏移和极性的容忍度；
确认它会拒绝过弱、不完整、含糊或位于范围边缘的证据。
此检查不测量真实扬声器/麦克风延迟，也不验证房间中的双讲表现。
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
    with tempfile.TemporaryDirectory(prefix="acoustic-delay-probe-") as temporary:
        executable = Path(temporary) / ("acoustic_delay_probe.exe" if os.name == "nt" else "acoustic_delay_probe")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main/audio"),
            str(ROOT / "main/audio/acoustic_delay_probe.cpp"),
            str(ROOT / "tools/tests/acoustic_delay_probe.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
