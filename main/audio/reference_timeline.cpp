#include "reference_timeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <new>
#include "esp_heap_caps.h"

namespace voiceaudio {
namespace {
constexpr double kPhaseCorrectionMaxUs = 25.0;
constexpr double kRateSlewPpm = 25.0;
constexpr int64_t kRateWindowUs = 1000000;
constexpr int64_t kMaxClockJumpUs = 50000;
constexpr double kPositionTolerance = 1.0e-6;
struct Point {
    bool available = false;
    double position = 0.0;
    int16_t pcm = 0;
    uint32_t generation = 0, serial = 0;
    uint64_t output_sample = 0;
};
}  // 命名空间

ReferenceTimeline::~ReferenceTimeline() { stop(); }
bool ReferenceTimeline::start()
{
    if (blocks_) return false;
    blocks_ = static_cast<AlignedBlock *>(heap_caps_malloc(
        sizeof(AlignedBlock) * kHistoryBlocks, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!blocks_) return false;
    for (size_t i = 0; i < kHistoryBlocks; ++i) new (blocks_ + i) AlignedBlock{};
    write_index_ = block_count_ = 0;
    microphone_clock_ = playback_clock_ = {};
    current_generation_ = last_serial_ = 0;
    expected_output_sample_16k_ = 0;
    microphone_overflow_events_ = aligned_blocks_ = aligned_samples_ = unmapped_blocks_ = 0;
    history_overwrites_ = gap_blocks_ = clock_errors_ = generation_changes_ = 0;
    max_clock_residual_us_ = 0;
    clock_rate_suspect_windows_ = 0;
    first_clock_failure_ = {};
    last_anchor_delta_us_ = 0;
    next_microphone_sample_ = 0.0;
    have_continuous_mapping_ = false;
    healthy_ = true;
    return true;
}
void ReferenceTimeline::stop()
{
    if (blocks_) heap_caps_free(blocks_);
    blocks_ = nullptr;
    write_index_ = block_count_ = 0;
    healthy_ = false;
}
int32_t ReferenceTimeline::ratePpm(const Clock &clock, uint32_t nominal_rate)
{
    return clock.ready ? static_cast<int32_t>(std::llround(
        (clock.rate_hz / nominal_rate - 1.0) * 1000000.0)) : 0;
}
const char *ReferenceTimeline::clockSourceName(ClockSource source)
{
    switch (source) {
    case ClockSource::Microphone: return "microphone";
    case ClockSource::Playback: return "playback";
    case ClockSource::Mapping: return "mapping";
    default: return "none";
    }
}
const char *ReferenceTimeline::clockFailureReasonName(ClockFailureReason reason)
{
    switch (reason) {
    case ClockFailureReason::InvalidTimestamp: return "invalid_timestamp";
    case ClockFailureReason::NonMonotonic: return "non_monotonic";
    case ClockFailureReason::ClockJump: return "clock_jump";
    case ClockFailureReason::RateOutOfRange: return "rate_out_of_range";
    case ClockFailureReason::MicrophoneInvalid: return "microphone_invalid";
    case ClockFailureReason::MicrophoneOverflow: return "microphone_overflow";
    case ClockFailureReason::MappingPhase: return "mapping_phase";
    case ClockFailureReason::MappingStep: return "mapping_step";
    default: return "none";
    }
}
bool ReferenceTimeline::rejectClock(ClockSource source, ClockFailureReason reason,
                                     uint64_t sample_end, int64_t end_us, double residual_us,
                                     double observed_ppm, uint64_t window_us, uint32_t bad_windows)
{
    if (!first_clock_failure_.valid) {
        const auto bounded_signed = [](double value, double low, double high) {
            return std::isnan(value) ? 0.0 : std::clamp(value, low, high);
        };
        // INT64_MAX 下方可精确表示的最大 double，
        // 避免无效坐标超出模型范围时发生越界转换。
        const double max_i64 = std::nextafter(static_cast<double>(INT64_MAX), 0.0);
        first_clock_failure_ = {true, source, reason, sample_end, end_us,
            static_cast<int64_t>(bounded_signed(residual_us, static_cast<double>(INT64_MIN), max_i64)),
            static_cast<int32_t>(bounded_signed(observed_ppm, INT32_MIN, INT32_MAX)),
            static_cast<uint32_t>(std::min<uint64_t>(window_us, UINT32_MAX)), bad_windows};
    }
    if (healthy_) ++clock_errors_;
    healthy_ = false;
    return false;
}
bool ReferenceTimeline::observeClock(Clock *clock, uint64_t sample_end, int64_t end_us,
                                     uint32_t nominal_rate, ClockSource source)
{
    if (end_us <= 0) return rejectClock(source, ClockFailureReason::InvalidTimestamp, sample_end, end_us);
    if (!clock->ready) {
        *clock = {sample_end, end_us, sample_end, end_us, static_cast<double>(end_us),
                  static_cast<double>(nominal_rate), true};
        return true;
    }
    if (sample_end == clock->latest_samples && end_us == clock->latest_observed_us) return true;
    if (sample_end <= clock->latest_samples || end_us <= clock->latest_observed_us)
        return rejectClock(source, ClockFailureReason::NonMonotonic, sample_end, end_us);
    const double predicted_us = clock->modeled_end_us +
        static_cast<double>(sample_end - clock->latest_samples) * 1000000.0 / clock->rate_hz;
    const double residual_us = static_cast<double>(end_us) - predicted_us;
    const double absolute_residual_us = std::abs(residual_us);
    if (!std::isfinite(absolute_residual_us) || absolute_residual_us > std::numeric_limits<uint32_t>::max())
        return rejectClock(source, ClockFailureReason::ClockJump, sample_end, end_us, residual_us);
    max_clock_residual_us_ = std::max(max_clock_residual_us_,
        static_cast<uint32_t>(std::ceil(absolute_residual_us)));
    if (absolute_residual_us > kMaxClockJumpUs)
        return rejectClock(source, ClockFailureReason::ClockJump, sample_end, end_us, residual_us);
    const int64_t elapsed_us = end_us - clock->baseline_us;
    if (elapsed_us >= kRateWindowUs) {
        const double observed_rate = static_cast<double>(sample_end - clock->baseline_samples) *
                                     1000000.0 / elapsed_us;
        const double observed_ppm = (observed_rate / nominal_rate - 1.0) * 1000000.0;
        if (!std::isfinite(observed_rate) || std::abs(observed_ppm) > kMaxClockErrorPpm) {
            // 这些是 ISR 到达时刻，不是硬件锁存的 EOF 时刻。
            // 一个迟到端点会在相邻窗口造成方向相反的虚假速率。
            // 重新设置独立窗口基准但不接受其速率；持续同向误差必须再确认一次。
            const int32_t sign = observed_ppm < 0.0 ? -1 : 1;
            ++clock_rate_suspect_windows_;
            if (clock->suspect_rate_sign == sign)
                return rejectClock(source, ClockFailureReason::RateOutOfRange,
                    sample_end, end_us, residual_us, observed_ppm, elapsed_us, 2);
            clock->suspect_rate_sign = sign;
        } else {
            clock->suspect_rate_sign = 0;
            const double max_rate_step = nominal_rate * kRateSlewPpm / 1000000.0;
            clock->rate_hz += std::clamp((observed_rate - clock->rate_hz) * 0.25,
                                        -max_rate_step, max_rate_step);
        }
        clock->baseline_samples = sample_end;
        clock->baseline_us = end_us;
    }
    clock->modeled_end_us = predicted_us + std::clamp(residual_us * 0.05,
        -kPhaseCorrectionMaxUs, kPhaseCorrectionMaxUs);
    clock->latest_samples = sample_end;
    clock->latest_observed_us = end_us;
    return true;
}
void ReferenceTimeline::observeMicrophoneDma(uint64_t sample_end, int64_t observed_end_us,
                                             uint32_t overflow_events, bool timeline_valid)
{
    if (!blocks_) return;
    microphone_overflow_events_ = overflow_events;
    if (!healthy_) return;
    if (timeline_valid && !overflow_events && !sample_end && !observed_end_us) return;
    if (overflow_events)
        rejectClock(ClockSource::Microphone, ClockFailureReason::MicrophoneOverflow, sample_end, observed_end_us);
    else if (!timeline_valid)
        rejectClock(ClockSource::Microphone, ClockFailureReason::MicrophoneInvalid, sample_end, observed_end_us);
    else
        observeClock(&microphone_clock_, sample_end, observed_end_us, 16000, ClockSource::Microphone);
}
bool ReferenceTimeline::append(uint32_t dma_generation, uint32_t playback_serial,
                                uint64_t tx_end_sample_24k, int64_t tx_end_us,
                                uint64_t first_output_sample_16k,
                                int64_t first_content_sample_16k_thirds,
                                const int16_t *samples, size_t sample_count, size_t valid_from,
                                bool interval_valid)
{
    if (!blocks_ || !samples || !sample_count || sample_count > kSamplesPerBlock ||
        valid_from > sample_count || !dma_generation ||
        first_output_sample_16k > std::numeric_limits<uint64_t>::max() - sample_count) return false;
    if (dma_generation != current_generation_) {
        if (current_generation_ && dma_generation < current_generation_) {
            ++unmapped_blocks_; return false;
        }
        if (current_generation_) ++generation_changes_;
        playback_clock_ = {};
        current_generation_ = dma_generation;
        expected_output_sample_16k_ = first_output_sample_16k;
        last_serial_ = playback_serial;
        have_continuous_mapping_ = false;
    }
    const bool contiguous = first_output_sample_16k == expected_output_sample_16k_;
    if (!contiguous || !interval_valid) ++gap_blocks_;
    if (!contiguous) have_continuous_mapping_ = false;
    expected_output_sample_16k_ = first_output_sample_16k + sample_count;
    last_serial_ = playback_serial;
    if (!healthy_ || !microphone_clock_.ready ||
        !observeClock(&playback_clock_, tx_end_sample_24k, tx_end_us, 24000, ClockSource::Playback)) {
        ++unmapped_blocks_; have_continuous_mapping_ = false; return false;
    }
    const int64_t anchor_delta_us = tx_end_us - microphone_clock_.latest_observed_us;
    if (std::abs(anchor_delta_us) > kMaxAnchorDistanceUs) {
        ++unmapped_blocks_; have_continuous_mapping_ = false; return false;
    }
    last_anchor_delta_us_ = static_cast<int32_t>(anchor_delta_us);
    const double content_tx_sample = static_cast<double>(first_content_sample_16k_thirds) * 0.5;
    const double first_content_us = playback_clock_.modeled_end_us +
        (content_tx_sample - static_cast<double>(tx_end_sample_24k)) * 1000000.0 /
        playback_clock_.rate_hz;
    double first_microphone_sample = static_cast<double>(microphone_clock_.latest_samples) +
        (first_content_us - microphone_clock_.modeled_end_us) * microphone_clock_.rate_hz / 1000000.0;
    double step = microphone_clock_.rate_hz * 1.5 / playback_clock_.rate_hz;
    if (have_continuous_mapping_) {
        const double phase_error_samples = first_microphone_sample - next_microphone_sample_;
        if (std::abs(phase_error_samples) > 800.0) {
            ++unmapped_blocks_;
            return rejectClock(ClockSource::Mapping, ClockFailureReason::MappingPhase,
                tx_end_sample_24k, tx_end_us, phase_error_samples * 1000000.0 / 16000.0);
        }
        // 保持上一精确端点。通过有界改变小数步长，让相位趋近时钟拟合，
        // 不丢块、不重复块，也不在 DMA 边界产生位置跳变。
        first_microphone_sample = next_microphone_sample_;
        step += std::clamp(phase_error_samples / (sample_count * 50.0), -0.0002, 0.0002);
    }
    if (!std::isfinite(first_microphone_sample) || !std::isfinite(step) || step < 0.995 || step > 1.005) {
        ++unmapped_blocks_;
        return rejectClock(ClockSource::Mapping, ClockFailureReason::MappingStep,
            tx_end_sample_24k, tx_end_us, 0.0, (step - 1.0) * 1000000.0);
    }
    AlignedBlock &block = blocks_[write_index_];
    block.dma_generation = dma_generation;
    block.playback_serial = playback_serial;
    block.first_output_sample_16k = first_output_sample_16k;
    block.first_microphone_sample = first_microphone_sample;
    block.microphone_step = step;
    block.sample_count = static_cast<uint16_t>(sample_count);
    block.valid_from = static_cast<uint16_t>(interval_valid ? valid_from : sample_count);
    block.first_valid_microphone_sample = first_microphone_sample + block.valid_from * step;
    block.last_valid_microphone_sample = first_microphone_sample + (sample_count - 1) * step;
    block.first_integer_microphone_sample = std::ceil(block.first_valid_microphone_sample - kPositionTolerance);
    block.last_integer_microphone_sample = std::floor(block.last_valid_microphone_sample + kPositionTolerance);
    std::copy_n(samples, sample_count, block.pcm);
    next_microphone_sample_ = first_microphone_sample + sample_count * step;
    have_continuous_mapping_ = true;
    write_index_ = (write_index_ + 1) % kHistoryBlocks;
    if (block_count_ < kHistoryBlocks) ++block_count_;
    else ++history_overwrites_;
    ++aligned_blocks_;
    aligned_samples_ += static_cast<uint32_t>(sample_count - block.valid_from);
    return interval_valid;
}
bool ReferenceTimeline::copyMicrophoneFrame(uint64_t first_microphone_sample, size_t sample_count,
                                             int16_t *destination, size_t capacity,
                                             uint32_t dma_generation, uint32_t playback_serial,
                                             size_t *valid_samples, uint8_t *valid_mask) const
{
    if (valid_samples) *valid_samples = 0;
    if (!destination || sample_count > capacity || sample_count > kMaxQuerySamples ||
        first_microphone_sample > std::numeric_limits<uint64_t>::max() - sample_count) return false;
    std::fill_n(destination, sample_count, 0);
    if (valid_mask) std::fill_n(valid_mask, sample_count, 0);
    if (!blocks_ || !healthy_) return false;
    std::array<uint8_t, kMaxQuerySamples> matched_mask{};
    size_t matched = 0;
    const double query_begin = static_cast<double>(first_microphone_sample);
    const double query_end = query_begin + static_cast<double>(sample_count);
    const double query_last = query_end - 1.0;
    // 有意放宽一个样本：预筛选不得改变小数/容差边界处
    // 原本的 ceil/floor 判断。
    const double bridge_minimum_first = query_begin - 1.0;
    const auto write_sample = [&](size_t offset, float value) {
        value = std::clamp(value, static_cast<float>(std::numeric_limits<int16_t>::min()),
                           static_cast<float>(std::numeric_limits<int16_t>::max()));
        destination[offset] = static_cast<int16_t>(std::nearbyint(value));
        if (!matched_mask[offset]) { matched_mask[offset] = 1; ++matched; }
    };
    Point previous_last{};
    const size_t oldest = (write_index_ + kHistoryBlocks - block_count_) % kHistoryBlocks;
    for (size_t index = 0; index < block_count_; ++index) {
        const AlignedBlock &block = blocks_[(oldest + index) % kHistoryBlocks];
        if (block.valid_from >= block.sample_count ||
            (dma_generation && block.dma_generation != dma_generation) ||
            (playback_serial && block.playback_serial != playback_serial)) {
            previous_last.available = false;
            continue;
        }
        const double first_position = block.first_valid_microphone_sample;
        const double last_position = block.last_valid_microphone_sample;
        const uint64_t first_output = block.first_output_sample_16k + block.valid_from;
        // 仅连接相邻的有效源样本，包括跨 DMA 边界的小数区间。
        // 无效块或索引缺口始终打断连接，即使两侧值都为零。
        if (previous_last.available && previous_last.generation == block.dma_generation &&
            (!playback_serial || previous_last.serial == block.playback_serial) &&
            previous_last.output_sample + 1 == first_output &&
            previous_last.position <= query_end &&
            first_position >= bridge_minimum_first) {
            const double distance = first_position - previous_last.position;
            if (distance > kPositionTolerance && distance <= 1.01) {
                const double begin = std::max(query_begin, std::ceil(previous_last.position - kPositionTolerance));
                const double end = std::min(query_last, std::floor(first_position + kPositionTolerance));
                if (begin <= end) {
                    const float inverse_distance = static_cast<float>(1.0 / distance);
                    for (double target = begin; target <= end; target += 1.0) {
                        const size_t offset = static_cast<size_t>(target - query_begin);
                        const float fraction = std::clamp(static_cast<float>(target - previous_last.position) * inverse_distance, 0.0f, 1.0f);
                        write_sample(offset, previous_last.pcm + fraction *
                            (static_cast<float>(block.pcm[block.valid_from]) - previous_last.pcm));
                    }
                }
            }
        }
        const double begin = std::max(query_begin, block.first_integer_microphone_sample);
        const double end = std::min(query_last, block.last_integer_microphone_sample);
        if (begin <= end) {
            const size_t first_offset = static_cast<size_t>(begin - query_begin);
            const size_t last_offset = static_cast<size_t>(end - query_begin);
            float fractional_index = static_cast<float>((begin - block.first_microphone_sample) / block.microphone_step);
            const float increment = static_cast<float>(1.0 / block.microphone_step);
            for (size_t offset = first_offset; offset <= last_offset; ++offset) {
                const size_t left = std::clamp<size_t>(static_cast<size_t>(std::max(0.0f, fractional_index)),
                    block.valid_from, block.sample_count - 1);
                const size_t right = std::min<size_t>(left + 1, block.sample_count - 1);
                const float fraction = std::clamp(fractional_index - static_cast<float>(left), 0.0f, 1.0f);
                write_sample(offset, block.pcm[left] + fraction *
                    (static_cast<float>(block.pcm[right]) - block.pcm[left]));
                fractional_index += increment;
            }
        }
        previous_last = {true, last_position, block.pcm[block.sample_count - 1],
                         block.dma_generation, block.playback_serial,
                         block.first_output_sample_16k + block.sample_count - 1};
    }
    if (valid_mask) std::copy_n(matched_mask.data(), sample_count, valid_mask);
    if (valid_samples) *valid_samples = matched;
    return matched == sample_count;
}
ReferenceTimeline::Metrics ReferenceTimeline::metrics() const
{
    Metrics result{};
    result.available = blocks_ != nullptr;
    result.healthy = healthy_ && microphone_clock_.ready && playback_clock_.ready;
    result.aligned_blocks = aligned_blocks_;
    result.aligned_samples = aligned_samples_;
    result.unmapped_blocks = unmapped_blocks_;
    result.history_overwrites = history_overwrites_;
    result.microphone_rate_hz = microphone_clock_.ready
        ? static_cast<uint32_t>(std::llround(microphone_clock_.rate_hz)) : 16000;
    result.playback_rate_hz = playback_clock_.ready
        ? static_cast<uint32_t>(std::llround(playback_clock_.rate_hz)) : 24000;
    result.microphone_rate_ppm = ratePpm(microphone_clock_, 16000);
    result.playback_rate_ppm = ratePpm(playback_clock_, 24000);
    result.last_anchor_delta_us = last_anchor_delta_us_;
    result.microphone_overflow_events = microphone_overflow_events_;
    result.blocks_available = static_cast<uint32_t>(block_count_);
    result.gap_blocks = gap_blocks_;
    result.clock_errors = clock_errors_;
    result.generation_changes = generation_changes_;
    result.max_clock_residual_us = max_clock_residual_us_;
    result.clock_rate_suspect_windows = clock_rate_suspect_windows_;
    const HealthRange range = healthRange();
    result.reference_range_available = range.reference_range_available;
    result.oldest_reference_microphone_sample = range.oldest_reference_microphone_sample;
    result.latest_reference_microphone_sample_end = range.latest_reference_microphone_sample_end;
    return result;
}
ReferenceTimeline::HealthRange ReferenceTimeline::healthRange() const
{
    HealthRange result{};
    result.healthy = healthy_ && microphone_clock_.ready && playback_clock_.ready;
    result.unmapped_blocks = unmapped_blocks_;
    const auto integer_range = [](const AlignedBlock &block, uint64_t *first, uint64_t *end) {
        if (block.valid_from >= block.sample_count) return false;
        const double begin = block.first_integer_microphone_sample;
        const double last = block.last_integer_microphone_sample;
        // 一个代次可能开始于 RX 样本零之前。
        // 绝不能把负数、非有限或越界浮点值转换为 uint64。
        if (!std::isfinite(begin) || !std::isfinite(last) || last < 0.0 || begin > last ||
            last >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return false;
        *first = begin <= 0.0 ? 0 : static_cast<uint64_t>(begin);
        *end = static_cast<uint64_t>(last) + 1;
        return true;
    };
    // 不查询 PCM，也不增加存储。两次边界搜索均由固定的 30 块环约束，
    // 通常各检查一块。
    const size_t oldest = (write_index_ + kHistoryBlocks - block_count_) % kHistoryBlocks;
    uint64_t first = 0, end = 0;
    for (size_t i = 0; i < block_count_; ++i) {
        if (integer_range(blocks_[(oldest + i) % kHistoryBlocks], &first, &end)) {
            result.reference_range_available = true;
            result.oldest_reference_microphone_sample = first;
            break;
        }
    }
    if (result.reference_range_available)
        for (size_t i = 0; i < block_count_; ++i) {
            const size_t index = (write_index_ + kHistoryBlocks - 1 - i) % kHistoryBlocks;
            if (integer_range(blocks_[index], &first, &end)) {
                result.latest_reference_microphone_sample_end = end;
                break;
            }
        }
    return result;
}
}  // 命名空间 voiceaudio
