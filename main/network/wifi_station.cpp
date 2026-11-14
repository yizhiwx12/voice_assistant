#include "wifi_station.hpp"

#include <cstring>

#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "wifi_station";
constexpr uint8_t kMaximumRetries = 5;
constexpr uint32_t kConnectionCycleMs = 60000;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}

esp_err_t WifiStation::start()
{
    if (started_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (CONFIG_VOICE_WIFI_SSID[0] == '\0') {
        state_.store(State::NotConfigured, std::memory_order_release);
        ESP_LOGW(kTag, "Wi-Fi is not configured; set SSID and password in menuconfig");
        return ESP_OK;
    }
    if (std::strlen(CONFIG_VOICE_WIFI_SSID) >= sizeof(wifi_config_t::sta.ssid) ||
        std::strlen(CONFIG_VOICE_WIFI_PASSWORD) >= sizeof(wifi_config_t::sta.password)) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Wi-Fi SSID or password is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "NVS initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Network interface initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Default event loop creation failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_netif_create_default_wifi_sta() == nullptr) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Failed to create the default Wi-Fi station interface");
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Wi-Fi driver initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &WifiStation::eventHandler, this);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                         &WifiStation::eventHandler, this);
    }
    if (err != ESP_OK) {
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Wi-Fi event registration failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_config{};
    std::memcpy(wifi_config.sta.ssid, CONFIG_VOICE_WIFI_SSID,
                std::strlen(CONFIG_VOICE_WIFI_SSID));
    std::memcpy(wifi_config.sta.password, CONFIG_VOICE_WIFI_PASSWORD,
                std::strlen(CONFIG_VOICE_WIFI_PASSWORD));
    wifi_config.sta.threshold.authmode = CONFIG_VOICE_WIFI_PASSWORD[0] == '\0'
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        // 驱动可能发布 STA_START 之前，先发布初始连接周期。
        // start() 后不覆盖随后到达的 GOT_IP 或 Failed 事件。
        std::lock_guard<std::mutex> lock(connection_mutex_);
        started_ = true;
        connect_started_ms_.store(nowMs(), std::memory_order_release);
        state_.store(State::Connecting, std::memory_order_release);
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
#ifdef CONFIG_VOICE_WIFI_POWER_SAVE
        constexpr wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
#else
        constexpr wifi_ps_type_t power_save = WIFI_PS_NONE;
#endif
        err = esp_wifi_set_ps(power_save);
        if (err == ESP_OK) {
            ESP_LOGI(kTag, "Wi-Fi power save mode=%d", static_cast<int>(power_save));
        }
    }
    if (err != ESP_OK) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        state_.store(State::Failed, std::memory_order_release);
        ESP_LOGE(kTag, "Starting Wi-Fi failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "Connecting to the configured 2.4 GHz Wi-Fi network");
    return ESP_OK;
}

void WifiStation::poll()
{
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (state() != State::Connecting || !started_) return;
    const uint32_t started = connect_started_ms_.load(std::memory_order_acquire);
    if (nowMs() - started < kConnectionCycleMs) return;
    // 先设为 Failed，避免断开事件又启动一个连接周期。
    state_.store(State::Failed, std::memory_order_release);
    ip_address_.store(0, std::memory_order_relaxed);
    esp_wifi_disconnect();
    ESP_LOGE(kTag, "Wi-Fi connection cycle timed out after %u ms; restart or explicit retry required",
             static_cast<unsigned>(kConnectionCycleMs));
}

esp_err_t WifiStation::retry()
{
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (!started_) return ESP_ERR_INVALID_STATE;
    if (state() == State::Connected || state() == State::Connecting) return ESP_OK;
    retry_count_.store(0, std::memory_order_relaxed);
    connect_started_ms_.store(nowMs(), std::memory_order_release);
    state_.store(State::Connecting, std::memory_order_release);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) state_.store(State::Failed, std::memory_order_release);
    return err;
}

void WifiStation::eventHandler(void *argument, const char *event_base, int32_t event_id,
                               void *event_data)
{
    auto *station = static_cast<WifiStation *>(argument);
    station->handleEvent(event_base, event_id, event_data);
}

void WifiStation::handleEvent(const char *event_base, int32_t event_id, void *event_data)
{
    // 事件回调与主循环的截止时间和重试逻辑共用一把短时锁。
    // SDK 连接和断开接口只把工作入队，此处不持锁休眠。
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (std::strcmp(event_base, WIFI_EVENT) == 0 && event_id == WIFI_EVENT_STA_START) {
        if (!started_ || state() != State::Connecting) return;
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            state_.store(State::Failed, std::memory_order_release);
            ESP_LOGE(kTag, "Initial connection request failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (std::strcmp(event_base, WIFI_EVENT) == 0 && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ip_address_.store(0, std::memory_order_relaxed);
        // 新的网络中断只获得一个有界重连周期。
        // 超时或耗尽次数后保持停止，直到用户主动重试或重启。
        if (state() == State::Failed) return;
        if (state() == State::Connected) connect_started_ms_.store(nowMs(), std::memory_order_release);
        const uint8_t retries = retry_count_.load(std::memory_order_relaxed);
        if (retries < kMaximumRetries) {
            retry_count_.store(retries + 1, std::memory_order_relaxed);
            state_.store(State::Connecting, std::memory_order_release);
            const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
            ESP_LOGW(kTag, "Wi-Fi disconnected (reason=%d), retry %u/%u",
                     event->reason, static_cast<unsigned>(retries + 1), kMaximumRetries);
            const esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                state_.store(State::Failed, std::memory_order_release);
                ESP_LOGE(kTag, "Reconnect request failed: %s", esp_err_to_name(err));
            }
        } else {
            state_.store(State::Failed, std::memory_order_release);
            ESP_LOGE(kTag, "Wi-Fi connection failed after %u retries", kMaximumRetries);
        }
        return;
    }

    if (std::strcmp(event_base, IP_EVENT) == 0 && event_id == IP_EVENT_STA_GOT_IP) {
        if (!started_ || state() == State::Failed || state() == State::NotConfigured) return;
        const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);
        ip_address_.store(event->ip_info.ip.addr, std::memory_order_relaxed);
        retry_count_.store(0, std::memory_order_relaxed);
        state_.store(State::Connected, std::memory_order_release);
        ESP_LOGI(kTag, "Connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        return;
    }
    if (std::strcmp(event_base, IP_EVENT) == 0 && event_id == IP_EVENT_STA_LOST_IP) {
        if (!started_ || state() != State::Connected) return;
        ip_address_.store(0, std::memory_order_relaxed);
        retry_count_.store(0, std::memory_order_relaxed);
        connect_started_ms_.store(nowMs(), std::memory_order_release);
        state_.store(State::Connecting, std::memory_order_release);
        // 强制重新关联并执行 DHCP；对应断开事件也使用有限重试预算，
        // 避免保留过期的 Connected 状态和 IP。
        const esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK) state_.store(State::Failed, std::memory_order_release);
    }
}
