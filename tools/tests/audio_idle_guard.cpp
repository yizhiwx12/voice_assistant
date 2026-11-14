#include "app/audio_idle_guard.hpp"
#include "esp_freertos_hooks.h"
#include "freertos/task.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
uint32_t now_tick = 100;
unsigned core = 0;
unsigned registrations = 0;
esp_err_t registration_result = ESP_OK;
esp_freertos_idle_cb_t callback = nullptr;
std::vector<TickType_t> delays;
std::function<void()> during_delay;

void idle()
{
    assert(callback != nullptr);
    assert(callback());
}

void resetDelays()
{
    delays.clear();
    during_delay = nullptr;
}
}  // 命名空间

esp_err_t esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t cb, unsigned cpu)
{
    assert(cpu == 0);
    ++registrations;
    if (registration_result == ESP_OK) callback = cb;
    return registration_result;
}

TickType_t xTaskGetTickCount() { return now_tick; }
unsigned xPortGetCoreID() { return core; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && ticks <= 2);
    delays.push_back(ticks);
    now_tick += ticks;
    // 调用前先复制，使嵌套工作任务检查点可以改变下一步调度动作，
    // 又不会使正在执行的函数失效。
    auto action = during_delay;
    if (action) action();
}

int main(int argc, char **argv)
{
    using namespace voice::audio_idle_guard;
    std::atomic<bool> running{true};
    assert(!snapshot().initialized);
    assert(!cooperate(running, true, 8));

    if (argc > 1 && std::string(argv[1]) == "registration_failure") {
        registration_result = ESP_ERR_NO_MEM;
        assert(initialize() == ESP_ERR_NO_MEM);
        assert(initialize() == ESP_ERR_NO_MEM);
        assert(registrations == 1);
        assert(!snapshot().initialized);
        assert(snapshot().init_error == ESP_ERR_NO_MEM);
        assert(!cooperate(running, true, 8));
        assert(delays.empty());
        std::cout << "audio idle guard: registration failure propagated\n";
        return 0;
    }

    assert(initialize() == ESP_OK);
    assert(initialize() == ESP_OK);
    assert(registrations == 1);
    assert(snapshot().initialized);
    assert(snapshot().idle_generation == 0);
    assert(cooperate(running, true, 8));
    assert(delays.empty());

    // 50 ms 之前到达检查点不会暂停音频服务。
    now_tick = 149;
    assert(cooperate(running, true, 8));
    assert(delays.empty());

    // 保护器在一个无关任务时间片后继续等待，
    // 只有下一时间片实际执行空闲钩子后才退出。
    now_tick = 150;
    during_delay = [] { if (delays.size() == 2) idle(); };
    assert(cooperate(running, true, 8));
    assert(delays == std::vector<TickType_t>({2, 2}));
    assert(snapshot().idle_generation == 1);
    assert(snapshot().cooperative_waits == 1);
    assert(snapshot().max_cooperative_wait_ticks == 4);
    assert(snapshot().max_idle_gap_ticks == 52);
    resetDelays();
    assert(cooperate(running, true, 8));
    assert(delays.empty());

    // 饱和的 SDK/网络任务不能让参考工作任务永久停留。
    now_tick += 50;
    assert(!cooperate(running, true, 8));
    assert(delays == std::vector<TickType_t>({2, 2, 2, 2}));
    assert(snapshot().timed_out_waits == 1);
    resetDelays();

    // 即使分发上限为 100 ms，仍会在时间片之间检查停止状态。
    during_delay = [&] { running.store(false); };
    assert(!cooperate(running, true, 100));
    assert(delays == std::vector<TickType_t>({2}));
    assert(snapshot().cancelled_waits == 1);
    resetDelays();
    running.store(true);

    // 支持 VoiceClient::stopping_ 使用的反向生命周期标记。
    std::atomic<bool> stopping{false};
    during_delay = [&] { stopping.store(true); };
    assert(!cooperate(stopping, false, 100));
    assert(delays == std::vector<TickType_t>({2}));
    resetDelays();

    // 一次分发等待可能覆盖下一个播放安全边界。两个工作任务
    // 使用同一个空闲进度信号，而非交替固定延时。
    stopping.store(false);
    during_delay = [&] {
        during_delay = [] { idle(); };
        assert(cooperate(running, true, 8));
    };
    const auto before_nested = snapshot().idle_generation;
    assert(cooperate(stopping, false, 100));
    assert(delays == std::vector<TickType_t>({2, 2}));
    assert(snapshot().idle_generation == before_nested + 1);
    resetDelays();

    // 仅保护 CPU0；在生产代码保留此检查，防止意外应用调用
    // 延迟 CPU1 上的采集或 AFE。
    now_tick += 50;
    core = 1;
    assert(cooperate(running, true, 8));
    assert(delays.empty());
    core = 0;

    // FreeRTOS 节拍回绕采用无符号减法，等待仍然有界。
    now_tick = UINT32_MAX - 10;
    idle();
    now_tick = 30;
    assert(cooperate(running, true, 8));
    assert(delays.empty());
    now_tick = 50;
    during_delay = [] { idle(); };
    assert(cooperate(running, true, 8));
    assert(delays == std::vector<TickType_t>({2}));
    resetDelays();

    // 请求的延时始终不超出有限上限，即使上限为奇数。
    now_tick += 50;
    assert(!cooperate(running, true, 3));
    assert(delays == std::vector<TickType_t>({2, 1}));
    assert(snapshot().timed_out_waits == 2);

    std::cout << "audio idle guard: fresh idle, actual progress, bounded waits, "
                 "stop, shared workers, core affinity and tick wrap passed\n";
}
