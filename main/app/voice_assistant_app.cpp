#include "voice_assistant_app.hpp"
#include "board_pins.hpp"
#include "app/memory_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#if defined(CONFIG_VOICE_SERIAL_COMMANDS_ENABLED) && CONFIG_VOICE_SERIAL_COMMANDS_ENABLED
#include "driver/uart.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "assistant_app";
constexpr size_t kToneChunkFrames = 240;

void testBacklight(St7789Display &display)
{
    constexpr std::array<uint8_t, 5> brightness = {20, 45, 70, 100, 65};
    for (const uint8_t percent : brightness) {
        ESP_ERROR_CHECK(display.setBacklightBrightness(percent));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void playTone(Max98357aAudio &audio, float frequency_hz, uint32_t duration_ms)
{
    std::array<int16_t, kToneChunkFrames> samples{};
    const uint32_t sample_rate = audio.sampleRateHz();
    const size_t total_frames = static_cast<size_t>(sample_rate) * duration_ms / 1000;
    constexpr float kPi2 = 6.28318530718f;
    constexpr int32_t kAmplitude = 16000;
    constexpr size_t kFadeFrames = 80;

    for (size_t offset = 0; offset < total_frames;) {
        const size_t frames = std::min(kToneChunkFrames, total_frames - offset);
        for (size_t i = 0; i < frames; ++i) {
            const size_t frame = offset + i;
            const size_t fade_distance = std::min(frame, total_frames - frame - 1);
            const float envelope = std::min(1.0f,
                static_cast<float>(fade_distance) / kFadeFrames);
            const float phase = kPi2 * frequency_hz * static_cast<float>(frame) / sample_rate;
            samples[i] = static_cast<int16_t>(std::sin(phase) * kAmplitude * envelope);
        }
        ESP_ERROR_CHECK(audio.writeMono(samples.data(), frames));
        offset += frames;
    }
}

void playStartupChime(Max98357aAudio &audio)
{
    constexpr std::array<float, 4> notes = {523.25f, 659.25f, 783.99f, 1046.50f};
    constexpr std::array<int16_t, kToneChunkFrames> silence{};
    for (const float note : notes) {
        playTone(audio, note, 180);
        for (int i = 0; i < 7; ++i) {
            ESP_ERROR_CHECK(audio.writeMono(silence.data(), silence.size()));
        }
    }
}
}

VoiceAssistantApp::VoiceAssistantApp() = default;

esp_err_t VoiceAssistantApp::init()
{
    // 启动阶段按依赖顺序装配：先让屏幕可见，再启用按键、音频和网络。
    // 关键硬件初始化失败向入口返回，避免带着半套资源继续运行。
    const esp_err_t memory_hook = voice::memory::install();
    if (memory_hook != ESP_OK)
        ESP_LOGW(kTag, "Memory failure hook unavailable: %s", esp_err_to_name(memory_hook));
    voice::memory::log("app_before_display");
    ESP_RETURN_ON_ERROR(display_.init(), kTag, "Display initialization failed");
    ESP_RETURN_ON_ERROR(display_.setRotation(0), kTag, "Display rotation setup failed");
    testBacklight(display_);
    ESP_RETURN_ON_ERROR(ui_.init(display_), kTag, "UI initialization failed");
    voice::memory::log("app_after_ui");

#if defined(CONFIG_VOICE_LOCAL_BUTTONS_ENABLED) && CONFIG_VOICE_LOCAL_BUTTONS_ENABLED
    // 按钮接线可在 menuconfig 调整，但不能抢占麦克风/扬声器/屏幕的引脚。
    LocalButtons::Config button_config;
    button_config.first_gpio = static_cast<gpio_num_t>(CONFIG_VOICE_BUTTON_FIRST_GPIO);
    button_config.second_gpio = static_cast<gpio_num_t>(CONFIG_VOICE_BUTTON_SECOND_GPIO);
#if defined(CONFIG_VOICE_BUTTON_ACTIVE_LOW) && CONFIG_VOICE_BUTTON_ACTIVE_LOW
    button_config.active_low = true;
#else
    button_config.active_low = false;
#endif
    if (voice::board::isPeripheralPin(button_config.first_gpio) ||
        voice::board::isPeripheralPin(button_config.second_gpio)) {
        ESP_LOGE(kTag, "Local button GPIO conflicts with fixed peripheral wiring: first=%d second=%d",
                 static_cast<int>(button_config.first_gpio), static_cast<int>(button_config.second_gpio));
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(buttons_.init(button_config, static_cast<uint32_t>(esp_timer_get_time() / 1000)),
                        kTag, "Local button initialization failed");
    ESP_LOGI(kTag, "Local buttons ready: first=%d second=%d active_low=%u",
             static_cast<int>(button_config.first_gpio), static_cast<int>(button_config.second_gpio),
             static_cast<unsigned>(button_config.active_low));
#endif

    ESP_RETURN_ON_ERROR(speaker_.init(), kTag, "Speaker initialization failed");
    // 开机音与实时播放器先后使用 I2S TX；交接前释放驱动，不能同时占有通道。
    ESP_LOGI(kTag, "Playing startup chime");
    playStartupChime(speaker_);
    ESP_RETURN_ON_ERROR(speaker_.deinit(), kTag, "Speaker shutdown failed");

    const esp_err_t wifi_err = wifi_station_.start();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi initialization failed: %s", esp_err_to_name(wifi_err));
    }
    voice::memory::log("app_after_wifi_start");

#ifdef CONFIG_VOICE_REALTIME_ENABLED
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    // 首次 Wi-Fi 认证和重试在 CPU0 上运行，优先级高于参考音频任务。
    // 第一次连接尝试完成前不要启用连续 RX；
    // run() 在此期间继续刷新屏幕。
    if (wifi_station_.state() == WifiStation::State::Connecting) {
        conversation_start_pending_ = true;
        ESP_LOGI(kTag, "Audio startup deferred until initial Wi-Fi connection attempt settles");
    } else
#endif
    {
        ESP_RETURN_ON_ERROR(startConversation(), kTag, "Realtime audio initialization failed");
    }
#endif

    return ESP_OK;
}

#ifdef CONFIG_VOICE_REALTIME_ENABLED
esp_err_t VoiceAssistantApp::startConversation()
{
    if (conversation_started_) return ESP_OK;
    // 分配资源前先清除标志，保证部分启动失败后主循环不会重试。
    // 启动错误的处理策略仍由调用者负责。
    conversation_start_pending_ = false;
#if defined(CONFIG_VOICE_SERIAL_COMMANDS_ENABLED) && CONFIG_VOICE_SERIAL_COMMANDS_ENABLED
    // 可选开发输入。设备日常操作使用按键；
    // 缺少串口驱动不能影响音频或本地交互。
    const esp_err_t serial_err = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
    serial_commands_ready_ = serial_err == ESP_OK;
    if (!serial_commands_ready_)
        ESP_LOGW(kTag, "Serial debug commands unavailable: %s", esp_err_to_name(serial_err));
#endif
    ESP_RETURN_ON_ERROR(conversation_.start(status_, wifi_station_), kTag,
                        "Realtime audio initialization failed");
    conversation_started_ = true;
    voice::memory::log("app_after_conversation");
    ESP_LOGI(kTag, "Startup main stack_free=%u internal=%u psram=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ESP_OK;
}
#endif

[[noreturn]] void VoiceAssistantApp::run()
{
    // 固定周期处理按键、网络状态和 UI；重型采集/播放/联网工作在独立任务中。
    // 使用周期延时而非不断累加固定 sleep，减少每次执行耗时带来的节拍漂移。
    TickType_t next_cycle = xTaskGetTickCount();
    const TickType_t cycle_ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(voice::ButtonGestures::kPollMs));
    for (;;) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        wifi_station_.poll();
        const uint8_t buttons = buttons_.poll(now_ms);
#ifdef CONFIG_VOICE_REALTIME_ENABLED
        if (conversation_start_pending_ &&
            wifi_station_.state() != WifiStation::State::Connecting) {
            ESP_LOGI(kTag, "Audio startup resumed after initial Wi-Fi connection attempt (state=%u)",
                     static_cast<unsigned>(wifi_station_.state()));
            // 首次启动错误沿用 app_main 的致命错误策略；
            // 不要每个节拍都重试已部分初始化的音频资源。
            ESP_ERROR_CHECK(startConversation());
        }
        if (conversation_started_) {
            if (voice::ButtonGestures::has(buttons, voice::ButtonEvent::FirstShort)) {
                ESP_LOGI(kTag, "button dispatch first_short conversation_started=1");
                conversation_.localAction();
            }
            if (voice::ButtonGestures::has(buttons, voice::ButtonEvent::FirstLong)) {
                ESP_LOGI(kTag, "button dispatch first_long conversation_started=1");
                conversation_.retryConnection();
            }
#if defined(CONFIG_VOICE_SERIAL_COMMANDS_ENABLED) && CONFIG_VOICE_SERIAL_COMMANDS_ENABLED
            // 每轮只读取有限个串口字符，保持 LVGL 响应。
            if (serial_commands_ready_) {
                uint8_t input[16]{};
                const int count = uart_read_bytes(UART_NUM_0, input, sizeof(input), 0);
                for (int i = 0; i < count; ++i) {
                    const char command = static_cast<char>(input[i]);
                    if (!ui_.command(command)) conversation_.command(command);
                }
            }
#endif
        } else if (buttons & (static_cast<uint8_t>(voice::ButtonEvent::FirstShort) |
                              static_cast<uint8_t>(voice::ButtonEvent::FirstLong))) {
            ESP_LOGW(kTag, "button ignored first event conversation_started=0");
        }
#endif
        if (voice::ButtonGestures::has(buttons, voice::ButtonEvent::SecondShort)) ui_.command(']');
        if (voice::ButtonGestures::has(buttons, voice::ButtonEvent::SecondLong)) ui_.command('[');
        // 每个 20 毫秒输入周期都检查字幕版本。render() 会跳过未变化状态，
        // 因此空闲周期不复制文字，也不重绘像素。
        const esp_err_t render_error = ui_.render(status_, wifi_station_);
        // 短暂的 LVGL 锁竞争可以恢复。界面会保留待处理的翻页手势，
        // 在下一周期重试；
        // 不能让播放中的按键操作触发致命重启。
        if (render_error != ESP_OK && render_error != ESP_ERR_TIMEOUT)
            ESP_ERROR_CHECK(render_error);
        // 把处理耗时计入 20 毫秒周期，不在工作结束后再额外休眠 20 毫秒。
        // 启动或耗时较长的操作导致超期时，
        // 跳过已错过的周期，不补跑积压帧。
        const TickType_t finished = xTaskGetTickCount();
        if (static_cast<int32_t>(finished - next_cycle) >= static_cast<int32_t>(cycle_ticks))
            next_cycle = finished;
        vTaskDelayUntil(&next_cycle, cycle_ticks);
    }
}
