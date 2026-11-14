"""在电脑端用真实 LVGL 9.3 渲染实际界面及中文字库。

不构建 ESP-IDF 固件，不使用设备、串口或语音服务。原生目标文件
单独缓存在 build/host-ui-lvgl，方便快速复跑。
检查 RGB565 像素和真实对象坐标，不用桩替代 LVGL 文字几何。
本检查不测量目标设备帧率。
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("g++") or "C:/mingw64/bin/g++.exe")
    parser.add_argument("--ui-source", type=Path, default=ROOT / "main/display/voice_assistant_ui.cpp")
    parser.add_argument("--preview-dir", type=Path,
                        help="Save RGB565 renders as PPM, plus 3x PNG previews if Pillow is installed")
    args = parser.parse_args()
    compiler = Path(args.compiler).resolve()
    cc = compiler.with_name("gcc.exe" if os.name == "nt" else "gcc")
    ar = compiler.with_name("ar.exe" if os.name == "nt" else "ar")
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    cache = ROOT / "build/host-ui-lvgl"
    cache.mkdir(parents=True, exist_ok=True)
    lvgl = ROOT / "managed_components/lvgl__lvgl"
    config = ROOT / "tools/tests/ui_lvgl_stubs/lv_conf.h"
    common = ["-O2", "-DLV_CONF_INCLUDE_SIMPLE", "-I", str(config.parent), "-I", str(lvgl)]
    source_dirs = ["core", "display", "draw", "font", "indev", "layouts", "misc", "osal", "stdlib", "themes", "tick", "widgets", "others"]
    sources = sorted({p for folder in source_dirs for p in (lvgl / "src" / folder).rglob("*.c")
                      if "/draw/" not in p.as_posix() or "/draw/sw/" in p.as_posix() or p.parent.name == "draw"})
    sources.extend(sorted((lvgl / "src").glob("*.c")))
    sources.extend(sorted((lvgl / "src/libs/bin_decoder").glob("*.c")))
    # 禁用字库的编译单元成本很低；全部纳入可安全兼容配置变化，
    # 无需维护可能遗漏的手写列表。
    sources.append(ROOT / "main/fonts/lv_font_chinese_16.c")
    config_hash = hashlib.sha256(config.read_bytes()).hexdigest()[:12]
    newest_header = max(p.stat().st_mtime for p in (lvgl / "src").rglob("*.h"))

    def compile_one(source):
        key = hashlib.sha256(str(source).encode()).hexdigest()[:16]
        obj = cache / f"{source.stem}-{key}-{config_hash}.o"
        if not obj.exists() or obj.stat().st_mtime < max(source.stat().st_mtime, newest_header):
            result = subprocess.run([str(cc), "-std=c11", *common, "-c", str(source), "-o", str(obj)],
                                    env=environment, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        return obj

    with ThreadPoolExecutor(max_workers=min(6, os.cpu_count() or 2)) as workers:
        objects = list(workers.map(compile_one, sources))
    archive = cache / "lvgl-native.a"
    archive.unlink(missing_ok=True)
    subprocess.run([str(ar), "rcs", str(archive), *map(str, objects)], check=True, env=environment, timeout=60)
    executable = cache / ("ui_lvgl_render.exe" if os.name == "nt" else "ui_lvgl_render")
    subprocess.run([str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2", "-static-libgcc", "-static-libstdc++",
                    *common, "-I", str(args.ui_source.resolve().parent),
                    "-I", str(ROOT / "main"), "-I", str(ROOT / "main/display"),
                    "-I", str(ROOT / "tools/tests/ui_render_stubs"),
                    "-I", str(ROOT / "tools/tests/ui_text_stubs"),
                    "-I", str(ROOT / "tools/tests/firmware_stubs"),
                    str(args.ui_source), str(ROOT / "tools/tests/ui_lvgl_render.cpp"), str(archive),
                    "-o", str(executable)], check=True, env=environment, timeout=90)
    render_arguments = [str(executable)]
    if args.preview_dir:
        args.preview_dir.mkdir(parents=True, exist_ok=True)
        render_arguments.append(str(args.preview_dir.resolve()))
    subprocess.run(render_arguments, check=True, env=environment, timeout=60)
    if args.preview_dir:
        # Pillow 为可选依赖，部分开发环境已经安装。
        # 可复现的原生 PPM 输出不需要额外库。
        try:
            from PIL import Image
        except ImportError:
            print(f"PPM previews saved in {args.preview_dir}; PNG conversion skipped (Pillow unavailable)")
        else:
            for name in ("voice-ui-idle", "voice-ui-speaking"):
                with Image.open(args.preview_dir / (name + ".ppm")) as preview:
                    preview.resize((720, 720), Image.Resampling.NEAREST).save(
                        args.preview_dir / (name + ".png"))
            print(f"PPM and PNG previews saved in {args.preview_dir}")


if __name__ == "__main__":
    main()
