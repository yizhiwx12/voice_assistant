#include "reference_resampler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
using Resampler = voiceaudio::ReferenceResampler;

uint64_t outputLimit(uint64_t input)
{
    return (input / 3) * 2 + ((input % 3) * 2 + 2) / 3;
}

bool completeWindow(uint64_t output, uint64_t segment_start)
{
    const uint64_t upsampled = output * 3;
    const uint64_t oldest_tap = Resampler::kFilterTaps - 1 - (upsampled & 1U);
    return upsampled >= oldest_tap && (upsampled - oldest_tap) / 2 >= segment_start;
}

// 有意采用较慢的规格模型：保存完整源片段，并为每个 FIR 抽头
// 按绝对源坐标查询。不共享生产实现的流式环、槽遍历或输入驱动循环。
class AbsoluteFirOracle {
public:
    AbsoluteFirOracle()
    {
        static_assert(Resampler::kFilterTaps == 191 && Resampler::kGroupDelayUpsampledSamples == 95);
        constexpr double cutoff_ratio = 7000.0 / 48000.0;
        double coefficient_sum = 0.0;
        for (size_t tap = 0; tap < coefficients_.size(); ++tap) {
            const double centered = static_cast<double>(tap) - 95.0;
            const double phase = 2.0 * cutoff_ratio * centered;
            const double sinc = std::abs(phase) < 1.0e-12 ? 1.0 :
                std::sin(kPi * phase) / (kPi * phase);
            const double normalized = centered / 95.0;
            const double window = 0.42 + 0.5 * std::cos(kPi * normalized) +
                                  0.08 * std::cos(2.0 * kPi * normalized);
            const double coefficient = 2.0 * cutoff_ratio * sinc * window;
            coefficients_[tap] = static_cast<float>(coefficient);
            coefficient_sum += coefficient;
        }
        // 滤波器设计及归一化与生产初始化一样保留双精度。
        // 仅保留的抽头和有序运行时乘加使用 float，
        // 不使用快速数学优化或融合乘加。
        const double scale = 2.0 / coefficient_sum;
        for (float &coefficient : coefficients_)
            coefficient = static_cast<float>(coefficient * scale);
    }

    void reset(uint64_t first_input)
    {
        first_input_ = first_input;
        next_output_ = outputLimit(first_input);
        source_.clear();
    }

    std::vector<int16_t> append(const int16_t *input, size_t count, Resampler::Result *result)
    {
        if (count) source_.insert(source_.end(), input, input + count);
        const uint64_t end = first_input_ + source_.size();
        const uint64_t limit = outputLimit(end);
        result->first_output_sample_16k = next_output_;
        result->first_content_sample_16k_thirds = static_cast<int64_t>(next_output_ * 3) - 95;
        result->output_samples = static_cast<size_t>(limit - next_output_);
        result->valid_from_output = result->output_samples;
        std::vector<int16_t> output;
        while (next_output_ < limit) {
            const uint64_t upsampled = next_output_ * 3;
            const size_t parity = static_cast<size_t>(upsampled % 2);
            float sum = 0.0f;
            bool full_window = true;
            for (size_t tap = parity; tap < coefficients_.size(); tap += 2) {
                if (upsampled < tap || (upsampled - tap) / 2 < first_input_) {
                    full_window = false;
                    continue;  // 重置原点之前的已知补零。
                }
                const uint64_t source_index = (upsampled - tap) / 2;
                assert(source_index < end);
                sum += coefficients_[tap] *
                    static_cast<float>(source_[static_cast<size_t>(source_index - first_input_)]);
            }
            if (full_window && result->valid_from_output == result->output_samples)
                result->valid_from_output = output.size();
            const float rounded = std::nearbyint(sum);
            output.push_back(static_cast<int16_t>(std::clamp(rounded, -32768.0f, 32767.0f)));
            ++next_output_;
        }
        return output;
    }

