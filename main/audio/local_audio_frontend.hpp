#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "sdkconfig.h"
#include "esp_afe_sr_iface.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcm_block_adapter.hpp"
#include "pcm_dc_blocker.hpp"
#include "source_signal_history.hpp"
#include "sr_model_store.hpp"

// 连续的单麦克风 AFE 输出及 VAD。原始手动采集保持独立，
// 自动对话使用处理后的时间线。
// AFE = 本地音频前端，VAD = 语音活动检测，AEC = 扬声器回声消除。
// 这里只处理音频和“是否说话”，不会识别文字；ASR 模型在电脑端配置。
// feed/fetch 任务通过有界缓冲工作，回调不能做联网请求或长时间等待。
class LocalAudioFrontend {
public:
    LocalAudioFrontend() = default;
    ~LocalAudioFrontend();
    LocalAudioFrontend(const LocalAudioFrontend &) = delete;
    LocalAudioFrontend &operator=(const LocalAudioFrontend &) = delete;
    using OutputCallback = std::function<void(const int16_t *, size_t, uint64_t, bool)>;
    using SourceWindow = voiceaudio::SourceSignalHistory::Window;
    // 在首次错误的 UART 输出前锁存。计数器是独立原子观测，并非调度轨迹；
    // 正在进行的 DSP 可能随后完成并改变实时指标。
    struct FailureSnapshot {
        bool valid = false;
        esp_err_t error = ESP_OK;
        uint64_t at_us = 0;
        uint32_t queue_frames = 0;
        uint32_t queue_peak = 0;
        uint32_t feed_samples = 0;
        uint32_t output_samples = 0;
        uint32_t feed_max_us = 0;
        uint32_t fetch_max_us = 0;
    };
    struct Metrics {
        bool enabled = false;
        bool ready = false;
        bool speech = false;
        bool aec_requested = false;
        bool aec_active = false;
        esp_err_t last_error = ESP_OK;
        uint32_t feed_samples = 0;
        uint32_t output_samples = 0;
        uint32_t output_frames = 0;
        uint32_t queue_frames = 0;
        uint32_t queue_peak = 0;
        uint32_t feed_max_us = 0;
        uint32_t fetch_max_us = 0;
        uint32_t feed_stack_free = 0;
        uint32_t fetch_stack_free = 0;
        uint32_t feed_chunk_samples = 0;
        uint32_t fetch_chunk_samples = 0;
        uint32_t vad_transitions = 0;
        uint32_t feed_total_us = 0;
        uint32_t output_blocks = 0;
        uint32_t output_rms = 0;
        uint32_t output_peak = 0;
        // 实际 SDK 取值，在工作任务启动前读取一次。有符号能量以 0.01 dBFS 表示；
        // INT32_MIN 表示不可用，绝不是 0 dBFS。
        bool vad_thresholds_valid = false;
        int32_t vad_energy_threshold_cdbfs = INT32_MIN;
        int32_t vad_probability_threshold_milli = -1;
        // SDK 在 AGC 前的 VAD 输入能量，为 128 ms 语音窗口的平均值。
        // 这是独立原子观测，不是对齐的输入/输出 PCM。
        // 峰值/计数覆盖本次前端启动以来的全部时段，包括非播放期。
        bool vad_input_energy_valid = false;
        int32_t vad_input_energy_cdbfs = INT32_MIN;
        int32_t vad_input_energy_peak_cdbfs = INT32_MIN;
        uint32_t vad_input_energy_frames = 0;
        uint32_t vad_input_energy_invalid_frames = 0;
        // 最新 20 ms 配对输入快照，仅用于诊断。
        // feed 与 fetch 工作任务独立缓冲，因此这些值与 AFE 输出并非样本对齐。
        uint32_t microphone_rms = 0;
        uint32_t microphone_peak = 0;
        int32_t microphone_mean = 0;
        uint32_t microphone_ac_rms = 0;
        uint32_t reference_rms = 0;
        uint32_t reference_peak = 0;
        int32_t reference_mean = 0;
        uint32_t reference_ac_rms = 0;
        int32_t feed_core = -1;
        int32_t fetch_core = -1;
        uint32_t feed_budget_waits = 0;
        uint32_t fetch_budget_waits = 0;
        uint32_t feed_batch_max = 0;
        uint32_t fetch_batch_max = 0;
        // 保守的处理实际耗时，不是 CPU 时间。成功 fetch 可包含内部 AFE 等待；
        // 较长空等待不计入工作耗时，但仍计入四次尝试的协作等待上限。
        uint32_t feed_work_wall_max_us = 0;
        uint32_t fetch_work_wall_max_us = 0;
    };

