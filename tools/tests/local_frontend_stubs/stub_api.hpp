#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
constexpr esp_err_t ESP_ERR_NOT_FINISHED = 0x10C;
const char *esp_err_to_name(esp_err_t);
void stubLog(const char *, ...);
#define ESP_LOGI(tag, ...) (static_cast<void>(tag), stubLog(__VA_ARGS__))
#define ESP_LOGE(tag, ...) (static_cast<void>(tag), stubLog(__VA_ARGS__))

using TickType_t = uint32_t;
using BaseType_t = int;
using QueueHandle_t = void *;
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
constexpr int MALLOC_CAP_SPIRAM = 1;
constexpr int MALLOC_CAP_8BIT = 2;
void *heap_caps_malloc(size_t, int);
void heap_caps_free(void *);

QueueHandle_t xQueueCreateWithCaps(unsigned, unsigned, int);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDeleteWithCaps(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateBinary();
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xTaskCreateWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned,
                             TaskHandle_t *, int);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned,
                                         TaskHandle_t *, BaseType_t, int);
void vTaskDeleteWithCaps(TaskHandle_t);
void vTaskDelay(TickType_t);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
int64_t esp_timer_get_time();

struct srmodel_list_t { int unused = 0; };
enum vad_state_t { VAD_SILENCE, VAD_SPEECH };
constexpr int AFE_TYPE_SR = 0;
constexpr int AFE_TYPE_FD = 3;
constexpr int AFE_MODE_LOW_COST = 0;
constexpr int VAD_MODE_0 = 0;
constexpr int AFE_MEMORY_ALLOC_MORE_PSRAM = 2;
constexpr int AEC_MODE_SR_LOW_COST = 0;
constexpr int AEC_MODE_FD_LOW_COST = 5;
constexpr int AEC_NLP_LEVEL_NORMAL = 0;
constexpr int AEC_NLP_LEVEL_AGGR = 1;
constexpr int AEC_NLP_LEVEL_VERYAGGR = 2;
struct afe_pcm_config_t {
    int total_ch_num = 0;
    int mic_num = 0;
    uint8_t *mic_ids = nullptr;
    int ref_num = 0;
    uint8_t *ref_ids = nullptr;
    int sample_rate = 0;
};
struct afe_config_t {
    char *vad_model_name = nullptr;
    bool vad_init = false;
    int vad_mode = 0;
    int vad_min_speech_ms = 0;
    int vad_min_noise_ms = 0;
    int vad_delay_ms = 0;
    bool vad_mute_playback = false;
    bool aec_init = false;
    int aec_mode = AEC_MODE_SR_LOW_COST;
    int aec_filter_length = 4;
    int aec_nlp_level = 1;
    bool se_init = false;
    bool ns_init = false;
    bool agc_init = false;
    bool wakenet_init = false;
    char *wakenet_model_name = nullptr;
    char *wakenet_model_name_2 = nullptr;
    int memory_alloc_mode = 0;
    int afe_ringbuf_size = 0;
    int afe_perferred_core = 0;
    int afe_perferred_priority = 0;
    float afe_linear_gain = 1;
    bool fixed_first_channel = false;
    bool fixed_output_channel = false;
    bool output_playback_channel = false;
    afe_pcm_config_t pcm_config{};
    int afe_type = AFE_TYPE_SR;
    int afe_mode = AFE_MODE_LOW_COST;
};
struct esp_afe_sr_data_t;
struct afe_fetch_result_t {
    int16_t *data = nullptr;
    int data_size = 0;
    int16_t *vad_cache = nullptr;
    int vad_cache_size = 0;
    float data_volume = 0;
    vad_state_t vad_state = VAD_SILENCE;
    int ret_value = ESP_OK;
};
struct esp_afe_sr_iface_t {
    esp_afe_sr_data_t *(*create_from_config)(afe_config_t *);
    int (*get_feed_chunksize)(esp_afe_sr_data_t *);
    int (*get_fetch_chunksize)(esp_afe_sr_data_t *);
    int (*get_samp_rate)(esp_afe_sr_data_t *);
    int (*get_feed_channel_num)(esp_afe_sr_data_t *);
    int (*get_fetch_channel_num)(esp_afe_sr_data_t *);
    int (*feed)(esp_afe_sr_data_t *, const int16_t *);
    afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *, TickType_t);
    void (*destroy)(esp_afe_sr_data_t *);
    void (*print_pipeline)(esp_afe_sr_data_t *);
    float (*get_vad_energy_threshold)(esp_afe_sr_data_t *);
    float (*get_vadnet_threshold)(esp_afe_sr_data_t *);
};
afe_config_t *afe_config_init(const char *, srmodel_list_t *, int, int);
afe_config_t *afe_config_check(afe_config_t *);
void afe_config_free(afe_config_t *);
const esp_afe_sr_iface_t *esp_afe_handle_from_config(afe_config_t *);