    // 与抽头符号匹配的输入会产生大于一的 FIR 增益，
    // 有意验证正负两方向的 int16 饱和分支。
    void forceClippingAt(std::vector<int16_t> *input, size_t output, bool negative) const
    {
        const size_t upsampled = output * 3;
        for (size_t tap = upsampled % 2; tap < coefficients_.size(); tap += 2) {
            assert(upsampled >= tap && (upsampled - tap) / 2 < input->size());
            const bool positive = (coefficients_[tap] >= 0.0f) != negative;
            (*input)[(upsampled - tap) / 2] = positive ? 32767 : -32768;
        }
    }

private:
    std::array<float, 191> coefficients_{};
    std::vector<int16_t> source_;
    uint64_t first_input_ = 0, next_output_ = 0;
};

void absoluteFirOracleRegression()
{
    AbsoluteFirOracle oracle;
    Resampler production;
    assert(production.initialize());
    std::mt19937 random(0x19102416);
    std::vector<int16_t> input(4096);
    for (int16_t &sample : input)
        sample = static_cast<int16_t>(static_cast<int32_t>(random() & 0xffffU) - 32768);
    oracle.forceClippingAt(&input, 256, false);
    oracle.forceClippingAt(&input, 1000, true);
    size_t compared_samples = 0;
    bool saw_positive_clip = false, saw_negative_clip = false;
    // 跨非零片段复用同一个生产对象。重置必须丢弃旧 FIR 历史，
    // 包括坐标倒退的情况。
    for (uint64_t first : {0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL,
                           (1ULL << 32), (1ULL << 32) + 1, (1ULL << 32) + 2,
                           (1ULL << 32) + 3, (1ULL << 32) + 4, (1ULL << 32) + 5, 0ULL}) {
        production.reset(first);
        oracle.reset(first);
        size_t offset = 0;
        while (offset < input.size()) {
            // 包含单样本块、生产用 240 样本 DMA 块，
            // 以及穿过两种多相状态的任意切分。
            const size_t requested = offset < 8 ? 1 :
                (offset % 3 == 0 ? 240 : 1 + random() % 701);
            const size_t count = std::min(requested, input.size() - offset);
            std::vector<int16_t> actual(count * 2 + 8, -111);
            Resampler::Result observed{}, expected{};
            const auto wanted = oracle.append(input.data() + offset, count, &expected);
            assert(production.process(input.data() + offset, count,
                                      actual.data(), actual.size(), &observed));
            assert(observed.output_samples == wanted.size());
            assert(observed.output_samples == expected.output_samples);
            assert(observed.valid_from_output == expected.valid_from_output);
            assert(observed.first_output_sample_16k == expected.first_output_sample_16k);
            assert(observed.first_content_sample_16k_thirds == expected.first_content_sample_16k_thirds);
            assert(std::equal(wanted.begin(), wanted.end(), actual.begin()));
            assert(std::all_of(actual.begin() + observed.output_samples, actual.end(),
                               [](int16_t sample) { return sample == -111; }));
            saw_positive_clip |= std::find(wanted.begin(), wanted.end(), 32767) != wanted.end();
            saw_negative_clip |= std::find(wanted.begin(), wanted.end(), -32768) != wanted.end();
            compared_samples += wanted.size();
            offset += count;
            assert(production.input_samples_seen() == first + offset);
            assert(production.next_output_sample_16k() == outputLimit(first + offset));
        }
        Resampler::Result observed{}, expected{};
        assert(oracle.append(nullptr, 0, &expected).empty());
        assert(production.process(nullptr, 0, nullptr, 0, &observed));
        assert(observed.output_samples == 0 && observed.valid_from_output == expected.valid_from_output);
        assert(observed.first_output_sample_16k == expected.first_output_sample_16k);
        assert(observed.first_content_sample_16k_thirds == expected.first_content_sample_16k_thirds);
    }
    assert(saw_positive_clip && saw_negative_clip);
    std::printf("Independent absolute-index FIR oracle: %zu exact PCM samples, 13 reset segments, "
                "all six start phases/64-bit origins, random chunks, warm-up/timestamps and both saturation directions passed\n",
                compared_samples);
}

