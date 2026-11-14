#pragma once
#include <cstdint>
#include "esp_err.h"
using gpio_num_t = int;
inline constexpr int GPIO_NUM_NC = -1;
inline constexpr int GPIO_NUM_3 = 3, GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6;
inline constexpr int GPIO_NUM_7 = 7, GPIO_NUM_8 = 8, GPIO_NUM_9 = 9, GPIO_NUM_10 = 10;
inline constexpr int GPIO_NUM_11 = 11, GPIO_NUM_12 = 12, GPIO_NUM_13 = 13, GPIO_NUM_14 = 14;
inline constexpr int GPIO_NUM_15 = 15, GPIO_NUM_16 = 16, GPIO_NUM_17 = 17, GPIO_NUM_18 = 18;
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
inline constexpr int GPIO_MODE_INPUT = 1;
inline constexpr int GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1;
inline constexpr int GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1;
inline constexpr int GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
    uint64_t pin_bit_mask = 0;
    int mode = 0, pull_up_en = 0, pull_down_en = 0, intr_type = 0;
};
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(gpio_num_t);
