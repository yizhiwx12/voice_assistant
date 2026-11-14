#include "app/local_buttons.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct esp_timer {
    esp_timer_cb_t callback = nullptr;
    void *arg = nullptr;
    uint64_t period_us = 0;
    bool active = false;
};
namespace {
std::vector<esp_timer *> timers;
uint64_t timer_now_us = 0;
esp_err_t timer_create_result = ESP_OK, timer_start_result = ESP_OK;
esp_timer_create_args_t last_timer_config{};
unsigned timer_stops = 0, timer_deletes = 0;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    if (!args || !args->callback || !out) return ESP_ERR_INVALID_ARG;
    if (timer_create_result != ESP_OK) return timer_create_result;
    last_timer_config = *args;
    auto *timer = new esp_timer{args->callback, args->arg, 0, false};
    timers.push_back(timer);
    *out = timer;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    if (!timer || !period_us || timer->active) return ESP_ERR_INVALID_STATE;
    if (timer_start_result != ESP_OK) return timer_start_result;
    timer->period_us = period_us;
    timer->active = true;
    return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    ++timer_stops;
    timer->active = false;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    ++timer_deletes;
    timer->active = false;
    timers.erase(std::remove(timers.begin(), timers.end(), timer), timers.end());
    delete timer;
    return ESP_OK;
}
int64_t esp_timer_get_time() { return static_cast<int64_t>(timer_now_us); }
void esp_timer_test_set_time(uint32_t now_ms)
{
    timer_now_us = static_cast<uint64_t>(now_ms) * 1000ULL;
    for (auto *timer : timers)
        if (timer->active) timer->callback(timer->arg);
}

namespace {
unsigned checks = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "Local buttons check failed: %s\n", message);
        std::exit(1);
    }
}
uint8_t value(voice::ButtonEvent event) { return static_cast<uint8_t>(event); }
using Event = voice::ButtonEvent;
using Gestures = voice::ButtonGestures;
gpio_config_t last_gpio{};
unsigned configurations = 0;
uint64_t reserved = 0;
esp_err_t configuration_result = ESP_OK;
std::array<int, 49> levels{};
void resetGpio()
{
    levels.fill(1);
    configurations = 0;
    reserved = 0;
    configuration_result = ESP_OK;
    last_gpio = {};
}
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    ++configurations;
    last_gpio = *config;
    return configuration_result;
}
int gpio_get_level(gpio_num_t pin)
{
    check(pin >= 0 && pin < 49, "disabled pins must never be sampled");
    return levels[pin];
}
bool esp_gpio_is_reserved(uint64_t pins) { return (reserved & pins) != 0; }

