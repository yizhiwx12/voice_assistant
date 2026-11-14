#pragma once
#include "freertos/FreeRTOS.h"
#include "test_device_state.hpp"
inline unsigned uxTaskGetStackHighWaterMark(void *) { return 8192; }
inline TickType_t xTaskGetTickCount() { return device_test::state.now_ms; }
inline void vTaskDelay(TickType_t ticks)
{
    device_test::advanceTime(ticks);
    if (device_test::state.running) {
        ++device_test::state.loop_delays;
        if (device_test::state.now_ms >= device_test::state.stop_at_ms) throw device_test::StopLoop{};
    }
}
inline void vTaskDelayUntil(TickType_t *previous, TickType_t increment)
{
    *previous += increment;
    const TickType_t now = xTaskGetTickCount();
    vTaskDelay(static_cast<int32_t>(*previous - now) > 0 ? *previous - now : 0);
}
