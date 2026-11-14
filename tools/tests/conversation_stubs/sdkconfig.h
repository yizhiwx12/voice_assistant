#pragma once
#include "stub_api.hpp"
#define CONFIG_VOICE_WS_SERVER_URL "ws://127.0.0.1/ws/voice"
#define CONFIG_VOICE_AUDIO_SERVER_URL "http://127.0.0.1"
#define CONFIG_VOICE_SERVER_PROFILE "realtime_default"
#define CONFIG_VOICE_MIC_GAIN 1
#define CONFIG_VOICE_PLAYBACK_VOLUME 40
#define CONFIG_VOICE_UPLINK_SEND_TIMEOUT_MS 200
#define CONFIG_VOICE_AUTO_LISTEN_ENABLED 1
#define CONFIG_VOICE_AUTO_PREROLL_MS 200
#define CONFIG_VOICE_AUTO_START_CONFIRM_MS 160
#define CONFIG_VOICE_AUTO_START_MIN_AC_RMS 80
#define CONFIG_VOICE_AUTO_SILENCE_MS 700
#define CONFIG_VOICE_AUTO_HANDSHAKE_MS 2000
#define CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED 1
#define CONFIG_VOICE_BARGE_IN_ENABLED 1
#define CONFIG_VOICE_BARGE_IN_CONFIRM_MS 200
#define CONFIG_VOICE_BARGE_IN_MIN_AC_RMS 80
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
unsigned uxQueueSpacesAvailable(QueueHandle_t);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
unsigned esp_get_free_heap_size();
unsigned esp_get_minimum_free_heap_size();
unsigned heap_caps_get_free_size(int);
