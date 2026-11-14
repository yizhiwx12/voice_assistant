#include "reference_timeline.hpp"
#ifdef REFERENCE_TIMELINE_BASELINE_COMPARE
// 仅开发用的差分构建从 build/aec-throughput-baseline
// 提供保留的优化前生产类，并重命名其类型。
#include "original_reference_timeline.hpp"
#endif
#include "esp_heap_caps.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace {
bool fail_allocation = false;
size_t outstanding = 0;
size_t allocated_bytes = 0;
using Timeline = voiceaudio::ReferenceTimeline;
std::array<int16_t, 160> pcm{};
std::array<int16_t, 320> output{};
std::array<uint8_t, 320> valid{};
constexpr int64_t kStartUs = 12300000000LL;

void nominalBlock(Timeline &timeline, uint32_t index, uint32_t generation = 1,
                   uint32_t serial = 77, bool interval_valid = true, size_t warmup = 0,
                   uint64_t rx_base = 0)
{
    const uint64_t first = static_cast<uint64_t>(index) * 160;
    const int64_t end_us = kStartUs + static_cast<int64_t>(index + 1) * 10000;
    timeline.observeMicrophoneDma(rx_base + first + 160, end_us, 0);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<int16_t>(first + i);
    const bool result = timeline.append(generation, serial, static_cast<uint64_t>(index + 1) * 240,
        end_us, first, static_cast<int64_t>(first * 3) - 95, pcm.data(), pcm.size(), warmup, interval_valid);
    assert(result == interval_valid);
}

void drift(double rx_ppm, double tx_ppm, bool jitter)
{
    Timeline timeline;
    assert(timeline.start());
    const double rx_rate = 16000.0 * (1.0 + rx_ppm / 1000000.0);
    const double tx_rate = 24000.0 * (1.0 + tx_ppm / 1000000.0);
    pcm.fill(1000);
    uint64_t first_output = 0;
    for (size_t n = 1; n <= 6000; ++n) {
        const int64_t rx_jitter = jitter ? static_cast<int64_t>(n % 7) * 3 - 9 : 0;
        const int64_t tx_jitter = jitter ? static_cast<int64_t>(n % 5) * 4 - 8 : 0;
        const int64_t rx_us = kStartUs + static_cast<int64_t>(std::llround(n * 160.0 * 1000000.0 / rx_rate)) + rx_jitter;
        const int64_t tx_us = kStartUs + static_cast<int64_t>(std::llround(n * 240.0 * 1000000.0 / tx_rate)) + tx_jitter;
        timeline.observeMicrophoneDma(n * 160, rx_us, 0);
        assert(timeline.append(1, 99, n * 240, tx_us, first_output,
            static_cast<int64_t>(first_output * 3) - 95, pcm.data(), pcm.size(), n == 1 ? 64 : 0));
        first_output += pcm.size();
        if (n > 500 && n % 100 == 0) {
            size_t count = 0;
            assert(timeline.copyMicrophoneFrame((n - 8) * 160, output.size(), output.data(), output.size(), 1, 99, &count, valid.data()));
            assert(count == output.size());
            for (size_t i = 0; i < output.size(); ++i) assert(output[i] == 1000 && valid[i] == 1);
        }
    }
    const auto metrics = timeline.metrics();
    assert(metrics.healthy && !metrics.acoustically_calibrated);
    assert(metrics.blocks_available == Timeline::kHistoryBlocks && metrics.history_overwrites > 0);
    assert(metrics.clock_errors == 0 && metrics.unmapped_blocks == 0 && metrics.gap_blocks == 0);
    assert(std::abs(metrics.microphone_rate_ppm - rx_ppm) < 30);
    assert(std::abs(metrics.playback_rate_ppm - tx_ppm) < 30);
}

