"""无需硬件，检查真实 Wi-Fi 重试、截止时间和事件顺序。"""

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
    with tempfile.TemporaryDirectory(prefix="voice-wifi-") as temporary:
        executable = Path(temporary) / ("wifi_station.exe" if os.name == "nt" else "wifi_station")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/wifi_stubs"), "-I", str(ROOT / "main"),
            str(ROOT / "main/network/wifi_station.cpp"),
            str(ROOT / "tools/tests/wifi_station.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
