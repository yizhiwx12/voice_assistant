#include "voice_client.hpp"
#include "app/memory_diagnostics.hpp"
#include "app/audio_idle_guard.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <cctype>
#include <cmath>
#include <utility>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "voice_transport";
constexpr uint8_t kContinuation = 0;
constexpr uint8_t kText = 1;
constexpr uint8_t kBinary = 2;
constexpr uint8_t kClose = 8;
constexpr uint8_t kPing = 9;
constexpr uint8_t kPong = 10;
constexpr uint32_t kDispatchWorkBudgetUs = 8000;
constexpr uint32_t kDispatchMessageBudget = 8;
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
constexpr bool kAecDispatch = true;
#else
constexpr bool kAecDispatch = false;
#endif

void observeMaximum(std::atomic<uint32_t> &maximum, uint32_t value)
{
    uint32_t previous = maximum.load(std::memory_order_relaxed);
    while (value > previous && !maximum.compare_exchange_weak(
        previous, value, std::memory_order_relaxed)) {}
}

uint8_t *allocateBytes(size_t size)
{
    auto *result = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return result != nullptr ? result : static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
}

bool validUri(const char *uri)
{
    if (uri == nullptr) {
        return false;
    }
    const size_t size = std::strlen(uri);
    if (size < 6 || size > 512 ||
        (std::strncmp(uri, "ws://", 5) != 0 && std::strncmp(uri, "wss://", 6) != 0)) {
        return false;
    }
    const char *authority = uri + (uri[2] == 's' ? 6 : 5);
    const char *end = std::strchr(authority, '/');
    if (authority == end || *authority == '\0') {
        return false;
    }
    for (const char *position = uri; *position; ++position) {
        if (static_cast<unsigned char>(*position) <= 32 || *position == '@' ||
            *position == '?' || *position == '#') {
            return false;
        }
    }
    return true;
}

struct CancelAckIdentity {
    uint32_t epoch = 0;
    uint32_t turn = 0;
    uint32_t generation = 0;
    bool valid = false;
};

bool cancelAckIdentity(cJSON *event, CancelAckIdentity &identity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(event, "type");
    if (!cJSON_IsString(value) || !value->valuestring ||
        std::strcmp(value->valuestring, "response.cancelled") != 0) return false;

    // 事件类型准确匹配，即使身份字段有误仍可用于诊断。
    identity.valid = false;
    const cJSON *fields[] = {
        cJSON_GetObjectItemCaseSensitive(event, "session_epoch"),
        cJSON_GetObjectItemCaseSensitive(event, "turn_id"),
        cJSON_GetObjectItemCaseSensitive(event, "generation")};
    uint32_t parsed[3]{};
    for (size_t i = 0; i < 3; ++i) {
        if (!cJSON_IsNumber(fields[i]) || !std::isfinite(fields[i]->valuedouble) ||
            fields[i]->valuedouble < 0 || fields[i]->valuedouble > UINT32_MAX ||
            std::floor(fields[i]->valuedouble) != fields[i]->valuedouble) return true;
        parsed[i] = static_cast<uint32_t>(fields[i]->valuedouble);
    }
    if (!parsed[0] || !parsed[1] || !parsed[2]) return true;
    identity.epoch = parsed[0];
    identity.turn = parsed[1];
    identity.generation = parsed[2];
    identity.valid = true;
    return true;
}

