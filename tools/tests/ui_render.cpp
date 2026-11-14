#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#define private public
#include "display/voice_assistant_ui.hpp"
#undef private

extern bool ui_test_lvgl_lock_result;

namespace {
size_t checks = 0;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "UI render check failed: %s\n", message);
        std::exit(1);
    }
}
}

esp_err_t St7789Display::attachLvglDisplay(lv_display_t *display) {
    lvgl_display_ = display;
    return ESP_OK;
}

int main() {
    St7789Display display;
    display.panel_io_ = reinterpret_cast<void *>(1);
    display.panel_ = reinterpret_cast<void *>(1);
    VoiceAssistantUi ui;
    require(ui.init(display) == ESP_OK, "production UI initializes with host display adapter");
    require(lv_obj_get_height(ui.details_viewport_) == 3 * 31 &&
            ui.details_viewport_->y + lv_obj_get_height(ui.details_viewport_) <= ui.page_indicator_->y &&
            ui.ip_address_->y + 31 <= 240,
            "three complete text lines and status rows fit in the 240 px display");
    VoiceAssistantStatus status;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    WifiStation wifi;
    wifi.state_.store(WifiStation::State::Connected);
    wifi.ip_address_.store(0x0100007f);
    require(ui.render(status, wifi) == ESP_OK, "idle render succeeds");
    require(ui.title_->text == "已暂停 | 已连接", "header shows interaction/connection state");
    require(ui.details_->text == "已暂停自动监听\n按键1恢复对话",
            "idle screen uses the physical action button instead of serial commands");
    require(!ui.command('v'), "provider details switching is removed");

    status.setListening(true);
    ui.render(status, wifi);
    require(ui.details_->text == "请直接说话\n说完后自动提交\n按键1暂停监听",
            "listening screen explains automatic submission and physical pause");
    status.setResponse(VoiceResponseState::Complete);
    status.setConversation(std::string(600, 'x').c_str());
    require(ui.render(status, wifi) == ESP_OK && ui.page_count_ > 3,
            "long answers use the production page path");
    require(ui.command(']') && ui.command(']'), "shared next-page command remains owned by UI");
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 2,
            "answer page selection moves to page three");
    require(ui.command('[') && ui.render(status, wifi) == ESP_OK && ui.page_index_ == 1,
            "shared previous-page command moves to page two");
    require(ui.details_->text == std::string(600, 'x'), "paging preserves the full answer");
    // 页面保持用户所选位置；已移除每五秒自动翻页，
    // 避免阅读长回答期间页面自行移动。
    ui_test_tick += 5000;
    ui.render(status, wifi);
    require(ui.page_index_ == 1, "selected answer page remains stable without auto paging");
    status.setConversation(std::string(900, 'n').c_str());
    ui.render(status, wifi);
    require(ui.page_index_ == 0 && ui.details_->text == std::string(900, 'n'),
            "replacement answer starts at its own first page");

    status.setPlayback(VoicePlaybackState::Playing);
    status.setBargeInEnabled(true);
    status.setBargeIn(VoiceBargeInState::Candidate);
    ui.render(status, wifi);
    require(ui.details_->text == std::string(900, 'n'), "playback keeps the answer content");
    require(ui.network_->text == "正在确认插话" && ui.ip_address_->text == "键1停止，键2翻页" &&
            ui.title_->text == "自动插话 | 已连接",
            "playback prioritizes the current interaction and physical key hints");

    // 物理键 2 将短按/长按映射为这些共享翻页命令。
    // 在文字持续到达时验证实际 Speaking 渲染路径，
    // 而不是只对完成回答检查分页。
    ui_test_tick += 100;
    require(ui.command(']'), "next-page gesture is queued before a busy LVGL frame");
    ui_test_lvgl_lock_result = false;
    require(ui.render(status, wifi) == ESP_ERR_TIMEOUT && ui.page_index_ == 0,
            "a busy LVGL frame defers pending page movement without discarding it");
    ui_test_lvgl_lock_result = true;
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 1 &&
            ui.details_viewport_->scroll_y == lv_obj_get_height(ui.details_viewport_),
            "next-page gesture scrolls the answer while playback is active");
    const auto answer_id = status.conversationId();
    ui_test_tick += 5000;
    status.appendConversation(std::string(240, 's').c_str());
    status.setBargeIn(VoiceBargeInState::Armed);
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 1 &&
            status.conversationId() == answer_id &&
            ui.details_->text == std::string(900, 'n') + std::string(240, 's'),
            "streaming reply text and D2 state updates preserve the selected page");
    status.setPlayback(VoicePlaybackState::Draining);
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 1,
            "draining playback preserves the answer page");
    require(ui.command('[') && ui.render(status, wifi) == ESP_OK && ui.page_index_ == 0,
            "previous-page gesture scrolls backward during playback");
    status.setPlayback(VoicePlaybackState::Idle);
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 0,
            "completing playback retains the manually selected answer page");

    status.setCapture(VoiceCaptureState::Recording);
    ui.render(status, wifi);
    require(ui.details_->text == std::string(900, 'n') + std::string(240, 's') &&
            ui.ip_address_->text == "按键1取消本轮",
            "recording retains interaction content and physical stop hint");

    status.setPipelineStage("多次重连失败，请尝试重启");
    status.setSession(VoiceSessionState::Failed);
    ui.render(status, wifi);
    require(ui.details_->text == "多次重连失败，请尝试重启",
            "network failure shows the recovery policy without a serial command");
    status.setCapture(VoiceCaptureState::Idle);
    status.setPlayback(VoicePlaybackState::Idle);
    status.setBargeInEnabled(false);
    status.setBargeIn(VoiceBargeInState::Disabled);
    status.setResponse(VoiceResponseState::Idle);
    status.setPipelineStage("连接中断，正在自动重连（2/5）");
    status.setSession(VoiceSessionState::Connecting);
    ui.render(status, wifi);
    require(ui.details_->text == "连接中断，正在自动重连（2/5）",
            "preparing screen exposes automatic reconnect progress");
    status.setSession(VoiceSessionState::Ready);
    status.setListening(true);
    ui.render(status, wifi);
    require(ui.title_->text == "自动监听 | 已连接", "successful reconnect restores the active mode header");
    status.setBargeIn(VoiceBargeInState::Unavailable);
    ui.render(status, wifi);
    require(ui.title_->text == "自动监听 | 已连接" && ui.network_->text == "插话暂不可用" &&
            ui.ip_address_->text == "自动监听仍可使用",
            "D2 degradation stays visible while ordinary automatic listening remains usable");
    status.setBargeInEnabled(true);
    status.setBargeIn(VoiceBargeInState::Cancelling);
    ui.render(status, wifi);
    require(ui.network_->text == "已停播，正在取消旧回复",
            "D2 handoff keeps priority over idle help");
    status.setBargeInEnabled(false);
    status.setBargeIn(VoiceBargeInState::Disabled);
    wifi.state_.store(WifiStation::State::Failed);
    ui.render(status, wifi);
    require(ui.network_->text == "WiFi 连接失败" && ui.ip_address_->text == "请尝试重启",
            "exhausted WiFi recovery gives a local restart suggestion");
    require(!ui.command('?') && !ui.command('i'), "debug controller commands remain routed to their owner");
    // 自动字幕与手动阅读使用相同的三行换行页。
    // 显式换行使状态检查不依赖主机近似几何中的字形宽度。
    const char *subtitles = u8"甲\n乙\n丙\n丁\n戊\n己\n庚\n辛\n壬\n癸";
    const auto following = [&](uint32_t offset, uint32_t end, uint16_t progress,
                               bool reset = false, bool active = true) {
        return ui.update("", subtitles, nullptr, nullptr, 0x0320, 0xffff,
                         false, 0, true, reset, false, active, offset, end, progress);
    };
    const uint32_t second_page = static_cast<uint32_t>(std::strlen(u8"甲\n乙\n丙\n"));
    const uint32_t third_page = static_cast<uint32_t>(std::strlen(u8"甲\n乙\n丙\n丁\n戊\n己\n"));
    const uint32_t final_page = static_cast<uint32_t>(std::strlen(subtitles) - 3);
    require(following(0, 3, 0, true) == ESP_OK && ui.details_viewport_->height == 93 &&
            ui.details_->text == subtitles && ui.page_index_ == 0 && ui.last_page_visible_,
            "spoken subtitles retain three wrapped lines and a visible page count");
    require(ui.page_mode_->text == "随声翻页",
            "active timestamps explicitly identify automatic spoken paging");
    const auto first_updates = ui.details_->text_updates;
    const auto first_modes = ui.details_->mode_updates;
    const auto first_layouts = ui_test_layout_updates;
    require(following(4, 7, 500) == ESP_OK && ui.page_index_ == 0 &&
            ui.details_viewport_->scroll_y == 0 && ui.details_->x == 0 &&
            ui.details_->text_updates == first_updates && ui.details_->mode_updates == first_modes &&
            ui_test_layout_updates == first_layouts,
            "within-page progress preserves fixed text without redundant layout work");
    require(following(second_page, second_page + 3, 0) == ESP_OK && ui.page_index_ == 1 &&
            ui.details_viewport_->scroll_y == 93 && ui.page_indicator_->text == "2/4",
            "the fourth spoken line immediately selects the second page");
    require(following(final_page, final_page + 3, 1000) == ESP_OK && ui.page_index_ == 3 &&
            ui.details_viewport_->scroll_y == 279 && ui.page_indicator_->text == "4/4",
            "late timestamps jump directly to the final partial page");
    require(following(6000, 6003, 0) == ESP_OK && ui.page_index_ == 3,
            "cursor ahead of available text preserves the existing page");
    require(following(0, 3, 0, false, false) == ESP_OK && ui.page_index_ == 3 &&
            ui.details_viewport_->scroll_y == 279,
            "stopping metadata preserves the final visible page");
    require(ui.page_mode_->text == "阅读",
            "missing or stopped metadata does not claim automatic following");
    require(ui.command('[') && following(final_page, final_page + 3, 0) == ESP_OK &&
            ui.manual_browse_ && ui.page_index_ == 2,
            "manual paging takes priority over the spoken position");
    require(ui.page_mode_->text == "手动阅读",
            "physical paging visibly identifies manual control");
    require(following(0, 3, 0) == ESP_OK && ui.page_index_ == 2,
            "later timestamps do not override manual browsing in the same reply");
    require(following(0, 3, 0, true) == ESP_OK && !ui.manual_browse_ &&
            ui.page_index_ == 0 && ui.details_viewport_->height == 93,
            "a new subtitle reply clears manual override and restores automatic pages");

    const char *fallback = u8"甲\n乙\n丙\n丁";
    require(ui.update("", fallback, nullptr, nullptr, 0x0320, 0xffff,
                      false, 0, true, true, false, false, 0) == ESP_OK &&
            ui.details_viewport_->height == 93 && ui.page_index_ == 0,
            "missing timing metadata retains the ordinary three-line view");
    ui_test_tick += 30000;
    require(ui.update("", fallback, nullptr, nullptr, 0x0320, 0xffff,
                      false, 0, true, false, false, false, 0) == ESP_OK && ui.page_index_ == 0,
            "ordinary text has no wall-clock automatic paging");

    // 验证生产状态修订路由，包括其热路径。
    wifi.state_.store(WifiStation::State::Connected);
    status.setSession(VoiceSessionState::Ready);
    status.setResponse(VoiceResponseState::Generating);
    status.setPlayback(VoicePlaybackState::Playing);
    status.resetSubtitle(8, 1, 2, 3);
    status.setSubtitleText(8, 1, 2, 3, subtitles);
    status.setSubtitlePosition(8, 1, 2, 3, 0, true, 3, 0, 1000, 0, 0);
    require(ui.render(status, wifi) == ESP_OK && ui.details_viewport_->height == 93 && ui.page_index_ == 0,
            "new spoken subtitle text starts automatic three-line pages through render");
    const auto mode_updates = ui.details_->mode_updates;
    const auto text_updates = ui.details_->text_updates;
    const auto layout_updates = ui_test_layout_updates;
    status.setSubtitlePosition(8, 1, 2, 3, 4, true, 7, 1000, 2000, 1500, 500);
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 0 &&
            ui.details_->mode_updates == mode_updates && ui.details_->text_updates == text_updates &&
            ui_test_layout_updates == layout_updates,
            "sample-only updates within a page avoid text, mode and layout work");
    const auto body_revision = status.conversationRevision();
    status.setFrontend(VoiceFrontendState::Speech);
    require(ui.render(status, wifi) == ESP_OK && status.conversationRevision() == body_revision &&
            ui.details_->text_updates == text_updates && ui.details_->mode_updates == mode_updates,
            "VAD status changes do not rebuild the subtitle text");
    for (int packet = 0; packet < 100; ++packet) status.setPlayback(VoicePlaybackState::Playing);
    require(ui.render(status, wifi) == ESP_OK && ui.details_->text_updates == text_updates,
            "repeated audio-packet Playing reports do not rebuild subtitle text");
    status.setSubtitlePosition(8, 1, 2, 3, third_page, true, third_page + 3, 2000, 3000, 2500, 500);
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 2 &&
            ui.details_viewport_->scroll_y == 186 && ui.details_->text_updates == text_updates,
            "subtitle-only revision flips directly to the current spoken page");
    const auto held_scroll = ui.details_viewport_->scroll_y;
    status.setSubtitlePosition(8, 1, 2, 3, 6000, true, 6003, 3000, 4000, 3500, 500);
    require(ui.render(status, wifi) == ESP_OK && ui.details_viewport_->scroll_y == held_scroll,
            "cursor ahead of the cached text retains the visible page through render");
    ui_test_tick += 30000;
    require(ui.render(status, wifi) == ESP_OK && ui.details_viewport_->scroll_y == held_scroll,
            "wall clock alone cannot advance the spoken page");
    status.disableSubtitle(8, 1, 2, 3);
    require(ui.render(status, wifi) == ESP_OK && ui.details_viewport_->scroll_y == held_scroll,
            "subtitle unavailability retains the last automatic page");
    require(ui.page_mode_->text == "阅读",
            "subtitle-only unavailability clears the automatic mode label on the fast path");
    status.setPlayback(VoicePlaybackState::Idle);
    status.setResponse(VoiceResponseState::Complete);
    require(ui.render(status, wifi) == ESP_OK && ui.page_index_ == 2,
            "completed playback retains the last visible automatic page");
    require(ui.page_mode_->text == "阅读",
            "completed playback shows the retained text as ordinary reading");
    status.setPlayback(VoicePlaybackState::Playing);
    status.setSubtitlePosition(8, 1, 2, 3, second_page, true, second_page + 3, 4000, 5000, 4500, 500);
    require(ui.render(status, wifi) == ESP_OK, "following resumes when a valid current position is published");
    require(ui.command(']') && ui.render(status, wifi) == ESP_OK && ui.manual_browse_,
            "physical page control suspends following through render");
    const auto selected_scroll = ui.details_viewport_->scroll_y;
    status.setSubtitlePosition(8, 1, 2, 3, 0, true, 3, 5000, 6000, 5500, 500);
    require(ui.render(status, wifi) == ESP_OK && ui.details_viewport_->scroll_y == selected_scroll,
            "manual selection suspends subsequent subtitle cursor movement");
    const auto reply_id = status.conversationId();
    status.setSubtitleText(8, 1, 2, 3, u8"\n子\n丑\n寅");
    require(ui.render(status, wifi) == ESP_OK && status.conversationId() == reply_id &&
            ui.details_viewport_->scroll_y == selected_scroll && ui.manual_browse_,
            "streaming subtitle append preserves manual selection and reply identity");
    status.resetSubtitle(8, 2, 3, 4);
    status.setSubtitleText(8, 2, 3, 4, subtitles);
    status.setSubtitlePosition(8, 2, 3, 4, 0, true, 3, 0, 1000, 0, 0);
    require(ui.render(status, wifi) == ESP_OK && !ui.manual_browse_ && ui.page_index_ == 0 &&
            ui.details_viewport_->height == 93,
            "a new reply clears manual override and restores automatic following through render");
    std::printf("UI render: %zu checks passed (stub geometry; device pixel validation pending)\n", checks);
}
