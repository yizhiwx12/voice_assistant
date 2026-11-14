#include "reference_monitor.hpp"
#include "esp_heap_caps.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {
using Monitor = voiceaudio::ReferenceMonitor;
using Block = PlaybackReference::Block;
using Kind = PlaybackReference::IntervalKind;
std::unordered_map<void *, size_t> allocations;
bool fail_allocation = false;
size_t allocation_bytes = 0;

Block block(uint32_t generation, uint32_t serial, uint64_t first_sample,
            int64_t end_us, Kind kind = Kind::Content, int16_t value = 2000)
{
    Block result{};
    result.dma_generation = generation;
    result.playback_serial = serial;
    result.first_sample_24k = first_sample;
    result.observed_end_us = end_us;
    result.sample_count = PlaybackReference::kSamplesPerBlock;
    result.interval_kind = kind;
    result.stereo_match = true;
    result.amplifier_enabled = kind != Kind::AmplifierMuted;
    result.known_silence = kind == Kind::DigitalSilence || kind == Kind::AmplifierMuted;
    result.mono.fill(result.known_silence ? 0 : value);
    return result;
}

void feed(Monitor &monitor, const Block &value, uint64_t microphone_end,
          bool overwritten = false)
{
    monitor.observeMicrophone(microphone_end, value.observed_end_us, 0);
    monitor.consume(value, overwritten);
}

void startupAndAllocationFailure()
{
    Monitor unavailable;
    std::array<int16_t, 320> pcm{};
    assert(!unavailable.probeAcousticDelay(1600, pcm.data()));
    unavailable.observeMicrophone(160, 10000, 0);
    unavailable.consume(block(1, 1, 0, 10000), false);
    unavailable.probe(16000);
    assert(!unavailable.metrics().ready && unavailable.metrics().input_samples == 0);
    fail_allocation = true;
    assert(!unavailable.start() && !unavailable.metrics().ready);
    assert(allocations.empty());
    assert(unavailable.start());
    assert(!unavailable.start());
    assert(unavailable.timelineMetrics().available);
    assert(allocation_bytes > 0 && allocation_bytes < 64 * 1024);
}

void continuousMuteAndProbe()
{
    Monitor monitor;
    assert(monitor.start());
    constexpr int64_t start_us = 12300000000LL;  // 超过 32 位微秒计数回绕点。
    for (uint64_t index = 0; index < 100; ++index) {
        feed(monitor, block(1, 7, index * 240, start_us + (index + 1) * 10000,
                           Kind::AmplifierMuted), (index + 1) * 160);
        monitor.probe((index + 1) * 160);
    }
    const auto metrics = monitor.metrics();
    assert(metrics.ready && metrics.generation == 1 && metrics.playback_serial == 7);
    assert(metrics.input_samples == 24000 && metrics.output_samples == 16000);
    assert(metrics.warmup_samples == 63 && metrics.generation_resets == 1);
    assert(metrics.gap_resets == 0 && metrics.unknown_blocks == 0 && metrics.processing_errors == 0);
    assert(metrics.probe_frames > 0 && metrics.probe_complete_frames > 0);
    assert(metrics.probe_valid_samples <= metrics.probe_samples && metrics.probe_valid_samples > 0);
    assert(monitor.timelineMetrics().aligned_blocks > 0);
    const auto before = metrics.probe_frames;
    monitor.probe(16000);  // 同一帧不反复探测。
    assert(monitor.metrics().probe_frames == before);
}

