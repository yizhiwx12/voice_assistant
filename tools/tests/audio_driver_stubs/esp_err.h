#pragma once
#include "../firmware_stubs/esp_err.h"
#define ESP_ERR_TIMEOUT 0x107
inline const char *esp_err_to_name(esp_err_t) { return "mock_error"; }
