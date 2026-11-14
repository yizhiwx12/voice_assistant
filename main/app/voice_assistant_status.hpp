#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "session_selection.hpp"

enum class VoiceAssistantState : uint8_t {
    Ready,
    Preparing,
    Listening,
    Recording,
    Transcribing,
    Uploading,
    Speaking,
    Complete,
    UploadError,
    Error,
};

enum class VoiceSessionState : uint8_t { Offline, Connecting, Ready, Failed };
enum class VoiceCaptureState : uint8_t { Idle, Starting, Recording, Finishing };
enum class VoiceResponseState : uint8_t { Idle, Waiting, Generating, Complete, Cancelled, Failed };
enum class VoicePlaybackState : uint8_t { Idle, Buffering, Playing, Draining };
enum class VoiceFrontendState : uint8_t { Disabled, Starting, Silence, Speech, Failed };
// D1 观察独立于自动监听。D2 插话另行启用；
// 只有控制器能停止播放或创建回合。
enum class VoiceBargeInState : uint8_t {
    Disabled, Armed, Candidate, Confirmed, Cooldown, Unavailable, Cancelling, Starting
};

struct VoiceSubtitlePosition {
    bool active = false;
    uint32_t text_byte_offset = 0;
    // 当前有效的时间范围。旧 text_byte_offset 仍指向当前词的起点；
    // 通过范围信息，渲染器可以选择词内字形，
    // 无需再复制一份字幕时间线。
    uint32_t word_end_byte_offset = 0;
    uint32_t word_start_sample = 0;
    uint32_t word_end_sample = 0;
    uint32_t playback_sample = 0;
    uint16_t word_progress_permille = 0;
    // 已知的下一词锚点，附带从当前词起点到下一词起点的 DMA 进度。
    // 没有下一条字幕信息时绝不能外推。
    uint32_t next_word_byte_offset = UINT32_MAX;
    uint16_t next_word_progress_permille = 0;
    uint32_t revision = 0;
    uint32_t epoch = 0;
    uint32_t turn = 0;
    uint32_t generation = 0;
    uint32_t stream_id = 0;
};

// 跨任务的界面状态快照：控制器写入，UI 读取，不让 LVGL 直接访问网络/音频内部状态。
// 文本容量按 UTF-8 字节计；截断与字幕偏移必须保持完整字符边界。
class VoiceAssistantStatus {
public:
    // 保留有容量上限的用户话语和回答，不依赖液晶屏当前可见页。
    static constexpr size_t kConversationCapacity = 8192;
    VoiceAssistantState state() const
    {
        if (realtime_.load()) {
            if (session() == VoiceSessionState::Failed || response() == VoiceResponseState::Failed)
                return VoiceAssistantState::UploadError;
            if (session() != VoiceSessionState::Ready) return VoiceAssistantState::Preparing;
            if (capture() == VoiceCaptureState::Starting) return VoiceAssistantState::Preparing;
            if (capture() == VoiceCaptureState::Recording) return VoiceAssistantState::Recording;
            if (capture() == VoiceCaptureState::Finishing) return VoiceAssistantState::Transcribing;
            if (playback() != VoicePlaybackState::Idle) return VoiceAssistantState::Speaking;
            if (response() == VoiceResponseState::Waiting || response() == VoiceResponseState::Generating)
                return VoiceAssistantState::Uploading;
            if (response() == VoiceResponseState::Complete) return VoiceAssistantState::Complete;
            return listening() ? VoiceAssistantState::Listening : VoiceAssistantState::Ready;
        }
        return static_cast<VoiceAssistantState>(state_.load(std::memory_order_acquire));
    }