void explicitUnknownAndWarmup()
{
    Monitor monitor;
    assert(monitor.start());
    feed(monitor, block(1, 9, 0, 10000), 160);
    feed(monitor, block(1, 9, 240, 20000), 320);
    const auto before = monitor.metrics();
    auto boundary = block(1, 9, 480, 30000);
    boundary.amplifier_boundary = true;
    feed(monitor, boundary, 480);
    assert(monitor.metrics().unknown_blocks == 1);
    assert(monitor.metrics().output_samples == before.output_samples);
    feed(monitor, block(1, 9, 720, 40000), 640);
    assert(monitor.metrics().warmup_samples == before.warmup_samples + 63);
    assert(monitor.metrics().gap_resets == 0);  // 明确的无效区间，不是缺失数据。

    feed(monitor, block(1, 9, 960, 50000, Kind::Unknown), 800);
    auto mismatch = block(1, 9, 1200, 60000);
    mismatch.stereo_match = false;
    feed(monitor, mismatch, 960);
    auto partial = block(1, 9, 1440, 70000);
    partial.sample_count = 239;
    feed(monitor, partial, 1120);
    assert(monitor.metrics().unknown_blocks == 4);
    feed(monitor, block(1, 9, 1680, 80000, Kind::DigitalSilence), 1280);
    assert(monitor.metrics().warmup_samples == before.warmup_samples + 126);
    assert(monitor.metrics().processing_errors == 0);
}

void gapOverwriteGenerationAndSerial()
{
    Monitor monitor;
    assert(monitor.start());
    feed(monitor, block(2, 5, 0, 10000), 160);
    // 丢失一次回调；不相邻的样本区间重置 FIR 状态。
    feed(monitor, block(2, 5, 480, 30000), 480);
    assert(monitor.metrics().gap_resets == 1);
    auto gap = block(2, 5, 720, 40000);
    gap.gap_before = true;
    feed(monitor, gap, 640);
    assert(monitor.metrics().gap_resets == 2);
    feed(monitor, block(2, 5, 960, 50000), 800, true);
    assert(monitor.metrics().gap_resets == 3 && monitor.metrics().overwritten_blocks == 1);
    // 排空后的作用域改变元数据，但不打断物理样本。
    const auto before_serial_warmup = monitor.metrics().warmup_samples;
    feed(monitor, block(2, 6, 1200, 60000), 960);
    assert(monitor.metrics().gap_resets == 3 && monitor.metrics().playback_serial == 6);
    assert(monitor.metrics().warmup_samples == before_serial_warmup);
    feed(monitor, block(3, 8, 0, 70000), 1120);
    assert(monitor.metrics().generation_resets == 2);
    assert(monitor.metrics().generation == 3 && monitor.metrics().playback_serial == 8);
    assert(monitor.metrics().gap_resets == 3 && monitor.metrics().processing_errors == 0);
    assert(monitor.metrics().warmup_samples == 5 * 63);
}

void zeroBoundaryAndSerialRemainContinuous()
{
    Monitor monitor;
    assert(monitor.start());
    for (uint64_t i = 0; i < 12; ++i) {
        auto value = block(1, i < 6 ? 0 : 10, i * 240, (i + 1) * 10000,
                           Kind::DigitalSilence);
        value.amplifier_boundary = i == 5 || i == 6;
        feed(monitor, value, (i + 1) * 160);
    }
    assert(monitor.metrics().warmup_samples == 63 && !monitor.metrics().gap_resets);
    assert(!monitor.metrics().unknown_blocks);
    std::array<int16_t, 320> reference{};
    size_t valid = 0;
    // 小数查询跨播放元数据连接物理相邻点。
    assert(monitor.copyReferenceFrame(800, reference.size(), 0, 0,
        reference.data(), reference.size(), &valid));
    assert(valid == reference.size());
    for (int16_t sample : reference) assert(sample == 0);
    // 非零功放转换仍为未知，
    // 不能因连续序号处理而变为有效。
    auto unknown = block(1, 11, 12 * 240, 130000);
    unknown.amplifier_boundary = true;
    feed(monitor, unknown, 13 * 160);
    assert(monitor.metrics().unknown_blocks == 1);
}

