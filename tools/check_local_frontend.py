"""使用有界的电脑端 RTOS/AFE 替代实现运行真实 VAD/AEC 前端。

检查单麦基线、成对 SR/AEC 观察、成对全双工插话前端的配置和生命周期，
以及资源顺序、通道约定、核心绑定、有限协作等待、
追赶处理期间的 PCM 身份和错误处理。
不运行 VADNet/AEC DSP，也不证明声学准确性或 ESP32 实际调度性能。
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
    parser.add_argument("--aec", action="store_true", help="run only the paired SR/AEC and FD/AEC configurations")
    args = parser.parse_args()
    environment = os.environ.copy()
    environment["PATH"] = str(Path(args.compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="local-frontend-") as temporary:
        modes = ["sr_aec", "fd_aec"] if args.aec else ["mono", "sr_aec", "fd_aec"]
        for mode in modes:
            executable = Path(temporary) / ("local_frontend_" + mode + (".exe" if os.name == "nt" else ""))
            command = [
                args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread",
                "-static-libgcc", "-static-libstdc++",
                "-I", str(ROOT / "tools/tests/local_frontend_stubs"),
                "-I", str(ROOT / "main"),
                "-I", str(ROOT / "main/audio"),
            ]
            if mode != "mono":
                command.append("-DCONFIG_VOICE_AEC_OBSERVATION_ENABLED=1")
            if mode == "fd_aec":
                command.append("-DCONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED=1")
            command.extend([
                str(ROOT / "main/audio/local_audio_frontend.cpp"),
                str(ROOT / "tools/tests/local_frontend_lifecycle.cpp"), "-o", str(executable),
            ])
            subprocess.run(command, check=True, env=environment, timeout=60)
            subprocess.run([str(executable)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
