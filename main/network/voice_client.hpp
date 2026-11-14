#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// 本传输层不保存云端凭证，也不选择云服务商。
// Handler 在 voice_rx 上串行调用，不在 WebSocket 回调任务中运行。
// ESP32 只连接本地语音服务，云端密钥和模型选择留在电脑上。
// WebSocket 回调组包后入队，voice_rx 顺序分发，避免 SDK 回调线程执行音频业务。
// 队列和单条消息都有上限；取消/重连通过连接身份隔离旧消息。
class VoiceClient {
public:
    enum class Event : uint8_t { Connected, Disconnected, Text, Binary, Failure };
    using Handler = std::function<void(Event, const uint8_t *, size_t)>;
    struct Diagnostics {
        // reason 始终指向固定本地分类，不引用线上消息文字。
        const char *reason = "none";
        int64_t first_failure_us = 0;
        esp_err_t result = ESP_OK;
        int close_code = 0;
        int socket_errno = 0;
        int error_type = 0;
        int handshake_status = 0;
        int last_send_count = 0;
        uint32_t last_send_us = 0;
        // 连接范围内回调的墙钟耗时，包含应用等待；
        // 不代表 CPU 利用率，也不只是 Opus 解码耗时。
        uint32_t rx_messages = 0;
        uint32_t rx_binary_messages = 0;
        uint32_t rx_handler_last_us = 0;
        uint32_t rx_handler_max_us = 0;
        uint32_t rx_batch_max_us = 0;
        uint32_t rx_batch_messages_peak = 0;
        uint32_t rx_budget_waits = 0;
        uint32_t rx_queue_peak = 0;
        int rx_core = -1;
    };

    VoiceClient() = default;
    ~VoiceClient();
    VoiceClient(const VoiceClient &) = delete;
    VoiceClient &operator=(const VoiceClient &) = delete;

    // 生命周期调用由所属模块串行执行。不能在 Handler 内调用 stop()；
    // 应通知独立的应用控制任务。
    esp_err_t start(const char *uri, Handler handler);
    esp_err_t stop();
    esp_err_t sendText(const char *text, size_t size, uint32_t timeout_ms = 500);
    esp_err_t sendBinary(const uint8_t *data, size_t size, uint32_t timeout_ms = 500);
    bool connected() const { return connected_.load(std::memory_order_acquire); }
    Diagnostics diagnostics() const;
    void logDispatchDiagnostics(const char *trigger = "manual") const;

private:
    struct Message {
        Event event;
        uint8_t *data;
        size_t size;
        // 完整 WebSocket 文本消息进入传输队列时记录 response.cancelled 标记。
        // 只保留数值身份和时间，
        // 不在此处保留或打印控制载荷。
        uint8_t cancel_ack_state = 0; // 1 表示有效身份，2 表示无效身份。
        uint32_t cancel_ack_epoch = 0;
        uint32_t cancel_ack_turn = 0;
        uint32_t cancel_ack_generation = 0;
        int64_t cancel_ack_received_us = 0;
    };

    static constexpr size_t kMessageLimit = 16384;
    static constexpr size_t kQueueDepth = 16;
    static void websocketEvent(void *argument, esp_event_base_t base, int32_t id, void *data);
    static void dispatchEntry(void *argument);
    void onEvent(int32_t id, const esp_websocket_event_data_t *data);
    void onData(const esp_websocket_event_data_t &data);
    void dispatch();
    bool enqueue(Event event, const uint8_t *data = nullptr, size_t size = 0);
    void fail(const char *reason, esp_err_t result = ESP_FAIL);
    void recordFailure(const char *reason, esp_err_t result);
    void snapshotDispatch(Diagnostics *snapshot) const;
    static void logDispatch(const Diagnostics &snapshot, const char *trigger);
    void clearAssembly();
    void logCancelAck(const char *phase, const Message &message, uint32_t queue_depth,
                      int64_t now_us, uint32_t wait_us = 0) const;
    esp_err_t send(bool binary, const uint8_t *data, size_t size, uint32_t timeout_ms);

    esp_websocket_client_handle_t client_ = nullptr;
    Handler handler_;
    std::string uri_;
    QueueHandle_t messages_ = nullptr;
    SemaphoreHandle_t send_mutex_ = nullptr;
    SemaphoreHandle_t dispatched_stop_ = nullptr;
    TaskHandle_t dispatch_task_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{true};
    std::atomic<bool> failed_{false};
    mutable std::mutex diagnostics_mutex_;
    Diagnostics diagnostics_{};
    // 分派的高频路径只使用 32 位原子诊断，不使用 diagnostics_mutex_。
    // 分别采样的计数器不是同一个一致的音频或 RTOS 事务；
    // 所属模块在停止后读取最终值。
    std::atomic<uint32_t> rx_messages_{0}, rx_binary_messages_{0};
    std::atomic<uint32_t> rx_handler_last_us_{0}, rx_handler_max_us_{0};
    std::atomic<uint32_t> rx_batch_max_us_{0}, rx_batch_messages_peak_{0};
    std::atomic<uint32_t> rx_budget_waits_{0}, rx_queue_peak_{0};
    std::atomic<uint32_t> rx_core_{UINT32_MAX};
    uint8_t *assembly_ = nullptr;
    size_t assembly_size_ = 0;
    uint8_t message_opcode_ = 0;
    size_t frame_size_ = 0;
    size_t frame_offset_ = 0;
    bool frame_active_ = false;
};
