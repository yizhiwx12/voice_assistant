#pragma once
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_NVS_NO_FREE_PAGES = 0x110d, ESP_ERR_NVS_NEW_VERSION_FOUND = 0x1110;
inline const char *esp_err_to_name(esp_err_t) { return "stub"; }
using esp_event_base_t = const char *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, int32_t, void *);
inline constexpr char WIFI_EVENT[] = "WIFI_EVENT", IP_EVENT[] = "IP_EVENT";
constexpr int ESP_EVENT_ANY_ID = -1, WIFI_EVENT_STA_START = 1, WIFI_EVENT_STA_DISCONNECTED = 2;
constexpr int IP_EVENT_STA_GOT_IP = 10, IP_EVENT_STA_LOST_IP = 11;
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t {
    struct {
        uint8_t ssid[32], password[64];
        struct { int authmode; } threshold;
        struct { bool capable, required; } pmf_cfg;
    } sta;
};
struct wifi_event_sta_disconnected_t { int reason = 0; };
struct ip_event_got_ip_t { struct { struct { uint32_t addr; } ip; } ip_info; };
using wifi_ps_type_t = int;
constexpr int WIFI_PS_MIN_MODEM = 1, WIFI_PS_NONE = 0, WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3;
constexpr int WIFI_MODE_STA = 1, WIFI_IF_STA = 0;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 0U, 0U, 0U, static_cast<unsigned>((ip)->addr)
esp_err_t nvs_flash_init();
esp_err_t nvs_flash_erase();
esp_err_t esp_netif_init();
void *esp_netif_create_default_wifi_sta();
esp_err_t esp_event_loop_create_default();
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
esp_err_t esp_wifi_connect();
esp_err_t esp_wifi_disconnect();
int64_t esp_timer_get_time();
void stubLog(const char *, ...);
#define ESP_LOGI(tag, ...) do { (void)(tag); stubLog(__VA_ARGS__); } while (false)
#define ESP_LOGW(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
