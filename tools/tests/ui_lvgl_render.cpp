#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "esp_lvgl_port.h"
#define private public
#include "voice_assistant_ui.hpp"
#undef private
#include "fonts/lv_font_chinese_16.h"

namespace {
constexpr int kWidth = 240;
constexpr int kHeight = 240;
std::array<uint16_t, kWidth * kHeight> pixels{};
alignas(16) std::array<uint16_t, kWidth * 32> draw_buffer{};
size_t checks = 0;
size_t flushes = 0;
size_t flushed_pixels = 0;
const char *preview_directory = nullptr;

void require(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "Real LVGL check failed: %s\n", message);
        std::exit(1);
    }
}

void savePreview(const char *name)
{
    if (preview_directory == nullptr) return;
    const std::string path = std::string(preview_directory) + "/" + name + ".ppm";
    FILE *output = std::fopen(path.c_str(), "wb");
    require(output != nullptr, "preview output opens");
    std::fprintf(output, "P6\n%d %d\n255\n", kWidth, kHeight);
    std::array<uint8_t, kWidth * kHeight * 3> rgb{};
    for (size_t i = 0; i < pixels.size(); ++i) {
        rgb[3 * i] = static_cast<uint8_t>(((pixels[i] >> 11) & 31) * 255 / 31);
        rgb[3 * i + 1] = static_cast<uint8_t>(((pixels[i] >> 5) & 63) * 255 / 63);
        rgb[3 * i + 2] = static_cast<uint8_t>((pixels[i] & 31) * 255 / 31);
    }
    require(std::fwrite(rgb.data(), 1, rgb.size(), output) == rgb.size(), "preview pixels write");
    require(std::fclose(output) == 0, "preview output closes");
}

void checkScreenRegions(VoiceAssistantUi &ui)
{
    lv_obj_update_layout(ui.screen_);
    lv_area_t header{}, body{}, mode{}, page{}, state{}, hint{}, rule{};
    lv_obj_get_coords(ui.title_, &header);
    lv_obj_get_coords(ui.details_viewport_, &body);
    lv_obj_get_coords(ui.page_mode_, &mode);
    lv_obj_get_coords(ui.page_indicator_, &page);
    lv_obj_get_coords(ui.network_, &state);
    lv_obj_get_coords(ui.ip_address_, &hint);
    lv_obj_get_coords(ui.header_rule_, &rule);
    require(header.x1 >= 38 && header.x2 <= 227 && header.y2 < rule.y1,
            "voice mark and mode header occupy separate stable regions");
    require(body.x1 == 12 && body.x2 == 227 && body.y1 == 49 && body.y2 == 141,
            "full-width three-line body preserves its 216 by 93 pixel reading region");
    require(body.y2 < mode.y1 && body.y2 < page.y1 &&
            mode.y2 < state.y1 && page.y2 < state.y1 && mode.x2 < page.x1,
            "reading mode and page number never overlap body or status");
    require(state.y2 < hint.y1 && hint.y2 < kHeight && state.x1 == hint.x1,
            "fixed single-line status and control hints stay within the screen");
    require(lv_label_get_long_mode(ui.title_) == LV_LABEL_LONG_MODE_DOTS &&
            lv_label_get_long_mode(ui.network_) == LV_LABEL_LONG_MODE_DOTS &&
            lv_label_get_long_mode(ui.ip_address_) == LV_LABEL_LONG_MODE_DOTS,
            "auxiliary text elides instead of wrapping or animating");
    require(lv_color_to_u32(lv_obj_get_style_bg_color(ui.screen_, LV_PART_MAIN)) ==
            lv_color_to_u32(lv_color_make(16, 48, 65)),
            "production screen uses the fixed deep sea RGB565 palette");
    const uint16_t mark_pixel = pixels[23 * kWidth + 18];
    require(((mark_pixel >> 5) & 63) > 40 && ((mark_pixel >> 11) & 31) > 10,
            "static voice mark renders the contrasting sea-green accent");
}

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    ++flushes;
    flushed_pixels += static_cast<size_t>(lv_area_get_width(area)) * lv_area_get_height(area);
    const auto *source = reinterpret_cast<const uint16_t *>(data);
    const int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x >= 0 && x < kWidth && y >= 0 && y < kHeight)
                pixels[y * kWidth + x] = source[(y - area->y1) * width + x - area->x1];
        }
    }
    lv_display_flush_ready(display);
}

