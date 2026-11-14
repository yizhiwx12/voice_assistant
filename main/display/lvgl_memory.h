#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL 内置 TLSF 分配器在整个进程生命周期内拥有此内存池。
// 调用者必须串行执行 lv_init/lv_deinit 及本钩子。
// 重复初始化复用同一块 PSRAM；LVGL 没有释放内存池的钩子。
void *voice_lvgl_pool_alloc(size_t bytes);

#ifdef __cplusplus
}
#endif
