#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"

namespace voice::audio_idle_guard {

// 守卫仅注册一次，并在整个应用运行期间保留。
// 回调只更新固定大小的原子变量；
// 空闲任务中不打印日志、不分配内存，也不加锁。
struct Snapshot {
    bool initialized = false;
    esp_err_t init_error = ESP_ERR_INVALID_STATE;
    uint32_t last_idle_tick = 0;
    uint32_t idle_generation = 0;
    uint32_t current_idle_gap_ticks = 0;
    uint32_t max_idle_gap_ticks = 0;
    uint32_t cooperative_waits = 0;
    uint32_t cooperative_wait_ticks = 0;
    uint32_t max_cooperative_wait_ticks = 0;
    uint32_t cancelled_waits = 0;
    uint32_t timed_out_waits = 0;
};

// 注册 CPU0 空闲回调，成功后重复调用不会重复注册；
// 注册失败会明确返回给调用者。
esp_err_t initialize();

// 空闲任务连续 50 毫秒未运行时，主动让出 CPU0；每次重试阻塞两个时钟节拍。
// 调用者必须提供有限的最长等待时间，防止其他高优先级任务长期占用 CPU
// 时参考音频服务和网络控制失去响应。该上限约束主动请求的延迟，
// 不包含被其他任务抢占的时间。
//
// 此函数供 CPU0 上的 AEC 工作任务在无锁边界调用。空闲回调已推进或无需等待
// 时返回 true；初始化失败、lifecycle != active_value 或等待超时时返回 false。
// 播放/参考任务使用 (running, true, 8)，分发任务使用 (stopping, false, 100)。
bool cooperate(const std::atomic<bool> &lifecycle, bool active_value,
               uint32_t max_wait_ms);

Snapshot snapshot();

}  // 命名空间 voice::audio_idle_guard 结束
