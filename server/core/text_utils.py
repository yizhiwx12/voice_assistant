"""传输/显示文字工具：按字节截断时保护 UTF-8，单行格式仅用于兼容接口。

显示容量不等于 LLM/TTS 正文容量，不能在厂商适配层提前截短回答。
"""


def truncate_utf8(text, byte_limit):
    encoded = text.encode("utf-8")
    if len(encoded) <= byte_limit:
        return text
    return encoded[:byte_limit].decode("utf-8", errors="ignore")


def clean_single_line(text):
    return " ".join(text.replace("\r", " ").replace("\n", " ").split())
