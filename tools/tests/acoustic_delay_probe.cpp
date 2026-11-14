#include "acoustic_delay_probe.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

using Probe = voiceaudio::AcousticDelayProbe;
using Reason = Probe::RejectReason;
namespace voiceaudio {
struct AcousticDelayProbeTestAccess {
    static AcousticDelayProbe::PlaybackStats *stats(AcousticDelayProbe &probe)
    { return probe.activeStats(); }
    static AcousticDelayProbe::Metrics &metrics(AcousticDelayProbe &probe)
    { return probe.metrics_; }
};
}  // 命名空间 voiceaudio
namespace {
using Microphone = std::array<int16_t, Probe::kMicrophoneSamples>;
using Reference = std::array<int16_t, Probe::kReferenceSamples>;
using Mask = std::array<uint8_t, Probe::kReferenceSamples>;

void check(bool condition, const char *message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

uint32_t randomWord(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

Reference source(uint32_t seed = 0x35a9487b)
{
    Reference reference{};
    for (auto &sample : reference)
        sample = static_cast<int16_t>((static_cast<int>(randomWord(seed) % 1001) - 500) * 16);
    return reference;
}

Microphone delayed(const Reference &reference, size_t delay, int gain_divisor = 8, int dc = 0)
{
    Microphone microphone{};
    for (size_t i = 0; i < microphone.size(); ++i)
        microphone[i] = static_cast<int16_t>(reference[1280 + i - delay] / gain_divisor + dc);
    return microphone;
}

void knownLagsAndDc()
{
    Probe probe;
    Mask mask{};
    mask.fill(1);
    Reference reference = source();
    const Reference unchanged_reference = reference;
    for (const size_t delay : {size_t{16}, size_t{352}, size_t{784}, size_t{1264}}) {
        const Microphone microphone = delayed(reference, delay);
        check(probe.process(microphone.data(), reference.data(), mask.data()), "known positive in-range lag accepted");
        const auto metrics = probe.metrics();
        check(metrics.last_accepted && metrics.best_delay_samples == delay &&
              metrics.correlation_per_mille == 1000 && metrics.correlation_polarity == 1 &&
              metrics.peak_margin_per_mille >= Probe::kMinimumPeakMarginPermille && !metrics.calibrated,
              "known lag sign/normalization and uncalibrated status");
    }
    check(reference == unchanged_reference, "probe must not mutate borrowed reference");

    Microphone microphone = delayed(reference, 352, 8, 12000);
    for (auto &sample : reference) sample = static_cast<int16_t>(sample + 7000);
    check(probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().best_delay_samples == 352 && probe.metrics().correlation_per_mille == 1000,
          "independent large DC offsets removed before normalization");
    reference = source();
    microphone = delayed(reference, 352, -8);
    check(probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().correlation_polarity == -1 && probe.metrics().correlation_per_mille == 1000,
          "inverted acoustic polarity reported without losing lag");
    check(probe.metrics().attempts == 6 && probe.metrics().accepted == 6 && probe.metrics().rejected == 0,
          "accepted attempts accumulate");
    probe.reset();
    check(probe.metrics().attempts == 0 && !probe.metrics().last_accepted, "explicit metrics reset");
}

void energyAndDcRejections()
{
    Probe probe;
    Mask mask{};
    mask.fill(1);
    Reference reference = source();
    Microphone microphone = delayed(reference, 352, 4000);
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::LowMicrophoneEnergy,
          "weak microphone cannot provide calibration evidence");
    microphone.fill(15000);
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::LowMicrophoneEnergy && probe.metrics().mic_ac_rms == 0,
          "DC-only microphone is not acoustic signal");
    reference.fill(12000);
    microphone = delayed(source(), 352);
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::LowReferenceEnergy,
          "DC-only reference cannot correlate");
    reference = source();
    for (auto &sample : reference) sample = static_cast<int16_t>(sample / 4000);
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::LowReferenceEnergy,
          "weak reference does not produce an accepted best lag");
    check(probe.metrics().attempts == 4 && probe.metrics().rejected == 4 && !probe.metrics().accepted,
          "energy rejections accumulate without claiming calibration");
}

