#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
class Max98357aAudio {
public:
    esp_err_t init() { return ESP_OK; }
    esp_err_t deinit() { return ESP_OK; }
    uint32_t sampleRateHz() const { return 16000; }
    esp_err_t writeMono(const int16_t *, size_t) { return ESP_OK; }
};