bool classifyCancelAck(const uint8_t *data, size_t size, CancelAckIdentity &identity)
{
    if (data == nullptr || size == 0 || size > 512) return false;
    // 普通 ASR 或播放控制消息不含该短语。
    // 先做低成本的有界预筛选，再由 cJSON 精确判断类型。
    constexpr char kAckHint[] = "response.cancelled";
    // 经过 JSON 转义的类型或键也需要进入精确解析。
    bool hint_found = std::memchr(data, '\\', size) != nullptr;
    for (size_t i = 0; i + sizeof(kAckHint) - 1 <= size && !hint_found; ++i)
        hint_found = std::memcmp(data + i, kAckHint, sizeof(kAckHint) - 1) == 0;
    if (!hint_found) return false;
    // 探测运行在较小的 SDK 回调栈上。真实 ACK 是扁平对象；
    // 深层嵌套候选留给普通 RX 处理器。
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t character = data[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
        } else if (character == '"') quoted = true;
        else if (character == '{' || character == '[') {
            if (++depth > 8) return false;
        } else if ((character == '}' || character == ']') && depth) --depth;
    }
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(reinterpret_cast<const char *>(data), size, &end, 0);
    if (!root) return false;
    bool is_ack = false;
    const char *cursor = end;
    while (cursor && static_cast<size_t>(cursor - reinterpret_cast<const char *>(data)) < size &&
           std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    if (cursor && static_cast<size_t>(cursor - reinterpret_cast<const char *>(data)) == size &&
        cJSON_IsObject(root)) is_ack = cancelAckIdentity(root, identity);
    cJSON_Delete(root);
    return is_ack;
}
} // 匿名命名空间结束

VoiceClient::~VoiceClient()
{
    stop();
    if (send_mutex_ != nullptr) {
        vSemaphoreDelete(send_mutex_);
        send_mutex_ = nullptr;
    }
}