void independentNoise()
{
    Probe probe;
    Mask mask{};
    mask.fill(1);
    const Reference reference = source();
    const Reference independent = source(0x09871243);
    Microphone microphone{};
    for (size_t i = 0; i < microphone.size(); ++i) microphone[i] = independent[i];
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::LowCorrelation,
          "independent noise cannot be accepted from its largest searched lag");
    for (size_t i = 0; i < microphone.size(); ++i)
        microphone[i] = static_cast<int16_t>(independent[i] / 2 + reference[1280 + i - 352] / 16);
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::LowCorrelation,
          "dominant uncorrelated near-end signal rejects weak echo evidence");
}

void maskAndInputRejections()
{
    Probe probe;
    Mask mask{};
    mask.fill(1);
    const Reference reference = source();
    const Microphone microphone = delayed(reference, 352);
    mask[0] = 0;  // 即使只是远处的无效延迟点，也表示搜索范围未得到完整支持。
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::IncompleteReference,
          "unknown part of the bounded search cannot be treated as valid silence");
    mask.fill(0);
    for (size_t i = 1280 - 352; i < 1280 - 352 + 320; ++i) mask[i] = 1;
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::IncompleteReference,
          "one valid candidate window alone does not demonstrate a delay peak");
    mask.fill(1);
    check(!probe.process(nullptr, reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::InvalidInput, "null microphone rejected");
    check(!probe.process(microphone.data(), nullptr, mask.data()), "null reference rejected");
    check(!probe.process(microphone.data(), reference.data(), nullptr), "null mask rejected");
    check(probe.metrics().attempts == probe.metrics().accepted + probe.metrics().rejected,
          "all attempts accounted for");
}

void rangeAndAmbiguity()
{
    Probe probe;
    Mask mask{};
    mask.fill(1);
    Reference reference = source();
    for (const size_t delay : {size_t{0}, size_t{1280}}) {
        const Microphone microphone = delayed(reference, delay);
        check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
              probe.metrics().reject_reason == Reason::BoundaryPeak &&
              probe.metrics().best_delay_samples == delay && probe.metrics().correlation_per_mille == 1000,
              "range edge is reported as a candidate, not accepted as a demonstrated peak");
    }
    const Reference noise = source();
    for (size_t i = 0; i < reference.size(); ++i) reference[i] = noise[i % 160];
    const Microphone microphone = delayed(reference, 352);
    check(!probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.metrics().reject_reason == Reason::AmbiguousPeak &&
          probe.metrics().correlation_per_mille == 1000 && probe.metrics().peak_margin_per_mille == 0,
          "periodic signal has competing peaks and cannot establish unique delay");
    check(!probe.metrics().last_accepted && !probe.metrics().calibrated,
          "rejection never leaves prior acceptance/calibration status active");
    check(Probe::rejectReasonName(Reason::AmbiguousPeak)[0] != '\0', "diagnostic reason name available");
}

