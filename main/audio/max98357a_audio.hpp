#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "../app/board_pins.hpp"
#include "playback_reference.hpp"
#include "playback_progress_clock.hpp"
#include "freertos/FreeRTOS.h"

// MAX98357A 扬声器驱动：单声道 PCM 复制为双声道 I2S，软件音量在写入前应用。
// SD 引脚负责硬件静音；resetPlayback() 先停声再重建 DMA，防止下一轮重播残留。
// 默认接线统一放在 app/board_pins.hpp，日常音量使用 menuconfig 配置。
class Max98357aAudio {
public:
    struct Config {
        int i2s_port = I2S_NUM_0;
        uint32_t sample_rate_hz = 16000;
        gpio_num_t dout_gpio = voice::board::kSpeakerData;
        gpio_num_t bclk_gpio = voice::board::kSpeakerBclk;
        gpio_num_t ws_gpio = voice::board::kSpeakerWs;
        gpio_num_t sd_mode_gpio = voice::board::kSpeakerEnable;
        uint8_t volume_percent = 40;
        bool playback_reference_enabled = false;  // P4.3 观测仅用于 24 kHz。
    };

    Max98357aAudio() = default;
    Max98357aAudio(const Max98357aAudio &) = delete;
    Max98357aAudio &operator=(const Max98357aAudio &) = delete;

    esp_err_t init();
    esp_err_t init(const Config &config);
    // 丢弃整个 DMA 环，同时保留关闭 GPIO 的所有权。
    // 功放保持禁用，直到下一回答调用 setEnabled(true)。
    esp_err_t resetPlayback();
    esp_err_t deinit();

    esp_err_t setEnabled(bool enabled);
    esp_err_t setVolume(uint8_t percent);
    esp_err_t writeMono(const int16_t *samples, size_t sample_count, uint32_t timeout_ms = 1000);
    // 入队超过 TX DMA 总容量的静音。返回表示本调用前
    // 排队的所有音频都已物理离开 I2S DMA 环。
    esp_err_t drain(uint32_t timeout_ms = 1000,
                    void (*progress)(void *) = nullptr, void *context = nullptr);
    PlaybackReference::Metrics playbackReferenceMetrics() const { return reference_.metrics(); }
    PlaybackReference::Tag playbackReferenceTag() const { return reference_.tag(); }
    void pollPlaybackReference() { reference_.pump(); }
    bool bindPlaybackSerial(uint32_t serial);
    PlaybackProgressClock::Snapshot playbackProgress() const { return progress_.snapshot(); }
    // 在标准 SDK DMA memcpy 前后调用的内部观测器。
    void observeWrite(const void *buffer, size_t dma_offset, size_t source_offset,
                      size_t bytes, bool completed);
    void logPlaybackReference();
    static void logPlaybackReferenceSnapshot(const PlaybackReference::Metrics &metrics);
    size_t copyPlaybackReferenceHistory(PlaybackReference::Block *destination, size_t capacity,
                                        uint32_t generation = 0) const
    { return reference_.copyHistory(destination, capacity, generation); }
    bool copyNextPlaybackReference(uint32_t cursor, PlaybackReference::Block *destination,
                                   bool *overwritten = nullptr) const
    { return reference_.copyHistoryAfter(cursor, destination, overwritten); }

    bool isReady() const { return ready_; }
    uint32_t sampleRateHz() const { return config_.sample_rate_hz; }

private:
    static constexpr size_t kChunkFrames = 240;
    static constexpr size_t kDmaDescriptors = 4;

    static bool isSupportedSampleRate(uint32_t sample_rate_hz);
    esp_err_t startChannel(bool enabled);
    esp_err_t stopChannel();
    static bool IRAM_ATTR onDmaSent(i2s_chan_handle_t channel, i2s_event_data_t *event,
                                    void *user_data);
    void logReferenceMetrics() const;

    Config config_{};
    i2s_chan_handle_t tx_channel_ = nullptr;
    int16_t *stereo_buffer_ = nullptr;
    bool channel_enabled_ = false;
    bool enabled_ = false;
    bool ready_ = false;
    bool configured_ = false;
    bool sd_configured_ = false;
    std::atomic<uint32_t> amplifier_enabled_{0};
    PlaybackReference reference_;
    PlaybackProgressClock progress_;
    portMUX_TYPE progress_mux_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t progress_serial_ = 0;
    uint64_t progress_submitted_ = 0;
    uint64_t progress_write_first_ = 0;
    bool progress_content_write_ = false;
    bool progress_write_active_ = false;
    bool draining_ = false;
};