void conversionFailureAndRecovery()
{
    Monitor monitor;
    assert(monitor.start());
    feed(monitor, block(1, 1, 0, 10000), 160);
    const auto before = monitor.metrics();
    // 首个输出坐标可表示，但完整块超出 Result 的有符号内容时间戳范围。
    // 转换须在消费任何样本前失败，随后在普通代次边界恢复。
    const uint64_t edge = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 2;
    feed(monitor, block(1, 1, edge, 20000), 320);
    assert(monitor.metrics().processing_errors == 1);
    assert(monitor.metrics().input_samples == before.input_samples);
    assert(monitor.metrics().output_samples == before.output_samples);
    feed(monitor, block(2, 2, 0, 30000), 480);
    assert(monitor.metrics().input_samples == before.input_samples + 240);
    assert(monitor.metrics().output_samples == before.output_samples + 160);
    assert(monitor.metrics().warmup_samples == before.warmup_samples + 63);
}

void syntheticAcousticDelayAndMissingMask()
{
    Monitor monitor;
    assert(monitor.start());
    monitor.setPlaybackVolumeLabel(40);
    constexpr size_t kBlocks = 60;
    constexpr uint64_t kDelaySamples = 320;  // 麦克风比 TX 滞后 20 ms。
    constexpr int64_t kStartUs = 4560000000LL;
    std::vector<int16_t> playback(kBlocks * 240);
    uint32_t random = 0x16a24f17U;
    for (int16_t &sample : playback) {
        random = random * 1664525U + 1013904223U;
        sample = static_cast<int16_t>(static_cast<int32_t>((random >> 8) % 24001U) - 12000);
    }
    // 根据连续滤波信号及文档规定的固定 FIR 延迟独立构造麦克风数据。
    // 不读取观测器内部状态，也不把查询输出复制回去作为预期麦克风信号。
    voiceaudio::ReferenceResampler expected_resampler;
    assert(expected_resampler.initialize());
    std::vector<int16_t> filtered(kBlocks * 160);
    voiceaudio::ReferenceResampler::Result converted{};
    assert(expected_resampler.process(playback.data(), playback.size(),
        filtered.data(), filtered.size(), &converted));
    assert(converted.output_samples == filtered.size());
    const auto microphone_frame = [&](uint64_t first) {
        std::array<int16_t, 320> pcm{};
        for (size_t i = 0; i < pcm.size(); ++i) {
            const uint64_t numerator = (first + i - kDelaySamples) * 3 +
                voiceaudio::ReferenceResampler::kGroupDelayUpsampledSamples;
            const size_t left = static_cast<size_t>(numerator / 3);
            const double fraction = static_cast<double>(numerator % 3) / 3.0;
            assert(left + 1 < filtered.size());
            pcm[i] = static_cast<int16_t>(std::nearbyint(filtered[left] + fraction *
                (static_cast<double>(filtered[left + 1]) - filtered[left])));
        }
        return pcm;
    };
    const auto feed_block = [&](uint64_t index, bool unknown) {
        auto value = block(4, 42, index * 240, kStartUs + (index + 1) * 10000);
        std::copy_n(playback.data() + index * 240, value.mono.size(), value.mono.begin());
        if (unknown) value.interval_kind = Kind::Unknown;
        feed(monitor, value, (index + 1) * 160);
    };
    for (uint64_t index = 0; index < 40; ++index) feed_block(index, false);
    const auto first_pcm = microphone_frame(4800);
    assert(monitor.probeAcousticDelay(4800, first_pcm.data()));
    auto metrics = monitor.acousticMetrics();
    assert(metrics.attempts == 1 && metrics.accepted == 1 && metrics.last_accepted);
    assert(metrics.best_delay_samples == kDelaySamples);  // 正延迟方向。
    assert(metrics.correlation_per_mille >= 990 && metrics.correlation_polarity == 1);
    assert(!metrics.calibrated && !monitor.timelineMetrics().acoustically_calibrated);
    voiceaudio::AcousticDelayProbe::PlaybackStats accepted{};
    assert(monitor.acousticPlaybackStatsCount() == 1);
    assert(monitor.copyAcousticPlaybackStats(0, &accepted));
    assert(accepted.dma_generation == 4 && accepted.playback_serial == 42 && accepted.volume_percent == 40);
    assert(accepted.attempts == 1 && accepted.accepted == 1 && accepted.rejected == 0 && accepted.has_accepted);
    assert(accepted.last_accepted_delay_samples == kDelaySamples &&
           accepted.last_accepted_correlation_per_mille == metrics.correlation_per_mille &&
           accepted.last_accepted_peak_margin_per_mille == metrics.peak_margin_per_mille &&
           accepted.last_accepted_polarity == 1 &&
           accepted.accepted_min_delay_samples == kDelaySamples &&
           accepted.accepted_max_delay_samples == kDelaySamples);
    assert(accepted.accepted_delay_histogram[kDelaySamples /
        voiceaudio::AcousticDelayProbe::kDelayStepSamples] == 1);
    assert(!monitor.probeAcousticDelay(4800, first_pcm.data()));
    assert(!monitor.probeAcousticDelay(1600, first_pcm.data()));  // 倒退坐标不能回绕越过限流检查。
    assert(!monitor.probeAcousticDelay(7999, first_pcm.data()));
    assert(!monitor.probeAcousticDelay(8000, nullptr));
    assert(monitor.acousticMetrics().attempts == 1);  // 200 ms 有界执行节奏。

    // 下一个 100 ms 参考窗口内的未知标记区间必须产生缺失掩码。
    // 即使麦克风存在很强的合成相关信号，
    // 也不能把它视为有效静音参考。
    monitor.setPlaybackVolumeLabel(55);  // 相同身份保留最初标签。
    for (uint64_t index = 40; index < kBlocks; ++index) feed_block(index, index == 46);
    const auto next_pcm = microphone_frame(8000);
    assert(monitor.probeAcousticDelay(8000, next_pcm.data()));
    metrics = monitor.acousticMetrics();
    assert(metrics.attempts == 2 && metrics.accepted == 1 && metrics.rejected == 1);
    assert(!metrics.last_accepted && !metrics.calibrated);
    assert(metrics.reject_reason == voiceaudio::AcousticDelayProbe::RejectReason::IncompleteReference);
    assert(monitor.metrics().unknown_blocks == 1 && monitor.metrics().processing_errors == 0);
    voiceaudio::AcousticDelayProbe::PlaybackStats rejected{};
    assert(monitor.copyAcousticPlaybackStats(0, &rejected));
    assert(rejected.volume_percent == 40 && rejected.attempts == 2 && rejected.accepted == 1 && rejected.rejected == 1);
    assert(rejected.last_accepted_delay_samples == accepted.last_accepted_delay_samples &&
           rejected.last_accepted_correlation_per_mille == accepted.last_accepted_correlation_per_mille &&
           rejected.last_accepted_peak_margin_per_mille == accepted.last_accepted_peak_margin_per_mille &&
           rejected.last_accepted_polarity == accepted.last_accepted_polarity &&
           rejected.accepted_delay_histogram == accepted.accepted_delay_histogram);
    assert(rejected.rejected_reason_counts[static_cast<size_t>(
        voiceaudio::AcousticDelayProbe::RejectReason::IncompleteReference)] == 1);

    // 新代次及序号开启作用域；取消后的零序号不会开启。
    // 这些元数据变化都不能清除先前已接受的观测。
    feed(monitor, block(5, 43, 0, kStartUs + 610000), 9760);
    assert(monitor.acousticPlaybackStatsCount() == 2);
    voiceaudio::AcousticDelayProbe::PlaybackStats newest{}, old{};
    assert(monitor.copyAcousticPlaybackStats(0, &newest));
    assert(newest.dma_generation == 5 && newest.playback_serial == 43 && newest.volume_percent == 55);
    assert(newest.attempts == 0 && !newest.has_accepted);
    assert(monitor.copyAcousticPlaybackStats(1, &old));
    assert(old.playback_serial == 42 && old.accepted_delay_histogram == accepted.accepted_delay_histogram);
    monitor.setPlaybackVolumeLabel(70);
    feed(monitor, block(5, 43, 240, kStartUs + 620000), 9920);
    assert(monitor.acousticPlaybackStatsCount() == 2);
    assert(monitor.copyAcousticPlaybackStats(0, &newest) && newest.volume_percent == 55);
    feed(monitor, block(5, 44, 480, kStartUs + 630000), 10080);
    assert(monitor.acousticPlaybackStatsCount() == 3);
    assert(monitor.copyAcousticPlaybackStats(0, &newest) && newest.playback_serial == 44 && newest.volume_percent == 70);
    feed(monitor, block(6, 0, 0, kStartUs + 640000, Kind::AmplifierMuted), 10240);
    assert(monitor.acousticPlaybackStatsCount() == 3);
    std::array<int16_t, 320> zero_pcm{};
    assert(monitor.probeAcousticDelay(11200, zero_pcm.data()));
    assert(monitor.copyAcousticPlaybackStats(0, &newest) && newest.playback_serial == 44 && newest.attempts == 0);
    monitor.setPlaybackVolumeLabel(25);
    feed(monitor, block(6, 45, 240, kStartUs + 650000), 10400);
    assert(monitor.acousticPlaybackStatsCount() == 4);
    assert(monitor.copyAcousticPlaybackStats(3, &old) && old.playback_serial == 42 &&
           old.last_accepted_delay_samples == kDelaySamples && old.has_accepted);
    monitor.setPlaybackVolumeLabel(30);
    feed(monitor, block(7, 46, 0, kStartUs + 660000), 10560);
    assert(monitor.acousticPlaybackStatsCount() == voiceaudio::AcousticDelayProbe::kPlaybackScopes);
    assert(monitor.copyAcousticPlaybackStats(0, &newest) && newest.playback_serial == 46 && newest.volume_percent == 30);
    assert(monitor.copyAcousticPlaybackStats(3, &old) && old.playback_serial == 43 && old.volume_percent == 55);
    const auto sentinel = old;
    assert(!monitor.copyAcousticPlaybackStats(4, &old));
    assert(old.playback_serial == sentinel.playback_serial && old.volume_percent == sentinel.volume_percent);
}
}  // 命名空间

