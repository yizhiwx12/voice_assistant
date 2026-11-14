#include "app/voice_assistant_app.hpp"
#include "test_device_state.hpp"
#include "driver/gpio.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

struct esp_timer {
    esp_timer_cb_t callback;
    void *arg;
    uint32_t period_ms = 0, next_ms = 0;
    bool active = false;
};

namespace {
unsigned checks = 0;
std::vector<esp_timer *> timers;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "Device controls check failed: %s\n", message);
        std::exit(1);
    }
}
void runFor(VoiceAssistantApp &app, uint32_t milliseconds)
{
    device_test::state.stop_at_ms = device_test::state.now_ms + milliseconds;
    device_test::state.running = true;
    try { app.run(); }
    catch (const device_test::StopLoop &) {}
    device_test::state.running = false;
}
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    if (!args || !args->callback || !out) return ESP_ERR_INVALID_ARG;
    auto *timer = new esp_timer{args->callback, args->arg};
    timers.push_back(timer);
    *out = timer;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    if (!timer || timer->active || !period_us || period_us % 1000) return ESP_ERR_INVALID_ARG;
    timer->period_ms = static_cast<uint32_t>(period_us / 1000);
    timer->next_ms = device_test::state.now_ms + timer->period_ms;
    timer->active = true;
    return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->active = false;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    timers.erase(std::remove(timers.begin(), timers.end(), timer), timers.end());
    delete timer;
    return ESP_OK;
}
int64_t esp_timer_get_time() { return static_cast<int64_t>(device_test::state.now_ms) * 1000; }
void device_test::advanceTime(uint32_t milliseconds)
{
    for (uint32_t i = 0; i < milliseconds; ++i) {
        ++state.now_ms;
        for (auto *timer : timers) {
            if (timer->active && state.now_ms >= timer->next_ms) {
                timer->next_ms += timer->period_ms;
                timer->callback(timer->arg);
            }
        }
    }
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    ++device_test::state.gpio_configurations;
    device_test::state.gpio_mask = config->pin_bit_mask;
    device_test::state.gpio_pullup = config->pull_up_en;
    device_test::state.gpio_pulldown = config->pull_down_en;
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    check(pin == 7 || pin == 3, "only confirmed button GPIOs are sampled");
    for (const auto &press : device_test::state.presses)
        if (press[0] == static_cast<uint32_t>(pin) &&
            device_test::state.now_ms >= press[1] && device_test::state.now_ms < press[2]) return 0;
    return 1;
}
bool esp_gpio_is_reserved(uint64_t) { return false; }

int main()
{
    device_test::reset();
#if defined(DEVICE_TEST_INVALID_BUTTON_GPIO)
    VoiceAssistantApp invalid;
    check(invalid.init() == ESP_ERR_INVALID_ARG && device_test::state.gpio_configurations == 0,
          "peripheral GPIO map is rejected before button gpio_config");
    std::printf("Device controls invalid GPIO guard passed (%u assertions)\n", checks);
    return 0;
#endif
    {
    VoiceAssistantApp app;
    check(app.init() == ESP_OK, "default standalone controls initialize");
    auto &state = device_test::state;
    check(state.gpio_configurations == 1 && state.gpio_mask == ((1ULL << 7) | (1ULL << 3)),
          "confirmed GPIO7/GPIO3 configured together");
    check(state.gpio_pullup == GPIO_PULLUP_ENABLE && state.gpio_pulldown == GPIO_PULLDOWN_DISABLE,
          "confirmed low-active contacts use input pullups");
    check(state.conversation_starts == 1, "connected boot initializes conversation once");
    const uint32_t origin = state.now_ms;
    state.presses = {{7, origin + 100, origin + 200}, {7, origin + 500, origin + 1800},
                     {3, origin + 2200, origin + 2300}, {3, origin + 2600, origin + 3900},
                     {7, origin + 4200, origin + 5500}, {3, origin + 4200, origin + 5500}};
    runFor(app, 6000);
    std::vector<char> events;
    for (const auto &event : state.events) events.push_back(event.first);
    check(events == std::vector<char>{'A', 'R', ']', '['},
          "physical short/long actions route exactly once and simultaneous hold does nothing");
    check(state.action_calls == 1 && state.retry_calls == 1, "controller local action and retry each called once");
    check(state.wifi_polls == state.loop_delays && state.loop_delays == 300,
          "Wi-Fi recovery is polled each 20ms app cycle");
    check(state.renders == state.loop_delays,
          "UI observes every app cycle without adding a separate subtitle refresh throttle");
    for (size_t i = 1; i < state.render_times.size(); ++i) {
        const uint32_t at = state.render_times[i];
        check(at - state.render_times[i - 1] <= 20,
              "subtitle observation never waits longer than one input cycle during normal work");
    }
    }

    device_test::reset();
    {
    device_test::state.wifi_initially_connecting = true;
    VoiceAssistantApp pending;
    check(pending.init() == ESP_OK && device_test::state.conversation_starts == 0,
          "AEC boot defers audio during initial Wi-Fi connection");
    device_test::state.wifi_connect_at_ms = device_test::state.now_ms + 200;
    runFor(pending, 600);
    check(device_test::state.conversation_starts == 1, "Wi-Fi poll completion releases deferred audio startup once");
    check(device_test::state.renders == device_test::state.wifi_polls &&
          device_test::state.wifi_polls == 30,
          "screen and Wi-Fi progress continue during deferred startup");
    }

    // 两次完整点击都落在一次慢渲染内。旧的仅应用 GPIO 轮询会丢失它们；
    // 定时扫描必须保留这些点击。
    device_test::reset();
    {
        VoiceAssistantApp stalled;
        check(stalled.init() == ESP_OK, "slow rendering scenario initializes");
        auto &state = device_test::state;
        const uint32_t origin = state.now_ms;
        state.render_delay_ms = 400;
        state.presses = {{7, origin + 40, origin + 120}, {3, origin + 200, origin + 280}};
        runFor(stalled, 1800);
        std::vector<char> events;
        for (const auto &event : state.events) events.push_back(event.first);
        check(events == std::vector<char>{'A', ']'},
              "both short gestures survive a render that spans their complete press/release");
        check(state.action_calls == 1, "retained first-key action reaches controller exactly once");
    }

    device_test::reset();
    {
        VoiceAssistantApp contention;
        check(contention.init() == ESP_OK, "UI contention scenario initializes");
        auto &state = device_test::state;
        state.render_timeouts_remaining = 3;
        const uint32_t origin = state.now_ms;
        state.presses = {{7, origin + 40, origin + 120}, {3, origin + 180, origin + 260}};
        runFor(contention, 600);
        check(state.render_timeouts_remaining == 0 && state.renders >= 6,
              "transient LVGL contention retries rendering without fatal reset");
        check(state.action_calls == 1 && std::count_if(state.events.begin(), state.events.end(),
              [](const auto &event) { return event.first == ']'; }) == 1,
              "button dispatch continues during UI lock contention");
    }
    std::printf("Device controls: production app routing and polling checks passed (%u assertions; hardware pending)\n", checks);
}
