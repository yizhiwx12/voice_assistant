#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "../app/board_pins.hpp"

// ICS-43434 麦克风驱动：把 I2S 的 32 位槽读回，保留 24 位有效信号。
// 默认接线见 app/board_pins.hpp；自动对话的 16 kHz 采样率必须与前端/协议一起修改。
// ISR 只发布 DMA 计数，耗时转换和业务回调放在读取任务中。
class Ics43434Microphone {
public:
    enum class ConsumerPhase { Starting, Read, Convert, Callback, Queue, Stopped };

    struct ServiceMetrics {
        // ISR 仅尝试一次有界快照。为假表示复制与消费者发布重叠，
        // 其余字段不提供有效数据。
        bool coherent = false;
        ConsumerPhase phase = ConsumerPhase::Starting;
        int core = -1;
        uint32_t read_calls = 0, read_completions = 0;
        uint64_t first_read_end_us = 0;
        uint64_t last_read_start_us = 0, last_read_end_us = 0;
        uint64_t last_first_sample = 0, last_sample_end = 0;
        uint64_t max_read_us = 0, max_read_gap_us = 0;
        uint64_t phase_since_us = 0;
        uint64_t last_callback_us = 0, max_callback_us = 0;
    };

    struct Config {
        int i2s_port = I2S_NUM_1;
        uint32_t sample_rate_hz = 16000;
        gpio_num_t bclk_gpio = voice::board::kMicrophoneBclk;
        gpio_num_t ws_gpio = voice::board::kMicrophoneWs;
        gpio_num_t din_gpio = voice::board::kMicrophoneData;
        bool left_channel = true;
        bool observe_dma_timing = false;
        // 延后启用 RX 时钟/DMA，直到唯一读取消费者就绪。
        bool start_immediately = true;
    };

    struct DmaMetrics {
        uint32_t completed_blocks = 0;
        uint64_t completed_samples = 0;
        uint64_t last_completion_us = 0;
        uint32_t overflow_events = 0;
        uint64_t overflow_event_samples = 0;
        uint32_t invalid_events = 0;
        bool timing_enabled = false;
        bool timeline_valid = false;
        bool capture_started = false;
        bool first_read_started = false;
        // 启动诊断时刻，不是物理 RX/TX 对齐锚点。
        // capture_start_us 在成功的启用调用前取样；
        // first_read_us 在第一次合法驱动读取前取样。
        uint64_t capture_start_us = 0;
        uint64_t first_read_us = 0;
        uint64_t first_overflow_us = 0;
        uint64_t first_overflow_sample_end = 0;
        ServiceMetrics service{};
        ServiceMetrics first_overflow_service{};
    };

    struct ReadTiming {
        // read() 实际返回的样本序号，init() 后从零开始。
        // 仅当 continuous 为真时，它才对应物理 DMA 样本索引。
        uint64_t first_sample = 0;
        uint64_t sample_end = 0;
        uint64_t dma_completed_samples = 0;
        uint64_t dma_completion_us = 0;
        uint64_t completed_samples_ahead = 0;
        uint32_t overflow_events = 0;
        uint64_t overflow_event_samples = 0;
        bool timing_enabled = false;
        bool continuous = false;
    };

    Ics43434Microphone() = default;
    Ics43434Microphone(const Ics43434Microphone &) = delete;
    Ics43434Microphone &operator=(const Ics43434Microphone &) = delete;

    esp_err_t init();
    esp_err_t init(const Config &config);
    // 串行化生命周期操作。延迟初始化已就绪，但本调用成功前不能读取。
    // 重复启动会被拒绝，失败可以重试。
    // 本方法启用 RX 后不再打印日志或执行阻塞工作。
    esp_err_t startCapture();
    esp_err_t deinit();

    // 样本为左对齐存放在 int32_t 中的有符号 24 位值。
    // 只允许一个读取消费者。可选时序描述 DMA 完成锚点及已消费样本范围，
    // 绝不是本任务被调度的时刻。DMA 队列溢出会永久破坏物理样本关联，
    // 直到受控 deinit/init；原始读取仍可使用。
    esp_err_t read(int32_t *samples, size_t sample_capacity, size_t *samples_read,
                   uint32_t timeout_ms = 1000, ReadTiming *timing = nullptr);

