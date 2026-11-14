#pragma once
#include "esp_err.h"
namespace voice::memory {
inline esp_err_t install() { return ESP_OK; }
inline void log(const char *) {}
}