esp_err_t VoiceClient::start(const char *uri, Handler handler)
{
    if (client_ != nullptr || dispatch_task_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!validUri(uri) || !handler) {
        return ESP_ERR_INVALID_ARG;
    }
    if (kAecDispatch) {
        const esp_err_t idle_guard_result = voice::audio_idle_guard::initialize();
        if (idle_guard_result != ESP_OK) return idle_guard_result;
    }
    voice::memory::log("ws_before_alloc");
    uri_ = uri;
    handler_ = std::move(handler);
    messages_ = xQueueCreate(kQueueDepth, sizeof(Message));
    if (send_mutex_ == nullptr) {
        send_mutex_ = xSemaphoreCreateMutex();
    }
    dispatched_stop_ = xSemaphoreCreateBinary();
    assembly_ = allocateBytes(kMessageLimit + 1);
    if (messages_ == nullptr || send_mutex_ == nullptr || dispatched_stop_ == nullptr || assembly_ == nullptr) {
        voice::memory::log("ws_buffer_failed");
        stop();
        return ESP_ERR_NO_MEM;
    }
    failed_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        diagnostics_ = {};
    }
    rx_messages_.store(0, std::memory_order_relaxed);
    rx_binary_messages_.store(0, std::memory_order_relaxed);
    rx_handler_last_us_.store(0, std::memory_order_relaxed);
    rx_handler_max_us_.store(0, std::memory_order_relaxed);
    rx_batch_max_us_.store(0, std::memory_order_relaxed);
    rx_batch_messages_peak_.store(0, std::memory_order_relaxed);
    rx_budget_waits_.store(0, std::memory_order_relaxed);
    rx_queue_peak_.store(0, std::memory_order_relaxed);
    rx_core_.store(UINT32_MAX, std::memory_order_relaxed);
    stopping_.store(false, std::memory_order_release);
    // 当前原始 Opus 的定点 DSP 解码使用 alloca。
    // 此任务执行下行处理器，因此栈必须同时容纳
    // 解码器及 JSON 和控制消息解析。
    // 采集、送入和取出任务使用 CPU1；播放/参考与下行解码
    // 共享 CPU0，优先级低于 Wi-Fi。两个应用工作任务在安全边界
    // 协调真实空闲进度。未启用 AEC 时不固定核心。
    const auto create_dispatch = [this](int caps) {
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
        return xTaskCreatePinnedToCoreWithCaps(dispatchEntry, "voice_rx", 32768, this, 5,
                                              &dispatch_task_, 0, caps);
#else
        return xTaskCreateWithCaps(dispatchEntry, "voice_rx", 32768, this, 5,
                                   &dispatch_task_, caps);
#endif
    };
    ESP_LOGI(kTag, "dispatch_scheduling aec=%u core=%d priority=5 budget_us=%u max_messages=%u delay_ticks=1 "
             "idle_feedback=%u idle_gap_ms=50 idle_max_wait_ms=100",
             static_cast<unsigned>(kAecDispatch), kAecDispatch ? 0 : -1,
             static_cast<unsigned>(kDispatchWorkBudgetUs), static_cast<unsigned>(kDispatchMessageBudget),
             static_cast<unsigned>(kAecDispatch));
    BaseType_t created = create_dispatch(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        created = create_dispatch(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        dispatch_task_ = nullptr;
        voice::memory::log("ws_dispatch_failed");
        stop();
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_client_config_t config{};
    config.uri = uri_.c_str();
    config.disable_auto_reconnect = true;
    config.enable_close_reconnect = false;
    config.task_prio = 5;
    config.task_stack = 6144;
    config.task_name = "voice_ws";
    config.buffer_size = 2048;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 20;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    config.network_timeout_ms = 5000;
    config.ping_interval_sec = 20;
    config.pingpong_timeout_sec = 60;
    if (std::strncmp(uri, "wss://", 6) == 0) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#else
        stop();
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    client_ = esp_websocket_client_init(&config);
    if (client_ == nullptr) {
        voice::memory::log("ws_init_failed");
        stop();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, websocketEvent, this);
    if (result == ESP_OK) {
        voice::memory::log("ws_before_sdk_start");
        result = esp_websocket_client_start(client_);
    }
    if (result != ESP_OK) {
        voice::memory::log("ws_start_failed");
        stop();
    } else {
        voice::memory::log("ws_started");
    }
    return result;
}

esp_err_t VoiceClient::stop()
{
    if (dispatch_task_ != nullptr && xTaskGetCurrentTaskHandle() == dispatch_task_) {
        return ESP_ERR_INVALID_STATE;
    }
    // 发送或停止流程等待组件内部接收互斥锁时，回调可能正在施加 TCP 背压。
    // 先让该回调退出。
    const bool was_stopping = stopping_.exchange(true, std::memory_order_acq_rel);
    if (!was_stopping) {
        const auto diagnostic = diagnostics();
        ESP_LOGI(kTag, "stop origin=local prior=%s close_code=%d", diagnostic.reason, diagnostic.close_code);
    }
    connected_.store(false, std::memory_order_release);
    if (send_mutex_ != nullptr) {
        xSemaphoreTake(send_mutex_, portMAX_DELAY);
    }
    if (client_ != nullptr) {
        esp_websocket_client_stop(client_);
        esp_websocket_unregister_events(client_, WEBSOCKET_EVENT_ANY, websocketEvent);
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
    if (send_mutex_ != nullptr) {
        xSemaphoreGive(send_mutex_);
    }
    if (dispatch_task_ != nullptr) {
        // 处理器归应用所有，等待队列时必须有上限并能响应中断请求。
        // 任务持有音频锁时不能直接删除。
        xSemaphoreTake(dispatched_stop_, portMAX_DELAY);
        dispatch_task_ = nullptr;
    }
    if (!was_stopping) logDispatch(diagnostics(), "stop");
    if (messages_ != nullptr) {
        Message message{};
        while (xQueueReceive(messages_, &message, 0) == pdTRUE) {
            heap_caps_free(message.data);
        }
        vQueueDelete(messages_);
        messages_ = nullptr;
    }
    if (dispatched_stop_ != nullptr) {
        vSemaphoreDelete(dispatched_stop_);
        dispatched_stop_ = nullptr;
    }
    heap_caps_free(assembly_);
    assembly_ = nullptr;
    clearAssembly();
    handler_ = {};
    uri_.clear();
    if (!was_stopping) voice::memory::log("ws_after_stop");
    return ESP_OK;
}

esp_err_t VoiceClient::sendText(const char *text, size_t size, uint32_t timeout_ms)
{
    return send(false, reinterpret_cast<const uint8_t *>(text), size, timeout_ms);
}

esp_err_t VoiceClient::sendBinary(const uint8_t *data, size_t size, uint32_t timeout_ms)
{
    return send(true, data, size, timeout_ms);
}

esp_err_t VoiceClient::send(bool binary, const uint8_t *data, size_t size, uint32_t timeout_ms)
{
    if (data == nullptr || size == 0 || size > kMessageLimit || size > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stopping_.load(std::memory_order_acquire) || !connected() || send_mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t timeout = std::max<TickType_t>(1, pdMS_TO_TICKS(timeout_ms));
    if (xSemaphoreTake(send_mutex_, timeout) != pdTRUE) {
        recordFailure("send_lock_timeout", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!stopping_.load(std::memory_order_acquire) && connected() && client_ != nullptr) {
        const int64_t started = esp_timer_get_time();
        const int count = binary
            ? esp_websocket_client_send_bin(client_, reinterpret_cast<const char *>(data), static_cast<int>(size), timeout)
            : esp_websocket_client_send_text(client_, reinterpret_cast<const char *>(data), static_cast<int>(size), timeout);
        result = count == static_cast<int>(size) ? ESP_OK : ESP_ERR_TIMEOUT;
        {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            diagnostics_.last_send_count = count;
            diagnostics_.last_send_us = static_cast<uint32_t>(esp_timer_get_time() - started);
        }
        if (result != ESP_OK) {
            fail("send_incomplete", result); // 消息只发送了一部分时，必须终止当前会话并报错。
        }
    }
    xSemaphoreGive(send_mutex_);
    return result;
}

void VoiceClient::websocketEvent(void *argument, esp_event_base_t, int32_t id, void *data)
{
    static_cast<VoiceClient *>(argument)->onEvent(id, static_cast<const esp_websocket_event_data_t *>(data));
}

void VoiceClient::onEvent(int32_t id, const esp_websocket_event_data_t *data)
{
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        connected_.store(true, std::memory_order_release);
        clearAssembly();
        enqueue(Event::Connected);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        connected_.store(false, std::memory_order_release);
        if (data != nullptr) {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            if (data->close_status_code) diagnostics_.close_code = data->close_status_code;
        }
        // 正常关闭之前可能还有 session.closed 应用事件排在 voice_rx 队列中。
        // 保留先进先出顺序，
        // 由所属模块判断该关闭是否符合当前会话预期。
        if (diagnostics().close_code != 1000) {
            recordFailure(id == WEBSOCKET_EVENT_CLOSED ? "peer_closed" : "disconnected", ESP_ERR_INVALID_STATE);
        }
        clearAssembly();
        enqueue(Event::Disconnected);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        // 只复制数值诊断；data_ptr 可能包含地址或密钥。
        if (data != nullptr) {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            if (data->close_status_code) diagnostics_.close_code = data->close_status_code;
            const auto error_type = data->error_handle.error_type;
            diagnostics_.error_type = static_cast<int>(error_type);
            // WebSocket 组件只在 TCP 传输错误时初始化 socket errno。
            // 其他错误事件的此字段可能未定义，
            // 因此只有对应类型才公开该值。
            diagnostics_.socket_errno =
                error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT
                    ? data->error_handle.esp_transport_sock_errno
                    : 0;
            diagnostics_.handshake_status = data->error_handle.esp_ws_handshake_status_code;
        }
        fail("websocket_error");
    } else if (id == WEBSOCKET_EVENT_DATA && data != nullptr) {
        onData(*data);
    }
}

void VoiceClient::clearAssembly()
{
    assembly_size_ = 0;
    message_opcode_ = 0;
    frame_size_ = 0;
    frame_offset_ = 0;
    frame_active_ = false;
}

void VoiceClient::logCancelAck(const char *phase, const Message &message, uint32_t queue_depth,
                               int64_t now_us, uint32_t wait_us) const
{
    if (message.cancel_ack_state == 0) return;
    ESP_LOGI(kTag, "cancel_ack_transport phase=%s valid=%u epoch=%u turn=%u generation=%u "
             "queue_depth=%u at_us=%lld wait_us=%u",
             phase, static_cast<unsigned>(message.cancel_ack_state == 1),
             static_cast<unsigned>(message.cancel_ack_epoch), static_cast<unsigned>(message.cancel_ack_turn),
             static_cast<unsigned>(message.cancel_ack_generation), static_cast<unsigned>(queue_depth),
             static_cast<long long>(now_us), static_cast<unsigned>(wait_us));
}

void VoiceClient::onData(const esp_websocket_event_data_t &data)
{
    if (failed_.load(std::memory_order_acquire)) {
        return;
    }
    const uint8_t opcode = data.op_code & 0x0F;
    if (opcode == kClose) {
        connected_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(diagnostics_mutex_);
            if (data.payload_offset == 0 && data.data_ptr != nullptr && data.data_len >= 2) {
                const auto *bytes = reinterpret_cast<const uint8_t *>(data.data_ptr);
                diagnostics_.close_code = (static_cast<int>(bytes[0]) << 8) | bytes[1];
            }
        }
        if (diagnostics().close_code == 1000) {
            ESP_LOGI(kTag, "peer_close normal=1 close_code=1000");
        } else recordFailure("peer_close_frame", ESP_ERR_INVALID_STATE);
    }
    if (opcode == kPing || opcode == kPong || opcode == kClose) {
        // 控制应答由 esp_websocket_client 负责。
        // Ping/Pong 可穿插在分片应用消息之间。
        return;
    }
    if ((opcode != kText && opcode != kBinary && opcode != kContinuation) ||
        data.data_len < 0 || data.payload_len < 0 || data.payload_offset < 0 ||
        (data.data_len > 0 && data.data_ptr == nullptr)) {
        fail("invalid_fragment");
        return;
    }
    const size_t fragment = static_cast<size_t>(data.data_len);
    const size_t frame_size = static_cast<size_t>(data.payload_len);
    const size_t frame_offset = static_cast<size_t>(data.payload_offset);
    if (frame_offset == 0) {
        if (frame_active_ || (opcode != kContinuation && message_opcode_ != 0) ||
            (opcode == kContinuation && message_opcode_ == 0)) {
            fail("fragment_order");
            return;
        }
        if (opcode != kContinuation) {
            message_opcode_ = opcode;
        }
        frame_active_ = true;
        frame_size_ = frame_size;
        frame_offset_ = 0;
    }
    if (!frame_active_ || frame_offset != frame_offset_ || frame_size != frame_size_ ||
        frame_offset > frame_size || fragment > frame_size - frame_offset ||
        fragment > kMessageLimit - assembly_size_ ||
        frame_size > kMessageLimit - (assembly_size_ - frame_offset_)) {
        fail("fragment_bounds");
        return;
    }
    if (fragment != 0) {
        std::memcpy(assembly_ + assembly_size_, data.data_ptr, fragment);
    }
    assembly_size_ += fragment;
    frame_offset_ += fragment;
    if (frame_offset_ == frame_size_) {
        frame_active_ = false;
        frame_offset_ = 0;
        frame_size_ = 0;
        if (data.fin) {
            if (assembly_size_ == 0) {
                fail("empty_message");
                return;
            }
            enqueue(message_opcode_ == kText ? Event::Text : Event::Binary, assembly_, assembly_size_);
            clearAssembly();
        }
    }
}

bool VoiceClient::enqueue(Event event, const uint8_t *data, size_t size)
{
    if (stopping_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire)) {
        return false;
    }
    Message message{event, nullptr, size};
    if (event == Event::Text) {
        CancelAckIdentity identity;
        if (classifyCancelAck(data, size, identity)) {
            message.cancel_ack_state = identity.valid ? 1 : 2;
            message.cancel_ack_epoch = identity.epoch;
            message.cancel_ack_turn = identity.turn;
            message.cancel_ack_generation = identity.generation;
            message.cancel_ack_received_us = esp_timer_get_time();
            logCancelAck("received", message, uxQueueMessagesWaiting(messages_),
                         message.cancel_ack_received_us);
        }
    }
    if (size != 0) {
        message.data = allocateBytes(size + 1);
        if (message.data == nullptr) {
            fail("receive_no_memory", ESP_ERR_NO_MEM);
            return false;
        }
        std::memcpy(message.data, data, size);
        message.data[size] = 0;
    }
    const int64_t deadline = esp_timer_get_time() + 5'000'000;
    while (!stopping_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire)) {
        if (xQueueSend(messages_, &message, pdMS_TO_TICKS(20)) == pdTRUE) {
            observeMaximum(rx_queue_peak_, uxQueueMessagesWaiting(messages_));
            if (message.cancel_ack_state != 0) {
                const int64_t now = esp_timer_get_time();
                logCancelAck("enqueued", message, uxQueueMessagesWaiting(messages_), now,
                             static_cast<uint32_t>(std::clamp<int64_t>(
                                 now - message.cancel_ack_received_us, 0, UINT32_MAX)));
            }
            return true;
        }
        if (esp_timer_get_time() >= deadline) {
            // 不能丢掉数据包后继续 Opus 流。
            // 消费者停滞时终止会话；否则此等待用于施加 TCP 背压，
            // 同时播放任务消费其有界队列。
            fail("receive_queue_stalled", ESP_ERR_TIMEOUT);
            break;
        }
    }
    heap_caps_free(message.data);
    return false;
}