    // 不持有网络或麦克风资源。enqueue() 只有一个采集生产者；
    // stop()/start() 改变资源前，必须先停止该生产者。
    esp_err_t start();
    // 仅在停止状态设置。回调在 fetch 工作任务执行，借用恰好 320 个 PCM 样本，不得阻塞。
    // 偏移在每次 start() 后从零单调递增；帧中任一样本被 VAD 判为语音，speech 即为真。
    // 前端终止性失败会停止输出并通过指标暴露；
    // 缺失块与 vad_cache 均不会被悄悄重放或替换。
    esp_err_t setOutputCallback(OutputCallback callback);
    esp_err_t enqueue(const int16_t *samples, size_t count);
    // AEC 模式仅接受明确配对的帧。两组数组描述同一麦克风样本区间；
    // 缺失参考绝不隐式用零替换。有界队列满时，唯一配对生产者可保留并重试同一帧。
    // ESP_ERR_NOT_FINISHED 表示尚未提交且前端继续运行；
    // 持有者须在重试前执行自身的时间线/年龄限制。改变资源前先停止该生产者。
    esp_err_t tryEnqueuePaired(const int16_t *microphone, const int16_t *reference, size_t count);
    // 严格提交：队列满时锁存 frontend_backlog/ESP_ERR_TIMEOUT。
    esp_err_t enqueuePaired(const int16_t *microphone, const int16_t *reference, size_t count);
    // 超时时保留全部 AFE/模型资源，工作任务退出后可重试。
    esp_err_t stop();
    // 外部持有者在诊断中保留其真实失败原因。
    void invalidate(esp_err_t error, const char *reason = "microphone_unavailable");
    Metrics metrics() const;
    void logSchedulingDiagnostics() const;
    // 顺序坐标中的非阻塞数值输入证据。前端资源仍存活时查询，
    // 例如在 fetch 回调中；绝不与 stop 竞争。
    // AFE 算法延迟未经校准，因此这不是物理对齐。
    SourceWindow sourceWindow(uint64_t first, uint64_t end) const;
    FailureSnapshot failureSnapshot() const {
        return failure_snapshot_ready_.load(std::memory_order_acquire)
            ? failure_snapshot_ : FailureSnapshot{};
    }
    bool aecRequested() const { return aec_requested_.load(std::memory_order_acquire); }
    bool aecActive() const { return aec_active_.load(std::memory_order_acquire); }

private:
    struct InputFrame {
        std::array<int16_t, 320> microphone;
        std::array<int16_t, 320> reference;
        bool paired = false;
    };
    static void feedEntry(void *argument);
    static void fetchEntry(void *argument);
    void feedLoop();
    void fetchLoop();
    void fail(esp_err_t error, const char *reason);
    void release();