    void enableRealtime() { realtime_.store(true); }
    bool realtime() const { return realtime_.load(); }
    VoiceSessionState session() const { return session_.load(); }
    VoiceCaptureState capture() const { return capture_.load(); }
    VoiceResponseState response() const { return response_.load(); }
    VoicePlaybackState playback() const { return playback_.load(); }
    VoiceFrontendState frontend() const { return frontend_.load(); }
    VoiceBargeInState bargeIn() const { return barge_in_.load(); }
    bool bargeInEnabled() const { return barge_in_enabled_.load(); }
    // 监听必须显式启用。控制器负责 VAD 回合边界；
    // 仅做观察不代表已启用监听，也不代表采集或播放已经开始。
    bool listening() const { return listening_.load(); }
    void setListening(bool value)
    {
        if (listening_.exchange(value) != value) text_revision_.fetch_add(1);
    }
    void setFrontend(VoiceFrontendState value)
    {
        if (frontend_.exchange(value) != value) text_revision_.fetch_add(1);
    }
    void setBargeIn(VoiceBargeInState value)
    {
        if (barge_in_.exchange(value) != value) text_revision_.fetch_add(1);
    }
    void setBargeInEnabled(bool value)
    {
        if (barge_in_enabled_.exchange(value) != value) text_revision_.fetch_add(1);
    }
    void setSession(VoiceSessionState value)
    {
        if (session_.exchange(value) != value) {
            if (value != VoiceSessionState::Ready) clearSelection();
            text_revision_.fetch_add(1);
        }
    }
    void setCapture(VoiceCaptureState value)
    {
        if (capture_.exchange(value) != value) text_revision_.fetch_add(1);
    }
    void setResponse(VoiceResponseState value)
    {
        if (response_.exchange(value) != value) text_revision_.fetch_add(1);
    }
    void setPlayback(VoicePlaybackState value)
    {
        // 每个下行音频包都会带来一次 Playing 状态。
        // 这不属于正文变化，不能因此复制整份回答。
        if (playback_.exchange(value) != value) text_revision_.fetch_add(1);
    }

    void setState(VoiceAssistantState state)
    {
        state_.store(static_cast<uint8_t>(state), std::memory_order_release);
    }

    uint32_t samples() const { return samples_.load(std::memory_order_relaxed); }
    void setSamples(uint32_t samples) { samples_.store(samples, std::memory_order_relaxed); }
    int httpStatus() const { return http_status_.load(std::memory_order_relaxed); }
    void setHttpStatus(int status) { http_status_.store(status, std::memory_order_relaxed); }
    TickType_t resultDisplayUntil() const
    {
        return result_display_until_.load(std::memory_order_acquire);
    }
    void setResultDisplayUntil(TickType_t tick)
    {
        result_display_until_.store(tick, std::memory_order_release);
    }

    void setConversation(const char *text, bool truncated = false)
    {
        portENTER_CRITICAL(&text_mux_);
        size_t copied = 0;
        conversation_truncated_.store(
            copyBounded(conversation_.data(), conversation_.size(), text, copied) || truncated);
        conversation_length_ = trimUtf8(conversation_.data(), copied);
        conversation_id_.fetch_add(1, std::memory_order_release);
        conversation_revision_.fetch_add(1, std::memory_order_release);
        text_revision_.fetch_add(1, std::memory_order_release);
        portEXIT_CRITICAL(&text_mux_);
    }

    void copyConversation(char *destination, size_t capacity) const
    {
        if (destination == nullptr || capacity == 0) {
            return;
        }
        portENTER_CRITICAL(&text_mux_);
        const size_t copied = conversation_length_ < capacity ? conversation_length_ : capacity - 1;
        std::memcpy(destination, conversation_.data(), copied);
        destination[copied] = '\0';
        // 完整存储已经落在 UTF-8 字符边界；
        // 只有调用者缓冲较小时，末尾才可能再次截在字符内部。
        if (copied < conversation_length_) trimUtf8(destination, copied);
        portEXIT_CRITICAL(&text_mux_);
    }

    uint32_t conversationId() const { return conversation_id_.load(std::memory_order_acquire); }
    // 独立于连接、VAD 和模式变化；
    // 渲染器刷新其他状态时，可以保留已有回答副本。
    uint32_t conversationRevision() const { return conversation_revision_.load(std::memory_order_acquire); }
    bool conversationTruncated() const { return conversation_truncated_.load(); }

    void setSelection(const VoiceSessionSelection &value)
    {
        portENTER_CRITICAL(&selection_mux_);
        selection_ = value;
        portEXIT_CRITICAL(&selection_mux_);
        text_revision_.fetch_add(1, std::memory_order_release);
    }

    VoiceSessionSelection selection() const
    {
        VoiceSessionSelection result;
        portENTER_CRITICAL(&selection_mux_);
        result = selection_;
        portEXIT_CRITICAL(&selection_mux_);
        return result;
    }

    void clearSelection()
    {
        // 小栈音频故障路径也会调用此处。
        // 将空聚合对象放在静态存储区，避免额外创建约 1.3 KiB 的临时对象。
        static constexpr VoiceSessionSelection empty{};
        setSelection(empty);
    }