// 对高变化轨迹中的每个 PCM 样本、掩码、匹配数及完成结果计算哈希。
// 测试数据来自保留的原实现，不是查询优化后实现生成的。
// 开发用差分构建还会直接重跑原始类。
template <class TimelineType>
uint64_t variationTrace(uint64_t rx_base, double rx_ppm, double tx_ppm)
{
    TimelineType timeline;
    assert(timeline.start());
    std::array<int16_t, 160> source{};
    std::array<int16_t, 320> actual{};
    std::array<uint8_t, 320> mask{};
    uint64_t hash = 1469598103934665603ULL;
    const auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    const double rx_rate = 16000.0 * (1.0 + rx_ppm / 1000000.0);
    const double tx_rate = 24000.0 * (1.0 + tx_ppm / 1000000.0);
    uint64_t clock_block = 0, generation_origin = 0;
    uint32_t generation = 1;
    for (uint32_t index = 0; index < 600; ++index) {
        // 缺失整个源区间，验证连续性屏障。
        clock_block += index && index % 157 == 0 ? 2 : 1;
        if (index == 300) { generation_origin = clock_block - 1; ++generation; }
        const uint64_t local_block = clock_block - generation_origin;
        const uint64_t first_output = (local_block - 1) * source.size();
        const uint32_t serial = 77 + index / 67;
        const bool interval_valid = index % 113 != 41;
        // 首代次有意包含负的有效源坐标。
        // 之后的预热裁剪验证部分范围及空范围。
        const size_t warmup = index == 300 ? 64 : (index % 89 == 53 ? 159 : 0);
        for (size_t i = 0; i < source.size(); ++i) {
            uint32_t value = static_cast<uint32_t>(first_output + i) * 1103515245U + serial * 12345U;
            value ^= value >> 13;
            source[i] = static_cast<int16_t>(static_cast<int32_t>(value & 0xffffU) - 32768);
        }
        const int64_t rx_us = kStartUs + static_cast<int64_t>(std::llround(
            clock_block * 160.0 * 1000000.0 / rx_rate)) + static_cast<int64_t>(index % 7) * 3 - 9;
        const int64_t tx_us = kStartUs + static_cast<int64_t>(std::llround(
            clock_block * 240.0 * 1000000.0 / tx_rate)) + static_cast<int64_t>(index % 5) * 4 - 8;
        timeline.observeMicrophoneDma(rx_base + clock_block * 160, rx_us, 0);
        assert(timeline.append(generation, serial, local_block * 240, tx_us, first_output,
            static_cast<int64_t>(first_output * 3) - 95, source.data(), source.size(), warmup,
            interval_valid) == interval_valid);
        const auto metrics = timeline.metrics();
        assert(metrics.healthy && metrics.clock_errors == 0 && metrics.unmapped_blocks == 0);
        add(metrics.reference_range_available);
        add(metrics.oldest_reference_microphone_sample);
        add(metrics.latest_reference_microphone_sample_end);
        if constexpr (std::is_same<TimelineType, Timeline>::value) {
            const auto range = timeline.healthRange();
            assert(range.healthy == metrics.healthy && range.unmapped_blocks == metrics.unmapped_blocks);
            assert(range.reference_range_available == metrics.reference_range_available);
            assert(range.oldest_reference_microphone_sample == metrics.oldest_reference_microphone_sample);
            assert(range.latest_reference_microphone_sample_end == metrics.latest_reference_microphone_sample_end);
        }
        const uint64_t rx_end = rx_base + clock_block * 160;
        // 相邻整数位置覆盖块内/跨块的小数插值、
        // 过期历史及未来覆盖范围。
        for (uint64_t behind : {0ULL, 1ULL, 31ULL, 32ULL, 33ULL, 159ULL, 160ULL, 319ULL, 4801ULL}) {
            const uint64_t first = behind <= rx_end ? rx_end - behind : 0;
            for (size_t samples : {size_t{0}, size_t{1}, size_t{160}, size_t{320}}) {
                for (unsigned filter = 0; filter < 4; ++filter) {
                    size_t matched = 0;
                    const uint32_t query_generation = filter == 0 ? 0 : generation;
                    const uint32_t query_serial = filter <= 1 ? 0 : serial + (filter == 3 ? 1 : 0);
                    const bool complete = timeline.copyMicrophoneFrame(first, samples,
                        actual.data(), actual.size(), query_generation, query_serial, &matched, mask.data());
                    add(complete); add(matched);
                    for (size_t i = 0; i < samples; ++i) {
                        add(static_cast<uint16_t>(actual[i]));
                        add(mask[i]);
                    }
                }
            }
        }
    }
    return hash;
}

void variationFixtures()
{
    struct Fixture { uint64_t base; double rx_ppm, tx_ppm; uint64_t hash; };
    const Fixture fixtures[] = {
        {0, 0, 0, 0x5ebb41417ad5f83dULL},
        {1ULL << 32, 250, -250, 0xe8db635882c60779ULL},
        {1ULL << 32, -250, 250, 0xf1a5e694070a7c63ULL},
        {1ULL << 52, 0, 0, 0x4b3bee86ffaf5d5cULL},
    };
    for (const auto &fixture : fixtures) {
        const uint64_t actual = variationTrace<Timeline>(fixture.base, fixture.rx_ppm, fixture.tx_ppm);
        std::printf("Variation trace base=%llu rx_ppm=%.0f tx_ppm=%.0f hash=%016llx\n",
            static_cast<unsigned long long>(fixture.base), fixture.rx_ppm, fixture.tx_ppm,
            static_cast<unsigned long long>(actual));
#ifdef REFERENCE_TIMELINE_BASELINE_COMPARE
        const uint64_t original = variationTrace<voiceaudio::OriginalReferenceTimeline>(
            fixture.base, fixture.rx_ppm, fixture.tx_ppm);
        assert(actual == original);
#endif
        assert(actual == fixture.hash);
    }
}
}  // 命名空间

