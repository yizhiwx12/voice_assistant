"""通用 HTTP 辅助函数；错误信息不包含上游响应正文。"""

import json
from urllib.error import HTTPError, URLError


def request_json(request, *, opener, timeout, label):
    try:
        with opener(request, timeout=timeout) as response:
            result = json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        # 厂商响应正文可能重复凭证、音频或生成内容。
        status = exc.code
        exc.close()
        raise RuntimeError(f"{label} HTTP {status}") from None
    except (URLError, TimeoutError, OSError):
        raise RuntimeError(f"{label} connection failed") from None
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise RuntimeError(f"{label} returned invalid JSON") from None
    if not isinstance(result, dict):
        raise RuntimeError(f"{label} returned an unexpected response")
    return result


def require_credential(credentials, reference):
    value = credentials.get(reference)
    if not isinstance(value, str) or not value.strip():
        # 日志只能记录凭证引用名称，不能记录其真实值。
        raise RuntimeError(f"Required credential {reference} is not set")
    return value.strip()