VoiceClient::Diagnostics VoiceClient::diagnostics() const
{
    Diagnostics snapshot;
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        snapshot = diagnostics_;
    }
    snapshotDispatch(&snapshot);
    return snapshot;
}

void VoiceClient::snapshotDispatch(Diagnostics *snapshot) const
{
    snapshot->rx_messages = rx_messages_.load(std::memory_order_relaxed);
    snapshot->rx_binary_messages = rx_binary_messages_.load(std::memory_order_relaxed);
    snapshot->rx_handler_last_us = rx_handler_last_us_.load(std::memory_order_relaxed);
    snapshot->rx_handler_max_us = rx_handler_max_us_.load(std::memory_order_relaxed);
    snapshot->rx_batch_max_us = rx_batch_max_us_.load(std::memory_order_relaxed);
    snapshot->rx_batch_messages_peak = rx_batch_messages_peak_.load(std::memory_order_relaxed);
    snapshot->rx_budget_waits = rx_budget_waits_.load(std::memory_order_relaxed);
    snapshot->rx_queue_peak = rx_queue_peak_.load(std::memory_order_relaxed);
    const uint32_t core = rx_core_.load(std::memory_order_relaxed);
    snapshot->rx_core = core == UINT32_MAX ? -1 : static_cast<int>(core);
}

