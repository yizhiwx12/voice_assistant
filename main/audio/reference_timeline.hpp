#pragma once

#include <cstddef>
#include <cstdint>

namespace voiceaudio {

// 仅在串行化任务上下文使用。这是观测到的数字时间线；
// DMA 时间戳并未校准扬声器、房间、麦克风或 AFE 的声学延迟。
class ReferenceTimeline {
public:
    static constexpr size_t kSamplesPerBlock = 160;
    static constexpr size_t kHistoryBlocks = 30;  // 16 kHz 下对应 300 ms。
    static constexpr size_t kMaxQuerySamples = 320;
    static constexpr uint32_t kMaxAnchorDistanceUs = 250000;
    static constexpr int32_t kMaxClockErrorPpm = 2000;
    enum class ClockSource : uint32_t { None, Microphone, Playback, Mapping };
    enum class ClockFailureReason : uint32_t {
        None, InvalidTimestamp, NonMonotonic, ClockJump, RateOutOfRange,
        MicrophoneInvalid, MicrophoneOverflow, MappingPhase, MappingStep
    };
    struct FirstClockFailure {
        bool valid = false;
        ClockSource source = ClockSource::None;
        ClockFailureReason reason = ClockFailureReason::None;
        uint64_t sample_end = 0;
        int64_t observed_end_us = 0;
        int64_t residual_us = 0;
        int32_t observed_ppm = 0;
        uint32_t window_us = 0;
        uint32_t consecutive_bad_windows = 0;
    };
    struct Metrics {
        bool available = false;
        bool healthy = false;
        bool acoustically_calibrated = false;
        uint32_t aligned_blocks = 0;
        uint32_t aligned_samples = 0;
        uint32_t unmapped_blocks = 0;
        uint32_t history_overwrites = 0;
        uint32_t microphone_rate_hz = 16000;
        uint32_t playback_rate_hz = 24000;
        int32_t microphone_rate_ppm = 0;
        int32_t playback_rate_ppm = 0;
        int32_t last_anchor_delta_us = 0;
        uint32_t microphone_overflow_events = 0;
        uint32_t blocks_available = 0;
        uint32_t gap_blocks = 0;
        uint32_t clock_errors = 0;
        uint32_t generation_changes = 0;
        uint32_t max_clock_residual_us = 0;
        uint32_t clock_rate_suspect_windows = 0;
        // 保留的映射有效源点包络，舍入到麦克风整数网格。
        // 不保证两端点间连续覆盖；查询仍检查缺口、身份及健康状态。
        bool reference_range_available = false;
        uint64_t oldest_reference_microphone_sample = 0;
        uint64_t latest_reference_microphone_sample_end = 0;  // 不包含该端点。
    };
    // 必要配对路径的任务持有者快照，使用追加时缓存的整数边界，
    // 避免时钟速率舍入/ppm 计算。与 Metrics 一样，此包络不保证连续覆盖。
    struct HealthRange {
        bool healthy = false;
        bool reference_range_available = false;
        uint32_t unmapped_blocks = 0;
        uint64_t oldest_reference_microphone_sample = 0;
        uint64_t latest_reference_microphone_sample_end = 0;
    };
    ReferenceTimeline() = default;
    ~ReferenceTimeline();
    ReferenceTimeline(const ReferenceTimeline &) = delete;
    ReferenceTimeline &operator=(const ReferenceTimeline &) = delete;
    bool start();
    void stop();
    void observeMicrophoneDma(uint64_t sample_end, int64_t observed_end_us,
                              uint32_t overflow_events, bool timeline_valid = true);
    // 内容坐标以源 16 kHz 样本的三分之一表示（当前 FIR 为 3*m-95）；
    // 对应的 24 kHz 源坐标为 coordinate/2。
    // interval_valid=false 保存无效区间，绝不表示有效静音。
    bool append(uint32_t dma_generation, uint32_t playback_serial,
                uint64_t tx_end_sample_24k, int64_t tx_end_us,
                uint64_t first_output_sample_16k,
                int64_t first_content_sample_16k_thirds,
                const int16_t *samples, size_t sample_count, size_t valid_from,
                bool interval_valid = true);
    // 插值到精确 RX 样本位置。缺失位置写入零且 mask=0，
    // valid_samples 不统计缺失位置。真表示整帧有效。
    bool copyMicrophoneFrame(uint64_t first_microphone_sample, size_t sample_count,
                             int16_t *destination, size_t capacity,
                             uint32_t dma_generation = 0, uint32_t playback_serial = 0,
                             size_t *valid_samples = nullptr, uint8_t *valid_mask = nullptr) const;
    Metrics metrics() const;
    HealthRange healthRange() const;
    FirstClockFailure firstClockFailure() const { return first_clock_failure_; }
    static const char *clockSourceName(ClockSource source);
    static const char *clockFailureReasonName(ClockFailureReason reason);
private:
    struct AlignedBlock {
        uint32_t dma_generation = 0;
        uint32_t playback_serial = 0;
        uint64_t first_output_sample_16k = 0;
        double first_microphone_sample = 0.0;
        double microphone_step = 1.0;
        double first_valid_microphone_sample = 0.0;
        double last_valid_microphone_sample = 0.0;
        // 整数值 double 保持精确查询算术，包括负的初始位置；
        // 仅在 healthRange() 通过与诊断相同的边界检查后，才转换为 uint64。
        double first_integer_microphone_sample = 0.0;
        double last_integer_microphone_sample = 0.0;
        uint16_t sample_count = 0;
        uint16_t valid_from = 0;
        int16_t pcm[kSamplesPerBlock]{};
    };
    struct Clock {
        uint64_t baseline_samples = 0;
        int64_t baseline_us = 0;
        uint64_t latest_samples = 0;
        int64_t latest_observed_us = 0;
        double modeled_end_us = 0.0;
        double rate_hz = 0.0;
        bool ready = false;
        int32_t suspect_rate_sign = 0;
    };
    bool observeClock(Clock *clock, uint64_t sample_end, int64_t end_us,
                      uint32_t nominal_rate, ClockSource source);
    bool rejectClock(ClockSource source, ClockFailureReason reason, uint64_t sample_end,
                     int64_t end_us, double residual_us = 0.0, double observed_ppm = 0.0,
                     uint64_t window_us = 0, uint32_t bad_windows = 0);
    static int32_t ratePpm(const Clock &clock, uint32_t nominal_rate);
    AlignedBlock *blocks_ = nullptr;  // 位于 PSRAM，ISR 不访问。
    size_t write_index_ = 0;
    size_t block_count_ = 0;
    Clock microphone_clock_{}, playback_clock_{};
    uint32_t current_generation_ = 0, last_serial_ = 0;
    uint64_t expected_output_sample_16k_ = 0;
    uint32_t microphone_overflow_events_ = 0;
    uint32_t aligned_blocks_ = 0, aligned_samples_ = 0, unmapped_blocks_ = 0;
    uint32_t history_overwrites_ = 0, gap_blocks_ = 0, clock_errors_ = 0;
    uint32_t generation_changes_ = 0, max_clock_residual_us_ = 0;
    uint32_t clock_rate_suspect_windows_ = 0;
    FirstClockFailure first_clock_failure_{};
    int32_t last_anchor_delta_us_ = 0;
    double next_microphone_sample_ = 0.0;
    bool have_continuous_mapping_ = false;
    bool healthy_ = false;
};
}  // 命名空间 voiceaudio