size_t ink(const lv_area_t &area)
{
    size_t count = 0;
    for (int y = std::max<int32_t>(0, area.y1); y <= std::min<int32_t>(kHeight - 1, area.y2); ++y) {
        for (int x = std::max<int32_t>(0, area.x1); x <= std::min<int32_t>(kWidth - 1, area.x2); ++x) {
            // 白色抗锯齿文字带红色分量；深绿背景低于该阈值。
            // 检查实际渲染像素。
            if (((pixels[y * kWidth + x] >> 11) & 31) >= 12) ++count;
        }
    }
    return count;
}

void checkVisible(VoiceAssistantUi &ui, const char *context)
{
    lv_tick_inc(20);
    lv_refr_now(ui.lv_display_);
    lv_area_t viewport{};
    lv_obj_get_content_coords(ui.details_viewport_, &viewport);
    if (ink(viewport) < 10) {
        lv_area_t label{};
        lv_obj_get_coords(ui.details_, &label);
        std::fprintf(stderr, "blank: %s viewport=(%ld,%ld)-(%ld,%ld) label=(%ld,%ld)-(%ld,%ld) scroll=(%ld,%ld) textbytes=%zu\n",
                     context, static_cast<long>(viewport.x1), static_cast<long>(viewport.y1),
                     static_cast<long>(viewport.x2), static_cast<long>(viewport.y2),
                     static_cast<long>(label.x1), static_cast<long>(label.y1),
                     static_cast<long>(label.x2), static_cast<long>(label.y2),
                     static_cast<long>(lv_obj_get_scroll_x(ui.details_viewport_)),
                     static_cast<long>(lv_obj_get_scroll_y(ui.details_viewport_)),
                     std::strlen(lv_label_get_text(ui.details_)));
    }
    require(ink(viewport) >= 10, context);
}

void checkActivePage(VoiceAssistantUi &ui, uint32_t source_offset)
{
    const char *text = lv_label_get_text(ui.details_);
    require(source_offset < std::strlen(text), "spoken glyph lies inside the full rendered answer");
    const uint32_t character = lv_text_encoded_get_char_id(text, source_offset);
    lv_point_t letter{};
    // 这是独立的公开 LVGL 布局基准；生产实现缓存页边界，
    // 而不是逐字形查询位置。
    lv_label_get_letter_pos(ui.details_, character, &letter);
    lv_area_t label{}, viewport{};
    lv_obj_get_content_coords(ui.details_, &label);
    lv_obj_get_content_coords(ui.details_viewport_, &viewport);
    const int32_t page_height = lv_obj_get_height(ui.details_viewport_);
    require(page_height == 3 * lv_font_get_line_height(&lv_font_chinese_16),
            "automatic subtitle viewport always contains three complete lines");
    require(ui.page_index_ == letter.y / page_height &&
            lv_obj_get_scroll_y(ui.details_viewport_) == (letter.y / page_height) * page_height,
            "automatic page agrees with public LVGL glyph geometry");
    const int32_t glyph_top = label.y1 + letter.y;
    require(glyph_top >= viewport.y1 &&
            glyph_top + lv_font_get_line_height(&lv_font_chinese_16) - 1 <= viewport.y2,
            "current spoken glyph is completely inside the three-line viewport");
    require(lv_obj_get_scroll_x(ui.details_viewport_) == 0 && lv_obj_get_x(ui.details_) == 0,
            "subtitles retain fixed horizontal alignment");
}

