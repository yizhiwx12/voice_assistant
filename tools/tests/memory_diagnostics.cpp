// 生产诊断配合主机堆元数据及失败回调桩。
#include "app/memory_diagnostics.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
esp_alloc_failed_hook_t installed_hook = nullptr;
esp_err_t registration_result = ESP_OK;
unsigned registrations = 0;
unsigned heap_queries = 0;
unsigned error_logs = 0;
std::string log_text;
thread_local bool inside_hook = false;
thread_local bool inside_critical = false;

void invokeHook(size_t bytes, uint32_t caps, const char *function)
{
    assert(installed_hook);
    inside_hook = true;
    installed_hook(bytes, caps, function);
    inside_hook = false;
}

void checkRegionSnapshot()
{
    const auto value = voice::memory::capture();
    assert(heap_queries == 3);
    assert(value.internal.free_bytes == 70000);
    assert(value.internal.minimum_free_bytes == 1200);
    assert(value.internal.largest_free_block == 5000);
    assert(value.dma.free_bytes == 60000 && value.dma.largest_free_block == 4000);
    assert(value.psram.free_bytes == 7000000 && value.psram.minimum_free_bytes == 6000000);
    assert(!value.failure_hook_installed && !value.first_failure_ready && value.failed_allocations == 0);
}

void checkBudget()
{
    auto value = voice::memory::capture();
    // 总内存充足不能掩盖内部堆碎片化。
    assert(!voice::memory::hasInternalBudget(value, 60000, 6144));
    assert(voice::memory::hasInternalBudget(value, 70000, 5000));
    assert(!voice::memory::hasInternalBudget(value, 70001, 5000));
    assert(!voice::memory::hasInternalBudget(value, 70000, 5001));
    assert(voice::memory::hasInternalBudget(value, 0, 0));
    assert(!voice::memory::hasInternalBudget(value, std::numeric_limits<size_t>::max(), 1));
}

void checkRegistration()
{
    registration_result = ESP_FAIL;
    assert(voice::memory::install() == ESP_FAIL && registrations == 1);
    assert(!voice::memory::capture().failure_hook_installed);
    registration_result = ESP_OK;
    assert(voice::memory::install() == ESP_OK && registrations == 2);
    assert(voice::memory::capture().failure_hook_installed);
    assert(voice::memory::install() == ESP_OK && registrations == 2);
}

void checkFirstFailureCopy()
{
    assert(voice::memory::install() == ESP_OK);
    char caller_owned[] = "heap_caps_malloc";
    invokeHook(6144, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, caller_owned);
    assert(heap_queries == 0 && log_text.empty());
    std::memset(caller_owned, 'x', sizeof(caller_owned) - 1);
    invokeHook(128, MALLOC_CAP_SPIRAM, "later_recoverable_failure");
    const auto value = voice::memory::capture();
    assert(value.failed_allocations == 2 && value.first_failure_ready);
    assert(value.first_failed_bytes == 6144);
    assert(value.first_failed_caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    assert(std::strcmp(value.first_failed_function.data(), "heap_caps_malloc") == 0);
}

void checkNameBoundary(bool null_name)
{
    assert(voice::memory::install() == ESP_OK);
    char name[160];
    std::memset(name, 'z', sizeof(name));
    name[sizeof(name) - 1] = '\0';
    invokeHook(0, 0, null_name ? nullptr : name);
    const auto value = voice::memory::capture();
    assert(value.failed_allocations == 1 && value.first_failure_ready);
    assert(value.first_failed_bytes == 0 && value.first_failed_caps == 0);
    const size_t length = std::strlen(value.first_failed_function.data());
    assert(length == (null_name ? 0 : value.first_failed_function.size() - 1));
    assert(value.first_failed_function.back() == '\0');
}

void checkConcurrentCallbacks()
{
    assert(voice::memory::install() == ESP_OK);
    std::atomic<bool> go{false};
    std::atomic<unsigned> completed{0};
    std::vector<std::thread> threads;
    for (size_t worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&, worker] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int index = 0; index < 500; ++index)
                invokeHook(100 + worker, static_cast<uint32_t>(100 + worker), "concurrent");
            completed.fetch_add(1, std::memory_order_release);
        });
    }
    go.store(true, std::memory_order_release);
    // 在回调竞争期间读取。首条记录及计数必须一起捕获；
    // 读取方绝不能看到只复制了一半的函数名。
    while (completed.load(std::memory_order_acquire) != 8) {
        const auto during = voice::memory::capture();
        if (during.first_failure_ready) {
            assert(during.failed_allocations > 0);
            assert(during.first_failed_bytes >= 100 && during.first_failed_bytes <= 107);
            assert(during.first_failed_caps == during.first_failed_bytes);
            assert(std::strcmp(during.first_failed_function.data(), "concurrent") == 0);
        } else {
            assert(during.failed_allocations == 0);
        }
        std::this_thread::yield();
    }
    for (auto &thread : threads) thread.join();
    const auto value = voice::memory::capture();
    assert(value.failed_allocations == 4000 && value.first_failure_ready);
    assert(value.first_failed_bytes >= 100 && value.first_failed_bytes <= 107);
    assert(value.first_failed_caps == value.first_failed_bytes);
    assert(std::strcmp(value.first_failed_function.data(), "concurrent") == 0);
    assert(error_logs == 0);
}

