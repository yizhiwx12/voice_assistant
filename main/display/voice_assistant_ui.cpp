#include "voice_assistant_ui.hpp"
#include "device_status_display.hpp"

#include "esp_lvgl_port.h"
#include "esp_netif_ip_addr.h"
#include "fonts/lv_font_chinese_16.h"
#include "src/misc/lv_text_private.h"

#include <cstddef>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
constexpr uint32_t kRefreshPeriodMs = 20;
constexpr int kScreenWidth = 240;
constexpr int kDetailsWidth = kScreenWidth - 24;
constexpr uint16_t kSea = 0x1188;       // 背景色 #123247，采用 RGB565 格式。
constexpr uint16_t kText = 0xE7BE;      // 正文颜色 #EAF4F4。
constexpr uint16_t kMuted = 0x9DD8;     // 辅助文字颜色 #9BBCC8。
constexpr uint16_t kSignal = 0x76D9;    // 强调色 #70D9CF。
constexpr uint16_t kRule = 0x2AAC;      // #294F63

const char *frontendLabel(VoiceFrontendState state, bool listening)
{
    switch (state) {
    case VoiceFrontendState::Starting: return "本地检测准备中";
    case VoiceFrontendState::Silence: return listening ? "自动监听：静音" : "本地检测：静音";
    case VoiceFrontendState::Speech: return listening ? "自动监听：人声" : "本地检测：人声";
    case VoiceFrontendState::Failed: return "本地检测异常（可手动）";
    case VoiceFrontendState::Disabled: return "手动模式，VAD 已关闭";
    }
    return "";
}

const char *bargeInLabel(VoiceBargeInState state, bool enabled)
{
    if (enabled) {
        switch (state) {
        case VoiceBargeInState::Armed: return "自动插话：等待人声";
        case VoiceBargeInState::Candidate: return "自动插话：人声候选";
        case VoiceBargeInState::Confirmed: return "自动插话：人声已确认";
        case VoiceBargeInState::Cooldown: return "自动插话：等待静音";
        case VoiceBargeInState::Unavailable: return "插话暂不可用";
        case VoiceBargeInState::Cancelling: return "已停播，正在取消旧回复";
        case VoiceBargeInState::Starting: return "插话已接收，正在准备识别";
        case VoiceBargeInState::Disabled: return "";
        }
    }
    switch (state) {
    case VoiceBargeInState::Armed: return "插话观察：等待人声";
    case VoiceBargeInState::Candidate: return "插话观察：人声候选";
    case VoiceBargeInState::Confirmed: return "插话观察：人声已确认";
    case VoiceBargeInState::Cooldown: return "插话观察：等待静音";
    case VoiceBargeInState::Unavailable: return "插话暂不可用";
    case VoiceBargeInState::Cancelling: return "正在取消旧回复";
    case VoiceBargeInState::Starting: return "正在准备识别";
    case VoiceBargeInState::Disabled: return "";
    }
    return "";
}

template <std::size_t Capacity>
void setLabelTextIfChanged(lv_obj_t *label, std::array<char, Capacity> &cached,
                           const char *text)
{
    const char *value = text == nullptr ? "" : text;
    if (std::strcmp(cached.data(), value) == 0) {
        return;
    }
    lv_label_set_text(label, value);
    std::snprintf(cached.data(), cached.size(), "%s", value);
}

void setVisibilityIfChanged(lv_obj_t *object, bool visible, bool &cached_visible)
{
    if (visible == cached_visible) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    cached_visible = visible;
}

size_t utf8CharBytes(const char *text, size_t remaining)
{
    if (text == nullptr || remaining == 0) return 0;
    const uint8_t lead = static_cast<uint8_t>(text[0]);
    const size_t width = lead < 0x80 ? 1 : (lead < 0xe0 ? 2 : (lead < 0xf0 ? 3 : 4));
    return std::min(width, remaining);
}

size_t utf8CharacterCount(const char *text, size_t bytes)
{
    size_t count = 0;
    for (size_t i = 0; i < bytes;) {
        const size_t width = utf8CharBytes(text + i, bytes - i);
        if (!width) break;
        i += width;
        ++count;
    }
    return count;
}

