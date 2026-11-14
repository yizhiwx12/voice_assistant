#include "audio_service.hpp"
#include "app/memory_diagnostics.hpp"
#include "app/audio_idle_guard.hpp"
#include "reference_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <utility>
#include <new>
#include <memory>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#if defined(CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT) && CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT
#include "afe_signal_probe.hpp"
#endif

namespace {
constexpr char kTag[] = "audio_service";
constexpr size_t kCaptureSamples = 320;
constexpr size_t kPlaybackFrames = 50;  // 容量为一秒，与数据包大小无关。
constexpr size_t kAecCaptureFrames = 10;  // 最多保留 200 ms 麦克风 PCM。
constexpr uint64_t kAecReferenceWaitSamples = 1280;  // 回看 80 ms 已完成的参考信号。
constexpr uint32_t kPlaybackWorkBudgetUs = 8000;
constexpr uint32_t kPlaybackFrameBudget = 2;
constexpr uint32_t kPlaybackStartupSamples = 24000 * 160 / 1000;
constexpr uint32_t kPlaybackStartupTimeoutMs = 300;

uint32_t elapsedUs(int64_t started)
{
    return static_cast<uint32_t>(std::clamp<int64_t>(esp_timer_get_time() - started, 0, UINT32_MAX));
}

void observeMaximum(std::atomic<uint32_t> &value, uint32_t sample)
{
    uint32_t previous = value.load(std::memory_order_relaxed);
    while (sample > previous && !value.compare_exchange_weak(previous, sample, std::memory_order_relaxed)) {}
}

void addSaturated(std::atomic<uint32_t> &value, uint32_t amount)
{
    uint32_t previous = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(previous, previous > UINT32_MAX - amount ? UINT32_MAX : previous + amount,
                                        std::memory_order_relaxed)) {}
}
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
constexpr uint64_t kAecReferenceDelaySamples = CONFIG_VOICE_AEC_REFERENCE_DELAY_MS * 16ULL;
#else
constexpr uint64_t kAecReferenceDelaySamples = 0;
#endif

struct AcousticSnapshot {
    std::array<voiceaudio::AcousticDelayProbe::PlaybackStats,
               voiceaudio::AcousticDelayProbe::kPlaybackScopes> scopes{};
    size_t count = 0;
};

void logCaptureStartup(const Ics43434Microphone::DmaMetrics &metrics, uint64_t snapshot_us)
{
    const int64_t read_delay = metrics.capture_started && metrics.first_read_started &&
        metrics.first_read_us >= metrics.capture_start_us
        ? static_cast<int64_t>(metrics.first_read_us - metrics.capture_start_us) : -1;
    ESP_LOGI(kTag, "capture_startup started=%u first_read=%u enable_us=%llu first_read_us=%llu "
             "startup_read_us=%lld first_overflow_us=%llu first_overflow_sample_end=%llu",
             static_cast<unsigned>(metrics.capture_started),
             static_cast<unsigned>(metrics.first_read_started),
             static_cast<unsigned long long>(metrics.capture_start_us),
             static_cast<unsigned long long>(metrics.first_read_us),
             static_cast<long long>(read_delay),
             static_cast<unsigned long long>(metrics.first_overflow_us),
             static_cast<unsigned long long>(metrics.first_overflow_sample_end));
    const auto log_service = [](const char *scope, const Ics43434Microphone::ServiceMetrics &service,
                                uint64_t at_us) {
        ESP_LOGI(kTag, "capture_service scope=%s coherent=%u at_us=%llu phase=%s core=%d phase_since_us=%llu "
                 "phase_age_us=%llu read_calls=%u read_completions=%u first_read_end_us=%llu "
                 "last_read_start_us=%llu last_read_end_us=%llu last_first_sample=%llu last_sample_end=%llu "
                 "read_max_us=%llu read_gap_max_us=%llu callback_us=%llu callback_max_us=%llu",
                 scope, static_cast<unsigned>(service.coherent), static_cast<unsigned long long>(at_us),
                 Ics43434Microphone::consumerPhaseName(service.phase), service.core,
                 static_cast<unsigned long long>(service.phase_since_us),
                 static_cast<unsigned long long>(at_us >= service.phase_since_us ? at_us - service.phase_since_us : 0),
                 static_cast<unsigned>(service.read_calls), static_cast<unsigned>(service.read_completions),
                 static_cast<unsigned long long>(service.first_read_end_us),
                 static_cast<unsigned long long>(service.last_read_start_us),
                 static_cast<unsigned long long>(service.last_read_end_us),
                 static_cast<unsigned long long>(service.last_first_sample),
                 static_cast<unsigned long long>(service.last_sample_end),
                 static_cast<unsigned long long>(service.max_read_us),
                 static_cast<unsigned long long>(service.max_read_gap_us),
                 static_cast<unsigned long long>(service.last_callback_us),
                 static_cast<unsigned long long>(service.max_callback_us));
    };
    log_service("current", metrics.service, snapshot_us);
    if (metrics.first_overflow_us)
        log_service("first_overflow", metrics.first_overflow_service, metrics.first_overflow_us);
}

void logAcousticPlayback(const voiceaudio::AcousticDelayProbe::PlaybackStats &stats, size_t index)
{
    ESP_LOGI(kTag, "acoustic_playback index=%u generation=%u playback_serial=%u volume=%u "
             "attempts=%u accepted=%u rejected=%u has_accepted=%u saturated=%u "
             "last_delay_samples=%u last_correlation=%u last_margin=%u last_polarity=%d "
             "min_delay_samples=%u max_delay_samples=%u sample_rate=16000 calibrated=0",
             static_cast<unsigned>(index), static_cast<unsigned>(stats.dma_generation),
             static_cast<unsigned>(stats.playback_serial), static_cast<unsigned>(stats.volume_percent),
             static_cast<unsigned>(stats.attempts), static_cast<unsigned>(stats.accepted),
             static_cast<unsigned>(stats.rejected), static_cast<unsigned>(stats.has_accepted),
             static_cast<unsigned>(stats.saturated), static_cast<unsigned>(stats.last_accepted_delay_samples),
             static_cast<unsigned>(stats.last_accepted_correlation_per_mille),
             static_cast<unsigned>(stats.last_accepted_peak_margin_per_mille),
             static_cast<int>(stats.last_accepted_polarity),
             static_cast<unsigned>(stats.accepted_min_delay_samples),
             static_cast<unsigned>(stats.accepted_max_delay_samples));
    // 稀疏且精确的 1 ms 分组，最多 81 项。
    // 快照释放播放消费者的互斥锁后，才进行所有格式化和打印。
    char bins[256]{};
    size_t used = 0;
    for (size_t bin = 0; bin < stats.accepted_delay_histogram.size(); ++bin) {
        const uint32_t count = stats.accepted_delay_histogram[bin];
        if (!count) continue;
        if (used + 18 >= sizeof(bins)) {
            ESP_LOGI(kTag, "acoustic_hist generation=%u playback_serial=%u bin_ms_count=%s",
                     static_cast<unsigned>(stats.dma_generation),
                     static_cast<unsigned>(stats.playback_serial), bins);
            used = 0;
        }
        const int written = std::snprintf(bins + used, sizeof(bins) - used, "%s%u:%u",
            used ? "," : "", static_cast<unsigned>(bin), static_cast<unsigned>(count));
        if (written > 0) used += static_cast<size_t>(written);
    }
    if (used)
        ESP_LOGI(kTag, "acoustic_hist generation=%u playback_serial=%u bin_ms_count=%s",
                 static_cast<unsigned>(stats.dma_generation),
                 static_cast<unsigned>(stats.playback_serial), bins);
    const auto &rejected = stats.rejected_reason_counts;
    ESP_LOGI(kTag, "acoustic_rejections generation=%u playback_serial=%u invalid_input=%u "
             "incomplete_reference=%u low_mic_energy=%u low_ref_energy=%u low_correlation=%u "
             "boundary_peak=%u ambiguous_peak=%u insufficient_peak_support=%u",
             static_cast<unsigned>(stats.dma_generation), static_cast<unsigned>(stats.playback_serial),
             static_cast<unsigned>(rejected[1]), static_cast<unsigned>(rejected[2]),
             static_cast<unsigned>(rejected[3]), static_cast<unsigned>(rejected[4]),
             static_cast<unsigned>(rejected[5]), static_cast<unsigned>(rejected[6]),
             static_cast<unsigned>(rejected[7]), static_cast<unsigned>(rejected[8]));
}

class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
}  // 命名空间

AudioService::~AudioService()
{
    if (stop() != ESP_OK) {
        // 此时返回会销毁仍被音频工作任务引用的成员。
        ESP_LOGE(kTag, "Audio workers did not stop before destruction");
        std::abort();
    }
}

esp_err_t AudioService::start(CaptureCallback capture, PlaybackDoneCallback playback_done,
                              ErrorCallback error, uint8_t gain, uint8_t volume)
{
    if (playback_queue_) return ESP_ERR_INVALID_STATE;
    if (!capture || gain < 1 || gain > 16 || volume > 100) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT) && CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT
    // 隔离诊断必须在任一音频服务生产者或外设初始化前完成。
    // 失败或未停止的探测不能与正常运行共享 AFE 资源。
    const esp_err_t probe_result = voiceaudio::runAfeSignalProbe();
    if (probe_result != ESP_OK) return probe_result;
#endif
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    const esp_err_t idle_guard_result = voice::audio_idle_guard::initialize();
    if (idle_guard_result != ESP_OK) return idle_guard_result;
#endif
    capture_ = std::move(capture);
    playback_done_ = std::move(playback_done);
    error_ = std::move(error);
    gain_ = gain;
    speaker_config_.sample_rate_hz = 24000;
    speaker_config_.volume_percent = volume;
#if CONFIG_VOICE_PLAYBACK_REFERENCE_ENABLED
    speaker_config_.playback_reference_enabled = true;