std::vector<int16_t> run(const std::vector<int16_t> &input,
                         const std::vector<size_t> &chunks,
                         size_t *valid_total = nullptr,
                         uint64_t first_input = 0)
{
    Resampler resampler;
    assert(resampler.initialize());
    resampler.reset(first_input);
    assert(!chunks.empty());
    std::vector<int16_t> output;
    size_t offset = 0;
    size_t chunk_index = 0;
    size_t valid_samples = 0;
    while (offset < input.size()) {
        const size_t count = std::min(chunks[chunk_index++ % chunks.size()], input.size() - offset);
        std::vector<int16_t> block(count * 2 + 8);
        voiceaudio::ReferenceResampler::Result result{};
        const uint64_t first_output = outputLimit(first_input + offset);
        assert(resampler.process(input.data() + offset, count, block.data(), block.size(), &result));
        assert(result.first_output_sample_16k == first_output);
        assert(result.first_content_sample_16k_thirds ==
               static_cast<int64_t>(first_output * 3) - Resampler::kGroupDelayUpsampledSamples);
        assert(result.output_samples == outputLimit(first_input + offset + count) - first_output);
        assert(result.valid_from_output <= result.output_samples);
        size_t expected_valid = 0;
        while (expected_valid < result.output_samples &&
               !completeWindow(first_output + expected_valid, first_input)) ++expected_valid;
        assert(result.valid_from_output == expected_valid);
        valid_samples += result.output_samples - result.valid_from_output;
        output.insert(output.end(), block.begin(), block.begin() + result.output_samples);
        offset += count;
    }
    if (valid_total) *valid_total = valid_samples;
    return output;
}

double rms(const std::vector<int16_t> &samples, size_t begin)
{
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = begin; i < samples.size(); ++i) {
        const double value = samples[i];
        sum += value * value;
        ++count;
    }
    return count ? std::sqrt(sum / count) : 0.0;
}

std::vector<int16_t> sine(double frequency, size_t count)
{
    std::vector<int16_t> result(count);
    for (size_t i = 0; i < count; ++i)
        result[i] = static_cast<int16_t>(std::sin(2.0 * kPi * frequency * i / 24000.0) * 10000.0);
    return result;
}

uint64_t digest(const std::vector<int16_t> &samples, uint64_t hash)
{
    for (int16_t sample : samples) {
        const auto value = static_cast<uint16_t>(sample);
        hash = (hash ^ (value & 0xffU)) * 1099511628211ULL;
        hash = (hash ^ (value >> 8)) * 1099511628211ULL;
    }
    return hash;
}