void *heap_caps_malloc(size_t bytes, int caps)
{
    assert(caps & MALLOC_CAP_SPIRAM);
    assert(!(caps & MALLOC_CAP_INTERNAL));
    if (fail_allocation) { fail_allocation = false; return nullptr; }
    void *memory = std::malloc(bytes);
    assert(memory);
    assert(allocations.emplace(memory, bytes).second);
    allocation_bytes = bytes;
    return memory;
}

void heap_caps_free(void *memory)
{
    if (!memory) return;
    assert(allocations.erase(memory) == 1);
    std::free(memory);
}

int main()
{
    startupAndAllocationFailure();
    assert(allocations.empty());
    continuousMuteAndProbe();
    assert(allocations.empty());
    explicitUnknownAndWarmup();
    assert(allocations.empty());
    gapOverwriteGenerationAndSerial();
    assert(allocations.empty());
    zeroBoundaryAndSerialRemainContinuous();
    assert(allocations.empty());
    conversionFailureAndRecovery();
    assert(allocations.empty());
    syntheticAcousticDelayAndMissingMask();
    assert(allocations.empty());
    std::puts("Passed production reference monitor integration: 7 scenario groups, allocation fallback, continuous muted zero/probe, 64-bit anchors, zero boundary/serial continuity, invalid boundaries/warm-up, event gaps/history overwrite, generation reset, conversion failure/recovery, synthetic positive acoustic delay/missing-mask rejection, retained accepted scope statistics/histogram, volume labels/four-record bound and backward/throttled analysis rejection; no ISR or hardware used");
}
