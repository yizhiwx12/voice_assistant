#include "max98357a_audio.hpp"
#include "i2s_write_observer.h"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "max98357a";
std::atomic<Max98357aAudio *> observed_speaker{nullptr};
std::atomic<i2s_chan_handle_t> observed_channel{nullptr};
}

extern "C" void voice_i2s_write_observer(void *channel, const void *buffer,
                                         size_t dma_offset, size_t source_offset,
                                         size_t bytes, int completed)
{
    if (observed_channel.load(std::memory_order_acquire) != channel) return;
    auto *speaker = observed_speaker.load(std::memory_order_acquire);
    if (speaker) speaker->observeWrite(buffer, dma_offset, source_offset, bytes, completed != 0);
}

void Max98357aAudio::observeWrite(const void *buffer, size_t dma_offset,
                                size_t source_offset, size_t bytes, bool completed)
{
    portENTER_CRITICAL(&progress_mux_);
    if (progress_write_active_) {
        if ((dma_offset | source_offset | bytes) % (2 * sizeof(int16_t))) progress_.invalidate();
        else if (!completed) progress_.beforeWrite(buffer, progress_serial_,
            progress_write_first_ + (source_offset + bytes) / (2 * sizeof(int16_t)), progress_content_write_);
        else progress_.afterWrite(buffer);
    }
    portEXIT_CRITICAL(&progress_mux_);
}

bool Max98357aAudio::bindPlaybackSerial(uint32_t serial)
{
    if (!reference_.bindPlaybackSerial(serial)) return false;
    portENTER_CRITICAL(&progress_mux_);
    if (progress_serial_ != serial) {
        progress_serial_ = serial;
        progress_submitted_ = 0;
        if (reference_.enabled()) progress_.bind(serial);
        else progress_.reset();
    }
    portEXIT_CRITICAL(&progress_mux_);
    return true;
}

bool Max98357aAudio::isSupportedSampleRate(uint32_t sample_rate_hz)
{
    switch (sample_rate_hz) {
    case 16000:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
    case 88200:
    case 96000:
        return true;
    default:
        return false;
    }
}

esp_err_t Max98357aAudio::init()
{
    return init(Config{});
}

esp_err_t Max98357aAudio::init(const Config &config)
{
    if (configured_ || ready_ || !isSupportedSampleRate(config.sample_rate_hz) || config.volume_percent > 100 ||
        (config.i2s_port != I2S_NUM_0 && config.i2s_port != I2S_NUM_1) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.dout_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.bclk_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.ws_gpio) ||
        config.dout_gpio == config.bclk_gpio || config.dout_gpio == config.ws_gpio ||
        config.bclk_gpio == config.ws_gpio ||
        (config.sd_mode_gpio != GPIO_NUM_NC &&
         (!GPIO_IS_VALID_OUTPUT_GPIO(config.sd_mode_gpio) ||
          config.sd_mode_gpio == config.dout_gpio || config.sd_mode_gpio == config.bclk_gpio ||
          config.sd_mode_gpio == config.ws_gpio))) {
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;
    if (config_.sd_mode_gpio != GPIO_NUM_NC) {
        // IDF 6.1 的 gpio_config() 虽会警告，仍会抢占已保留的输出。
        // 拒绝其他持有者的引脚，并在输出启用前预置 LOW，
        // 防止先前输出锁存值短暂打开功放。
        if (esp_gpio_is_reserved(1ULL << config_.sd_mode_gpio)) return ESP_ERR_INVALID_STATE;
        esp_err_t err = gpio_set_level(config_.sd_mode_gpio, 0);
        if (err != ESP_OK) return err;
        gpio_config_t sd_config{};
        sd_config.pin_bit_mask = 1ULL << config_.sd_mode_gpio;
        sd_config.mode = GPIO_MODE_OUTPUT;
        sd_config.pull_up_en = GPIO_PULLUP_DISABLE;
        sd_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        sd_config.intr_type = GPIO_INTR_DISABLE;
        err = gpio_config(&sd_config);
        if (err != ESP_OK) return err;
        sd_configured_ = true;
    }
    configured_ = true;
    const esp_err_t err = startChannel(true);
    if (err != ESP_OK) deinit();
    return err;
}

