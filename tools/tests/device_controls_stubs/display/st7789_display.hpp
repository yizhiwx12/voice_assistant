#pragma once
#include <cstdint>
#include "esp_err.h"
class St7789Display {
public:
    esp_err_t init() { return ESP_OK; }
    esp_err_t setRotation(uint8_t) { return ESP_OK; }
    esp_err_t setBacklightBrightness(uint8_t) { return ESP_OK; }
};
