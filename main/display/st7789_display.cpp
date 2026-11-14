#include "st7789_display.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

namespace {
constexpr char kTag[] = "st7789";
constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;

struct FontGlyph {
    char character;
    std::array<uint8_t, 7> rows;
};

constexpr std::array<FontGlyph, 39> kFont = {{
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
}};

const FontGlyph &findGlyph(char character)
{
    if (character >= 'a' && character <= 'z') {
        character = static_cast<char>(character - ('a' - 'A'));
    }
    for (const FontGlyph &glyph : kFont) {
        if (glyph.character == character) {
            return glyph;
        }
    }
    for (const FontGlyph &glyph : kFont) {
        if (glyph.character == '?') {
            return glyph;
        }
    }
    return kFont.back();
}
}

esp_err_t St7789Display::init()
{
    return init(Config{});
}

esp_err_t St7789Display::init(const Config &config)
{
    if (ready_ || config.width <= 0 || config.height <= 0 || config.pixel_clock_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;
    const size_t dma_buffer_bytes = static_cast<size_t>(config_.width) * kChunkRows * sizeof(uint16_t);
    dma_buffer_ = static_cast<uint16_t *>(heap_caps_malloc(dma_buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    mutex_ = xSemaphoreCreateMutex();
    if (dma_buffer_ == nullptr || mutex_ == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate display buffers or synchronization objects");
        deinit();
        return ESP_ERR_NO_MEM;
    }

    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = kBacklightSpeedMode;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.timer_num = kBacklightTimer;
    timer_config.freq_hz = 5000;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        deinit();
        return err;
    }

    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = config_.backlight_gpio;
    channel_config.speed_mode = kBacklightSpeedMode;
    channel_config.channel = kBacklightChannel;
    channel_config.timer_sel = kBacklightTimer;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        deinit();
        return err;
    }
    backlight_initialized_ = true;

    spi_bus_config_t bus_config{};
    bus_config.sclk_io_num = config_.sclk_gpio;
    bus_config.mosi_io_num = config_.mosi_gpio;
    bus_config.miso_io_num = -1;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    // 原始启动绘图缓冲小于 LVGL 绘图缓冲。
    // 若按启动缓冲设置总线传输上限，会把完整 LVGL 刷新拆开，
    // 必须等首个 DMA 块完成后才能排入其余部分。
    bus_config.max_transfer_sz = static_cast<int>(
        static_cast<size_t>(config_.width) * kDrawBufferLines * sizeof(uint16_t));
    err = spi_bus_initialize(kSpiHost, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        deinit();
        return err;
    }
    spi_bus_initialized_ = true;

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = config_.cs_gpio;
    io_config.dc_gpio_num = config_.dc_gpio;
    io_config.spi_mode = 0;
    io_config.pclk_hz = config_.pixel_clock_hz;
    io_config.trans_queue_depth = 1;
    // 接入 LVGL 时会注册唯一的传输完成回调。
    // 接入前的直接绘图改用轮询屏障等待。
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &panel_io_);
    if (err != ESP_OK) {
        deinit();
        return err;
    }

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = config_.reset_gpio;
    panel_config.rgb_ele_order = config_.color_order;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panel_config.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel_);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(panel_, config_.x_gap, config_.y_gap);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(panel_, config_.invert_colors);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_, true);
    }
    if (err != ESP_OK) {
        deinit();
        return err;
    }

    ready_ = true;
    ESP_LOGI(kTag, "ST7789 ready: %dx%d, SPI %d Hz", config_.width, config_.height, config_.pixel_clock_hz);
    return setBacklightBrightness(70);
}

esp_err_t St7789Display::deinit()
{
    // 接入后，LVGL 仍会持有刷新等待钩子和事件回调。
    // 显示对象还存活时不能销毁它们依赖的面板输入输出上下文。
    if (lvgl_display_ != nullptr) return ESP_ERR_INVALID_STATE;
    ready_ = false;
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
        panel_io_ = nullptr;
    }
    if (spi_bus_initialized_) {
        spi_bus_free(kSpiHost);
        spi_bus_initialized_ = false;
    }
    if (backlight_initialized_) {
        ledc_stop(kBacklightSpeedMode, kBacklightChannel, 0);
        backlight_initialized_ = false;
    }
    if (dma_buffer_ != nullptr) {
        heap_caps_free(dma_buffer_);
        dma_buffer_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    return ESP_OK;
}

