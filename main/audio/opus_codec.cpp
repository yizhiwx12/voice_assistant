#include "opus_codec.hpp"

#include <algorithm>
#include <cstring>

#include "opus.h"

namespace voiceaudio {

esp_err_t OpusEncoder::open(uint32_t rate, uint32_t bitrate)
{
    close();
    if ((rate != 16000 && rate != 24000) || bitrate < 6000 || bitrate > 128000)
        return ESP_ERR_INVALID_ARG;
    int error = OPUS_OK;
    auto *encoder = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error != OPUS_OK) return ESP_ERR_NO_MEM;
    handle_ = encoder;
    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(static_cast<int>(bitrate))) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_DTX(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(3)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK ||
        lookahead < 0 || lookahead > static_cast<int>(rate / 50)) {
        close();
        return ESP_FAIL;
    }
    frame_samples_ = rate / 50;
    pre_skip_ = lookahead;
    return ESP_OK;
}

esp_err_t OpusEncoder::encode(const PacketCallback &send)
{
    const int bytes = opus_encode(static_cast<::OpusEncoder *>(handle_), frame_.data(),
                                   frame_samples_, packet_.data(), packet_.size());
    if (bytes <= 0) return ESP_FAIL;
    encoded_samples_ += frame_samples_;
    pending_ = 0;
    return send(packet_.data(), static_cast<size_t>(bytes));
}

esp_err_t OpusEncoder::feed(const int16_t *samples, size_t count, const PacketCallback &send)
{
    if (!handle_ || finished_) return ESP_ERR_INVALID_STATE;
    if ((!samples && count) || !send) return ESP_ERR_INVALID_ARG;
    input_samples_ += count;
    while (count) {
        const size_t take = std::min(count, static_cast<size_t>(frame_samples_) - pending_);
        std::copy_n(samples, take, frame_.data() + pending_);
        pending_ += take;
        samples += take;
        count -= take;
        if (pending_ == frame_samples_) {
            const esp_err_t err = encode(send);
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}

esp_err_t OpusEncoder::finish(const PacketCallback &send)
{
    if (!handle_ || finished_) return ESP_ERR_INVALID_STATE;
    if (!send) return ESP_ERR_INVALID_ARG;
    finished_ = true;
    // 始终把编码器的前瞻延迟经接收方传递，包括输入恰为 20 ms 整数倍的情况。
    // valid_end_sample 会裁去这些补齐样本。
    while (encoded_samples_ < input_samples_ + pre_skip_) {
        std::fill(frame_.begin() + pending_, frame_.begin() + frame_samples_, 0);
        const esp_err_t err = encode(send);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

void OpusEncoder::close()
{
    if (handle_) opus_encoder_destroy(static_cast<::OpusEncoder *>(handle_));
    handle_ = nullptr;
    frame_samples_ = pre_skip_ = 0;
    pending_ = 0;
    input_samples_ = encoded_samples_ = 0;
    finished_ = false;
}

esp_err_t OpusDecoder::open(uint32_t rate)
{
    close();
    if (rate != 16000 && rate != 24000) return ESP_ERR_INVALID_ARG;
    int error = OPUS_OK;
    handle_ = opus_decoder_create(rate, 1, &error);
    if (!handle_ || error != OPUS_OK) {
        close();
        return ESP_ERR_NO_MEM;
    }
    rate_ = rate;
    return ESP_OK;
}

esp_err_t OpusDecoder::decode(const uint8_t *packet, size_t bytes, int16_t *samples,
                              size_t capacity, size_t *samples_written)
{
    if (!handle_) return ESP_ERR_INVALID_STATE;
    if (!packet || !bytes || bytes > 1275 || !samples || !samples_written ||
        capacity < rate_ / 50) return ESP_ERR_INVALID_ARG;
    *samples_written = 0;
    if (opus_packet_get_nb_channels(packet) != 1 ||
        opus_packet_get_nb_samples(packet, static_cast<opus_int32>(bytes), rate_) !=
            static_cast<int>(rate_ / 50)) return ESP_ERR_INVALID_SIZE;
    const int count = opus_decode(static_cast<::OpusDecoder *>(handle_), packet,
                                  static_cast<opus_int32>(bytes), samples, rate_ / 50, 0);
    if (count != static_cast<int>(rate_ / 50)) return ESP_FAIL;
    *samples_written = count;
    return ESP_OK;
}

void OpusDecoder::close()
{
    if (handle_) opus_decoder_destroy(static_cast<::OpusDecoder *>(handle_));
    handle_ = nullptr;
    rate_ = 0;
}

}  // 命名空间 voiceaudio
