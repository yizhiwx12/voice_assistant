#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "../app/board_pins.hpp"

// ST7789 底层屏幕驱动：管理 SPI、DMA 和背光，界面布局由 VoiceAssistantUi 负责。
// 接线见 app/board_pins.hpp；pixel_clock_hz 是 SPI 时钟，不是屏幕帧率。
class St7789Display {
public:
    static constexpr int kDefaultWidth = 240;
    static constexpr int kDefaultHeight = 240;
    // 中文字库行高为 31 像素。一整行字幕必须能放进单个绘图缓冲和 SPI 事务，
    // 避免拆成两次局部刷新。
    // 双缓冲仍可让下一次渲染与颜色 DMA 并行。
    static constexpr uint32_t kDrawBufferLines = 32;

    struct Config {
        int width = kDefaultWidth;
        int height = kDefaultHeight;
        gpio_num_t sclk_gpio = voice::board::kDisplayClock;
        gpio_num_t mosi_gpio = voice::board::kDisplayMosi;
        gpio_num_t cs_gpio = voice::board::kDisplayCs;
        gpio_num_t dc_gpio = voice::board::kDisplayDc;
        gpio_num_t backlight_gpio = voice::board::kDisplayBacklight;
        gpio_num_t reset_gpio = voice::board::kDisplayReset;
        int pixel_clock_hz = 40'000'000;
        int x_gap = 0;
        int y_gap = 0;
        lcd_rgb_element_order_t color_order = LCD_RGB_ELEMENT_ORDER_RGB;
        bool invert_colors = true;
    };

    St7789Display() = default;
    St7789Display(const St7789Display &) = delete;
    St7789Display &operator=(const St7789Display &) = delete;

    esp_err_t init();
    esp_err_t init(const Config &config);
    esp_err_t deinit();
    esp_err_t attachLvglDisplay(lv_display_t *display);

    int width() const { return config_.width; }
    int height() const { return config_.height; }
    esp_lcd_panel_io_handle_t panelIoHandle() const { return panel_io_; }
    esp_lcd_panel_handle_t panelHandle() const { return panel_; }

    esp_err_t setRotation(uint8_t rotation);
    esp_err_t setGap(int x_gap, int y_gap);
    esp_err_t setColorInversion(bool enabled);
    esp_err_t setBacklightBrightness(uint8_t percent);
    esp_err_t displayOn(bool enabled);
    esp_err_t sleep(bool enabled);

    esp_err_t fillScreen(uint16_t color);
    esp_err_t fillRect(int x, int y, int width, int height, uint16_t color);
    esp_err_t drawRect(int x, int y, int width, int height, uint16_t color);
    esp_err_t drawPixel(int x, int y, uint16_t color);
    esp_err_t drawLine(int x0, int y0, int x1, int y1, uint16_t color);
    esp_err_t drawBitmap(int x, int y, int width, int height, const uint16_t *pixels);
    esp_err_t drawText(int x, int y, const char *text, uint16_t foreground,
                       uint16_t background, uint8_t scale = 1);

    static constexpr uint16_t color565(uint8_t red, uint8_t green, uint8_t blue)
    {
        return static_cast<uint16_t>(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
    }

private:
    static constexpr int kChunkRows = 16;
    static constexpr int kBacklightPwmResolution = 10;
    static constexpr uint32_t kBacklightPwmMax = (1U << kBacklightPwmResolution) - 1U;

    esp_err_t fillRectUnlocked(int x, int y, int width, int height, uint16_t color);
    esp_err_t drawPixelUnlocked(int x, int y, uint16_t color);
    esp_err_t drawBitmapUnlocked(int x, int y, int width, int height, const uint16_t *pixels);
    esp_err_t pushBlockUnlocked(int x, int y, int width, int height, const uint16_t *pixels);
    static void waitForLvglFlush(lv_display_t *display);
    static void observeLvglRefresh(lv_event_t *event);

    Config config_{};
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t *dma_buffer_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    bool spi_bus_initialized_ = false;
    bool backlight_initialized_ = false;
    bool ready_ = false;
    lv_display_t *lvgl_display_ = nullptr;
    // 只由 LVGL 任务访问，不在 SPI 完成中断中访问。
    int64_t refresh_started_us_ = 0;
    int64_t perf_window_started_us_ = 0;
    int64_t perf_last_refresh_us_ = 0;
    uint32_t refresh_flushes_ = 0;
    uint32_t refresh_bytes_ = 0;
    uint32_t perf_refreshes_ = 0;
    uint32_t perf_flushes_ = 0;
    uint64_t perf_bytes_ = 0;
    uint64_t perf_refresh_us_ = 0;
    uint32_t perf_max_refresh_us_ = 0;
};