esp_err_t St7789Display::attachLvglDisplay(lv_display_t *display)
{
    if (!ready_ || panel_io_ == nullptr || display == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    // 托管显示适配层拥有 driver_data 和 DMA 完成回调。
    // 其 SPI 路径保留了 LVGL 默认忙等；这里只使用 user_data 和公开等待钩子，
    // 完成通知仍由适配层负责。
    if (lv_display_get_user_data(display) != nullptr &&
        lv_display_get_user_data(display) != this) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    lvgl_display_ = display;
    lv_display_set_user_data(display, this);
    lv_display_set_flush_wait_cb(display, waitForLvglFlush);
    lv_display_remove_event_cb_with_user_data(display, observeLvglRefresh, this);
    lv_display_add_event_cb(display, observeLvglRefresh, LV_EVENT_REFR_START, this);
    lv_display_add_event_cb(display, observeLvglRefresh, LV_EVENT_FLUSH_START, this);
    lv_display_add_event_cb(display, observeLvglRefresh, LV_EVENT_REFR_READY, this);
    // LVGL 接管后不再允许直接绘图。
    // 将启动绘图缓冲归还内部 RAM，避免音频、网络和字幕同时运行时
    // 仍占用约 7.5 KiB 空间。
    if (dma_buffer_ != nullptr) {
        heap_caps_free(dma_buffer_);
        dma_buffer_ = nullptr;
    }
    xSemaphoreGive(mutex_);
    lvgl_port_unlock();
    ESP_LOGI(kTag, "LVGL DMA: %lu-line double buffers, blocking flush fence, raw scratch released",
             static_cast<unsigned long>(kDrawBufferLines));
    return ESP_OK;
}

void St7789Display::waitForLvglFlush(lv_display_t *display)
{
    auto *driver = static_cast<St7789Display *>(lv_display_get_user_data(display));
    assert(driver != nullptr && driver->panel_io_ != nullptr);
    // 在 LVGL 任务中运行，不在 SPI 中断中运行。IDF 参数接口通过
    // spi_device_get_trans_result() 等待已排队的颜色传输，
    // 阻塞等待而不是占满音频和 Wi-Fi 所需的 CPU。
    // 命令为 -1 且没有参数时不发送任何命令；原有适配层中断回调
    // 仍是唯一调用 lv_display_flush_ready() 的位置。
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(driver->panel_io_, -1, nullptr, 0));
}

void St7789Display::observeLvglRefresh(lv_event_t *event)
{
    auto *driver = static_cast<St7789Display *>(lv_event_get_user_data(event));
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_REFR_START) {
        driver->refresh_started_us_ = esp_timer_get_time();
        driver->refresh_flushes_ = 0;
        driver->refresh_bytes_ = 0;
        return;
    }
    if (code == LV_EVENT_FLUSH_START) {
        const auto *area = static_cast<const lv_area_t *>(lv_event_get_param(event));
        if (area != nullptr) {
            ++driver->refresh_flushes_;
            driver->refresh_bytes_ += static_cast<uint32_t>(lv_area_get_width(area)) *
                static_cast<uint32_t>(lv_area_get_height(area)) * sizeof(uint16_t);
        }
        return;
    }
    if (code != LV_EVENT_REFR_READY || driver->refresh_flushes_ == 0) return;

    const int64_t now = esp_timer_get_time();
    // 空闲后重新开始计时：画面未变不算慢动画。
    if (driver->perf_window_started_us_ == 0 || now - driver->perf_last_refresh_us_ > 250'000) {
        driver->perf_window_started_us_ = driver->refresh_started_us_;
        driver->perf_refreshes_ = 0;
        driver->perf_flushes_ = 0;
        driver->perf_bytes_ = 0;
        driver->perf_refresh_us_ = 0;
        driver->perf_max_refresh_us_ = 0;
    }
    driver->perf_last_refresh_us_ = now;
    const uint32_t elapsed = static_cast<uint32_t>(now - driver->refresh_started_us_);
    ++driver->perf_refreshes_;
    driver->perf_flushes_ += driver->refresh_flushes_;
    driver->perf_bytes_ += driver->refresh_bytes_;
    driver->perf_refresh_us_ += elapsed;
    driver->perf_max_refresh_us_ = std::max(driver->perf_max_refresh_us_, elapsed);
    if (now - driver->perf_window_started_us_ < 5'000'000) return;

    // REFR_READY 表示 LVGL 渲染和提交周期结束，但最后一段 DMA 可能仍未完成。
    // 因此这既不是面板帧率，也不是完整 DMA 延迟。
    ESP_LOGI(kTag, "display_perf cycles=%lu flushes=%lu bytes=%llu avg_us=%llu max_us=%lu dma_tail_excluded=1",
             static_cast<unsigned long>(driver->perf_refreshes_),
             static_cast<unsigned long>(driver->perf_flushes_),
             static_cast<unsigned long long>(driver->perf_bytes_),
             static_cast<unsigned long long>(driver->perf_refresh_us_ / driver->perf_refreshes_),
             static_cast<unsigned long>(driver->perf_max_refresh_us_));
    driver->perf_window_started_us_ = 0;
}

