#include "ics43434_microphone.hpp"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "ics43434";
constexpr size_t kDmaFrameNum = 256;
// 启用 IDF 的 PSRAM 原子操作兼容处理后，即使内部 RAM 的 uint32_t
// 操作走硬件 CAS 路径，全局 C++ 无锁特征仍可能为假。
// 下面检查整个 ISR 持有对象，而不依赖该全局特征。
}

bool Ics43434Microphone::isSupportedSampleRate(uint32_t sample_rate_hz)
{
    switch (sample_rate_hz) {
    case 16000:
    case 32000:
    case 44100:
    case 48000:
        return true;
    default:
        return false;
    }
}

esp_err_t Ics43434Microphone::init()
{
    return init(Config{});
}

esp_err_t Ics43434Microphone::init(const Config &config)
{
    if (ready_ || rx_channel_ || !isSupportedSampleRate(config.sample_rate_hz) ||
        (config.i2s_port != I2S_NUM_0 && config.i2s_port != I2S_NUM_1) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.bclk_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.ws_gpio) ||
        !GPIO_IS_VALID_GPIO(config.din_gpio) ||
        config.bclk_gpio == config.ws_gpio || config.bclk_gpio == config.din_gpio ||
        config.ws_gpio == config.din_gpio) {
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;
    resetTiming();

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(config_.i2s_port, I2S_ROLE_MASTER);
    channel_config.dma_frame_num = kDmaFrameNum;
    esp_err_t err = i2s_new_channel(&channel_config, nullptr, &rx_channel_);
    if (err != ESP_OK) {
        deinit();
        return err;
    }

    i2s_std_config_t std_config{};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config_.sample_rate_hz);
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    std_config.slot_cfg.slot_mask = config_.left_channel ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;
    std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.bclk = config_.bclk_gpio;
    std_config.gpio_cfg.ws = config_.ws_gpio;
    std_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.din = config_.din_gpio;
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(rx_channel_, &std_config);
    if (err != ESP_OK) {
        deinit();
        return err;
    }
    if (config_.observe_dma_timing) {
        // 上下文及全部由 ISR 写入的原子字段都必须位于内部 RAM，
        // 即使 CONFIG_I2S_ISR_IRAM_SAFE 未启用也一样。
        const bool internal = esp_ptr_internal(this) && esp_ptr_internal(
            reinterpret_cast<const uint8_t *>(this) + sizeof(*this) - 1);
        esp_err_t observer_err = ESP_ERR_INVALID_STATE;
        if (internal) {
            i2s_event_callbacks_t callbacks{};
            callbacks.on_recv = onDmaReceived;
            callbacks.on_recv_q_ovf = onDmaOverflow;
            observer_err = i2s_channel_register_event_callback(rx_channel_, &callbacks, this);
            if (observer_err == ESP_OK) {
                timing_enabled_.store(1, std::memory_order_release);
            }
        }
        // IDF 先校验注册参数，再更新回调。
        // 缺少观测器不能阻止已建立的原始手动采集。
        if (observer_err != ESP_OK)
            ESP_LOGW(kTag, "RX DMA timing unavailable: %s; raw capture remains available",
                     esp_err_to_name(observer_err));
    }
    ready_ = true;
    // UART 日志须在启用前输出，避免刚启动的读取消费者等待控制台时，
    // 有界 DMA 队列已被填满。
    ESP_LOGI(kTag, "I2S RX configured: %u Hz, 32-bit Philips slot, BCLK=%d WS=%d DIN=%d channel=%s start=%s",
             static_cast<unsigned int>(config_.sample_rate_hz), config_.bclk_gpio,
             config_.ws_gpio, config_.din_gpio, config_.left_channel ? "left" : "right",
             config_.start_immediately ? "immediate" : "consumer");
    if (config_.start_immediately) {
        err = startCapture();
        if (err != ESP_OK) {
            deinit();
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t Ics43434Microphone::startCapture()
{
    if (!ready_ || !rx_channel_ || channel_enabled_) return ESP_ERR_INVALID_STATE;
    const uint64_t started_us = static_cast<uint64_t>(esp_timer_get_time());
    // 只有第一次成功启用才开始本代次。回调可能早于启用返回，
    // 之后不能再重置这些计数器。
    dma_timeline_valid_.store(timing_enabled_.load(std::memory_order_acquire),
                              std::memory_order_release);
    const esp_err_t err = i2s_channel_enable(rx_channel_);
    if (err != ESP_OK) {
        dma_timeline_valid_.store(0, std::memory_order_release);
        return err;
    }
    channel_enabled_ = true;
    capture_start_us_low_.store(static_cast<uint32_t>(started_us), std::memory_order_relaxed);
    capture_start_us_high_.store(static_cast<uint32_t>(started_us >> 32), std::memory_order_relaxed);
    capture_started_.store(1, std::memory_order_release);
    return ESP_OK;
}

esp_err_t Ics43434Microphone::deinit()
{
    ready_ = false;
    if (rx_channel_ != nullptr) {
        if (channel_enabled_) {
            const esp_err_t err = i2s_channel_disable(rx_channel_);
            // 禁用失败时，DMA 及其 ISR 可能仍在运行。
            // 保留句柄/上下文，使持有者可安全重试关闭。
            if (err != ESP_OK) return err;
            channel_enabled_ = false;
        }
        const esp_err_t err = i2s_del_channel(rx_channel_);
        if (err != ESP_OK) return err;
        rx_channel_ = nullptr;
    }
    timing_enabled_.store(0, std::memory_order_release);
    return ESP_OK;
}

esp_err_t Ics43434Microphone::read(int32_t *samples, size_t sample_capacity,
                                   size_t *samples_read, uint32_t timeout_ms, ReadTiming *timing)
{
    if (samples_read) *samples_read = 0;
    if (timing) *timing = ReadTiming{};
    if (!ready_ || !channel_enabled_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == nullptr || samples_read == nullptr || sample_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_capacity > SIZE_MAX / sizeof(*samples)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t read_start_us = static_cast<uint64_t>(esp_timer_get_time());
    if (!first_read_started_.load(std::memory_order_relaxed)) {
        first_read_us_low_.store(static_cast<uint32_t>(read_start_us), std::memory_order_relaxed);
        first_read_us_high_.store(static_cast<uint32_t>(read_start_us >> 32), std::memory_order_relaxed);
        first_read_started_.store(1, std::memory_order_release);
    }
    if (service_task_.read_completions && read_start_us >= service_task_.last_read_end_us) {
        const uint64_t gap_us = read_start_us - service_task_.last_read_end_us;
        if (gap_us > service_task_.max_read_gap_us) service_task_.max_read_gap_us = gap_us;
    }
    changeConsumerPhase(ConsumerPhase::Read, read_start_us);
    service_task_.last_read_start_us = read_start_us;
    ++service_task_.read_calls;
    publishService();
    size_t bytes_read = 0;
    const esp_err_t err = i2s_channel_read(rx_channel_, samples,
                                          sample_capacity * sizeof(*samples),
                                          &bytes_read, timeout_ms);
    const uint64_t read_end_us = static_cast<uint64_t>(esp_timer_get_time());
    if (!service_task_.read_completions) service_task_.first_read_end_us = read_end_us;
    ++service_task_.read_completions;
    service_task_.last_read_end_us = read_end_us;
    if (read_end_us >= read_start_us && read_end_us - read_start_us > service_task_.max_read_us)
        service_task_.max_read_us = read_end_us - read_start_us;
    if (bytes_read > sample_capacity * sizeof(*samples) || bytes_read % sizeof(*samples) != 0) {
        // 驱动已消费无法表示的样本范围。
        // 重试原始采集无法恢复它与物理 DMA 索引的关联。
        read_timeline_valid_.store(0, std::memory_order_release);
        publishService();
        return ESP_ERR_INVALID_SIZE;
    }

    *samples_read = bytes_read / sizeof(*samples);
    const uint64_t first = returned_samples_;
    returned_samples_ += *samples_read;
    service_task_.last_first_sample = first;
    service_task_.last_sample_end = returned_samples_;
    publishService();
    if (timing) {
        const DmaMetrics snapshot = dmaMetrics();
        timing->first_sample = first;
        timing->sample_end = returned_samples_;
        timing->dma_completed_samples = snapshot.completed_samples;
        timing->dma_completion_us = snapshot.last_completion_us;
        if (snapshot.completed_samples >= returned_samples_)
            timing->completed_samples_ahead = snapshot.completed_samples - returned_samples_;
        timing->overflow_events = snapshot.overflow_events;
        timing->overflow_event_samples = snapshot.overflow_event_samples;
        timing->timing_enabled = snapshot.timing_enabled;
        timing->continuous = snapshot.timeline_valid && snapshot.timing_enabled &&
                             snapshot.completed_samples >= returned_samples_;
    }
    // IDF 可能在复制部分数据后报告超时。
    // 即使调用方因错误停止处理，也要精确统计已经消费的这些样本。
    return err;
}

const char *Ics43434Microphone::consumerPhaseName(ConsumerPhase phase)
{
    switch (phase) {
    case ConsumerPhase::Starting: return "starting";
    case ConsumerPhase::Read: return "read";
    case ConsumerPhase::Convert: return "convert";
    case ConsumerPhase::Callback: return "callback";
    case ConsumerPhase::Queue: return "queue";
    case ConsumerPhase::Stopped: return "stopped";
    }
    return "unknown";
}

void Ics43434Microphone::changeConsumerPhase(ConsumerPhase phase, uint64_t now_us)
{
    if (service_task_.phase == phase && service_task_.phase_since_us) return;
    if (service_task_.phase == ConsumerPhase::Callback && now_us >= service_task_.phase_since_us) {
        service_task_.last_callback_us = now_us - service_task_.phase_since_us;
        if (service_task_.last_callback_us > service_task_.max_callback_us)
            service_task_.max_callback_us = service_task_.last_callback_us;
    }
    service_task_.phase = phase;
    service_task_.phase_since_us = now_us;
}

void Ics43434Microphone::noteConsumerPhase(ConsumerPhase phase)
{
    changeConsumerPhase(phase, static_cast<uint64_t>(esp_timer_get_time()));
    publishService();
}

void Ics43434Microphone::encodeService(const ServiceMetrics &metrics, uint32_t *words)
{
    words[0] = metrics.read_calls;
    words[1] = metrics.read_completions;
    words[2] = static_cast<uint32_t>(metrics.phase);
    words[3] = static_cast<uint32_t>(metrics.core);
    const uint64_t values[] = {
        metrics.first_read_end_us, metrics.last_read_start_us, metrics.last_read_end_us,
        metrics.last_first_sample, metrics.last_sample_end, metrics.max_read_us,
        metrics.max_read_gap_us, metrics.phase_since_us, metrics.last_callback_us,
        metrics.max_callback_us,
    };
    static_assert(sizeof(values) / sizeof(values[0]) * 2 + 4 == kServiceWordCount,
                  "service publication layout");
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        words[4 + i * 2] = static_cast<uint32_t>(values[i]);
        words[5 + i * 2] = static_cast<uint32_t>(values[i] >> 32);
    }
}

Ics43434Microphone::ServiceMetrics Ics43434Microphone::decodeService(const uint32_t *words)
{
    ServiceMetrics metrics{};
    metrics.coherent = true;
    metrics.read_calls = words[0];
    metrics.read_completions = words[1];
    metrics.phase = static_cast<ConsumerPhase>(words[2]);
    metrics.core = words[3] == UINT32_MAX ? -1 : static_cast<int>(words[3]);
    uint64_t *values[] = {
        &metrics.first_read_end_us, &metrics.last_read_start_us, &metrics.last_read_end_us,
        &metrics.last_first_sample, &metrics.last_sample_end, &metrics.max_read_us,
        &metrics.max_read_gap_us, &metrics.phase_since_us, &metrics.last_callback_us,
        &metrics.max_callback_us,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        *values[i] = static_cast<uint64_t>(words[5 + i * 2]) << 32 | words[4 + i * 2];
    return metrics;
}

void Ics43434Microphone::publishService()
{
    service_sequence_.fetch_add(1, std::memory_order_acq_rel);
    service_task_.core = xPortGetCoreID();
    uint32_t words[kServiceWordCount];
    encodeService(service_task_, words);
    for (size_t i = 0; i < kServiceWordCount; ++i)
        service_words_[i].store(words[i], std::memory_order_relaxed);
    service_sequence_.fetch_add(1, std::memory_order_release);
}

bool IRAM_ATTR Ics43434Microphone::onDmaReceived(i2s_chan_handle_t, i2s_event_data_t *event,
                                                  void *user_data)
{
    auto *self = static_cast<Ics43434Microphone *>(user_data);
    if (self) self->recordDmaEvent(event, false);
    return false;
}

bool IRAM_ATTR Ics43434Microphone::onDmaOverflow(i2s_chan_handle_t, i2s_event_data_t *event,
                                                  void *user_data)
{
    auto *self = static_cast<Ics43434Microphone *>(user_data);
    if (self) self->recordDmaEvent(event, true);
    return false;
}

void IRAM_ATTR Ics43434Microphone::recordDmaEvent(i2s_event_data_t *event, bool overflow)
{
    timing_sequence_.fetch_add(1, std::memory_order_acq_rel);
    if (overflow) {
        if (dma_overflow_events_.fetch_add(1, std::memory_order_relaxed) == 0) {
            const uint64_t overflow_us = static_cast<uint64_t>(esp_timer_get_time());
            first_overflow_us_low_.store(static_cast<uint32_t>(overflow_us), std::memory_order_relaxed);
            first_overflow_us_high_.store(static_cast<uint32_t>(overflow_us >> 32), std::memory_order_relaxed);
            first_overflow_sample_low_.store(static_cast<uint32_t>(dma_sample_cursor_isr_),
                                              std::memory_order_relaxed);
            first_overflow_sample_high_.store(static_cast<uint32_t>(dma_sample_cursor_isr_ >> 32),
                                               std::memory_order_relaxed);
            // 只做一次有界尝试：消费者若在发布中途被抢占，
            // 在此 ISR 执行期间无法完成，绝不能自旋等待其序号。
            const uint32_t service_sequence = service_sequence_.load(std::memory_order_acquire);
            if (!(service_sequence & 1U)) {
                for (size_t i = 0; i < kServiceWordCount; ++i)
                    first_overflow_service_words_[i].store(
                        service_words_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                first_overflow_service_coherent_.store(
                    service_sequence == service_sequence_.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
        }
        // IDF 的溢出大小是被丢弃队列缓冲的大小。
        // 该计数不能恢复连续读取时间线，因此永久标记失效。
        if (event && event->size % sizeof(int32_t) == 0)
            dma_overflow_samples_isr_ += event->size / sizeof(int32_t);
        dma_overflow_samples_low_.store(static_cast<uint32_t>(dma_overflow_samples_isr_),
                                        std::memory_order_relaxed);
        dma_overflow_samples_high_.store(static_cast<uint32_t>(dma_overflow_samples_isr_ >> 32),
                                         std::memory_order_relaxed);
        dma_timeline_valid_.store(0, std::memory_order_relaxed);
    } else {
        dma_completed_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (event && event->size == kDmaFrameNum * sizeof(int32_t)) {
            dma_sample_cursor_isr_ += kDmaFrameNum;
        } else {
            dma_invalid_events_.fetch_add(1, std::memory_order_relaxed);
            dma_timeline_valid_.store(0, std::memory_order_relaxed);
        }
        const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
        dma_completed_samples_low_.store(static_cast<uint32_t>(dma_sample_cursor_isr_),
                                         std::memory_order_relaxed);
        dma_completed_samples_high_.store(static_cast<uint32_t>(dma_sample_cursor_isr_ >> 32),
                                          std::memory_order_relaxed);
        dma_completion_us_low_.store(static_cast<uint32_t>(now_us), std::memory_order_relaxed);
        dma_completion_us_high_.store(static_cast<uint32_t>(now_us >> 32), std::memory_order_relaxed);
    }
    timing_sequence_.fetch_add(1, std::memory_order_release);
}

void Ics43434Microphone::resetTiming()
{
    dma_sample_cursor_isr_ = dma_overflow_samples_isr_ = returned_samples_ = 0;
    timing_sequence_.store(0, std::memory_order_relaxed);
    timing_enabled_.store(0, std::memory_order_relaxed);
    dma_completed_blocks_.store(0, std::memory_order_relaxed);
    dma_completed_samples_low_.store(0, std::memory_order_relaxed);
    dma_completed_samples_high_.store(0, std::memory_order_relaxed);
    dma_completion_us_low_.store(0, std::memory_order_relaxed);
    dma_completion_us_high_.store(0, std::memory_order_relaxed);
    dma_overflow_events_.store(0, std::memory_order_relaxed);
    dma_overflow_samples_low_.store(0, std::memory_order_relaxed);
    dma_overflow_samples_high_.store(0, std::memory_order_relaxed);
    dma_invalid_events_.store(0, std::memory_order_relaxed);
    dma_timeline_valid_.store(0, std::memory_order_relaxed);
    read_timeline_valid_.store(1, std::memory_order_relaxed);
    capture_started_.store(0, std::memory_order_relaxed);
    first_read_started_.store(0, std::memory_order_relaxed);
    capture_start_us_low_.store(0, std::memory_order_relaxed);
    capture_start_us_high_.store(0, std::memory_order_relaxed);
    first_read_us_low_.store(0, std::memory_order_relaxed);
    first_read_us_high_.store(0, std::memory_order_relaxed);
    first_overflow_us_low_.store(0, std::memory_order_relaxed);
    first_overflow_us_high_.store(0, std::memory_order_relaxed);
    first_overflow_sample_low_.store(0, std::memory_order_relaxed);
    first_overflow_sample_high_.store(0, std::memory_order_relaxed);
    service_task_ = ServiceMetrics{};
    service_sequence_.store(0, std::memory_order_relaxed);
    first_overflow_service_coherent_.store(0, std::memory_order_relaxed);
    uint32_t service_words[kServiceWordCount];
    encodeService(service_task_, service_words);
    for (size_t i = 0; i < kServiceWordCount; ++i) {
        service_words_[i].store(service_words[i], std::memory_order_relaxed);
        first_overflow_service_words_[i].store(0, std::memory_order_relaxed);
    }
}

Ics43434Microphone::DmaMetrics Ics43434Microphone::dmaMetrics() const
{
    DmaMetrics snapshot{};
    uint32_t overflow_service_words[kServiceWordCount]{};
    bool overflow_service_coherent = false;
    for (;;) {
        const uint32_t sequence = timing_sequence_.load(std::memory_order_acquire);
        if (sequence & 1U) continue;
        snapshot.completed_blocks = dma_completed_blocks_.load(std::memory_order_relaxed);
        snapshot.completed_samples =
            static_cast<uint64_t>(dma_completed_samples_high_.load(std::memory_order_relaxed)) << 32 |
            dma_completed_samples_low_.load(std::memory_order_relaxed);
        snapshot.last_completion_us =
            static_cast<uint64_t>(dma_completion_us_high_.load(std::memory_order_relaxed)) << 32 |
            dma_completion_us_low_.load(std::memory_order_relaxed);
        snapshot.overflow_events = dma_overflow_events_.load(std::memory_order_relaxed);
        snapshot.overflow_event_samples =
            static_cast<uint64_t>(dma_overflow_samples_high_.load(std::memory_order_relaxed)) << 32 |
            dma_overflow_samples_low_.load(std::memory_order_relaxed);
        snapshot.invalid_events = dma_invalid_events_.load(std::memory_order_relaxed);
        snapshot.timeline_valid = dma_timeline_valid_.load(std::memory_order_relaxed) != 0;
        snapshot.first_overflow_us =
            static_cast<uint64_t>(first_overflow_us_high_.load(std::memory_order_relaxed)) << 32 |
            first_overflow_us_low_.load(std::memory_order_relaxed);
        snapshot.first_overflow_sample_end =
            static_cast<uint64_t>(first_overflow_sample_high_.load(std::memory_order_relaxed)) << 32 |
            first_overflow_sample_low_.load(std::memory_order_relaxed);
        overflow_service_coherent = first_overflow_service_coherent_.load(std::memory_order_relaxed) != 0;
        if (overflow_service_coherent)
            for (size_t i = 0; i < kServiceWordCount; ++i)
                overflow_service_words[i] = first_overflow_service_words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence == timing_sequence_.load(std::memory_order_relaxed)) break;
    }
    if (overflow_service_coherent)
        snapshot.first_overflow_service = decodeService(overflow_service_words);
    uint32_t service_words[kServiceWordCount];
    // 诊断调用方的优先级可能高于唯一消费者。
    // 消费者在奇数发布序号期间被抢占时，诊断方绝不能自旋。
    const uint32_t service_sequence = service_sequence_.load(std::memory_order_acquire);
    if (!(service_sequence & 1U)) {
        for (size_t i = 0; i < kServiceWordCount; ++i)
            service_words[i] = service_words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (service_sequence == service_sequence_.load(std::memory_order_relaxed))
            snapshot.service = decodeService(service_words);
    }
    snapshot.capture_started = capture_started_.load(std::memory_order_acquire) != 0;
    if (snapshot.capture_started)
        snapshot.capture_start_us =
            static_cast<uint64_t>(capture_start_us_high_.load(std::memory_order_relaxed)) << 32 |
            capture_start_us_low_.load(std::memory_order_relaxed);
    snapshot.first_read_started = first_read_started_.load(std::memory_order_acquire) != 0;
    if (snapshot.first_read_started)
        snapshot.first_read_us =
            static_cast<uint64_t>(first_read_us_high_.load(std::memory_order_relaxed)) << 32 |
            first_read_us_low_.load(std::memory_order_relaxed);
    snapshot.timing_enabled = timing_enabled_.load(std::memory_order_acquire) != 0;
    if (!snapshot.timing_enabled || !read_timeline_valid_.load(std::memory_order_acquire))
        snapshot.timeline_valid = false;
    return snapshot;
}