    void setPipelineStage(const char *text)
    {
        portENTER_CRITICAL(&text_mux_);
        std::snprintf(pipeline_stage_.data(), pipeline_stage_.size(), "%s",
                      text == nullptr ? "" : text);
        portEXIT_CRITICAL(&text_mux_);
        text_revision_.fetch_add(1, std::memory_order_release);
    }

    void copyPipelineStage(char *destination, size_t capacity) const
    {
        if (destination == nullptr || capacity == 0) {
            return;
        }
        portENTER_CRITICAL(&text_mux_);
        std::snprintf(destination, capacity, "%s", pipeline_stage_.data());
        portEXIT_CRITICAL(&text_mux_);
    }

    uint32_t textRevision() const
    {
        return text_revision_.load(std::memory_order_acquire);
    }

    void setAudioMetrics(uint32_t rms, uint32_t peak, uint32_t clipped_samples)
    {
        rms_.store(rms, std::memory_order_relaxed);
        peak_.store(peak, std::memory_order_relaxed);
        clipped_samples_.store(clipped_samples, std::memory_order_relaxed);
    }

    void appendConversation(const char *text)
    {
        if (!text || !text[0]) return;
        portENTER_CRITICAL(&text_mux_);
        if (conversation_truncated_.load()) {
            portEXIT_CRITICAL(&text_mux_);
            return;
        }
        const size_t used = conversation_length_;
        size_t copied = 0;
        const bool truncated = copyBounded(conversation_.data() + used,
                                          conversation_.size() - used, text, copied);
        conversation_truncated_.store(truncated);
        conversation_length_ = trimUtf8(conversation_.data(), used + copied);
        if (conversation_length_ != used || truncated) {
            conversation_revision_.fetch_add(1, std::memory_order_release);
            text_revision_.fetch_add(1, std::memory_order_release);
        }
        portEXIT_CRITICAL(&text_mux_);
    }

    // 字幕文本是当前回答的另一份规范化显示。
    // 首个非空批次替换回答，后续批次继续追加，
    // 不会创建新的对话页面。
    void resetSubtitle(uint32_t epoch, uint32_t turn, uint32_t generation,
                       uint32_t stream_id)
    {
        portENTER_CRITICAL(&subtitle_mux_);
        subtitle_ = {};
        subtitle_.epoch = epoch;
        subtitle_.turn = turn;
        subtitle_.generation = generation;
        subtitle_.stream_id = stream_id;
        subtitle_text_started_ = false;
        subtitle_revision_.fetch_add(1, std::memory_order_release);
        portEXIT_CRITICAL(&subtitle_mux_);
    }

    void setSubtitleText(uint32_t epoch, uint32_t turn, uint32_t generation,
                         uint32_t stream_id, const char *text)
    {
        if (text == nullptr || text[0] == '\0') return;
        portENTER_CRITICAL(&subtitle_mux_);
        if (subtitle_.epoch != epoch || subtitle_.turn != turn ||
            subtitle_.generation != generation || subtitle_.stream_id != stream_id) {
            portEXIT_CRITICAL(&subtitle_mux_);
            return;
        }
        if (!subtitle_text_started_) {
            setConversation(text);
            subtitle_text_started_ = true;
        } else {
            appendConversation(text);
        }
        subtitle_revision_.fetch_add(1, std::memory_order_release);
        portEXIT_CRITICAL(&subtitle_mux_);
    }

    void setSubtitlePosition(uint32_t epoch, uint32_t turn, uint32_t generation,
                             uint32_t stream_id, uint32_t byte_offset, bool active,
                             uint32_t word_end_byte_offset = 0,
                             uint32_t word_start_sample = 0,
                             uint32_t word_end_sample = 0,
                             uint32_t playback_sample = 0,
                             uint16_t word_progress_permille = 0,
                             uint32_t next_word_byte_offset = UINT32_MAX,
                             uint16_t next_word_progress_permille = 0)
    {
        portENTER_CRITICAL(&subtitle_mux_);
        if (subtitle_.epoch == epoch && subtitle_.turn == turn &&
            subtitle_.generation == generation && subtitle_.stream_id == stream_id) {
            const bool changed = subtitle_.text_byte_offset != byte_offset ||
                subtitle_.active != active ||
                subtitle_.word_end_byte_offset != word_end_byte_offset ||
                subtitle_.word_start_sample != word_start_sample ||
                subtitle_.word_end_sample != word_end_sample ||
                subtitle_.playback_sample != playback_sample ||
                subtitle_.word_progress_permille != word_progress_permille ||
                subtitle_.next_word_byte_offset != next_word_byte_offset ||
                subtitle_.next_word_progress_permille != next_word_progress_permille;
            subtitle_.text_byte_offset = byte_offset;
            subtitle_.active = active;
            subtitle_.word_end_byte_offset = word_end_byte_offset;
            subtitle_.word_start_sample = word_start_sample;
            subtitle_.word_end_sample = word_end_sample;
            subtitle_.playback_sample = playback_sample;
            subtitle_.word_progress_permille = word_progress_permille;
            subtitle_.next_word_byte_offset = next_word_byte_offset;
            subtitle_.next_word_progress_permille = next_word_progress_permille;
            if (changed) subtitle_revision_.fetch_add(1, std::memory_order_release);
        }
        portEXIT_CRITICAL(&subtitle_mux_);
    }

