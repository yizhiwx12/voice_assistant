#pragma once
void memoryTestLog(bool error, const char *format, ...) __attribute__((format(gnu_printf, 2, 3)));
#define ESP_LOGI(tag, ...) (static_cast<void>(tag), memoryTestLog(false, __VA_ARGS__))
#define ESP_LOGE(tag, ...) (static_cast<void>(tag), memoryTestLog(true, __VA_ARGS__))