void *heap_caps_malloc(size_t bytes, int caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_allocation) { fail_allocation = false; return nullptr; }
    ++outstanding;
    allocated_bytes = bytes;
    return std::malloc(bytes);
}
void heap_caps_free(void *value) { assert(value && outstanding); --outstanding; std::free(value); }

int main()
{
    {
        Timeline timeline;
        fail_allocation = true;
        assert(!timeline.start() && !timeline.metrics().available && outstanding == 0);
        assert(timeline.start() && !timeline.start());
        assert(allocated_bytes <= 12 * 1024);
        assert(!timeline.metrics().reference_range_available);
        nominalBlock(timeline, 0, 1, 77, true, 64);
        // 内容在麦克风网格的 -95/3 处开始。预热排除 64 之前的源点：
        // 首有效点为 32+1/3，对应整数 33；末点为 127+1/3。
        assert(timeline.metrics().reference_range_available);
        assert(timeline.metrics().oldest_reference_microphone_sample == 33);
        assert(timeline.metrics().latest_reference_microphone_sample_end == 128);
        nominalBlock(timeline, 1);
        nominalBlock(timeline, 2);
        assert(timeline.metrics().latest_reference_microphone_sample_end == 448);
        size_t count = 0;
        assert(timeline.copyMicrophoneFrame(80, output.size(), output.data(), output.size(), 1, 77, &count, valid.data()));
        assert(count == output.size());
        for (size_t i = 0; i < output.size(); ++i)
            assert(std::abs(output[i] - static_cast<int>(112 + i)) <= 1 && valid[i] == 1);
        // 预热带有掩码，不能当作真实零参考。
        assert(!timeline.copyMicrophoneFrame(0, 64, output.data(), output.size(), 1, 77, &count, valid.data()));
        assert(count > 0 && count < 64 && valid[0] == 0);
        nominalBlock(timeline, 3, 1, 77, false);
        // 存储的未知零值不能推进保留的有效边界。
        assert(timeline.metrics().latest_reference_microphone_sample_end == 448);
        assert(!timeline.copyMicrophoneFrame(460, 100, output.data(), output.size(), 1, 77, &count, valid.data()));
        assert(count == 0);
        nominalBlock(timeline, 4);
        assert(timeline.metrics().latest_reference_microphone_sample_end == 768);
        assert(!timeline.copyMicrophoneFrame(600, 64, output.data(), output.size(), 1, 77, &count, valid.data()));
        assert(count > 0 && count < 64);
        // 明确身份过滤排除全部其他播放流。
        assert(!timeline.copyMicrophoneFrame(80, 64, output.data(), output.size(), 1, 78, &count));
        assert(count == 0);
        // 代次重置改变 TX 原点，保留有限旧历史。
        const int64_t new_end = kStartUs + 60000;
        timeline.observeMicrophoneDma(960, new_end, 0);
        pcm.fill(2222);
        assert(timeline.append(2, 88, 240, new_end, 0, -95, pcm.data(), pcm.size(), 64));
        assert(timeline.metrics().generation_changes == 1);
        assert(timeline.copyMicrophoneFrame(80, 64, output.data(), output.size(), 1, 77, &count));
        assert(!timeline.append(1, 77, 1440, new_end + 1000, 800, 2305, pcm.data(), pcm.size(), 0));
        assert(timeline.metrics().healthy);
        timeline.stop(); timeline.stop();
    }
    assert(outstanding == 0);
    {
        Timeline timeline;
        assert(timeline.start());
        nominalBlock(timeline, 0, 1, 77, true, 0);
        // 初始映射块可以早于 RX 原点开始；
        // 诊断整数包络裁剪到零，不进行无符号转换。
        assert(timeline.metrics().reference_range_available);
        assert(timeline.metrics().oldest_reference_microphone_sample == 0);
        assert(timeline.metrics().latest_reference_microphone_sample_end == 128);
        for (uint32_t i = 1; i <= Timeline::kHistoryBlocks; ++i)
            nominalBlock(timeline, i, 1, 77, false);
        // 有效点过期后，保留的未知存储不是有效前沿。
        // 指标不能把其中零 PCM 重新解释为有效静音。
        assert(timeline.metrics().healthy);
        assert(!timeline.metrics().reference_range_available);
        assert(timeline.metrics().oldest_reference_microphone_sample == 0);
        assert(timeline.metrics().latest_reference_microphone_sample_end == 0);
    }
    {
        Timeline timeline;
        assert(timeline.start());
        timeline.observeMicrophoneDma(0, 0, 0);
        assert(timeline.metrics().clock_errors == 0);
        for (uint32_t i = 0; i < 40; ++i) nominalBlock(timeline, i, 1, 77, true, i == 0 ? 64 : 0);
        // 固定历史保留索引 10..39。端点运算仍包含小数，
        // 反映保留区间，而非自启动以来的全部产量。
        assert(timeline.metrics().oldest_reference_microphone_sample == 1569);
        assert(timeline.metrics().latest_reference_microphone_sample_end == 6368);
        size_t count = 0;
        assert(!timeline.copyMicrophoneFrame(80, 64, output.data(), output.size(), 1, 77, &count));
        assert(count == 0 && timeline.metrics().blocks_available == 30);
        timeline.observeMicrophoneDma(6560, kStartUs + 410000, 1);
        assert(!timeline.metrics().healthy && timeline.metrics().microphone_overflow_events == 1);
        assert(!timeline.copyMicrophoneFrame(6000, 64, output.data(), output.size(), 1, 77, &count));
        assert(count == 0);
    }
    {
        // 64 位时间/计数器可跨越两个低字回绕点继续工作。
        Timeline timeline;
        assert(timeline.start());
        const uint64_t rx_base = 1ULL << 32;
        for (uint32_t i = 0; i < 4; ++i) nominalBlock(timeline, i, 1, 77, true, i == 0 ? 64 : 0, rx_base);
        size_t count = 0;
        assert(timeline.copyMicrophoneFrame(rx_base + 80, 320, output.data(), output.size(), 1, 77, &count));
        assert(count == 320);
        assert(timeline.metrics().reference_range_available);
        assert(timeline.metrics().oldest_reference_microphone_sample == rx_base + 33);
        assert(timeline.metrics().latest_reference_microphone_sample_end == rx_base + 608);
    }
    {
        Timeline timeline;
        assert(timeline.start());
        timeline.observeMicrophoneDma(160, kStartUs, 0);
        pcm.fill(1234);
        assert(!timeline.append(1, 1, 240, kStartUs + 300000, 0, -95, pcm.data(), pcm.size(), 0));
        assert(timeline.metrics().unmapped_blocks == 1);
        timeline.observeMicrophoneDma(320, kStartUs + 1000000, 0);
        assert(!timeline.metrics().healthy && timeline.metrics().clock_errors == 1);
    }
    {
        Timeline timeline;
        assert(timeline.start());
        nominalBlock(timeline, 0);
        // 无效读取/DMA 与驱动队列溢出分别处理。
        timeline.observeMicrophoneDma(320, kStartUs + 20000, 0, false);
        assert(!timeline.metrics().healthy && timeline.metrics().microphone_overflow_events == 0);
        assert(timeline.metrics().clock_errors == 1);
    }
    {
        Timeline timeline;
        assert(timeline.start());
        timeline.observeMicrophoneDma(160, kStartUs, 0);
        timeline.observeMicrophoneDma(16160, kStartUs + 1005000, 0);
        // 单个迟到端点不能证明物理频率错误。
        // 第二个独立同方向窗口才能确认。
        assert(!timeline.firstClockFailure().valid && timeline.metrics().clock_errors == 0);
        assert(timeline.metrics().clock_rate_suspect_windows == 1);
        timeline.observeMicrophoneDma(32160, kStartUs + 2010000, 0);
        assert(!timeline.metrics().healthy && timeline.metrics().clock_errors == 1);
        const auto failure = timeline.firstClockFailure();
        assert(failure.valid && failure.source == Timeline::ClockSource::Microphone);
        assert(failure.reason == Timeline::ClockFailureReason::RateOutOfRange);
        assert(failure.consecutive_bad_windows == 2);
    }
    drift(250, -250, false);
    {
        Timeline timeline;
        assert(timeline.start());
        for (uint32_t i = 0; i < 8; ++i)
            nominalBlock(timeline, i, 1, i < 4 ? 77 : 78, true, i == 0 ? 64 : 0);
        size_t count = 0;
        // 标称时钟下，FIR 内容边界含小数，因此该查询
        // 需要两个相邻块，即使它们序号不同。
        assert(timeline.copyMicrophoneFrame(600, 100, output.data(), output.size(), 1, 0, &count));
        assert(count == 100 && timeline.metrics().gap_blocks == 0);
        assert(!timeline.copyMicrophoneFrame(600, 100, output.data(), output.size(), 1, 77, &count));
        assert(count > 0 && count < 100);
        assert(!timeline.copyMicrophoneFrame(600, 100, output.data(), output.size(), 1, 78, &count));
        assert(count > 0 && count < 100);
    }
    drift(-250, 250, true);
    variationFixtures();
    assert(outstanding == 0);
    std::puts("Passed reference timeline: fractional RX mapping, FIR warm-up, gaps/identity/generations, 300ms window, 64-bit clocks, stale/jump/overflow failure, allocation fallback, continuous +/-250ppm drift with jitter and original high-variation PCM fixtures at 0/2^32/2^52");
}
