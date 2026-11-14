"""用确定性的调度替代实现检查 CPU0 音频空闲守卫。

检查生命周期、空闲反馈和有限等待，不衡量 ESP32 调度性能或声学表现。
固件构建和设备运行仍由用户执行。
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
    with tempfile.TemporaryDirectory(prefix="audio-idle-guard-") as temporary:
        executable = Path(temporary) / ("audio_idle_guard" + (".exe" if os.name == "nt" else ""))
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/audio_idle_guard_stubs"),
            "-I", str(ROOT / "main"),
            str(ROOT / "main/app/audio_idle_guard.cpp"),
            str(ROOT / "tools/tests/audio_idle_guard.cpp"),
            "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=10)
        subprocess.run([str(executable), "registration_failure"], check=True, env=environment, timeout=10)


if __name__ == "__main__":
    main()
