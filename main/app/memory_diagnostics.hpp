#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace voice::memory {

struct Region {
    size_t free_bytes = 0;
    // ESP-IDF 将各区域的最低值相加；该值用于提示最坏情况，
    // 不一定是某个时刻实际同时存在的总空闲内存。
    size_t minimum_free_bytes = 0;
    size_t largest_free_block = 0;
};

struct Snapshot {
    Region internal;
    Region dma;
    Region psram;
    bool failure_hook_installed = false;
    // 分配请求失败次数，也包含指定内存能力后失败、
    // 但调用者随后可能改用其他分配策略恢复的请求。
    uint32_t failed_allocations = 0;
    bool first_failure_ready = false;
    size_t first_failed_bytes = 0;
    uint32_t first_failed_caps = 0;
    std::array<char, 48> first_failed_function{};
};

// 应用启动初期、显示/音频/网络初始化前安装一次。
// 重复注册不会重复安装。钩子在内部 DRAM 的私有 ESP-IDF 临界区内
// 记录固定大小数据，不分配内存或打印日志。
// 日志和堆检查随后在临界区外完成。
esp_err_t install();
Snapshot capture();
void log(const char *stage);

// 根据已采集状态进行参考性的预算检查。
// 调用者行动前其他任务仍可能分配内存，因此不能保证之后的分配一定成功。
bool hasInternalBudget(const Snapshot &snapshot, size_t required_free,
                       size_t required_largest);

}  // 命名空间 voice::memory 结束
