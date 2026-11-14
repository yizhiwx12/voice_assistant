#include "memory_diagnostics.hpp"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace voice::memory {
namespace {
constexpr char kTag[] = "memory";
struct Failure {
    size_t bytes = 0;
    uint32_t caps = 0;
    std::array<char, 48> function{};
};
struct State {
    bool installed = false;
    bool first_failure_ready = false;
    uint32_t failure_count = 0;
    uint32_t last_logged_failures = 0;
    Failure first_failure;
};
// Xtensa 针对 PSRAM 的原子操作兼容逻辑使 std::atomic<T>::is_always_lock_free
// 为 false。使用私有、静态初始化的 ESP-IDF 临界区锁，
// 不依赖编译器的通用原子运行库。锁和状态均保留在内部 DRAM。
// 锁内不分配内存、不查询堆、不打印日志；
// 每次临界区只复制固定大小状态，或至多 47 个名称字节。
DRAM_ATTR portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR State state;

class StateGuard {
public:
    StateGuard() { portENTER_CRITICAL_SAFE(&state_lock); }
    ~StateGuard() { portEXIT_CRITICAL_SAFE(&state_lock); }
    StateGuard(const StateGuard &) = delete;
    StateGuard &operator=(const StateGuard &) = delete;
};

void allocationFailed(size_t bytes, uint32_t caps, const char *function)
{
    const StateGuard guard;
    ++state.failure_count;
    if (state.first_failure_ready) return;
    state.first_failure.bytes = bytes;
    state.first_failure.caps = caps;
    // 低内存回调中不保留调用者拥有的指针，不调用 printf，也不分配字符串。
    // 只复制有长度上限的名称，并在最后发布完成标志。
    size_t position = 0;
    if (function) {
        while (position + 1 < state.first_failure.function.size() && function[position]) {
            state.first_failure.function[position] = function[position];
            ++position;
        }
    }
    state.first_failure.function[position] = '\0';
    state.first_failure_ready = true;
}

Region region(uint32_t caps)
{
    multi_heap_info_t info{};
    heap_caps_get_info(&info, caps);
    return {info.total_free_bytes, info.minimum_free_bytes, info.largest_free_block};
}
}  // 匿名命名空间结束

esp_err_t install()
{
    {
        const StateGuard guard;
        if (state.installed) return ESP_OK;
    }
    const esp_err_t result = heap_caps_register_failed_alloc_callback(allocationFailed);
    if (result == ESP_OK) {
        const StateGuard guard;
        state.installed = true;
    }
    return result;
}

Snapshot capture()
{
    Snapshot value;
    value.internal = region(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    value.dma = region(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    value.psram = region(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    {
        const StateGuard guard;
        value.failure_hook_installed = state.installed;
        value.failed_allocations = state.failure_count;
        if (state.first_failure_ready) {
            value.first_failure_ready = true;
            value.first_failed_bytes = state.first_failure.bytes;
            value.first_failed_caps = state.first_failure.caps;
            value.first_failed_function = state.first_failure.function;
        }
    }
    return value;
}

void log(const char *stage)
{
    const Snapshot value = capture();
    ESP_LOGI(kTag, "stage=%s internal_free=%zu internal_min=%zu internal_largest=%zu "
             "dma_free=%zu dma_min=%zu dma_largest=%zu psram_free=%zu psram_min=%zu "
             "psram_largest=%zu alloc_failures=%u hook=%u",
             stage ? stage : "unknown", value.internal.free_bytes,
             value.internal.minimum_free_bytes, value.internal.largest_free_block,
             value.dma.free_bytes, value.dma.minimum_free_bytes, value.dma.largest_free_block,
             value.psram.free_bytes, value.psram.minimum_free_bytes, value.psram.largest_free_block,
             static_cast<unsigned>(value.failed_allocations),
             static_cast<unsigned>(value.failure_hook_installed));
    // 只在普通任务中报告失败。
    // 打印期间若再次分配失败，钩子会递增计数，并在下一阶段报告。
    bool report_failure = false;
    {
        const StateGuard guard;
        if (value.first_failure_ready && state.last_logged_failures != value.failed_allocations) {
            state.last_logged_failures = value.failed_allocations;
            report_failure = true;
        }
    }
    if (report_failure) {
        ESP_LOGE(kTag, "allocation_failure count=%u first_bytes=%zu first_caps=0x%x first_function=%s",
                 static_cast<unsigned>(value.failed_allocations), value.first_failed_bytes,
                 static_cast<unsigned>(value.first_failed_caps), value.first_failed_function.data());
    }
}

bool hasInternalBudget(const Snapshot &snapshot, size_t required_free,
                       size_t required_largest)
{
    return snapshot.internal.free_bytes >= required_free &&
           snapshot.internal.largest_free_block >= required_largest;
}

}  // 命名空间 voice::memory 结束
