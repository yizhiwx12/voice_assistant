#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace device_test {
struct StopLoop {};
struct State {
    uint32_t now_ms = 0, stop_at_ms = 0;
    bool running = false, wifi_initially_connecting = false;
    uint32_t wifi_connect_at_ms = 0;
    unsigned wifi_polls = 0, conversation_starts = 0, action_calls = 0, retry_calls = 0;
    unsigned renders = 0, gpio_configurations = 0, loop_delays = 0;
    uint32_t render_delay_ms = 0;
    unsigned render_timeouts_remaining = 0;
    uint64_t gpio_mask = 0;
    int gpio_pullup = 0, gpio_pulldown = 0;
    std::vector<std::array<uint32_t, 3>> presses; // 引脚、包含的起点、不包含的终点。
    std::vector<std::pair<char, uint32_t>> events;
    std::vector<uint32_t> render_times;
};
inline State state{};
// 即使应用工作停滞，也推进独立定时器服务。
void advanceTime(uint32_t milliseconds);
inline void reset() { state = {}; }
template <typename... Args> void log(const char *, Args...) {}
}
