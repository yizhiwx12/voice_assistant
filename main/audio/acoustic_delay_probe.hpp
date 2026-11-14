#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace voiceaudio {

// 用于受控纯远端测试的有界观测，仅在任务上下文执行。
// 参考信号已映射到 RX 样本时钟；正延迟表示
// microphone[i] 与 reference[1280 + i - lag] 相似。
// 
// 本类不改变时钟相位，不启用 AEC，也不声明校准完成。
// 仅靠相关性不能排除双讲；调用方/用户须确保诊断期间没有近端语音。
// 调用间隔不得短于 200 ms。
class AcousticDelayProbe {
public:
    static constexpr size_t kMicrophoneSamples = 320;
    static constexpr size_t kReferenceSamples = 1600;
    static constexpr size_t kMaximumDelaySamples = 1280;  // 16 kHz 下对应 80 ms。
    static constexpr size_t kDelayStepSamples = 16;       // 搜索网格间隔为 1 ms。
    static constexpr size_t kDecimation = 4;
    static constexpr uint32_t kMinimumMicrophoneRms = 8;
    static constexpr uint32_t kMinimumReferenceRms = 32;
    static constexpr uint32_t kMinimumCorrelationPermille = 650;
    static constexpr uint32_t kMinimumPeakMarginPermille = 80;
    static constexpr size_t kPlaybackScopes = 4;
    static constexpr size_t kDelayBins = kMaximumDelaySamples / kDelayStepSamples + 1;
    static constexpr size_t kRejectReasons = 9;  // 包括未使用的 None 分组。

    enum class RejectReason : uint8_t {
        None = 0,
        InvalidInput,
        IncompleteReference,
        LowMicrophoneEnergy,
        LowReferenceEnergy,
        LowCorrelation,
        BoundaryPeak,
        AmbiguousPeak,
        InsufficientPeakSupport,
    };

    struct Metrics {
        uint32_t attempts = 0;
        uint32_t accepted = 0;
        uint32_t rejected = 0;
        bool last_accepted = false;
        bool calibrated = false;  // 自动相关分析永远不会设置此标记。
        uint16_t best_delay_samples = 0;
        uint16_t correlation_per_mille = 0;  // 归一化相关系数的绝对值。
        uint16_t peak_margin_per_mille = 0;  // 最佳峰值与相距超过 2 ms 的次峰值之差。
        int8_t correlation_polarity = 0;
        uint32_t mic_ac_rms = 0;
        uint32_t reference_ac_rms = 0;
        RejectReason reject_reason = RejectReason::None;
    };

    struct PlaybackStats {
        uint32_t dma_generation = 0;
        uint32_t playback_serial = 0;
        uint8_t volume_percent = 0;  // 开启统计作用域时记录的标签。
        uint32_t attempts = 0;
        uint32_t accepted = 0;
        uint32_t rejected = 0;
        bool saturated = false;  // 至少一个计数器已无法继续递增。
        bool has_accepted = false;
        uint16_t last_accepted_delay_samples = 0;
        uint16_t last_accepted_correlation_per_mille = 0;
        uint16_t last_accepted_peak_margin_per_mille = 0;
        int8_t last_accepted_polarity = 0;
        uint16_t accepted_min_delay_samples = 0;
        uint16_t accepted_max_delay_samples = 0;
        std::array<uint32_t, kDelayBins> accepted_delay_histogram{};
        std::array<uint32_t, kRejectReasons> rejected_reason_counts{};
    };

    // 仅在任务中调用，并与 process()/copyPlaybackStats() 串行执行。
    // 非零身份变化会开启新的有界统计作用域；重复当前身份是幂等操作，保留原音量标签。
    // 零身份或无效音量仅暂停作用域计数，不清除较早的作用域。
    void setPlaybackScope(uint32_t dma_generation, uint32_t playback_serial, uint8_t volume_percent);
    size_t playbackStatsCount() const { return playback_stats_count_; }
    // newest_index=0 表示最近开启的作用域；复制失败不修改目标。
    // 不保留音频，也不分配内存。
    bool copyPlaybackStats(size_t newest_index, PlaybackStats *destination) const;

    // 仅借用输入，不动态分配、不在 ISR 中工作，也不保留音频。
    // 参考信号覆盖 RX 坐标 [mic_first-1280, mic_first+320)。
    // 全部 1600 个掩码字节都必须非零；不能跨缺失参考区间插值，
    // 也不能把未知区间用有效零样本替代。
    bool process(const int16_t *microphone_320, const int16_t *reference_1600,
                 const uint8_t *valid_mask_1600);
    Metrics metrics() const { return metrics_; }
    void reset();
    static const char *rejectReasonName(RejectReason reason);

private:
    friend struct AcousticDelayProbeTestAccess;
    bool reject(RejectReason reason);
    PlaybackStats *activeStats();
    void recordAccepted();
    Metrics metrics_{};
    // 所属 ReferenceMonitor 分配在 PSRAM 中。这里只保留固定大小的聚合观测，
    // 不保留 PCM，也不维护持续增长的事件历史。
    std::array<PlaybackStats, kPlaybackScopes> playback_stats_{};
    size_t playback_stats_next_ = 0;
    size_t playback_stats_count_ = 0;
    bool playback_scope_active_ = false;
};

}  // 命名空间 voiceaudio
