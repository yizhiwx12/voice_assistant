#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "esp_err.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "network/voice_protocol.hpp"

class VoiceClient {
public:
    enum class Event : uint8_t { Connected, Disconnected, Text, Binary, Failure };
    using Handler = std::function<void(Event, const uint8_t *, size_t)>;
    struct Diagnostics { const char *reason = "none"; int close_code = 0; };
    inline static std::function<void(const Handler &)> start_hook;
    inline static std::function<esp_err_t(const std::string &)> text_hook;
    inline static std::function<esp_err_t(const uint8_t *, size_t)> binary_hook;
    std::vector<std::string> sent;
    size_t sent_binary = 0;
    esp_err_t start(const char *, Handler handler) { if (start_hook) start_hook(handler); return ESP_OK; }
    esp_err_t stop() { return ESP_OK; }
    esp_err_t sendText(const char *text, size_t count, uint32_t) {
        const std::string event(text, count);
        sent.push_back(event);
        return text_hook ? text_hook(event) : ESP_OK;
    }
    esp_err_t sendBinary(const uint8_t *data, size_t bytes, uint32_t) {
        ++sent_binary;
        return binary_hook ? binary_hook(data, bytes) : ESP_OK;
    }
    Diagnostics diagnostics() const { return {}; }
    void logDispatchDiagnostics(const char * = "manual") const {}
};
