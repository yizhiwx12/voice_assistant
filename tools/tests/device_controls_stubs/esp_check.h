#pragma once
#include <stdexcept>
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { (void)(tag); const auto result = (call); if (result != ESP_OK) return result; } while (0)
#define ESP_ERROR_CHECK(call) do { if ((call) != ESP_OK) throw std::runtime_error("ESP_ERROR_CHECK"); } while (0)
