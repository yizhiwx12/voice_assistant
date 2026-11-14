// 验证生产 VoiceClient，仅替换 ESP-IDF/RTOS 调用。
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#define private public
#include "voice_client.hpp"
#undef private
#include "sdkconfig.h"
#include "app/audio_idle_guard.hpp"

namespace {
struct Queue { size_t capacity, item_size; std::deque<std::vector<uint8_t>> items; };
StubEventCallback callback;
void *callback_owner;
int64_t clock_us = 1000;
int send_count = -2;  // -2 表示完整传输。
bool lock_available = true;
std::string logs;
int created_core = -2;
unsigned created_priority = 0, created_stack = 0;
int task_failures_remaining = 0;
std::vector<int> task_attempt_cores;
std::vector<TickType_t> delays;
std::vector<void *> live_allocations;
std::function<void(TickType_t)> delay_hook;
std::function<void(QueueHandle_t, TickType_t)> send_hook;
bool in_handler = false;
void *last_handler_buffer = nullptr;
unsigned idle_guard_checks = 0;
void event(int id, esp_websocket_event_data_t &data) { callback(callback_owner, nullptr, id, &data); }
void open(VoiceClient &client)
{
    assert(client.start("ws://127.0.0.1/ws/voice", [](auto, auto, auto) {}) == ESP_OK);
    assert(created_core == (CONFIG_VOICE_AEC_OBSERVATION_ENABLED ? 0 : -1));
    assert(created_priority == 5 && created_stack == 32768);
    esp_websocket_event_data_t connected;
    event(WEBSOCKET_EVENT_CONNECTED, connected);
    assert(client.connected());
}
BaseType_t createTask(unsigned stack, unsigned priority, TaskHandle_t *handle, int core)
{
    created_core = core;
    created_priority = priority;
    created_stack = stack;
    task_attempt_cores.push_back(core);
    if (task_failures_remaining) { --task_failures_remaining; return 0; }
    *handle = reinterpret_cast<void *>(1);
    return pdPASS;
}
}

namespace voice::audio_idle_guard {
esp_err_t initialize() { return ESP_OK; }
bool cooperate(const std::atomic<bool> &lifecycle, bool active_value, uint32_t max_wait_ms)
{
    assert(!in_handler && !active_value && max_wait_ms == 100);
    ++idle_guard_checks;
    return lifecycle.load() == active_value;
}
}

QueueHandle_t xQueueCreate(unsigned count, unsigned bytes) { return new Queue{count, bytes, {}}; }
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t ticks)
{
    auto &queue = *static_cast<Queue *>(handle);
    if (send_hook) send_hook(handle, ticks);
    if (queue.items.size() == queue.capacity) return 0;
    const auto *bytes = static_cast<const uint8_t *>(item);
    queue.items.emplace_back(bytes, bytes + queue.item_size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t)
{
    auto &queue = *static_cast<Queue *>(handle);
    if (queue.items.empty()) return 0;
    std::memcpy(item, queue.items.front().data(), queue.item_size);
    queue.items.pop_front();
    return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t handle)
{ return static_cast<unsigned>(static_cast<Queue *>(handle)->items.size()); }
void vQueueDelete(QueueHandle_t handle) { delete static_cast<Queue *>(handle); }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new int(0); }
SemaphoreHandle_t xSemaphoreCreateBinary() { return new int(0); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return lock_available ? pdTRUE : 0; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t handle) { delete static_cast<int *>(handle); }
TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }
BaseType_t xTaskCreateWithCaps(void (*)(void *), const char *, unsigned stack, void *, unsigned priority,
                             TaskHandle_t *handle, int)
{ return createTask(stack, priority, handle, -1); }
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, unsigned stack, void *, unsigned priority,
                                         TaskHandle_t *handle, int core, int)
{ return createTask(stack, priority, handle, core); }
int xPortGetCoreID() { return CONFIG_VOICE_AEC_OBSERVATION_ENABLED ? 0 : 1; }
void vTaskDeleteWithCaps(void *) {}
void vTaskDelay(unsigned ticks)
{
    assert(!in_handler);
    assert(std::find(live_allocations.begin(), live_allocations.end(), last_handler_buffer) == live_allocations.end());
    delays.push_back(ticks);
    clock_us += ticks * 1000;
    if (delay_hook) delay_hook(ticks);
}
void *heap_caps_malloc(size_t bytes, int)
{
    void *pointer = std::malloc(bytes);
    assert(pointer);
    live_allocations.push_back(pointer);
    return pointer;
}
void heap_caps_free(void *pointer)
{
    if (pointer) {
        auto found = std::find(live_allocations.begin(), live_allocations.end(), pointer);
        assert(found != live_allocations.end());
        live_allocations.erase(found);
    }
    std::free(pointer);
}
int64_t esp_timer_get_time() { clock_us += 100; return clock_us; }
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *)
{ return reinterpret_cast<void *>(2); }
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t, int, StubEventCallback fn, void *owner)
{ callback = fn; callback_owner = owner; return ESP_OK; }
esp_err_t esp_websocket_unregister_events(esp_websocket_client_handle_t, int, StubEventCallback) { return ESP_OK; }
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t) { return ESP_OK; }
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t)
{
    esp_websocket_event_data_t stopped;
    event(WEBSOCKET_EVENT_DISCONNECTED, stopped);  // 本地停止回调属于预期清理。
    return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t) { return ESP_OK; }
