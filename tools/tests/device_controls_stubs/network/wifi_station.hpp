#pragma once
#include <cstdint>
#include "esp_err.h"
#include "test_device_state.hpp"
class WifiStation {
public:
    enum class State : uint8_t { NotConfigured, Connecting, Connected, Failed };
    esp_err_t start()
    {
        state_ = device_test::state.wifi_initially_connecting ? State::Connecting : State::Connected;
        return ESP_OK;
    }
    void poll()
    {
        ++device_test::state.wifi_polls;
        if (state_ == State::Connecting && device_test::state.wifi_connect_at_ms &&
            device_test::state.now_ms >= device_test::state.wifi_connect_at_ms)
            state_ = State::Connected;
    }
    State state() const { return state_; }
private:
    State state_ = State::NotConfigured;
};
