#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ics43434_microphone.hpp"
#include "local_audio_frontend.hpp"
#include "max98357a_audio.hpp"
#include "network/voice_protocol.hpp"
#include "opus_codec.hpp"
#include "reference_timeline.hpp"

namespace voiceaudio { class ReferenceMonitor; }

// 音频总管：采集任务读取麦克风，播放任务消费有界 PCM 队列，前端独立做 AFE/VAD。
// 原始手动采音与处理后的自动监听各有时间线，失败时不悄悄切换数据源。
// 播放完成以 DMA 真正送出的样本为准，不能把网络收完音频当成扬声器播完。
class AudioService {
public:
    AudioService() = default;
    ~AudioService();
    AudioService(const AudioService &) = delete;
    AudioService &operator=(const AudioService &) = delete;
    using CaptureCallback = std::function<void(const int16_t *, size_t)>;
    using PlaybackDoneCallback = std::function<void(const voice::Identity &)>;
    using ErrorCallback = std::function<void(esp_err_t)>;
    struct Metrics {
        uint32_t rms;
        uint32_t peak;
        uint32_t clipped_samples;
        uint32_t playback_queue_frames;
    };

    // 回调不得阻塞采集。播放完成表示音频 DMA 已排空；
    // 被取消的流不会收到完成回调。
    esp_err_t start(CaptureCallback capture, PlaybackDoneCallback playback_done,
                     ErrorCallback error = {}, uint8_t gain = 1, uint8_t volume = 40);
    // 串行化生命周期操作，须在网络生产者停止后执行。
    // 先退出工作任务，再释放麦克风/AFE/模型资源。
    esp_err_t stop();
    using FrontendMetrics = LocalAudioFrontend::Metrics;
    FrontendMetrics frontendMetrics() const { return frontend_.metrics(); }
    using FrontendSourceWindow = LocalAudioFrontend::SourceWindow;
    FrontendSourceWindow frontendSourceWindow(uint64_t first, uint64_t end) const {
        return frontend_.sourceWindow(first, end);
    }
    using FrontendOutputCallback = LocalAudioFrontend::OutputCallback;
    // 须在 start() 前注册；回调运行在 AFE fetch 工作任务。
    // 原始 CaptureCallback 保持独立，不作为自动模式的后备路径。
    esp_err_t setFrontendOutputCallback(FrontendOutputCallback callback)
    {
        if (playback_queue_) return ESP_ERR_INVALID_STATE;
        return frontend_.setOutputCallback(std::move(callback));
    }
    esp_err_t beginPlayback(const voice::Identity &identity, uint32_t stream_id,
                             bool opus, uint32_t pre_skip);
    esp_err_t feedPlayback(const voice::Identity &identity, uint32_t stream_id,
                            uint32_t sequence, uint64_t sample_offset,
                            const uint8_t *payload, size_t bytes);
    esp_err_t endPlayback(const voice::Identity &identity, uint32_t stream_id,
                           uint64_t valid_end_sample);
    // 同步屏障：中断队列背压并丢弃已排队 PCM。
    // 默认路径静音并重建 TX；AEC 健康时有界写零排空、静音并保持 TX 连续，
    // 已经提交的音频可能保留不超过 50 ms 的 DMA 尾音。
    esp_err_t stopPlayback();
    bool isPlaying() const { return playing_.load(std::memory_order_acquire); }
    struct PlaybackProgress {
        voice::Identity identity{};
        uint32_t stream_id = 0;
        uint64_t completed_samples = 0;  // 24 kHz 的实际内容样本数，不含 pre-skip、排空及欠载补零。
        bool available = false;
        bool coherent = false;
        bool playing = false;
    };
    PlaybackProgress playbackProgress() const;
    Metrics metrics() const;
    void logPlaybackReference();

private:
    struct PlaybackFrame {
        uint32_t serial;
        voice::Identity identity;
        uint16_t samples;
        bool end;
        std::array<int16_t, 480> pcm;
    };
    struct ReferenceCaptureFrame {
        uint64_t first_sample;
        uint32_t playback_serial;
        std::array<int16_t, 320> pcm;
    };
    struct AecCaptureFrame {
        uint64_t first_sample = 0;
        // 序号为零表示刚重置、尚未绑定的 TX 代次：
        // SD 为低且尚未提交 PCM；其他缺口使用时间线描述。
        bool known_mute = false;
        std::array<int16_t, 320> pcm{};
    };
    struct AecScratch {
        AecCaptureFrame frame;
        std::array<int16_t, 320> reference{};
        std::array<uint8_t, 320> valid_mask{};
    };
    static void captureTaskEntry(void *argument);
    static void playbackTaskEntry(void *argument);
    void captureTask();
    void playbackTask();
    esp_err_t enqueuePending(uint64_t end, uint32_t serial);
    esp_err_t enqueue(const PlaybackFrame &frame);
    // 仅在同时持有 stream_mutex_ 和 speaker_mutex_ 时重置。
    // 生产与播放计数器仍由各自原有任务持有；读取方拒绝作用域变化，
    // 不为热路径额外引入诊断互斥锁。
    void resetPlaybackWork(const voice::Identity &identity, uint32_t stream_id, uint32_t serial);
    void logPlaybackWork() const;
    // 调用方持有 speaker_mutex_；全部处理均留在播放侧。
    void pollReferenceMonitor();
    void publishReferenceClockFailure();  // 仅由串行化的参考持有者调用。
    // 对已到处理时机但仍不完整的目标，先给予有界转换机会，
    // 再让同一工作任务开始下一次阻塞 TX 写入。
    void serviceAecReferenceBeforePlayback();
    void pollAecReference(uint64_t microphone_sample_end);
    void failAecReference(esp_err_t error, const char *reason);
    struct AecServiceSnapshot {
        std::array<uint32_t, 26> words{};
        bool coherent = false;
        bool pending = false;
        uint32_t queue = 0, last_pair_us = 0, service_gap_us = 0, service_gap_max_us = 0;
    };
    AecServiceSnapshot aecServiceSnapshot() const;
    static void logAecServiceDiagnostics(const AecServiceSnapshot &snapshot);
    void logPendingAecFailure();
    void publishAecPairAttempt(uint64_t first_sample, uint64_t microphone_sample_end,
                              bool has_head, bool queried, uint32_t valid_samples);
    // 持有 speaker_mutex_；每次静音写入之间执行配对服务。
    esp_err_t drainPlayback();

