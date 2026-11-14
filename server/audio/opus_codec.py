"""通过稳定的 libopus C ABI 传输有状态的 20 ms 裸 Opus 音频。

每条音频流保持同一个编解码器实例，不能逐包重建。Windows x64 使用固定
版本 PyOgg 安装包自带的 DLL，或 OPUS_LIBRARY 指定的库路径。设备使用
裸 Opus 包，不能替换成 Ogg/WebM 容器。编解码测试会调用原生库验证绑定。"""

import ctypes as c
import ctypes.util
import importlib.util
import os
from functools import lru_cache
from pathlib import Path


class CodecError(RuntimeError):
    pass


@lru_cache(maxsize=1)
def library():
    location = os.environ.get("OPUS_LIBRARY")
    if not location:
        try:
            location = ctypes.util.find_library("opus")
        except (KeyError, OSError):
            # 嵌入式或测试环境可能完全没有 PATH。
            location = None
    if not location and os.name == "nt":
        spec = importlib.util.find_spec("pyogg")
        if spec and spec.origin:
            candidate = Path(spec.origin).parent / "opus.dll"
            if candidate.is_file():
                location = str(candidate)
    if not location:
        raise CodecError("libopus unavailable: install server/requirements.txt or set OPUS_LIBRARY to an absolute library path")
    try:
        lib = c.CDLL(location)
        lib.opus_get_version_string.restype = c.c_char_p
        lib.opus_encoder_create.argtypes = [c.c_int, c.c_int, c.c_int, c.POINTER(c.c_int)]
        lib.opus_encoder_create.restype = c.c_void_p
        lib.opus_decoder_create.argtypes = [c.c_int, c.c_int, c.POINTER(c.c_int)]
        lib.opus_decoder_create.restype = c.c_void_p
        lib.opus_encoder_destroy.argtypes = [c.c_void_p]
        lib.opus_decoder_destroy.argtypes = [c.c_void_p]
        lib.opus_encoder_ctl.argtypes = [c.c_void_p, c.c_int]
        lib.opus_encoder_ctl.restype = c.c_int
        lib.opus_encode.argtypes = [c.c_void_p, c.POINTER(c.c_int16), c.c_int, c.c_void_p, c.c_int32]
        lib.opus_encode.restype = c.c_int32
        lib.opus_decode.argtypes = [c.c_void_p, c.c_void_p, c.c_int32, c.POINTER(c.c_int16), c.c_int, c.c_int]
        lib.opus_decode.restype = c.c_int
        lib.opus_packet_get_nb_samples.argtypes = [c.c_void_p, c.c_int32, c.c_int32]
        lib.opus_packet_get_nb_samples.restype = c.c_int
    except (OSError, AttributeError):
        raise CodecError("Unable to load libopus: check library version and Python/DLL architecture") from None
    return lib


def native_version():
    return library().opus_get_version_string().decode("ascii", errors="replace")


class Encoder:
    def __init__(self, sample_rate, bitrate, *, vbr=True):
        if sample_rate not in (16000, 24000) or not 6000 <= bitrate <= 128000:
            raise CodecError("Unsupported Opus sample rate or bitrate")
        self.lib = library()
        self.rate = sample_rate
        self.frame_samples = sample_rate // 50
        error = c.c_int()
        self.state = self.lib.opus_encoder_create(sample_rate, 1, 2048, c.byref(error))
        if error.value or not self.state:
            raise CodecError("Unable to create Opus encoder")
        try:
            for request, value in ((4002, bitrate), (4006, int(vbr)), (4016, 0)):
                if self.lib.opus_encoder_ctl(self.state, request, c.c_int(value)) != 0:
                    raise CodecError("Unable to configure Opus encoder")
            lookahead = c.c_int()
            if self.lib.opus_encoder_ctl(self.state, 4027, c.byref(lookahead)) != 0:
                raise CodecError("Unable to read Opus encoder lookahead")
            self.pre_skip = lookahead.value
        except BaseException:
            self.close()
            raise
        self.pending = bytearray()
        self.valid_samples = 0
        self.packet_count = 0
        self.finished = False

    def _encode(self, pcm):
        samples = (c.c_int16 * self.frame_samples).from_buffer_copy(pcm)
        output = c.create_string_buffer(1275)
        length = self.lib.opus_encode(self.state, samples, self.frame_samples, output, len(output))
        if length < 0:
            raise CodecError("Opus encode failed")
        self.packet_count += 1
        return output.raw[:length]

    def feed(self, pcm):
        if self.finished or not self.state or len(pcm) % 2:
            raise CodecError("Invalid PCM or closed Opus stream")
        self.valid_samples += len(pcm) // 2
        self.pending.extend(pcm)
        frames = []
        frame_bytes = self.frame_samples * 2
        while len(self.pending) >= frame_bytes:
            frames.append(self._encode(bytes(self.pending[:frame_bytes])))
            del self.pending[:frame_bytes]
        return frames

    def finish(self):
        if self.finished or not self.state:
            raise CodecError("Opus stream already finished")
        self.finished = True
        if not self.valid_samples:
            return []
        # 即使原始音频恰好结束于整帧边界，也要计入编解码器前瞻样本。
        target = (self.valid_samples + self.pre_skip + self.frame_samples - 1) // self.frame_samples
        packets = []
        while self.packet_count < target:
            frame_bytes = self.frame_samples * 2
            pcm = bytes(self.pending[:frame_bytes]).ljust(frame_bytes, b"\0")
            del self.pending[:frame_bytes]
            packets.append(self._encode(pcm))
        return packets

    def close(self):
        if getattr(self, "state", None):
            self.lib.opus_encoder_destroy(self.state)
            self.state = None

    def __del__(self):
        self.close()


class Decoder:
    def __init__(self, sample_rate):
        if sample_rate not in (16000, 24000):
            raise CodecError("Unsupported Opus sample rate")
        self.lib = library()
        self.rate = sample_rate
        self.frame_samples = sample_rate // 50
        error = c.c_int()
        self.state = self.lib.opus_decoder_create(sample_rate, 1, c.byref(error))
        if error.value or not self.state:
            raise CodecError("Unable to create Opus decoder")

    def decode(self, packet):
        if not self.state or not 1 <= len(packet) <= 1275:
            raise CodecError("Invalid raw Opus packet")
        data = c.create_string_buffer(packet)
        samples = self.lib.opus_packet_get_nb_samples(data, len(packet), self.rate)
        if samples != self.frame_samples:
            raise CodecError("Opus packet must contain exactly 20 ms")
        pcm = (c.c_int16 * self.frame_samples)()
        count = self.lib.opus_decode(self.state, data, len(packet), pcm, self.frame_samples, 0)
        if count != self.frame_samples:
            raise CodecError("Opus decode failed")
        return bytes(pcm)

    def close(self):
        if getattr(self, "state", None):
            self.lib.opus_decoder_destroy(self.state)
            self.state = None

    def __del__(self):
        self.close()
