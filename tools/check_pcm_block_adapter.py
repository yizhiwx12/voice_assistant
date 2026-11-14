"""无需 ESP 硬件或模型推理，检查真实固定 PCM 适配器。

验证不同大小 AFE 块与 20 毫秒帧之间的精确采样顺序，
以及显式尾部重置和错误行为。声学效果和 VAD 准确性需要 ESP32-S3
与麦克风验证，本测试不对此作出结论。
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
    with tempfile.TemporaryDirectory(prefix="pcm-adapter-") as temporary:
        executable = Path(temporary) / ("pcm_adapter.exe" if os.name == "nt" else "pcm_adapter")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "main"),
            str(ROOT / "tools/tests/pcm_block_adapter.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
