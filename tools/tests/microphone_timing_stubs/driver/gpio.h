#pragma once
#include "esp_err.h"
using gpio_num_t = int;
constexpr int GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6;
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0 && (pin) < 49)
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