void invalidCallsPreserveState()
{
    Resampler tested, baseline;
    std::array<int16_t, 480> input{};
    input.fill(2000);
    std::array<int16_t, 400> destination{}, expected{};
    Resampler::Result result{};
    assert(!tested.process(input.data(), input.size(), destination.data(), destination.size(), &result));
    assert(tested.initialize() && baseline.initialize());
    result.output_samples = 999;
    destination.fill(-123);
    assert(!tested.process(nullptr, 1, destination.data(), destination.size(), &result));
    assert(!tested.process(input.data(), input.size(), nullptr, destination.size(), &result));
    assert(!tested.process(input.data(), input.size(), destination.data(), destination.size(), nullptr));
    assert(!tested.process(input.data(), input.size(), destination.data(), 1, &result));
    assert(!tested.process(input.data(), input.size(), input.data() + 1, input.size() - 1, &result));
    assert(!tested.process(input.data(), std::numeric_limits<size_t>::max(),
                           destination.data(), destination.size(), &result));
    assert(tested.input_samples_seen() == 0 && tested.next_output_sample_16k() == 0);
    assert(result.output_samples == 999);
    assert(std::all_of(destination.begin(), destination.end(), [](int16_t value) { return value == -123; }));
    assert(std::all_of(input.begin(), input.end(), [](int16_t value) { return value == 2000; }));
    Resampler::Result expected_result{};
    assert(tested.process(input.data(), input.size(), destination.data(), destination.size(), &result));
    assert(baseline.process(input.data(), input.size(), expected.data(), expected.size(), &expected_result));
    assert(result.output_samples == expected_result.output_samples &&
           std::equal(destination.begin(), destination.begin() + result.output_samples, expected.begin()));
    assert(tested.process(nullptr, 0, nullptr, 0, &result));
    assert(result.output_samples == 0 && tested.input_samples_seen() == input.size());

    // 极大重置坐标不会回绕。被拒绝的 process 调用
    // 连该越界边界也不改变，且仍可再次重置。
    tested.reset(std::numeric_limits<uint64_t>::max());
    assert(tested.next_output_sample_16k() == outputLimit(std::numeric_limits<uint64_t>::max()));
    const auto before = tested.input_samples_seen();
    assert(!tested.process(input.data(), 1, destination.data(), destination.size(), &result));
    assert(!tested.process(nullptr, 0, nullptr, 0, &result));
    assert(tested.input_samples_seen() == before);
    // 跨有符号时间戳范围的批次也会在消费首样本前被拒绝，
    // 即使该首样本本身可表示。
    const uint64_t last_output = static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 3);
    tested.reset((last_output * 3) / 2);
    const auto boundary_input = tested.input_samples_seen();
    assert(!tested.process(input.data(), input.size(), destination.data(), destination.size(), &result));
    assert(tested.input_samples_seen() == boundary_input);
    tested.reset();
    assert(tested.process(input.data(), input.size(), destination.data(), destination.size(), &result));
}

void longRunningCount()
{
    Resampler resampler;
    assert(resampler.initialize());
    std::array<int16_t, 480> input{};
    std::array<int16_t, 320> output{};
    for (size_t i = 0; i < 6000; ++i) {  // 24 kHz 下的 120 秒。
        Resampler::Result result{};
        assert(resampler.process(input.data(), input.size(), output.data(), output.size(), &result));
        assert(result.output_samples == 320 && result.first_output_sample_16k == i * 320);
    }
    assert(resampler.input_samples_seen() == 2880000 && resampler.next_output_sample_16k() == 1920000);
}

void explicitBoundaryAndNaturalTail()
{
    std::vector<int16_t> impulse(481, 0);
    impulse.back() = 20000;
    // 调用方持续送入已知零参考，自然排空整个 191 抽头尾部，
    // 不需要隐藏的刷新操作或相位重置。
    impulse.resize(481 + Resampler::kInputHistorySamples + 480, 0);
    const auto tail = run(impulse, {1, 479, 1, 1, 17, 240});
    assert(tail == run(impulse, {impulse.size()}));
    assert(std::any_of(tail.begin() + 320, tail.begin() + 400,
                       [](int16_t value) { return value != 0; }));
    assert(std::all_of(tail.end() - 320, tail.end(), [](int16_t value) { return value == 0; }));
    for (uint64_t start = 0; start < 6; ++start) {
        size_t valid = 0;
        const auto output = run(std::vector<int16_t>(480, 0), {1, 2, 3, 239, 17}, &valid, start);
        assert(valid > 0 && valid < output.size());
        assert(std::all_of(output.begin(), output.end(), [](int16_t value) { return value == 0; }));
    }
    Resampler resampler;
    assert(resampler.initialize());
    std::array<int16_t, 480> input{};
    input.back() = 20000;
    std::array<int16_t, 400> output{};
    Resampler::Result result{};
    assert(resampler.process(input.data(), input.size(), output.data(), output.size(), &result));
    resampler.reset(123);
    input.fill(0);
    assert(resampler.process(input.data(), input.size(), output.data(), output.size(), &result));
    assert(result.first_output_sample_16k == 82 && result.first_content_sample_16k_thirds == 151);
    assert(std::all_of(output.begin(), output.begin() + result.output_samples,
                       [](int16_t value) { return value == 0; }));
}
}

