#pragma once

// 仅主机使用的几何近似，用于生产 UI 状态/分页检查。
// 真实字形指标、像素布局及 LVGL 分配仍需设备测试。
#include <algorithm>
#include <cstdint>
#include <string>

using lv_color_t = uint32_t;
struct lv_point_t { int32_t x = 0, y = 0; };
struct lv_font_t { int line_height = 31; };
struct lv_obj_t {
    std::string text;
    int width = 224, height = 31, scroll_y = 0, x = 0, y = 0;
    uint32_t text_updates = 0;
    uint32_t mode_updates = 0, flags = 0;
    bool content_height = false, hidden = false;
};
struct lv_timer_t { uint32_t period = 33; };
struct lv_display_t { lv_obj_t screen; lv_timer_t refresh_timer; };
struct lv_event_t;
inline uint32_t ui_test_tick = 1000;
inline uint32_t ui_test_layout_updates = 0;
inline uint32_t ui_test_animation_starts = 0;
inline lv_timer_t ui_test_animation_timer;
inline bool ui_test_finish_animation = true;
inline constexpr int LV_PART_MAIN = 0, LV_PART_INDICATOR = 1;
inline constexpr int LV_ALIGN_TOP_MID = 0, LV_ALIGN_TOP_LEFT = 1, LV_ANIM_OFF = 0, LV_ANIM_ON = 1, LV_DIR_NONE = 2, LV_DIR_VER = 0, LV_DIR_HOR = 1;
inline constexpr int LV_COLOR_FORMAT_RGB565 = 0, LV_OPA_COVER = 255, LV_OPA_TRANSP = 0;
inline constexpr int LV_TEXT_ALIGN_LEFT = 1, LV_TEXT_ALIGN_CENTER = 0, LV_TEXT_ALIGN_RIGHT = 2, LV_SCROLLBAR_MODE_OFF = 0, LV_SIZE_CONTENT = -1;
inline constexpr int LV_LABEL_LONG_MODE_WRAP = 0, LV_LABEL_LONG_MODE_CLIP = 1, LV_LABEL_LONG_MODE_DOTS = 2;
inline constexpr int LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_FLOATING = 2;

