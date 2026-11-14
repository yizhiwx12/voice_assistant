#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "app/voice_assistant_status.hpp"

namespace voiceui {

// 复制状态标签时，不在液晶屏长度边界处截断 UTF-8 字符。
inline void copyUtf8Bounded(char *destination, size_t capacity, const char *source)
{
    if (destination == nullptr || capacity == 0) return;
    const char *value = source == nullptr ? "" : source;
    size_t size = 0;
    while (size + 1 < capacity && value[size] != '\0') {
        destination[size] = value[size];
        ++size;
    }
    destination[size] = '\0';
    if (size == 0) return;

    size_t start = size - 1;
    while (start && (static_cast<unsigned char>(destination[start]) & 0xc0) == 0x80) --start;
    const unsigned char lead = static_cast<unsigned char>(destination[start]);
    const size_t expected = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (size - start < expected) destination[start] = '\0';
}

inline void formatModeConnection(VoiceSessionState session, bool listening,
                                 bool barge_in_enabled, VoiceBargeInState barge_in,
                                 char *destination, size_t capacity)
{
    if (destination == nullptr || capacity == 0) return;
    const char *mode = barge_in_enabled ? "自动插话" :
        (barge_in != VoiceBargeInState::Disabled && barge_in != VoiceBargeInState::Unavailable ? "插话观察" :
         (listening ? "自动监听" : "已暂停"));
    const char *connection = "未连接";
    switch (session) {
    case VoiceSessionState::Offline: connection = "未连接"; break;
    case VoiceSessionState::Connecting: connection = "连接中"; break;
    case VoiceSessionState::Ready: connection = "已连接"; break;
    case VoiceSessionState::Failed: connection = "连接失败"; break;
    }
    std::snprintf(destination, capacity, "%s | %s", mode, connection);
    copyUtf8Bounded(destination, capacity, destination);
}

}  // 命名空间 voiceui 结束