void scopedAcceptedStatistics()
{
    Probe probe;
    Mask mask{};
    mask.fill(1);
    const Reference reference = source();
    Microphone microphone = delayed(reference, 352);
    check(probe.process(microphone.data(), reference.data(), mask.data()) &&
          probe.playbackStatsCount() == 0, "unscoped observations keep only lifecycle metrics");
    probe.setPlaybackScope(3, 12, 40);
    check(probe.playbackStatsCount() == 1, "first nonzero playback identity opens one scope");
    for (const size_t delay : {size_t{352}, size_t{784}, size_t{352}, size_t{16}}) {
        microphone = delayed(reference, delay, delay == 16 ? -8 : 8);
        check(probe.process(microphone.data(), reference.data(), mask.data()), "scoped known delay accepted");
    }
    Probe::PlaybackStats stats{};
    check(probe.copyPlaybackStats(0, &stats) && stats.dma_generation == 3 &&
          stats.playback_serial == 12 && stats.volume_percent == 40 && stats.attempts == 4 &&
          stats.accepted == 4 && stats.rejected == 0 && !stats.saturated && stats.has_accepted,
          "playback identity, volume and counts copied without PCM");
    check(stats.last_accepted_delay_samples == 16 && stats.last_accepted_correlation_per_mille == 1000 &&
          stats.last_accepted_peak_margin_per_mille >= Probe::kMinimumPeakMarginPermille &&
          stats.last_accepted_polarity == -1 && stats.accepted_min_delay_samples == 16 &&
          stats.accepted_max_delay_samples == 784, "accepted observation and min/max persist");
    check(stats.accepted_delay_histogram[1] == 1 && stats.accepted_delay_histogram[22] == 2 &&
          stats.accepted_delay_histogram[49] == 1 && stats.accepted_delay_histogram[0] == 0 &&
          stats.accepted_delay_histogram[80] == 0, "accepted lag distribution uses the 1ms grid");
    const auto last_delay = stats.last_accepted_delay_samples;
    const auto last_margin = stats.last_accepted_peak_margin_per_mille;
    mask[0] = 0;
    check(!probe.process(microphone.data(), reference.data(), mask.data()), "scoped mask rejection fixture");
    microphone.fill(12000);
    mask.fill(1);
    check(!probe.process(microphone.data(), reference.data(), mask.data()), "scoped DC rejection fixture");
    check(probe.copyPlaybackStats(0, &stats) && stats.attempts == 6 && stats.accepted == 4 &&
          stats.rejected == 2 && stats.last_accepted_delay_samples == last_delay &&
          stats.last_accepted_peak_margin_per_mille == last_margin && stats.last_accepted_polarity == -1 &&
          stats.rejected_reason_counts[static_cast<size_t>(Reason::IncompleteReference)] == 1 &&
          stats.rejected_reason_counts[static_cast<size_t>(Reason::LowMicrophoneEnergy)] == 1 &&
          stats.rejected_reason_counts[0] == 0, "rejects retain accepted values and aggregate their reasons");
    check(!probe.metrics().last_accepted && !probe.metrics().calibrated &&
          probe.metrics().attempts == 7 && probe.metrics().accepted == 5,
          "lightweight metrics still describe the latest attempt and whole lifetime");
}

void scopedLifecycleAndBoundedHistory()
{
    Probe probe;
    probe.setPlaybackScope(0, 1, 40);
    probe.setPlaybackScope(1, 0, 40);
    probe.setPlaybackScope(1, 1, 101);
    check(probe.playbackStatsCount() == 0, "zero or invalid identity labels create no scope");
    probe.setPlaybackScope(1, 10, 30);
    probe.setPlaybackScope(1, 10, 60);
    Probe::PlaybackStats stats{};
    check(probe.playbackStatsCount() == 1 && probe.copyPlaybackStats(0, &stats) &&
          stats.volume_percent == 30, "current identity is idempotent and its volume label immutable");
    probe.setPlaybackScope(0, 0, 0);
    check(!probe.process(nullptr, nullptr, nullptr), "unidentified attempt rejected only in lifecycle metrics");
    check(probe.copyPlaybackStats(0, &stats) && stats.attempts == 0, "unidentified attempts do not pollute prior scope");
    probe.setPlaybackScope(1, 10, 30);
    check(!probe.process(nullptr, nullptr, nullptr) && probe.playbackStatsCount() == 1 &&
          probe.copyPlaybackStats(0, &stats) && stats.attempts == 1, "resuming current identity reuses its scope");
    probe.setPlaybackScope(1, 11, 40);
    probe.setPlaybackScope(2, 11, 50);
    probe.setPlaybackScope(3, 12, 60);
    check(probe.playbackStatsCount() == Probe::kPlaybackScopes &&
          probe.copyPlaybackStats(3, &stats) && stats.dma_generation == 1 && stats.playback_serial == 10 &&
          stats.attempts == 1, "four playback scopes preserve oldest-to-newest identities");
    probe.setPlaybackScope(4, 13, 70);
    check(probe.playbackStatsCount() == Probe::kPlaybackScopes &&
          probe.copyPlaybackStats(0, &stats) && stats.dma_generation == 4 && stats.playback_serial == 13 &&
          stats.attempts == 0 && stats.volume_percent == 70, "fifth scope overwrites the oldest in bounded storage");
    check(probe.copyPlaybackStats(3, &stats) && stats.dma_generation == 1 && stats.playback_serial == 11,
          "remaining four scopes keep correct newest-index ordering");
    stats.dma_generation = 12345;
    check(!probe.copyPlaybackStats(Probe::kPlaybackScopes, &stats) && stats.dma_generation == 12345 &&
          !probe.copyPlaybackStats(0, nullptr), "invalid copies preserve output and reject null destination");
    probe.setPlaybackScope(1, 11, 80);
    check(probe.copyPlaybackStats(0, &stats) && stats.volume_percent == 80 && stats.attempts == 0,
          "return to a non-current identity creates a fresh measurement scope");
    probe.reset();
    check(probe.playbackStatsCount() == 0 && !probe.copyPlaybackStats(0, &stats) &&
          probe.metrics().attempts == 0 && !probe.metrics().calibrated, "reset clears lifecycle and all scoped observations");
    probe.setPlaybackScope(1, 1, 0);
    check(probe.copyPlaybackStats(0, &stats) && !stats.has_accepted && stats.attempts == 0 &&
          stats.accepted_min_delay_samples == 0 && stats.accepted_delay_histogram[22] == 0,
          "new scope after reset has no stale accepted values or distribution");
    static_assert(sizeof(Probe) <= 2048, "Acoustic playback observations must remain a small fixed allocation");
}

