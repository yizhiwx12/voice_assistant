#include "acoustic_delay_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace voiceaudio {
namespace {
constexpr size_t kLagCount = AcousticDelayProbe::kMaximumDelaySamples /
                             AcousticDelayProbe::kDelayStepSamples + 1;
constexpr size_t kPoints = AcousticDelayProbe::kMicrophoneSamples /
                           AcousticDelayProbe::kDecimation;
constexpr size_t kPeakExclusionSteps = 2;  // 一个主瓣，范围为 +/-2 ms。
static_assert(AcousticDelayProbe::kMicrophoneSamples % AcousticDelayProbe::kDecimation == 0);
static_assert(kLagCount == 81 && kPoints == 80, "Keep the diagnostic CPU budget explicit");
static_assert(kLagCount == AcousticDelayProbe::kDelayBins);
static_assert(static_cast<size_t>(AcousticDelayProbe::RejectReason::InsufficientPeakSupport) + 1 ==
              AcousticDelayProbe::kRejectReasons);

void increment(uint32_t &counter, bool *saturated = nullptr)
{
    if (counter != std::numeric_limits<uint32_t>::max()) ++counter;
    else if (saturated) *saturated = true;
}

uint32_t acRms(int64_t variance)
{
    return variance > 0 ? static_cast<uint32_t>(std::llround(
        std::sqrt(static_cast<double>(variance)) / static_cast<double>(kPoints))) : 0;
}

uint16_t perMille(float value)
{
    return static_cast<uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 1000.0f));
}
}  // 命名空间

void AcousticDelayProbe::setPlaybackScope(uint32_t generation, uint32_t serial, uint8_t volume)
{
    if (!generation || !serial || volume > 100) {
        playback_scope_active_ = false;
        return;
    }
    if (playback_stats_count_) {
        PlaybackStats &latest = playback_stats_[
            (playback_stats_next_ + kPlaybackScopes - 1) % kPlaybackScopes];
        if (latest.dma_generation == generation && latest.playback_serial == serial) {
            playback_scope_active_ = true;
            return;
        }
    }
    PlaybackStats &stats = playback_stats_[playback_stats_next_];
    stats = {};
    stats.dma_generation = generation;
    stats.playback_serial = serial;
    stats.volume_percent = volume;
    playback_stats_next_ = (playback_stats_next_ + 1) % kPlaybackScopes;
    if (playback_stats_count_ < kPlaybackScopes) ++playback_stats_count_;
    playback_scope_active_ = true;
}

bool AcousticDelayProbe::copyPlaybackStats(size_t newest_index, PlaybackStats *destination) const
{
    if (!destination || newest_index >= playback_stats_count_) return false;
    *destination = playback_stats_[
        (playback_stats_next_ + kPlaybackScopes - 1 - newest_index) % kPlaybackScopes];
    return true;
}

AcousticDelayProbe::PlaybackStats *AcousticDelayProbe::activeStats()
{
    return playback_scope_active_ && playback_stats_count_
        ? &playback_stats_[(playback_stats_next_ + kPlaybackScopes - 1) % kPlaybackScopes] : nullptr;
}

void AcousticDelayProbe::reset()
{
    metrics_ = {};
    for (PlaybackStats &stats : playback_stats_) stats = {};
    playback_stats_next_ = playback_stats_count_ = 0;
    playback_scope_active_ = false;
}

void AcousticDelayProbe::recordAccepted()
{
    increment(metrics_.accepted);
    metrics_.last_accepted = true;
    PlaybackStats *stats = activeStats();
    if (!stats) return;
    increment(stats->accepted, &stats->saturated);
    stats->last_accepted_delay_samples = metrics_.best_delay_samples;
    stats->last_accepted_correlation_per_mille = metrics_.correlation_per_mille;
    stats->last_accepted_peak_margin_per_mille = metrics_.peak_margin_per_mille;
    stats->last_accepted_polarity = metrics_.correlation_polarity;
    if (!stats->has_accepted) {
        stats->accepted_min_delay_samples = stats->accepted_max_delay_samples = metrics_.best_delay_samples;
        stats->has_accepted = true;
    } else {
        stats->accepted_min_delay_samples = std::min(stats->accepted_min_delay_samples, metrics_.best_delay_samples);
        stats->accepted_max_delay_samples = std::max(stats->accepted_max_delay_samples, metrics_.best_delay_samples);
    }
    increment(stats->accepted_delay_histogram[metrics_.best_delay_samples / kDelayStepSamples],
              &stats->saturated);
}

bool AcousticDelayProbe::reject(RejectReason reason)
{
    increment(metrics_.rejected);
    metrics_.reject_reason = reason;
    if (PlaybackStats *stats = activeStats()) {
        increment(stats->rejected, &stats->saturated);
        increment(stats->rejected_reason_counts[static_cast<size_t>(reason)], &stats->saturated);
    }
    return false;
}

