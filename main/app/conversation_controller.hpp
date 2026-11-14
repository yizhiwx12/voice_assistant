#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "audio/audio_service.hpp"
#include "audio/automatic_utterance.hpp"
#include "audio/barge_in_observer.hpp"
#include "network/voice_client.hpp"
#include "network/wifi_station.hpp"
#include "voice_assistant_status.hpp"

struct cJSON;

// 支持手动原始音频基线，以及显式启用的 AFE 半双工端点检测。
// 云端协议、凭证和最终话语提交都留在服务端处理。
// 对话状态机：把按键、VAD（是否有人说话）和服务端事件串成一轮对话。
// 控制任务管理开始/取消/恢复，上行任务编码发送；回调只提交有界工作，避免堵住采音。
// epoch/turn/generation 标识连接、轮次与回答，取消后旧音频/字幕不能污染下一轮。
class ConversationController {
public:
    esp_err_t start(VoiceAssistantStatus &status, WifiStation &wifi);
    void command(char command);
    // 实体按键使用有明确含义的操作，不依赖串口命令。
    void localAction();
    void retryConnection();

private:
    enum class CommandKind : uint8_t { Start, End, Cancel, Reconnect, Played, Auto, Manual, Observe, BargeIn, LocalAction };
    enum class InteractionMode : uint8_t { Manual, Automatic, Observe, BargeIn };
    struct Command { CommandKind kind; voice::Identity identity{}; };
    enum class InputKind : uint8_t { Start, Audio, End };
    struct Input {
        InputKind kind;
        uint32_t epoch;
        uint32_t turn;
        std::array<int16_t, 320> pcm{};
    };
    static void controlEntry(void *argument);
    static void uplinkEntry(void *argument);
    void controlLoop();
    void uplinkLoop();
    void startSpeech(bool automatic = false);
    void endSpeech();
    void interrupt();
    void resetConnection();
    void restoreInteractionMode();
    void scheduleRecovery();
    void fail(const char *stage, const char *reason, esp_err_t result = ESP_FAIL);
    void observeUplinkQueue();
    void resetUplinkMetrics(uint32_t epoch, uint32_t turn);
    bool recordUplinkMetrics(const Input &input, uint32_t packets, uint64_t send_us,
                             uint32_t send_max_us, esp_err_t process_result,
                             esp_err_t send_result, uint32_t encode_us, uint32_t frame_us);
    void logUplinkMetrics(const char *trigger);
    void logSessionSelection();
    void updateFrontendStatus();
    void logFrontendMetrics();
    void onNetwork(VoiceClient::Event event, const uint8_t *data, size_t bytes);
    void onControl(const uint8_t *data, size_t bytes);
    void clearSubtitleTimeline();
    void disableSubtitleTimeline(const char *reason);
    void onSubtitle(cJSON *event, const voice::Identity &identity, bool escaped_nul);
    void updateSubtitleProgress();
    void publishSubtitleProgress(const AudioService::PlaybackProgress &progress);
    bool prepareSubtitleText(const voice::Identity &identity);
    void appendResponseText(const char *text);
    void commitResponseText();
    void restoreResponseText();
    void onBinary(const uint8_t *data, size_t bytes);
    void onCapture(const int16_t *samples, size_t count);
    void onFrontend(const int16_t *samples, size_t count, uint64_t offset, bool speech);
    void updateAutomatic();
    void setAutomatic(bool enabled);
    void setBargeInObservation(bool enabled);
    void updateBargeInObservation();
    void logBargeInObservation(bool replay_events = false);
    void setBargeIn(bool enabled);
    void beginBargeInHandoff();
    void updateBargeInHandoff();
    void cancelSpeech();
    void finishCaptureLocked();
    void onPlayed(const voice::Identity &identity);
    bool sendControl(const char *type, const voice::Identity &identity);
    bool accepts(const voice::Identity &identity);

