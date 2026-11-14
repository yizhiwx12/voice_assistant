#include "local_buttons.hpp"

#include "esp_private/esp_gpio_reserve.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "local_buttons";
}

LocalButtons::~LocalButtons()
{
    if (timer_) {
        // IDF 6.1 单独调用 stop() 不会等待已派发的回调。
        // 本对象只在所属用户任务中销毁，不在定时器上下文销毁。
        esp_timer_stop_blocking(timer_, UINT32_MAX);
        esp_timer_delete(timer_);
    }
}

esp_err_t LocalButtons::init(const Config &config, uint32_t now_ms)
{
    if (initialized_) return ESP_ERR_INVALID_STATE;
    if ((config.first_gpio != GPIO_NUM_NC && !GPIO_IS_VALID_GPIO(config.first_gpio)) ||
        (config.second_gpio != GPIO_NUM_NC && !GPIO_IS_VALID_GPIO(config.second_gpio)) ||
        (config.first_gpio != GPIO_NUM_NC && config.first_gpio == config.second_gpio))
        return ESP_ERR_INVALID_ARG;
    uint64_t pins = 0;
    if (config.first_gpio != GPIO_NUM_NC) pins |= 1ULL << config.first_gpio;
    if (config.second_gpio != GPIO_NUM_NC) pins |= 1ULL << config.second_gpio;
    // IDF 6.1 的 gpio_config() 只警告，仍可能改写其他模块占用的引脚。
    // 修改任一按键输入前，先拒绝已被外设占用的引脚。
    if (pins && esp_gpio_is_reserved(pins)) return ESP_ERR_INVALID_STATE;
    if (pins) {
        gpio_config_t input{};
        input.pin_bit_mask = pins;
        input.mode = GPIO_MODE_INPUT;
        input.pull_up_en = config.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        input.pull_down_en = config.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
        input.intr_type = GPIO_INTR_DISABLE;
        const esp_err_t result = gpio_config(&input);
        if (result != ESP_OK) return result;
    }
    config_ = config;
    pin_mask_ = pins;
    last_first_pressed_ = pressed(config_.first_gpio);
    last_second_pressed_ = pressed(config_.second_gpio);
    level_snapshot_.store(static_cast<uint8_t>((last_first_pressed_ ? 0 : 1) |
                                               (last_second_pressed_ ? 0 : 2)),
                          std::memory_order_relaxed);
    gestures_.reset(last_first_pressed_, last_second_pressed_, now_ms);
    ESP_LOGI(kTag, "ready first_gpio=%d first_level=%d second_gpio=%d second_level=%d active_low=%u",
             static_cast<int>(config_.first_gpio), last_first_pressed_ ? 0 : 1,
             static_cast<int>(config_.second_gpio), last_second_pressed_ ? 0 : 1,
             static_cast<unsigned>(config_.active_low));
    if (pins) {
        esp_timer_create_args_t timer_config{};
        timer_config.callback = timer_callback;
        timer_config.arg = this;
        timer_config.dispatch_method = ESP_TIMER_TASK;
        timer_config.name = "local_buttons";
        // 回调延迟后，反复读取当前电平不能还原过去的触点变化。
        // 直接恢复真实采样，不进行补采。
        timer_config.skip_unhandled_events = true;
        esp_err_t result = esp_timer_create(&timer_config, &timer_);
        if (result != ESP_OK) {
            pin_mask_ = 0;
            return result;
        }
        result = esp_timer_start_periodic(timer_, kSampleMs * 1000ULL);
        if (result != ESP_OK) {
            esp_timer_delete(timer_);
            timer_ = nullptr;
            pin_mask_ = 0;
            return result;
        }
    }
    initialized_ = true;
    return ESP_OK;
}

bool LocalButtons::pressed(gpio_num_t pin) const
{
    if (pin == GPIO_NUM_NC) return false;
    const bool high = gpio_get_level(pin) != 0;
    return config_.active_low ? !high : high;
}

uint8_t LocalButtons::poll(uint32_t now_ms)
{
    (void)now_ms;
    if (!enabled()) return 0;
    if (level_log_pending_.exchange(0, std::memory_order_acquire) != 0) {
        const uint8_t levels = level_snapshot_.load(std::memory_order_relaxed);
        (void)levels;
        ESP_LOGI(kTag, "level first=%u second=%u", static_cast<unsigned>(levels & 1U),
                 static_cast<unsigned>((levels >> 1) & 1U));
    }
    const uint32_t read = read_position_.load(std::memory_order_relaxed);
    if (read == write_position_.load(std::memory_order_acquire)) return 0;
    const uint8_t event = events_[read % kEventCapacity];
    read_position_.store(read + 1, std::memory_order_release);
    ESP_LOGI(kTag, "dequeue event mask=%u first=%s second=%s",
             static_cast<unsigned>(event),
             voice::ButtonGestures::has(event, voice::ButtonEvent::FirstShort) ? "short" :
                 voice::ButtonGestures::has(event, voice::ButtonEvent::FirstLong) ? "long" : "none",
             voice::ButtonGestures::has(event, voice::ButtonEvent::SecondShort) ? "short" :
                 voice::ButtonGestures::has(event, voice::ButtonEvent::SecondLong) ? "long" : "none");
    return event;
}

void LocalButtons::timer_callback(void *context)
{
    auto *buttons = static_cast<LocalButtons *>(context);
    buttons->sample(static_cast<uint32_t>(esp_timer_get_time() / 1000));
}

void LocalButtons::sample(uint32_t now_ms)
{
    const bool first_pressed = pressed(config_.first_gpio);
    const bool second_pressed = pressed(config_.second_gpio);
    if (first_pressed != last_first_pressed_ || second_pressed != last_second_pressed_) {
        level_snapshot_.store(static_cast<uint8_t>((first_pressed ? 0 : 1) |
                                                   (second_pressed ? 0 : 2)),
                              std::memory_order_relaxed);
        level_log_pending_.store(1, std::memory_order_release);
        last_first_pressed_ = first_pressed;
        last_second_pressed_ = second_pressed;
    }
    const uint8_t event = gestures_.poll(first_pressed, second_pressed, now_ms);
    if (!event) return;
    const uint32_t write = write_position_.load(std::memory_order_relaxed);
    if (write - read_position_.load(std::memory_order_acquire) >= kEventCapacity) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    events_[write % kEventCapacity] = event;
    write_position_.store(write + 1, std::memory_order_release);
}
