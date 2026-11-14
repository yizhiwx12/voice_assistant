#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#define private public
#include "network/wifi_station.hpp"
#undef private
#include "stub_api.hpp"

namespace {
std::atomic<int64_t> clock_us{1000};
std::atomic<unsigned> connect_calls{0}, disconnect_calls{0};
esp_err_t connect_result = ESP_OK, disconnect_result = ESP_OK, power_result = ESP_OK;
esp_event_handler_t wifi_handler = nullptr, ip_handler = nullptr;
void *wifi_argument = nullptr, *ip_argument = nullptr;
std::function<void()> power_hook;

void defaults() {
    clock_us.store(1000); connect_calls.store(0); disconnect_calls.store(0);
    connect_result = disconnect_result = power_result = ESP_OK;
    power_hook = {};
}
void event(WifiStation &station, const char *base, int id) {
    wifi_event_sta_disconnected_t disconnected{201};
    ip_event_got_ip_t address{};
    address.ip_info.ip.addr = 42;
    station.handleEvent(base, id, base == WIFI_EVENT ? static_cast<void *>(&disconnected) : &address);
}
void start(WifiStation &station) {
    assert(station.start() == ESP_OK);
    assert(station.state() == WifiStation::State::Connecting);
    assert(wifi_handler && ip_handler && wifi_argument == &station && ip_argument == &station);
    event(station, WIFI_EVENT, WIFI_EVENT_STA_START);
    assert(connect_calls.load() == 1);
}
}

esp_err_t nvs_flash_init() { return ESP_OK; }
esp_err_t nvs_flash_erase() { return ESP_OK; }
esp_err_t esp_netif_init() { return ESP_OK; }
void *esp_netif_create_default_wifi_sta() { return reinterpret_cast<void *>(1); }
esp_err_t esp_event_loop_create_default() { return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *arg) {
    assert(id == ESP_EVENT_ANY_ID);
    if (!std::strcmp(base, WIFI_EVENT)) { wifi_handler = handler; wifi_argument = arg; }
    else { ip_handler = handler; ip_argument = arg; }
    return ESP_OK;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(int) { return ESP_OK; }
esp_err_t esp_wifi_set_config(int, const wifi_config_t *) { return ESP_OK; }
esp_err_t esp_wifi_start() { return ESP_OK; }
esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { if (power_hook) power_hook(); return power_result; }
esp_err_t esp_wifi_connect() { ++connect_calls; return connect_result; }
esp_err_t esp_wifi_disconnect() { ++disconnect_calls; return disconnect_result; }
int64_t esp_timer_get_time() { return clock_us.load(); }
void stubLog(const char *, ...) {}

int main() {
    {
        defaults(); WifiStation station; start(station);
        event(station, IP_EVENT, IP_EVENT_STA_GOT_IP);
        assert(station.state() == WifiStation::State::Connected && station.ipAddress() == 42);
        clock_us.store(100000000); station.poll();
        assert(station.state() == WifiStation::State::Connected && !disconnect_calls.load());
    }
    {
        // start() 返回前交付的事件绝不会被覆盖。
        defaults(); WifiStation station;
        power_hook = [&] { event(station, WIFI_EVENT, WIFI_EVENT_STA_START); event(station, IP_EVENT, IP_EVENT_STA_GOT_IP); };
        assert(station.start() == ESP_OK && station.state() == WifiStation::State::Connected);
        assert(station.ipAddress() == 42 && connect_calls.load() == 1);
    }
    {
        defaults(); WifiStation station; start(station);
        clock_us.store(60000000); station.poll();
        assert(station.state() == WifiStation::State::Connecting);
        clock_us.store(60001000); station.poll();
        assert(station.state() == WifiStation::State::Failed && disconnect_calls.load() == 1);
        event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        event(station, IP_EVENT, IP_EVENT_STA_GOT_IP);
        assert(station.state() == WifiStation::State::Failed && !station.ipAddress());
        assert(connect_calls.load() == 1);
        assert(station.retry() == ESP_OK && station.state() == WifiStation::State::Connecting);
        event(station, IP_EVENT, IP_EVENT_STA_GOT_IP);
        assert(station.state() == WifiStation::State::Connected);
    }
    {
        defaults(); WifiStation station; start(station);
        for (unsigned i = 0; i < 5; ++i) event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        assert(connect_calls.load() == 6 && station.state() == WifiStation::State::Connecting);
        event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        assert(station.state() == WifiStation::State::Failed);
        for (unsigned i = 0; i < 10; ++i) event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        assert(connect_calls.load() == 6);
    }
    {
        // 成功后再次掉线，会获得独立的有上限连接周期。
        defaults(); WifiStation station; start(station);
        event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        event(station, IP_EVENT, IP_EVENT_STA_GOT_IP);
        clock_us.store(90000000);
        event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        assert(!station.ipAddress() && station.retry_count_.load() == 1);
        station.poll(); assert(station.state() == WifiStation::State::Connecting);
        clock_us.store(150000000); station.poll();
        assert(station.state() == WifiStation::State::Failed);
    }
    {
        defaults(); WifiStation station; start(station);
        event(station, IP_EVENT, IP_EVENT_STA_GOT_IP);
        clock_us.store(80000000);
        event(station, IP_EVENT, IP_EVENT_STA_LOST_IP);
        assert(!station.ipAddress() && station.state() == WifiStation::State::Connecting);
        assert(disconnect_calls.load() == 1);
        event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        assert(connect_calls.load() == 2 && station.retry_count_.load() == 1);
    }
    {
        defaults(); WifiStation station; start(station);
        connect_result = ESP_FAIL;
        event(station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
        assert(station.state() == WifiStation::State::Failed);
        assert(station.retry() == ESP_FAIL && station.state() == WifiStation::State::Failed);
        event(station, IP_EVENT, IP_EVENT_STA_GOT_IP);
        assert(station.state() == WifiStation::State::Failed);
    }
    {
        defaults(); WifiStation station;
        assert(station.retry() == ESP_ERR_INVALID_STATE);
        power_result = ESP_FAIL;
        assert(station.start() == ESP_FAIL && station.state() == WifiStation::State::Failed);
        event(station, WIFI_EVENT, WIFI_EVENT_STA_START);
        assert(!connect_calls.load());
    }
    {
        // 时钟回绕后，无符号经过时间截止判断仍有效。
        defaults(); WifiStation station;
        clock_us.store((static_cast<int64_t>(UINT32_MAX) - 30000) * 1000);
        start(station);
        clock_us.fetch_add(60000000); station.poll();
        assert(station.state() == WifiStation::State::Failed);
    }
    for (unsigned i = 0; i < 100; ++i) {
        // poll/GOT_IP 可在截止点竞争，获胜状态保持一致：
        // 成功阻止断开，超时阻止迟到事件恢复连接。
        defaults(); WifiStation station; start(station);
        clock_us.store(60001000);
        std::thread timeout([&] { station.poll(); });
        std::thread success([&] { event(station, IP_EVENT, IP_EVENT_STA_GOT_IP); });
        timeout.join(); success.join();
        if (station.state() == WifiStation::State::Connected)
            assert(station.ipAddress() == 42 && !disconnect_calls.load());
        else assert(station.state() == WifiStation::State::Failed && !station.ipAddress() && disconnect_calls.load() == 1);
    }
    std::puts("Wi-Fi recovery: 9 scenarios and 100 concurrent deadline/IP races passed (host stubs)");
}
