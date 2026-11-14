#include "source_signal_history.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

using History = voiceaudio::SourceSignalHistory;
using Frame = std::array<int16_t, History::kFrameSamples>;

namespace {
void append(History &history, const Frame &raw, const Frame &dc, const Frame &reference)
{
    assert(history.append(History::measure(raw.data()), History::measure(dc.data()),
                          History::measure(reference.data())));
}
History::Signal expected(const std::vector<int16_t> &samples)
{
    assert(!samples.empty() && samples.size() % History::kFrameSamples == 0);
    long double sum = 0, squares = 0;
    uint32_t peak = 0, maximum = 0;
    for (const int32_t sample : samples) {
        sum += sample;
        squares += static_cast<long double>(sample) * sample;
        peak = std::max(peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
    }
    for (size_t first = 0; first < samples.size(); first += History::kFrameSamples) {
        long double mean = 0, variance = 0;
        for (size_t i = first; i < first + History::kFrameSamples; ++i) mean += samples[i];
        mean /= History::kFrameSamples;
        for (size_t i = first; i < first + History::kFrameSamples; ++i) {
            const long double centered = samples[i] - mean;
            variance += centered * centered;
        }
        maximum = std::max(maximum, static_cast<uint32_t>(std::sqrt(variance / History::kFrameSamples)));
    }
    const long double mean = sum / samples.size();
    long double variance = 0;
    for (const int32_t sample : samples) {
        const long double centered = sample - mean;
        variance += centered * centered;
    }
    return {static_cast<int32_t>(mean), static_cast<uint32_t>(std::sqrt(squares / samples.size())),
            static_cast<uint32_t>(std::sqrt(variance / samples.size())), peak, maximum};
}
void equal(const History::Signal &actual, const History::Signal &reference)
{
    assert(actual.mean == reference.mean && actual.rms == reference.rms &&
           actual.ac_rms == reference.ac_rms && actual.peak == reference.peak &&
           actual.max_frame_ac_rms == reference.max_frame_ac_rms);
}
void invalid(const History::Window &window)
{
    assert(!window.valid && window.frames == 0 && window.samples == 0);
    assert(window.ordinal_mapping && !window.lag_calibrated);
}
}

int main()
{
    unsigned scenarios = 0;
    {
        History history;
        invalid(history.window(0, 1));
        Frame raw{}, dc{}, reference{};
        raw.fill(-32768);
        dc.fill(123);
        for (size_t i = 0; i < reference.size(); ++i) reference[i] = i % 2 ? 32767 : -32768;
        append(history, raw, dc, reference);
        invalid(history.window(0, 320)); // 只是已接受，尚未送入 SDK。
        history.commit(321);             // 无效提交不能发布未来证据。
        invalid(history.window(0, 320));
        history.commit(256);
        invalid(history.window(0, 256)); // 必须提交完整 20 ms 统计帧。
        history.commit(64);
        const auto window = history.window(1, 319);
        assert(window.valid && window.requested_first == 1 && window.requested_end == 319);
        assert(window.covered_first == 0 && window.covered_end == 320 && window.frames == 1 && window.samples == 320);
        assert(window.available_first == 0 && window.available_end == 320);
        equal(window.raw, {-32768, 32768, 0, 32768, 0});
        equal(window.dc, {123, 123, 0, 123, 0});
        equal(window.reference, {0, 32767, 32767, 32768, 32767});
        ++scenarios;
    }
    {
        History history;
        std::vector<int16_t> raw_values, dc_values, reference_values;
        for (unsigned frame = 0; frame < 8; ++frame) {
            Frame raw{}, dc{}, reference{};
            for (size_t i = 0; i < raw.size(); ++i) {
                raw[i] = static_cast<int16_t>((i * 73 + frame * 193) % 2003 - 900);
                dc[i] = frame % 2 ? -300 : 500;
                reference[i] = static_cast<int16_t>(-37 - static_cast<int>(i % 13));
            }
            append(history, raw, dc, reference);
            raw_values.insert(raw_values.end(), raw.begin(), raw.end());
            dc_values.insert(dc_values.end(), dc.begin(), dc.end());
            reference_values.insert(reference_values.end(), reference.begin(), reference.end());
        }
        history.commit(8 * 320);
        const auto window = history.window(0, 8 * 320);
        assert(window.valid && window.frames == 8);
        equal(window.raw, expected(raw_values));
        equal(window.dc, expected(dc_values));
        equal(window.reference, expected(reference_values));
        // 帧间直流变化计入整个窗口的中心化信号，
        // 但每个独立恒定帧的交流分量均为零。
        assert(window.dc.ac_rms == 400 && window.dc.max_frame_ac_rms == 0);
        invalid(history.window(320, 320));
        invalid(history.window(321, 320));
        invalid(history.window(0, 2561));
        invalid(history.window(std::numeric_limits<uint64_t>::max() - 1,
                               std::numeric_limits<uint64_t>::max()));
        ++scenarios;
    }
    {
        // SDK 块为 512 样本；排队的 320 样本输入不能让
        // 适配器尚未消费的尾部被输出窗口查询使用。
        History history;
        Frame values{};
        values.fill(42);
        for (unsigned i = 0; i < 3; ++i) append(history, values, values, values);
        history.commit(512);
        assert(history.window(0, 320).valid);
        const auto future = history.window(320, 512);
        invalid(future);
        assert(future.available_end == 320 && future.covered_end == 640);
        append(history, values, values, values);
        history.commit(512);
        assert(history.window(0, 960).valid);
        invalid(history.window(960, 1024));
        ++scenarios;
    }
    {
        History history;
        for (unsigned frame = 0; frame < History::kCapacity + 2; ++frame) {
            Frame values{};
            values.fill(static_cast<int16_t>(frame - 70));
            append(history, values, values, values);
            history.commit(320);
        }
        const auto expired = history.window(0, 320);
        invalid(expired);
        assert(expired.available_first == 640 && expired.available_end == 130 * 320);
        const auto oldest = history.window(640, 960);
        assert(oldest.valid && oldest.raw.mean == -68);
        const auto entire = history.window(640, 130 * 320);
        assert(entire.valid && entire.frames == 128 && entire.samples == 40960);
        invalid(history.window(639, 130 * 320));
        // 尚未发布的追加可以覆盖最旧已提交槽。
        // 顺序身份必须拒绝陈旧的已公布边界。
        Frame replacement{};
        replacement.fill(777);
        append(history, replacement, replacement, replacement);
        invalid(history.window(640, 960));
        assert(history.window(960, 1280).valid);
        ++scenarios;
    }
    {
        // 单 feed 写者与 fetch 读者并发：任何可用结果都必须
        // 完整属于同一已发布序号，不能包含撕裂的 64 位字段
        // 或混合的 raw/DC/R 通道，无须等待。
        History history;
        std::atomic<bool> finished{false};
        std::atomic<uint64_t> published{0};
        std::thread writer([&] {
            for (unsigned frame = 0; frame < 5000; ++frame) {
                Frame raw{}, dc{}, reference{};
                const int amplitude = 100 + frame % 1000;
                for (size_t i = 0; i < raw.size(); ++i) raw[i] = (i % 2 ? amplitude : -amplitude) + 37;
                dc.fill(static_cast<int16_t>(-static_cast<int>(frame % 1000)));
                reference.fill(static_cast<int16_t>(2000 + frame % 1000));
                append(history, raw, dc, reference);
                history.commit(320);
                published.store(static_cast<uint64_t>(frame + 1) * 320, std::memory_order_release);
            }
            finished = true;
        });
        unsigned valid = 0;
        do {
            const uint64_t end = published.load(std::memory_order_acquire);
            if (!end) continue;
            const auto result = history.window(end - 320, end);
            if (!result.valid) continue;
            const unsigned frame = static_cast<unsigned>(end / 320 - 1);
            const uint32_t amplitude = 100 + frame % 1000;
            assert(result.raw.mean == 37 && result.raw.ac_rms == amplitude &&
                   result.raw.peak == amplitude + 37 && result.raw.max_frame_ac_rms == amplitude);
            equal(result.dc, {-static_cast<int32_t>(frame % 1000), frame % 1000, 0, frame % 1000, 0});
            equal(result.reference, {static_cast<int32_t>(2000 + frame % 1000), 2000 + frame % 1000,
                                     0, 2000 + frame % 1000, 0});
            ++valid;
        } while (!finished.load());
        writer.join();
        assert(valid > 0);
        assert(history.window(4999 * 320, 5000 * 320).valid);
        ++scenarios;
    }
    std::printf("source_signal_history: %u scenarios passed; no DSP/alignment claim\n", scenarios);
}
