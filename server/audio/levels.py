"""PCM16 电平统计：只累计样本数、均方和峰值，不保存或记录原始录音。"""

import math
import struct


class PcmLevelMeter:
    def __init__(self):
        self.samples = 0
        self.total = 0
        self.squares = 0
        self.peak = 0
        self.clipped = 0
        self.frame_rms_max = 0

    def feed(self, pcm):
        if len(pcm) % 2:
            raise ValueError("Level diagnostics require PCM16")
        count = len(pcm) // 2
        if not count:
            return
        total = squares = peak = clipped = 0
        for (sample,) in struct.iter_unpack("<h", pcm):
            total += sample
            squares += sample * sample
            peak = max(peak, abs(sample))
            clipped += sample in (-32768, 32767)
        self.samples += count
        self.total += total
        self.squares += squares
        self.peak = max(self.peak, peak)
        self.clipped += clipped
        self.frame_rms_max = max(self.frame_rms_max, math.isqrt(squares // count))

    def metrics(self):
        count = self.samples
        mean_square = self.squares / count if count else 0
        mean = self.total / count if count else 0
        return {
            "pcm_samples": count,
            "pcm_rms": int(math.sqrt(mean_square)),
            "pcm_ac_rms": int(math.sqrt(max(0, mean_square - mean * mean))),
            "pcm_peak": self.peak,
            "pcm_clipped": self.clipped,
            "pcm_frame_rms_max": self.frame_rms_max,
        }
