"""在不使用硬件或云调用的情况下检查真实对话生命周期。

使用真实 cJSON、控制器和协议代码；外设、连接、时钟及 RTOS 使用桩实现。
另由独立 VoiceClient 检查验证正常关闭帧的回调先进先出顺序。
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
    with tempfile.TemporaryDirectory(prefix="voice-lifecycle-") as temporary:
        executable = Path(temporary) / ("conversation_lifecycle.exe" if os.name == "nt" else "conversation_lifecycle")
        subprocess.run([
            args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-DCONFIG_VOICE_DEVICE_AUTOSTART=1",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-static-libgcc", "-static-libstdc++",
            "-I", str(ROOT / "tools/tests/conversation_stubs"),
            "-I", str(ROOT / "tools/tests/voice_transport_stubs"),
            "-I", str(ROOT / "tools/tests/ui_text_stubs"),
            "-I", str(ROOT / "main"), "-I", str(ROOT / "main/app"),
            "-I", str(ROOT / "managed_components/espressif__cjson/cJSON"),
            str(ROOT / "main/app/conversation_controller.cpp"),
            str(ROOT / "main/network/voice_protocol.cpp"),
            str(ROOT / "managed_components/espressif__cjson/cJSON/cJSON.c"),
            str(ROOT / "tools/tests/conversation_lifecycle.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
