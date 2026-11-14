#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
constexpr uint32_t MALLOC_CAP_INTERNAL = 1;
constexpr uint32_t MALLOC_CAP_8BIT = 2;
constexpr uint32_t MALLOC_CAP_DMA = 4;
constexpr uint32_t MALLOC_CAP_SPIRAM = 8;
struct multi_heap_info_t {
    size_t total_free_bytes;
    size_t minimum_free_bytes;
    size_t largest_free_block;
};
using esp_alloc_failed_hook_t = void (*)(size_t, uint32_t, const char *);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t);
void heap_caps_get_info(multi_heap_info_t *, uint32_t);