#endif
    playback_queue_ = xQueueCreateWithCaps(kPlaybackFrames, sizeof(PlaybackFrame),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playback_queue_) {
        playback_queue_ = xQueueCreateWithCaps(kPlaybackFrames, sizeof(PlaybackFrame),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    stream_mutex_ = xSemaphoreCreateMutex();
    speaker_mutex_ = xSemaphoreCreateMutex();
    capture_done_ = xSemaphoreCreateBinary();
    capture_started_signal_ = xSemaphoreCreateBinary();
    playback_done_signal_ = xSemaphoreCreateBinary();
    Ics43434Microphone::Config microphone_config{};
    microphone_config.start_immediately = false;
#if CONFIG_VOICE_REFERENCE_ALIGNMENT_ENABLED
    microphone_config.observe_dma_timing = true;
#endif
    esp_err_t err = (!playback_queue_ || !stream_mutex_ || !speaker_mutex_ ||
                     !capture_done_ || !capture_started_signal_ || !playback_done_signal_)
        ? ESP_ERR_NO_MEM : microphone_.init(microphone_config);
    if (err == ESP_OK) err = speaker_.init(speaker_config_);
    if (err == ESP_OK) err = speaker_.setEnabled(false);
#if CONFIG_VOICE_REFERENCE_ALIGNMENT_ENABLED
    if (err == ESP_OK && speaker_.playbackReferenceMetrics().history_available) {
        void *storage = heap_caps_malloc(sizeof(voiceaudio::ReferenceMonitor),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage) {
            reference_monitor_ = new (storage) voiceaudio::ReferenceMonitor();
            if (!reference_monitor_->start()) {
                reference_monitor_->~ReferenceMonitor();
                heap_caps_free(reference_monitor_);
                reference_monitor_ = nullptr;
            }
        }
        if (!reference_monitor_)
            ESP_LOGW(kTag, "Reference alignment observer unavailable; half-duplex remains active");
        else {
            reference_capture_queue_ = xQueueCreateWithCaps(12, sizeof(ReferenceCaptureFrame),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (reference_capture_queue_) {
                reference_capture_scratch_ = static_cast<ReferenceCaptureFrame *>(heap_caps_malloc(
                    sizeof(ReferenceCaptureFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!reference_capture_scratch_) {
                    vQueueDeleteWithCaps(reference_capture_queue_);
                    reference_capture_queue_ = nullptr;
                } else new (reference_capture_scratch_) ReferenceCaptureFrame{};
            }
            if (!reference_capture_queue_)
                ESP_LOGW(kTag, "Acoustic delay probe unavailable; digital observation remains active");
            reference_monitor_->setPlaybackVolumeLabel(volume);
        }
    }
#endif
    reference_history_cursor_ = reference_poll_max_us_ = 0;
    reference_pump_max_us_ = reference_convert_max_us_ = reference_query_max_us_ = 0;
    reference_acoustic_max_us_ = reference_budget_yields_ = reference_capture_stale_ = reference_max_batch_ = 0;
    reference_history_pending_ = false;
    reference_capture_dropped_.store(0, std::memory_order_relaxed);
    capture_stack_free_min_.store(0, std::memory_order_relaxed);
    playback_stack_free_min_.store(0, std::memory_order_relaxed);
    aec_error_.store(ESP_OK);
    aec_failure_log_pending_.store(false);
    aec_failure_snapshot_ = {};
    aec_capture_peak_.store(0);
    aec_pairs_.store(0);
    aec_initial_silence_.store(0);
    aec_frontend_waits_.store(0);
    aec_last_valid_samples_.store(0);
    aec_pair_max_us_.store(0);
    aec_attempt_sequence_.store(0);
    for (auto &word : aec_attempt_words_) word.store(0);
    aec_last_pair_us_.store(0);
    aec_target_waiting_.store(false);
    aec_frontend_waiting_.store(false);
    aec_last_query_first_ = 0;
    aec_last_query_valid_ = aec_last_query_begin_ = aec_last_query_end_ = aec_last_query_missing_ = 0;
    reference_latest_sequence_ = 0;
    reference_oldest_sample_ = reference_latest_sample_end_ = 0;
    reference_timeline_healthy_ = reference_timeline_unmapped_ = 0;
    aec_catchup_passes_.store(0);
    aec_catchup_yields_.store(0);
    reference_last_service_us_ = 0;
    reference_service_gap_us_.store(0);
    reference_service_gap_max_us_.store(0);
    reference_clock_failure_ready_.store(false, std::memory_order_relaxed);
    reference_clock_failure_ = {};
    capture_start_error_.store(ESP_ERR_INVALID_STATE);
    playback_reference_drained_ = true;
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    if (err == ESP_OK) {
        aec_capture_queue_ = xQueueCreateWithCaps(kAecCaptureFrames, sizeof(AecCaptureFrame),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        aec_scratch_ = static_cast<AecScratch *>(heap_caps_calloc(
            1, sizeof(AecScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (aec_scratch_) new (aec_scratch_) AecScratch{};
        aec_capture_scratch_ = static_cast<AecCaptureFrame *>(heap_caps_calloc(
            1, sizeof(AecCaptureFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (aec_capture_scratch_) new (aec_capture_scratch_) AecCaptureFrame{};
    }
#endif
    if (err == ESP_OK) {
        voice::memory::log("audio_after_hardware");
        // 原始手动采集与处理后的自动输出彼此独立。
        // AFE 失败通过 frontendMetrics/UI 保持可见；控制器必须禁用自动监听，
        // 不能悄悄改为上传原始音频。
        const esp_err_t frontend_err = frontend_.start();
        if (frontend_err != ESP_OK)
            ESP_LOGE(kTag, "Local AFE/VAD failed: %s; raw manual capture remains available",
                     esp_err_to_name(frontend_err));
        if (frontend_.aecRequested() &&
            (!reference_monitor_ || !aec_capture_queue_ || !aec_scratch_ || !aec_capture_scratch_))
            failAecReference(ESP_ERR_NO_MEM, "reference_resources");
        voice::memory::log("audio_after_frontend");
#if CONFIG_VOICE_REFERENCE_ALIGNMENT_ENABLED
        // AFE/模型初始化耗时可能超过八个 ISR 槽的容量。
        // RX 保持禁用；开始观测前为 TX 创建新代次。
        if (reference_monitor_) {
            // 整个模型加载期间，RX 虽已配置但仍保持禁用。
            // 只有 TX 需要在清除启动积压后创建新代次。
            err = speaker_.resetPlayback();
            reference_history_cursor_ = speaker_.playbackReferenceMetrics().latest_history_sequence;
        }
#endif
        running_.store(true, std::memory_order_release);
    }
    // DMA 采集与两个 AFE 工作任务保留在 CPU1，播放/参考信号的持有者运行在 CPU0。
    // 下行队列出现突发时，参考信号持有者仍须持续轮询；此前与 feed 同放 CPU1，
    // 同优先级 DSP 工作会延迟参考服务，足以触发 200 ms 配对年龄保护。
    // 现在 fetch 与 feed 共用 CPU1，为 CPU0 的参考服务和 Opus 解码留出空间。
    // 所有工作任务保留有界执行点；传输任务仍为优先级 5。
    // M 路径保持原策略。
    const bool aec_tasks = frontend_.aecRequested();
    if (err == ESP_OK) {
        const BaseType_t created = aec_tasks
            ? xTaskCreatePinnedToCore(playbackTaskEntry, "voice_play", 6144, this, 6, &playback_task_, 0)
            : xTaskCreate(playbackTaskEntry, "voice_play", 6144, this, 5, &playback_task_);
        if (created != pdPASS) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        const BaseType_t created = aec_tasks
            ? xTaskCreatePinnedToCore(captureTaskEntry, "voice_capture", 6144, this, 7, &capture_task_, 1)
            : xTaskCreate(captureTaskEntry, "voice_capture", 6144, this, 6, &capture_task_);
        if (created != pdPASS) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        // 消费者自行启用 RX 后立即进入 read()。
        // 两者之间不能插入 UART 输出或模型/任务分配。
        if (xSemaphoreTake(capture_started_signal_, pdMS_TO_TICKS(1000)) != pdTRUE)
            err = ESP_ERR_TIMEOUT;
        else err = capture_start_error_.load(std::memory_order_acquire);
    }
    if (err != ESP_OK) {
        voice::memory::log("audio_start_failed");
        stop();
        return err;
    }
    voice::memory::log("audio_after_tasks");
    ESP_LOGI(kTag, "audio_scheduling aec=%u capture_core=%d capture_priority=%u reference_core=%d "
             "reference_priority=%u frontend_core=%d frontend_priority=%u",
             static_cast<unsigned>(aec_tasks), aec_tasks ? 1 : -1, aec_tasks ? 7U : 6U,
             aec_tasks ? 0 : -1, aec_tasks ? 6U : 5U, aec_tasks ? 1 : -1, aec_tasks ? 6U : 5U);
    ESP_LOGI(kTag, "Continuous PCM16 capture 16k; raw manual / AFE automatic; playback 24k; queue 1000 ms; gain %u",
             static_cast<unsigned>(gain_));
    return ESP_OK;
}

void AudioService::captureTaskEntry(void *argument)
{
    auto *self = static_cast<AudioService *>(argument);
    self->captureTask();
    self->microphone_.noteConsumerPhase(Ics43434Microphone::ConsumerPhase::Stopped);
    xSemaphoreGive(self->capture_done_);
    vTaskDelete(nullptr);
}

void AudioService::captureTask()
{
    std::array<int32_t, kCaptureSamples> raw{};
    std::array<int16_t, kCaptureSamples> pcm{};
    size_t filled = 0;
    uint64_t frame_first_sample = 0;
    uint32_t frame_serial = 0;
    bool frame_known_mute = false;
    uint32_t stack_probe_frames = 0;
    bool frame_continuous = true;
    const esp_err_t start_error = running_.load(std::memory_order_acquire)
        ? microphone_.startCapture() : ESP_ERR_INVALID_STATE;
    capture_start_error_.store(start_error, std::memory_order_release);
    xSemaphoreGive(capture_started_signal_);
    if (start_error != ESP_OK) return;
    while (running_.load(std::memory_order_acquire)) {
        size_t count = 0;
        const uint32_t capture_serial = serial_.load(std::memory_order_acquire);
        Ics43434Microphone::ReadTiming timing{};
        const esp_err_t err = microphone_.read(raw.data(), kCaptureSamples - filled, &count, 100,
            (reference_capture_queue_ || frontend_.aecRequested()) ? &timing : nullptr);
        if (!running_.load(std::memory_order_acquire)) return;
        if (err != ESP_OK || !count) {
            ESP_LOGE(kTag, "Microphone read failed: %s", esp_err_to_name(err));
            frontend_.invalidate(err == ESP_OK ? ESP_ERR_TIMEOUT : err);
            if (error_) error_(err == ESP_OK ? ESP_ERR_TIMEOUT : err);
            return;
        }
        microphone_.noteConsumerPhase(Ics43434Microphone::ConsumerPhase::Convert);
        if (!filled) {
            frame_first_sample = timing.first_sample;
            frame_serial = capture_serial;
            frame_continuous = timing.continuous;
            const auto playback = speaker_.playbackReferenceTag();
            frame_known_mute = playback.playback_serial == 0;
        } else {
            const auto playback = speaker_.playbackReferenceTag();
            frame_continuous = frame_continuous && timing.continuous &&
                timing.first_sample == frame_first_sample + filled && frame_serial == capture_serial;
            frame_known_mute = frame_known_mute && playback.playback_serial == 0;
        }
        uint32_t clipped = 0;
        for (size_t i = 0; i < count; ++i) {
            // 麦克风的有符号 24 位样本在 32 位槽中左对齐。
            // 先缩放再限幅，不能直接将其重新解释为 PCM16。
            const int64_t scaled = static_cast<int64_t>(raw[i]) * gain_ / 65536;
            if (scaled > INT16_MAX || scaled < INT16_MIN) ++clipped;
            pcm[filled + i] = static_cast<int16_t>(std::clamp<int64_t>(scaled, INT16_MIN, INT16_MAX));
        }
        clipped_.fetch_add(clipped, std::memory_order_relaxed);
        filled += count;
        if (filled != kCaptureSamples) continue;
        uint64_t energy = 0;
        uint32_t peak = 0;
        for (const int16_t sample : pcm) {
            const int32_t value = sample;
            energy += static_cast<int64_t>(value) * value;
            peak = std::max(peak, static_cast<uint32_t>(value < 0 ? -value : value));
        }
        rms_.store(static_cast<uint32_t>(std::sqrt(static_cast<double>(energy) / kCaptureSamples)),
                   std::memory_order_relaxed);
        peak_.store(peak, std::memory_order_relaxed);
        microphone_.noteConsumerPhase(Ics43434Microphone::ConsumerPhase::Callback);
        capture_(pcm.data(), pcm.size());
        microphone_.noteConsumerPhase(Ics43434Microphone::ConsumerPhase::Queue);
        // AFE 的调度/背压绝不能改变手动录音时间线。
        // enqueue 为非阻塞操作；其错误会锁存本地 VAD 失败状态。
        if (!frontend_.aecRequested()) {
            frontend_.enqueue(pcm.data(), pcm.size());
        } else if (frontend_.aecActive() && aec_error_.load() == ESP_OK) {
            if (!frame_continuous) {
                failAecReference(ESP_ERR_INVALID_STATE, "capture_timeline");
            } else if (aec_capture_queue_ && aec_capture_scratch_) {
                aec_capture_scratch_->first_sample = frame_first_sample;
                aec_capture_scratch_->known_mute = frame_known_mute;
                aec_capture_scratch_->pcm = pcm;
                if (xQueueSend(aec_capture_queue_, aec_capture_scratch_, 0) != pdTRUE) {
                    failAecReference(ESP_ERR_TIMEOUT, "pair_queue_full");
                } else {
                    const uint32_t peak = uxQueueMessagesWaiting(aec_capture_queue_);
                    const uint32_t previous = aec_capture_peak_.load();
                    if (peak > previous) aec_capture_peak_.store(peak);
                }
            }
        }
        if (reference_capture_queue_ && !frontend_.aecRequested() && frame_continuous &&
            playing_.load(std::memory_order_acquire) &&
            frame_serial == serial_.load(std::memory_order_acquire)) {
            // 此单生产者临时区放在 PSRAM，避免在采集任务有限的栈上
            // 再增加一份 656 字节 PCM 副本。
            reference_capture_scratch_->first_sample = frame_first_sample;
            reference_capture_scratch_->playback_serial = frame_serial;
            reference_capture_scratch_->pcm = pcm;
            if (xQueueSend(reference_capture_queue_, reference_capture_scratch_, 0) != pdTRUE)
                reference_capture_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        filled = 0;
        if (++stack_probe_frames >= 50) {
            stack_probe_frames = 0;
            capture_stack_free_min_.store(uxTaskGetStackHighWaterMark(nullptr), std::memory_order_relaxed);
        }
    }
}

esp_err_t AudioService::stop()
{
    running_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    serial_.fetch_add(1, std::memory_order_acq_rel);
    if (capture_task_) {
        if (xSemaphoreTake(capture_done_, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        capture_task_ = nullptr;
    }
    if (playback_task_) {
        if (xSemaphoreTake(playback_done_signal_, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        playback_task_ = nullptr;
    }
    // 此时采集生产者已无法入队；前端先完成正在进行的 feed，
    // 再退出 fetch 工作任务，随后销毁 AFE，最后卸载模型。
    esp_err_t result = frontend_.stop();
    if (result != ESP_OK) return result;
    decoder_.close();
    const esp_err_t speaker_result = speaker_.deinit();
    const esp_err_t microphone_result = microphone_.deinit();
    if (speaker_result != ESP_OK) result = speaker_result;
    if (result == ESP_OK && microphone_result != ESP_OK) result = microphone_result;
    if (result != ESP_OK) return result;  // 硬件仍持有资源时保留状态。
    if (reference_monitor_) {
        reference_monitor_->~ReferenceMonitor();
        heap_caps_free(reference_monitor_);
        reference_monitor_ = nullptr;
    }
    if (playback_queue_) vQueueDeleteWithCaps(playback_queue_);
    if (reference_capture_queue_) vQueueDeleteWithCaps(reference_capture_queue_);
    if (aec_capture_queue_) vQueueDeleteWithCaps(aec_capture_queue_);
    aec_capture_queue_ = nullptr;
    if (aec_scratch_) {
        aec_scratch_->~AecScratch();
        heap_caps_free(aec_scratch_);
        aec_scratch_ = nullptr;
    }
    if (aec_capture_scratch_) {
        aec_capture_scratch_->~AecCaptureFrame();
        heap_caps_free(aec_capture_scratch_);
        aec_capture_scratch_ = nullptr;
    }
    if (reference_capture_scratch_) {
        reference_capture_scratch_->~ReferenceCaptureFrame();
        heap_caps_free(reference_capture_scratch_);
        reference_capture_scratch_ = nullptr;
    }
    if (stream_mutex_) vSemaphoreDelete(stream_mutex_);
    if (speaker_mutex_) vSemaphoreDelete(speaker_mutex_);
    if (capture_done_) vSemaphoreDelete(capture_done_);
    if (capture_started_signal_) vSemaphoreDelete(capture_started_signal_);
    if (playback_done_signal_) vSemaphoreDelete(playback_done_signal_);
    playback_queue_ = nullptr;
    reference_capture_queue_ = nullptr;
    stream_mutex_ = speaker_mutex_ = capture_done_ = capture_started_signal_ = playback_done_signal_ = nullptr;
    playing_.store(false);
    has_pending_ = false;
    ended_ = false;
    return result;
}

esp_err_t AudioService::stopPlayback()
{
    if (!playback_queue_) return ESP_ERR_INVALID_STATE;
    // 先设置隔离屏障，再获取任何可能竞争的锁。
    // 因背压阻塞的生产者会在一次 20 ms 重试内发现序号变化。
    active_.store(false, std::memory_order_release);
    serial_.fetch_add(1, std::memory_order_acq_rel);
    Lock stream_lock(stream_mutex_);
    decoder_.close();
    has_pending_ = false;
    ended_ = false;
    xQueueReset(playback_queue_);
    Lock speaker_lock(speaker_mutex_);
    pollReferenceMonitor();
    esp_err_t err = ESP_OK;
    if (frontend_.aecRequested() && frontend_.aecActive() && aec_error_.load() == ESP_OK) {
        // AEC 需要连续且经物理观测的参考信号。拉低 SD 前先用零样本冲刷有界 TX 环，
        // 跨回答保留时钟及代次；取消时可能保留不超过 50 ms 的 DMA 尾音。
        // 下面默认的非 AEC 路径仍保留立即静音/重置屏障。
        if (!playback_reference_drained_) err = drainPlayback();
        if (err == ESP_OK) err = speaker_.setEnabled(false);
        pollReferenceMonitor();
        if (err != ESP_OK) {
            failAecReference(err, "playback_fence");
            // 即使排空失败，也要保留停止可闻声音的安全措施。
            speaker_.resetPlayback();
        }
    } else {
        err = speaker_.resetPlayback();
        playback_reference_drained_ = err == ESP_OK;
    }
    if (reference_capture_queue_) xQueueReset(reference_capture_queue_);
    playing_.store(false, std::memory_order_release);
    return err;
}

esp_err_t AudioService::beginPlayback(const voice::Identity &identity, uint32_t stream_id,
                                      bool opus, uint32_t pre_skip)
{
    if (!identity.epoch || !identity.turn || !identity.generation || !stream_id ||
        pre_skip > 960 || (!opus && pre_skip)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = stopPlayback();
    if (err != ESP_OK) return err;
    Lock lock(stream_mutex_);
    if (opus) {
        err = decoder_.open(24000);
        if (err != ESP_OK) return err;
    }
    identity_ = identity;
    stream_id_ = stream_id;
    sequence_ = 0;
    offset_ = emitted_until_ = pending_offset_ = 0;
    pre_skip_ = pre_skip;
    opus_ = opus;
    has_pending_ = false;
    ended_ = false;
    {
        Lock speaker_lock(speaker_mutex_);
        // 新建/清空的 DMA 代次或成功排空的 AEC 作用域只包含零样本。
        // 此前回答不能越过此绑定边界。
        if (!speaker_.bindPlaybackSerial(serial_.load(std::memory_order_acquire))) {
            if (frontend_.aecRequested()) {
                failAecReference(ESP_ERR_INVALID_STATE, "playback_binding");
                return ESP_ERR_INVALID_STATE;
            }
            ESP_LOGW(kTag, "Reference playback identity binding rejected");
        }
        resetPlaybackWork(identity, stream_id, serial_.load(std::memory_order_acquire));
    }
    active_.store(true, std::memory_order_release);
    return ESP_OK;
}

void AudioService::resetPlaybackWork(const voice::Identity &identity, uint32_t stream_id, uint32_t serial)
{
    playback_work_.sequence.fetch_add(1, std::memory_order_acq_rel);
    for (auto *counter : {&playback_work_.decoded_packets, &playback_work_.decoded_samples,
         &playback_work_.decode_last_us, &playback_work_.decode_max_us, &playback_work_.enqueue_waits,
         &playback_work_.enqueue_wait_total_us, &playback_work_.enqueue_wait_max_us, &playback_work_.queue_peak,
         &playback_work_.empty_waits, &playback_work_.empty_wait_total_us, &playback_work_.empty_wait_max_us,
         &playback_work_.write_calls, &playback_work_.write_samples, &playback_work_.write_max_us,
         &playback_work_.write_gap_max_us, &playback_work_.budget_waits,
         &playback_work_.enqueued_samples, &playback_work_.startup_samples,
         &playback_work_.startup_wait_us}) counter->store(0, std::memory_order_relaxed);
    playback_last_write_end_us_ = 0;
    playback_work_.startup_ready.store(false, std::memory_order_relaxed);
    playback_work_.startup_deadline.store(false, std::memory_order_relaxed);
    playback_work_.startup_end_received.store(false, std::memory_order_relaxed);
    playback_work_.end_received.store(false, std::memory_order_relaxed);
    playback_work_.serial.store(serial, std::memory_order_relaxed);
    playback_work_.epoch.store(identity.epoch, std::memory_order_relaxed);
    playback_work_.turn.store(identity.turn, std::memory_order_relaxed);
    playback_work_.generation.store(identity.generation, std::memory_order_relaxed);
    playback_work_.stream.store(stream_id, std::memory_order_relaxed);
    playback_work_.sequence.fetch_add(1, std::memory_order_release);
}

void AudioService::logPlaybackWork() const
{
    const uint32_t before = playback_work_.sequence.load(std::memory_order_acquire);
    const auto load = [](const std::atomic<uint32_t> &counter) { return counter.load(std::memory_order_relaxed); };
    const uint32_t serial = load(playback_work_.serial), epoch = load(playback_work_.epoch);
    const uint32_t turn = load(playback_work_.turn), generation = load(playback_work_.generation);
    const uint32_t stream = load(playback_work_.stream), decoded_packets = load(playback_work_.decoded_packets);
    const uint32_t decoded_samples = load(playback_work_.decoded_samples), decode_last = load(playback_work_.decode_last_us);
    const uint32_t decode_max = load(playback_work_.decode_max_us), enqueue_waits = load(playback_work_.enqueue_waits);
    const uint32_t enqueue_total = load(playback_work_.enqueue_wait_total_us), enqueue_max = load(playback_work_.enqueue_wait_max_us);
    const uint32_t peak = load(playback_work_.queue_peak), empty_waits = load(playback_work_.empty_waits);
    const uint32_t empty_total = load(playback_work_.empty_wait_total_us), empty_max = load(playback_work_.empty_wait_max_us);
    const uint32_t writes = load(playback_work_.write_calls), samples = load(playback_work_.write_samples);
    const uint32_t write_max = load(playback_work_.write_max_us), gap_max = load(playback_work_.write_gap_max_us);
    const uint32_t waits = load(playback_work_.budget_waits);
    const bool startup_ready = playback_work_.startup_ready.load(std::memory_order_acquire);
    const uint32_t startup_samples = load(playback_work_.startup_samples);
    const uint32_t startup_wait = load(playback_work_.startup_wait_us);
    const bool startup_deadline = playback_work_.startup_deadline.load(std::memory_order_relaxed);
    const bool startup_end = playback_work_.startup_end_received.load(std::memory_order_relaxed);
    const bool end = playback_work_.end_received.load(std::memory_order_relaxed);
    const bool coherent = !(before & 1) && before == playback_work_.sequence.load(std::memory_order_acquire);
    ESP_LOGI(kTag, "playback_flow scope=stream coherent=%u serial=%u epoch=%u turn=%u generation=%u stream=%u "
             "decoded_packets=%u decoded_samples=%u decode_last_us=%u decode_max_us=%u enqueue_waits=%u "
             "enqueue_wait_total_us=%u enqueue_wait_max_us=%u queue_peak=%u end_received=%u timing=wall totals=saturating32",
             static_cast<unsigned>(coherent), serial, epoch, turn, generation, stream, decoded_packets, decoded_samples,
             decode_last, decode_max, enqueue_waits, enqueue_total, enqueue_max, peak, static_cast<unsigned>(end));
    ESP_LOGI(kTag, "playback_service scope=stream coherent=%u serial=%u empty_waits=%u empty_wait_total_us=%u "
             "empty_wait_max_us=%u content_write_calls=%u content_write_samples=%u content_write_max_us=%u "
             "content_write_gap_max_us=%u budget_waits=%u timing=wall gap=end_to_start excludes_drain=1 "
             "empty_wait_is_not_dma_underrun=1 dma_underruns=unmeasured",
             static_cast<unsigned>(coherent), serial, empty_waits, empty_total, empty_max, writes, samples, write_max, gap_max, waits);
    ESP_LOGI(kTag, "playback_startup scope=stream coherent=%u serial=%u ready=%u "
             "sample_watermark=%u buffered_samples=%u target_samples=%u rate=24000 wait_us=%u max_wait_ms=%u "
             "deadline_hit=%u end_received=%u",
             static_cast<unsigned>(coherent), serial, static_cast<unsigned>(startup_ready),
             static_cast<unsigned>(frontend_.aecRequested()), startup_samples,
             frontend_.aecRequested() ? kPlaybackStartupSamples : 0U, startup_wait,
             frontend_.aecRequested() ? kPlaybackStartupTimeoutMs : 100U,
             static_cast<unsigned>(startup_deadline), static_cast<unsigned>(startup_end));
}

esp_err_t AudioService::enqueue(const PlaybackFrame &frame)
{
    const int64_t started = esp_timer_get_time();
    bool waited = false;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    while (active_.load(std::memory_order_acquire) &&
           serial_.load(std::memory_order_acquire) == frame.serial) {
        if (xQueueSend(playback_queue_, &frame, waited ? pdMS_TO_TICKS(20) : 0) == pdTRUE) {
            if (!frame.end) addSaturated(playback_work_.enqueued_samples, frame.samples);
            observeMaximum(playback_work_.queue_peak, uxQueueMessagesWaiting(playback_queue_));
            result = ESP_OK;
            break;
        }
        waited = true;
    }
    if (waited) {
        playback_work_.enqueue_waits.fetch_add(1, std::memory_order_relaxed);
        const uint32_t elapsed = elapsedUs(started);
        addSaturated(playback_work_.enqueue_wait_total_us, elapsed);
        observeMaximum(playback_work_.enqueue_wait_max_us, elapsed);
    }
    return result;
}

esp_err_t AudioService::enqueuePending(uint64_t end, uint32_t serial)
{
    if (!has_pending_) return ESP_OK;
    const uint64_t start = std::max<uint64_t>({pre_skip_, pending_offset_, emitted_until_});
    const uint64_t stop = std::min<uint64_t>(end, pending_offset_ + 480);
    if (stop <= start) return ESP_OK;
    PlaybackFrame frame{};
    frame.serial = serial;
    frame.identity = identity_;
    frame.samples = stop - start;
    std::copy_n(pending_pcm_.data() + start - pending_offset_, frame.samples, frame.pcm.data());
    const esp_err_t err = enqueue(frame);
    if (err == ESP_OK) emitted_until_ = stop;
    return err;
}

esp_err_t AudioService::feedPlayback(const voice::Identity &identity, uint32_t stream_id,
                                     uint32_t sequence, uint64_t sample_offset,
                                     const uint8_t *payload, size_t bytes)
{
    if (!playback_queue_ || !payload) return ESP_ERR_INVALID_ARG;
    const uint32_t serial = serial_.load(std::memory_order_acquire);
    Lock lock(stream_mutex_);
    if (!active_.load(std::memory_order_acquire) || ended_ || serial != serial_.load(std::memory_order_acquire) ||
        identity != identity_ || stream_id != stream_id_) return ESP_ERR_INVALID_STATE;
    if (sequence != sequence_ || sample_offset != offset_) return ESP_ERR_INVALID_ARG;
    std::array<int16_t, 480> decoded{};
    const int64_t decode_started = esp_timer_get_time();
    if (opus_) {
        size_t samples = 0;
        const esp_err_t err = decoder_.decode(payload, bytes, decoded.data(), decoded.size(), &samples);
        if (err != ESP_OK || samples != decoded.size()) return ESP_ERR_INVALID_SIZE;
    } else {
        if (bytes != decoded.size() * sizeof(int16_t)) return ESP_ERR_INVALID_SIZE;
        for (size_t i = 0; i < decoded.size(); ++i) {
            decoded[i] = static_cast<int16_t>(static_cast<uint16_t>(payload[2 * i]) |
                                             static_cast<uint16_t>(payload[2 * i + 1]) << 8);
        }
    }
    const uint32_t decode_us = elapsedUs(decode_started);
    playback_work_.decode_last_us.store(decode_us, std::memory_order_relaxed);
    observeMaximum(playback_work_.decode_max_us, decode_us);
    playback_work_.decoded_packets.fetch_add(1, std::memory_order_relaxed);
    addSaturated(playback_work_.decoded_samples, decoded.size());
    const esp_err_t err = enqueuePending(offset_, serial);
    if (err != ESP_OK) return err;
    pending_pcm_ = decoded;
    pending_offset_ = offset_;
    has_pending_ = true;
    offset_ += decoded.size();
    ++sequence_;
    return ESP_OK;
}

esp_err_t AudioService::endPlayback(const voice::Identity &identity, uint32_t stream_id,
                                    uint64_t valid_end_sample)
{
    if (!playback_queue_) return ESP_ERR_INVALID_STATE;
    const uint32_t serial = serial_.load(std::memory_order_acquire);
    Lock lock(stream_mutex_);
    if (!active_.load(std::memory_order_acquire) || ended_ || serial != serial_.load(std::memory_order_acquire) ||
        identity != identity_ || stream_id != stream_id_) return ESP_ERR_INVALID_STATE;
    if (!has_pending_ || valid_end_sample <= pre_skip_ ||
        valid_end_sample < std::max<uint64_t>(pre_skip_, emitted_until_) ||
        valid_end_sample > offset_) return ESP_ERR_INVALID_ARG;
    esp_err_t err = enqueuePending(valid_end_sample, serial);
    if (err != ESP_OK) return err;
    PlaybackFrame end{};
    end.serial = serial;
    end.identity = identity_;
    end.end = true;
    err = enqueue(end);
    if (err == ESP_OK) {
        has_pending_ = false;
        ended_ = true;
        playback_work_.end_received.store(true, std::memory_order_release);
        decoder_.close();
    }
    return err;
}

void AudioService::playbackTaskEntry(void *argument)
{
    auto *self = static_cast<AudioService *>(argument);
    self->playbackTask();
    xSemaphoreGive(self->playback_done_signal_);
    vTaskDelete(nullptr);
}

void AudioService::playbackTask()
{
    PlaybackFrame frame{};
    uint32_t started_serial = 0;
    uint32_t stack_probe_frames = 0;
    uint32_t batch_work_us = 0, batch_frames = 0;
    while (running_.load(std::memory_order_acquire)) {
        // CPU0 的两个工作任务依据同一份空闲进度观测协作。
        // 分别休眠一个节拍可能交替运行，却始终不给 IDLE0 执行机会。
        // 此边界不持有音频锁或借用帧；等待保持有界，
        // 使外部 CPU 压力下参考服务仍能获得执行机会。
        if (frontend_.aecRequested()) {
            voice::audio_idle_guard::cooperate(running_, true, 8);
            if (!running_.load(std::memory_order_acquire)) break;
        }
        logPendingAecFailure();
        if (++stack_probe_frames >= 50) {
            stack_probe_frames = 0;
            playback_stack_free_min_.store(uxTaskGetStackHighWaterMark(nullptr), std::memory_order_relaxed);
        }
        // 有序参考工作或因 AFE 背压保留的完整配对会再获得一次短服务机会。
        // 等待队列时不持有扬声器锁，不绕过游标或年龄限制。
        const TickType_t idle_wait =
            reference_history_pending_.load(std::memory_order_relaxed) ||
            aec_frontend_waiting_.load(std::memory_order_relaxed) ? 1 : pdMS_TO_TICKS(20);
        const uint32_t wait_serial = playback_work_.serial.load(std::memory_order_acquire);
        const bool measure_empty = active_.load(std::memory_order_acquire) && playing_.load(std::memory_order_acquire) &&
            !playback_work_.end_received.load(std::memory_order_acquire) && uxQueueMessagesWaiting(playback_queue_) == 0;
        const int64_t wait_started = measure_empty ? esp_timer_get_time() : 0;
        const auto record_empty_wait = [&] {
            // 调用方持有 speaker_mutex_；新作用域无法在此次身份检查与
            // 旧等待记录发布之间重置计数器。
            if (measure_empty && wait_serial == playback_work_.serial.load(std::memory_order_relaxed) &&
                wait_serial == serial_.load(std::memory_order_acquire)) {
                playback_work_.empty_waits.fetch_add(1, std::memory_order_relaxed);
                const uint32_t elapsed = elapsedUs(wait_started);
                addSaturated(playback_work_.empty_wait_total_us, elapsed);
                observeMaximum(playback_work_.empty_wait_max_us, elapsed);
            }
        };
        if (xQueueReceive(playback_queue_, &frame, idle_wait) != pdTRUE) {
            Lock lock(speaker_mutex_);
            record_empty_wait();
            pollReferenceMonitor();
            batch_work_us = batch_frames = 0;
            continue;
        }
        if (measure_empty) { Lock lock(speaker_mutex_); record_empty_wait(); }
        if (!running_.load(std::memory_order_acquire)) break;
        if (frame.serial != serial_.load(std::memory_order_acquire)) continue;
        const int64_t frame_started = esp_timer_get_time();
        if (!frame.end && started_serial != frame.serial) {
            const int64_t startup_started = esp_timer_get_time();
            const bool sample_prefill = frontend_.aecRequested();
            const TickType_t deadline = xTaskGetTickCount() +
                pdMS_TO_TICKS(sample_prefill ? kPlaybackStartupTimeoutMs : 100);
            // 统计 pre-skip 之后的真实 PCM，包含当前持有帧。
            // 已完成的短回答可立即播放；慢生产者获得有界抖动缓冲，不会只到一帧就开播。
            // 保留非 AEC 路径原有的四帧入队/100 ms 策略；
            // 样本水位仅用于同时运行 AEC 的情况。
            while ((sample_prefill ?
                       (playback_work_.enqueued_samples.load(std::memory_order_acquire) < kPlaybackStartupSamples &&
                        !playback_work_.end_received.load(std::memory_order_acquire)) :
                       uxQueueMessagesWaiting(playback_queue_) < 4) &&
                   running_.load(std::memory_order_acquire) &&
                   frame.serial == serial_.load(std::memory_order_acquire) &&
                   static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                Lock lock(speaker_mutex_);
                pollReferenceMonitor();
            }
            {
                Lock lock(speaker_mutex_);
                if (!running_.load(std::memory_order_acquire)) break;
                if (frame.serial != serial_.load(std::memory_order_acquire)) continue;
                const uint32_t buffered = playback_work_.enqueued_samples.load(std::memory_order_acquire);
                const bool ended = playback_work_.end_received.load(std::memory_order_acquire);
                playback_work_.startup_samples.store(buffered, std::memory_order_relaxed);
                playback_work_.startup_wait_us.store(elapsedUs(startup_started), std::memory_order_relaxed);
                playback_work_.startup_end_received.store(ended, std::memory_order_relaxed);
                playback_work_.startup_deadline.store(sample_prefill ?
                                                      (!ended && buffered < kPlaybackStartupSamples) :
                                                      uxQueueMessagesWaiting(playback_queue_) < 4,
                                                      std::memory_order_relaxed);
                playback_work_.startup_ready.store(true, std::memory_order_release);
            }
            started_serial = frame.serial;
        }
        esp_err_t err = ESP_OK;
        bool done = false;
        {
            Lock lock(speaker_mutex_);
            pollReferenceMonitor();
            if (frame.serial != serial_.load(std::memory_order_acquire)) continue;
            if (!frame.end) {
                err = speaker_.setEnabled(true);
                if (err == ESP_OK) playing_.store(true, std::memory_order_release);
                // 每次写入最多 10 ms PCM，限制正常硬件写入中的阻塞时间。
                // AEC 还可在该次写入前使用一个有界的参考追赶时间片。
                for (size_t offset = 0; err == ESP_OK && offset < frame.samples; offset += 240) {
                    if (frame.serial != serial_.load(std::memory_order_acquire)) break;
                    if (frontend_.aecRequested() && aec_target_waiting_.load(std::memory_order_relaxed))
                        serviceAecReferenceBeforePlayback();
                    if (!running_.load(std::memory_order_acquire) ||
                        frame.serial != serial_.load(std::memory_order_acquire)) break;
                    playback_reference_drained_ = false;
                    const int64_t write_started = esp_timer_get_time();
                    if (playback_last_write_end_us_ && static_cast<uint64_t>(write_started) >= playback_last_write_end_us_)
                        observeMaximum(playback_work_.write_gap_max_us, static_cast<uint32_t>(
                            std::min<uint64_t>(UINT32_MAX, write_started - playback_last_write_end_us_)));
                    const size_t count = std::min<size_t>(240, frame.samples - offset);
                    err = speaker_.writeMono(frame.pcm.data() + offset,
                                              count, 100);
                    playback_last_write_end_us_ = esp_timer_get_time();
                    playback_work_.write_calls.fetch_add(1, std::memory_order_relaxed);
                    observeMaximum(playback_work_.write_max_us, static_cast<uint32_t>(std::min<uint64_t>(
                        UINT32_MAX, playback_last_write_end_us_ - write_started)));
                    if (err == ESP_OK) addSaturated(playback_work_.write_samples, count);
                    // 在 10 ms 块之间保留物理 ISR 历史；
                    // 较重的转换/配对在帧边界运行。只有已经等待的有序目标需要写入前追赶。
                    if (frontend_.aecRequested()) speaker_.pollPlaybackReference();
                    else pollReferenceMonitor();
                }
                if (frontend_.aecRequested()) pollReferenceMonitor();
            } else {
                err = drainPlayback();
                pollReferenceMonitor();
                if (err == ESP_OK && frame.serial == serial_.load(std::memory_order_acquire)) {
                    err = speaker_.setEnabled(false);
                    playing_.store(false, std::memory_order_release);
                    done = err == ESP_OK;
                }
            }
        }
        if (err != ESP_OK) {
            active_.store(false, std::memory_order_release);
            playing_.store(false, std::memory_order_release);
            if (error_) error_(err);
        } else if (done && playback_done_ && frame.serial == serial_.load(std::memory_order_acquire)) {
            playback_done_(frame.identity);
        }
        if (!frame.end && err == ESP_OK && frontend_.aecRequested() &&
            running_.load(std::memory_order_acquire) && aec_error_.load() == ESP_OK &&
            frame.serial == serial_.load(std::memory_order_acquire)) {
            batch_work_us = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
                static_cast<uint64_t>(batch_work_us) + elapsedUs(frame_started)));
            ++batch_frames;
            if (batch_work_us >= kPlaybackWorkBudgetUs || batch_frames >= kPlaybackFrameBudget) {
                // 完整 PCM 帧及回调已处理，此处不持有扬声器/流锁，也不借用 DMA 缓冲。
                // 实际耗时包含 I2S 等待。
                bool current_scope = false;
                {
                    Lock lock(speaker_mutex_);
                    current_scope = running_.load(std::memory_order_acquire) && aec_error_.load() == ESP_OK &&
                        frame.serial == serial_.load(std::memory_order_acquire);
                    if (current_scope) playback_work_.budget_waits.fetch_add(1, std::memory_order_relaxed);
                }
                if (current_scope) vTaskDelay(1);
                batch_work_us = batch_frames = 0;
            }
        } else batch_work_us = batch_frames = 0;
    }
}

esp_err_t AudioService::drainPlayback()
{
    const esp_err_t err = speaker_.drain(100, [](void *context) {
        static_cast<AudioService *>(context)->serviceAecReferenceBeforePlayback();
    }, this);
    if (err == ESP_OK) playback_reference_drained_ = true;
    return err;
}

AudioService::Metrics AudioService::metrics() const
{
    return {rms_.load(std::memory_order_relaxed), peak_.load(std::memory_order_relaxed),
            clipped_.load(std::memory_order_relaxed),
            playback_queue_ ? static_cast<uint32_t>(uxQueueMessagesWaiting(playback_queue_)) : 0};
}

AudioService::PlaybackProgress AudioService::playbackProgress() const
{
    PlaybackProgress result{};
    const uint32_t before = playback_work_.sequence.load(std::memory_order_acquire);
    const uint32_t serial = playback_work_.serial.load(std::memory_order_relaxed);
    result.identity = {playback_work_.epoch.load(std::memory_order_relaxed),
                       playback_work_.turn.load(std::memory_order_relaxed),
                       playback_work_.generation.load(std::memory_order_relaxed)};
    result.stream_id = playback_work_.stream.load(std::memory_order_relaxed);
    const auto progress = speaker_.playbackProgress();
    result.completed_samples = progress.completed_samples;
    result.playing = playing_.load(std::memory_order_acquire);
    result.coherent = !(before & 1) && before == playback_work_.sequence.load(std::memory_order_acquire) &&
        serial == serial_.load(std::memory_order_acquire) && serial == progress.serial && progress.coherent;
    result.available = result.coherent && progress.available;
    return result;
}

void AudioService::logPlaybackReference()
{
    if (!speaker_mutex_) return;
    // 仅在用户请求快照时执行：音频热路径不分配内存，
    // 也不把四条直方图记录放到控制器栈上。
    std::unique_ptr<AcousticSnapshot, decltype(&heap_caps_free)> acoustic_snapshot(
        reference_monitor_ ? static_cast<AcousticSnapshot *>(heap_caps_calloc(
            1, sizeof(AcousticSnapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr,
        &heap_caps_free);
    if (acoustic_snapshot) new (acoustic_snapshot.get()) AcousticSnapshot{};
    PlaybackReference::Metrics reference{};
    Ics43434Microphone::DmaMetrics microphone{};
    uint64_t microphone_snapshot_us = 0;
    voiceaudio::ReferenceMonitor::Metrics monitor{};
    voiceaudio::ReferenceTimeline::Metrics timeline{};
    voiceaudio::AcousticDelayProbe::Metrics acoustic{};
    uint32_t max_poll_us = 0;
    bool monitor_enabled = false;
    bool acoustic_enabled = false;
    uint32_t capture_dropped = 0;
    uint32_t pump_max = 0, convert_max = 0, query_max = 0, acoustic_max = 0;
    uint32_t budget_yields = 0, stale_capture = 0, max_batch = 0;
    bool history_pending = false;
    {
        Lock lock(speaker_mutex_);
        pollReferenceMonitor();
        reference = speaker_.playbackReferenceMetrics();
        microphone = microphone_.dmaMetrics();
        microphone_snapshot_us = esp_timer_get_time();
        monitor_enabled = reference_monitor_ != nullptr;
        if (monitor_enabled) {
            monitor = reference_monitor_->metrics();
            timeline = reference_monitor_->timelineMetrics();
            acoustic = reference_monitor_->acousticMetrics();
            if (acoustic_snapshot) {
                acoustic_snapshot->count = reference_monitor_->acousticPlaybackStatsCount();
                for (size_t i = 0; i < acoustic_snapshot->count; ++i)
                    reference_monitor_->copyAcousticPlaybackStats(i, &acoustic_snapshot->scopes[i]);
            }
        }
        max_poll_us = reference_poll_max_us_;
        acoustic_enabled = reference_capture_queue_ != nullptr;
        capture_dropped = reference_capture_dropped_.load(std::memory_order_relaxed);
        pump_max = reference_pump_max_us_;
        convert_max = reference_convert_max_us_;
        query_max = reference_query_max_us_;
        acoustic_max = reference_acoustic_max_us_;
        budget_yields = reference_budget_yields_;
        stale_capture = reference_capture_stale_;
        max_batch = reference_max_batch_;
        history_pending = reference_history_pending_.load(std::memory_order_relaxed);
    }
    // 多行 UART 快照可能超过 ISR 队列 80 ms 的容量。
    // 打印前释放播放消费者的互斥锁。
    logPendingAecFailure();
    logPlaybackWork();
    if (frontend_.aecRequested()) {
        const auto idle = voice::audio_idle_guard::snapshot();
        ESP_LOGI(kTag, "audio_idle core=0 initialized=%u error=%d generation=%u gap_ticks=%u "
                 "observed_max_gap_ticks=%u waits=%u wait_ticks=%u max_wait_ticks=%u "
                 "timeouts=%u cancelled=%u tick_hz=%u scope=boot",
                 static_cast<unsigned>(idle.initialized), static_cast<int>(idle.init_error),
                 static_cast<unsigned>(idle.idle_generation), static_cast<unsigned>(idle.current_idle_gap_ticks),
                 static_cast<unsigned>(idle.max_idle_gap_ticks), static_cast<unsigned>(idle.cooperative_waits),
                 static_cast<unsigned>(idle.cooperative_wait_ticks), static_cast<unsigned>(idle.max_cooperative_wait_ticks),
                 static_cast<unsigned>(idle.timed_out_waits), static_cast<unsigned>(idle.cancelled_waits),
                 static_cast<unsigned>(configTICK_RATE_HZ));
    }
    frontend_.logSchedulingDiagnostics();
    Max98357aAudio::logPlaybackReferenceSnapshot(reference);
    logCaptureStartup(microphone, microphone_snapshot_us);
    logAecServiceDiagnostics(aecServiceSnapshot());
    ESP_LOGI(kTag, "capture_timeline enabled=%u valid=%u dma_blocks=%u sample_end=%llu "
             "dma_end_us=%llu overflow=%u invalid=%u overflow_event_samples=%llu",
             static_cast<unsigned>(microphone.timing_enabled),
             static_cast<unsigned>(microphone.timeline_valid),
             static_cast<unsigned>(microphone.completed_blocks),
             static_cast<unsigned long long>(microphone.completed_samples),
             static_cast<unsigned long long>(microphone.last_completion_us),
             static_cast<unsigned>(microphone.overflow_events),
             static_cast<unsigned>(microphone.invalid_events),
             static_cast<unsigned long long>(microphone.overflow_event_samples));
    if (!monitor_enabled) {
        ESP_LOGI(kTag, "reference_alignment enabled=0 aec_active=%u calibrated=0",
                 static_cast<unsigned>(frontend_.aecActive()));
        return;
    }
    ESP_LOGI(kTag, "reference_resampler ready=%u generation=%u playback_serial=%u in=%u out=%u "
             "warmup=%u unknown=%u gaps=%u generation_resets=%u overwritten=%u errors=%u max_poll_us=%u "
             "group_delay_us=1979 aec_active=%u calibrated=0",
             static_cast<unsigned>(monitor.ready), static_cast<unsigned>(monitor.generation),
             static_cast<unsigned>(monitor.playback_serial), static_cast<unsigned>(monitor.input_samples),
             static_cast<unsigned>(monitor.output_samples), static_cast<unsigned>(monitor.warmup_samples),
             static_cast<unsigned>(monitor.unknown_blocks), static_cast<unsigned>(monitor.gap_resets),
             static_cast<unsigned>(monitor.generation_resets), static_cast<unsigned>(monitor.overwritten_blocks),
             static_cast<unsigned>(monitor.processing_errors), static_cast<unsigned>(max_poll_us),
             static_cast<unsigned>(frontend_.aecActive()));
    ESP_LOGI(kTag, "reference_alignment enabled=1 available=%u healthy=%u mapped=%u unmapped=%u "
             "history=%u overwrites=%u mic_ppm=%ld tx_ppm=%ld anchor_delta_us=%ld "
             "clock_errors=%u clock_suspect_windows=%u max_clock_residual_us=%u "
             "probe_frames=%u probe_complete=%u probe_valid=%u probe_samples=%u aec_active=%u calibrated=0",
             static_cast<unsigned>(timeline.available), static_cast<unsigned>(timeline.healthy),
             static_cast<unsigned>(timeline.aligned_blocks), static_cast<unsigned>(timeline.unmapped_blocks),
             static_cast<unsigned>(timeline.blocks_available), static_cast<unsigned>(timeline.history_overwrites),
             static_cast<long>(timeline.microphone_rate_ppm), static_cast<long>(timeline.playback_rate_ppm),
             static_cast<long>(timeline.last_anchor_delta_us),
             static_cast<unsigned>(timeline.clock_errors),
             static_cast<unsigned>(timeline.clock_rate_suspect_windows),
             static_cast<unsigned>(timeline.max_clock_residual_us),
             static_cast<unsigned>(monitor.probe_frames), static_cast<unsigned>(monitor.probe_complete_frames),
             static_cast<unsigned>(monitor.probe_valid_samples), static_cast<unsigned>(monitor.probe_samples),
             static_cast<unsigned>(frontend_.aecActive()));
    ESP_LOGI(kTag, "acoustic_delay_probe enabled=%u attempts=%u accepted=%u rejected=%u "
             "last_accepted=%u delay_samples=%u correlation=%u margin=%u mic_ac_rms=%u "
             "ref_ac_rms=%u polarity=%d reject_reason=%s capture_dropped=%u calibrated=0",
             static_cast<unsigned>(acoustic_enabled),
             static_cast<unsigned>(acoustic.attempts), static_cast<unsigned>(acoustic.accepted),
             static_cast<unsigned>(acoustic.rejected), static_cast<unsigned>(acoustic.last_accepted),
             static_cast<unsigned>(acoustic.best_delay_samples),
             static_cast<unsigned>(acoustic.correlation_per_mille),
             static_cast<unsigned>(acoustic.peak_margin_per_mille),
             static_cast<unsigned>(acoustic.mic_ac_rms), static_cast<unsigned>(acoustic.reference_ac_rms),
             static_cast<int>(acoustic.correlation_polarity),
             voiceaudio::AcousticDelayProbe::rejectReasonName(acoustic.reject_reason),
             static_cast<unsigned>(capture_dropped));
    ESP_LOGI(kTag, "audio_task_stacks capture_free_min=%u playback_free_min=%u unit=bytes",
             static_cast<unsigned>(capture_stack_free_min_.load(std::memory_order_relaxed)),
             static_cast<unsigned>(playback_stack_free_min_.load(std::memory_order_relaxed)));
    ESP_LOGI(kTag, "reference_work total_max_us=%u pump_max_us=%u convert_batch_max_us=%u "
             "query_max_us=%u acoustic_max_us=%u max_batch=%u budget_yields=%u "
             "pending=%u stale_capture=%u optional_probes=%u catchup_passes=%u catchup_yields=%u "
             "timing=wall includes_pump=1",
             static_cast<unsigned>(max_poll_us), static_cast<unsigned>(pump_max),
             static_cast<unsigned>(convert_max), static_cast<unsigned>(query_max),
             static_cast<unsigned>(acoustic_max), static_cast<unsigned>(max_batch),
             static_cast<unsigned>(budget_yields), static_cast<unsigned>(history_pending),
             static_cast<unsigned>(stale_capture), static_cast<unsigned>(!frontend_.aecRequested()),
             static_cast<unsigned>(aec_catchup_passes_.load(std::memory_order_relaxed)),
             static_cast<unsigned>(aec_catchup_yields_.load(std::memory_order_relaxed)));
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    ESP_LOGI(kTag, "aec_pairing requested=1 active=%u error=%u queue=%u queue_peak=%u pairs=%u "
             "known_mute_pairs=%u last_reference_valid=%u wait_ms=80 extra_delay_ms=%u calibrated=0 "
             "max_pair_us=%u frontend_waits=%u",
             static_cast<unsigned>(frontend_.aecActive()), static_cast<unsigned>(aec_error_.load()),
             static_cast<unsigned>(aec_capture_queue_ ? uxQueueMessagesWaiting(aec_capture_queue_) : 0),
             static_cast<unsigned>(aec_capture_peak_.load()), static_cast<unsigned>(aec_pairs_.load()),
             static_cast<unsigned>(aec_initial_silence_.load()),
             static_cast<unsigned>(aec_last_valid_samples_.load()),
             static_cast<unsigned>(CONFIG_VOICE_AEC_REFERENCE_DELAY_MS),
             static_cast<unsigned>(aec_pair_max_us_.load()),
             static_cast<unsigned>(aec_frontend_waits_.load(std::memory_order_relaxed)));
#endif
    ESP_LOGI(kTag, "acoustic_playback_history available=%u count=%u capacity=4 newest_first=1",
             static_cast<unsigned>(acoustic_snapshot != nullptr),
             static_cast<unsigned>(acoustic_snapshot ? acoustic_snapshot->count : 0));
    if (acoustic_snapshot)
        for (size_t i = 0; i < acoustic_snapshot->count; ++i)
            logAcousticPlayback(acoustic_snapshot->scopes[i], i);
}

void AudioService::serviceAecReferenceBeforePlayback()
{
    const int64_t started = esp_timer_get_time();
    const uint32_t serial = serial_.load(std::memory_order_acquire);
    for (unsigned pass = 0; pass < 4; ++pass) {
        if (pass) aec_catchup_passes_.fetch_add(1, std::memory_order_relaxed);
        pollReferenceMonitor();
        if (!aec_target_waiting_.load(std::memory_order_relaxed) ||
            !running_.load(std::memory_order_acquire) ||
            serial != serial_.load(std::memory_order_acquire) || aec_error_.load() != ESP_OK) return;
        // 保留每次轮询 4 ms/8 块的预算；下一次 TX 等待前，
        // 所需的旧参考最多再获得三次机会。采集可抢占，AFE 与本任务同优先级并共享时间片。
        if (esp_timer_get_time() - started >= 16000) break;
    }
    aec_catchup_yields_.fetch_add(1, std::memory_order_relaxed);
}

void AudioService::pollReferenceMonitor()
{
    const int64_t started = esp_timer_get_time();
    const uint32_t service_gap = reference_last_service_us_ &&
        static_cast<uint64_t>(started) >= reference_last_service_us_
        ? static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
            static_cast<uint64_t>(started) - reference_last_service_us_)) : 0;
    reference_last_service_us_ = started;
    reference_service_gap_us_.store(service_gap, std::memory_order_relaxed);
    if (service_gap > reference_service_gap_max_us_.load(std::memory_order_relaxed))
        reference_service_gap_max_us_.store(service_gap, std::memory_order_relaxed);
    const auto pump = [&]() {
        const int64_t before = esp_timer_get_time();
        speaker_.pollPlaybackReference();
        reference_pump_max_us_ = std::max(reference_pump_max_us_,
            static_cast<uint32_t>(esp_timer_get_time() - before));
    };
    pump();
    if (!reference_monitor_) return;
    const auto microphone = microphone_.dmaMetrics();
    if (!microphone.completed_blocks) return;
    reference_monitor_->observeMicrophone(microphone.completed_samples,
        static_cast<int64_t>(microphone.last_completion_us), microphone.overflow_events,
        microphone.timeline_valid);
    publishReferenceClockFailure();
    if (frontend_.aecRequested() && microphone.completed_blocks && !microphone.timeline_valid)
        failAecReference(ESP_ERR_INVALID_STATE, "capture_timeline");
    if (!microphone.timing_enabled || !microphone.completed_blocks || !microphone.last_completion_us) return;
    PlaybackReference::Block block{};
    bool overwritten = false;
    const int64_t conversion_started = esp_timer_get_time();
    uint32_t converted = 0;
    // 保持样本顺序，同时把每次服务限制为八块/4 ms 软耗时预算。
    // 单块被抢占后可能超过软预算；指标保留该实际耗时。
    for (; converted < 8 && speaker_.copyNextPlaybackReference(
            reference_history_cursor_, &block, &overwritten);) {
        reference_monitor_->consume(block, overwritten);
        reference_history_cursor_ = block.history_sequence;
        ++converted;
        if (converted % 4 == 0) pump();
        if (esp_timer_get_time() - conversion_started >= 4000) break;
    }
    reference_max_batch_ = std::max(reference_max_batch_, converted);
    reference_convert_max_us_ = std::max(reference_convert_max_us_,
        static_cast<uint32_t>(esp_timer_get_time() - conversion_started));
    pump();
    const bool pending = speaker_.copyNextPlaybackReference(reference_history_cursor_, &block, &overwritten);
    reference_history_pending_.store(pending, std::memory_order_relaxed);
    reference_latest_sequence_ = speaker_.playbackReferenceMetrics().latest_history_sequence;
    publishReferenceClockFailure();
    const auto timeline = reference_monitor_->timelineHealthRange();
    reference_oldest_sample_ = timeline.oldest_reference_microphone_sample;
    reference_latest_sample_end_ = timeline.latest_reference_microphone_sample_end;
    reference_timeline_healthy_ = static_cast<uint32_t>(timeline.healthy) |
        (static_cast<uint32_t>(timeline.reference_range_available) << 1);
    reference_timeline_unmapped_ = timeline.unmapped_blocks;
    if (pending) ++reference_budget_yields_;
    const int64_t query_started = esp_timer_get_time();
    // AEC 模式下，必要的配对查询已经完成数字覆盖观测。
    // 不要重复查询，也不要在此实时消费者中执行可选的 81 延迟点声学搜索。
    // M/B 观测保持不变。
    const bool optional_probes = !frontend_.aecRequested();
    if (!pending && optional_probes) reference_monitor_->probe(microphone.completed_samples);
    reference_query_max_us_ = std::max(reference_query_max_us_,
        static_cast<uint32_t>(esp_timer_get_time() - query_started));
    // 同一持有者串行执行参考转换、查询和 AEC 配对。
    // 采集或 AFE 工作任务均不直接读取 ReferenceTimeline。
    pollAecReference(microphone.completed_samples);
    const int64_t acoustic_started = esp_timer_get_time();
    if (!pending && optional_probes && reference_capture_queue_) {
        ReferenceCaptureFrame frame{};
        for (size_t i = 0; i < 12 && xQueuePeek(reference_capture_queue_, &frame, 0) == pdTRUE; ++i) {
            // 等待 TX 完成及 FIR 支持范围就绪，保证原始采集生产者不阻塞；
            // 其固定队列中的数据永远不会变成上传数据。
            if (microphone.completed_samples < frame.first_sample + frame.pcm.size() + 1280) break;
            if (xQueueReceive(reference_capture_queue_, &frame, 0) != pdTRUE) break;
            if (frame.playback_serial != serial_.load(std::memory_order_acquire) ||
                microphone.completed_samples - frame.first_sample > 3200) {
                ++reference_capture_stale_;
                continue;
            }
            // 每次服务机会最多执行一次实际的 81 延迟点分析。
            // 限流的诊断帧可以丢弃，上传路径独立。
            if (reference_monitor_->probeAcousticDelay(frame.first_sample, frame.pcm.data())) break;
        }
    }
    reference_acoustic_max_us_ = std::max(reference_acoustic_max_us_,
        static_cast<uint32_t>(esp_timer_get_time() - acoustic_started));
    pump();
    reference_poll_max_us_ = std::max(reference_poll_max_us_,
        static_cast<uint32_t>(esp_timer_get_time() - started));
}

void AudioService::publishReferenceClockFailure()
{
    if (reference_clock_failure_ready_.load(std::memory_order_relaxed)) return;
    const auto failure = reference_monitor_->firstClockFailure();
    if (!failure.valid) return;
    reference_clock_failure_ = failure;
    reference_clock_failure_ready_.store(true, std::memory_order_release);
}

void AudioService::failAecReference(esp_err_t error, const char *reason)
{
    uint32_t expected = ESP_OK;
    if (!aec_error_.compare_exchange_strong(expected, error)) return;
    // 在 UART 可能延迟本任务前先做快照；
    // 采集回调失败时绝不读取其他持有者的临时区，也不获取其互斥锁。
    aec_failure_snapshot_.service = aecServiceSnapshot();
    aec_failure_snapshot_.microphone = microphone_.dmaMetrics();
    aec_failure_snapshot_.at_us = esp_timer_get_time();
    aec_failure_snapshot_.frontend_waits = aec_frontend_waits_.load(std::memory_order_relaxed);
    if (reference_clock_failure_ready_.load(std::memory_order_acquire))
        aec_failure_snapshot_.clock = reference_clock_failure_;
    frontend_.invalidate(error, reason);
    aec_failure_snapshot_.frontend = frontend_.failureSnapshot();
    ESP_LOGE(kTag, "aec_reference_failed reason=%s error=%s; automatic frontend stopped; raw manual remains available",
             reason, esp_err_to_name(error));
    aec_failure_log_pending_.store(true, std::memory_order_release);
}

void AudioService::logPendingAecFailure()
{
    if (!aec_failure_log_pending_.exchange(false, std::memory_order_acquire)) return;
    const auto &microphone = aec_failure_snapshot_.microphone;
    logCaptureStartup(microphone, aec_failure_snapshot_.at_us);
    logAecServiceDiagnostics(aec_failure_snapshot_.service);
    ESP_LOGI(kTag, "aec_handoff_failure frontend_waits=%u",
             static_cast<unsigned>(aec_failure_snapshot_.frontend_waits));
    const auto &clock = aec_failure_snapshot_.clock;
    ESP_LOGI(kTag, "aec_clock_failure valid=%u source=%s reason=%s sample_end=%llu "
             "observed_end_us=%lld residual_us=%lld observed_ppm=%ld window_us=%u bad_windows=%u",
             static_cast<unsigned>(clock.valid),
             voiceaudio::ReferenceTimeline::clockSourceName(clock.source),
             voiceaudio::ReferenceTimeline::clockFailureReasonName(clock.reason),
             static_cast<unsigned long long>(clock.sample_end),
             static_cast<long long>(clock.observed_end_us),
             static_cast<long long>(clock.residual_us), static_cast<long>(clock.observed_ppm),
             static_cast<unsigned>(clock.window_us), static_cast<unsigned>(clock.consecutive_bad_windows));
    const auto &frontend = aec_failure_snapshot_.frontend;
    if (frontend.valid)
        ESP_LOGI(kTag, "frontend_failure at_us=%llu error=%s queue=%u queue_peak=%u "
                 "feed_samples=%u output_samples=%u feed_max_us=%u fetch_max_us=%u",
                 static_cast<unsigned long long>(frontend.at_us), esp_err_to_name(frontend.error),
                 static_cast<unsigned>(frontend.queue_frames), static_cast<unsigned>(frontend.queue_peak),
                 static_cast<unsigned>(frontend.feed_samples), static_cast<unsigned>(frontend.output_samples),
                 static_cast<unsigned>(frontend.feed_max_us), static_cast<unsigned>(frontend.fetch_max_us));
    ESP_LOGI(kTag, "capture_failure valid=%u dma_sample_end=%llu dma_end_us=%llu overflow=%u invalid=%u",
             static_cast<unsigned>(microphone.timeline_valid),
             static_cast<unsigned long long>(microphone.completed_samples),
             static_cast<unsigned long long>(microphone.last_completion_us),
             static_cast<unsigned>(microphone.overflow_events), static_cast<unsigned>(microphone.invalid_events));
}

void AudioService::publishAecPairAttempt(uint64_t first_sample, uint64_t microphone_sample_end,
                                        bool has_head, bool queried, uint32_t valid_samples)
{
    const uint64_t now = esp_timer_get_time();
    const uint32_t words[] = {
        static_cast<uint32_t>(first_sample), static_cast<uint32_t>(first_sample >> 32),
        static_cast<uint32_t>(microphone_sample_end), static_cast<uint32_t>(microphone_sample_end >> 32),
        static_cast<uint32_t>((has_head ? 1 : 0) | (queried ? 2 : 0) |
            (reference_history_pending_.load(std::memory_order_relaxed) ? 4 : 0)),
        has_head && microphone_sample_end >= first_sample
            ? static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, microphone_sample_end - first_sample)) : 0,
        static_cast<uint32_t>(uxQueueMessagesWaiting(aec_capture_queue_)), valid_samples,
        static_cast<uint32_t>(now), static_cast<uint32_t>(now >> 32),
        aec_last_pair_us_.load(std::memory_order_relaxed),
        static_cast<uint32_t>(aec_last_query_first_), static_cast<uint32_t>(aec_last_query_first_ >> 32),
        aec_last_query_valid_, aec_last_query_begin_, aec_last_query_end_, aec_last_query_missing_,
        reference_history_cursor_, reference_latest_sequence_, reference_latest_sequence_ - reference_history_cursor_,
        static_cast<uint32_t>(reference_latest_sample_end_), static_cast<uint32_t>(reference_latest_sample_end_ >> 32),
        static_cast<uint32_t>(reference_oldest_sample_), static_cast<uint32_t>(reference_oldest_sample_ >> 32),
        reference_timeline_healthy_, reference_timeline_unmapped_
    };
    static_assert(sizeof(words) / sizeof(words[0]) == std::tuple_size<decltype(aec_attempt_words_)>::value,
                  "AEC diagnostic publication layout");
    // 播放侧是唯一写者；即使读取被拒绝，所有字段仍使用原子操作。
    // 顺序一致性使一次性读取在双核间安全。
    aec_attempt_sequence_.fetch_add(1);
    for (size_t i = 0; i < aec_attempt_words_.size(); ++i) aec_attempt_words_[i].store(words[i]);
    aec_attempt_sequence_.fetch_add(1);
}

AudioService::AecServiceSnapshot AudioService::aecServiceSnapshot() const
{
    AecServiceSnapshot snapshot;
    const uint32_t before = aec_attempt_sequence_.load();
    for (size_t i = 0; i < snapshot.words.size(); ++i) snapshot.words[i] = aec_attempt_words_[i].load();
    snapshot.coherent = before && !(before & 1) && before == aec_attempt_sequence_.load();
    // 另一核或被抢占的持有者正在发布时，绝不重试等待。
    // 不一致的数据直接丢弃，不作为证据展示。
    if (!snapshot.coherent) snapshot.words.fill(0);
    snapshot.pending = reference_history_pending_.load(std::memory_order_relaxed);
    snapshot.queue = aec_capture_queue_ ? uxQueueMessagesWaiting(aec_capture_queue_) : 0;
    snapshot.last_pair_us = aec_last_pair_us_.load(std::memory_order_relaxed);
    snapshot.service_gap_us = reference_service_gap_us_.load(std::memory_order_relaxed);
    snapshot.service_gap_max_us = reference_service_gap_max_us_.load(std::memory_order_relaxed);
    return snapshot;
}

void AudioService::logAecServiceDiagnostics(const AecServiceSnapshot &snapshot)
{
    const auto &words = snapshot.words;
    const uint64_t first = (static_cast<uint64_t>(words[1]) << 32) | words[0];
    const uint64_t dma_end = (static_cast<uint64_t>(words[3]) << 32) | words[2];
    const uint64_t at_us = (static_cast<uint64_t>(words[9]) << 32) | words[8];
    ESP_LOGI(kTag, "aec_service snapshot_coherent=%u attempt_at_us=%llu head=%u queried=%u "
             "head_first_sample=%llu attempt_dma_end=%llu head_age_samples=%u head_age_ms=%u "
             "attempt_pending=%u pending_now=%u attempt_queue=%u queue_now=%u valid_samples=%u "
             "previous_pair_us=%u last_pair_us=%u service_gap_us=%u service_gap_max_us=%u",
             static_cast<unsigned>(snapshot.coherent), static_cast<unsigned long long>(at_us),
             static_cast<unsigned>(words[4] & 1), static_cast<unsigned>((words[4] >> 1) & 1),
             static_cast<unsigned long long>(first), static_cast<unsigned long long>(dma_end),
             static_cast<unsigned>(words[5]), static_cast<unsigned>(words[5] / 16),
             static_cast<unsigned>((words[4] >> 2) & 1),
             static_cast<unsigned>(snapshot.pending),
             static_cast<unsigned>(words[6]),
             static_cast<unsigned>(snapshot.queue),
             static_cast<unsigned>(words[7]), static_cast<unsigned>(words[10]),
             static_cast<unsigned>(snapshot.last_pair_us),
             static_cast<unsigned>(snapshot.service_gap_us),
             static_cast<unsigned>(snapshot.service_gap_max_us));
    const uint64_t queried_first = (static_cast<uint64_t>(words[12]) << 32) | words[11];
    const uint64_t reference_end = (static_cast<uint64_t>(words[21]) << 32) | words[20];
    const uint64_t reference_first = (static_cast<uint64_t>(words[23]) << 32) | words[22];
    ESP_LOGI(kTag, "aec_reference_progress coherent=%u last_query_first=%llu last_query_valid=%u "
             "first_valid_offset=%u last_valid_end=%u missing_prefix=%u missing_suffix=%u missing_interior=%u "
             "missing_all=%u converted_sequence=%u latest_sequence=%u pending_blocks=%u range_available=%u "
             "oldest_reference_sample=%llu latest_reference_sample_end=%llu timeline_healthy=%u unmapped=%u",
             static_cast<unsigned>(snapshot.coherent), static_cast<unsigned long long>(queried_first),
             static_cast<unsigned>(words[13]), static_cast<unsigned>(words[14]), static_cast<unsigned>(words[15]),
             static_cast<unsigned>(words[16] & 1), static_cast<unsigned>((words[16] >> 1) & 1),
             static_cast<unsigned>((words[16] >> 2) & 1), static_cast<unsigned>((words[16] >> 3) & 1),
             static_cast<unsigned>(words[17]), static_cast<unsigned>(words[18]), static_cast<unsigned>(words[19]),
             static_cast<unsigned>((words[24] >> 1) & 1),
             static_cast<unsigned long long>(reference_first), static_cast<unsigned long long>(reference_end),
             static_cast<unsigned>(words[24] & 1), static_cast<unsigned>(words[25]));
}

void AudioService::pollAecReference(uint64_t microphone_sample_end)
{
    aec_target_waiting_.store(false, std::memory_order_relaxed);
    aec_frontend_waiting_.store(false, std::memory_order_relaxed);
    if (!frontend_.aecRequested() || !frontend_.aecActive() || aec_error_.load() != ESP_OK ||
        !aec_capture_queue_ || !aec_scratch_) return;
    const int64_t started = esp_timer_get_time();
    const auto fail_pair = [&](esp_err_t error, const char *reason) {
        aec_last_pair_us_.store(static_cast<uint32_t>(esp_timer_get_time() - started),
                               std::memory_order_relaxed);
        failAecReference(error, reason);
    };
    // 每次机会的工作量有界；追赶时绝不跳过输入顺序。
    for (unsigned i = 0; i < 4; ++i) {
        auto &frame = aec_scratch_->frame;
        if (xQueuePeek(aec_capture_queue_, &frame, 0) != pdTRUE) {
            publishAecPairAttempt(0, microphone_sample_end, false, false, 0);
            break;
        }
        publishAecPairAttempt(frame.first_sample, microphone_sample_end, true, false, 0);
        if (microphone_sample_end < frame.first_sample + frame.pcm.size() + kAecReferenceWaitSamples) break;
        if (microphone_sample_end - frame.first_sample > kAecCaptureFrames * kCaptureSamples) {
            fail_pair(ESP_ERR_TIMEOUT, "pair_age");
            break;
        }
        size_t valid_samples = 0;
        bool complete = false;
        aec_scratch_->valid_mask.fill(0);
        if (!frame.known_mute && frame.first_sample >= kAecReferenceDelaySamples)
            complete = reference_monitor_->copyReferenceFrame(
                frame.first_sample - kAecReferenceDelaySamples, frame.pcm.size(),
                0, 0, aec_scratch_->reference.data(),
                aec_scratch_->reference.size(), &valid_samples, aec_scratch_->valid_mask.data());
        if (frame.known_mute) {
            // 采集帧属于刚重置、尚未绑定的 TX 代次。
            // SD 为低，且此作用域尚未提交 PCM。
            std::fill(aec_scratch_->reference.begin(), aec_scratch_->reference.end(), 0);
            valid_samples = frame.pcm.size();
            complete = true;
        }
        aec_last_valid_samples_.store(valid_samples, std::memory_order_relaxed);
        aec_last_query_first_ = frame.first_sample;
        aec_last_query_valid_ = valid_samples;
        aec_last_query_begin_ = frame.pcm.size();
        aec_last_query_end_ = 0;
        if (frame.known_mute) {
            aec_last_query_begin_ = 0;
            aec_last_query_end_ = frame.pcm.size();
        } else {
            for (size_t offset = 0; offset < aec_scratch_->valid_mask.size(); ++offset) {
                if (!aec_scratch_->valid_mask[offset]) continue;
                aec_last_query_begin_ = std::min(aec_last_query_begin_, static_cast<uint32_t>(offset));
                aec_last_query_end_ = offset + 1;
            }
        }
        aec_last_query_missing_ = (aec_last_query_begin_ ? 1U : 0U) |
            (aec_last_query_end_ < frame.pcm.size() ? 2U : 0U) |
            (valid_samples && aec_last_query_end_ - aec_last_query_begin_ > valid_samples ? 4U : 0U) |
            (!valid_samples ? 8U : 0U);
        publishAecPairAttempt(frame.first_sample, microphone_sample_end, true, true, valid_samples);
        if (!complete) {
            // 较新的未转换 TX 尾部不能让已覆盖目标长期得不到服务。
            // 仅不完整目标等待有序转换，现有年龄/容量限制仍约束该等待。
            if (reference_history_pending_.load(std::memory_order_relaxed)) {
                aec_target_waiting_.store(true, std::memory_order_relaxed);
                break;
            }
            fail_pair(ESP_ERR_INVALID_STATE, "reference_gap");
            break;
        }
        // 保留原队首，直到下一持有者接受其完整麦克风/参考配对。
        // AFE 队列短暂满载属于正常背压，不是时间线中断；在后续服务调用重试。
        // 绝不阻塞采集，也不在持有 speaker_mutex_ 时等待。
        // 保持不变的采集容量与配对年龄保护共同限制持续停滞。
        const esp_err_t enqueue_result = frontend_.tryEnqueuePaired(
            frame.pcm.data(), aec_scratch_->reference.data(), frame.pcm.size());
        if (enqueue_result == ESP_ERR_NOT_FINISHED) {
            aec_frontend_waits_.fetch_add(1, std::memory_order_relaxed);
            aec_frontend_waiting_.store(true, std::memory_order_relaxed);
            break;
        }
        if (enqueue_result != ESP_OK) {
            fail_pair(enqueue_result, "frontend_pair");
            break;
        }
        // 播放持有者是唯一消费者；采集仅追加数据。
        if (xQueueReceive(aec_capture_queue_, &frame, 0) != pdTRUE) {
            fail_pair(ESP_ERR_INVALID_STATE, "pair_commit");
            break;
        }
        if (frame.known_mute) aec_initial_silence_.fetch_add(1, std::memory_order_relaxed);
        aec_pairs_.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
    aec_last_pair_us_.store(elapsed, std::memory_order_relaxed);
    if (elapsed > aec_pair_max_us_.load()) aec_pair_max_us_.store(elapsed);
}

