"""校验打包后的 ESP-SR 模型，并生成固件和构建来源信息。

只读取模型产物、分区信息和公开组件元数据，
不会复制 sdkconfig 或服务端配置中的凭证。
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MODEL_NAME = "vadnet1_medium"


def parse_models(image: bytes) -> list[dict]:
    """ESP-SR 基于指针的加载器使用数据前，先检查每个偏移量。"""
    if len(image) < 4:
        raise ValueError("Model image is truncated")
    count = struct.unpack_from("<I", image)[0]
    if not 1 <= count <= 64:
        raise ValueError("Model count is invalid")
    position = 4
    models = []
    intervals = []

    def name_at(offset: int) -> str:
        if offset + 32 > len(image):
            raise ValueError("Model header is truncated")
        raw = image[offset : offset + 32]
        if b"\0" not in raw:
            raise ValueError("Model name must be NUL terminated")
        name = raw.split(b"\0", 1)[0].decode("ascii")
        if not name or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise ValueError("Unexpected model/file name")
        return name

    for _ in range(count):
        name = name_at(position)
        position += 32
        if position + 4 > len(image):
            raise ValueError("Missing file count")
        files_count = struct.unpack_from("<I", image, position)[0]
        position += 4
        if not 1 <= files_count <= 256:
            raise ValueError("Model file count is invalid")
        files = []
        for _ in range(files_count):
            file_name = name_at(position)
            position += 32
            if position + 8 > len(image):
                raise ValueError("Missing model file range")
            offset, size = struct.unpack_from("<II", image, position)
            position += 8
            if size == 0 or offset + size > len(image):
                raise ValueError("Model file exceeds image bounds")
            intervals.append((offset, offset + size))
            files.append({"name": file_name, "offset": offset, "bytes": size})
        if "_MODEL_INFO_" not in {item["name"] for item in files}:
            raise ValueError(f"{name} has no model info")
        if len({item["name"] for item in files}) != len(files):
            raise ValueError("Model contains duplicate file names")
        models.append({"name": name, "files": files})
    intervals.sort()
    previous_end = position
    for start, end in intervals:
        if start < previous_end:
            raise ValueError("Model data overlaps header or another file")
        previous_end = end
    if len({item["name"] for item in models}) != len(models):
        raise ValueError("Model contains duplicate model names")
    return models


def model_partition(table: bytes) -> dict:
    partitions = []
    for position in range(0, len(table) - 31, 32):
        magic = struct.unpack_from("<H", table, position)[0]
        if magic != 0x50AA:
            break  # MD5 记录或已擦除的尾部。
        _, kind, subtype, offset, size, label, _ = struct.unpack_from(
            "<HBBII16sI", table, position
        )
        name = label.split(b"\0", 1)[0].decode("ascii")
        if size <= 0 or offset % 4096:
            raise ValueError("Unaligned or empty flash partition")
        partitions.append({"name": name, "type": kind, "subtype": subtype,
                           "offset": offset, "bytes": size})
    ordered = sorted(partitions, key=lambda item: item["offset"])
    for left, right in zip(ordered, ordered[1:]):
        if left["offset"] + left["bytes"] > right["offset"]:
            raise ValueError("Flash partitions overlap")
    matches = [item for item in partitions if item["name"] == "model"]
    if len(matches) != 1 or matches[0]["type"] != 1 or matches[0]["subtype"] != 0x82:
        raise ValueError("Expected one model/data/spiffs partition")
    return matches[0]


def write_if_changed(path: Path, content: str) -> None:
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")


def prepare(build: Path, component: Path) -> dict:
    image_path = build / "srmodels" / "srmodels.bin"
    image = image_path.read_bytes()
    models = parse_models(image)
    # P4.1 仅包含 VADNet，不加入未请求的 WakeNet、MultiNet 或降噪模型。
    if [item["name"] for item in models] != [MODEL_NAME]:
        raise ValueError(f"P4.1 requires only {MODEL_NAME}; inspect ESP-SR model selection")
    partition = model_partition((build / "partition_table" / "partition-table.bin").read_bytes())
    if len(image) > partition["bytes"]:
        raise ValueError("Packed model exceeds model partition")
    flash = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    flash_files = flash["flash_files"]
    matching_image = None
    for offset, filename in flash_files.items():
        if int(offset, 0) == partition["offset"]:
            matching_image = (build / filename).resolve()
    if matching_image != image_path.resolve():
        raise ValueError("Full flash manifest does not include the packed model at its partition offset")
    metadata = (component / "idf_component.yml").read_text(encoding="utf-8")
    match = re.search(r'^version:\s*[\"\']?([0-9][^\"\'\s]*)', metadata, re.MULTILINE)
    if match is None:
        raise ValueError("ESP-SR component metadata has no version")
    digest = hashlib.sha256(image).hexdigest()
    report = {
        "component": "espressif/esp-sr",
        "component_version": match.group(1),
        "model": MODEL_NAME,
        "image": "srmodels/srmodels.bin",
        "image_bytes": len(image),
        "sha256": digest,
        "partition": partition,
        "models": models,
        "full_flash_files": flash_files,
        "hardware_verified": False,
    }
    header = (
        "// Generated by tools/prepare_sr_model.py; do not edit.\n"
        "#pragma once\n#include <cstddef>\n"
        "namespace voiceaudio::srmanifest {\n"
        f'inline constexpr char kComponentVersion[] = "{match.group(1)}";\n'
        f'inline constexpr char kModelName[] = "{MODEL_NAME}";\n'
        f"inline constexpr size_t kImageBytes = {len(image)};\n"
        f'inline constexpr char kSha256[] = "{digest}";\n'
        "}\n"
    )
    write_if_changed(build / "generated" / "sr_model_manifest.hpp", header)
    write_if_changed(build / "p4_model_manifest.json", json.dumps(report, indent=2) + "\n")
    print(f"P4 model verified: ESP-SR {match.group(1)}, {MODEL_NAME}, {len(image)} bytes, SHA256 {digest}")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--component-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.build_dir.resolve(), args.component_dir.resolve())
    except (OSError, ValueError, KeyError, UnicodeError, struct.error) as error:
        parser.exit(1, f"P4 model verification failed: {error}\n")


if __name__ == "__main__":
    main()