int main()
{
    Gestures button;
    button.reset(false, false, 0);
    check(button.poll(true, false, 10) == 0, "contact closes without immediate action");
    check(button.poll(false, false, 20) == 0, "press bounce is discarded");
    check(button.poll(true, false, 30) == 0, "second edge starts fresh debounce");
    check(button.poll(true, false, 59) == 0, "press below debounce boundary");
    check(button.poll(true, false, 60) == 0, "stable press waits for release");
    check(button.poll(false, false, 80) == 0, "release waits for debounce");
    check(button.poll(true, false, 90) == 0, "release bounce retains current gesture");
    check(button.poll(false, false, 100) == 0, "new release debounce");
    check(button.poll(false, false, 130) == value(Event::FirstShort), "one short event after stable release");
    check(button.poll(false, false, 200) == 0, "short event does not repeat");

    button.reset(false, false, 0);
    button.poll(false, true, 10);
    button.poll(false, true, 40);
    check(button.poll(false, true, 1039) == 0, "long press below boundary");
    check(button.poll(false, true, 1040) == value(Event::SecondLong), "long event at one second");
    check(button.poll(false, true, 4000) == 0, "holding never repeats long event");
    button.poll(false, false, 4010);
    check(button.poll(false, false, 4040) == 0, "long release never emits short event");

    button.reset(false, false, 0);
    button.poll(true, false, 0);
    button.poll(true, false, 30);
    button.poll(false, false, 1020);
    check(button.poll(false, false, 1050) == value(Event::FirstShort), "tentative release cannot cause delayed long press");

    button.reset(true, true, 0);
    check(button.poll(true, true, 5000) == 0, "startup held contacts never act");
    button.poll(false, true, 5010);
    check(button.poll(false, true, 5040) == 0, "startup held release never emits short");
    button.poll(false, false, 5050);
    check(button.poll(false, false, 5080) == 0, "second startup release remains silent");
    button.poll(true, false, 5100);
    button.poll(true, false, 5130);
    button.poll(false, false, 5200);
    check(button.poll(false, false, 5230) == value(Event::FirstShort), "new press works after startup release");

    button.reset(false, false, 0);
    button.poll(true, false, 0);
    button.poll(true, false, 30);
    button.poll(true, true, 100);
    check(button.poll(true, true, 1130) == 0, "overlap suppresses both long actions");
    button.poll(false, true, 1200);
    check(button.poll(false, true, 1230) == 0, "overlap release suppresses first short");
    button.poll(false, false, 1240);
    check(button.poll(false, false, 1270) == 0, "overlap release suppresses second short");
    button.poll(false, true, 1300);
    button.poll(false, true, 1330);
    button.poll(false, false, 1400);
    check(button.poll(false, false, 1430) == value(Event::SecondShort), "both buttons rearm after overlap");

    button.reset(false, false, 0);
    button.poll(true, false, 0);
    button.poll(true, false, 30);
    button.poll(true, true, 40);
    button.poll(true, false, 50); // 第二次触点动作始终未形成稳定按下。
    button.poll(false, false, 60);
    check(button.poll(false, false, 90) == 0, "bouncing overlap suppresses current press");
    button.poll(false, true, 100);
    button.poll(false, true, 130);
    button.poll(false, false, 150);
    check(button.poll(false, false, 180) == value(Event::SecondShort), "bouncing overlap still rearms released contact");

    constexpr uint32_t near_wrap = UINT32_MAX - 20;
    button.reset(false, false, near_wrap - 5);
    button.poll(true, false, near_wrap);
    button.poll(true, false, 9);
    check(button.poll(true, false, 1009) == value(Event::FirstLong), "clock wrap preserves debounce and long threshold");

    resetGpio();
    LocalButtons disabled;
    check(disabled.init({}, 0) == ESP_OK && !disabled.enabled(), "unconfirmed default pins stay disabled");
    check(disabled.poll(1000) == 0 && configurations == 0, "disabled polling claims no GPIO");
    check(disabled.init({}, 0) == ESP_ERR_INVALID_STATE, "double initialization rejected");

    LocalButtons duplicate;
    check(duplicate.init({7, 7, true}, 0) == ESP_ERR_INVALID_ARG && configurations == 0, "same GPIO rejected before configuration");
    LocalButtons invalid;
    check(invalid.init({49, -1, true}, 0) == ESP_ERR_INVALID_ARG, "invalid GPIO rejected");
    reserved = 1ULL << 7;
    LocalButtons conflict;
    check(conflict.init({7, 18, true}, 0) == ESP_ERR_INVALID_STATE && configurations == 0, "peripheral ownership conflict changes neither pin");
    reserved = 0;
    configuration_result = ESP_FAIL;
    LocalButtons failed;
    check(failed.init({7, 18, true}, 0) == ESP_FAIL && !failed.enabled(), "GPIO failure leaves input disabled");
    configuration_result = ESP_OK;
    check(failed.init({7, 18, true}, 0) == ESP_OK, "initialization failure is retryable");
    check(last_gpio.pin_bit_mask == ((1ULL << 7) | (1ULL << 18)) && last_gpio.mode == GPIO_MODE_INPUT &&
          last_gpio.pull_up_en == GPIO_PULLUP_ENABLE && last_gpio.pull_down_en == GPIO_PULLDOWN_DISABLE &&
          last_gpio.intr_type == GPIO_INTR_DISABLE, "active-low contacts configure input pullups without interrupt");
    levels[7] = 0;
    failed.poll(10);
    esp_timer_test_set_time(10);
    esp_timer_test_set_time(40);
    levels[7] = 1;
    esp_timer_test_set_time(100);
    esp_timer_test_set_time(130);
    check(failed.poll(130) == value(Event::FirstShort), "active-low GPIO samples reach the first action");

    // 采样由定时器驱动：UI/控制器忙碌后，延迟的应用消费者
    // 仍能收到完整释放手势。
    resetGpio();
    LocalButtons buffered;
    check(buffered.init({7, -1, true}, 0) == ESP_OK, "buffered button initializes");
    levels[7] = 0;
    esp_timer_test_set_time(10);
    esp_timer_test_set_time(40);
    esp_timer_test_set_time(100);
    levels[7] = 1;
    esp_timer_test_set_time(130);
    esp_timer_test_set_time(160);
    check(buffered.poll(5000) == value(Event::FirstShort), "timer scan preserves click during consumer stall");
    check(buffered.poll(5000) == 0, "buffered click is consumed once");
    check(last_timer_config.dispatch_method == ESP_TIMER_TASK && last_timer_config.skip_unhandled_events,
          "sampling uses short task callback and discards delayed catchup ticks");

    resetGpio();
    LocalButtons fifo;
    check(fifo.init({7, 3, true}, 10000) == ESP_OK, "two-button queue initializes");
    const auto click = [&](gpio_num_t pin, uint32_t start) {
        levels[pin] = 0;
        esp_timer_test_set_time(start);
        esp_timer_test_set_time(start + 30);
        levels[pin] = 1;
        esp_timer_test_set_time(start + 80);
        esp_timer_test_set_time(start + 110);
    };
    click(7, 10100);
    click(3, 10300);
    click(3, 10500);
    check(fifo.poll(12000) == value(Event::FirstShort) && fifo.poll(12000) == value(Event::SecondShort) &&
          fifo.poll(12000) == value(Event::SecondShort) && fifo.poll(12000) == 0,
          "queue preserves temporal order and repeated page clicks while consumer is busy");
    for (uint32_t i = 0; i < LocalButtons::kEventCapacity + 3; ++i) click(3, 13000 + i * 200);
    check(fifo.dropped_events() == 3, "full queue counts and drops newest events without overwriting earlier intent");
    for (uint32_t i = 0; i < LocalButtons::kEventCapacity; ++i)
        check(fifo.poll(20000) == value(Event::SecondShort), "bounded queue remains intact after overflow");
    check(fifo.poll(20000) == 0, "bounded queue drains completely");
    click(7, 21000);
    check(fifo.poll(22000) == value(Event::FirstShort), "sampling resumes normally after full queue drains");

    timer_create_result = ESP_ERR_NO_MEM;
    LocalButtons no_timer;
    check(no_timer.init({7, -1, true}, 22000) == ESP_ERR_NO_MEM && !no_timer.enabled(),
          "timer creation failure leaves driver disabled and reports error");
    timer_create_result = ESP_OK;
    timer_start_result = ESP_FAIL;
    const unsigned deleted_before = timer_deletes;
    check(no_timer.init({7, -1, true}, 22000) == ESP_FAIL && !no_timer.enabled() && timer_deletes == deleted_before + 1,
          "timer start failure deletes unused handle and keeps initialization retryable");
    timer_start_result = ESP_OK;
    check(no_timer.init({7, -1, true}, 22000) == ESP_OK, "timer failure can be retried successfully");
    const unsigned stopped_before = timer_stops, deleted_before_scope = timer_deletes;
    {
        LocalButtons lifetime;
        check(lifetime.init({7, -1, true}, 22000) == ESP_OK, "scoped button driver initializes");
    }
    check(timer_stops == stopped_before + 1 && timer_deletes == deleted_before_scope + 1,
          "destruction waits for in-flight callbacks before deleting timer handle");

    resetGpio();
    levels[18] = 0;
    LocalButtons high;
    check(high.init({-1, 18, false}, 0) == ESP_OK && high.enabled(), "one configured high-active contact supported");
    check(last_gpio.pull_up_en == GPIO_PULLUP_DISABLE && last_gpio.pull_down_en == GPIO_PULLDOWN_ENABLE,
          "high-active contact configures pulldown");
    levels[18] = 1;
    esp_timer_test_set_time(10);
    esp_timer_test_set_time(40);
    esp_timer_test_set_time(1040);
    check(high.poll(1040) == value(Event::SecondLong), "active-high GPIO samples reach second long action");
    std::printf("Local buttons: %u checks passed (GPIO stubs; physical wiring pending)\n", checks);
}
