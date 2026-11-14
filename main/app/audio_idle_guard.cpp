#include "audio_idle_guard.hpp"

#include <algorithm>
#include <mutex>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"

namespace voice::audio_idle_guard {
namespace {

std::once_flag g_initialize_once;
DRAM_ATTR std::atomic<bool> g_initialized{false};
DRAM_ATTR std::atomic<esp_err_t> g_init_error{ESP_ERR_INVALID_STATE};

// 这些值由 CPU0 空闲回调和两个 AEC 工作任务共同读写。
// 显式放在内部 DRAM 中，使 IDF 针对 PSRAM 的原子操作兼容逻辑走硬件路径；
// 此芯片的 C++ 全局无锁特征值可能为 false。
DRAM_ATTR std::atomic<uint32_t> g_last_idle_tick{0};
DRAM_ATTR std::atomic<uint32_t> g_idle_generation{0};
DRAM_ATTR std::atomic<uint32_t> g_max_idle_gap_ticks{0};
DRAM_ATTR std::atomic<uint32_t> g_cooperative_waits{0};
DRAM_ATTR std::atomic<uint32_t> g_cooperative_wait_ticks{0};
DRAM_ATTR std::atomic<uint32_t> g_max_cooperative_wait_ticks{0};
DRAM_ATTR std::atomic<uint32_t> g_cancelled_waits{0};
DRAM_ATTR std::atomic<uint32_t> g_timed_out_waits{0};

void observeMaximum(std::atomic<uint32_t> &target, uint32_t value)
{
    uint32_t previous = target.load(std::memory_order_relaxed);
    while (value > previous && !target.compare_exchange_weak(
        previous, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

// 由 CPU0 上的 esp_vApplicationIdleHook() 调用。
// 回调只能执行原子操作，空闲钩子不得阻塞或进行输入输出。
bool idleHook()
{
    const uint32_t now = xTaskGetTickCount();
    g_last_idle_tick.store(now, std::memory_order_release);
    g_idle_generation.fetch_add(1, std::memory_order_release);
    return true;
}

}  // 匿名命名空间结束

esp_err_t initialize()
{
    std::call_once(g_initialize_once, [] {
        const uint32_t now = xTaskGetTickCount();
        g_last_idle_tick.store(now, std::memory_order_relaxed);
        const esp_err_t result = esp_register_freertos_idle_hook_for_cpu(idleHook, 0);
        g_init_error.store(result, std::memory_order_release);
        if (result == ESP_OK) g_initialized.store(true, std::memory_order_release);
    });
    return g_init_error.load(std::memory_order_acquire);
}

bool cooperate(const std::atomic<bool> &lifecycle, bool active_value,
               uint32_t max_wait_ms)
{
    if (lifecycle.load(std::memory_order_acquire) != active_value) {
        g_cancelled_waits.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_initialized.load(std::memory_order_acquire)) return false;
    if (xPortGetCoreID() != 0) return true;

    const TickType_t trigger_ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(50));
    const TickType_t slice_ticks = 2;
    const TickType_t max_wait_ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(max_wait_ms));
    const TickType_t started = xTaskGetTickCount();
    const uint32_t observed_generation = g_idle_generation.load(std::memory_order_acquire);
    const auto progressed = [&] {
        const uint32_t generation = g_idle_generation.load(std::memory_order_acquire);
        const uint32_t last = g_last_idle_tick.load(std::memory_order_acquire);
        // 先读取已发布的空闲时钟，再取当前时间；即使此处被空闲任务抢占，
        // 也只会让 last 显得更早，不会让它像来自未来。
        const uint32_t now = xTaskGetTickCount();
        observeMaximum(g_max_idle_gap_ticks, now - last);
        return generation != observed_generation || now - last < trigger_ticks;
    };

    if (progressed()) return true;
    g_cooperative_waits.fetch_add(1, std::memory_order_relaxed);

    for (;;) {
        if (lifecycle.load(std::memory_order_acquire) != active_value) {
            g_cancelled_waits.fetch_add(1, std::memory_order_relaxed);
            const uint32_t waited = xTaskGetTickCount() - started;
            g_cooperative_wait_ticks.fetch_add(waited, std::memory_order_relaxed);
            observeMaximum(g_max_cooperative_wait_ticks, waited);
            return false;
        }
        if (progressed()) {
            const uint32_t waited = xTaskGetTickCount() - started;
            g_cooperative_wait_ticks.fetch_add(waited, std::memory_order_relaxed);
            observeMaximum(g_max_cooperative_wait_ticks, waited);
            return true;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= max_wait_ticks) {
            g_timed_out_waits.fetch_add(1, std::memory_order_relaxed);
            g_cooperative_wait_ticks.fetch_add(elapsed, std::memory_order_relaxed);
            observeMaximum(g_max_cooperative_wait_ticks, elapsed);
            return false;
        }
        vTaskDelay(std::min<TickType_t>(slice_ticks, max_wait_ticks - elapsed));
    }
}

Snapshot snapshot()
{
    Snapshot result{};
    result.initialized = g_initialized.load(std::memory_order_acquire);
    result.init_error = g_init_error.load(std::memory_order_acquire);
    result.last_idle_tick = g_last_idle_tick.load(std::memory_order_acquire);
    result.idle_generation = g_idle_generation.load(std::memory_order_acquire);
    result.current_idle_gap_ticks = result.initialized ?
        xTaskGetTickCount() - result.last_idle_tick : 0;
    observeMaximum(g_max_idle_gap_ticks, result.current_idle_gap_ticks);
    result.max_idle_gap_ticks = g_max_idle_gap_ticks.load(std::memory_order_relaxed);
    result.cooperative_waits = g_cooperative_waits.load(std::memory_order_relaxed);
    result.cooperative_wait_ticks = g_cooperative_wait_ticks.load(std::memory_order_relaxed);
    result.max_cooperative_wait_ticks = g_max_cooperative_wait_ticks.load(std::memory_order_relaxed);
    result.cancelled_waits = g_cancelled_waits.load(std::memory_order_relaxed);
    result.timed_out_waits = g_timed_out_waits.load(std::memory_order_relaxed);
    return result;
}

}  // 命名空间 voice::audio_idle_guard 结束
