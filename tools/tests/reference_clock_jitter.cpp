#include "reference_timeline.hpp"
#include "esp_heap_caps.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Timeline = voiceaudio::ReferenceTimeline;
namespace {
constexpr int64_t kStartUs = 12300000000LL;
size_t allocations = 0;
struct TraceResult {
    Timeline::Metrics metrics;
    Timeline::FirstClockFailure failure;
    uint32_t first_bad_ms = 0;
};
enum class Jitter { None, RxWindow, TxWindow, RxStartup, TxStartup, RepeatedRx, RepeatedTx };

TraceResult trace(double rx_ppm, double tx_ppm, Jitter jitter, uint64_t sample_base = 0)
{
    Timeline timeline;
    assert(timeline.start());
    // 样本持续采集，只改变某一次回调的时间戳；
    // 没有丢弃输入，也没有伪造参考区间。
    timeline.observeMicrophoneDma(sample_base + 256,
        kStartUs + 16000 + (jitter == Jitter::RxStartup ? 3000 : 0), 0);
    std::array<int16_t, 160> pcm{};
    std::array<int16_t, 320> output{};
    std::array<uint8_t, 320> mask{};
    const double rx_rate = 16000.0 * (1.0 + rx_ppm / 1000000.0);
    const double tx_rate = 24000.0 * (1.0 + tx_ppm / 1000000.0);
    TraceResult result{};
    for (uint32_t ms = 2; ms <= 12000; ms += 2) {
        if (ms > 16 && ms % 16 == 0) {
            const bool late = (jitter == Jitter::RxWindow && ms == 1024) ||
                (jitter == Jitter::RepeatedRx && ms >= 1024 && (ms - 16) % 1008 == 0);
            // 每条漂移轨迹都包含小幅、零均值的回调抖动。
            const int64_t noise = rx_ppm || tx_ppm ? static_cast<int>(ms % 7) * 3 - 9 : 0;
            const int64_t at = kStartUs + static_cast<int64_t>(std::llround(ms * 16.0 * 1000000.0 / rx_rate));
            timeline.observeMicrophoneDma(sample_base + static_cast<uint64_t>(ms) * 16,
                at + noise + (late ? 3000 : 0), 0);
        }
        if (ms % 10 == 0) {
            const bool late = (jitter == Jitter::TxWindow && ms == 1010) ||
                (jitter == Jitter::TxStartup && ms == 10) ||
                (jitter == Jitter::RepeatedTx && ms > 10 && ms % 1000 == 10);
            const int64_t noise = rx_ppm || tx_ppm ? static_cast<int>((ms / 10) % 5) * 4 - 8 : 0;
            const int64_t at = kStartUs + static_cast<int64_t>(std::llround(ms * 24.0 * 1000000.0 / tx_rate));
            const uint64_t first = static_cast<uint64_t>(ms - 10) * 16;
            pcm.fill(1234);
            timeline.append(1, 77, static_cast<uint64_t>(ms) * 24,
                at + noise + (late ? 3000 : 0), first, static_cast<int64_t>(first * 3) - 95,
                pcm.data(), pcm.size(), 0);
        }
        const auto metrics = timeline.metrics();
        if (metrics.clock_errors && !result.first_bad_ms) result.first_bad_ms = ms;
        if (metrics.healthy && ms >= 400 && ms % 100 == 0) {
            const uint64_t first = sample_base + static_cast<uint64_t>(ms - 120) * 16;
            size_t count = 0;
            assert(timeline.copyMicrophoneFrame(first, output.size(), output.data(), output.size(),
                                                1, 77, &count, mask.data()));
            assert(count == output.size());
            for (size_t i = 0; i < count; ++i) assert(output[i] == 1234 && mask[i] == 1);
        }
    }
    result.metrics = timeline.metrics();
    result.failure = timeline.firstClockFailure();
    return result;
}

void healthyJitter(double rx_ppm, double tx_ppm, Jitter jitter, uint64_t base = 0)
{
    const auto r = trace(rx_ppm, tx_ppm, jitter, base);
    assert(r.metrics.healthy && !r.failure.valid && r.metrics.clock_errors == 0);
    assert(r.metrics.gap_blocks == 0 && r.metrics.unmapped_blocks == 0);
    std::printf("jitter survived rx_ppm=%.0f tx_ppm=%.0f case=%u suspects=%u base=%llu\n",
        rx_ppm, tx_ppm, static_cast<unsigned>(jitter), r.metrics.clock_rate_suspect_windows,
        static_cast<unsigned long long>(base));
}

void sustainedDrift(bool receive, double ppm)
{
    const auto r = trace(receive ? ppm : 0, receive ? 0 : ppm, Jitter::None);
    assert(!r.metrics.healthy && r.failure.valid && r.metrics.clock_errors == 1);
    assert(r.failure.source == (receive ? Timeline::ClockSource::Microphone : Timeline::ClockSource::Playback));
    assert(r.failure.reason == Timeline::ClockFailureReason::RateOutOfRange);
    assert(r.failure.consecutive_bad_windows == 2);
    assert(r.first_bad_ms >= 1980 && r.first_bad_ms <= 2080);
    assert((r.failure.observed_ppm < 0) == (ppm < 0));
    assert(r.failure.window_us >= 1000000 && r.failure.window_us <= 1030000);
    std::printf("persistent drift rejected source=%s ppm=%.0f first_bad_ms=%u observed_ppm=%ld\n",
        receive ? "RX" : "TX", ppm, r.first_bad_ms, static_cast<long>(r.failure.observed_ppm));
}

void firstFailureRetention()
{
    Timeline t;
    assert(t.start());
    t.observeMicrophoneDma(256, kStartUs + 16000, 0);
    t.observeMicrophoneDma(512, kStartUs + 16000, 0);
    const auto original = t.firstClockFailure();
    assert(original.valid && original.reason == Timeline::ClockFailureReason::NonMonotonic);
    t.observeMicrophoneDma(768, kStartUs + 48000, 1, false);
    const auto retained = t.firstClockFailure();
    assert(retained.reason == original.reason && retained.sample_end == original.sample_end);
    assert(retained.observed_end_us == original.observed_end_us && t.metrics().clock_errors == 1);
    t.stop(); assert(t.start());
    assert(!t.firstClockFailure().valid && t.metrics().clock_rate_suspect_windows == 0);
    t.observeMicrophoneDma(256, kStartUs + 16000, 0);
    t.observeMicrophoneDma(512, kStartUs + 82001, 0);
    assert(t.firstClockFailure().reason == Timeline::ClockFailureReason::ClockJump);
    assert(t.firstClockFailure().residual_us == 50001);
    t.stop(); assert(t.start());
    t.observeMicrophoneDma(256, kStartUs + 16000, 1, false);
    assert(t.firstClockFailure().reason == Timeline::ClockFailureReason::MicrophoneOverflow);
    t.stop(); assert(t.start());
    t.observeMicrophoneDma(256, kStartUs + 16000, 0, false);
    assert(t.firstClockFailure().reason == Timeline::ClockFailureReason::MicrophoneInvalid);
    t.stop(); assert(t.start());
    t.observeMicrophoneDma(256, -1, 0);
    assert(t.firstClockFailure().reason == Timeline::ClockFailureReason::InvalidTimestamp);
    t.stop(); assert(t.start());
    t.observeMicrophoneDma(256, kStartUs + 16000, 0);
    t.observeMicrophoneDma(UINT64_MAX, INT64_MAX, 0);
    assert(t.firstClockFailure().reason == Timeline::ClockFailureReason::ClockJump);
    assert(t.firstClockFailure().residual_us == INT64_MIN);
    assert(std::strcmp(Timeline::clockSourceName(Timeline::ClockSource::Playback), "playback") == 0);
    assert(std::strcmp(Timeline::clockFailureReasonName(Timeline::ClockFailureReason::ClockJump), "clock_jump") == 0);
}
}  // 命名空间

void *heap_caps_malloc(size_t bytes, int caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations;
    return std::malloc(bytes);
}
void heap_caps_free(void *p) { assert(p && allocations); --allocations; std::free(p); }

int main()
{
    for (const auto jitter : {Jitter::None, Jitter::RxWindow, Jitter::TxWindow, Jitter::RxStartup,
                              Jitter::TxStartup, Jitter::RepeatedRx, Jitter::RepeatedTx}) {
        healthyJitter(0, 0, jitter);
        healthyJitter(250, -250, jitter);
        healthyJitter(-250, 250, jitter, 1ULL << 32);
    }
    for (bool receive : {false, true})
        for (double ppm : {-5000.0, -3000.0, -2500.0, 2500.0, 3000.0, 5000.0})
            sustainedDrift(receive, ppm);
    firstFailureRetention();
    assert(allocations == 0);
    std::puts("Passed clock jitter: isolated/window/startup/repeated 3ms ISR delay, continuous PCM coverage, +/-250ppm with jitter and 64-bit samples; sustained +/-2500/3000/5000ppm rejected by 2080ms; first failure retained; 50ms jump/nonmonotonic/overflow/invalid remain fatal");
}
