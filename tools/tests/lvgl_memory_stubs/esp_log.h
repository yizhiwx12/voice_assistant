#pragma once

void stub_lvgl_log(const char *level, const char *tag, const char *format, ...);
#define ESP_LOGI(tag, ...) stub_lvgl_log("I", tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) stub_lvgl_log("E", tag, __VA_ARGS__)
