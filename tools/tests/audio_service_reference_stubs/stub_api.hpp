#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_INVALID_SIZE = 0x104, ESP_ERR_TIMEOUT = 0x107,
    ESP_ERR_NOT_FINISHED = 0x10C;
const char *esp_err_to_name(esp_err_t);
void serviceReferenceLog(const char *, ...);
#define ESP_LOGI(tag, ...) (static_cast<void>(tag), serviceReferenceLog(__VA_ARGS__))
#define ESP_LOGW(tag, ...) (static_cast<void>(tag), serviceReferenceLog(__VA_ARGS__))
#define ESP_LOGE(tag, ...) (static_cast<void>(tag), serviceReferenceLog(__VA_ARGS__))
using QueueHandle_t = void *;
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr int pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(value) (value)
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define configTICK_RATE_HZ 1000
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4;
void *heap_caps_malloc(size_t, int);
void *heap_caps_calloc(size_t, size_t, int);
void heap_caps_free(void *);
QueueHandle_t xQueueCreateWithCaps(unsigned, unsigned, int);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueuePeek(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueReset(QueueHandle_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDeleteWithCaps(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateBinary();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
TickType_t xTaskGetTickCount();
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
int64_t esp_timer_get_time();
#define IRAM_ATTR
