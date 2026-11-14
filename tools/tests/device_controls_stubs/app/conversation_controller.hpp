#pragma once
#include "app/voice_assistant_status.hpp"
#include "network/wifi_station.hpp"
#include "test_device_state.hpp"
class ConversationController {
public:
    esp_err_t start(VoiceAssistantStatus &status, WifiStation &)
    {
        ++device_test::state.conversation_starts;
        status.enableRealtime();
        status.setSession(VoiceSessionState::Ready);
        return ESP_OK;
    }
    void localAction()
    {
        ++device_test::state.action_calls;
        device_test::state.events.emplace_back('A', device_test::state.now_ms);
    }
    void retryConnection()
    {
        ++device_test::state.retry_calls;
        device_test::state.events.emplace_back('R', device_test::state.now_ms);
    }
    void command(char value)
    {
        device_test::state.events.emplace_back(value, device_test::state.now_ms);
    }
};
