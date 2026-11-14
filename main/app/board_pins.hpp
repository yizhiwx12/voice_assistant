#pragma once

#include <array>
#include <cstddef>

#include "driver/gpio.h"

// 当前 ESP32-S3 原型的接线表：只在实际重新接线时修改这里。
// Wi-Fi、服务器地址、音量和按钮 GPIO 请用 menuconfig 的 Voice Assistant 菜单。
// LLM / ASR / TTS 在电脑端 server/config/voice_config.json 选择，不在固件中选择。
namespace voice::board {

inline constexpr gpio_num_t kMicrophoneBclk = static_cast<gpio_num_t>(4);
inline constexpr gpio_num_t kMicrophoneWs = static_cast<gpio_num_t>(5);
inline constexpr gpio_num_t kMicrophoneData = static_cast<gpio_num_t>(6);

inline constexpr gpio_num_t kSpeakerEnable = static_cast<gpio_num_t>(8);
inline constexpr gpio_num_t kSpeakerData = static_cast<gpio_num_t>(15);
inline constexpr gpio_num_t kSpeakerBclk = static_cast<gpio_num_t>(16);
inline constexpr gpio_num_t kSpeakerWs = static_cast<gpio_num_t>(17);

inline constexpr gpio_num_t kDisplayDc = static_cast<gpio_num_t>(9);
inline constexpr gpio_num_t kDisplayCs = static_cast<gpio_num_t>(10);
inline constexpr gpio_num_t kDisplayMosi = static_cast<gpio_num_t>(11);
inline constexpr gpio_num_t kDisplayClock = static_cast<gpio_num_t>(12);
inline constexpr gpio_num_t kDisplayBacklight = static_cast<gpio_num_t>(13);
inline constexpr gpio_num_t kDisplayReset = static_cast<gpio_num_t>(14);

// 驱动默认接线和按钮冲突检查共用此表，避免换线后漏改保留引脚名单。
inline constexpr std::array<gpio_num_t, 13> kPeripheralPins{
    kMicrophoneBclk, kMicrophoneWs, kMicrophoneData,
    kSpeakerEnable, kSpeakerData, kSpeakerBclk, kSpeakerWs,
    kDisplayDc, kDisplayCs, kDisplayMosi, kDisplayClock, kDisplayBacklight, kDisplayReset,
};

constexpr bool isPeripheralPin(gpio_num_t pin)
{
    for (const auto reserved : kPeripheralPins) {
        if (pin == reserved) return true;
    }
    return false;
}

constexpr bool peripheralPinsAreUnique()
{
    for (std::size_t i = 0; i < kPeripheralPins.size(); ++i) {
        for (std::size_t j = i + 1; j < kPeripheralPins.size(); ++j) {
            if (kPeripheralPins[i] == kPeripheralPins[j]) return false;
        }
    }
    return true;
}

static_assert(peripheralPinsAreUnique(), "Board peripherals must not share a GPIO");

}  // 命名空间 voice::board 结束
