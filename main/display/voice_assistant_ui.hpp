#pragma once

#include <cstdint>
#include <cstddef>
#include <array>

#include "esp_err.h"
#include "lvgl.h"
#include "st7789_display.hpp"
#include "app/voice_assistant_status.hpp"
#include "network/wifi_station.hpp"

// LVGL 界面：把状态快照渲染成屏幕，缓存上次显示内容以减少重复布局与 SPI 传输。
// 当前全文预载模式下，字幕只定位完整正文；无有效时间戳时保留全文和按键翻页。
class VoiceAssistantUi {
public:
    esp_err_t init(St7789Display &display);
    esp_err_t update(const char *title, const char *details, const char *network,
                     const char *ip_address, uint16_t background, uint16_t accent,
                     bool show_progress, uint8_t progress_percent,
                     bool page_details = false, bool reset_page = false,
                     bool text_truncated = false, bool follow_subtitle = false,
                     uint32_t subtitle_byte_offset = 0,
                     uint32_t subtitle_word_end_offset = 0,
                     uint16_t subtitle_word_progress = 0,
                     // 为现有调用者保留；翻页只使用当前词。
                     uint32_t subtitle_next_word_offset = UINT32_MAX,
                     uint16_t subtitle_next_word_progress = 0);
    esp_err_t render(const VoiceAssistantStatus &status, const WifiStation &wifi_station);
    // 实体按键和调试命令共用的翻页控制；
    // 其他命令交给对话逻辑。
    bool command(char value);

private:
    static lv_color_t toLvColor(uint16_t rgb565);
    void updateSubtitlePage(const char *text, uint32_t start_offset,
                            uint32_t end_offset, uint16_t progress, size_t length);
    void updatePageIndicator(bool page_details);

    lv_display_t *lv_display_ = nullptr;
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *details_viewport_ = nullptr;
    lv_obj_t *details_ = nullptr;
    lv_obj_t *page_indicator_ = nullptr;
    lv_obj_t *page_mode_ = nullptr;
    std::array<lv_obj_t *, 3> voice_marks_{};
    lv_obj_t *header_rule_ = nullptr;
    lv_obj_t *footer_rule_ = nullptr;
    lv_obj_t *network_ = nullptr;
    lv_obj_t *ip_address_ = nullptr;
    lv_obj_t *progress_ = nullptr;
    std::array<char, 40> last_title_text_{};
    std::array<char, 40> last_page_text_{};
    std::array<char, 40> last_page_mode_text_{};
    std::array<char, VoiceAssistantStatus::kConversationCapacity> conversation_text_{};
    std::array<char, 64> last_network_text_{};
    std::array<char, 40> last_ip_text_{};
    uint16_t last_background_ = 0xFFFF;
    uint16_t last_accent_ = 0xFFFF;
    uint8_t last_progress_percent_ = 0xFF;
    uint8_t last_state_ = 0xFF;
    uint32_t last_text_revision_ = 0xFFFFFFFF;
    uint32_t last_conversation_revision_ = 0xFFFFFFFF;
    uint32_t last_conversation_id_ = 0xFFFFFFFF;
    uint32_t last_subtitle_revision_ = 0xFFFFFFFF;
    int32_t details_content_height_ = 0;
    int32_t scroll_target_y_ = 0;
    // 只缓存当前三行的字节范围。单纯采样进度更新只扫描一个词；
    // 只有跨页时才运行 LVGL 换行计算。
    uint32_t subtitle_page_start_ = 0;
    uint32_t subtitle_page_end_ = 0;
    uint16_t subtitle_layout_page_ = 0;
    uint32_t subtitle_word_start_ = UINT32_MAX;
    uint32_t subtitle_word_end_ = 0;
    uint32_t subtitle_word_characters_ = 0;
    size_t conversation_length_ = 0;
    bool subtitle_following_ = false;
    bool manual_browse_ = false;
    bool details_text_truncated_ = false;
    uint16_t page_index_ = 0;
    uint16_t page_count_ = 1;
    int16_t manual_page_delta_ = 0;
    int last_wifi_state_ = -1;
    bool rendered_once_ = false;
    bool last_network_visible_ = false;
    bool last_ip_visible_ = false;
    bool last_progress_visible_ = false;
    bool last_page_visible_ = false;
    bool last_page_mode_visible_ = false;
};
