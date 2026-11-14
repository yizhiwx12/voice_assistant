#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "automatic_utterance.hpp"

namespace voiceaudio {

// 观测处理后的 AEC 输出。D1 不交接；明确启用 D2 后，允许有界交接已确认缓冲。
// 调用方负责播放/参考健康检查，并通过采集屏障串行访问此对象。
// 前端回调中没有音频/网络副作用，也不分配内存。
class BargeInObserver {
public:
    using Frame = AutomaticUtterance::Frame;
    enum class Phase : uint8_t { Disabled, Armed, Candidate, Confirmed, Cooldown, Fault };
    struct Snapshot {
        Phase phase = Phase::Disabled;
        uint32_t candidates = 0, confirmations = 0, rejected = 0;
        uint32_t overflows = 0, discontinuities = 0, timeouts = 0;
        uint64_t candidate_offset = 0, confirmed_offset = 0;
        uint64_t oldest_offset = 0, next_offset = 0;
        uint32_t speech_frames = 0;
        size_t buffered_frames = 0, buffered_peak = 0;
        uint32_t energy_rejections = 0, min_ac_rms = 80;
        uint64_t last_frame_offset = 0;
        int32_t frame_mean = 0;
        uint32_t frame_rms = 0, frame_ac_rms = 0, frame_peak = 0;
        bool frame_vad = false;
        uint32_t window_ac_rms = 0, window_frames = 0;
        uint32_t energy_active_frames = 0, energy_tail_active_frames = 0;
    };

    void configure(Frame *storage, size_t capacity, size_t preroll_frames,
                   size_t confirmation_frames, size_t hold_frames,
                   uint32_t min_ac_rms = 80)
    {
        frames_ = storage;
        capacity_ = capacity;
        preroll_ = preroll_frames;
        confirmation_ = confirmation_frames;
        hold_limit_ = hold_frames;
        metrics_ = {};
        metrics_.min_ac_rms = min_ac_rms;
        handoff_enabled_ = false;
        disable();
    }
    void enable(uint64_t first_sample)
    {
        clear();
        first_sample_ = first_sample;
        have_offset_ = false;
        metrics_.candidate_offset = metrics_.confirmed_offset = 0;
        metrics_.next_offset = 0;
        metrics_.last_frame_offset = 0;
        metrics_.frame_mean = 0;
        metrics_.frame_rms = metrics_.frame_ac_rms = metrics_.frame_peak = 0;
        metrics_.frame_vad = false;
        metrics_.window_ac_rms = 0;
        metrics_.window_frames = 0;
        metrics_.energy_active_frames = 0;
        metrics_.energy_tail_active_frames = 0;
        metrics_.phase = frames_ && capacity_ && preroll_ < capacity_ &&
            confirmation_ && hold_limit_ ? Phase::Armed : Phase::Fault;
    }
    void disable() { clear(); metrics_.phase = Phase::Disabled; }
    void fault() { clear(); metrics_.phase = Phase::Fault; }
    void setHandoffEnabled(bool enabled) { handoff_enabled_ = enabled; }
    // 调用方通过采集屏障串行保护两个对象，并在接管前验证回答身份。
    // 目标校验失败会保留双方缓冲，不能悄悄丢弃起始 PCM。
    bool handoff(AutomaticUtterance &destination)
    {
        if (!handoff_enabled_ || metrics_.phase != Phase::Confirmed ||
            !destination.adoptConfirmed(frames_, capacity_, head_, size_,
                                        metrics_.candidate_offset, metrics_.confirmed_offset)) return false;
        disable();
        return true;
    }
    Snapshot snapshot() const
    {
        auto result = metrics_;
        result.buffered_frames = size_;
        result.oldest_offset = size_ ? frames_[head_].offset : 0;
        return result;
    }

