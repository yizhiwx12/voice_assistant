"""把裸 PCM16 包装为 WAV，供批量 ASR 和兼容 HTTP 接口使用。

输入必须是完整的双字节样本；写入格式固定为单声道、16 位。
"""

import io
import wave

SAMPLE_RATE_HZ = 16000


def pcm16_to_wav(audio: bytes, sample_rate_hz: int = SAMPLE_RATE_HZ) -> bytes:
    if len(audio) % 2:
        raise ValueError("PCM16 audio must contain complete samples")
    if sample_rate_hz <= 0:
        raise ValueError("Sample rate must be positive")
    wav_buffer = io.BytesIO()
    with wave.open(wav_buffer, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate_hz)
        wav_file.writeframes(audio)
    return wav_buffer.getvalue()
