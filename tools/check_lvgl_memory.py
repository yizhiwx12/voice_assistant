"""检查仅使用 PSRAM 的 LVGL 内存池归属及真实内置分配器钩子。

分配和失败测试中使用 ESP 堆/日志替代接口。另分别在启用及禁用钩子时
编译 LVGL 真实内置内存源码并检查符号，确认原有 64 KiB 静态池已移除。
不使用设备或云端。
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
    compiler = Path(args.compiler).resolve()
    cc = compiler.with_name("gcc.exe" if os.name == "nt" else "gcc")
    nm = compiler.with_name("nm.exe" if os.name == "nt" else "nm")
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="lvgl-memory-") as temporary:
        directory = Path(temporary)
        executable = directory / ("lvgl_memory.exe" if os.name == "nt" else "lvgl_memory")
        subprocess.run([
            str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-Wl,--wrap=abort",
            "-I", str(ROOT / "tools/tests/lvgl_memory_stubs"), "-I", str(ROOT / "main"),
            str(ROOT / "main/display/lvgl_memory.cpp"),
            str(ROOT / "tools/tests/lvgl_memory.cpp"), "-o", str(executable),
        ], check=True, env=environment, timeout=60)
        for mode, code, expected in (
            ("normal", 0, "100 reinitializations passed"),
            ("allocation-failed", 86, "Cannot allocate 65536-byte LVGL pool in PSRAM"),
            ("invalid-size", 86, "Invalid LVGL fixed-pool size"),
            ("changed-size", 86, "Invalid LVGL fixed-pool size"),
        ):
            result = subprocess.run([str(executable), mode], env=environment,
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == code, (mode, result.returncode, result.stdout, result.stderr)
            assert expected in result.stdout, (mode, result.stdout)

        source = ROOT / "managed_components/lvgl__lvgl/src/stdlib/builtin/lv_mem_core_builtin.c"
        for hooked in (False, True):
            output = directory / ("hooked.o" if hooked else "baseline.o")
            command = [str(cc), "-std=c99", "-O0", "-DLV_CONF_SKIP",
                       "-I", str(ROOT / "main"), "-c", str(source), "-o", str(output)]
            if hooked:
                command += ["-DLV_MEM_POOL_ALLOC=voice_lvgl_pool_alloc",
                            '-DLV_MEM_POOL_INCLUDE="display/lvgl_memory.h"']
            subprocess.run(command, check=True, env=environment, timeout=60)
            symbols = subprocess.run([str(nm), "-S", str(output)], check=True,
                                     env=environment, capture_output=True, text=True, timeout=15).stdout
            assert ("work_mem_int" in symbols) == (not hooked), symbols
            if hooked:
                assert "voice_lvgl_pool_alloc" in symbols, symbols

        lvgl = ROOT / "managed_components/lvgl__lvgl"
        objects = [directory / "hooked.o"]
        for index, relative in enumerate((
            "src/stdlib/builtin/lv_tlsf.c", "src/stdlib/builtin/lv_string_builtin.c",
            "src/stdlib/lv_mem.c", "src/misc/lv_ll.c",
        )):
            output = directory / f"lvgl_{index}.o"
            subprocess.run([str(cc), "-std=c99", "-O2", "-ffunction-sections", "-fdata-sections",
                            "-DLV_CONF_SKIP", "-c", str(lvgl / relative), "-o", str(output)],
                           check=True, env=environment, timeout=60)
            objects.append(output)
        integration = directory / ("lvgl_tlsf.exe" if os.name == "nt" else "lvgl_tlsf")
        subprocess.run([
            str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            "-static-libgcc", "-static-libstdc++", "-Wl,--gc-sections", "-DLV_CONF_SKIP",
            "-I", str(ROOT / "tools/tests/lvgl_memory_stubs"),
            "-I", str(ROOT / "main"), "-I", str(lvgl),
            str(ROOT / "main/display/lvgl_memory.cpp"),
            str(ROOT / "tools/tests/lvgl_tlsf_pool.cpp"),
            *[str(path) for path in objects], "-o", str(integration),
        ], check=True, env=environment, timeout=60)
        subprocess.run([str(integration)], check=True, env=environment, timeout=15)
        print("LVGL memory: 4 allocation/failure scenarios, real allocator hook/static-pool removal and TLSF reuse passed")


if __name__ == "__main__":
    main()
