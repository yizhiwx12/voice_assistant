#pragma once
#include <cstdint>
bool esp_gpio_is_reserved(uint64_t);
uint64_t esp_gpio_revoke(uint64_t);
