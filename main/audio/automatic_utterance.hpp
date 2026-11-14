#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace voiceaudio {

// 单一持有者（控制器的采集屏障），存储由调用方在 PSRAM 中提供。
// 所有边界使用连续的 AFE 输出时钟，绝不重放 VAD 缓存。
class AutomaticUtterance {
public:
    struct Frame { std::array<int16_t, 320> pcm{}; uint64_t offset = 0; bool speech = false; };
    enum class Phase : uint8_t { Disabled, Listening, Opening, Streaming, Draining };
    enum class Result : uint8_t { Accepted, Triggered, Overflow, Discontinuity };
    struct StartMetrics {
        uint32_t candidates = 0, confirmations = 0, rejected = 0, energy_rejections = 0;
        uint32_t window_frames = 0, energy_active_frames = 0, energy_tail_active_frames = 0;
        uint32_t frame_ac_rms = 0, window_ac_rms = 0;
        uint64_t candidate_offset = 0, confirmed_offset = 0;
        size_t confirm_frames = 8;
        uint32_t min_ac_rms = 80;
        bool pending = false;
    };

    void configure(Frame *storage, size_t capacity, size_t preroll, size_t silence,
                   size_t confirmation = 8, uint32_t min_ac_rms = 80)
    {
        frames_ = storage;
        capacity_ = capacity;
        preroll_ = preroll;
        silence_limit_ = silence;
        confirmation_ = std::max<size_t>(1, confirmation);
        min_ac_rms_ = min_ac_rms;
        start_metrics_ = {};
        start_metrics_.confirm_frames = confirmation_;
        start_metrics_.min_ac_rms = min_ac_rms_;
        disable();
    }
    void disable() { phase_ = Phase::Disabled; clear(); }
    void listen() { clear(); phase_ = Phase::Listening; }
    Phase phase() const { return phase_; }
    size_t size() const { return size_; }
    uint32_t utteranceFrames() const { return utterance_frames_; }
    uint64_t nextOffset() const { return next_offset_; }
    StartMetrics startMetrics() const { return start_metrics_; }
    // D2 在与观测器相同的采集屏障内调用。触碰已有目标前先校验完整源数据，
    // 且两者存储必须分离。只有明确确认的区间才绕过安静门限。
    // 复制过程有界且不分配内存，也不创建云端轮次。
    bool adoptConfirmed(const Frame *source, size_t source_capacity, size_t source_head,
                        size_t count, uint64_t candidate_offset, uint64_t confirmed_offset)
    {
        if (!frames_ || !source || !capacity_ || !source_capacity ||
            source == frames_ || source_head >= source_capacity || !count ||
            count > source_capacity || count > capacity_ || count > 1500 ||
            candidate_offset >= confirmed_offset ||
            (confirmed_offset - candidate_offset) % 320) return false;
        const uint64_t first = source[source_head].offset;
        if (candidate_offset < first || (candidate_offset - first) % 320 ||
            first > std::numeric_limits<uint64_t>::max() - count * 320 ||
            confirmed_offset > first + count * 320) return false;
        bool candidate_seen = false;
        for (size_t i = 0; i < count; ++i) {
            const auto &frame = source[(source_head + i) % source_capacity];
            if (frame.offset != first + i * 320) return false;
            if (frame.offset >= candidate_offset && frame.offset < confirmed_offset) {
                if (!frame.speech) return false;
                candidate_seen = true;
            }
        }
        if (!candidate_seen) return false;

        clear();
        phase_ = Phase::Opening;
        for (size_t i = 0; i < count; ++i) {
            const auto &frame = source[(source_head + i) % source_capacity];
            frames_[size_++] = frame;
            ++utterance_frames_;
            next_offset_ = frame.offset + 320;
            // 确认之前的历史绝不能为本轮触发结束点。
            // 保留首个完整的尾部静音窗口，不保留结束点之后的语音
            // 或仍属扬声器播放阶段的陈旧输出。
            if (frame.offset >= confirmed_offset)
                quiet_frames_ = frame.speech ? 0 : quiet_frames_ + 1;
            if (quiet_frames_ >= silence_limit_ || utterance_frames_ >= 1500) {
                phase_ = Phase::Draining;
                break;
            }
        }
        have_offset_ = true;
        return true;
    }
    void started()
    {
        if (phase_ == Phase::Opening) phase_ = Phase::Streaming;
    }
    // 继续处理，直到采集边界从 AFE 输出。
    // 避免 s 命令与正在进行的 feed/fetch 竞争时截断一个词。
    void finishAfter(uint64_t offset)
    {
        if (phase_ != Phase::Opening && phase_ != Phase::Streaming) return;
        finish_offset_ = offset;
        finish_requested_ = true;
        if (have_offset_ && next_offset_ >= offset) phase_ = Phase::Draining;
    }
    bool pop(Frame &frame)
    {
        if (!size_) return false;
        frame = frames_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }
    Result append(const int16_t *pcm, size_t count, uint64_t offset, bool speech)
    {
        if (phase_ == Phase::Disabled || phase_ == Phase::Draining) return Result::Accepted;
        if (!pcm || count != 320 || offset > std::numeric_limits<uint64_t>::max() - count ||
            (have_offset_ && offset != next_offset_))
            return Result::Discontinuity;
        have_offset_ = true;
        next_offset_ = offset + count;
        // 候选开始时冻结完整预录段，同时保留确认区间。
        // 额外确认延迟不能把词的起始部分挤出滚动历史。
        if (phase_ == Phase::Listening && !onset_frames_ && !(speech && quiet_gate_))
            while (size_ && size_ >= preroll_) discardOldest();
        if (!frames_ || size_ >= capacity_) return Result::Overflow;
        auto &frame = frames_[(head_ + size_) % capacity_];
        std::copy_n(pcm, count, frame.pcm.begin());
        frame.offset = offset;
        frame.speech = speech;
        ++size_;
        if (phase_ == Phase::Listening) {
            // 启用/恢复后先要求安静。
            // 已缓存的扬声器输出或持续噪声候选不能重新开启轮次。
            if (!speech) {
                if (onset_frames_) {
                    ++start_metrics_.rejected;
                    resetOnset();
                    quiet_gate_ = false;
                    trimHistory();
                }
                if (++quiet_frames_ >= 15) quiet_gate_ = true;
            } else {
                quiet_frames_ = 0;
                if (quiet_gate_) {
                    if (!onset_frames_) {
                        ++start_metrics_.candidates;
                        start_metrics_.candidate_offset = offset;
                        start_metrics_.confirmed_offset = 0;
                        start_metrics_.window_frames = start_metrics_.energy_active_frames = 0;
                        start_metrics_.energy_tail_active_frames = 0;
                        start_metrics_.pending = true;
                    }
                    // 精确计算帧的交流能量，仅在统计时去除直流。
                    // 单次高能量脉冲不能借 SDK VAD 的拖尾进入云端轮次。
                    // 绝不修改 PCM 或其 VAD 标签。
                    int64_t sum = 0;
                    uint64_t squares = 0;
                    for (size_t i = 0; i < count; ++i) {
                        const int32_t sample = pcm[i];
                        sum += sample;
                        squares += static_cast<int64_t>(sample) * sample;
                    }
                    const uint64_t variance = count * squares - static_cast<uint64_t>(sum * sum);
                    if (onset_variance_ > std::numeric_limits<uint64_t>::max() - variance)
                        return Result::Overflow;
                    onset_variance_ += variance;
                    ++onset_frames_;
                    const uint64_t floor = static_cast<uint64_t>(min_ac_rms_) * min_ac_rms_;
                    const uint64_t energy = variance / count / count;
                    const bool active = energy >= floor;
                    if (active) ++onset_active_;
                    onset_tail_ = static_cast<uint8_t>(((onset_tail_ << 1U) | (active ? 1U : 0U)) & 7U);
                    const uint32_t tail_active = (onset_tail_ & 1U) + ((onset_tail_ >> 1U) & 1U) +
                                                 ((onset_tail_ >> 2U) & 1U);
                    const uint64_t mean_energy = onset_variance_ / onset_frames_ / count / count;
                    start_metrics_.window_frames = static_cast<uint32_t>(onset_frames_);
                    start_metrics_.energy_active_frames = onset_active_;
                    start_metrics_.energy_tail_active_frames = tail_active;
                    start_metrics_.frame_ac_rms = squareRoot(energy);
                    start_metrics_.window_ac_rms = squareRoot(mean_energy);
                    if (onset_frames_ >= confirmation_) {
                        const size_t minimum_active = std::min(confirmation_, std::max<size_t>(3, (confirmation_ + 1) / 2));
                        if (mean_energy < floor || onset_active_ < minimum_active ||
                            tail_active < std::min<size_t>(2, confirmation_)) {
                            ++start_metrics_.energy_rejections;
                            resetOnset();
                            quiet_gate_ = false;
                            trimHistory();
                        } else {
                            ++start_metrics_.confirmations;
                            start_metrics_.confirmed_offset = next_offset_;
                            start_metrics_.pending = false;
                            phase_ = Phase::Opening;
                            utterance_frames_ = static_cast<uint32_t>(size_);
                            return Result::Triggered;
                        }
                    }
                }
            }
            return Result::Accepted;
        }
        ++utterance_frames_;
        quiet_frames_ = speech ? 0 : quiet_frames_ + 1;
        if (quiet_frames_ >= silence_limit_ || utterance_frames_ >= 1500 ||
            (finish_requested_ && next_offset_ >= finish_offset_)) phase_ = Phase::Draining;
        return Result::Accepted;
    }

private:
    static uint32_t squareRoot(uint64_t value)
    {
        uint64_t root = 0, bit = uint64_t{1} << 62;
        while (bit > value) bit >>= 2;
        while (bit) {
            if (value >= root + bit) { value -= root + bit; root = (root >> 1) + bit; }
            else root >>= 1;
            bit >>= 2;
        }
        return static_cast<uint32_t>(root);
    }
    void trimHistory() { while (size_ > preroll_) discardOldest(); }
    void resetOnset()
    {
        onset_frames_ = 0;
        onset_variance_ = 0;
        onset_active_ = 0;
        onset_tail_ = 0;
        start_metrics_.pending = false;
    }
    void discardOldest() { if (size_) { head_ = (head_ + 1) % capacity_; --size_; } }
    void clear()
    {
        head_ = size_ = 0;
        utterance_frames_ = quiet_frames_ = 0;
        next_offset_ = finish_offset_ = 0;
        have_offset_ = quiet_gate_ = finish_requested_ = false;
        resetOnset();
    }
    Frame *frames_ = nullptr;
    size_t capacity_ = 0, preroll_ = 10, silence_limit_ = 35, head_ = 0, size_ = 0;
    uint32_t utterance_frames_ = 0, quiet_frames_ = 0;
    uint64_t next_offset_ = 0, finish_offset_ = 0;
    size_t confirmation_ = 8, onset_frames_ = 0;
    uint64_t onset_variance_ = 0;
    uint32_t min_ac_rms_ = 80, onset_active_ = 0;
    uint8_t onset_tail_ = 0;
    StartMetrics start_metrics_{};
    bool have_offset_ = false, quiet_gate_ = false, finish_requested_ = false;
    Phase phase_ = Phase::Disabled;
};
}  // 命名空间 voiceaudio