bool AcousticDelayProbe::process(const int16_t *microphone, const int16_t *reference,
                                 const uint8_t *valid_mask)
{
    increment(metrics_.attempts);
    if (PlaybackStats *stats = activeStats()) increment(stats->attempts, &stats->saturated);
    metrics_.last_accepted = false;
    metrics_.calibrated = false;
    metrics_.best_delay_samples = metrics_.correlation_per_mille = 0;
    metrics_.peak_margin_per_mille = 0;
    metrics_.correlation_polarity = 0;
    metrics_.mic_ac_rms = metrics_.reference_ac_rms = 0;
    metrics_.reject_reason = RejectReason::None;
    if (!microphone || !reference || !valid_mask) return reject(RejectReason::InvalidInput);
    for (size_t i = 0; i < kReferenceSamples; ++i)
        if (!valid_mask[i]) return reject(RejectReason::IncompleteReference);

    int64_t sum_x = 0, sum_xx = 0;
    for (size_t i = 0; i < kMicrophoneSamples; i += kDecimation) {
        const int32_t value = microphone[i];
        sum_x += value;
        // 单个 PCM16 乘积可放入有符号 32 位整数，只有累加和需要 64 位范围。
        // 避免在 S3 上为每个点调用 64 位乘法辅助函数。
        sum_xx += value * value;
    }
    // n*sum(x*x)-sum(x)^2 在归一化前精确去除直流分量。
    // 对于 80 个 PCM16 点，所有乘积均远低于有符号 64 位范围。
    const int64_t variance_x = static_cast<int64_t>(kPoints) * sum_xx - sum_x * sum_x;
    metrics_.mic_ac_rms = acRms(variance_x);
    if (variance_x <= 0 || metrics_.mic_ac_rms < kMinimumMicrophoneRms)
        return reject(RejectReason::LowMicrophoneEnergy);

    std::array<float, kLagCount> scores{};
    std::array<uint32_t, kLagCount> rms_values{};
    std::array<int8_t, kLagCount> polarities{};
    scores.fill(-1.0f);  // 能量不足的延迟点不能成为峰值。
    float best_score = -1.0f;
    size_t best_lag = 0, energized_lags = 0;
    for (size_t lag = 0; lag < kLagCount; ++lag) {
        const size_t start = kMaximumDelaySamples - lag * kDelayStepSamples;
        int64_t sum_y = 0, sum_yy = 0, sum_xy = 0;
        for (size_t i = 0; i < kMicrophoneSamples; i += kDecimation) {
            const int32_t x = microphone[i];
            const int32_t y = reference[start + i];
            sum_y += y;
            sum_yy += y * y;
            sum_xy += x * y;
        }
        const int64_t variance_y = static_cast<int64_t>(kPoints) * sum_yy - sum_y * sum_y;
        const uint32_t rms = acRms(variance_y);
        rms_values[lag] = rms;
        if (variance_y <= 0 || rms < kMinimumReferenceRms) continue;
        ++energized_lags;
        const int64_t covariance = static_cast<int64_t>(kPoints) * sum_xy - sum_x * sum_y;
        const double normalized = static_cast<double>(covariance) /
            std::sqrt(static_cast<double>(variance_x) * static_cast<double>(variance_y));
        const float score = static_cast<float>(std::min(1.0, std::abs(normalized)));
        scores[lag] = score;
        polarities[lag] = normalized < 0.0 ? -1 : 1;
        if (score > best_score) { best_score = score; best_lag = lag; }
    }
    if (!energized_lags) return reject(RejectReason::LowReferenceEnergy);
    metrics_.best_delay_samples = static_cast<uint16_t>(best_lag * kDelayStepSamples);
    metrics_.correlation_per_mille = perMille(best_score);
    metrics_.correlation_polarity = polarities[best_lag];
    metrics_.reference_ac_rms = rms_values[best_lag];
    if (best_score * 1000.0f < kMinimumCorrelationPermille)
        return reject(RejectReason::LowCorrelation);
    // 搜索范围边缘只表示已搜索点中的最佳延迟，并不能证明存在峰值。
    // 保留候选值用于诊断，不接受该值，也不改变相位。
    if (best_lag == 0 || best_lag + 1 == kLagCount)
        return reject(RejectReason::BoundaryPeak);
    if (energized_lags < 3 || scores[best_lag - 1] < 0.0f || scores[best_lag + 1] < 0.0f)
        return reject(RejectReason::InsufficientPeakSupport);

    float competing_peak = -1.0f;
    for (size_t lag = 0; lag < kLagCount; ++lag) {
        const size_t distance = lag > best_lag ? lag - best_lag : best_lag - lag;
        if (distance > kPeakExclusionSteps) competing_peak = std::max(competing_peak, scores[lag]);
    }
    if (competing_peak < 0.0f) return reject(RejectReason::InsufficientPeakSupport);
    metrics_.peak_margin_per_mille = perMille(std::max(0.0f, best_score - competing_peak));
    if (best_score <= std::max(scores[best_lag - 1], scores[best_lag + 1]) + 0.001f ||
        (best_score - competing_peak) * 1000.0f < kMinimumPeakMarginPermille)
        return reject(RejectReason::AmbiguousPeak);

    recordAccepted();
    return true;
}

const char *AcousticDelayProbe::rejectReasonName(RejectReason reason)
{
    switch (reason) {
    case RejectReason::None: return "none";
    case RejectReason::InvalidInput: return "invalid_input";
    case RejectReason::IncompleteReference: return "incomplete_reference";
    case RejectReason::LowMicrophoneEnergy: return "low_mic_energy";
    case RejectReason::LowReferenceEnergy: return "low_ref_energy";
    case RejectReason::LowCorrelation: return "low_correlation";
    case RejectReason::BoundaryPeak: return "boundary_peak";
    case RejectReason::AmbiguousPeak: return "ambiguous_peak";
    case RejectReason::InsufficientPeakSupport: return "insufficient_peak_support";
    }
    return "unknown";
}

}  // 命名空间 voiceaudio