void saturatedStatistics()
{
    Probe probe;
    probe.setPlaybackScope(5, 25, 40);
    auto *stats = voiceaudio::AcousticDelayProbeTestAccess::stats(probe);
    auto &metrics = voiceaudio::AcousticDelayProbeTestAccess::metrics(probe);
    constexpr uint32_t maximum = std::numeric_limits<uint32_t>::max();
    stats->attempts = stats->accepted = stats->accepted_delay_histogram[22] = maximum - 1;
    metrics.attempts = metrics.accepted = maximum - 1;
    Mask mask{};
    mask.fill(1);
    const Reference reference = source();
    const Microphone microphone = delayed(reference, 352);
    check(probe.process(microphone.data(), reference.data(), mask.data()) &&
          stats->attempts == maximum && stats->accepted == maximum &&
          stats->accepted_delay_histogram[22] == maximum && !stats->saturated,
          "reaching counter maximum does not wrap or falsely report lost increments");
    check(probe.process(microphone.data(), reference.data(), mask.data()) &&
          stats->attempts == maximum && stats->accepted == maximum &&
          stats->accepted_delay_histogram[22] == maximum && stats->saturated &&
          metrics.attempts == maximum && metrics.accepted == maximum,
          "accepted counters and histogram saturate explicitly without wrap");
    const auto accepted_delay = stats->last_accepted_delay_samples;
    stats->rejected = maximum;
    stats->rejected_reason_counts[static_cast<size_t>(Reason::InvalidInput)] = maximum;
    metrics.rejected = maximum;
    check(!probe.process(nullptr, reference.data(), mask.data()) && stats->rejected == maximum &&
          stats->rejected_reason_counts[static_cast<size_t>(Reason::InvalidInput)] == maximum &&
          stats->last_accepted_delay_samples == accepted_delay && stats->has_accepted && metrics.rejected == maximum,
          "rejection saturation preserves the latest accepted measurement");
    probe.setPlaybackScope(6, 26, 40);
    Probe::PlaybackStats fresh{};
    check(probe.copyPlaybackStats(0, &fresh) && !fresh.saturated && !fresh.has_accepted && fresh.attempts == 0,
          "counter saturation does not contaminate a new scope");
}
}  // 命名空间

int main()
{
    knownLagsAndDc();
    energyAndDcRejections();
    independentNoise();
    maskAndInputRejections();
    rangeAndAmbiguity();
    scopedAcceptedStatistics();
    scopedLifecycleAndBoundedHistory();
    saturatedStatistics();
    std::cout << "acoustic delay probe: 8 scenario groups passed (correlation/rejection, persistent scoped accepted distribution, bounded history/lifecycle, saturation); no hardware calibration claimed\n";
}
