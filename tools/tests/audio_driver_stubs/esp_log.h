#pragma once
template<typename... Args> inline void mock_log(Args...) {}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
