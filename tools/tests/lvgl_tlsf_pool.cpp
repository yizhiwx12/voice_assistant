// 使用真实 LVGL 内存/TLSF/链表实现，仅模拟 ESP 堆。
#include "src/core/lv_global.h"
#include "src/stdlib/lv_mem.h"
#include "display/lvgl_memory.h"
#include "esp_heap_caps.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

lv_global_t lv_global{};
namespace {
alignas(16) unsigned char external_pool[65536];
unsigned allocations = 0;
}

void *heap_caps_malloc(size_t bytes, unsigned capabilities)
{
    assert(bytes == sizeof(external_pool));
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations;
    return external_pool;
}

void stub_lvgl_log(const char *, const char *, const char *, ...) {}

int main()
{
    lv_global.memory_zero = ZERO_MEM_SENTINEL;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        lv_mem_init();
        auto *text = static_cast<unsigned char *>(lv_malloc(8192));
        assert(text >= external_pool && text + 8192 <= external_pool + sizeof(external_pool));
        std::memset(text, 0x53, 8192);
        text = static_cast<unsigned char *>(lv_realloc(text, 16384));
        assert(text && text[0] == 0x53 && text[8191] == 0x53);
        lv_free(text);
        assert(lv_malloc(65536) == nullptr); // 内存池保持有界，不回退到内部内存。
        lv_mem_monitor_t monitor{};
        lv_mem_monitor(&monitor);
        assert(monitor.free_size > 50000 && monitor.free_size < sizeof(external_pool));
        assert(lv_mem_test() == LV_RESULT_OK);
        lv_mem_deinit();
    }
    assert(allocations == 1);
    std::puts("Real LVGL TLSF: 100 init/allocate/reallocate/exhaust/free/deinit cycles reused one PSRAM pool");
}
