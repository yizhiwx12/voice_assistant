#pragma once
#include "test_device_state.hpp"
#define ESP_LOGI(tag, ...) device_test::log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) device_test::log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) device_test::log(tag, __VA_ARGS__)
