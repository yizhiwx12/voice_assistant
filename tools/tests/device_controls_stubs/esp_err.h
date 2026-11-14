#pragma once
using esp_err_t = int;
inline constexpr int ESP_OK = 0, ESP_FAIL = -1;
inline constexpr int ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_ARG = 0x102;
inline constexpr int ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_TIMEOUT = 0x107;
inline const char *esp_err_to_name(esp_err_t) { return "stub_error"; }