int esp_websocket_client_send_bin(esp_websocket_client_handle_t, const char *, int bytes, TickType_t)
{ return send_count == -2 ? bytes : send_count; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t handle, const char *data, int bytes, TickType_t ticks)
{ return esp_websocket_client_send_bin(handle, data, bytes, ticks); }
void stubLog(const char *format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    logs += text;
    logs += '\n';
}

int main()
{
    VoiceClient client;
    open(client);
    esp_websocket_event_data_t error;
    error.data_ptr = "SECRET-RAW-ERROR ws://password@example.invalid/?key=secret";
    error.data_len = static_cast<int>(std::strlen(error.data_ptr));
    error.error_handle.esp_transport_sock_errno = 104;
    error.error_handle.error_type = WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
    error.error_handle.esp_ws_handshake_status_code = 403;
    event(WEBSOCKET_EVENT_ERROR, error);
    const auto first = client.diagnostics();
    assert(std::string(first.reason) == "websocket_error" && first.first_failure_us > 0);
    assert(first.socket_errno == 104 && first.handshake_status == 403);
    error.close_status_code = 1008;
    event(WEBSOCKET_EVENT_DISCONNECTED, error);
    client.fail("receive_queue_stalled", ESP_ERR_TIMEOUT);
    const auto later = client.diagnostics();
    assert(std::string(later.reason) == first.reason && later.first_failure_us == first.first_failure_us);
    assert(later.close_code == 1008 && later.result == first.result);
    assert(logs.find("SECRET") == std::string::npos && logs.find("password") == std::string::npos);
    assert(client.stop() == ESP_OK);

    open(client);
    assert(client.diagnostics().first_failure_us == 0 && client.diagnostics().close_code == 0);
    // 非 TCP 错误事件不会初始化传输 errno 字段。
    error.error_handle.error_type = WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT;
    error.error_handle.esp_transport_sock_errno = 1007858696;
    error.error_handle.esp_ws_handshake_status_code = 0;
    event(WEBSOCKET_EVENT_ERROR, error);
    const auto non_tcp = client.diagnostics();
    assert(non_tcp.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT);
    assert(non_tcp.socket_errno == 0);
    client.stop();

    open(client);
    assert(client.diagnostics().first_failure_us == 0 && client.diagnostics().close_code == 0);
    const char close_bytes[] = {char(0x03), char(0xF0), 'S', 'E', 'C', 'R', 'E', 'T'};  // 关闭码 1008。
    esp_websocket_event_data_t close;
    close.op_code = 8;
    close.data_ptr = close_bytes;
    close.data_len = close.payload_len = sizeof(close_bytes);
    event(WEBSOCKET_EVENT_DATA, close);
    assert(!client.connected());
    assert(std::string(client.diagnostics().reason) == "peer_close_frame");
    assert(client.diagnostics().close_code == 1008);
    assert(logs.find("SECRET") == std::string::npos);
    client.stop();

    open(client);
    // WebSocket 回调可能在 voice_rx 分发之前的 session.closed JSON 前就看到关闭。
    // 正常关闭必须保留 FIFO，让应用分类，不能直接锁存传输故障。
    auto &queue = *static_cast<Queue *>(client.messages_);
    queue.items.clear();  // 持有者已经处理 Connected。
    const char idle_json[] = "{\"type\":\"session.closed\",\"reason\":\"idle_timeout\"}";
    esp_websocket_event_data_t idle;
    idle.op_code = 1;
    idle.fin = true;
    idle.data_ptr = idle_json;
    idle.data_len = idle.payload_len = sizeof(idle_json) - 1;
    event(WEBSOCKET_EVENT_DATA, idle);
    const char normal_bytes[] = {char(0x03), char(0xE8)};  // 关闭码 1000。
    close.data_ptr = normal_bytes;
    close.data_len = close.payload_len = sizeof(normal_bytes);
    event(WEBSOCKET_EVENT_DATA, close);
    esp_websocket_event_data_t disconnected;
    disconnected.close_status_code = 1000;
    event(WEBSOCKET_EVENT_DISCONNECTED, disconnected);
    assert(client.diagnostics().close_code == 1000);
    assert(client.diagnostics().first_failure_us == 0 && !client.failed_.load());
    VoiceClient::Message received{};
    assert(xQueueReceive(client.messages_, &received, 0) == pdTRUE);
    assert(received.event == VoiceClient::Event::Text);
    assert(std::string(reinterpret_cast<const char *>(received.data), received.size) == idle_json);
    heap_caps_free(received.data);
    assert(xQueueReceive(client.messages_, &received, 0) == pdTRUE);
    assert(received.event == VoiceClient::Event::Disconnected);
    client.stop();

    open(client);
    client.stop();
    assert(client.diagnostics().first_failure_us == 0);
    assert(logs.find("origin=local") != std::string::npos);

    // 只有 SDK 分片与 WebSocket 延续帧都完整后，真实 cJSON 才识别转义类型。
    // 排队 ACK 仍位于较早二进制数据之后，
    // 其跟踪记录在持有者回调前输出。
    open(client);
    VoiceClient::Message ack_pending{};
    assert(xQueueReceive(client.messages_, &ack_pending, 0) == pdTRUE); // 已连接。
    const uint8_t older_audio[] = {9};
    assert(client.enqueue(VoiceClient::Event::Binary, older_audio, sizeof(older_audio)));
    const std::string ack_json = R"({"type":"\u0072esponse.\u0063ancelled","session_epoch":4294967295,"turn_id":7,"generation":2,"ignored":"SECRET-RAW-ACK ws://password@example.invalid/?key=secret"})";
    const size_t ack_log_start = logs.size();
    esp_websocket_event_data_t ack_fragment;
    ack_fragment.op_code = 1;
    ack_fragment.fin = false;
    ack_fragment.data_ptr = ack_json.data();
    ack_fragment.payload_len = static_cast<int>(ack_json.size() / 2);
    ack_fragment.data_len = 12;
    event(WEBSOCKET_EVENT_DATA, ack_fragment);
    assert(uxQueueMessagesWaiting(client.messages_) == 1 && logs.size() == ack_log_start);
    ack_fragment.payload_offset = 12;
    ack_fragment.data_ptr = ack_json.data() + 12;
    ack_fragment.data_len = ack_fragment.payload_len - 12;
    event(WEBSOCKET_EVENT_DATA, ack_fragment);
    assert(uxQueueMessagesWaiting(client.messages_) == 1 && logs.size() == ack_log_start);
    ack_fragment.op_code = 0;
    ack_fragment.fin = true;
    ack_fragment.payload_offset = 0;
    ack_fragment.data_ptr = ack_json.data() + ack_json.size() / 2;
    ack_fragment.data_len = ack_fragment.payload_len = static_cast<int>(ack_json.size() - ack_json.size() / 2);
    event(WEBSOCKET_EVENT_DATA, ack_fragment);
    assert(uxQueueMessagesWaiting(client.messages_) == 2);
    assert(logs.find("phase=received valid=1 epoch=4294967295 turn=7 generation=2 queue_depth=1", ack_log_start) != std::string::npos);
    assert(logs.find("phase=enqueued valid=1 epoch=4294967295 turn=7 generation=2 queue_depth=2", ack_log_start) != std::string::npos);
    clock_us += 200000;
    unsigned ack_delivered = 0;
    client.handler_ = [&](auto kind, const uint8_t *data, size_t size) {
        last_handler_buffer = const_cast<uint8_t *>(data);
        if (++ack_delivered == 1) {
            assert(kind == VoiceClient::Event::Binary && size == 1 && *data == 9);
        } else {
            assert(kind == VoiceClient::Event::Text && std::string(reinterpret_cast<const char *>(data), size) == ack_json);
            assert(logs.find("phase=dequeued valid=1 epoch=4294967295 turn=7 generation=2 queue_depth=0", ack_log_start) != std::string::npos);
            client.stopping_.store(true);
        }
    };
    client.dispatch();
    assert(ack_delivered == 2);
    assert(logs.find("SECRET", ack_log_start) == std::string::npos && logs.find("password", ack_log_start) == std::string::npos);
    client.stop();

    // 分类永不改变交付。非 ACK、坏 JSON 或过大的诊断候选不标记；
    // 类型精确匹配但身份错误的 ACK 仅记录 valid=0 和数值零，绝不记录原值。
    open(client);
    assert(xQueueReceive(client.messages_, &ack_pending, 0) == pdTRUE);
    const std::vector<std::pair<std::string, unsigned>> classifications = {
        {R"({"type":"asr.partial","text":"response.cancelled SECRET"})", 0},
        {R"({"type":"response.cancelled.extra","session_epoch":1,"turn_id":1,"generation":1})", 0},
        {R"({"type":"response.cancelled","session_epoch":1,"turn_id":1,"generation":1} SECRET)", 0},
        {R"({"type":"response.cancelled","session_epoch":)", 0},
        {R"({"type":"response.cancelled","session_epoch":"SECRET","turn_id":1,"generation":1})", 2},
        {R"({"type":"response.cancelled","session_epoch":4294967296,"turn_id":1,"generation":1})", 2},
        {R"({"type":"response.cancelled","session_epoch":1.5,"turn_id":1,"generation":1})", 2},
        {R"({"type":"response.cancelled","session_epoch":1,"turn_id":-1,"generation":1})", 2},
        {R"({"type":"response.cancelled","session_epoch":1,"turn_id":1,"generation":0})", 2},
        {R"({"type":"response.cancelled","session_epoch":1,"turn_id":1})", 2},
        {R"({"type":"response.cancelled","session_epoch":1,"turn_id":1,"generation":1,"ignored":[[[[[[[[0]]]]]]]]})", 0},
        {R"({"type":"response.cancelled","session_epoch":1,"turn_id":1,"generation":1,"ignored":")" + std::string(512, 'x') + "\"}", 0},
    };
    for (const auto &entry : classifications) {
        const size_t before = logs.size();
        assert(client.enqueue(VoiceClient::Event::Text, reinterpret_cast<const uint8_t *>(entry.first.data()), entry.first.size()));
        assert(xQueueReceive(client.messages_, &ack_pending, 0) == pdTRUE);
        assert(ack_pending.cancel_ack_state == entry.second);
        assert(ack_pending.cancel_ack_epoch == 0 && ack_pending.cancel_ack_turn == 0 && ack_pending.cancel_ack_generation == 0);
        assert(std::string(reinterpret_cast<const char *>(ack_pending.data), ack_pending.size) == entry.first);
        if (entry.second) assert(logs.find("phase=received valid=0 epoch=0 turn=0 generation=0", before) != std::string::npos);
        else assert(logs.size() == before);
        heap_caps_free(ack_pending.data);
    }
    client.stop();

    // 满 FIFO 报告既有阻塞入队时长；
    // 这些诊断不引入优先队列或 ACK 特殊旁路。
    open(client);
    assert(xQueueReceive(client.messages_, &ack_pending, 0) == pdTRUE);
    for (size_t i = 0; i < VoiceClient::kQueueDepth; ++i) assert(client.enqueue(VoiceClient::Event::Binary));
    const size_t blocked_log_start = logs.size();
    unsigned blocked_sends = 0;
    send_hook = [&](QueueHandle_t handle, TickType_t ticks) {
        assert(ticks == 20);
        ++blocked_sends;
        clock_us += 20000;
        VoiceClient::Message drained{};
        assert(xQueueReceive(handle, &drained, 0) == pdTRUE && drained.data == nullptr);
    };
    assert(client.enqueue(VoiceClient::Event::Text, reinterpret_cast<const uint8_t *>(ack_json.data()), ack_json.size()));
    send_hook = {};
    assert(blocked_sends == 1 && uxQueueMessagesWaiting(client.messages_) == VoiceClient::kQueueDepth);
    const auto enqueued_log = logs.find("phase=enqueued", blocked_log_start);
    assert(enqueued_log != std::string::npos);
    const auto waited_field = logs.find("wait_us=", enqueued_log);
    assert(waited_field != std::string::npos && std::strtoul(logs.c_str() + waited_field + 8, nullptr, 10) >= 20000);
    client.stop();

    open(client);
    send_count = 3;
    const uint8_t audio[] = {1, 2, 3, 4};
    assert(client.sendBinary(audio, sizeof(audio), 200) == ESP_ERR_TIMEOUT);
    assert(!client.connected() && client.failed_.load());
    const auto partial = client.diagnostics();
    assert(std::string(partial.reason) == "send_incomplete" && partial.last_send_count == 3);
    assert(partial.last_send_us > 0);
    client.stop();

    open(client);
    lock_available = false;
    assert(client.sendBinary(audio, sizeof(audio), 200) == ESP_ERR_TIMEOUT);
    assert(std::string(client.diagnostics().reason) == "send_lock_timeout");
    lock_available = true;
    client.stop();

    // 两次分配尝试都必须保留请求的核亲和性及栈设置。
    task_attempt_cores.clear();
    task_failures_remaining = 1;
    open(client);
    assert(task_attempt_cores.size() == 2);
    for (int core : task_attempt_cores)
        assert(core == (CONFIG_VOICE_AEC_OBSERVATION_ENABLED ? 0 : -1));
    client.stop();

    // 验证生产分发循环，不测试预算模型。应用音频/控制回调流
    // 必须保持 FIFO、释放前一消息，并在累计回调工作后真正阻塞。
    open(client);
    VoiceClient::Message pending{};
    assert(xQueueReceive(client.messages_, &pending, 0) == pdTRUE);  // 已连接。
    assert(pending.data == nullptr);
    std::vector<std::pair<VoiceClient::Event, uint8_t>> expected, delivered;
    for (uint8_t i = 0; i < 12; ++i) {
        const auto kind = i == 6 ? VoiceClient::Event::Text :
            i == 11 ? VoiceClient::Event::Disconnected : VoiceClient::Event::Binary;
        expected.emplace_back(kind, i);
        assert(client.enqueue(kind, &i, 1));
    }
    delays.clear();
    client.handler_ = [&](auto kind, const uint8_t *data, size_t size) {
        in_handler = true;
        assert(size == 1 && data);
        last_handler_buffer = const_cast<uint8_t *>(data);
        delivered.emplace_back(kind, *data);
        clock_us += 3000;
        in_handler = false;
        if (delivered.size() == expected.size()) client.stopping_.store(true);
    };
    client.dispatch();
    assert(delivered == expected);
    assert(delays == std::vector<TickType_t>({1, 1, 1}));
    auto work = client.diagnostics();
    assert(work.rx_messages == 12 && work.rx_binary_messages == 10);
    assert(work.rx_handler_last_us == 3100 && work.rx_handler_max_us == 3100);
    assert(work.rx_batch_max_us == 9300 && work.rx_batch_messages_peak == 3);
    assert(work.rx_budget_waits == 3 && work.rx_queue_peak == 12);
    assert(work.rx_core == (CONFIG_VOICE_AEC_OBSERVATION_ENABLED ? 0 : 1));
    client.logDispatchDiagnostics();
    assert(logs.find("trigger=manual messages=12 binary_messages=10") != std::string::npos);
    assert(client.stop() == ESP_OK);

    // 极短回调仍通过消息数量上限获得有界机会。
    open(client);
    assert(client.diagnostics().rx_messages == 0 && client.diagnostics().rx_budget_waits == 0);
    assert(xQueueReceive(client.messages_, &pending, 0) == pdTRUE);
    unsigned short_delivered = 0;
    for (uint8_t i = 0; i < 10; ++i) assert(client.enqueue(VoiceClient::Event::Binary, &i, 1));
    delays.clear();
    client.handler_ = [&](auto, const uint8_t *data, size_t) {
        last_handler_buffer = const_cast<uint8_t *>(data);
        if (++short_delivered == 10) client.stopping_.store(true);
    };
    client.dispatch();
    work = client.diagnostics();
    assert(short_delivered == 10 && delays == std::vector<TickType_t>({1}));
    assert(work.rx_batch_messages_peak == 8 && work.rx_batch_max_us == 800);
    assert(work.rx_budget_waits == 1);
    client.stop();

    // 慢回调内部失败会在下一个排队包或预算等待前交付，
    // 独立持有者随后可安全停止。
    open(client);
    assert(xQueueReceive(client.messages_, &pending, 0) == pdTRUE);
    for (uint8_t i = 0; i < 6; ++i) assert(client.enqueue(VoiceClient::Event::Binary, &i, 1));
    std::vector<VoiceClient::Event> failure_events;
    delays.clear();
    client.handler_ = [&](auto kind, const uint8_t *data, size_t) {
        failure_events.push_back(kind);
        if (kind == VoiceClient::Event::Binary) {
            last_handler_buffer = const_cast<uint8_t *>(data);
            clock_us += 20000;
            client.fail("test_callback_failure");
        } else {
            assert(kind == VoiceClient::Event::Failure);
            client.stopping_.store(true);
        }
    };
    client.dispatch();
    assert(failure_events == std::vector<VoiceClient::Event>({VoiceClient::Event::Binary, VoiceClient::Event::Failure}));
    assert(delays.empty() && uxQueueMessagesWaiting(client.messages_) == 5);
    assert(client.diagnostics().rx_messages == 1 && client.diagnostics().rx_budget_waits == 0);
    client.stop();

    // 持有者在回调期间请求停止时，也会阻止下一包及额外延时。
    // 剩余缓冲由正常清理释放。
    open(client);
    assert(xQueueReceive(client.messages_, &pending, 0) == pdTRUE);
    for (uint8_t i = 0; i < 6; ++i) assert(client.enqueue(VoiceClient::Event::Binary, &i, 1));
    unsigned stop_delivered = 0;
    delays.clear();
    client.handler_ = [&](auto, const uint8_t *data, size_t) {
        last_handler_buffer = const_cast<uint8_t *>(data);
        ++stop_delivered;
        clock_us += 20000;
        client.stopping_.store(true);
    };
    client.dispatch();
    assert(stop_delivered == 1 && delays.empty() && uxQueueMessagesWaiting(client.messages_) == 5);
    client.stop();
    last_handler_buffer = nullptr;
    assert(live_allocations.empty());
    assert((idle_guard_checks > 0) == static_cast<bool>(CONFIG_VOICE_AEC_OBSERVATION_ENABLED));
    assert(logs.find("dispatch_work") != std::string::npos && logs.find("timing=wall") != std::string::npos);
    assert(logs.find("SECRET") == std::string::npos && logs.find("password") == std::string::npos);
    std::printf("Passed 15 production VoiceClient scenarios, AEC=%d (failure/protocol diagnostics; fragmented/escaped cancellation ACK traces and blocked FIFO; affinity fallback; real time/count budget waits with mixed FIFO and buffer release; prompt failure/stop); no raw payload logged. RTOS CPU share/WDT remain hardware tests.\n", CONFIG_VOICE_AEC_OBSERVATION_ENABLED);
}