void checkDeferredLogging()
{
    assert(voice::memory::install() == ESP_OK);
    voice::memory::log("before_ws");
    assert(log_text.find("stage=before_ws internal_free=70000 internal_min=1200 internal_largest=5000") != std::string::npos);
    assert(log_text.find("dma_free=60000") != std::string::npos);
    assert(log_text.find("psram_free=7000000") != std::string::npos);
    assert(error_logs == 0);
    invokeHook(6144, MALLOC_CAP_INTERNAL, "xTaskCreate");
    assert(error_logs == 0);
    voice::memory::log("ws_failed");
    assert(error_logs == 1 && log_text.find("allocation_failure count=1 first_bytes=6144") != std::string::npos);
    voice::memory::log(nullptr);
    assert(error_logs == 1 && log_text.find("stage=unknown") != std::string::npos);
    invokeHook(500, MALLOC_CAP_DMA, "subsequent");
    voice::memory::log("later");
    assert(error_logs == 2 && log_text.find("allocation_failure count=2 first_bytes=6144") != std::string::npos);
}
}  // 命名空间

__attribute__((noinline)) void *operator new(size_t bytes)
{
    if (inside_hook || inside_critical) std::abort();
    void *result = std::malloc(bytes ? bytes : 1);
    if (!result) throw std::bad_alloc();
    return result;
}
__attribute__((noinline)) void *operator new[](size_t bytes) { return ::operator new(bytes); }
__attribute__((noinline)) void operator delete(void *memory) noexcept { std::free(memory); }
__attribute__((noinline)) void operator delete[](void *memory) noexcept { std::free(memory); }
__attribute__((noinline)) void operator delete(void *memory, size_t) noexcept { std::free(memory); }
__attribute__((noinline)) void operator delete[](void *memory, size_t) noexcept { std::free(memory); }

esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    assert(!inside_hook && !inside_critical);
    ++registrations;
    if (registration_result == ESP_OK) installed_hook = callback;
    return registration_result;
}

void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)
{
    assert(!inside_hook && !inside_critical);
    ++heap_queries;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) *info = {70000, 1200, 5000};
    else if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) *info = {60000, 1000, 4000};
    else if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) *info = {7000000, 6000000, 5000000};
    else std::abort();
}

void memoryTestLog(bool error, const char *format, ...)
{
    assert(!inside_hook && !inside_critical);
    if (error) ++error_logs;
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log_text += line;
    log_text += '\n';
}

void memoryTestEnterCritical(portMUX_TYPE *lock)
{
    assert(!inside_critical);  // 生产代码绝不递归获取此锁。
    while (lock->flag.test_and_set(std::memory_order_acquire)) {}
    inside_critical = true;
}

void memoryTestExitCritical(portMUX_TYPE *lock)
{
    assert(inside_critical);
    inside_critical = false;
    lock->flag.clear(std::memory_order_release);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!std::strcmp(argv[1], "snapshot")) checkRegionSnapshot();
    else if (!std::strcmp(argv[1], "budget")) checkBudget();
    else if (!std::strcmp(argv[1], "registration")) checkRegistration();
    else if (!std::strcmp(argv[1], "first-copy")) checkFirstFailureCopy();
    else if (!std::strcmp(argv[1], "long-name")) checkNameBoundary(false);
    else if (!std::strcmp(argv[1], "null-name")) checkNameBoundary(true);
    else if (!std::strcmp(argv[1], "concurrent")) checkConcurrentCallbacks();
    else if (!std::strcmp(argv[1], "logging")) checkDeferredLogging();
    else std::abort();
}
