#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"

namespace voiceaudio {

// 单声道、20 ms 的原始 Opus 数据包；包装对象持有一个流的状态。
class OpusEncoder {
public:
    using PacketCallback = std::function<esp_err_t(const uint8_t *, size_t)>;
    ~OpusEncoder() { close(); }
    OpusEncoder() = default;
    OpusEncoder(const OpusEncoder &) = delete;
    OpusEncoder &operator=(const OpusEncoder &) = delete;

    esp_err_t open(uint32_t rate = 16000, uint32_t bitrate = 24000);
    esp_err_t feed(const int16_t *samples, size_t count, const PacketCallback &send);
    esp_err_t finish(const PacketCallback &send);
    void close();
    uint32_t preSkipSamples() const { return pre_skip_; }
    uint64_t inputSamples() const { return input_samples_; }

private:
    esp_err_t encode(const PacketCallback &send);
    void *handle_ = nullptr;
    uint32_t frame_samples_ = 0;
    uint32_t pre_skip_ = 0;
    size_t pending_ = 0;
    uint64_t input_samples_ = 0;
    uint64_t encoded_samples_ = 0;
    bool finished_ = false;
    std::array<int16_t, 480> frame_{};
    std::array<uint8_t, 1275> packet_{};
};

class OpusDecoder {
public:
    ~OpusDecoder() { close(); }
    OpusDecoder() = default;
    OpusDecoder(const OpusDecoder &) = delete;
    OpusDecoder &operator=(const OpusDecoder &) = delete;

    esp_err_t open(uint32_t rate = 24000);
    esp_err_t decode(const uint8_t *packet, size_t bytes, int16_t *samples,
                     size_t capacity, size_t *samples_written);
    void close();

private:
    void *handle_ = nullptr;
    uint32_t rate_ = 0;
};

}  // 命名空间 voiceaudio
