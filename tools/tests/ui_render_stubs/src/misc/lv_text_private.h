#pragma once

#include "lvgl.h"

inline constexpr int LV_TEXT_FLAG_NONE = 0;

// 匹配主机桩的 8 px 字符单元，而非真实字体几何。
// 独立的真实 LVGL 检查验证生产换行及字形位置。
inline uint32_t lv_text_get_next_line(const char *text, uint32_t length,
                                     const lv_font_t *, int32_t, int32_t width,
                                     int32_t *, int)
{
    const uint32_t columns = static_cast<uint32_t>(std::max(1, width / 8));
    uint32_t offset = 0, characters = 0;
    while (offset < length && text[offset]) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        const uint32_t bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        offset += std::min(bytes, length - offset);
        if (lead == '\n' || lead == '\r') break;
        if (++characters >= columns) {
            if (offset < length && (text[offset] == '\n' || text[offset] == '\r')) ++offset;
            break;
        }
    }
    return offset;
}
