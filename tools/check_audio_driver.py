"""通过电脑端 GPIO/I2S 模拟接口检查真实 MAX98357A 驱动的生命周期。

检查资源交接、DMA 重置期间静音、旧 DMA 丢弃和失败清理。
验证的是驱动调用及状态，不验证电平、可闻爆音、真实 DMA 时序或底板接线。
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
    with tempfile.TemporaryDirectory(prefix="audio-driver-") as temporary:
        executable = Path(temporary) / ("audio_lifecycle.exe" if os.name == "nt" else "audio_lifecycle")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "tools/tests/audio_driver_stubs"),
            "-I", str(ROOT / "main/audio"), str(ROOT / "main/audio/max98357a_audio.cpp"),
            str(ROOT / "main/audio/playback_reference.cpp"),
            str(ROOT / "tools/tests/audio_driver_lifecycle.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
