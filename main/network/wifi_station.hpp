#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "esp_err.h"

// Wi-Fi 连接状态机：凭证来自 menuconfig，连接重试同时受次数和总时限约束。
// 主循环 poll() 与 SDK 回调共同更新状态；掉线恢复不会无限占用后台任务。
class WifiStation {
public:
    enum class State : uint8_t {
        NotConfigured,
        Connecting,
        Connected,
        Failed,
    };

    esp_err_t start();
    // 通过主循环轮询限制单次连接周期，不额外创建任务或定时器。
    void poll();
    esp_err_t retry();
    State state() const { return state_.load(std::memory_order_acquire); }
    uint32_t ipAddress() const { return ip_address_.load(std::memory_order_relaxed); }

private:
    static void eventHandler(void *argument, const char *event_base, int32_t event_id,
                             void *event_data);
    void handleEvent(const char *event_base, int32_t event_id, void *event_data);

    std::atomic<State> state_{State::NotConfigured};
    std::atomic<uint32_t> ip_address_{0};
    std::atomic<uint8_t> retry_count_{0};
    std::atomic<uint32_t> connect_started_ms_{0};
    std::mutex connection_mutex_;
    bool started_ = false;
};