esp_err_t Max98357aAudio::startChannel(bool enabled)
{
    stereo_buffer_ = static_cast<int16_t *>(heap_caps_malloc(
        kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (stereo_buffer_ == nullptr) return ESP_ERR_NO_MEM;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(config_.i2s_port, I2S_ROLE_MASTER);
    channel_config.dma_frame_num = kChunkFrames;
    channel_config.dma_desc_num = kDmaDescriptors;
    // DMA 欠载时发送静音，不重播陈旧音频块。
    channel_config.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&channel_config, &tx_channel_, nullptr);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_config{};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config_.sample_rate_hz);
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.bclk = config_.bclk_gpio;
    std_config.gpio_cfg.ws = config_.ws_gpio;
    std_config.gpio_cfg.dout = config_.dout_gpio;
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(tx_channel_, &std_config);
    if (err != ESP_OK) return err;
    if (config_.playback_reference_enabled && config_.sample_rate_hz == 24000 && !reference_.enabled()) {
        // 先分配必要的 TX/DMA 资源，再分配可选观测器。
        const esp_err_t observer_err = reference_.start();
        if (observer_err != ESP_OK)
            ESP_LOGW(kTag, "Playback reference unavailable: %s; half-duplex remains active",
                     esp_err_to_name(observer_err));
    }
    amplifier_enabled_.store(0, std::memory_order_release);
    progress_.reset();
    progress_serial_ = 0;
    progress_submitted_ = 0;
    if (reference_.enabled()) {
        reference_.beginGeneration();
        i2s_event_callbacks_t callbacks{};
        callbacks.on_sent = onDmaSent;
        const esp_err_t observer_err = i2s_channel_register_event_callback(tx_channel_, &callbacks, this);
        if (observer_err != ESP_OK) {
            // 此时尚未安装回调，TX 也未启动。
            // 观测失败不能禁用已建立的播放路径。
            reference_.stop();
            ESP_LOGW(kTag, "Playback reference callback unavailable: %s", esp_err_to_name(observer_err));
        }
    }
    observed_speaker.store(this, std::memory_order_release);
    observed_channel.store(tx_channel_, std::memory_order_release);
    // UART 可能等待其他任务的日志锁，时间超过有界参考 ISR 队列容量。
    // 启动 TX 回调前完成此消息输出。
    ESP_LOGI(kTag, "I2S TX configured: %u Hz, 16-bit Philips, DOUT=%d BCLK=%d WS=%d",
             static_cast<unsigned int>(config_.sample_rate_hz),
             config_.dout_gpio, config_.bclk_gpio, config_.ws_gpio);
    if (enabled || sd_configured_) {
        err = i2s_channel_enable(tx_channel_);
        if (err != ESP_OK) return err;
        channel_enabled_ = true;
    }
    ready_ = true;
    enabled_ = false;
    // 没有 SD 的配置已在上面启用通道。
    if (enabled && !sd_configured_) {
        enabled_ = true;
        amplifier_enabled_.store(1, std::memory_order_release);
    }
    if (enabled) err = setEnabled(true);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

bool IRAM_ATTR Max98357aAudio::onDmaSent(i2s_chan_handle_t, i2s_event_data_t *event, void *user_data)
{
    auto *self = static_cast<Max98357aAudio *>(user_data);
    if (!event) return false;
    const bool enabled = self->amplifier_enabled_.load(std::memory_order_relaxed) != 0;
    // 在任务代码添加新标签前，先快照已完成描述符的旧标签。
    // 参考路径仍在 SDK 自动清零前读取借用 PCM。
    portENTER_CRITICAL_ISR(&self->progress_mux_);
    if (event->size != kChunkFrames * 2 * sizeof(int16_t)) self->progress_.invalidate();
    else self->progress_.onSent(event->dma_buf, enabled);
    portEXIT_CRITICAL_ISR(&self->progress_mux_);
    return self->reference_.onDmaSent(event->dma_buf, event->size, enabled);
}

esp_err_t Max98357aAudio::stopChannel()
{
    esp_err_t result = ESP_OK;
    ready_ = false;
    enabled_ = false;
    reference_.setAmplifierTransitionActive(true);
    if (sd_configured_) {
        const esp_err_t err = gpio_set_level(config_.sd_mode_gpio, 0);
        // 无法拉低关闭信号时，保留通道完整。
        // 后续重试可先静音，再移除时钟并删除 DMA 环。
        if (err != ESP_OK) {
            reference_.setAmplifierTransitionActive(false);
            return err;
        }
    }
    amplifier_enabled_.store(0, std::memory_order_release);
    reference_.setAmplifierTransitionActive(false);

    if (tx_channel_ != nullptr) {
        if (channel_enabled_) {
            const esp_err_t err = i2s_channel_disable(tx_channel_);
            if (err != ESP_OK && result == ESP_OK) {
                result = err;
            }
            if (err == ESP_OK) channel_enabled_ = false;
        }
        if (!channel_enabled_) {
            const esp_err_t err = i2s_del_channel(tx_channel_);
            if (err != ESP_OK && result == ESP_OK) result = err;
            if (err == ESP_OK) tx_channel_ = nullptr;
        }
    }

    if (tx_channel_ == nullptr && stereo_buffer_ != nullptr) {
        if (observed_speaker.load(std::memory_order_acquire) == this) {
            observed_channel.store(nullptr, std::memory_order_release);
            observed_speaker.store(nullptr, std::memory_order_release);
        }
        progress_.reset();
        heap_caps_free(stereo_buffer_);
        stereo_buffer_ = nullptr;
    }
    return result;
}

esp_err_t Max98357aAudio::resetPlayback()
{
    if (!configured_) return ESP_ERR_INVALID_STATE;
    esp_err_t err = stopChannel();
    if (err != ESP_OK) return err;
    reference_.pump();
    // 运行指标由 AudioService 释放扬声器互斥锁后打印。
    // 此处 UART 输出会延迟取消，并消耗 ISR 队列余量。
    err = startChannel(false);
    if (err != ESP_OK) stopChannel();
    return err;
}

esp_err_t Max98357aAudio::deinit()
{
    esp_err_t err = stopChannel();
    if (err != ESP_OK) return err;
    reference_.pump();
    logReferenceMetrics();
    reference_.stop();
    if (sd_configured_) {
        // IDF 6.1 的 gpio_reset_pin() 会启用上拉，可能唤醒 MAX98357A。
        // 保持 LOW、设置下拉，再禁用输出，最后撤销所有权。
        // 重建过程绝不释放此引脚。
        err = gpio_set_pull_mode(config_.sd_mode_gpio, GPIO_PULLDOWN_ONLY);
        if (err != ESP_OK) return err;
        err = gpio_set_direction(config_.sd_mode_gpio, GPIO_MODE_DISABLE);
        if (err != ESP_OK) return err;
        esp_gpio_revoke(1ULL << config_.sd_mode_gpio);
        sd_configured_ = false;
    }
    configured_ = false;
    return ESP_OK;
}

void Max98357aAudio::logPlaybackReference()
{
    reference_.pump();
    logReferenceMetrics();
}

void Max98357aAudio::logReferenceMetrics() const
{
    logPlaybackReferenceSnapshot(reference_.metrics());
}

void Max98357aAudio::logPlaybackReferenceSnapshot(const PlaybackReference::Metrics &metrics)
{
    if (!metrics.enabled) return;
    ESP_LOGI(kTag, "playback_reference generation=%u playback_serial=%u submitted_frames=%u completed_frames=%u "
             "blocks=%u silence=%u content=%u muted_blocks=%u amp_boundary=%u partial_bytes=%u stereo_mismatch=%u "
             "invalid=%u queue_dropped=%u queue=%u history=%u history_overwrites=%u "
             "completed_sample_24k_low=%u last_end_us_low=%u "
             "history_available=%u history_unavailable=%u discontinuity=%u binding_fault=%u",
             static_cast<unsigned>(metrics.dma_generation),
             static_cast<unsigned>(metrics.playback_serial),
             static_cast<unsigned>(metrics.submitted_frames),
             static_cast<unsigned>(metrics.completed_frames),
             static_cast<unsigned>(metrics.completed_blocks),
             static_cast<unsigned>(metrics.known_silence_blocks),
             static_cast<unsigned>(metrics.non_silence_blocks),
             static_cast<unsigned>(metrics.amplifier_muted_blocks),
             static_cast<unsigned>(metrics.amplifier_boundary_blocks),
             static_cast<unsigned>(metrics.partial_write_bytes),
             static_cast<unsigned>(metrics.stereo_mismatch_samples),
             static_cast<unsigned>(metrics.invalid_blocks),
             static_cast<unsigned>(metrics.queue_dropped_blocks),
             static_cast<unsigned>(metrics.queue_blocks),
             static_cast<unsigned>(metrics.history_blocks),
             static_cast<unsigned>(metrics.history_overwrites),
             static_cast<unsigned>(metrics.completed_sample_24k_low),
             static_cast<unsigned>(metrics.last_observed_end_us_low),
             static_cast<unsigned>(metrics.history_available),
             static_cast<unsigned>(metrics.history_unavailable_blocks),
             static_cast<unsigned>(metrics.discontinuity),
             static_cast<unsigned>(metrics.binding_fault));
}

esp_err_t Max98357aAudio::setEnabled(bool enabled)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled == enabled_) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    reference_.setAmplifierTransitionActive(true);
    if (config_.sd_mode_gpio != GPIO_NUM_NC) {
        err = gpio_set_level(config_.sd_mode_gpio, enabled ? 1 : 0);
        if (err == ESP_OK && enabled) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else if (enabled) {
        err = i2s_channel_enable(tx_channel_);
        channel_enabled_ = err == ESP_OK;
    } else {
        err = i2s_channel_disable(tx_channel_);
        if (err == ESP_OK) channel_enabled_ = false;
    }
    if (err == ESP_OK) {
        enabled_ = enabled;
        amplifier_enabled_.store(enabled ? 1 : 0, std::memory_order_release);
    }
    reference_.setAmplifierTransitionActive(false);
    if (err == ESP_OK && enabled && sd_configured_ && reference_.enabled()) {
        // 下一个完成的 DMA 块可能跨越 SD 边沿的结束时刻。
        // 允许调用方提交内容前，让已排空/空的环再保持一整块零样本，
        // 从而保留可证明的参考信号。
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
    reference_.pump();
    return err;
}

esp_err_t Max98357aAudio::setVolume(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    config_.volume_percent = percent;
    return ESP_OK;
}

esp_err_t Max98357aAudio::writeMono(const int16_t *samples, size_t sample_count, uint32_t timeout_ms)
{
    if (!ready_ || !enabled_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == nullptr && sample_count > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (offset < sample_count) {
        const size_t frames = std::min(kChunkFrames, sample_count - offset);
        for (size_t i = 0; i < frames; ++i) {
            const int32_t scaled = static_cast<int32_t>(samples[offset + i]) * config_.volume_percent / 100;
            stereo_buffer_[2 * i] = static_cast<int16_t>(scaled);
            stereo_buffer_[2 * i + 1] = static_cast<int16_t>(scaled);
        }

        const size_t bytes = frames * 2 * sizeof(int16_t);
        size_t bytes_written = 0;
        portENTER_CRITICAL(&progress_mux_);
        progress_write_first_ = progress_submitted_;
        progress_content_write_ = !draining_;
        progress_write_active_ = true;
        portEXIT_CRITICAL(&progress_mux_);
        const esp_err_t err = i2s_channel_write(tx_channel_, stereo_buffer_, bytes, &bytes_written, timeout_ms);
        portENTER_CRITICAL(&progress_mux_);
        progress_write_active_ = false;
        if (err != ESP_OK || bytes_written != bytes) progress_.invalidate();
        if (!draining_) progress_submitted_ += bytes_written / (2 * sizeof(int16_t));
        portEXIT_CRITICAL(&progress_mux_);
        reference_.recordSubmittedBytes(bytes_written, bytes, err == ESP_OK);
        reference_.pump();
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_written != bytes) {
            return ESP_ERR_TIMEOUT;
        }
        offset += frames;
    }
    return ESP_OK;
}

esp_err_t Max98357aAudio::drain(uint32_t timeout_ms, void (*progress)(void *), void *context)
{
    const int16_t silence[kChunkFrames] = {};
    draining_ = true;
    for (size_t i = 0; i <= kDmaDescriptors; ++i) {
        const esp_err_t err = writeMono(silence, kChunkFrames, timeout_ms);
        if (err != ESP_OK) { draining_ = false; return err; }
        if (progress) progress(context);
    }
    // 在播放工作任务中更新已完成参考，不输出 UART。
    // 明确的 ? 快照请求在扬声器互斥锁外打印。
    reference_.pump();
    reference_.markDrained();
    draining_ = false;
    return ESP_OK;
}
