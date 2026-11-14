#pragma once
#include "display/st7789_display.hpp"
#include "network/wifi_station.hpp"
#include "app/voice_assistant_status.hpp"
#include "test_device_state.hpp"
class VoiceAssistantUi {
public:
    esp_err_t init(St7789Display &) { return ESP_OK; }
    esp_err_t render(const VoiceAssistantStatus &, const WifiStation &)
    {
        ++device_test::state.renders;
        device_test::state.render_times.push_back(device_test::state.now_ms);
        device_test::advanceTime(device_test::state.render_delay_ms);
        if (device_test::state.render_timeouts_remaining) {
            --device_test::state.render_timeouts_remaining;
            return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }
    bool command(char value)
    {
        device_test::state.events.emplace_back(value, device_test::state.now_ms);
        return value == '[' || value == ']';
    }
};
