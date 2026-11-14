#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NOT_SUPPORTED = 0x106, ESP_ERR_TIMEOUT = 0x107;
using esp_event_base_t = const char *;
using QueueHandle_t = void *;
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
using TickType_t = unsigned;
using BaseType_t = int;
constexpr BaseType_t pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = ~TickType_t{0};
#define pdMS_TO_TICKS(value) (value)
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;

enum {
    WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_CONNECTED,
    WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_DATA, WEBSOCKET_EVENT_CLOSED,
};
enum esp_websocket_error_type_t {
    WEBSOCKET_ERROR_TYPE_NONE, WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,
    WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT, WEBSOCKET_ERROR_TYPE_HANDSHAKE,
    WEBSOCKET_ERROR_TYPE_SERVER_CLOSE,
};
struct esp_websocket_error_codes_t {
    int esp_tls_last_esp_err = 0, esp_tls_stack_err = 0, esp_tls_cert_verify_flags = 0;
    esp_websocket_error_type_t error_type = WEBSOCKET_ERROR_TYPE_NONE;
    int esp_ws_handshake_status_code = 0, esp_transport_sock_errno = 0;
};
using esp_websocket_client_handle_t = void *;
struct esp_websocket_event_data_t {
    const char *data_ptr = nullptr;
    int data_len = 0;
    bool fin = true;
    uint8_t op_code = 0;
    esp_websocket_client_handle_t client = nullptr;
    void *user_context = nullptr;
    int payload_len = 0, payload_offset = 0;
    esp_websocket_error_codes_t error_handle{};
    int close_status_code = 0;
};
struct esp_websocket_client_config_t {
    const char *uri = nullptr;
    bool disable_auto_reconnect = false, enable_close_reconnect = false;
    int task_prio = 0, task_stack = 0;
    const char *task_name = nullptr;
    int buffer_size = 0;
    bool keep_alive_enable = false;
    int keep_alive_idle = 0, keep_alive_interval = 0, keep_alive_count = 0;
    int network_timeout_ms = 0, ping_interval_sec = 0, pingpong_timeout_sec = 0;
};
using StubEventCallback = void (*)(void *, esp_event_base_t, int32_t, void *);
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateBinary();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle();
BaseType_t xTaskCreateWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned,
                                        TaskHandle_t *, int, int);
int xPortGetCoreID();
void vTaskDeleteWithCaps(void *);
void vTaskDelay(unsigned);
void *heap_caps_malloc(size_t, int);
void heap_caps_free(void *);
int64_t esp_timer_get_time();
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t, int, StubEventCallback, void *);
esp_err_t esp_websocket_unregister_events(esp_websocket_client_handle_t, int, StubEventCallback);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t, const char *, int, TickType_t);
int esp_websocket_client_send_text(esp_websocket_client_handle_t, const char *, int, TickType_t);
void stubLog(const char *, ...);
#define ESP_LOGI(tag, ...) stubLog(__VA_ARGS__)
#define ESP_LOGE(tag, ...) stubLog(__VA_ARGS__)
#define ESP_LOGW(tag, ...) stubLog(__VA_ARGS__)