    SrModelStore models_;
    const esp_afe_sr_iface_t *iface_ = nullptr;
    esp_afe_sr_data_t *afe_ = nullptr;
    QueueHandle_t input_queue_ = nullptr;
    SemaphoreHandle_t feed_done_ = nullptr;
    SemaphoreHandle_t fetch_done_ = nullptr;
    TaskHandle_t feed_task_ = nullptr;
    TaskHandle_t fetch_task_ = nullptr;
    using FeedAdapter = voiceaudio::PcmBlockAdapter<2048>;
    FeedAdapter *feed_adapter_ = nullptr;  // 位于 PSRAM，不占用 4 KiB 内部 BSS。
    using PairedFeedAdapter = voiceaudio::PairedPcmBlockAdapter<2048>;
    PairedFeedAdapter *paired_feed_adapter_ = nullptr;  // 位于 PSRAM，双通道交错 feed。
    // 仅 feed 工作任务访问；跨采集帧连续，创建任务前重置。
    voiceaudio::PcmDcBlocker microphone_dc_blocker_;
    bool microphone_dc_blocker_enabled_ = false;
    voiceaudio::SourceSignalHistory *source_history_ = nullptr;  // 仅 FD 模式，位于 PSRAM，由 feed 写入。
    voiceaudio::PcmBlockAdapter<320> output_adapter_;
    OutputCallback output_;
    uint64_t output_offset_ = 0;  // 仅 fetch 工作任务访问，创建任务前重置。
    bool output_pending_speech_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    // 停止期间持续 fetch，直到正在进行的 feed 完成，
    // 避免等待 AFE 环空间的 feed 因消费者退出而永久停留。
    std::atomic<bool> feed_exited_{true};
    std::atomic<bool> speech_{false};
#if defined(CONFIG_VOICE_LOCAL_FRONTEND_ENABLED) && CONFIG_VOICE_LOCAL_FRONTEND_ENABLED && \
    defined(CONFIG_VOICE_AEC_OBSERVATION_ENABLED) && CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    std::atomic<bool> aec_requested_{true};
#else
    std::atomic<bool> aec_requested_{false};
#endif
    std::atomic<bool> aec_active_{false};
    std::atomic<esp_err_t> last_error_{ESP_OK};
    FailureSnapshot failure_snapshot_;
    std::atomic<bool> failure_snapshot_ready_{false};
    std::atomic<uint32_t> feed_samples_{0};
    std::atomic<uint32_t> output_samples_{0};
    std::atomic<uint32_t> output_frames_{0};
    std::atomic<uint32_t> queue_peak_{0};
    std::atomic<uint32_t> feed_max_us_{0};
    std::atomic<uint32_t> fetch_max_us_{0};
    std::atomic<uint32_t> feed_stack_free_{0};
    std::atomic<uint32_t> fetch_stack_free_{0};
    std::atomic<uint32_t> feed_chunk_samples_{0};
    std::atomic<uint32_t> fetch_chunk_samples_{0};
    std::atomic<uint32_t> vad_transitions_{0};
    std::atomic<uint32_t> feed_total_us_{0};
    std::atomic<uint32_t> output_blocks_{0};
    std::atomic<uint32_t> output_rms_{0};
    std::atomic<uint32_t> output_peak_{0};
    std::atomic<bool> vad_thresholds_valid_{false};
    std::atomic<int32_t> vad_energy_threshold_cdbfs_{INT32_MIN};
    std::atomic<int32_t> vad_probability_threshold_milli_{-1};
    std::atomic<int32_t> vad_input_energy_cdbfs_{INT32_MIN};
    std::atomic<int32_t> vad_input_energy_peak_cdbfs_{INT32_MIN};
    std::atomic<uint32_t> vad_input_energy_frames_{0};
    std::atomic<uint32_t> vad_input_energy_invalid_frames_{0};
    std::atomic<uint32_t> microphone_rms_{0};
    std::atomic<uint32_t> microphone_peak_{0};
    std::atomic<int32_t> microphone_mean_{0};
    std::atomic<uint32_t> microphone_ac_rms_{0};
    std::atomic<uint32_t> reference_rms_{0};
    std::atomic<uint32_t> reference_peak_{0};
    std::atomic<int32_t> reference_mean_{0};
    std::atomic<uint32_t> reference_ac_rms_{0};
    std::atomic<uint32_t> feed_budget_waits_{0};
    std::atomic<uint32_t> fetch_budget_waits_{0};
    std::atomic<uint32_t> feed_batch_max_{0};
    std::atomic<uint32_t> fetch_batch_max_{0};
    std::atomic<uint32_t> feed_work_wall_max_us_{0};
    std::atomic<uint32_t> fetch_work_wall_max_us_{0};
};