esp_err_t St7789Display::pushBlockUnlocked(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (lvgl_display_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, x, y, x + width, y + height, pixels);
    if (err != ESP_OK) {
        return err;
    }
    // 按接口文档，参数调用返回前会等已排队的颜色 DMA 结束。
    // 命令 -1 且无参数时不发送命令，也不改变像素缓冲。
    return esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0);
}

esp_err_t St7789Display::fillRectUnlocked(int x, int y, int width, int height, uint16_t color)
{
    if (lvgl_display_ != nullptr || dma_buffer_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t right = static_cast<int64_t>(x) + width;
    const int64_t bottom = static_cast<int64_t>(y) + height;
    const int clipped_x = std::max(0, x);
    const int clipped_y = std::max(0, y);
    const int clipped_right = static_cast<int>(std::min<int64_t>(config_.width, right));
    const int clipped_bottom = static_cast<int>(std::min<int64_t>(config_.height, bottom));
    if (clipped_x >= clipped_right || clipped_y >= clipped_bottom) {
        return ESP_OK;
    }

    const int clipped_width = clipped_right - clipped_x;
    for (int row = clipped_y; row < clipped_bottom;) {
        const int rows = std::min(kChunkRows, clipped_bottom - row);
        const size_t pixel_count = static_cast<size_t>(clipped_width) * rows;
        std::fill_n(dma_buffer_, pixel_count, color);
        esp_err_t err = pushBlockUnlocked(clipped_x, row, clipped_width, rows, dma_buffer_);
        if (err != ESP_OK) {
            return err;
        }
        row += rows;
    }
    return ESP_OK;
}

esp_err_t St7789Display::drawPixelUnlocked(int x, int y, uint16_t color)
{
    if (lvgl_display_ != nullptr || dma_buffer_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) {
        return ESP_OK;
    }
    dma_buffer_[0] = color;
    return pushBlockUnlocked(x, y, 1, 1, dma_buffer_);
}

esp_err_t St7789Display::drawBitmapUnlocked(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (lvgl_display_ != nullptr || dma_buffer_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == nullptr || width <= 0 || height <= 0 || x < 0 || y < 0 ||
        static_cast<int64_t>(x) + width > config_.width ||
        static_cast<int64_t>(y) + height > config_.height) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int row = 0; row < height;) {
        const int rows = std::min(kChunkRows, height - row);
        const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint16_t);
        for (int i = 0; i < rows; ++i) {
            std::memcpy(dma_buffer_ + static_cast<size_t>(i) * width,
                        pixels + static_cast<size_t>(row + i) * width,
                        row_bytes);
        }
        esp_err_t err = pushBlockUnlocked(x, y + row, width, rows, dma_buffer_);
        if (err != ESP_OK) {
            return err;
        }
        row += rows;
    }
    return ESP_OK;
}