    Ics43434Microphone microphone_;
    LocalAudioFrontend frontend_;
    Max98357aAudio speaker_;
    Max98357aAudio::Config speaker_config_{};
    CaptureCallback capture_;
    PlaybackDoneCallback playback_done_;
    ErrorCallback error_;
    QueueHandle_t playback_queue_ = nullptr;
    QueueHandle_t reference_capture_queue_ = nullptr;  // 有界 PSRAM 队列，仅用于诊断。
    ReferenceCaptureFrame *reference_capture_scratch_ = nullptr;  // 单采集生产者使用，位于 PSRAM。
    QueueHandle_t aec_capture_queue_ = nullptr;  // 200 ms，采集侧交给播放持有者。
    AecScratch *aec_scratch_ = nullptr;  // 仅播放持有者使用，位于 PSRAM。
    AecCaptureFrame *aec_capture_scratch_ = nullptr;  // 仅采集持有者使用，位于 PSRAM。
    std::atomic<uint32_t> aec_error_{ESP_OK};
    std::atomic<uint32_t> aec_capture_peak_{0};
    std::atomic<uint32_t> aec_pairs_{0};
    std::atomic<uint32_t> aec_initial_silence_{0};
    std::atomic<uint32_t> aec_frontend_waits_{0};
    std::atomic<uint32_t> aec_last_valid_samples_{0};
    std::atomic<uint32_t> aec_pair_max_us_{0};
    // 播放持有者发布有界快照。采集侧也可能报告失败，
    // 因此绝不能直接检查播放临时区/时间线。
    std::atomic<uint32_t> aec_attempt_sequence_{0};
    std::array<std::atomic<uint32_t>, 26> aec_attempt_words_{};
    std::atomic<bool> aec_target_waiting_{false};
    // 因 AFE 满载而保留的完整配对需要再次获得服务，
    // 不能等到新播放帧到达或 20 ms 超时后才处理。
    std::atomic<bool> aec_frontend_waiting_{false};
    // 这些字段属于 speaker_mutex_ 的持有者。跨任务失败日志
    // 仅读取已发布的原子摘要，绝不读取这些可变值。
    uint64_t aec_last_query_first_ = 0;
    uint32_t aec_last_query_valid_ = 0, aec_last_query_begin_ = 0, aec_last_query_end_ = 0;
    uint32_t aec_last_query_missing_ = 0;
    uint32_t reference_latest_sequence_ = 0;
    uint64_t reference_oldest_sample_ = 0, reference_latest_sample_end_ = 0;
    uint32_t reference_timeline_healthy_ = 0, reference_timeline_unmapped_ = 0;
    std::atomic<uint32_t> aec_catchup_passes_{0};
    std::atomic<uint32_t> aec_catchup_yields_{0};
    std::atomic<uint32_t> aec_last_pair_us_{0};
    uint64_t reference_last_service_us_ = 0;  // 仅 speaker_mutex_ 持有者访问。
    std::atomic<uint32_t> reference_service_gap_us_{0};
    std::atomic<uint32_t> reference_service_gap_max_us_{0};
    // 由串行化参考持有者写入一次，所有工作任务停止前保持不变。
    // 采集可进行有界的 acquire 快照，无须检查该持有者的可变时间线或等待其互斥锁。
    voiceaudio::ReferenceTimeline::FirstClockFailure reference_clock_failure_{};
    std::atomic<bool> reference_clock_failure_ready_{false};
    struct AecFailureSnapshot {
        LocalAudioFrontend::FailureSnapshot frontend;
        AecServiceSnapshot service{};
        Ics43434Microphone::DmaMetrics microphone{};
        uint64_t at_us = 0;
        uint32_t frontend_waits = 0;
        voiceaudio::ReferenceTimeline::FirstClockFailure clock{};
    } aec_failure_snapshot_;
    // 首次失败记录在工作任务停止前保持不变。只有赢得 release/acquire
    // 交换的调用方才在采集路径和锁外打印详情。
    std::atomic<bool> aec_failure_log_pending_{false};
    SemaphoreHandle_t stream_mutex_ = nullptr;
    SemaphoreHandle_t speaker_mutex_ = nullptr;
    SemaphoreHandle_t capture_done_ = nullptr;
    SemaphoreHandle_t capture_started_signal_ = nullptr;
    SemaphoreHandle_t playback_done_signal_ = nullptr;
    TaskHandle_t capture_task_ = nullptr;
    TaskHandle_t playback_task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<esp_err_t> capture_start_error_{ESP_ERR_INVALID_STATE};
    std::atomic<uint32_t> serial_{1};
    std::atomic<bool> active_{false};
    std::atomic<bool> playing_{false};
    voice::Identity identity_{};
    uint32_t stream_id_ = 0;
    uint32_t sequence_ = 0;
    uint64_t offset_ = 0;
    uint64_t emitted_until_ = 0;
    uint64_t pending_offset_ = 0;
    uint32_t pre_skip_ = 0;
    bool opus_ = false;
    bool has_pending_ = false;
    bool ended_ = false;
    bool playback_reference_drained_ = true;  // 由播放持有者访问，受 speaker_mutex_ 保护。
    struct PlaybackWork {
        std::atomic<uint32_t> sequence{0}, serial{0}, epoch{0}, turn{0}, generation{0}, stream{0};
        std::atomic<uint32_t> decoded_packets{0}, decoded_samples{0}, decode_last_us{0}, decode_max_us{0};
        std::atomic<uint32_t> enqueue_waits{0}, enqueue_wait_total_us{0}, enqueue_wait_max_us{0}, queue_peak{0};
        std::atomic<uint32_t> empty_waits{0}, empty_wait_total_us{0}, empty_wait_max_us{0};
        std::atomic<uint32_t> write_calls{0}, write_samples{0}, write_max_us{0}, write_gap_max_us{0};
        std::atomic<uint32_t> budget_waits{0};
        // 当前序号的生产者总量。首次写入前恰好等于可用 PCM，
        // 其中包括消费者已经持有的帧。
        std::atomic<uint32_t> enqueued_samples{0};
        std::atomic<uint32_t> startup_samples{0}, startup_wait_us{0};
        std::atomic<bool> startup_ready{false}, startup_deadline{false}, startup_end_received{false};
        std::atomic<bool> end_received{false};
    } playback_work_;
    uint64_t playback_last_write_end_us_ = 0;  // 仅 speaker_mutex_ 持有者访问，不含排空。
    uint8_t gain_ = 1;
    voiceaudio::OpusDecoder decoder_;
    std::array<int16_t, 480> pending_pcm_{};
    std::atomic<uint32_t> rms_{0};
    std::atomic<uint32_t> peak_{0};
    std::atomic<uint32_t> clipped_{0};
    voiceaudio::ReferenceMonitor *reference_monitor_ = nullptr;  // 位于 PSRAM，仅供任务访问。
    uint32_t reference_history_cursor_ = 0;
    uint32_t reference_poll_max_us_ = 0;
    uint32_t reference_pump_max_us_ = 0;
    uint32_t reference_convert_max_us_ = 0;
    uint32_t reference_query_max_us_ = 0;
    uint32_t reference_acoustic_max_us_ = 0;
    uint32_t reference_budget_yields_ = 0;
    uint32_t reference_capture_stale_ = 0;
    uint32_t reference_max_batch_ = 0;
    std::atomic<bool> reference_history_pending_{false};
    std::atomic<uint32_t> reference_capture_dropped_{0};
    std::atomic<uint32_t> capture_stack_free_min_{0};
    std::atomic<uint32_t> playback_stack_free_min_{0};
};
