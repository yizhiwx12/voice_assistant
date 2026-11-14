#pragma once

#include <array>
#include <cstdint>

#include "playback_reference.hpp"
#include "reference_resampler.hpp"
#include "reference_timeline.hpp"
#include "acoustic_delay_probe.hpp"

namespace voiceaudio {

// 仅任务上下文使用的 P4.3 B 观测器。不向 AFE 送入数据，
// 不改变麦克风、播放、网络或对话行为。访问由 AudioService 串行化。
class ReferenceMonitor {
public:
    struct Metrics {
        bool ready = false;
        uint32_t generation = 0;
        uint32_t playback_serial = 0;
        uint32_t input_samples = 0;
        uint32_t output_samples = 0;
        uint32_t warmup_samples = 0;
        uint32_t unknown_blocks = 0;
        uint32_t gap_resets = 0;
        uint32_t generation_resets = 0;
        uint32_t overwritten_blocks = 0;
        uint32_t processing_errors = 0;
        uint32_t probe_frames = 0;
        uint32_t probe_complete_frames = 0;
        uint32_t probe_valid_samples = 0;
        uint32_t probe_samples = 0;
    };

    bool start();
    void observeMicrophone(uint64_t sample_end, int64_t end_us, uint32_t overflow_events,
                           bool timeline_valid = true);
    void consume(const PlaybackReference::Block &block, bool overwritten);
    void probe(uint64_t microphone_sample_end);
    // 仅播放期间输入延迟且带物理索引的采集帧。
    // 结果只是相关性观测，绝不改变对齐。
    // 真表示已执行一次分析（接受或拒绝），假表示被限流。
    bool probeAcousticDelay(uint64_t first_microphone_sample, const int16_t *pcm);
    // TX 完成位置已超过麦克风区间后，由串行化播放/观测持有者调用。
    // 任何任务都不能与 consume() 并发查询时间线。
    bool copyReferenceFrame(uint64_t first_microphone_sample, size_t sample_count,
                            uint32_t dma_generation, uint32_t playback_serial,
                            int16_t *destination, size_t capacity,
                            size_t *valid_samples = nullptr,
                            uint8_t *valid_mask = nullptr) const;
    void setPlaybackVolumeLabel(uint8_t percent) { volume_label_ = percent; }
    size_t acousticPlaybackStatsCount() const { return acoustic_probe_.playbackStatsCount(); }
    bool copyAcousticPlaybackStats(size_t newest_index, AcousticDelayProbe::PlaybackStats *output) const
    { return acoustic_probe_.copyPlaybackStats(newest_index, output); }
    Metrics metrics() const { return metrics_; }
    ReferenceTimeline::Metrics timelineMetrics() const { return timeline_.metrics(); }
    ReferenceTimeline::HealthRange timelineHealthRange() const { return timeline_.healthRange(); }
    ReferenceTimeline::FirstClockFailure firstClockFailure() const { return timeline_.firstClockFailure(); }
    AcousticDelayProbe::Metrics acousticMetrics() const { return acoustic_probe_.metrics(); }

private:
    ReferenceResampler resampler_;
    ReferenceTimeline timeline_;
    Metrics metrics_{};
    uint64_t expected_input_ = 0;
    uint64_t last_probe_end_ = 0;
    bool pending_reset_ = true;
    uint64_t last_acoustic_probe_ = 0;
    bool have_acoustic_probe_ = false;
    uint8_t volume_label_ = 0;
    AcousticDelayProbe acoustic_probe_;
    std::array<int16_t, 1600> acoustic_reference_{};
    std::array<uint8_t, 1600> acoustic_mask_{};
    // 一个 20 ms 诊断帧，在 RX 后方 80 ms 处查询，
    // 使对应 TX 完成记录与 FIR 支持范围应已到达。
    std::array<int16_t, 320> probe_pcm_{};
};

}  // 命名空间 voiceaudio
