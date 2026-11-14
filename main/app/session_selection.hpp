#pragma once

#include <array>
#include <cstdint>

// 这里只保留公开的 session.ready 元数据，不保留接口地址、凭证、
// 任意服务商选项或原始 JSON。空文本表示未知。
enum class VoiceSelectionFlag : uint8_t { Unknown, No, Yes };

struct VoiceSelectionCapabilities {
    VoiceSelectionFlag streaming_input = VoiceSelectionFlag::Unknown;
    VoiceSelectionFlag streaming_output = VoiceSelectionFlag::Unknown;
    VoiceSelectionFlag partial_results = VoiceSelectionFlag::Unknown;
};

struct VoiceProviderSelection {
    std::array<char, 81> id{};
    std::array<char, 33> adapter{};
    std::array<char, 97> model{};
    std::array<char, 97> voice{};
    std::array<char, 97> resource_id{};
    VoiceSelectionCapabilities capabilities{};
};

struct VoiceSessionSelection {
    // 只有已接受的当前 session.ready 才有非零值。缺少元数据时
    // 仍保留 epoch，但不代表已知服务商或能力。
    uint32_t epoch = 0;
    bool metadata_present = false;
    bool rejected_fields = false;
    std::array<char, 81> profile{};
    std::array<char, 17> mode{};
    VoiceSelectionFlag require_streaming_asr = VoiceSelectionFlag::Unknown;
    VoiceProviderSelection asr{}, llm{}, tts{};
};

static_assert(sizeof(VoiceSessionSelection) <= 1408, "Keep session metadata bounded");
