#include "lvgl_memory.h"

#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "lvgl_memory";
void *pool = nullptr;
size_t pool_bytes = 0;
}  // 匿名命名空间结束

extern "C" void *voice_lvgl_pool_alloc(size_t bytes)
{
    if (bytes < 2048 || (pool && bytes != pool_bytes)) {
        ESP_LOGE(kTag, "Invalid LVGL fixed-pool size: requested=%u existing=%u",
                 static_cast<unsigned>(bytes), static_cast<unsigned>(pool_bytes));
        std::abort();
    }
    if (!pool) {
        // 普通 malloc 可能耗尽网络和 DMA 所需的内部 RAM。
        // 该内存池必须位于外部存储器，不能静默回退。
        pool = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pool) {
            ESP_LOGE(kTag, "Cannot allocate %u-byte LVGL pool in PSRAM; display initialization stopped",
                     static_cast<unsigned>(bytes));
            std::abort();
        }
        pool_bytes = bytes;
        ESP_LOGI(kTag, "LVGL fixed TLSF pool: %u bytes in PSRAM (reused across reinitialization)",
                 static_cast<unsigned>(pool_bytes));
    }
    return pool;
}
