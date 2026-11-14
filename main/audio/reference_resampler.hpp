#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace voiceaudio {

// 有状态的 24 kHz -> 16 kHz 抗混叠重采样器，使用 2/3 多相 FIR。
// 跨 process() 调用保留相位/历史。输出样本索引遵循有理时间线；
// 内容坐标扣除固定 95/48000 秒（约 1.979 ms）的滤波器群延迟。
class ReferenceResampler {
public:
    static constexpr size_t kFilterTaps = 191;  // 在 48 kHz 上采样率下计算。
    static constexpr size_t kInputHistorySamples = kFilterTaps / 2 + 1;
    static constexpr uint32_t kGroupDelayUpsampledSamples = (kFilterTaps - 1) / 2;
    static constexpr uint32_t kInputRateHz = 24000;
    static constexpr uint32_t kOutputRateHz = 16000;

    struct Result {
        size_t output_samples = 0;
        // 此偏移之前的输出包含重置后的补零 FIR 历史。
        // 剩余输出具有完整的真实输入滤波窗口。
        size_t valid_from_output = 0;
        uint64_t first_output_sample_16k = 0;
        // 源 16 kHz 样本的三分之一单位坐标。输出样本 m 对应
        // 3*m - kGroupDelayUpsampledSamples；FIR 预热期间为负。
        int64_t first_content_sample_16k_thirds = 0;
    };

    ReferenceResampler() = default;
    bool initialize();
    // 开始一个明确不连续的新片段。保留全局有理相位，
    // 丢弃旧 FIR 历史，并在 Result 中报告补零预热区间。
    // 调用方在重置前提供真实零样本以排空片段尾部；
    // reset 本身绝不输出或重播旧样本。
    void reset(uint64_t first_input_sample_24k = 0);
    // 输入/输出不能重叠。所有参数、容量、时间戳检查均在
    // 修改状态、目标或结果前完成。允许使用空指针执行空调用。
    // 超出 Result 的 int64_t 时间戳范围的坐标会被拒绝，
    // 包括越界重置后产生的坐标。
    bool process(const int16_t *input, size_t input_samples,
                 int16_t *output, size_t output_capacity, Result *result);

    bool ready() const { return ready_; }
    uint64_t input_samples_seen() const { return input_index_; }
    uint64_t next_output_sample_16k() const { return next_output_sample_16k_; }

private:
    std::array<float, kFilterTaps> coefficients_{};
    std::array<int16_t, kInputHistorySamples> history_{};
    size_t history_position_ = 0;
    size_t history_valid_samples_ = 0;
    uint64_t input_index_ = 0;
    uint64_t next_output_sample_16k_ = 0;
    bool ready_ = false;
};

}  // 命名空间 voiceaudio