size_t utf8OffsetForCharacter(const char *text, size_t bytes, size_t character)
{
    size_t offset = 0;
    while (offset < bytes && character) {
        const size_t width = utf8CharBytes(text + offset, bytes - offset);
        if (!width) break;
        offset += width;
        --character;
    }
    return offset;
}
}

lv_color_t VoiceAssistantUi::toLvColor(uint16_t rgb565)
{
    const uint8_t red = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) * 255 / 31);
    const uint8_t green = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) * 255 / 63);
    const uint8_t blue = static_cast<uint8_t>((rgb565 & 0x1F) * 255 / 31);
    return lv_color_make(red, green, blue);
}

void VoiceAssistantUi::updateSubtitlePage(const char *text, uint32_t start_offset,
                                          uint32_t end_offset, uint16_t progress,
                                          size_t length)
{
    // 字幕定位信息可能早于文本快照到达。
    // 后续文字到齐前保持当前页，不选择 NUL 或空白末行。
    if (length == 0 || start_offset >= length) return;
    size_t word_start = start_offset;
    while (word_start && (static_cast<uint8_t>(text[word_start]) & 0xc0) == 0x80) --word_start;
    size_t word_end = std::min<size_t>(end_offset, length);
    while (word_end > word_start && word_end < length &&
           (static_cast<uint8_t>(text[word_end]) & 0xc0) == 0x80) --word_end;
    if (word_end <= word_start)
        word_end = word_start + utf8CharBytes(text + word_start, length - word_start);
    if (subtitle_word_start_ != word_start || subtitle_word_end_ != word_end) {
        subtitle_word_start_ = static_cast<uint32_t>(word_start);
        subtitle_word_end_ = static_cast<uint32_t>(word_end);
        subtitle_word_characters_ = static_cast<uint32_t>(utf8CharacterCount(
            text + word_start, word_end - word_start));
    }
    // 词级时间戳不包含逐字音素时间。
    // 词内时长按字数均分；下一词真正开始前，
    // 不能插值进入它前面的标点或静音区间。
    const size_t character = std::min<size_t>(subtitle_word_characters_ - 1,
        static_cast<uint64_t>(std::min<uint16_t>(progress, 1000)) * subtitle_word_characters_ / 1000);
    const size_t active_offset = word_start + utf8OffsetForCharacter(
        text + word_start, word_end - word_start, character);
    if (active_offset < subtitle_page_start_) {
        subtitle_page_start_ = subtitle_page_end_ = 0;
        subtitle_layout_page_ = 0;
    }
    // 完全复用 LVGL 标签的换行辅助函数，包含中日韩文字和拉丁词的断行规则。
    // 推进未变化的文本时，每个字节只访问一次；
    // 不会在每个音频节拍扫描或布局整份标签。
    while (subtitle_page_end_ <= active_offset) {
        if (subtitle_page_end_ > subtitle_page_start_) {
            subtitle_page_start_ = subtitle_page_end_;
            ++subtitle_layout_page_;
        }
        size_t end = subtitle_page_start_;
        for (unsigned line = 0; line < 3 && end < length; ++line) {
            const uint32_t bytes = lv_text_get_next_line(text + end,
                static_cast<uint32_t>(length - end), &lv_font_chinese_16,
                0, kDetailsWidth, nullptr, LV_TEXT_FLAG_NONE);
            if (bytes == 0) return;
            end += std::min<size_t>(bytes, length - end);
        }
        subtitle_page_end_ = static_cast<uint32_t>(end);
        if (end <= subtitle_page_start_) return;
    }
    const uint16_t page = std::min<uint16_t>(subtitle_layout_page_, page_count_ - 1);
    // 迟到的元数据不能让读者退回已经读完的页面。
    if (page <= page_index_) return;
    page_index_ = page;
    scroll_target_y_ = page_index_ * lv_obj_get_height(details_viewport_);
    lv_obj_scroll_to_y(details_viewport_, scroll_target_y_, LV_ANIM_OFF);
    updatePageIndicator(true);
}

