#pragma once
#include <cstdint>
#include "esp_err.h"
using gpio_num_t = int;
constexpr int GPIO_NUM_NC = -1;
constexpr int GPIO_NUM_8 = 8, GPIO_NUM_15 = 15, GPIO_NUM_16 = 16, GPIO_NUM_17 = 17;
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0 && (pin) < 49)
constexpr int GPIO_MODE_DISABLE = 0, GPIO_MODE_OUTPUT = 2;
constexpr int GPIO_PULLUP_DISABLE = 0, GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_INTR_DISABLE = 0, GPIO_PULLDOWN_ONLY = 2;
struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
};
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
esp_err_t gpio_set_pull_mode(gpio_num_t, int);
esp_err_t gpio_set_direction(gpio_num_t, int);
