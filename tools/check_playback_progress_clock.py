"""无需 IDF 或硬件，编译无锁的 DMA 身份时钟。"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    compiler = shutil.which("g++") or "C:/mingw64/bin/g++.exe"
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="playback-progress-") as d:
        exe = Path(d) / "clock.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-I", str(ROOT / "tools/tests/audio_driver_stubs"),
                        "-I", str(ROOT / "main/audio"),
                        str(ROOT / "tools/tests/playback_progress_clock.cpp"), "-o", str(exe)],
                       check=True, env=env, timeout=60)
        subprocess.run([str(exe)], check=True, env=env, timeout=15)
    print("Playback progress clock: DMA identity, underrun/drain, partial tail, stale serial, mute/race failure, uint64 and geometry checks passed")

if __name__ == "__main__":
    main()
