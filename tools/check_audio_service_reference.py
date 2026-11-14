"""使用确定性的 RTOS 替代实现运行真实 AudioService 参考音频调度。

重采样、时间线和声学模块使用真实代码；设备驱动、编解码器和 RTOS 被替代。
检查调度与释放行为，不衡量执行时间、硬件内存压力、真实 DMA 中断或声学准确性。
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
    with tempfile.TemporaryDirectory(prefix="audio-service-reference-") as temporary:
        for variant, use_aec, boot_probe in (
            ("single_mic", False, False),
            ("aec", True, False),
            ("aec_probe_boot", True, True),
        ):
            executable = Path(temporary) / (f"audio_service_reference_{variant}.exe" if os.name == "nt"
                                           else f"audio_service_reference_{variant}")
            subprocess.run([
                args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-ffp-contract=off",
                "-static-libgcc", "-static-libstdc++",
                "-DCONFIG_VOICE_LOCAL_FRONTEND_ENABLED=1",
                f"-DCONFIG_VOICE_AEC_OBSERVATION_ENABLED={int(use_aec)}",
                f"-DCONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT={int(boot_probe)}",
                "-DCONFIG_VOICE_AEC_REFERENCE_DELAY_MS=0",
                "-I", str(ROOT / "tools/tests/audio_service_reference_stubs"),
                "-I", str(ROOT / "main"), "-I", str(ROOT / "main/audio"),
                str(ROOT / "main/audio/audio_service.cpp"),
                str(ROOT / "main/audio/reference_monitor.cpp"),
                str(ROOT / "main/audio/reference_timeline.cpp"),
                str(ROOT / "main/audio/reference_resampler.cpp"),
                str(ROOT / "main/audio/acoustic_delay_probe.cpp"),
                str(ROOT / "tools/tests/audio_service_reference.cpp"), "-o", str(executable),
            ], check=True, env=environment, timeout=60)
            subprocess.run([str(executable)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
