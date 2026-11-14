#pragma once

#include "button_gestures.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"

#include <array>
#include <atomic>

class LocalButtons {
public:
    LocalButtons() = default;
    ~LocalButtons();
    LocalButtons(const LocalButtons &) = delete;
    LocalButtons &operator=(const LocalButtons &) = delete;

    struct Config {
        // 确认实际接线之前不占用任何引脚。
        gpio_num_t first_gpio = GPIO_NUM_NC;
        gpio_num_t second_gpio = GPIO_NUM_NC;
        bool active_low = true;
    };

    esp_err_t init(const Config &config, uint32_t now_ms);
    // 取出一个按键事件。采样每 10 毫秒独立运行，界面或控制器周期变慢也不会丢掉已缓存点击。
    // now_ms 参数保留给现有调用者兼容使用。
    uint8_t poll(uint32_t now_ms);
    bool enabled() const { return initialized_ && pin_mask_ != 0; }
    uint32_t dropped_events() const { return dropped_events_.load(std::memory_order_relaxed); }

    static constexpr uint32_t kSampleMs = 10;
    static constexpr uint32_t kEventCapacity = 16;

private:
    static void timer_callback(void *context);
    void sample(uint32_t now_ms);
    bool pressed(gpio_num_t pin) const;
    Config config_{};
    voice::ButtonGestures gestures_{};
    uint64_t pin_mask_ = 0;
    bool initialized_ = false;
    esp_timer_handle_t timer_ = nullptr;
    // 一个定时器生产事件，一个应用循环消费事件。队列溢出时丢弃最新事件，
    // 保留较早的用户操作，并累计丢失次数。
    std::array<uint8_t, kEventCapacity> events_{};
    std::atomic<uint32_t> write_position_{0};
    std::atomic<uint32_t> read_position_{0};
    std::atomic<uint32_t> dropped_events_{0};
    bool last_first_pressed_ = false;
    bool last_second_pressed_ = false;
    std::atomic<uint8_t> level_snapshot_{0xFF};
    std::atomic<uint32_t> level_log_pending_{0};
};
