#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace voiceaudio {

// 已接受输入流的顺序坐标中的诊断证据。
// AFE 算法延迟尚未校准，这不是波形对齐。
class SourceSignalHistory {
public:
    static constexpr size_t kFrameSamples = 320;
    static constexpr size_t kCapacity = 128;
    struct Signal {
        int32_t mean = 0;
        uint32_t rms = 0, ac_rms = 0, peak = 0, max_frame_ac_rms = 0;
    };
    struct Window {
        bool valid = false;
        bool ordinal_mapping = true, lag_calibrated = false;
        uint64_t requested_first = 0, requested_end = 0;
        uint64_t covered_first = 0, covered_end = 0;
        uint64_t available_first = 0, available_end = 0;
        uint32_t frames = 0, samples = 0;
        Signal raw, dc, reference;
    };
    struct Statistics {
        int64_t sum = 0;
        uint64_t squares = 0;
        uint32_t peak = 0;
    };

    static Statistics measure(const int16_t *samples)
    {
        Statistics result;
        for (size_t i = 0; i < kFrameSamples; ++i) {
            const int32_t sample = samples[i];
            result.sum += sample;
            result.squares += static_cast<int64_t>(sample) * sample;
            result.peak = std::max(result.peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
        }
        return result;
    }

    // 单一 feed 持有者。Raw/DC/reference 描述同一个已接受的 20 ms 帧。
    // 发布内容只有数值统计，绝不复制 PCM。
    bool append(const Statistics &raw, const Statistics &dc, const Statistics &reference)
    {
        if (written_end_ > std::numeric_limits<uint64_t>::max() - kFrameSamples) return false;
        auto &slot = slots_[(written_end_ / kFrameSamples) % kCapacity];
        const uint32_t before = slot.sequence.load(std::memory_order_seq_cst);
        slot.sequence.store(before + 1, std::memory_order_seq_cst);
        store64(slot.words, 0, written_end_);
        store(slot.words, 2, raw);
        store(slot.words, 7, dc);
        store(slot.words, 12, reference);
        slot.sequence.store(before + 2, std::memory_order_seq_cst);
        written_end_ += kFrameSamples;
        return true;
    }

    // 仅在 iface.feed() 成功后提交，不受输入适配器未填满帧影响。
    // fetch 可能与该返回竞争，此时查询报告不可用，
    // 而不等待，也不把未来 PCM 当作证据。
    void commit(size_t samples)
    {
        if (samples > written_end_ - committed_end_) return;
        committed_end_ += samples;
        const uint32_t before = frontier_sequence_.load(std::memory_order_seq_cst);
        frontier_sequence_.store(before + 1, std::memory_order_seq_cst);
        store64(frontier_, 0, written_end_);
        store64(frontier_, 2, committed_end_);
        frontier_sequence_.store(before + 2, std::memory_order_seq_cst);
    }

    Window window(uint64_t first, uint64_t end) const
    {
        Window result;
        result.requested_first = first;
        result.requested_end = end;
        uint64_t written = 0, committed = 0;
        if (!frontier(written, committed)) return result;
        result.available_first = written > kCapacity * kFrameSamples
            ? written - kCapacity * kFrameSamples : 0;
        result.available_end = committed / kFrameSamples * kFrameSamples;
        if (end <= first || end > std::numeric_limits<uint64_t>::max() - (kFrameSamples - 1)) return result;
        result.covered_first = first / kFrameSamples * kFrameSamples;
        result.covered_end = (end + kFrameSamples - 1) / kFrameSamples * kFrameSamples;
        if (result.covered_first < result.available_first || result.covered_end > result.available_end ||
            result.covered_end - result.covered_first > kCapacity * kFrameSamples) return result;
        std::array<Statistics, 3> totals{};
        std::array<uint32_t, 3> maxima{};
        for (uint64_t offset = result.covered_first; offset < result.covered_end; offset += kFrameSamples) {
            std::array<Statistics, 3> values{};
            if (!read(offset, values)) return result;
            for (size_t channel = 0; channel < values.size(); ++channel) {
                totals[channel].sum += values[channel].sum;
                totals[channel].squares += values[channel].squares;
                totals[channel].peak = std::max(totals[channel].peak, values[channel].peak);
                maxima[channel] = std::max(maxima[channel], describe(values[channel], kFrameSamples, 0).ac_rms);
            }
        }
        result.samples = static_cast<uint32_t>(result.covered_end - result.covered_first);
        result.frames = result.samples / kFrameSamples;
        result.raw = describe(totals[0], result.samples, maxima[0]);
        result.dc = describe(totals[1], result.samples, maxima[1]);
        result.reference = describe(totals[2], result.samples, maxima[2]);
        result.valid = true;
        return result;
    }

private:
    struct Slot {
        std::atomic<uint32_t> sequence{0};
        std::array<std::atomic<uint32_t>, 17> words{};
    };
    // 每个序列只有一个 feed 写者：用 load/store 发布奇偶序号，
    // 避免原子读改写辅助函数。全部共享字段保持为 32 位原子，
    // 采用 seq_cst 校验，没有应用层互斥锁。
    // 这并不声明目标库会把所有原子操作报告为无锁。
    static uint32_t squareRoot(uint64_t value)
    {
        uint64_t root = 0;
        for (uint64_t bit = uint64_t{1} << 62; bit; bit >>= 2) {
            if (value >= root + bit) { value -= root + bit; root = (root >> 1) + bit; }
            else root >>= 1;
        }
        return static_cast<uint32_t>(root);
    }
    static Signal describe(const Statistics &value, uint32_t count, uint32_t maximum)
    {
        Signal result;
        result.mean = static_cast<int32_t>(value.sum / static_cast<int64_t>(count));
        result.rms = squareRoot(value.squares / count);
        // 最多 40960 个 PCM16 样本，两项均小于 2^61。
        const uint64_t variance = count * value.squares - static_cast<uint64_t>(value.sum * value.sum);
        result.ac_rms = squareRoot(variance / count / count);
        result.peak = value.peak;
        result.max_frame_ac_rms = maximum;
        return result;
    }
    template <size_t N>
    static void store64(std::array<std::atomic<uint32_t>, N> &words, size_t index, uint64_t value)
    {
        words[index].store(static_cast<uint32_t>(value), std::memory_order_seq_cst);
        words[index + 1].store(static_cast<uint32_t>(value >> 32), std::memory_order_seq_cst);
    }
    template <size_t N>
    static uint64_t load64(const std::array<std::atomic<uint32_t>, N> &words, size_t index)
    {
        return words[index].load(std::memory_order_seq_cst) |
            (static_cast<uint64_t>(words[index + 1].load(std::memory_order_seq_cst)) << 32);
    }
    static int64_t signedValue(uint64_t bits)
    {
        // 逆转定义明确的有符号到无符号取模转换，
        // 不依赖由实现定义的越界无符号转换。
        return bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? static_cast<int64_t>(bits) : -1 - static_cast<int64_t>(~bits);
    }
    template <size_t N>
    static void store(std::array<std::atomic<uint32_t>, N> &words, size_t index, const Statistics &value)
    {
        store64(words, index, static_cast<uint64_t>(value.sum));
        store64(words, index + 2, value.squares);
        words[index + 4].store(value.peak, std::memory_order_seq_cst);
    }
    bool frontier(uint64_t &written, uint64_t &committed) const
    {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const uint32_t before = frontier_sequence_.load(std::memory_order_seq_cst);
            if (before & 1) continue;
            written = load64(frontier_, 0);
            committed = load64(frontier_, 2);
            if (frontier_sequence_.load(std::memory_order_seq_cst) == before) return true;
        }
        return false;
    }
    bool read(uint64_t offset, std::array<Statistics, 3> &values) const
    {
        const auto &slot = slots_[(offset / kFrameSamples) % kCapacity];
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const uint32_t before = slot.sequence.load(std::memory_order_seq_cst);
            if (!before || (before & 1)) continue;
            const uint64_t observed = load64(slot.words, 0);
            for (size_t channel = 0; channel < values.size(); ++channel) {
                const size_t index = 2 + channel * 5;
                values[channel].sum = signedValue(load64(slot.words, index));
                values[channel].squares = load64(slot.words, index + 2);
                values[channel].peak = slot.words[index + 4].load(std::memory_order_seq_cst);
            }
            if (slot.sequence.load(std::memory_order_seq_cst) == before) return observed == offset;
        }
        return false;
    }
    std::array<Slot, kCapacity> slots_{};
    std::atomic<uint32_t> frontier_sequence_{0};
    std::array<std::atomic<uint32_t>, 4> frontier_{};
    uint64_t written_end_ = 0, committed_end_ = 0;  // 仅 feed 持有者访问。
};

}  // 命名空间 voiceaudio