    bool isReady() const { return ready_; }
    uint32_t sampleRateHz() const { return config_.sample_rate_hz; }
    DmaMetrics dmaMetrics() const;
    // 仅供唯一读取消费者任务调用。
    // 诊断任务阶段绝不能代替 DMA 完成时钟作为物理样本锚点。
    void noteConsumerPhase(ConsumerPhase phase);
    static const char *consumerPhaseName(ConsumerPhase phase);

private:
    static bool isSupportedSampleRate(uint32_t sample_rate_hz);
    // IRAM_ATTR 仅用于定义：IDF 每次使用都会生成独立段名，
    // 因此在声明中重复使用会在 GCC 15 下冲突。
    static bool onDmaReceived(i2s_chan_handle_t channel, i2s_event_data_t *event,
                                        void *user_data);
    static bool onDmaOverflow(i2s_chan_handle_t channel, i2s_event_data_t *event,
                                        void *user_data);
    void resetTiming();  // 仅在安装回调前调用。
    void recordDmaEvent(i2s_event_data_t *event, bool overflow);
    void changeConsumerPhase(ConsumerPhase phase, uint64_t now_us);
    void publishService();
    static constexpr size_t kServiceWordCount = 24;
    static void encodeService(const ServiceMetrics &metrics, uint32_t *words);
    static ServiceMetrics decodeService(const uint32_t *words);

    Config config_{};
    i2s_chan_handle_t rx_channel_ = nullptr;
    bool channel_enabled_ = false;
    bool ready_ = false;
    // I2S 在同一 RX ISR 中串行分发 on_recv 和 on_recv_q_ovf。
    // 64 位运算由 ISR 独占，拆分为无锁字段并用序号计数器发布，
    // 快照重试放在任务上下文。中断中不调用可能在 Xtensa 上加锁的 64 位原子辅助函数。
    uint64_t dma_sample_cursor_isr_ = 0;
    uint64_t dma_overflow_samples_isr_ = 0;
    uint64_t returned_samples_ = 0;  // 仅读取消费者访问。
    std::atomic<uint32_t> timing_sequence_{0};
    std::atomic<uint32_t> timing_enabled_{0};
    std::atomic<uint32_t> dma_completed_blocks_{0};
    std::atomic<uint32_t> dma_completed_samples_low_{0}, dma_completed_samples_high_{0};
    std::atomic<uint32_t> dma_completion_us_low_{0}, dma_completion_us_high_{0};
    std::atomic<uint32_t> dma_overflow_events_{0};
    std::atomic<uint32_t> dma_overflow_samples_low_{0}, dma_overflow_samples_high_{0};
    std::atomic<uint32_t> dma_invalid_events_{0};
    std::atomic<uint32_t> dma_timeline_valid_{0};
    std::atomic<uint32_t> read_timeline_valid_{0};
    // release 发布标记置位后保持不变，仅在 DMA/读取任务停止后的受控初始化中清除。
    // 独立发布机制避免向上面仅由 ISR 写入的时序序列加入任务写者。
    std::atomic<uint32_t> capture_started_{0}, first_read_started_{0};
    std::atomic<uint32_t> capture_start_us_low_{0}, capture_start_us_high_{0};
    std::atomic<uint32_t> first_read_us_low_{0}, first_read_us_high_{0};
    // 首次溢出由 ISR 持有，并包含在时序序号快照中。
    std::atomic<uint32_t> first_overflow_us_low_{0}, first_overflow_us_high_{0};
    std::atomic<uint32_t> first_overflow_sample_low_{0}, first_overflow_sample_high_{0};
    // 独立的单任务写者发布机制，使 ISR 的 timing_sequence_ 保持单写者。
    // 每个共享字段都是内部 RAM 中的无锁 uint32_t；
    // ISR 永不等待此序号变为偶数。
    ServiceMetrics service_task_{};
    std::atomic<uint32_t> service_sequence_{0};
    std::atomic<uint32_t> service_words_[kServiceWordCount]{};
    std::atomic<uint32_t> first_overflow_service_coherent_{0};
    std::atomic<uint32_t> first_overflow_service_words_[kServiceWordCount]{};
};
