#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace voiceaudio {

// 单持有者、固定容量的样本适配器。每个样本恰好输出一次；
// reset 明确丢弃未满块，绝不为整段话补齐。
// P4.1 用它独立于手动上传来验证 AFE 块大小。
template <size_t Capacity>
class PcmBlockAdapter {
public:
    bool configure(size_t block_samples)
    {
        if (!block_samples || block_samples > Capacity) return false;
        block_samples_ = block_samples;
        filled_ = 0;
        return true;
    }

    void reset() { filled_ = 0; }
    size_t pendingSamples() const { return filled_; }

    template <typename Consumer>
    bool append(const int16_t *samples, size_t count, Consumer &&consume)
    {
        if (!block_samples_ || (!samples && count)) return false;
        while (count) {
            const size_t copied = std::min(count, block_samples_ - filled_);
            std::copy_n(samples, copied, buffer_.data() + filled_);
            samples += copied;
            count -= copied;
            filled_ += copied;
            if (filled_ == block_samples_) {
                filled_ = 0;
                if (!consume(buffer_.data(), block_samples_)) return false;
            }
        }
        return true;
    }

private:
    std::array<int16_t, Capacity> buffer_{};
    size_t block_samples_ = 0;
    size_t filled_ = 0;
};

// 用于交错 [microphone, reference] PCM 的单持有者适配器。
// 块大小以每通道帧数表示，与 ESP-SR feed API 一致。
template <size_t Capacity>
class PairedPcmBlockAdapter {
public:
    bool configure(size_t block_frames)
    {
        if (!block_frames || block_frames > Capacity) return false;
        block_frames_ = block_frames;
        filled_ = 0;
        return true;
    }

    void reset() { filled_ = 0; }
    size_t pendingFrames() const { return filled_; }

    template <typename Consumer>
    bool append(const int16_t *microphone, const int16_t *reference, size_t frames,
                Consumer &&consume)
    {
        if (!block_frames_ || ((!microphone || !reference) && frames)) return false;
        while (frames) {
            const size_t copied = std::min(frames, block_frames_ - filled_);
            for (size_t i = 0; i < copied; ++i) {
                buffer_[(filled_ + i) * 2] = microphone[i];
                buffer_[(filled_ + i) * 2 + 1] = reference[i];
            }
            microphone += copied;
            reference += copied;
            frames -= copied;
            filled_ += copied;
            if (filled_ == block_frames_) {
                filled_ = 0;
                if (!consume(buffer_.data(), block_frames_)) return false;
            }
        }
        return true;
    }

private:
    std::array<int16_t, Capacity * 2> buffer_{};
    size_t block_frames_ = 0;
    size_t filled_ = 0;
};

}  // 命名空间 voiceaudio