esp_err_t St7789Display::setRotation(uint8_t rotation)
{
    if (!ready_ || rotation > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    switch (rotation) {
    case 0: break;
    case 1: swap_xy = true; mirror_x = true; break;
    case 2: mirror_x = true; mirror_y = true; break;
    case 3: swap_xy = true; mirror_y = true; break;
    }
    esp_err_t err = esp_lcd_panel_swap_xy(panel_, swap_xy);
    if (err == ESP_OK) {
        err = esp_lcd_panel_mirror(panel_, mirror_x, mirror_y);
    }
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::setGap(int x_gap, int y_gap)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = esp_lcd_panel_set_gap(panel_, x_gap, y_gap);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::setColorInversion(bool enabled)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = esp_lcd_panel_invert_color(panel_, enabled);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::setBacklightBrightness(uint8_t percent)
{
    if (!ready_ || !backlight_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const uint32_t duty = (kBacklightPwmMax * percent) / 100U;
    esp_err_t err = ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(kBacklightSpeedMode, kBacklightChannel);
    }
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::displayOn(bool enabled)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = esp_lcd_panel_disp_on_off(panel_, enabled);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::sleep(bool enabled)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = esp_lcd_panel_disp_sleep(panel_, enabled);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::fillScreen(uint16_t color)
{
    return fillRect(0, 0, config_.width, config_.height, color);
}

esp_err_t St7789Display::fillRect(int x, int y, int width, int height, uint16_t color)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = fillRectUnlocked(x, y, width, height, color);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::drawRect(int x, int y, int width, int height, uint16_t color)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t err = fillRectUnlocked(x, y, width, 1, color);
    if (err == ESP_OK && height > 1) {
        err = fillRectUnlocked(x, y + height - 1, width, 1, color);
    }
    if (err == ESP_OK && height > 2) {
        err = fillRectUnlocked(x, y + 1, 1, height - 2, color);
    }
    if (err == ESP_OK && height > 2 && width > 1) {
        err = fillRectUnlocked(x + width - 1, y + 1, 1, height - 2, color);
    }
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::drawPixel(int x, int y, uint16_t color)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = drawPixelUnlocked(x, y, color);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::drawLine(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x0 == x1) {
        return fillRect(x0, std::min(y0, y1), 1, std::abs(y1 - y0) + 1, color);
    }
    if (y0 == y1) {
        return fillRect(std::min(x0, x1), y0, std::abs(x1 - x0) + 1, 1, color);
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    esp_err_t err = ESP_OK;
    while (true) {
        err = drawPixelUnlocked(x0, y0, color);
        if (err != ESP_OK || (x0 == x1 && y0 == y1)) {
            break;
        }
        const int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::drawBitmap(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t err = drawBitmapUnlocked(x, y, width, height, pixels);
    xSemaphoreGive(mutex_);
    return err;
}

esp_err_t St7789Display::drawText(int x, int y, const char *text, uint16_t foreground,
                                  uint16_t background, uint8_t scale)
{
    if (!ready_ || text == nullptr || scale == 0 || scale > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t length = std::strlen(text);
    const int width = static_cast<int>(length * 6U * scale);
    const int height = 7 * scale;
    if (length == 0 || width <= 0 || height <= 0 || width > config_.width ||
        height > 24 || x < 0 || y < 0 || x + width > config_.width ||
        y + height > config_.height) {
        return ESP_ERR_INVALID_ARG;
    }

    static std::array<uint16_t, kDefaultWidth * 24> bitmap{};
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int row = 0; row < height; ++row) {
        std::fill_n(bitmap.data() + static_cast<size_t>(row) * width, width, background);
    }

    for (size_t character_index = 0; character_index < length; ++character_index) {
        const FontGlyph &glyph = findGlyph(text[character_index]);
        const int glyph_x = static_cast<int>(character_index * 6U * scale);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph.rows[static_cast<size_t>(row)] & (1U << (4 - column))) == 0) {
                    continue;
                }
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        bitmap[static_cast<size_t>(row * scale + dy) * width +
                               glyph_x + column * scale + dx] = foreground;
                    }
                }
            }
        }
    }

    const esp_err_t err = drawBitmapUnlocked(x, y, width, height, bitmap.data());
    xSemaphoreGive(mutex_);
    return err;
}