    void disableSubtitle(uint32_t epoch, uint32_t turn, uint32_t generation,
                         uint32_t stream_id)
    {
        portENTER_CRITICAL(&subtitle_mux_);
        if (subtitle_.epoch == epoch && subtitle_.turn == turn &&
            subtitle_.generation == generation && subtitle_.stream_id == stream_id) {
            subtitle_.active = false;
            subtitle_revision_.fetch_add(1, std::memory_order_release);
        }
        portEXIT_CRITICAL(&subtitle_mux_);
    }

    VoiceSubtitlePosition subtitlePosition() const
    {
        VoiceSubtitlePosition result;
        portENTER_CRITICAL(&subtitle_mux_);
        result = subtitle_;
        result.revision = subtitle_revision_.load(std::memory_order_acquire);
        portEXIT_CRITICAL(&subtitle_mux_);
        return result;
    }

private:
    static bool copyBounded(char *destination, size_t capacity, const char *source, size_t &size)
    {
        const char *value = source == nullptr ? "" : source;
        size = 0;
        while (size + 1 < capacity && value[size] != '\0') {
            destination[size] = value[size];
            ++size;
        }
        destination[size] = '\0';
        return value[size] != '\0';
    }

    static size_t trimUtf8(char *text, size_t size)
    {
        if (!size) return 0;
        size_t start = size - 1;
        while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
        const unsigned char lead = static_cast<unsigned char>(text[start]);
        const size_t expected = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (size - start < expected) {
            text[start] = 0;
            return start;
        }
        return size;
    }
    std::atomic<bool> realtime_{false};
    std::atomic<bool> listening_{false};
    std::atomic<bool> barge_in_enabled_{false};
    std::atomic<VoiceSessionState> session_{VoiceSessionState::Offline};
    std::atomic<VoiceCaptureState> capture_{VoiceCaptureState::Idle};
    std::atomic<VoiceResponseState> response_{VoiceResponseState::Idle};
    std::atomic<VoicePlaybackState> playback_{VoicePlaybackState::Idle};
    std::atomic<VoiceFrontendState> frontend_{VoiceFrontendState::Disabled};
    std::atomic<VoiceBargeInState> barge_in_{VoiceBargeInState::Disabled};
    std::atomic<uint8_t> state_{static_cast<uint8_t>(VoiceAssistantState::Ready)};
    std::atomic<uint32_t> samples_{0};
    std::atomic<uint32_t> rms_{0};
    std::atomic<uint32_t> peak_{0};
    std::atomic<uint32_t> clipped_samples_{0};
    std::atomic<int> http_status_{0};
    std::atomic<TickType_t> result_display_until_{0};
    std::atomic<uint32_t> text_revision_{0};
    std::atomic<uint32_t> conversation_id_{0};
    std::atomic<uint32_t> conversation_revision_{0};
    std::atomic<bool> conversation_truncated_{false};
    mutable portMUX_TYPE text_mux_ = portMUX_INITIALIZER_UNLOCKED;
    mutable portMUX_TYPE selection_mux_ = portMUX_INITIALIZER_UNLOCKED;
    mutable portMUX_TYPE subtitle_mux_ = portMUX_INITIALIZER_UNLOCKED;
    VoiceSessionSelection selection_{};
    std::array<char, kConversationCapacity> conversation_{};
    size_t conversation_length_ = 0;  // 与文本一起由 text_mux_ 保护。
    std::array<char, 96> pipeline_stage_{};
    VoiceSubtitlePosition subtitle_{};
    std::atomic<uint32_t> subtitle_revision_{0};
    bool subtitle_text_started_ = false;
};
