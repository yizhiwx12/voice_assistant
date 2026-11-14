#pragma once
#include <cstddef>
#include <cstdint>
#include "driver/gpio.h"
constexpr int I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_ROLE_MASTER = 0;
constexpr int I2S_DATA_BIT_WIDTH_32BIT = 32, I2S_SLOT_MODE_MONO = 1, I2S_GPIO_UNUSED = -1;
constexpr int I2S_STD_SLOT_LEFT = 1, I2S_STD_SLOT_RIGHT = 2;
struct MockChannel;
using i2s_chan_handle_t = MockChannel *;
struct i2s_event_data_t { void *dma_buf = nullptr; size_t size = 0; };
using i2s_isr_callback_t = bool (*)(i2s_chan_handle_t, i2s_event_data_t *, void *);
struct i2s_event_callbacks_t {
    i2s_isr_callback_t on_recv = nullptr, on_recv_q_ovf = nullptr;
};
struct i2s_chan_config_t {
    int port, role;
    size_t dma_frame_num = 0;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) i2s_chan_config_t{port, role}
struct i2s_slot_config_t { int bits = 0, mode = 0, slot_mask = 0; };
struct i2s_std_config_t {
    int clk_cfg = 0;
    i2s_slot_config_t slot_cfg{};
    struct {
        int mclk = -1, bclk = -1, ws = -1, dout = -1, din = -1;
        struct { bool mclk_inv = false, bclk_inv = false, ws_inv = false; } invert_flags;
    } gpio_cfg;
};
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) i2s_slot_config_t{bits, mode, 0}
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t, const i2s_event_callbacks_t *, void *);