void VoiceAssistantUi::updatePageIndicator(bool page_details)
{
    const bool show_mode = page_details;
    setLabelTextIfChanged(page_mode_, last_page_mode_text_, show_mode ?
        (manual_browse_ ? "手动阅读" : subtitle_following_ ? "随声翻页" : "阅读") : "");
    setVisibilityIfChanged(page_mode_, show_mode, last_page_mode_visible_);
    if (page_details && (page_count_ > 1 || details_text_truncated_)) {
        char page_text[40]{};
        std::snprintf(page_text, sizeof(page_text), "%u/%u%s", page_index_ + 1,
                      page_count_, details_text_truncated_ ? "  ..." : "");
        setLabelTextIfChanged(page_indicator_, last_page_text_, page_text);
        setVisibilityIfChanged(page_indicator_, true, last_page_visible_);
    } else {
        setVisibilityIfChanged(page_indicator_, false, last_page_visible_);
    }
}

esp_err_t VoiceAssistantUi::init(St7789Display &display)
{
    if (lv_display_ != nullptr || display.panelIoHandle() == nullptr ||
        display.panelHandle() == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&port_config);
    if (err != ESP_OK) {
        return err;
    }

    lvgl_port_display_cfg_t display_config{};
    display_config.io_handle = display.panelIoHandle();
    display_config.panel_handle = display.panelHandle();
    display_config.buffer_size = static_cast<uint32_t>(display.width()) * St7789Display::kDrawBufferLines;
    display_config.double_buffer = true;
    display_config.hres = static_cast<uint32_t>(display.width());
    display_config.vres = static_cast<uint32_t>(display.height());
    display_config.monochrome = false;
    display_config.color_format = LV_COLOR_FORMAT_RGB565;
    display_config.flags.buff_dma = true;
    display_config.flags.swap_bytes = false;
    lv_display_ = lvgl_port_add_disp(&display_config);
    if (lv_display_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    err = display.attachLvglDisplay(lv_display_);
    if (err != ESP_OK) {
        return err;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    // 屏幕刷新和动画采样使用不同的 LVGL 定时器。
    // 两者都保持 50 Hz；翻页本身不使用动画。
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_), kRefreshPeriodMs);
    lv_timer_set_period(lv_anim_get_timer(), kRefreshPeriodMs);

    screen_ = lv_display_get_screen_active(lv_display_);
    title_ = lv_label_create(screen_);
    details_viewport_ = lv_obj_create(screen_);
    details_ = lv_label_create(details_viewport_);
    page_indicator_ = lv_label_create(screen_);
    page_mode_ = lv_label_create(screen_);
    for (auto &mark : voice_marks_) mark = lv_obj_create(screen_);
    header_rule_ = lv_obj_create(screen_);
    footer_rule_ = lv_obj_create(screen_);
    network_ = lv_label_create(screen_);
    ip_address_ = lv_label_create(screen_);
    progress_ = lv_bar_create(screen_);
    if (screen_ == nullptr || title_ == nullptr || details_viewport_ == nullptr ||
        details_ == nullptr || page_indicator_ == nullptr || page_mode_ == nullptr ||
        voice_marks_[0] == nullptr || voice_marks_[1] == nullptr || voice_marks_[2] == nullptr ||
        header_rule_ == nullptr || footer_rule_ == nullptr ||
        network_ == nullptr || ip_address_ == nullptr || progress_ == nullptr) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(screen_);
    lv_obj_set_scroll_dir(screen_, LV_DIR_NONE);
    lv_obj_set_style_bg_color(screen_, toLvColor(kSea), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_, 0, LV_PART_MAIN);
    lv_label_set_text(title_, "正在启动");
    lv_label_set_text(details_, "正在初始化设备");
    lv_obj_add_flag(network_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ip_address_, LV_OBJ_FLAG_HIDDEN);

    for (lv_obj_t *label : {title_, details_, network_, ip_address_, page_mode_}) {
        lv_obj_set_style_text_font(label, &lv_font_chinese_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, toLvColor(kMuted), LV_PART_MAIN);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(details_, toLvColor(kText), LV_PART_MAIN);
    lv_obj_set_style_text_color(title_, toLvColor(kText), LV_PART_MAIN);
    lv_obj_set_style_text_color(network_, toLvColor(kSignal), LV_PART_MAIN);
    lv_obj_set_style_text_align(details_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(details_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(details_, 0, LV_PART_MAIN);

    // 三根静态声纹柱构成设备标识，不随麦克风电平变化，
    // 也不播放动画，因此静止页面无需额外重绘。
    constexpr int mark_heights[] = {10, 20, 14};
    for (size_t i = 0; i < voice_marks_.size(); ++i) {
        lv_obj_remove_style_all(voice_marks_[i]);
        lv_obj_set_size(voice_marks_[i], 3, mark_heights[i]);
        lv_obj_set_style_bg_opa(voice_marks_[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(voice_marks_[i], toLvColor(kSignal), LV_PART_MAIN);
        lv_obj_set_style_radius(voice_marks_[i], 1, LV_PART_MAIN);
        lv_obj_align(voice_marks_[i], LV_ALIGN_TOP_LEFT, 12 + static_cast<int>(i) * 6,
                     23 - mark_heights[i] / 2);
    }
    for (lv_obj_t *rule : {header_rule_, footer_rule_}) {
        lv_obj_remove_style_all(rule);
        lv_obj_set_size(rule, kDetailsWidth, 1);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(rule, toLvColor(kRule), LV_PART_MAIN);
    }
    lv_obj_align(header_rule_, LV_ALIGN_TOP_LEFT, 12, 41);
    lv_obj_align(footer_rule_, LV_ALIGN_TOP_LEFT, 12, 176);

    lv_obj_set_size(title_, 190, 31);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title_, LV_ALIGN_TOP_LEFT, 38, 8);

    // 将完整换行标签裁剪为三条完整行，生成字库的行高为 31 像素。
    // 页面移动以像素为单位，
    // 不会切割或重新拼接 UTF-8 文本。
    lv_obj_remove_style_all(details_viewport_);
    lv_obj_set_size(details_viewport_, kDetailsWidth,
                    3 * lv_font_get_line_height(&lv_font_chinese_16));
    lv_obj_set_style_bg_opa(details_viewport_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(details_viewport_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(details_viewport_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(details_viewport_, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(details_viewport_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(details_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(details_viewport_, LV_ALIGN_TOP_LEFT, 12, 49);
    lv_obj_set_width(details_, kDetailsWidth);
    lv_obj_set_height(details_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_line_space(details_, 0, LV_PART_MAIN);
    lv_label_set_long_mode(details_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(details_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(page_indicator_, toLvColor(kSignal), LV_PART_MAIN);
    lv_obj_set_style_text_align(page_indicator_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(page_indicator_, 80);
    lv_obj_align(page_indicator_, LV_ALIGN_TOP_LEFT, 148, 153);
    lv_obj_add_flag(page_indicator_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(page_mode_, 128, 31);
    lv_obj_align(page_mode_, LV_ALIGN_TOP_LEFT, 12, 143);
    lv_obj_add_flag(page_mode_, LV_OBJ_FLAG_HIDDEN);

    // 状态和按键提示各占固定单行。诊断过长时省略，
    // 不换行挤占相邻区域，也不启动跑马灯。
    lv_obj_set_size(network_, kDetailsWidth, 31);
    lv_label_set_long_mode(network_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(network_, LV_ALIGN_TOP_LEFT, 12, 178);

    lv_obj_set_size(ip_address_, kDetailsWidth, 31);
    lv_label_set_long_mode(ip_address_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(ip_address_, LV_ALIGN_TOP_LEFT, 12, 209);

    lv_obj_set_size(progress_, kDetailsWidth, 3);
    lv_bar_set_range(progress_, 0, 100);
    lv_obj_align(progress_, LV_ALIGN_TOP_LEFT, 12, 173);
    lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t VoiceAssistantUi::update(const char *title, const char *details,
                                   const char *network, const char *ip_address,
                                   uint16_t background, uint16_t accent,
                                   bool show_progress, uint8_t progress_percent,
                                   bool page_details, bool reset_page,
                                   bool text_truncated, bool follow_subtitle,
                                   uint32_t subtitle_byte_offset,
                                   uint32_t subtitle_word_end_offset,
                                   uint16_t subtitle_word_progress,
                                   uint32_t /* 保留但未使用的 subtitle_next_word_offset 参数 */,
                                   uint16_t /* 保留但未使用的 subtitle_next_word_progress 参数 */)
{
    if (lv_display_ == nullptr || title_ == nullptr || details_ == nullptr ||
        network_ == nullptr || ip_address_ == nullptr || progress_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (progress_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    // 应用循环负责分派缓存的按键事件。LVGL 等待必须有上限，
    // 防止显示传输无限推迟取消或暂停；
    // 下次渲染周期继续处理待完成翻页和文本版本。
    if (!lvgl_port_lock(10)) {
        return ESP_ERR_TIMEOUT;
    }

    if (!rendered_once_ || last_background_ != background) {
        lv_obj_set_style_bg_color(screen_, toLvColor(background), LV_PART_MAIN);
        last_background_ = background;
    }
    setLabelTextIfChanged(title_, last_title_text_, title);
    const char *details_text = details == nullptr ? "" : details;
    if (reset_page || !page_details) {
        page_index_ = 0;
        scroll_target_y_ = 0;
        manual_browse_ = false;
        subtitle_following_ = false;
        subtitle_page_start_ = subtitle_page_end_ = 0;
        subtitle_layout_page_ = 0;
        subtitle_word_start_ = UINT32_MAX;
    }
    const bool manual_move = manual_page_delta_ != 0;
    const bool details_changed = std::strcmp(lv_label_get_text(details_), details_text) != 0;
    if (details_changed) lv_label_set_text(details_, details_text);
    if (details_changed || details_content_height_ == 0) {
        lv_obj_set_height(details_, LV_SIZE_CONTENT);
        lv_obj_update_layout(details_viewport_);
        details_content_height_ = lv_obj_get_height(details_);
        // 追加文本可能使末尾不完整行或拉丁词重新换行。
        subtitle_page_start_ = subtitle_page_end_ = 0;
        subtitle_layout_page_ = 0;
        subtitle_word_start_ = UINT32_MAX;
    }
    const int page_height = std::max(1, static_cast<int>(lv_obj_get_height(details_viewport_)));
    page_count_ = page_details ? static_cast<uint16_t>(std::max(1,
        (static_cast<int>(details_content_height_) + page_height - 1) / page_height)) : 1;
    const int layout_height = page_details ? page_count_ * page_height : details_content_height_;
    if (lv_obj_get_height(details_) != layout_height) {
        // 为最后不足一页的内容补足高度，让 LVGL 能将首行对齐视口顶部，
        // 避免滚动位置被限制在最后一行文字基线上。
        lv_obj_set_height(details_, layout_height);
        lv_obj_update_layout(details_viewport_);
    }
    details_text_truncated_ = text_truncated;
    subtitle_following_ = page_details && follow_subtitle && !manual_browse_;
    if (subtitle_following_) {
        updateSubtitlePage(details_text, subtitle_byte_offset, subtitle_word_end_offset,
                           subtitle_word_progress, std::strlen(details_text));
    }
    if (manual_move) {
        page_index_ = static_cast<uint16_t>(std::clamp(
            static_cast<int>(page_index_) + manual_page_delta_, 0,
            static_cast<int>(page_count_) - 1));
        manual_page_delta_ = 0;
        manual_browse_ = true;
        subtitle_following_ = false;
    }
    page_index_ = std::min<uint16_t>(page_index_, page_count_ - 1);
    scroll_target_y_ = page_index_ * page_height;
    if (lv_obj_get_scroll_y(details_viewport_) != scroll_target_y_)
        lv_obj_scroll_to_y(details_viewport_, scroll_target_y_, LV_ANIM_OFF);
    updatePageIndicator(page_details);

    const bool network_visible = network != nullptr && network[0] != '\0';
    setLabelTextIfChanged(network_, last_network_text_, network_visible ? network : "");
    setVisibilityIfChanged(network_, network_visible, last_network_visible_);
    const bool ip_visible = ip_address != nullptr && ip_address[0] != '\0';
    setLabelTextIfChanged(ip_address_, last_ip_text_, ip_visible ? ip_address : "");
    setVisibilityIfChanged(ip_address_, ip_visible, last_ip_visible_);

    if (show_progress) {
        if (!last_progress_visible_) {
            lv_obj_set_style_bg_color(progress_, toLvColor(0xFFFF), LV_PART_MAIN);
            last_progress_visible_ = true;
        }
        if (last_progress_percent_ != progress_percent) {
            lv_bar_set_value(progress_, progress_percent, LV_ANIM_OFF);
            last_progress_percent_ = progress_percent;
        }
        if (last_accent_ != accent) {
            lv_obj_set_style_bg_color(progress_, toLvColor(accent), LV_PART_INDICATOR);
            last_accent_ = accent;
        }
        lv_obj_clear_flag(progress_, LV_OBJ_FLAG_HIDDEN);
    } else if (last_progress_visible_) {
        lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);
        last_progress_visible_ = false;
    }

    rendered_once_ = true;
    lvgl_port_unlock();
    return ESP_OK;
}

bool VoiceAssistantUi::command(char value)
{
    if (value != '[' && value != ']') return false;
    manual_page_delta_ = static_cast<int16_t>(std::clamp(
        static_cast<int>(manual_page_delta_) + (value == ']' ? 1 : -1), -100, 100));
    return true;
}

esp_err_t VoiceAssistantUi::render(const VoiceAssistantStatus &status,
                                   const WifiStation &wifi_station)
{
    const VoiceAssistantState state = status.state();
    const uint8_t state_value = static_cast<uint8_t>(state);
    const uint32_t text_revision = status.textRevision();
    const uint32_t conversation_revision = status.conversationRevision();
    const auto subtitle = status.subtitlePosition();
    const WifiStation::State wifi_state = wifi_station.state();
    const int wifi_state_value = static_cast<int>(wifi_state);
    const bool state_changed = state_value != last_state_;
    const bool presentation_changed = state_changed || text_revision != last_text_revision_ ||
        conversation_revision != last_conversation_revision_;
    if (!presentation_changed && manual_page_delta_ == 0 &&
        ((state != VoiceAssistantState::Ready && state != VoiceAssistantState::Listening) ||
         wifi_state_value == last_wifi_state_)) {
        if (subtitle.revision == last_subtitle_revision_) return ESP_OK;
        if (subtitle_following_ && !manual_browse_) {
            // 大多数播放样本仍在同一页；
            // 这些更新不复制文本、不布局、不标记屏幕重绘，也不播放动画。
            if (!lvgl_port_lock(2)) return ESP_ERR_TIMEOUT;
            if (state == VoiceAssistantState::Speaking && subtitle.active)
                updateSubtitlePage(conversation_text_.data(), subtitle.text_byte_offset,
                    subtitle.word_end_byte_offset, subtitle.word_progress_permille,
                    conversation_length_);
            else {
                subtitle_following_ = false;
                updatePageIndicator(conversation_length_ != 0);
            }
            lvgl_port_unlock();
            last_subtitle_revision_ = subtitle.revision;
            return ESP_OK;
        }
    }

    const char *title = "";
    const char *details = "";
    const char *network = nullptr;
    const char *ip_text = nullptr;
    uint16_t background = kSea;
    uint16_t accent = kSignal;
    bool show_progress = false;
    uint8_t progress_percent = 0;
    bool page_details = false;
    const uint32_t conversation_id = status.conversationId();
    std::array<char, VoiceAssistantStatus::kConversationCapacity> &details_buffer = conversation_text_;
    std::array<char, 24> ip_buffer{};
    std::array<char, 64> mode_connection{};
    const bool reset_page = conversation_id != last_conversation_id_;
    const bool refresh_conversation_text = state_changed ||
        conversation_revision != last_conversation_revision_ || reset_page;
    if (refresh_conversation_text) {
        status.copyConversation(details_buffer.data(), details_buffer.size());
        conversation_length_ = std::strlen(details_buffer.data());
    }

    switch (state) {
    case VoiceAssistantState::Ready:
        background = kSea;
        title = "语音助手";
        details = status.realtime() ? "已暂停自动监听\n按键1恢复对话" : "麦克风准备中";
        break;
    case VoiceAssistantState::Preparing:
        title = "语音助手";
        details = status.capture() == VoiceCaptureState::Starting ?
            (status.listening() ? "正在准备识别\n正在缓存本轮语音" : "正在准备识别\n显示录音后再说话") :
            "正在连接语音服务...";
        if (status.realtime() && status.capture() != VoiceCaptureState::Starting) {
            status.copyPipelineStage(details_buffer.data(), details_buffer.size());
            if (details_buffer[0] != '\0') details = details_buffer.data();
        }
        if (status.bargeInEnabled() &&
            (status.bargeIn() == VoiceBargeInState::Cancelling ||
             status.bargeIn() == VoiceBargeInState::Starting)) {
            details = bargeInLabel(status.bargeIn(), true);
            network = "正在缓存本轮语音";
        }
        break;
    case VoiceAssistantState::Listening:
        title = status.realtime() ? "自动监听中" : "语音助手";
        details = status.realtime() ? "请直接说话\n说完后自动提交\n按键1暂停监听" : "请直接说话";
        break;
    case VoiceAssistantState::Recording:
        title = status.listening() ? "正在讲话" : "语音助手";
        page_details = details_buffer[0] != '\0';
        if (details_buffer[0] == '\0') {
            std::snprintf(details_buffer.data(), details_buffer.size(), "正在接收语音...");
        }
        details = details_buffer.data();
        network = status.listening() ? "说完后自动提交" : "正在录音";
        if (status.frontend() == VoiceFrontendState::Speech)
            network = status.listening() ? "人声：说完后自动提交" : "录音：人声";
        else if (status.frontend() == VoiceFrontendState::Silence)
            network = status.listening() ? "静音：等待自动提交" : "录音：静音";
        else if (status.frontend() == VoiceFrontendState::Failed) network = "录音中，本地检测异常";
        ip_text = "按键1取消本轮";
        break;
    case VoiceAssistantState::Transcribing:
        title = "语音助手";
        page_details = details_buffer[0] != '\0';
        details = details_buffer[0] == '\0' ? "正在完成识别..." : details_buffer.data();
        network = "正在识别";
        ip_text = "按键1取消本轮";
        break;
    case VoiceAssistantState::Uploading:
        title = "语音助手";
        page_details = details_buffer[0] != '\0';
        details = details_buffer[0] == '\0' ? "正在思考..." : details_buffer.data();
        network = "正在思考";
        ip_text = "按键1取消本轮";
        break;
    case VoiceAssistantState::Speaking:
        title = "语音助手";
        page_details = details_buffer[0] != '\0';
        details = details_buffer[0] == '\0' ? "正在播报..." : details_buffer.data();
        network = "正在播报";
        if (status.realtime()) {
            // 半双工暂不创建回合时，AFE 仍持续观察。
            // 复用其状态变化版本，不启用采集，也不逐音频块重绘；
            // 保留回答和页码位置。
            network = status.bargeInEnabled() ? "播报中，可直接插话" : "按键1停止播报";
            if (status.bargeInEnabled() && status.bargeIn() == VoiceBargeInState::Candidate)
                network = "正在确认插话";
            ip_text = "键1停止，键2翻页";
        }
        break;
    case VoiceAssistantState::Complete:
        title = "语音助手";
        page_details = details_buffer[0] != '\0';
        details = details_buffer.data();
        network = "按键1恢复对话";
        break;
    case VoiceAssistantState::UploadError:
        background = kSea;
        title = "语音助手";
        if (status.realtime()) {
            status.copyPipelineStage(details_buffer.data(), details_buffer.size());
        } else if (status.httpStatus() > 0) {
            std::snprintf(details_buffer.data(), details_buffer.size(),
                          "服务暂时不可用\nHTTP %d", status.httpStatus());
        } else {
            std::snprintf(details_buffer.data(), details_buffer.size(),
                          "服务暂时不可用，正在等待恢复");
        }
        details = details_buffer.data();
        page_details = status.realtime() && details_buffer[0] != '\0';
        network = status.realtime() ? "键1长按重试连接" : "等待服务恢复";
        break;
    case VoiceAssistantState::Error:
        background = kSea;
        title = "语音助手";
        details = "音频设备暂时不可用";
        network = "请尝试重启";
        break;
    }

    if (state == VoiceAssistantState::Ready || state == VoiceAssistantState::Listening) {
        switch (wifi_state) {
        case WifiStation::State::NotConfigured:
            network = "请配置 WiFi";
            break;
        case WifiStation::State::Connecting:
            network = "正在连接 WiFi";
            break;
        case WifiStation::State::Connected:
            network = status.realtime() ? frontendLabel(status.frontend(), status.listening()) : "WiFi 已连接";
            break;
        case WifiStation::State::Failed:
            network = "WiFi 连接失败";
            break;
        }

        std::snprintf(ip_buffer.data(), ip_buffer.size(), "%s", "等待 IP");
        const uint32_t ip_value = wifi_station.ipAddress();
        if (wifi_state == WifiStation::State::Connected && ip_value != 0) {
            esp_ip4_addr_t address{};
            address.addr = ip_value;
            std::snprintf(ip_buffer.data(), ip_buffer.size(), "IP " IPSTR, IP2STR(&address));
        } else if (wifi_state == WifiStation::State::Failed) {
            std::snprintf(ip_buffer.data(), ip_buffer.size(), "%s", "请尝试重启");
        } else if (wifi_state == WifiStation::State::NotConfigured) {
            std::snprintf(ip_buffer.data(), ip_buffer.size(), "%s", "未获取 IP");
        }
        ip_text = ip_buffer.data();
        if (status.realtime() && wifi_state == WifiStation::State::Connected) {
            ip_text = "键2翻页，长按上一页";
            if (!status.listening()) network = "自动监听已暂停";
        }
    }

    // 回答后继续显示本地检测状态，不替换回答文字。
    if (status.realtime() && state == VoiceAssistantState::Complete) {
        // 显示恢复自动监听时，保留空结果或回答页面，
        // 不能被固定的 Ready 帮助页面替换。
        title = status.listening() ? "自动监听中" : "语音助手";
        network = frontendLabel(status.frontend(), status.listening());
        if (!status.listening()) network = "自动监听已暂停";
        ip_text = status.listening() ? "键1暂停，键2翻页" : "键1继续，键2翻页";
    }

    // 启用 b 后，即使下一次播放尚未开始也要显示状态。
    // 保留 Wi-Fi 错误及现有回答和监听控件，这不代表创建新回合。
    if (status.realtime()) {
        const char *observation = status.bargeIn() == VoiceBargeInState::Armed ?
            (status.bargeInEnabled() ? "自动插话：等待播报" : "插话观察：等待播报") :
            status.bargeIn() == VoiceBargeInState::Unavailable ?
            bargeInLabel(status.bargeIn(), status.bargeInEnabled()) : nullptr;
        if (observation) {
            if (state == VoiceAssistantState::Complete ||
                ((state == VoiceAssistantState::Ready || state == VoiceAssistantState::Listening) &&
                 wifi_state == WifiStation::State::Connected)) network = observation;
            else if (state == VoiceAssistantState::Uploading) ip_text = observation;
            if (status.bargeIn() == VoiceBargeInState::Unavailable && status.listening() &&
                (state == VoiceAssistantState::Ready || state == VoiceAssistantState::Listening ||
                 state == VoiceAssistantState::Complete)) ip_text = "自动监听仍可使用";
        }
        if (status.bargeInEnabled() &&
            (status.bargeIn() == VoiceBargeInState::Cancelling ||
             status.bargeIn() == VoiceBargeInState::Starting) &&
            state != VoiceAssistantState::UploadError && state != VoiceAssistantState::Error &&
            status.session() == VoiceSessionState::Ready) {
            network = bargeInLabel(status.bargeIn(), true);
            ip_text = "正在缓存本轮语音";
        }
    }

    if (status.realtime()) {
        // 固定服务链路保留在诊断信息中。
        // 液晶屏只需要当前交互模式和连接状态。
        voiceui::formatModeConnection(status.session(), status.listening(), status.bargeInEnabled(),
                                       status.bargeIn(), mode_connection.data(), mode_connection.size());
        title = mode_connection.data();
    }

    const esp_err_t err = update(title, details, network, ip_text, background, accent,
                                 show_progress, progress_percent, page_details,
                                 reset_page,
                                 status.conversationTruncated(),
                                 state == VoiceAssistantState::Speaking && subtitle.active,
                                 subtitle.text_byte_offset,
                                 subtitle.word_end_byte_offset,
                                  subtitle.word_progress_permille,
                                  subtitle.next_word_byte_offset,
                                  subtitle.next_word_progress_permille);
    if (err != ESP_OK) {
        return err;
    }
    last_state_ = state_value;
    last_text_revision_ = text_revision;
    last_conversation_revision_ = conversation_revision;
    last_subtitle_revision_ = subtitle.revision;
    last_conversation_id_ = conversation_id;
    if (state == VoiceAssistantState::Ready || state == VoiceAssistantState::Listening) {
        last_wifi_state_ = wifi_state_value;
    }
    return ESP_OK;
}
