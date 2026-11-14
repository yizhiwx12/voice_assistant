#include "app/voice_assistant_app.hpp"

// ESP-IDF 唯一入口：这里只启动应用，具体硬件、网络和对话逻辑分别在对应模块。
extern "C" void app_main(void)
{
    static VoiceAssistantApp app;
    ESP_ERROR_CHECK(app.init());
    app.run();
}
