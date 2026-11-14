#include "reference_monitor.hpp"

#include <limits>

namespace voiceaudio {

bool ReferenceMonitor::start()
{
    if (metrics_.ready) return false;
    metrics_ = {};
    expected_input_ = last_probe_end_ = 0;
    last_acoustic_probe_ = 0;
    have_acoustic_probe_ = false;
    acoustic_probe_.reset();
    pending_reset_ = true;
    metrics_.ready = resampler_.initialize() && timeline_.start();
    return metrics_.ready;
}

void ReferenceMonitor::observeMicrophone(uint64_t sample_end, int64_t end_us,
                                        uint32_t overflow_events, bool timeline_valid)
{
    if (metrics_.ready)
        timeline_.observeMicrophoneDma(sample_end, end_us, overflow_events, timeline_valid);
}

void ReferenceMonitor::consume(const PlaybackReference::Block &block, bool overwritten)
{
    if (!metrics_.ready) return;
    // 生产 DMA 块始终从 240 样本边界开始。
    // 甚至在计算 FIR 内容时间戳前就拒绝无效坐标。
    if (block.first_sample_24k >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 2) -
                PlaybackReference::kSamplesPerBlock) {
        ++metrics_.processing_errors;
        pending_reset_ = true;
        return;
    }
    const bool new_generation = block.dma_generation != metrics_.generation;
    const bool gap = overwritten || block.gap_before || (!new_generation &&
        block.first_sample_24k != expected_input_);
    if (new_generation) ++metrics_.generation_resets;
    if (gap) ++metrics_.gap_resets;
    if (overwritten) ++metrics_.overwritten_blocks;
    metrics_.generation = block.dma_generation;
    metrics_.playback_serial = block.playback_serial;
    // 取消/新建且序号为零的 DMA 代次暂停计数，
    // 但保留最后四条播放记录，供后续指标快照使用。
    acoustic_probe_.setPlaybackScope(block.dma_generation, block.playback_serial, volume_label_);

    const bool valid = block.sample_count == PlaybackReference::kSamplesPerBlock &&
        (!block.amplifier_boundary || (block.interval_kind == PlaybackReference::IntervalKind::DigitalSilence &&
                                     block.known_silence)) &&
        block.stereo_match &&
        block.interval_kind != PlaybackReference::IntervalKind::Unknown;
    if (new_generation || gap || pending_reset_)
        resampler_.reset(block.first_sample_24k);

    std::array<int16_t, 160> converted{};
    ReferenceResampler::Result result{};
    if (!valid) {
        ++metrics_.unknown_blocks;
        pending_reset_ = true;
        // 生成明确标记无效的输出区间；零值仅用于存储，
        // 绝不报告为已知静音或有效 AEC 参考。
        const uint64_t first_output = (block.first_sample_24k / 3) * 2 +
            ((block.first_sample_24k % 3) * 2 + 2) / 3;
        timeline_.append(block.dma_generation, block.playback_serial,
            block.first_sample_24k + PlaybackReference::kSamplesPerBlock, block.observed_end_us,
            first_output, static_cast<int64_t>(first_output * 3) -
                ReferenceResampler::kGroupDelayUpsampledSamples,
            converted.data(), converted.size(), converted.size(), false);
    } else if (resampler_.process(block.mono.data(), block.sample_count,
                                 converted.data(), converted.size(), &result)) {
        metrics_.input_samples += block.sample_count;
        metrics_.output_samples += result.output_samples;
        metrics_.warmup_samples += result.valid_from_output;
        timeline_.append(block.dma_generation, block.playback_serial,
            block.first_sample_24k + block.sample_count, block.observed_end_us,
            result.first_output_sample_16k, result.first_content_sample_16k_thirds,
            converted.data(), result.output_samples, result.valid_from_output, true);
        pending_reset_ = false;
    } else {
        ++metrics_.processing_errors;
        pending_reset_ = true;
    }
    expected_input_ = block.first_sample_24k + PlaybackReference::kSamplesPerBlock;
}

void ReferenceMonitor::probe(uint64_t microphone_sample_end)
{
    constexpr uint64_t kProbeDelay = 1280;  // 80 ms，仅用于观测，不是 AEC 延迟。
    if (!metrics_.ready || microphone_sample_end < kProbeDelay + probe_pcm_.size() ||
        microphone_sample_end - last_probe_end_ < probe_pcm_.size()) return;
    last_probe_end_ = microphone_sample_end;
    size_t valid = 0;
    timeline_.copyMicrophoneFrame(microphone_sample_end - kProbeDelay - probe_pcm_.size(),
        probe_pcm_.size(), probe_pcm_.data(), probe_pcm_.size(), 0, 0, &valid);
    ++metrics_.probe_frames;
    if (valid == probe_pcm_.size()) ++metrics_.probe_complete_frames;
    metrics_.probe_valid_samples += valid;
    metrics_.probe_samples += probe_pcm_.size();
}

bool ReferenceMonitor::copyReferenceFrame(uint64_t first_microphone_sample, size_t sample_count,
                                         uint32_t dma_generation, uint32_t playback_serial,
                                         int16_t *destination, size_t capacity,
                                         size_t *valid_samples, uint8_t *valid_mask) const
{
    return timeline_.copyMicrophoneFrame(first_microphone_sample, sample_count,
        destination, capacity, dma_generation, playback_serial, valid_samples, valid_mask);
}

bool ReferenceMonitor::probeAcousticDelay(uint64_t first_microphone_sample, const int16_t *pcm)
{
    if (!metrics_.ready || !pcm || first_microphone_sample < 1280 ||
        (have_acoustic_probe_ && (first_microphone_sample <= last_acoustic_probe_ ||
         first_microphone_sample - last_acoustic_probe_ < 3200))) return false;
    last_acoustic_probe_ = first_microphone_sample;
    have_acoustic_probe_ = true;
    const uint64_t reference_first = first_microphone_sample - 1280;
    for (size_t offset = 0; offset < acoustic_reference_.size(); offset += 320)
        timeline_.copyMicrophoneFrame(reference_first + offset, 320,
            acoustic_reference_.data() + offset, 320, metrics_.generation,
            metrics_.playback_serial, nullptr, acoustic_mask_.data() + offset);
    acoustic_probe_.process(pcm, acoustic_reference_.data(), acoustic_mask_.data());
    return true;
}

}  // 命名空间 voiceaudio