inline lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}
inline lv_obj_t *lv_display_get_screen_active(lv_display_t *display) { return &display->screen; }
inline lv_timer_t *lv_display_get_refr_timer(lv_display_t *display) { return &display->refresh_timer; }
inline lv_timer_t *lv_anim_get_timer() { return &ui_test_animation_timer; }
inline void lv_timer_set_period(lv_timer_t *timer, uint32_t period) { timer->period = period; }
inline lv_obj_t *lv_label_create(lv_obj_t *) { return new lv_obj_t; }
inline lv_obj_t *lv_obj_create(lv_obj_t *) { return new lv_obj_t; }
inline lv_obj_t *lv_bar_create(lv_obj_t *) { return new lv_obj_t; }
inline void lv_label_set_text(lv_obj_t *object, const char *text) {
    object->text = text ? text : "";
    ++object->text_updates;
}
inline const char *lv_label_get_text(lv_obj_t *object) { return object->text.c_str(); }
inline void lv_label_set_text_static(lv_obj_t *object, const char *text) { lv_label_set_text(object, text); }
inline void lv_obj_set_width(lv_obj_t *object, int width) { object->width = width; }
inline void lv_obj_set_height(lv_obj_t *object, int height) {
    object->content_height = height == LV_SIZE_CONTENT;
    if (!object->content_height) object->height = height;
}
inline void lv_obj_set_size(lv_obj_t *object, int width, int height) {
    object->width = width; object->height = height;
}
inline int lv_obj_get_height(lv_obj_t *object) {
    if (!object->content_height) return object->height;
    const size_t columns = std::max(1, object->width / 8);
    size_t line = 0, lines = 1;
    for (const unsigned char value : object->text) {
        if ((value & 0xc0) == 0x80) continue;
        if (value == '\n') { ++lines; line = 0; }
        else if (++line > columns) { ++lines; line = 1; }
    }
    return static_cast<int>(lines) * 31;
}
inline void lv_obj_scroll_to_y(lv_obj_t *object, int y, int) { object->scroll_y = y; }
inline int32_t lv_obj_get_scroll_y(lv_obj_t *object) { return object->scroll_y; }
inline void lv_obj_scroll_to_x(lv_obj_t *object, int x, int) { object->x = x; }
inline int32_t lv_obj_get_scroll_x(lv_obj_t *object) { return object->x; }
inline void lv_obj_set_x(lv_obj_t *object, int x) { object->x = x; }
inline int32_t lv_obj_get_x(const lv_obj_t *object) { return object->x; }
inline void lv_label_get_letter_pos(const lv_obj_t *object, uint32_t character, lv_point_t *point) {
    const size_t columns = std::max(1, object->width / 8);
    size_t column = 0, row = 0;
    uint32_t seen = 0;
    for (const unsigned char value : object->text) {
        if ((value & 0xc0) == 0x80) continue;
        if (seen++ == character) break;
        if (value == '\n') { ++row; column = 0; }
        else if (++column >= columns) { ++row; column = 0; }
    }
    point->x = static_cast<int32_t>(column * 8);
    point->y = static_cast<int32_t>(row * 31);
}
struct lv_anim_t {
    void *var = nullptr;
    void (*execute)(void *, int32_t) = nullptr;
    int32_t end = 0;
};
inline void lv_anim_delete(void *, void (*)(void *, int32_t)) {}
inline void lv_anim_init(lv_anim_t *animation) { *animation = {}; }
inline void lv_anim_set_var(lv_anim_t *animation, void *var) { animation->var = var; }
inline void lv_anim_set_exec_cb(lv_anim_t *animation, void (*execute)(void *, int32_t)) {
    animation->execute = execute;
}
inline void lv_anim_set_values(lv_anim_t *animation, int32_t, int32_t end) { animation->end = end; }
inline void lv_anim_set_duration(lv_anim_t *, uint32_t) {}
inline int32_t lv_anim_path_ease_out(const lv_anim_t *) { return 0; }
inline void lv_anim_set_path_cb(lv_anim_t *, int32_t (*)(const lv_anim_t *)) {}
inline void lv_anim_start(lv_anim_t *animation) {
    ++ui_test_animation_starts;
    if (ui_test_finish_animation) animation->execute(animation->var, animation->end);
}
inline void lv_obj_add_flag(lv_obj_t *object, int flag) { object->flags |= flag; object->hidden = object->flags & LV_OBJ_FLAG_HIDDEN; }
inline void lv_obj_clear_flag(lv_obj_t *object, int flag) { object->flags &= ~flag; object->hidden = object->flags & LV_OBJ_FLAG_HIDDEN; }
inline void lv_obj_remove_style_all(lv_obj_t *) {}
inline int lv_font_get_line_height(const lv_font_t *font) { return font->line_height; }
inline uint32_t lv_tick_get() { return ui_test_tick; }
inline void lv_obj_align(lv_obj_t *object, int, int x, int y) { object->x = x; object->y = y; }
inline int32_t lv_text_get_width(const char *text, uint32_t length, const lv_font_t *, int32_t) {
    int32_t width = 0;
    for (uint32_t i = 0; i < length; ++i) {
        const unsigned char value = static_cast<unsigned char>(text[i]);
        if ((value & 0xc0) == 0x80) continue;
        width += value < 0x80 ? 8 : 16;
    }
    return width;
}
inline uint16_t lv_font_get_glyph_width(const lv_font_t *, uint32_t current, uint32_t) {
    return current ? current < 0x80 ? 8 : 16 : 0;
}
inline void lv_obj_update_layout(lv_obj_t *) { ++ui_test_layout_updates; }
inline void lv_label_set_long_mode(lv_obj_t *object, int) { ++object->mode_updates; }
inline void lv_obj_set_scroll_dir(lv_obj_t *, int) {}
inline void lv_obj_set_scrollbar_mode(lv_obj_t *, int) {}
inline void lv_obj_set_style_bg_color(lv_obj_t *, lv_color_t, int) {}
inline void lv_obj_set_style_bg_opa(lv_obj_t *, int, int) {}
inline void lv_obj_set_style_border_width(lv_obj_t *, int, int) {}
inline void lv_obj_set_style_pad_all(lv_obj_t *, int, int) {}
inline void lv_obj_set_style_radius(lv_obj_t *, int, int) {}
inline void lv_obj_set_style_text_align(lv_obj_t *, int, int) {}
inline void lv_obj_set_style_text_color(lv_obj_t *, lv_color_t, int) {}
inline void lv_obj_set_style_text_font(lv_obj_t *, const lv_font_t *, int) {}
inline void lv_obj_set_style_text_line_space(lv_obj_t *, int, int) {}
inline void lv_obj_set_style_text_letter_space(lv_obj_t *, int, int) {}
inline void lv_bar_set_range(lv_obj_t *, int, int) {}
inline void lv_bar_set_value(lv_obj_t *, int, int) {}