uint32_t firstOffsetOnPage(VoiceAssistantUi &ui, uint16_t page)
{
    const char *text = lv_label_get_text(ui.details_);
    uint32_t byte = 0, character = 0;
    const int32_t page_height = lv_obj_get_height(ui.details_viewport_);
    while (text[byte]) {
        lv_point_t letter{};
        lv_label_get_letter_pos(ui.details_, character, &letter);
        if (letter.y / page_height == page) return byte;
        lv_text_encoded_next(text, &byte);
        ++character;
    }
    require(false, "test answer reaches the requested page");
    return 0;
}

void following(VoiceAssistantUi &ui, const char *text, uint32_t start, uint32_t end,
               uint16_t progress, bool reset = false, bool active = true,
               uint32_t next = UINT32_MAX, uint16_t next_progress = 0)
{
    require(ui.update("", text, nullptr, nullptr, 0x0320, 0xffff, false, 0,
                      true, reset, false, active, start, end, progress,
                      next, next_progress) == ESP_OK,
            "production three-line subtitle update succeeds");
}
}

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *)
{
    lv_init();
    return ESP_OK;
}

lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *config)
{
    lv_display_t *display = lv_display_create(config->hres, config->vres);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    return display;
}

esp_err_t St7789Display::attachLvglDisplay(lv_display_t *display)
{
    lvgl_display_ = display;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    if (argc > 1) preview_directory = argv[1];
    St7789Display display;
    display.panel_io_ = reinterpret_cast<void *>(1);
    display.panel_ = reinterpret_cast<void *>(1);
    VoiceAssistantUi ui;
    require(ui.init(display) == ESP_OK, "real LVGL initializes with the production UI");
    const char *short_text = u8"\u8bed\u97f3\u52a9\u624b\u6b63\u5728\u64ad\u62a5\u5b57\u5e55\u7a33\u5b9a\u6d41\u7545";
    following(ui, short_text, 0, 9, 0, true);
    checkVisible(ui, "first three-line subtitle page has rendered text");
    checkActivePage(ui, 0);
    const size_t held_flushes = flushes;
    following(ui, short_text, 0, 9, 500);
    lv_refr_now(ui.lv_display_);
    require(flushes == held_flushes,
            "progress inside the current page sends no redundant screen transfer");
    following(ui, short_text, 0, 9, 750);
    lv_refr_now(ui.lv_display_);
    require(flushes == held_flushes, "a second within-page word change keeps the display still");
    const auto short_bytes = static_cast<uint32_t>(std::strlen(short_text));
    following(ui, short_text, short_bytes - 3, short_bytes, 1000);
    checkVisible(ui, "final character of a short answer stays visible");
    checkActivePage(ui, short_bytes - 3);

    // 使用公开 LVGL 字符位置作为跨页核对基准。
    // 验证真实字体指标、换行边界、裁剪及字体解码器，
    // 不假定每行包含固定数量的汉字。
    std::string long_text;
    for (int i = 0; i < 400; ++i) long_text += u8"\u8bed";
    following(ui, long_text.c_str(), 0, 3, 0, true);
    checkVisible(ui, "long answer begins with three visible lines");
    checkActivePage(ui, 0);
    const uint32_t first_boundary = firstOffsetOnPage(ui, 1);
    const uint32_t late_boundary = firstOffsetOnPage(ui, 5);
    for (uint32_t character = 0; character < 400; character += 7) {
        following(ui, long_text.c_str(), character * 3, character * 3 + 3, 0);
        checkVisible(ui, "each spoken page keeps text visible");
        checkActivePage(ui, character * 3);
    }
    following(ui, long_text.c_str(), 399 * 3, 400 * 3, 1000);
    checkVisible(ui, "last partial page remains visible");
    checkActivePage(ui, 399 * 3);
    lv_point_t final_letter{};
    lv_label_get_letter_pos(ui.details_, 399, &final_letter);
    const int32_t line_height = lv_font_get_line_height(&lv_font_chinese_16);
    require(final_letter.y / line_height % 3 < 2,
            "test final page is shorter than three complete text lines");
    require(lv_obj_get_scroll_y(ui.details_viewport_) == (ui.page_count_ - 1) * 3 * line_height,
            "padded final page aligns at its own first line");

    // 一个带时间戳的词可以跨视觉页。读到第二个字形时
    // 必须翻页，不必等到提供方的下一个词锚点。
    require(first_boundary >= 3, "test has a glyph before the first page boundary");
    following(ui, long_text.c_str(), first_boundary - 3, first_boundary + 6, 0, true);
    checkVisible(ui, "cross-page word starts on the previous page");
    checkActivePage(ui, first_boundary - 3);
    following(ui, long_text.c_str(), first_boundary - 3, first_boundary + 6, 500);
    checkVisible(ui, "within-word progress flips to the next page");
    checkActivePage(ui, first_boundary);
    following(ui, long_text.c_str(), first_boundary - 3, first_boundary + 6, 1000);
    checkActivePage(ui, first_boundary + 3);
    following(ui, long_text.c_str(), first_boundary - 3, first_boundary, 1000, true,
              true, first_boundary, 1000);
    checkActivePage(ui, first_boundary - 3);
    require(ui.page_index_ == 0,
            "a future anchor cannot flip pages early during a punctuation or silence gap");

    following(ui, long_text.c_str(), 0, 3, 0, true);
    checkVisible(ui, "late subtitle scenario starts on its first page");
    following(ui, long_text.c_str(), late_boundary, late_boundary + 3, 0);
    checkVisible(ui, "late subtitle jumps straight to the currently spoken page");
    checkActivePage(ui, late_boundary);
    const int32_t before_append = lv_obj_get_scroll_y(ui.details_viewport_);
    const uint16_t old_pages = ui.page_count_;
    long_text += long_text;
    following(ui, long_text.c_str(), late_boundary, late_boundary + 3, 0);
    checkVisible(ui, "streaming suffix does not empty the current page");
    checkActivePage(ui, late_boundary);
    require(ui.page_count_ > old_pages && lv_obj_get_scroll_y(ui.details_viewport_) == before_append,
            "new subtitle batches extend page count without resetting current reading");
    const int32_t held_scroll = lv_obj_get_scroll_y(ui.details_viewport_);
    following(ui, long_text.c_str(), 50000, 50003, 500);
    checkVisible(ui, "cursor ahead of available text retains readable context");
    require(lv_obj_get_scroll_y(ui.details_viewport_) == held_scroll,
            "a cursor beyond received text holds the page until its text arrives");
    following(ui, long_text.c_str(), 0, 3, 0, false, false);
    checkVisible(ui, "disabled metadata retains the visible page");
    require(lv_obj_get_scroll_y(ui.details_viewport_) == held_scroll,
            "stopping metadata does not jump back to page one");

    // 末尾换行可使 NUL 落在新的空页；
    // 当前词必须留在自身页，不能跟随终止符。
    const char *terminated = u8"\u8bed\n\u97f3\n\u52a9\n";
    const uint32_t last_spoken = static_cast<uint32_t>(std::strlen(terminated) - 4);
    following(ui, terminated, last_spoken, last_spoken + 3, 1000, true);
    checkVisible(ui, "trailing newline does not hide the final spoken character");
    checkActivePage(ui, last_spoken);
    require(ui.page_index_ == 0, "unspoken trailing newline does not select a blank page");
    following(ui, terminated, static_cast<uint32_t>(std::strlen(terminated)),
              static_cast<uint32_t>(std::strlen(terminated) + 3), 0);
    require(ui.page_index_ == 0, "a NUL cursor cannot select the trailing blank page");

    const char *mixed = u8"\u8bed\u97f3 AV 12,\n\u52a9\u624b\U0001F916\u5b57\u5e55\n\u64ad\u62a5\n\u6d4b\u8bd5\n\u7ed3\u675f";
    const uint32_t emoji = static_cast<uint32_t>(std::string(mixed).find(u8"\U0001F916"));
    const uint32_t mixed_tail = static_cast<uint32_t>(std::strlen(mixed) - 3);
    following(ui, mixed, emoji, emoji + 4, 500, true);
    checkVisible(ui, "missing four-byte font glyph preserves surrounding UTF-8 text");
    checkActivePage(ui, emoji);
    following(ui, mixed, mixed_tail, mixed_tail + 3, 500);
    checkVisible(ui, "mixed punctuation and UTF-8 text reach the proper page");
    checkActivePage(ui, mixed_tail);

    // 追加拉丁词的剩余部分可能改变尾部换行。
    // 每次追加后依据 LVGL 重新核对生产页边界。
    std::string reflow;
    for (int i = 0; i < 37; ++i) reflow += u8"\u8bed";
    reflow += " inter";
    following(ui, reflow.c_str(), 34 * 3, 35 * 3, 0, true);
    checkVisible(ui, "streamed Latin word starts beside Chinese context");
    checkActivePage(ui, 34 * 3);
    reflow += "nationalization next sentence";
    following(ui, reflow.c_str(), 34 * 3, 35 * 3, 0);
    checkVisible(ui, "word-wrap reflow retains the current Chinese word");
    checkActivePage(ui, 34 * 3);
    const uint32_t latin_tail = static_cast<uint32_t>(reflow.size() - 1);
    following(ui, reflow.c_str(), latin_tail, latin_tail + 1, 0);
    checkVisible(ui, "appended Latin suffix reaches its actual wrapped page");
    checkActivePage(ui, latin_tail);

    // 通过公开 render 方法验证修订分发。
    VoiceAssistantStatus status;
    WifiStation wifi;
    status.enableRealtime();
    status.setSession(VoiceSessionState::Ready);
    status.setResponse(VoiceResponseState::Generating);
    status.setPlayback(VoicePlaybackState::Playing);
    status.resetSubtitle(8, 1, 2, 3);
    status.setSubtitleText(8, 1, 2, 3, long_text.c_str());
    status.setSubtitlePosition(8, 1, 2, 3, 0, true, 3, 0, 1000, 0, 0);
    require(ui.render(status, wifi) == ESP_OK, "public render starts three-line following");
    checkVisible(ui, "public render current page has text");
    checkActivePage(ui, 0);
    const size_t render_held_flushes = flushes;
    status.setSubtitlePosition(8, 1, 2, 3, 3, true, 6, 1000, 2000, 1500, 500);
    require(ui.render(status, wifi) == ESP_OK, "subtitle-only revision reaches the cached page path");
    lv_refr_now(ui.lv_display_);
    require(flushes == render_held_flushes,
            "public render does not repaint for progress inside the same page");
    const auto conversation_revision = status.conversationRevision();
    status.setSubtitlePosition(8, 1, 2, 3, late_boundary, true, late_boundary + 3,
                               2000, 3000, 2500, 500);
    require(ui.render(status, wifi) == ESP_OK && status.conversationRevision() == conversation_revision,
            "subtitle-only page flip retains the cached answer copy");
    checkVisible(ui, "public render late cue selects its current page");
    checkActivePage(ui, late_boundary);
    const int32_t frozen_scroll = lv_obj_get_scroll_y(ui.details_viewport_);
    lv_tick_inc(30000);
    require(ui.render(status, wifi) == ESP_OK, "wall-clock-only update succeeds");
    lv_refr_now(ui.lv_display_);
    require(lv_obj_get_scroll_y(ui.details_viewport_) == frozen_scroll,
            "DMA freeze prevents wall-clock page extrapolation");
    status.disableSubtitle(8, 1, 2, 3);
    require(ui.render(status, wifi) == ESP_OK, "final DMA disable preserves the page");
    lv_refr_now(ui.lv_display_);
    require(lv_obj_get_scroll_y(ui.details_viewport_) == frozen_scroll,
            "stopping metadata keeps the visible page");
    status.setPlayback(VoicePlaybackState::Idle);
    status.setResponse(VoiceResponseState::Complete);
    require(ui.render(status, wifi) == ESP_OK &&
            lv_obj_get_scroll_y(ui.details_viewport_) == frozen_scroll,
            "completed playback holds the final automatic page");
    require(ui.command('[') && ui.render(status, wifi) == ESP_OK && ui.manual_browse_,
            "manual browsing remains available after playback completes");
    const int32_t manual_scroll = lv_obj_get_scroll_y(ui.details_viewport_);
    status.setPlayback(VoicePlaybackState::Playing);
    status.setSubtitlePosition(8, 1, 2, 3, 0, true, 3, 3000, 4000, 3500, 500);
    require(ui.render(status, wifi) == ESP_OK &&
            lv_obj_get_scroll_y(ui.details_viewport_) == manual_scroll,
            "manual page selection wins over subsequent sample updates");
    status.resetSubtitle(8, 2, 3, 4);
    status.setSubtitleText(8, 2, 3, 4, short_text);
    status.setSubtitlePosition(8, 2, 3, 4, 0, true, 3, 0, 1000, 0, 0);
    require(ui.render(status, wifi) == ESP_OK && !ui.manual_browse_ && ui.page_index_ == 0,
            "a new reply resets manual reading and resumes automatic following");
    checkVisible(ui, "new reply clears deeply scrolled old pages");
    checkActivePage(ui, 0);

    // 代表性生产屏幕使用真实字体及软件渲染器。
    // 额外品牌图形在全部播放样本间保持静态。
    status.disableSubtitle(8, 2, 3, 4);
    status.setPlayback(VoicePlaybackState::Idle);
    status.setResponse(VoiceResponseState::Idle);
    status.setListening(true);
    status.setFrontend(VoiceFrontendState::Silence);
    wifi.state_.store(WifiStation::State::Connected);
    wifi.ip_address_.store(0x0100007f);
    require(ui.render(status, wifi) == ESP_OK, "designed idle screen renders");
    checkVisible(ui, "idle reading instructions remain visible");
    checkScreenRegions(ui);
    savePreview("voice-ui-idle");
    const std::string answer = u8"今天的天气适合出门走走。带上一杯水，沿着熟悉的小路慢慢散步。也可以听听音乐，让自己放松一下。";
    status.setResponse(VoiceResponseState::Generating);
    status.setPlayback(VoicePlaybackState::Playing);
    status.setBargeInEnabled(true);
    status.setBargeIn(VoiceBargeInState::Armed);
    status.resetSubtitle(8, 3, 4, 5);
    status.setSubtitleText(8, 3, 4, 5, answer.c_str());
    status.setSubtitlePosition(8, 3, 4, 5, 0, true, 3, 0, 1000, 0, 0);
    require(ui.render(status, wifi) == ESP_OK, "designed full-answer playback screen renders");
    checkVisible(ui, "designed playback shows a full first page");
    checkScreenRegions(ui);
    require(std::strcmp(lv_label_get_text(ui.page_mode_), u8"随声翻页") == 0,
            "automatic paging state has a dedicated reading label");
    savePreview("voice-ui-speaking");
    const auto design_flushes = flushes;
    for (uint32_t sample = 20; sample <= 1000; sample += 20) {
        status.setSubtitlePosition(8, 3, 4, 5, 0, true, 3, 0, 1000, sample,
                                   static_cast<uint16_t>(sample));
        require(ui.render(status, wifi) == ESP_OK, "designed playback sample update succeeds");
        lv_refr_now(ui.lv_display_);
    }
    require(flushes == design_flushes,
            "static design adds no screen transfer across fifty same-page playback samples");
    std::printf("Real LVGL UI: %zu pixel/layout checks passed (%zu partial flushes; host only, hardware pending)\n", checks, flushes);
    lv_deinit();
}
