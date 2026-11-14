#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace voiceaudio {

// 用于 16 kHz PCM16 的连续一阶直流阻断器。极点为
// 32604/32768（约 0.995），对应截止频率约 12.78 Hz。
// 不改变样本数或帧边界，也不需要前瞻或分配。
class PcmDcBlocker {
public:
    void reset()
    {
        previous_ = 0;
        output_q16_ = 0;
        initialized_ = false;
    }

    int16_t process(int16_t input)
    {
        if (!initialized_) {
            previous_ = input;
            initialized_ = true;
            return 0;  // 恒定的启动偏置不能变成脉冲。
        }
        const int32_t difference = static_cast<int32_t>(input) - previous_;
        previous_ = input;
        // 跨帧保留亚样本精度。有符号除法向零截断，
        // 使反馈一直衰减到零，不出现整数极限环。
        // 对于有界 PCM16 输入，乘积始终小于 2^48。
        output_q16_ = static_cast<int64_t>(difference) * 65536 +
                      output_q16_ * 32604 / 32768;
        return static_cast<int16_t>(std::clamp<int64_t>(
            output_q16_ / 65536, INT16_MIN, INT16_MAX));
    }

    void process(int16_t *samples, size_t count)
    {
        for (size_t i = 0; i < count; ++i) samples[i] = process(samples[i]);
    }

private:
    int32_t previous_ = 0;
    int64_t output_q16_ = 0;
    bool initialized_ = false;
};

}  // 命名空间 voiceaudio
