"""配置向导的文件操作：保留未知凭证项、检测外部修改、备份后原子替换。

本模块不会启动服务，也不请求模型。业务配置与密钥始终分别保存。
"""

import json
import os
import tempfile
from datetime import datetime
from pathlib import Path
from uuid import uuid4

from core.config import (
    ConfigurationError, EXAMPLE_CONFIG_PATH, _read_json, parse_voice_config,
    select_profile,
)


def read_document(path, *, fallback=None):
    """返回可编辑对象及原文件字节；后者仅用来发现编辑期间的外部改动。"""
    path = Path(path)
    original = path.read_bytes() if path.exists() else None
    if original is None:
        return (_read_json(Path(fallback)) if fallback else {}), None
    # 使用同一份字节完成解析与并发检查，避免两次读取之间被外部编辑。
    try:
        value = json.loads(original.decode("utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError):
        raise ConfigurationError(f"{path.name} 格式错误，请先修复 UTF-8 JSON；原文件未改动") from None
    if not isinstance(value, dict):
        raise ConfigurationError(f"{path.name} 必须是 JSON 对象")
    return value, original


def validate_structure(data):
    """复用运行时结构校验，让缺字段/错类型在进入菜单前给出可读错误。"""
    return parse_voice_config(data, environ={})


def validate_document(data, *, profile, mode="realtime"):
    """复用运行时的完整校验，不在向导里维护另一套模型兼容规则。"""
    return select_profile(validate_structure(data), profile=profile, mode=mode)


def save_document(path, data, *, original):
    """成功时返回 (新文件字节, 备份路径)；失败时保留旧文件。

    备份可能含密钥，因此也放在已忽略的 config/backups/ 中，不能分享整个备份目录。
    """
    path = Path(path)
    encoded = (json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    current = path.read_bytes() if path.exists() else None
    if current != original:
        raise ConfigurationError(f"{path.name} 已被其他窗口修改，请退出后重新打开向导")
    path.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    descriptor, temporary = tempfile.mkstemp(prefix=".voice-config-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        if original is not None:
            backup_dir = path.parent / "backups"
            backup_dir.mkdir(exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            backup = backup_dir / f"{path.stem}.{stamp}.{uuid4().hex[:8]}.json"
            # 备份加载时的原字节，不能意外备份其他窗口刚写入的新版本。
            with backup.open("xb") as stream:
                stream.write(original)
        # 临时文件落盘/备份期间也可能发生外部编辑，再检查一次再替换。
        current = path.read_bytes() if path.exists() else None
        if current != original:
            raise ConfigurationError(f"{path.name} 已被其他窗口修改，请退出后重新打开向导")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return encoded, backup


def load_editable_config(path):
    return read_document(path, fallback=EXAMPLE_CONFIG_PATH)
