#pragma once

#include "audio/max98357a_audio.hpp"
#include "display/st7789_display.hpp"
#include "voice_assistant_status.hpp"
#include "display/voice_assistant_ui.hpp"
#include "network/wifi_station.hpp"
#include "app/conversation_controller.hpp"
#include "app/local_buttons.hpp"

// 顶层装配：依次初始化屏幕、按键、开机音、Wi-Fi 和对话控制器。
// run() 只负责轮询/分发；音频采集、网络收发和模型推理由各自模块处理。
class VoiceAssistantApp {
public:
    VoiceAssistantApp();

    esp_err_t init();
    [[noreturn]] void run();

private:
    esp_err_t startConversation();

    St7789Display display_;
    VoiceAssistantUi ui_;
    WifiStation wifi_station_;
    VoiceAssistantStatus status_;
    Max98357aAudio speaker_;
    ConversationController conversation_;
    LocalButtons buttons_;
    bool conversation_start_pending_ = false;
    bool conversation_started_ = false;
    bool serial_commands_ready_ = false;
};