void VoiceClient::logDispatchDiagnostics(const char *trigger) const
{
    logDispatch(diagnostics(), trigger);
}

void VoiceClient::logDispatch(const Diagnostics &snapshot, const char *trigger)
{
    ESP_LOGI(kTag, "dispatch_work trigger=%s messages=%u binary_messages=%u handler_last_us=%u "
             "handler_max_us=%u batch_max_us=%u batch_messages_peak=%u budget_waits=%u "
             "queue_peak=%u core=%d timing=wall scope=connection",
             trigger, static_cast<unsigned>(snapshot.rx_messages),
             static_cast<unsigned>(snapshot.rx_binary_messages),
             static_cast<unsigned>(snapshot.rx_handler_last_us),
             static_cast<unsigned>(snapshot.rx_handler_max_us),
             static_cast<unsigned>(snapshot.rx_batch_max_us),
             static_cast<unsigned>(snapshot.rx_batch_messages_peak),
             static_cast<unsigned>(snapshot.rx_budget_waits),
             static_cast<unsigned>(snapshot.rx_queue_peak), snapshot.rx_core);
}

void VoiceClient::recordFailure(const char *reason, esp_err_t result)
{
    Diagnostics snapshot;
    {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        if (diagnostics_.first_failure_us) return;
        diagnostics_.reason = reason;
        diagnostics_.first_failure_us = esp_timer_get_time();
        diagnostics_.result = result;
        snapshot = diagnostics_;
    }
    snapshotDispatch(&snapshot);
    ESP_LOGE(kTag, "first_failure reason=%s at_us=%lld result=%d close_code=%d socket_errno=%d error_type=%d handshake=%d send_count=%d send_us=%u",
             snapshot.reason, snapshot.first_failure_us, static_cast<int>(snapshot.result), snapshot.close_code,
             snapshot.socket_errno, snapshot.error_type, snapshot.handshake_status,
             snapshot.last_send_count, static_cast<unsigned>(snapshot.last_send_us));
    logDispatch(snapshot, "first_failure");
}

