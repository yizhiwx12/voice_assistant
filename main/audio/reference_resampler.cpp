#include "reference_resampler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace voiceaudio {
namespace {
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kUpsampledRateHz = 2.0 * ReferenceResampler::kInputRateHz;
constexpr double kCutoffHz = 7000.0;

// 计算 ceil(2 * input / 3)，即使 input 为 UINT64_MAX 也不溢出。
uint64_t outputLimit(uint64_t input)
{
    return (input / 3) * 2 + ((input % 3) * 2 + 2) / 3;
}

bool buffersOverlap(const int16_t *input, size_t input_samples,
                    const int16_t *output, size_t output_samples)
{
    if (!input_samples || !output_samples) return false;
    const auto input_address = reinterpret_cast<uintptr_t>(input);
    const auto output_address = reinterpret_cast<uintptr_t>(output);
    const size_t input_bytes = input_samples * sizeof(int16_t);
    const size_t output_bytes = output_samples * sizeof(int16_t);
    if (input_address <= output_address)
        return output_address - input_address < input_bytes;
    return input_address - output_address < output_bytes;
}

double sinc(double value)
{
    if (std::abs(value) < 1.0e-12) return 1.0;
    const double angle = kPi * value;
    return std::sin(angle) / angle;
}
}  // 命名空间

bool ReferenceResampler::initialize()
{
    constexpr double center = static_cast<double>(kGroupDelayUpsampledSamples);
    double coefficient_sum = 0.0;
    for (size_t tap = 0; tap < kFilterTaps; ++tap) {
        const double position = static_cast<double>(tap) - center;
        const double normalized_position = position / center;
        const double window = 0.42 + 0.5 * std::cos(kPi * normalized_position) +
                              0.08 * std::cos(2.0 * kPi * normalized_position);
        const double coefficient = 2.0 * (kCutoffHz / kUpsampledRateHz) *
            sinc(2.0 * (kCutoffHz / kUpsampledRateHz) * position) * window;
        coefficients_[tap] = static_cast<float>(coefficient);
        coefficient_sum += coefficient;
    }
    if (!std::isfinite(coefficient_sum) || std::abs(coefficient_sum) < 1.0e-12) {
        ready_ = false;
        return false;
    }
    // 两倍上采样在每个输入样本后插入一个零，因此原型滤波器
    // 采用二倍增益，使插值后的直流增益保持为一。
    const double scale = 2.0 / coefficient_sum;
    for (float &coefficient : coefficients_)
        coefficient = static_cast<float>(coefficient * scale);
    ready_ = true;
    reset();
    return true;
}

void ReferenceResampler::reset(uint64_t first_input_sample_24k)
{
    history_.fill(0);
    history_position_ = history_valid_samples_ = 0;
    input_index_ = first_input_sample_24k;
    // 跨缺口及 DMA 代次保持全局 16 kHz 输出相位。
    next_output_sample_16k_ = outputLimit(first_input_sample_24k);
}

bool ReferenceResampler::process(const int16_t *input, size_t input_samples,
                                 int16_t *output, size_t output_capacity, Result *result)
{
    if (!ready_ || (!input && input_samples) || !result ||
        input_samples > std::numeric_limits<size_t>::max() / sizeof(int16_t) ||
        input_samples > std::numeric_limits<uint64_t>::max() - input_index_)
        return false;

    const uint64_t end_index = input_index_ + input_samples;
    const uint64_t output_limit = outputLimit(end_index);
    if (output_limit < next_output_sample_16k_ ||
        output_limit - next_output_sample_16k_ > output_capacity) return false;

    const size_t total_output_samples =
        static_cast<size_t>(output_limit - next_output_sample_16k_);
    constexpr uint64_t kLastTimestampedOutput =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 3);
    // 消费任何输入前检查整个调用，不只检查第一个时间戳。
    // 这样下面每个上采样索引的乘法也都安全。
    if (next_output_sample_16k_ > kLastTimestampedOutput ||
        (total_output_samples && output_limit - 1 > kLastTimestampedOutput) ||
        total_output_samples > std::numeric_limits<size_t>::max() / sizeof(int16_t) ||
        (!output && total_output_samples) ||
        buffersOverlap(input, input_samples, output, total_output_samples)) return false;

    Result staged{};
    staged.first_output_sample_16k = next_output_sample_16k_;
    staged.first_content_sample_16k_thirds =
        static_cast<int64_t>(next_output_sample_16k_ * 3) -
        static_cast<int64_t>(kGroupDelayUpsampledSamples);
    staged.valid_from_output = total_output_samples;

    size_t output_offset = 0;
    for (size_t i = 0; i < input_samples; ++i) {
        const uint64_t current_index = input_index_++;
        const size_t current_slot = history_position_;
        history_[current_slot] = input[i];
        if (++history_position_ == kInputHistorySamples) history_position_ = 0;
        if (history_valid_samples_ < kInputHistorySamples) ++history_valid_samples_;
        while (next_output_sample_16k_ < output_limit &&
               (next_output_sample_16k_ * 3) / 2 <= current_index) {
            const uint64_t upsampled_index = next_output_sample_16k_ * 3;
            const size_t parity = static_cast<size_t>(upsampled_index & 1U);
            const size_t phase_taps = (kFilterTaps + 1 - parity) / 2;
            const size_t available_taps = std::min(history_valid_samples_, phase_taps);
            // 在此 2/3 比率下，每个输出恰在读入其最新源样本时产生。
            // 连续的同相抽头每次向后走一个环槽。
            // 让这个每秒约 150 万次乘加的内循环避开 64 位坐标及取模。
            size_t history_slot = current_slot;
            float sum = 0.0f;
            const size_t end_tap = parity + 2 * available_taps;
            for (size_t tap = parity; tap < end_tap; tap += 2) {
                sum += coefficients_[tap] *
                       static_cast<float>(history_[history_slot]);
                if (history_slot == 0) history_slot = kInputHistorySamples - 1;
                else --history_slot;
            }
            const float rounded = std::nearbyint(sum);
            if (rounded > static_cast<float>(std::numeric_limits<int16_t>::max()))
                output[output_offset] = std::numeric_limits<int16_t>::max();
            else if (rounded < static_cast<float>(std::numeric_limits<int16_t>::min()))
                output[output_offset] = std::numeric_limits<int16_t>::min();
            else
                output[output_offset] = static_cast<int16_t>(rounded);

            if (history_valid_samples_ >= phase_taps &&
                staged.valid_from_output == total_output_samples) {
                staged.valid_from_output = output_offset;
            }
            ++output_offset;
            ++next_output_sample_16k_;
        }
    }
    staged.output_samples = output_offset;
    *result = staged;
    return output_offset == total_output_samples;
}

}  // 命名空间 voiceaudio
