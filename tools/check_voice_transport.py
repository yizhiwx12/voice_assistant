"""使用电脑端 ESP-IDF 模拟接口检查真实 VoiceClient 故障诊断。

检查首个故障保留、数值关闭/错误码、凭证安全日志、
本地停止回调，以及部分发送和持锁发送。
不使用硬件或网络；RTOS 并发和设备吞吐量仍需 ESP32 测试。
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
    with tempfile.TemporaryDirectory(prefix="voice-transport-") as temporary:
        for aec in (0, 1):
            executable = Path(temporary) / (f"voice_transport_{aec}.exe" if os.name == "nt" else f"voice_transport_{aec}")
            subprocess.run([
                args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                f"-DCONFIG_VOICE_AEC_OBSERVATION_ENABLED={aec}",
                "-static-libgcc", "-static-libstdc++", "-I", str(ROOT / "tools/tests/voice_transport_stubs"),
                "-I", str(ROOT / "managed_components/espressif__cjson/cJSON"),
                "-I", str(ROOT / "main"),
                "-I", str(ROOT / "main/network"), str(ROOT / "main/network/voice_client.cpp"),
                str(ROOT / "managed_components/espressif__cjson/cJSON/cJSON.c"),
                str(ROOT / "tools/tests/voice_transport_diagnostics.cpp"), "-o", str(executable),
            ], check=True, env=environment, timeout=60)
            subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
