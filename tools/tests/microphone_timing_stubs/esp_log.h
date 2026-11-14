#pragma once
void microphoneTimingLog();
template<typename... Args> inline void mock_log(Args...) { microphoneTimingLog(); }
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