    void append(const int16_t *pcm, size_t count, uint64_t offset, bool speech)
    {
        if (metrics_.phase == Phase::Disabled || metrics_.phase == Phase::Fault ||
            offset < first_sample_) return;
        if (!pcm || count != kFrameSamples ||
            offset > std::numeric_limits<uint64_t>::max() - count ||
            (have_offset_ && offset != metrics_.next_offset)) {
            ++metrics_.discontinuities;
            fault();
            return;
        }
        have_offset_ = true;
        metrics_.next_offset = offset + count;
        // 测量当前这一帧输出，不依赖异步 AFE 诊断。保留 PCM 及原始 VAD 标签。
        // 仅在统计时去除帧均值，可拒绝直流/恒定偏置，
        // 不会过滤整段话，也不会丢失预录样本。
        int64_t sum = 0;
        uint64_t squares = 0;
        uint32_t peak = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t sample = pcm[i];
            sum += sample;
            squares += static_cast<int64_t>(sample) * sample;
            peak = std::max(peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
        }
        // 每一项最多为 320^2 * 32768^2（小于 2^47）。
        const uint64_t variance = kFrameSamples * squares - static_cast<uint64_t>(sum * sum);
        metrics_.last_frame_offset = offset;
        metrics_.frame_mean = static_cast<int32_t>(sum / static_cast<int64_t>(count));
        metrics_.frame_rms = squareRoot(squares / kFrameSamples);
        metrics_.frame_ac_rms = squareRoot(variance / kFrameSamples / kFrameSamples);
        metrics_.frame_peak = peak;
        metrics_.frame_vad = speech;

        if (metrics_.phase == Phase::Cooldown) {
            quiet_frames_ = speech ? 0 : quiet_frames_ + 1;
            // 同一段持续噪声/语音不能确认两次。
            // 后续安静可形成有效的新历史，旧候选 PCM 已清除。
            if (!speech && !pushHistory(pcm, offset, false)) return;
            if (speech) clearBuffer();
            if (quiet_frames_ >= kQuietFrames) {
                quiet_frames_ = 0;
                metrics_.phase = Phase::Armed;
            }
            return;
        }

        if (metrics_.phase == Phase::Armed) {
            if (!speech) { pushHistory(pcm, offset, false); return; }
            metrics_.phase = Phase::Candidate;
            metrics_.candidate_offset = offset;
            metrics_.confirmed_offset = 0;
            metrics_.speech_frames = 0;
            window_variance_ = 0;
            metrics_.window_ac_rms = 0;
            metrics_.window_frames = 0;
            energy_active_frames_ = 0;
            energy_tail_mask_ = 0;
            metrics_.energy_active_frames = 0;
            metrics_.energy_tail_active_frames = 0;
            ++metrics_.candidates;
        }

        if (metrics_.phase == Phase::Candidate) {
            if (!speech) {
                ++metrics_.rejected;
                metrics_.speech_frames = 0;
                metrics_.phase = Phase::Armed;
                // 仅保留最新预录段，包括当前安静帧。
                pushHistory(pcm, offset, false);
                return;
            }
            if (!push(pcm, offset, true)) return;
            if (window_variance_ > std::numeric_limits<uint64_t>::max() - variance ||
                metrics_.speech_frames == std::numeric_limits<uint32_t>::max()) {
                ++metrics_.overflows;
                fault();
                return;
            }
            window_variance_ += variance;
            ++metrics_.speech_frames;
            metrics_.window_frames = metrics_.speech_frames;
            const bool energy_active = metrics_.frame_ac_rms >= metrics_.min_ac_rms;
            if (energy_active) {
                ++energy_active_frames_;
                metrics_.energy_active_frames = energy_active_frames_;
            }
            energy_tail_mask_ = static_cast<uint8_t>(((energy_tail_mask_ << 1U) |
                                                       (energy_active ? 1U : 0U)) & kEnergyTailMask);
            metrics_.energy_tail_active_frames = popcount(energy_tail_mask_);
            const uint64_t denominator = static_cast<uint64_t>(metrics_.speech_frames) *
                                         kFrameSamples * kFrameSamples;
            const uint64_t mean_ac_energy = window_variance_ / denominator;
            metrics_.window_ac_rms = squareRoot(mean_ac_energy);
            if (metrics_.speech_frames >= confirmation_) {
                // 对整个连续 VAD 窗口应用门限，而非单独的 20 ms 音素；
                // 短暂安静或轻声帧不能重置窗口。80 PCM16 只是初始工程下限，
                // 并非校准后的扬声器/回声分类器，交流残余回声仍可能通过。
                // D2 还要求多数帧及窗口尾部具备能量，
                // 防止单个脉冲后的 SDK VAD 拖尾变成真实插话。
                const uint64_t minimum = metrics_.min_ac_rms;
                const size_t minimum_active_frames = std::max<size_t>(
                    kMinimumActiveEnergyFrames, (confirmation_ + 1) / 2);
                const bool persistent_energy = !handoff_enabled_ ||
                    (metrics_.energy_active_frames >= minimum_active_frames &&
                     metrics_.energy_tail_active_frames >= kMinimumTailActiveFrames);
                if (mean_ac_energy < minimum * minimum || !persistent_energy) {
                    ++metrics_.energy_rejections;
                    cooldown();
                    return;
                }
                ++metrics_.confirmations;
                metrics_.confirmed_offset = metrics_.next_offset;
                held_frames_ = 0;
                metrics_.phase = Phase::Confirmed;
            }
            return;
        }

        if (metrics_.phase == Phase::Confirmed) {
            if (!speech && !handoff_enabled_) {
                cooldown();
                quiet_frames_ = 1;
                pushHistory(pcm, offset, false);
                return;
            }
            if (!push(pcm, offset, speech)) return;
            if (speech) ++metrics_.speech_frames;
            if (++held_frames_ >= hold_limit_) {
                ++metrics_.timeouts;
                cooldown();
            }
        }
    }

private:
    static constexpr size_t kFrameSamples = 320;
    static constexpr uint32_t kQuietFrames = 15;  // 重新布防前等待 300 ms。
    static constexpr uint8_t kEnergyTailMask = 0x7;
    static constexpr uint32_t kMinimumActiveEnergyFrames = 3;
    static constexpr uint32_t kMinimumTailActiveFrames = 2;
    // 所有输入均为 PCM16 均方值（不超过 32768^2）。固定十六步整数开方
    // 避免在回调中使用浮点，且不参与阈值判断；阈值使用精确平方值。
    static uint32_t squareRoot(uint64_t value)
    {
        uint64_t root = 0;
        for (uint64_t bit = uint64_t{1} << 30; bit; bit >>= 2) {
            if (value >= root + bit) {
                value -= root + bit;
                root = (root >> 1) + bit;
            } else root >>= 1;
        }
        return static_cast<uint32_t>(root);
    }
    static uint32_t popcount(uint8_t value)
    {
        uint32_t count = 0;
        while (value) {
            value &= static_cast<uint8_t>(value - 1U);
            ++count;
        }
        return count;
    }
    void clearBuffer() { head_ = size_ = 0; }
    void clear()
    {
        clearBuffer();
        metrics_.speech_frames = 0;
        window_variance_ = 0;
        quiet_frames_ = held_frames_ = 0;
        energy_active_frames_ = 0;
        energy_tail_mask_ = 0;
    }
    void cooldown() { clear(); metrics_.phase = Phase::Cooldown; }
    bool pushHistory(const int16_t *pcm, uint64_t offset, bool speech)
    {
        while (size_ && size_ >= preroll_) discardOldest();
        return preroll_ ? push(pcm, offset, speech) : true;
    }
    void discardOldest() { head_ = (head_ + 1) % capacity_; --size_; }
    bool push(const int16_t *pcm, uint64_t offset, bool speech)
    {
        if (!frames_ || size_ >= capacity_) {
            ++metrics_.overflows;
            fault();
            return false;
        }
        auto &frame = frames_[(head_ + size_) % capacity_];
        std::copy_n(pcm, kFrameSamples, frame.pcm.begin());
        frame.offset = offset;
        frame.speech = speech;
        ++size_;
        metrics_.buffered_peak = std::max(metrics_.buffered_peak, size_);
        return true;
    }

    Frame *frames_ = nullptr;
    size_t capacity_ = 0, preroll_ = 0, confirmation_ = 0, hold_limit_ = 0;
    size_t head_ = 0, size_ = 0, held_frames_ = 0;
    uint32_t quiet_frames_ = 0;
    uint64_t first_sample_ = 0;
    uint64_t window_variance_ = 0;
    uint32_t energy_active_frames_ = 0;
    uint8_t energy_tail_mask_ = 0;
    bool have_offset_ = false;
    bool handoff_enabled_ = false;
    Snapshot metrics_{};
};

}  // 命名空间 voiceaudio