int main()
{
    constexpr size_t kInputCount = 48000;
    std::vector<int16_t> signal(kInputCount);
    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = static_cast<int16_t>((i * 7919U + i / 13U * 101U) & 0xffffU);
    const auto one_chunk = run(signal, {signal.size()});
    const auto irregular = run(signal, {1, 239, 17, 511, 320, 7, 1024, 93});
    assert(one_chunk == irregular);
    std::mt19937 generator(0x24a16);
    std::vector<size_t> random_chunks(200);
    for (size_t &chunk : random_chunks) chunk = 1 + generator() % 1200;
    assert(one_chunk == run(signal, random_chunks));
    assert(one_chunk.size() == 32000);  // 精确的 48000 * 2/3 计数。

    std::vector<int16_t> dc(48000, 12000);
    const auto dc_out = run(dc, {240, 480, 719});
    double dc_mean = 0.0;
    for (size_t i = 80; i < dc_out.size() - 80; ++i) dc_mean += dc_out[i];
    dc_mean /= static_cast<double>(dc_out.size() - 160);
    assert(std::abs(dc_mean - 12000.0) < 2.0);

    const auto passband = run(sine(3000.0, 24000), {480});
    const auto stopband = run(sine(10000.0, 24000), {320, 160, 960});
    const double pass_rms = rms(passband, 100);
    const double stop_rms = rms(stopband, 100);
    assert(pass_rms > 6500.0);
    assert(stop_rms < pass_rms * 0.03);  // 混叠前抑制超过 30 dB。
    for (const double frequency : {1000.0, 6000.0})
        assert(rms(run(sine(frequency, 24000), random_chunks), 100) > 6500.0);
    for (const double frequency : {8100.0, 9000.0, 11000.0})
        assert(rms(run(sine(frequency, 24000), random_chunks), 100) < pass_rms * 0.01);

    std::vector<int16_t> impulse(4800, 0);
    impulse[1200] = 20000;
    const auto impulse_out = run(impulse, {240});
    const auto peak = std::max_element(impulse_out.begin(), impulse_out.end(),
        [](int16_t left, int16_t right) { return std::abs(static_cast<int>(left)) < std::abs(static_cast<int>(right)); });
    const size_t peak_index = static_cast<size_t>(peak - impulse_out.begin());
    assert(std::abs(static_cast<int64_t>(peak_index) - 832) <= 1);

    voiceaudio::ReferenceResampler segmented;
    assert(segmented.initialize());
    segmented.reset(240);
    std::array<int16_t, 480> zeros{};
    std::array<int16_t, 400> converted{};
    voiceaudio::ReferenceResampler::Result segment_result{};
    assert(segmented.process(zeros.data(), zeros.size(), converted.data(), converted.size(), &segment_result));
    assert(segment_result.first_output_sample_16k == 160);
    assert(segment_result.first_content_sample_16k_thirds == 385);
    assert(segment_result.valid_from_output > 0 && segment_result.valid_from_output < segment_result.output_samples);
    const uint64_t before_failed_call = segmented.input_samples_seen();
    voiceaudio::ReferenceResampler::Result failed_result{};
    assert(!segmented.process(zeros.data(), zeros.size(), converted.data(), 1, &failed_result));
    assert(segmented.input_samples_seen() == before_failed_call);

    longRunningCount();
    explicitBoundaryAndNaturalTail();
    invalidCallsPreserveState();
    absoluteFirOracleRegression();

    uint64_t hash = 14695981039346656037ULL;
    for (const auto *samples : {&one_chunk, &dc_out, &passband, &stopband, &impulse_out})
        hash = digest(*samples, hash);
    std::printf("Resampler PCM regression digest: %016llx\n", static_cast<unsigned long long>(hash));
    std::puts("Passed reference resampler: 11 scenario groups, independent absolute-index FIR oracle/exact PCM, random chunk invariance, 120-second exact 2/3 count, unity DC, passband/alias rejection, impulse/group-delay timestamp, all-phase warm-up, natural zero tail, explicit reset, rejected-call state preservation/overflow; no hardware used");
}