void VoiceClient::fail(const char *reason, esp_err_t result)
{
    connected_.store(false, std::memory_order_release);
    recordFailure(reason, result);
    failed_.store(true, std::memory_order_release);
}

void VoiceClient::dispatchEntry(void *argument)
{
    static_cast<VoiceClient *>(argument)->dispatch();
}

void VoiceClient::dispatch()
{
    bool failure_delivered = false;
    uint32_t batch_work_us = 0, batch_messages = 0;
    while (!stopping_.load(std::memory_order_acquire)) {
        // 此处未持有处理器、PCM 或音频流锁，也未持有接收缓冲。
        // 播放任务到达自身安全边界前持续让出 CPU；
        // 所属连接模块请求关闭时立即停止。
        if (kAecDispatch) {
            voice::audio_idle_guard::cooperate(stopping_, false, 100);
            if (stopping_.load(std::memory_order_acquire)) break;
        }
        if (failed_.load(std::memory_order_acquire)) {
            if (!failure_delivered) {
                failure_delivered = true;
                handler_(Event::Failure, nullptr, 0);
            }
            if (!stopping_.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        Message message{};
        if (xQueueReceive(messages_, &message, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (!stopping_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire)) {
                rx_core_.store(xPortGetCoreID(), std::memory_order_relaxed);
                if (message.cancel_ack_state != 0) {
                    const int64_t now = esp_timer_get_time();
                    logCancelAck("dequeued", message, uxQueueMessagesWaiting(messages_), now,
                                 static_cast<uint32_t>(std::clamp<int64_t>(
                                     now - message.cancel_ack_received_us, 0, UINT32_MAX)));
                }
                const int64_t started = esp_timer_get_time();
                handler_(message.event, message.data, message.size);
                const uint32_t elapsed = static_cast<uint32_t>(std::clamp<int64_t>(
                    esp_timer_get_time() - started, 0, UINT32_MAX));
                rx_messages_.fetch_add(1, std::memory_order_relaxed);
                if (message.event == Event::Binary)
                    rx_binary_messages_.fetch_add(1, std::memory_order_relaxed);
                rx_handler_last_us_.store(elapsed, std::memory_order_relaxed);
                observeMaximum(rx_handler_max_us_, elapsed);
                batch_work_us = static_cast<uint32_t>(std::min<uint64_t>(
                    static_cast<uint64_t>(batch_work_us) + elapsed, UINT32_MAX));
                ++batch_messages;
                observeMaximum(rx_batch_max_us_, batch_work_us);
                observeMaximum(rx_batch_messages_peak_, batch_messages);
            }
            heap_caps_free(message.data);
            if (batch_work_us >= kDispatchWorkBudgetUs || batch_messages >= kDispatchMessageBudget) {
                // 应用回调已完整返回并释放音频和流锁，消息缓冲也已释放。
                // 真正阻塞一个时钟节拍，给低优先级任务提供
                // taskYIELD 无法保证的运行机会。
                // 先进先出顺序和 PCM 样本保持不变。
                if (!stopping_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire)) {
                    rx_budget_waits_.fetch_add(1, std::memory_order_relaxed);
                    vTaskDelay(1);
                }
                batch_work_us = batch_messages = 0;
            }
        } else {
            // 空队列接收等待已经提供了有界空闲机会。
            batch_work_us = batch_messages = 0;
        }
    }
    xSemaphoreGive(dispatched_stop_);
    vTaskDeleteWithCaps(nullptr);
}
