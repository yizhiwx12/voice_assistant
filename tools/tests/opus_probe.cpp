// 围绕实际固件包装器的轻量主机 C ABI，绝非模拟编解码器。
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "opus_codec.hpp"

#ifdef _WIN32
#define PROBE_EXPORT extern "C" __declspec(dllexport)
#else
#define PROBE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

PROBE_EXPORT int probe_encode(uint32_t rate, uint32_t bitrate, const int16_t *pcm,
                              size_t samples, uint8_t *output, size_t capacity,
                              size_t *written, uint32_t *pre_skip)
{
    if (!pcm || !samples || !output || !written || !pre_skip) return ESP_ERR_INVALID_ARG;
    *written = 0;
    voiceaudio::OpusEncoder encoder;
    int error = encoder.open(rate, bitrate);
    if (error != ESP_OK) return error;
    *pre_skip = encoder.preSkipSamples();
    auto send = [&](const uint8_t *packet, size_t bytes) -> esp_err_t {
        if (*written + 2 + bytes > capacity) return ESP_ERR_INVALID_SIZE;
        output[(*written)++] = bytes & 255;
        output[(*written)++] = bytes >> 8;
        std::memcpy(output + *written, packet, bytes);
        *written += bytes;
        return ESP_OK;
    };
    // 输入有意跨帧边界，验证真实缓冲。
    for (size_t offset = 0; offset < samples;) {
        const size_t count = std::min<size_t>(137, samples - offset);
        error = encoder.feed(pcm + offset, count, send);
        if (error != ESP_OK) return error;
        offset += count;
    }
    if (encoder.inputSamples() != samples) return ESP_FAIL;
    return encoder.finish(send);
}

PROBE_EXPORT void *probe_decoder_create(uint32_t rate)
{
    auto *decoder = new (std::nothrow) voiceaudio::OpusDecoder;
    if (!decoder) return nullptr;
    if (decoder->open(rate) != ESP_OK) {
        delete decoder;
        return nullptr;
    }
    return decoder;
}

PROBE_EXPORT int probe_decode(void *handle, const uint8_t *packet, size_t bytes,
                              int16_t *pcm, size_t capacity, size_t *samples)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return static_cast<voiceaudio::OpusDecoder *>(handle)->decode(packet, bytes, pcm, capacity, samples);
}

PROBE_EXPORT void probe_decoder_destroy(void *handle)
{
    delete static_cast<voiceaudio::OpusDecoder *>(handle);
}
