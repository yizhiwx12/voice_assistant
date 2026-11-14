#include "display/lvgl_memory.h"
#include "esp_heap_caps.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
alignas(16) unsigned char external_pool[65536];
unsigned allocations = 0;
bool fail_allocation = false;
}

void *heap_caps_malloc(size_t bytes, unsigned capabilities)
{
    ++allocations;
    assert(bytes == sizeof(external_pool));
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return fail_allocation ? nullptr : external_pool;
}

void stub_lvgl_log(const char *level, const char *tag, const char *format, ...)
{
    std::printf("%s %s: ", level, tag);
    va_list arguments;
    va_start(arguments, format);
    std::vprintf(format, arguments);
    va_end(arguments);
    std::printf("\n");
}

// 验证生产致命错误路径，同时避免触发平台崩溃对话框。
extern "C" [[noreturn]] void __wrap_abort()
{
    std::fflush(stdout);
    std::_Exit(86);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (std::strcmp(argv[1], "allocation-failed") == 0) {
        fail_allocation = true;
        voice_lvgl_pool_alloc(sizeof(external_pool));
        return 1;
    }
    if (std::strcmp(argv[1], "invalid-size") == 0) {
        voice_lvgl_pool_alloc(0);
        return 1;
    }
    auto *memory = static_cast<unsigned char *>(voice_lvgl_pool_alloc(sizeof(external_pool)));
    assert(memory == external_pool && allocations == 1);
    memory[0] = 17;
    memory[65535] = 93;
    if (std::strcmp(argv[1], "changed-size") == 0) {
        voice_lvgl_pool_alloc(32768);
        return 1;
    }
    // LVGL 在 lv_deinit 时销毁 TLSF 元数据，但没有内存池释放钩子。
    // 重复初始化必须复用同一次分配，不能每次泄漏 64 KiB。
    for (unsigned i = 0; i < 100; ++i)
        assert(voice_lvgl_pool_alloc(sizeof(external_pool)) == memory);
    assert(allocations == 1 && memory[0] == 17 && memory[65535] == 93);
    std::puts("LVGL PSRAM pool allocation and 100 reinitializations passed");
}
