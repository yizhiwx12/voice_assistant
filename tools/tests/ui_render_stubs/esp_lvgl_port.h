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
    int color_format = 0;
    struct { bool buff_dma = false, swap_bytes = false; } flags;
};
inline esp_err_t lvgl_port_init(const lvgl_port_cfg_t *) { return ESP_OK; }
inline lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *) { return new lv_display_t; }
inline bool ui_test_lvgl_lock_result = true;
inline bool lvgl_port_lock(uint32_t) { return ui_test_lvgl_lock_result; }
inline void lvgl_port_unlock() {}
