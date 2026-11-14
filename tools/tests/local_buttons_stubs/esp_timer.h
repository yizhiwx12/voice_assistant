#pragma once

#include <cstdint>
#include "esp_err.h"

using esp_timer_cb_t = void (*)(void *);
struct esp_timer;
using esp_timer_handle_t = esp_timer *;
enum esp_timer_dispatch_t { ESP_TIMER_TASK };
struct esp_timer_create_args_t {
    esp_timer_cb_t callback = nullptr;
    void *arg = nullptr;
    esp_timer_dispatch_t dispatch_method = ESP_TIMER_TASK;
    const char *name = nullptr;
    bool skip_unhandled_events = false;
};
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t);
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t, uint32_t);
esp_err_t esp_timer_delete(esp_timer_handle_t);
int64_t esp_timer_get_time();
void esp_timer_test_set_time(uint32_t now_ms);