    VoiceAssistantStatus *status_ = nullptr;
    WifiStation *wifi_ = nullptr;
    AudioService audio_;
    VoiceClient client_;
    QueueHandle_t commands_ = nullptr;
    QueueHandle_t uplink_ = nullptr;
    std::mutex state_mutex_;
    std::mutex capture_mutex_;
    std::mutex uplink_operation_;
    std::atomic<bool> accepting_capture_{false};
    std::atomic<bool> speech_start_queued_{false};
    std::atomic<bool> automatic_enabled_{false};
    std::atomic<bool> automatic_turn_{false};
    std::atomic<bool> cancel_pending_{false};
    voiceaudio::AutomaticUtterance automatic_;
    voiceaudio::AutomaticUtterance::Frame *automatic_storage_ = nullptr;
    std::atomic<bool> automatic_buffer_failed_{false};
    uint64_t raw_samples_ = 0;  // 连续麦克风采样时钟，由 capture_mutex_ 保护。
    uint64_t automatic_resume_sample_ = 0;
    int64_t automatic_trigger_us_ = 0;
    int64_t automatic_next_send_us_ = 0;
    int64_t automatic_tail_since_ = 0;
    // D1 只做观察。所有观察状态和 PCM 都由 capture_mutex_ 保护；
    // AFE 回调不创建云端回合，也不停止播放。
    voiceaudio::BargeInObserver barge_in_;
    voiceaudio::BargeInObserver::Frame *barge_in_storage_ = nullptr;
    bool barge_in_enabled_ = false;
    voice::Identity barge_in_reply_{};
    // 修改生命周期的任务必须先在 state_mutex_ 下递增版本，再使回答或播放范围失效。
    // AFE 只读取这个 32 位屏障；
    // 普通数据包和文本处理既不会使它失效，也不会阻塞回调。
    std::atomic<uint32_t> reply_revision_{0};
    uint32_t barge_in_reply_revision_ = 0;  // 由 capture_mutex_ 保护。
    uint32_t barge_in_boundary_resets_ = 0;
    int64_t barge_in_last_frame_us_ = 0;
    int64_t barge_in_last_log_us_ = 0;
    bool barge_in_log_pending_ = false;
    const char *barge_in_reason_ = "off";  // 固定诊断值，由 capture_mutex_ 保护。
    struct BargeInEnergySnapshot {
        bool valid = false;
        int64_t at_us = 0;
        uint64_t output_sample = 0;
        uint32_t speech_frames = 0;
        voice::Identity reply{};
        bool aec_active = false;
        uint32_t microphone_rms = 0, microphone_peak = 0;
        int32_t microphone_mean = 0;
        uint32_t microphone_ac_rms = 0;
        uint32_t reference_rms = 0, reference_peak = 0;
        int32_t reference_mean = 0;
        uint32_t reference_ac_rms = 0;
        uint32_t output_rms = 0, output_peak = 0;
        uint64_t frame_sample = 0;
        int32_t frame_mean = 0;
        uint32_t frame_rms = 0, frame_ac_rms = 0, frame_peak = 0;
        uint32_t window_ac_rms = 0, window_frames = 0, min_ac_rms = 0;
        uint32_t energy_active_frames = 0, energy_tail_active_frames = 0;
        bool frame_vad = false;
        AudioService::FrontendSourceWindow source_window{};
    };
    BargeInEnergySnapshot barge_in_candidate_energy_{};
    BargeInEnergySnapshot barge_in_confirmed_energy_{};
    BargeInEnergySnapshot barge_in_rejected_energy_{};
    // 当前整个回答的数值证据，也包含 VAD 从未生成候选的时段。
    // 由 capture_mutex_ 保护，不保留 PCM。
    struct BargeInSignalStats {
        voice::Identity reply{};
        uint32_t output_frames = 0, speech_frames = 0;
        uint32_t output_ac_peak = 0, microphone_ac_peak = 0, microphone_peak = 0;
        uint32_t vad_windows = 0, vad_above_floor = 0, last_vad_result = 0;
        int32_t vad_energy_peak_cdbfs = 0;
    };
    BargeInSignalStats barge_in_signal_{};
    // 每种事件一个标记，由 capture_mutex_ 保护；重置回答范围时保留这些标记，
    // 防止自动阶段日志重放旧缓存事件。现有最近快照存储
    // 仍会合并两次报告之间的事件，
    // 因此这是有界诊断，不是完整事件审计。
    struct BargeInEnergyLogMarker {
        bool valid = false;
        int64_t at_us = 0;
        voice::Identity reply{};
    };
    BargeInEnergyLogMarker barge_in_candidate_logged_{};
    BargeInEnergyLogMarker barge_in_confirmed_logged_{};
    BargeInEnergyLogMarker barge_in_rejected_logged_{};
    // D2 单独启用。AFE 只缓存；控制任务负责封闭旧回合、本地停播，
    // 以及有限时间内从取消 ACK 交接到新 ASR 的流程。
    std::atomic<bool> barge_in_interrupt_enabled_{false};
    std::atomic<bool> barge_in_handoff_{false};
    std::atomic<bool> response_cancel_pending_{false};
    voice::Identity response_cancel_target_{};  // 由 state_mutex_ 保护。
    int64_t barge_in_confirmed_us_ = 0;  // 由 capture_mutex_ 保护。
    int64_t response_cancel_since_us_ = 0;  // 仅由控制任务操作。
    std::atomic<uint32_t> barge_in_input_turn_{0};
    int64_t barge_in_first_uplink_us_ = 0;  // 由 capture_mutex_ 保护，不依赖 64 位原子辅助函数。
    bool barge_in_first_uplink_logged_ = false;  // 仅由控制任务操作。
    uint32_t barge_in_interruptions_ = 0;  // 由 capture_mutex_ 保护。
    std::atomic<bool> failed_{false};
    // failed_ 只唤醒一次清理；此锁存值在清理后仍保留，直到下一次显式连接尝试。
    // 因此，迟到的错误不会替换界面上的首个故障。
    std::atomic<bool> failure_latched_{false};
    // 首个故障分类也由 failure_mutex_ 保护；恢复流程归控制任务负责。
    bool failure_retryable_ = true;
    InteractionMode requested_mode_ = InteractionMode::Manual;
    bool restore_mode_pending_ = false;
    uint8_t reconnect_retries_ = 0;
    int64_t next_connect_us_ = 0;
    int64_t ready_since_us_ = 0;
    std::atomic<bool> resetting_{false};
    // 服务端通知的空闲关闭属于预期清理，不算回合失败。
    std::atomic<bool> idle_closing_{false};
    std::atomic<bool> idle_close_pending_{false};
    bool idle_closed_ = false;  // 由控制任务独占。
    bool start_after_ready_ = false;
    std::mutex failure_mutex_;
    const char *first_failure_stage_ = nullptr;  // 只保存固定的本地界面文字。
    uint32_t secondary_failures_ = 0;
    std::atomic<uint32_t> connection_attempt_{0};
    struct UplinkMetrics {
        uint32_t epoch = 0, turn = 0, frames = 0, packets = 0;
        uint64_t encode_us = 0, send_us = 0, frame_us = 0;
        uint32_t encode_max_us = 0, send_max_us = 0, frame_max_us = 0;
        uint32_t stack_free_min = 0;
        esp_err_t process_result = ESP_OK, send_result = ESP_OK;
    };
    std::mutex metrics_mutex_;
    UplinkMetrics uplink_metrics_{};
    std::atomic<uint32_t> uplink_queue_peak_{0};
    std::atomic<uint8_t> metrics_pending_{0};  // 1 表示定期统计，2 表示流结束统计。
    std::atomic<uint32_t> epoch_{0};
    std::atomic<uint32_t> turn_{0};
    uint32_t sealed_turn_ = 0;
    voice::Identity reply_{};
    bool server_done_ = false;
    bool playback_done_ = false;
    bool audio_failed_ = false;
    uint32_t captured_frames_ = 0;  // 由 capture_mutex_ 保护。
    int64_t speech_wait_since_ = 0;
    int64_t connect_since_ = 0;
    std::string uri_;
    bool opus_ = true;
    // 可选的显示专用 PSRAM：提交前缓存一份完整 LLM 回答。
    // 后半部分用于校验旧版只追加的 TTS 字幕文本。
    struct SubtitleTextDeleter { void operator()(char *text) const; };
    std::unique_ptr<char, SubtitleTextDeleter> subtitle_text_;
    voice::Identity subtitle_text_identity_{};
    size_t response_text_bytes_ = 0;
    bool response_text_truncated_ = false;
    bool subtitle_text_visible_ = false;
    bool response_text_replace_pending_ = false;
    bool response_text_buffering_ = false;
    bool response_text_committed_ = false;
    struct SubtitleWord {
        uint32_t start_sample = 0;
        uint32_t end_sample = 0;
        uint32_t text_byte_offset = 0;
        uint32_t text_end_byte_offset = 0;
    };
    static constexpr size_t kSubtitleWordCapacity = 512;
    struct SubtitleTimeline {
        voice::Identity identity{};
        uint32_t stream_id = 0;
        uint32_t next_sequence = 0;
        uint32_t text_bytes = 0;
        size_t word_count = 0;
        size_t word_head = 0;
        uint32_t last_word_start = 0;
        uint32_t last_word_offset = 0;
        bool have_word = false;
        SubtitleWord current_word{};
        bool have_current_word = false;
        uint32_t published_offset = UINT32_MAX;
        bool published_active = false;
        bool enabled = false;
        bool unavailable = false;
        bool fixed_response_text = false;
        std::array<SubtitleWord, kSubtitleWordCapacity> words{};
    } subtitle_timeline_{};
};
