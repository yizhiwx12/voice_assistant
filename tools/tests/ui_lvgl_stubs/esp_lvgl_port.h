#pragma once
#include "esp_err.h"
#include "lvgl.h"

struct lvgl_port_cfg_t {};
#define ESP_LVGL_PORT_INIT_CONFIG() lvgl_port_cfg_t{}
struct lvgl_port_display_cfg_t {
    void *io_handle = nullptr, *panel_handle = nullptr;
    uint32_t buffer_size = 0;
    bool double_buffer = false;
    int hres = 0, vres = 0;
    bool monochrome = false;
    lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565;
    struct { bool buff_dma = false, swap_bytes = false; } flags;
};
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *);
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *);
inline bool lvgl_port_lock(uint32_t) { return true; }
inline void lvgl_port_unlock() {}
