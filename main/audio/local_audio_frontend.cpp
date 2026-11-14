#include "local_audio_frontend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>

#include "app/memory_diagnostics.hpp"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "local_frontend";
// 吸收短暂的 ESP-SR feed 调度突发，不改变 AEC 的 200 ms 配对年龄保护。
// 这是有界背压缓冲，并不允许提交陈旧的麦克风帧。
constexpr size_t kInputFrames = 12;  // 240 ms 的有界工作任务交接队列。
constexpr uint32_t kWorkBudgetUs = 8000;
constexpr uint32_t kWorkBudgetBlocks = 4;
constexpr BaseType_t kAecFeedCore = 1;
constexpr BaseType_t kAecFetchCore = 1;

void observeMaximum(std::atomic<uint32_t> &value, uint32_t candidate)
{
    uint32_t previous = value.load(std::memory_order_relaxed);
    while (previous < candidate &&
           !value.compare_exchange_weak(previous, candidate, std::memory_order_relaxed)) {}
}

void observeStack(std::atomic<uint32_t> &value)
{
    const uint32_t available = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t previous = value.load(std::memory_order_relaxed);
    if (!previous || available < previous) value.store(available, std::memory_order_relaxed);
}

uint32_t elapsedUs(int64_t started)
{
    return static_cast<uint32_t>(std::clamp<int64_t>(
        esp_timer_get_time() - started, 0, UINT32_MAX));
}

int32_t diagnosticDbfs(float value)
{
    // PCM16 VAD 能量不能超过满量程。转换前拒绝非有限或不合理的 SDK 值；
    // 不可用不能当作静音。
    if (!std::isfinite(value) || value > 0.0f || value < -200.0f) return INT32_MIN;
    return static_cast<int32_t>(std::lround(value * 100.0f));
}

// 优先级 6 的工作任务在有界追赶批次后必须真正阻塞，让本核空闲任务运行。
// 只让出给同优先级 DSP 任务还不够。这是保守的实际耗时预算，不是 CPU 测量；
// 成功 fetch 可能在内部等待。长时间空调用计入尝试次数，
// 但其 SDK 等待不计入工作耗时。借用的 PCM 复制完成前不等待。
class WorkerBudget {
public:
    WorkerBudget(bool enabled, std::atomic<uint32_t> &waits, std::atomic<uint32_t> &batch_max,
                 std::atomic<uint32_t> &work_max)
        : enabled_(enabled), waits_(waits), batch_max_(batch_max), work_max_(work_max) {}

    void blocked(uint32_t wait_us)
    {
        if (enabled_ && wait_us >= kWorkBudgetUs) reset();
    }

    void complete(uint32_t wall_us)
    {
        if (!enabled_) return;
        ++blocks_;
        work_us_ = static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(work_us_) + wall_us, UINT32_MAX));
        observeMaximum(batch_max_, blocks_);
        observeMaximum(work_max_, work_us_);
        if (blocks_ < kWorkBudgetBlocks && work_us_ < kWorkBudgetUs) return;
        waits_.fetch_add(1, std::memory_order_relaxed);
        vTaskDelay(1);
        reset();
    }

    class Attempt {
    public:
        explicit Attempt(WorkerBudget &budget) : budget_(budget), started_(esp_timer_get_time()) {}
        ~Attempt()
        {
            const uint32_t elapsed = elapsedUs(started_);
            // 仅靠实际耗时不能证明任务阻塞；抢占也会延迟本应立即返回空的调用。
            // 所有尝试均保留在四次调用上限内，但较长的 SDK 空等待不计入工作量。
            budget_.complete(empty_wait_ && elapsed >= kWorkBudgetUs ? 0 : elapsed);
        }
        void emptyWait() { empty_wait_ = true; }
    private:
        WorkerBudget &budget_;
        int64_t started_;
        bool empty_wait_ = false;
    };

private:
    void reset() { blocks_ = work_us_ = 0; }
    bool enabled_;
    std::atomic<uint32_t> &waits_;
    std::atomic<uint32_t> &batch_max_;
    std::atomic<uint32_t> &work_max_;
    uint32_t blocks_ = 0;
    uint32_t work_us_ = 0;
};
}  // 命名空间

LocalAudioFrontend::~LocalAudioFrontend()
{
    if (stop() != ESP_OK) {
        ESP_LOGE(kTag, "AFE workers did not stop before destruction; models retained");
        std::abort();
    }
}

esp_err_t LocalAudioFrontend::setOutputCallback(OutputCallback callback)
{
    if (afe_ || input_queue_ || running_.load(std::memory_order_acquire))
        return ESP_ERR_INVALID_STATE;
    output_ = std::move(callback);
    return ESP_OK;
}

esp_err_t LocalAudioFrontend::start()
{
#ifndef CONFIG_VOICE_LOCAL_FRONTEND_ENABLED
    return ESP_OK;
#else
    if (afe_ || input_queue_) return ESP_ERR_INVALID_STATE;
    enabled_.store(true);
    ready_.store(false);
    speech_.store(false);
    aec_requested_.store(false, std::memory_order_release);
#if defined(CONFIG_VOICE_AEC_OBSERVATION_ENABLED) && CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    aec_requested_.store(true, std::memory_order_release);
#endif
    aec_active_.store(false, std::memory_order_release);
    last_error_.store(ESP_OK);
    failure_snapshot_ready_.store(false, std::memory_order_release);
    failure_snapshot_ = {};
    feed_samples_.store(0);
    output_samples_.store(0);
    output_frames_.store(0);
    queue_peak_.store(0);
    feed_max_us_.store(0);
    fetch_max_us_.store(0);
    feed_stack_free_.store(0);
    fetch_stack_free_.store(0);
    feed_chunk_samples_.store(0);
    fetch_chunk_samples_.store(0);
    vad_transitions_.store(0);
    feed_total_us_.store(0);
    output_blocks_.store(0);
    output_rms_.store(0);
    output_peak_.store(0);
    vad_thresholds_valid_.store(false);
    vad_energy_threshold_cdbfs_.store(INT32_MIN);
    vad_probability_threshold_milli_.store(-1);
    vad_input_energy_cdbfs_.store(INT32_MIN);
    vad_input_energy_peak_cdbfs_.store(INT32_MIN);
    vad_input_energy_frames_.store(0);
    vad_input_energy_invalid_frames_.store(0);
    microphone_rms_.store(0);
    microphone_peak_.store(0);
    microphone_mean_.store(0);
    microphone_ac_rms_.store(0);
    reference_rms_.store(0);
    reference_peak_.store(0);
    reference_mean_.store(0);
    reference_ac_rms_.store(0);
    feed_budget_waits_.store(0);
    fetch_budget_waits_.store(0);
    feed_batch_max_.store(0);
    fetch_batch_max_.store(0);
    feed_work_wall_max_us_.store(0);
    fetch_work_wall_max_us_.store(0);
    output_offset_ = 0;
    output_pending_speech_ = false;
    microphone_dc_blocker_.reset();
    microphone_dc_blocker_enabled_ = false;
    voice::memory::log("frontend_before_models");
    esp_err_t err = models_.load();
    if (err != ESP_OK) {
        fail(err, "model_load");
        return err;
    }
    voice::memory::log("frontend_models_ready");
    const bool use_aec = aecRequested();
    // 插话在扬声器活动期间消费 AFE 输出。SR AEC 有意省略非线性残余抑制，
    // D1/D2 因此使用全双工流水线，同时保留原有仅 C 模式的 SR 基线。
    bool full_duplex = false;
#if defined(CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED) && CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED
    full_duplex = use_aec;
#endif
    microphone_dc_blocker_enabled_ = full_duplex;
    const auto requested_type = full_duplex ? AFE_TYPE_FD : AFE_TYPE_SR;
    const auto requested_aec_mode = full_duplex ? AEC_MODE_FD_LOW_COST : AEC_MODE_SR_LOW_COST;
    const char *input_format = use_aec ? "MR" : "M";
    afe_config_t *config = afe_config_init(input_format, models_.models(), requested_type, AFE_MODE_LOW_COST);
    if (!config) {
        fail(ESP_ERR_NO_MEM, "afe_config");
        release();
        return ESP_ERR_NO_MEM;
    }
    // 绝不能从选定的 VADNet 悄悄回退到 WebRTC VAD。
    config->vad_model_name = const_cast<char *>(models_.vadModelName());
    config->vad_init = true;
    config->vad_mode = VAD_MODE_0;
    config->vad_min_speech_ms = 128;
    config->vad_min_noise_ms = 128;
    config->vad_delay_ms = 128;
    config->aec_init = use_aec;
    if (use_aec) config->aec_mode = requested_aec_mode;
    if (full_duplex) {
        config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
        // 插话必须在播放期间观测处理后的麦克风活动，
        // 不能屏蔽全部 VAD 事件，也不能改为取参考信号。
        config->vad_mute_playback = false;
        config->fixed_output_channel = true;
        config->output_playback_channel = false;
    }
    config->se_init = false;
    config->ns_init = false;
    config->agc_init = false;
    config->wakenet_init = false;
    config->wakenet_model_name = nullptr;
    config->wakenet_model_name_2 = nullptr;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // 保留 ESP-SR 默认内部环容量（2.6.1 中为 50 个 AFE 帧）。
    // 它须同时容纳 VAD 的语音+延迟历史及未读取 PCM：八个 32 ms 帧
    // 仅够 256 ms 历史，因此即使 fetch 已消费所有待处理帧，下一次 feed 仍会失败。
    // 该内部环独立于下面 12 个 20 ms 帧组成的有界输入队列。
    config->afe_linear_gain = 1.0f;
    config->fixed_first_channel = true;
    afe_config_t *original_config = config;
    afe_config_t *checked = afe_config_check(config);
    if (!checked) {
        afe_config_free(original_config);
        fail(ESP_ERR_INVALID_STATE, "afe_config_check");
        release();
        return ESP_ERR_INVALID_STATE;
    }
    if (checked != original_config) {
        afe_config_free(checked);
        afe_config_free(original_config);
        fail(ESP_ERR_INVALID_STATE, "afe_config_check_owner");
        release();
        return ESP_ERR_INVALID_STATE;
    }
    config = checked;
    const auto &format = config->pcm_config;
    if (format.sample_rate != 16000 || format.total_ch_num != (use_aec ? 2 : 1) ||
        format.mic_num != 1 || !format.mic_ids || format.mic_ids[0] != 0 ||
        format.ref_num != (use_aec ? 1 : 0) ||
        (use_aec && (!format.ref_ids || format.ref_ids[0] != 1)) ||
        config->aec_init != use_aec || (use_aec &&
        (config->aec_mode != requested_aec_mode || config->aec_filter_length <= 0 ||
         config->aec_nlp_level < AEC_NLP_LEVEL_NORMAL || config->aec_nlp_level > AEC_NLP_LEVEL_VERYAGGR)) ||
        (full_duplex && (config->aec_nlp_level != AEC_NLP_LEVEL_VERYAGGR ||
                        config->vad_mute_playback || !config->fixed_output_channel ||
                        config->output_playback_channel)) ||
        !config->vad_init || !config->vad_model_name ||
        config->se_init || config->ns_init || config->agc_init || config->wakenet_init ||
        config->afe_type != requested_type || config->afe_mode != AFE_MODE_LOW_COST) {
        afe_config_free(config);
        fail(ESP_ERR_INVALID_STATE, "afe_config_contract");
        release();
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(kTag, "AFE profile=%s full_duplex=%u aec_profile=%s nlp_requested=%s; "
             "actual DSP pipeline follows",
             full_duplex ? "barge_in" : "speech_recognition", static_cast<unsigned>(full_duplex),
             use_aec ? (full_duplex ? "FD_LOW_COST" : "SR_LOW_COST") : "off",
             full_duplex ? "VERYAGGR" : "SDK_default");
    ESP_LOGI(kTag, "mic_dc_blocker=%s coefficient=32604/32768 state=Q16 "
             "cutoff_hz=12.78 prefilter_input_stats=1 reference_unchanged=1",
             microphone_dc_blocker_enabled_ ? "on" : "off");
    ESP_LOGI(kTag, "AFE checked: format=%s rate=%d channels=%d mic=%d ref=%d "
             "type=%d mode=%d AEC=%u aec_mode=%d filter=%d nlp=%d NS=%u AGC=%u WakeNet=%u "
             "vad_mute_playback=%u fixed_output=%u output_reference=%u "
             "sdk_config_core=%d sdk_config_priority=%d",
             input_format, format.sample_rate, format.total_ch_num, format.mic_num, format.ref_num,
             static_cast<int>(config->afe_type), static_cast<int>(config->afe_mode),
             static_cast<unsigned>(config->aec_init), static_cast<int>(config->aec_mode),
             config->aec_filter_length, static_cast<int>(config->aec_nlp_level),
             static_cast<unsigned>(config->ns_init), static_cast<unsigned>(config->agc_init),
             static_cast<unsigned>(config->wakenet_init), static_cast<unsigned>(config->vad_mute_playback),
             static_cast<unsigned>(config->fixed_output_channel), static_cast<unsigned>(config->output_playback_channel),
             config->afe_perferred_core, config->afe_perferred_priority);
    const int afe_ring_frames = config->afe_ringbuf_size;
    iface_ = esp_afe_handle_from_config(config);
    if (iface_) afe_ = iface_->create_from_config(config);
    afe_config_free(config);
    if (!iface_ || !afe_) {
        fail(ESP_ERR_NO_MEM, "afe_create");
        release();
        return ESP_ERR_NO_MEM;
    }
    // 仅在启动线程独占 AFE 时查询。
    // 这些观测不会改变 SDK 阈值，也不增加 fetch 期间的取值调用。
    const float energy_threshold = iface_->get_vad_energy_threshold
        ? iface_->get_vad_energy_threshold(afe_) : std::numeric_limits<float>::quiet_NaN();
    const float probability_threshold = iface_->get_vadnet_threshold
        ? iface_->get_vadnet_threshold(afe_) : std::numeric_limits<float>::quiet_NaN();
    const bool energy_threshold_valid = std::isfinite(energy_threshold) &&
                                        energy_threshold >= -100.0f && energy_threshold <= 0.0f;
    const bool probability_threshold_valid = std::isfinite(probability_threshold) &&
                                             probability_threshold >= 0.5f && probability_threshold < 1.0f;
    if (energy_threshold_valid) vad_energy_threshold_cdbfs_.store(diagnosticDbfs(energy_threshold));
    if (probability_threshold_valid)
        vad_probability_threshold_milli_.store(static_cast<int32_t>(std::lround(probability_threshold * 1000.0f)));
    vad_thresholds_valid_.store(energy_threshold_valid && probability_threshold_valid);
    ESP_LOGI(kTag, "VAD actual: energy_threshold_valid=%u energy_threshold_dbfs=%.2f "
             "probability_threshold_valid=%u probability_threshold=%.3f window_ms=128 source=AFE_getters",
             static_cast<unsigned>(energy_threshold_valid), static_cast<double>(energy_threshold),
             static_cast<unsigned>(probability_threshold_valid), static_cast<double>(probability_threshold));
    voice::memory::log("frontend_afe_ready");
    const int feed_samples = iface_->get_feed_chunksize(afe_);
    const int fetch_samples = iface_->get_fetch_chunksize(afe_);
    void *adapter_storage = heap_caps_malloc(use_aec ? sizeof(PairedFeedAdapter) : sizeof(FeedAdapter),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!adapter_storage) {
        fail(ESP_ERR_NO_MEM, "afe_feed_adapter");
        release();
        return ESP_ERR_NO_MEM;
    }
    if (use_aec) paired_feed_adapter_ = new (adapter_storage) PairedFeedAdapter();
    else feed_adapter_ = new (adapter_storage) FeedAdapter();
    if (full_duplex) {
        void *history_storage = heap_caps_malloc(sizeof(voiceaudio::SourceSignalHistory),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!history_storage) {
            fail(ESP_ERR_NO_MEM, "source_history");
            release();
            return ESP_ERR_NO_MEM;
        }
        source_history_ = new (history_storage) voiceaudio::SourceSignalHistory();
    }
    if (iface_->get_samp_rate(afe_) != 16000 ||
        iface_->get_feed_channel_num(afe_) != (use_aec ? 2 : 1) ||
        iface_->get_fetch_channel_num(afe_) != 1 || feed_samples <= 0 || fetch_samples <= 0 ||
        (use_aec ? !paired_feed_adapter_->configure(static_cast<size_t>(feed_samples)) :
                   !feed_adapter_->configure(static_cast<size_t>(feed_samples))) ||
        !output_adapter_.configure(320)) {
        fail(ESP_ERR_INVALID_SIZE, "afe_format");
        release();
        return ESP_ERR_INVALID_SIZE;
    }
    feed_chunk_samples_.store(feed_samples);
    fetch_chunk_samples_.store(fetch_samples);
    aec_active_.store(use_aec, std::memory_order_release);
    input_queue_ = xQueueCreateWithCaps(kInputFrames, sizeof(InputFrame),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    feed_done_ = xSemaphoreCreateBinary();
    fetch_done_ = xSemaphoreCreateBinary();
    if (!input_queue_ || !feed_done_ || !fetch_done_) {
        fail(ESP_ERR_NO_MEM, "frontend_buffers");
        release();
        return ESP_ERR_NO_MEM;
    }
    // 即使 MORE_PSRAM 模式，AFE/模型分配仍可能消耗内部 RAM。
    // 启动时为 WebSocket/Wi-Fi 和 DMA 预留预算。
    // 此预检查基于当时状态，不能保证后续分配成功。
    if (!voice::memory::hasInternalBudget(voice::memory::capture(), 48 * 1024, 12 * 1024)) {
        voice::memory::log("frontend_internal_budget_failed");
        fail(ESP_ERR_NO_MEM, "internal_budget");
        release();
        voice::memory::log("frontend_budget_released");
        return ESP_ERR_NO_MEM;
    }
    running_.store(true, std::memory_order_release);
    // 创建工作任务前先发布状态，使工作任务立即失败时可清除 ready，
    // 不会被随后返回的 start() 覆盖失败状态。
    ready_.store(true, std::memory_order_release);
    // 两个应用 AFE 工作任务与采集保留在 CPU1，为 CPU0 的播放/参考和下行解码留空间。
    // 两者仍为优先级 6，并保留有界追赶等待；采集保持优先级 7。
    // 上面的 SDK 优先核设置只是配置，不是实际任务观测。
    // 原有单麦克风路径保持现有策略。
    const auto create_worker = [this, use_aec](void (*entry)(void *), const char *name,
                                               TaskHandle_t *handle) {
        if (use_aec)
            return xTaskCreatePinnedToCoreWithCaps(entry, name, 8192, this, 6, handle,
                                                  entry == feedEntry ? kAecFeedCore : kAecFetchCore,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return xTaskCreateWithCaps(entry, name, 8192, this, 5, handle,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    };
    if (create_worker(fetchEntry, "afe_fetch", &fetch_task_) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    } else {
        feed_exited_.store(false, std::memory_order_release);
        if (create_worker(feedEntry, "afe_feed", &feed_task_) != pdPASS) {
            feed_exited_.store(true, std::memory_order_release);
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        fail(err, "frontend_task");
        stop();
        return err;
    }
    voice::memory::log("frontend_workers_ready");
    iface_->print_pipeline(afe_);
    ESP_LOGI(kTag, "AFE/VAD ready: model=%s format=%s feed=%d fetch=%d; "
             "manual_uplink=raw_pcm auto_uplink=afe_pcm; NS=off AGC=off AEC=%s; queue=240ms; afe_ring_frames=%d",
             models_.vadModelName(), input_format, feed_samples, fetch_samples,
             use_aec ? "observation" : "off", afe_ring_frames);
    logSchedulingDiagnostics();
    return ESP_OK;
#endif
}

esp_err_t LocalAudioFrontend::enqueue(const int16_t *samples, size_t count)
{
    if (!enabled_.load(std::memory_order_relaxed)) return ESP_OK;
    if (!running_.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (aecRequested()) {
        fail(ESP_ERR_INVALID_STATE, "aec_unpaired_input");
        return ESP_ERR_INVALID_STATE;
    }
    if (!samples || count != 320) {
        fail(ESP_ERR_INVALID_SIZE, "capture_frame");
        return ESP_ERR_INVALID_SIZE;
    }
    InputFrame frame{};
    std::copy_n(samples, count, frame.microphone.data());
    if (xQueueSend(input_queue_, &frame, 0) != pdTRUE) {
        // 丢弃输入块会伪造 VAD 的样本时间线。
        // 因此明确停止观测，独立选择的原始手动路径仍可运行。
        fail(ESP_ERR_TIMEOUT, "frontend_backlog");
        return ESP_ERR_TIMEOUT;
    }
    observeMaximum(queue_peak_, uxQueueMessagesWaiting(input_queue_));
    return ESP_OK;
}

esp_err_t LocalAudioFrontend::enqueuePaired(const int16_t *microphone,
                                           const int16_t *reference, size_t count)
{
    const esp_err_t result = tryEnqueuePaired(microphone, reference, count);
    if (result == ESP_ERR_NOT_FINISHED) {
        fail(ESP_ERR_TIMEOUT, "frontend_backlog");
        return ESP_ERR_TIMEOUT;
    }
    return result;
}

esp_err_t LocalAudioFrontend::tryEnqueuePaired(const int16_t *microphone,
                                              const int16_t *reference, size_t count)
{
    if (!running_.load(std::memory_order_acquire) || !aecRequested() || !aecActive())
        return ESP_ERR_INVALID_STATE;
    if (!microphone || !reference || count != 320) {
        fail(ESP_ERR_INVALID_SIZE, "aec_pair_frame");
        return ESP_ERR_INVALID_SIZE;
    }
    // 只有配对生产者增加帧，feed 工作任务只会释放空间。
    // 避免复制仍须由原持有者保留并重试的帧。
    if (uxQueueMessagesWaiting(input_queue_) >= kInputFrames) return ESP_ERR_NOT_FINISHED;
    InputFrame frame{};
    uint64_t microphone_energy = 0;
    uint64_t reference_energy = 0;
    int64_t microphone_sum = 0;
    int64_t reference_sum = 0;
    uint32_t microphone_peak = 0;
    uint32_t reference_peak = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t microphone_sample = microphone[i];
        const int32_t reference_sample = reference[i];
        frame.microphone[i] = microphone[i];
        frame.reference[i] = reference[i];
        microphone_energy += static_cast<int64_t>(microphone_sample) * microphone_sample;
        reference_energy += static_cast<int64_t>(reference_sample) * reference_sample;
        microphone_sum += microphone_sample;
        reference_sum += reference_sample;
        microphone_peak = std::max(microphone_peak, static_cast<uint32_t>(
            microphone_sample < 0 ? -microphone_sample : microphone_sample));
        reference_peak = std::max(reference_peak, static_cast<uint32_t>(
            reference_sample < 0 ? -reference_sample : reference_sample));
    }
    frame.paired = true;
    if (xQueueSend(input_queue_, &frame, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    microphone_rms_.store(static_cast<uint32_t>(std::sqrt(
        static_cast<double>(microphone_energy) / count)), std::memory_order_relaxed);
    microphone_peak_.store(microphone_peak, std::memory_order_relaxed);
    microphone_mean_.store(static_cast<int32_t>(microphone_sum / static_cast<int64_t>(count)),
                           std::memory_order_relaxed);
    microphone_ac_rms_.store(static_cast<uint32_t>(std::sqrt(
        static_cast<double>(count * microphone_energy - microphone_sum * microphone_sum) /
        (count * count))), std::memory_order_relaxed);
    reference_rms_.store(static_cast<uint32_t>(std::sqrt(
        static_cast<double>(reference_energy) / count)), std::memory_order_relaxed);
    reference_peak_.store(reference_peak, std::memory_order_relaxed);
    reference_mean_.store(static_cast<int32_t>(reference_sum / static_cast<int64_t>(count)),
                          std::memory_order_relaxed);
    reference_ac_rms_.store(static_cast<uint32_t>(std::sqrt(
        static_cast<double>(count * reference_energy - reference_sum * reference_sum) /
        (count * count))), std::memory_order_relaxed);
    observeMaximum(queue_peak_, uxQueueMessagesWaiting(input_queue_));
    return ESP_OK;
}

void LocalAudioFrontend::fail(esp_err_t error, const char *reason)
{
    ready_.store(false, std::memory_order_release);
    speech_.store(false, std::memory_order_release);
    aec_active_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    esp_err_t expected = ESP_OK;
    if (last_error_.compare_exchange_strong(expected, error)) {
        failure_snapshot_.valid = true;
        failure_snapshot_.error = error;
        failure_snapshot_.at_us = esp_timer_get_time();
        failure_snapshot_.queue_frames = input_queue_ ? uxQueueMessagesWaiting(input_queue_) : 0;
        failure_snapshot_.queue_peak = std::max(failure_snapshot_.queue_frames,
            queue_peak_.load(std::memory_order_relaxed));
        failure_snapshot_.feed_samples = feed_samples_.load(std::memory_order_relaxed);
        failure_snapshot_.output_samples = output_samples_.load(std::memory_order_relaxed);
        failure_snapshot_.feed_max_us = feed_max_us_.load(std::memory_order_relaxed);
        failure_snapshot_.fetch_max_us = fetch_max_us_.load(std::memory_order_relaxed);
        failure_snapshot_ready_.store(true, std::memory_order_release);
        ESP_LOGE(kTag, "AFE/VAD unavailable: reason=%s error=%s; "
                 "automatic mode must stop; raw manual path remains available", reason, esp_err_to_name(error));
    }
}

void LocalAudioFrontend::invalidate(esp_err_t error, const char *reason)
{
    if (enabled_.load(std::memory_order_acquire))
        fail(error == ESP_OK ? ESP_FAIL : error, reason ? reason : "microphone_unavailable");
}

void LocalAudioFrontend::feedEntry(void *argument)
{
    auto *self = static_cast<LocalAudioFrontend *>(argument);
    self->feedLoop();
    self->feed_exited_.store(true, std::memory_order_release);
    xSemaphoreGive(self->feed_done_);
    vTaskDeleteWithCaps(nullptr);
}

void LocalAudioFrontend::fetchEntry(void *argument)
{
    auto *self = static_cast<LocalAudioFrontend *>(argument);
    self->fetchLoop();
    xSemaphoreGive(self->fetch_done_);
    vTaskDeleteWithCaps(nullptr);
}

void LocalAudioFrontend::feedLoop()
{
    InputFrame frame;
    uint32_t stack_probe_frames = 49;
    WorkerBudget budget(aecRequested(), feed_budget_waits_, feed_batch_max_, feed_work_wall_max_us_);
    while (running_.load(std::memory_order_acquire)) {
        const int64_t wait_started = esp_timer_get_time();
        const BaseType_t received = xQueueReceive(input_queue_, &frame, pdMS_TO_TICKS(20));
        if (received != pdTRUE) {
            budget.blocked(elapsedUs(wait_started));
            continue;
        }
        // 成功接收的实际耗时可能包含抢占，而非真实队列等待。
        // 从工作耗时中排除此部分，但保守保留上一批次，避免追赶反复清空预算。
        WorkerBudget::Attempt attempt(budget);
        const bool use_aec = aecRequested();
        if (frame.paired != use_aec) {
            fail(ESP_ERR_INVALID_STATE, "afe_input_channels");
            break;
        }
        // 该队列帧现由 feed 持有。接受后只应用一次，
        // 防止生产者在满队列重试时重复滤波。
        // 诊断仍针对原始配对输入，R 保持不变。
        voiceaudio::SourceSignalHistory::Statistics raw_statistics;
        if (source_history_) raw_statistics = voiceaudio::SourceSignalHistory::measure(frame.microphone.data());
        if (microphone_dc_blocker_enabled_)
            microphone_dc_blocker_.process(frame.microphone.data(), frame.microphone.size());
        if (source_history_) {
            const auto dc_statistics = voiceaudio::SourceSignalHistory::measure(frame.microphone.data());
            const auto reference_statistics = voiceaudio::SourceSignalHistory::measure(frame.reference.data());
            source_history_->append(raw_statistics, dc_statistics, reference_statistics);
        }
        const auto consume = [this](const int16_t *pcm, size_t count) {
                if (!running_.load(std::memory_order_acquire)) return false;
                const int64_t started = esp_timer_get_time();
                const int result = iface_->feed(afe_, pcm);
                const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
                observeMaximum(feed_max_us_, elapsed);
                feed_total_us_.fetch_add(elapsed, std::memory_order_relaxed);
                // 固定版本 ESP-SR 2.6.1 的单麦克风 afe_feed 写入处理后的单声道环，
                // 并返回 sr_rb_write 的字节数。MR 输入消费 2*count 个样本，
                // 但成功时仍返回 count*2 字节（已核对 lib/esp32s3/libesp_audio_front_end.a）。
                if (result != static_cast<int>(count * sizeof(int16_t))) {
                    fail(ESP_FAIL, "afe_feed");
                    return false;
                }
                feed_samples_.fetch_add(count, std::memory_order_relaxed);
                if (source_history_) source_history_->commit(count);
                return true;
            };
        const bool consumed = use_aec ? paired_feed_adapter_->append(
            frame.microphone.data(), frame.reference.data(), frame.microphone.size(), consume) :
            feed_adapter_->append(frame.microphone.data(), frame.microphone.size(), consume);
        if (!consumed) break;
        // 扫描 PSRAM 栈代价较高。首次消费块时取样用于短程诊断，
        // 此后每 50 块取样一次。
        if (++stack_probe_frames >= 50) {
            stack_probe_frames = 0;
            observeStack(feed_stack_free_);
        }
    }
}

void LocalAudioFrontend::fetchLoop()
{
    bool observed_state = false;
    uint32_t stack_probe_frames = 49;
    int64_t last_output_us = esp_timer_get_time();
    WorkerBudget budget(aecRequested(), fetch_budget_waits_, fetch_batch_max_, fetch_work_wall_max_us_);
    while (running_.load(std::memory_order_acquire) || !feed_exited_.load(std::memory_order_acquire)) {
        WorkerBudget::Attempt attempt(budget);
        const int64_t started = esp_timer_get_time();
        afe_fetch_result_t *result = iface_->fetch_with_delay(afe_, pdMS_TO_TICKS(50));
        // 包含等待一个块的有界耗时，不是 CPU 利用率。
        observeMaximum(fetch_max_us_, static_cast<uint32_t>(esp_timer_get_time() - started));
        const bool empty_result = !result || result->ret_value == ESP_FAIL ||
                                  result->ret_value == ESP_ERR_TIMEOUT;
        if (empty_result) attempt.emptyWait();
        if (!running_.load(std::memory_order_acquire)) continue;
        // ESP-SR 根据流水线不同，会用 nullptr/ESP_FAIL 表示空超时等待。
        // 50 ms 调度间隔并不表示 VAD 永久错误。
        if (empty_result) {
            if (feed_samples_.load(std::memory_order_relaxed) &&
                esp_timer_get_time() - last_output_us > 2000000)
                fail(ESP_ERR_TIMEOUT, "afe_output_stalled");
            continue;
        }
        if (result->ret_value != ESP_OK || !result->data || result->data_size <= 0 ||
            result->data_size % sizeof(int16_t) != 0 ||
            static_cast<size_t>(result->data_size) > fetch_chunk_samples_.load() * sizeof(int16_t)) {
            fail(ESP_FAIL, "afe_fetch");
            continue;
        }
        last_output_us = esp_timer_get_time();
        const size_t count = result->data_size / sizeof(int16_t);
        uint64_t energy = 0;
        uint32_t peak = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t sample = result->data[i];
            energy += static_cast<int64_t>(sample) * sample;
            peak = std::max(peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
        }
        output_rms_.store(static_cast<uint32_t>(std::sqrt(static_cast<double>(energy) / count)),
                          std::memory_order_relaxed);
        output_peak_.store(peak, std::memory_order_relaxed);
        // data_volume 是 SDK 实际的 VAD 输入窗口，
        // 不同于单个输出 PCM 块的 RMS。本路径不打印、不分配。
        const int32_t vad_energy = diagnosticDbfs(result->data_volume);
        vad_input_energy_cdbfs_.store(vad_energy, std::memory_order_relaxed);
        if (vad_energy != INT32_MIN) {
            if (vad_energy > vad_input_energy_peak_cdbfs_.load(std::memory_order_relaxed))
                vad_input_energy_peak_cdbfs_.store(vad_energy, std::memory_order_relaxed);
            vad_input_energy_frames_.fetch_add(1, std::memory_order_relaxed);
        } else {
            vad_input_energy_invalid_frames_.fetch_add(1, std::memory_order_relaxed);
        }
        output_samples_.fetch_add(count, std::memory_order_relaxed);
        output_blocks_.fetch_add(1, std::memory_order_relaxed);
        const bool speech = result->vad_state == VAD_SPEECH;
        // 应用转发语音及静音期间的每个 data 块，并自行保留触发前环。
        // 此处不要拼接 vad_cache：API 没有提供可安全定位它的输出偏移/重叠元数据。
        // 每次复制到 20 ms 边界，避免 512 样本 VAD 状态转换
        // 给适配器中已经缓存的部分帧贴上错误标签。
        size_t consumed = 0;
        while (consumed < count && running_.load(std::memory_order_acquire)) {
            const size_t copied = std::min(count - consumed,
                                           320 - output_adapter_.pendingSamples());
            output_pending_speech_ = output_pending_speech_ || speech;
            const bool appended = output_adapter_.append(result->data + consumed, copied,
                [this](const int16_t *pcm, size_t samples) {
                    if (!running_.load(std::memory_order_acquire)) return false;
                    if (output_) output_(pcm, samples, output_offset_, output_pending_speech_);
                    output_offset_ += samples;
                    output_pending_speech_ = false;
                    output_frames_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                });
            if (!appended) break;
            consumed += copied;
        }
        const bool previous = speech_.exchange(speech, std::memory_order_acq_rel);
        if (!observed_state || previous != speech) {
            vad_transitions_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGI(kTag, "vad=%s output_samples=%u",
                     speech ? "speech" : "silence", static_cast<unsigned>(output_samples_.load()));
            observed_state = true;
        }
        if (++stack_probe_frames >= 50) {
            stack_probe_frames = 0;
            observeStack(fetch_stack_free_);
        }
    }
}

esp_err_t LocalAudioFrontend::stop()
{
    ready_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    if (feed_task_) {
        if (xSemaphoreTake(feed_done_, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        feed_task_ = nullptr;
    }
    if (fetch_task_) {
        if (xSemaphoreTake(fetch_done_, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        fetch_task_ = nullptr;
    }
    release();
    return ESP_OK;
}

void LocalAudioFrontend::release()
{
    // 仅在工作任务尚未创建，或两个工作任务都已发信号后调用。
    if (iface_ && afe_) iface_->destroy(afe_);
    afe_ = nullptr;
    iface_ = nullptr;
    models_.unload();
    if (input_queue_) vQueueDeleteWithCaps(input_queue_);
    if (feed_done_) vSemaphoreDelete(feed_done_);
    if (fetch_done_) vSemaphoreDelete(fetch_done_);
    input_queue_ = nullptr;
    feed_done_ = fetch_done_ = nullptr;
    if (feed_adapter_) {
        feed_adapter_->~FeedAdapter();
        heap_caps_free(feed_adapter_);
        feed_adapter_ = nullptr;
    }
    if (paired_feed_adapter_) {
        paired_feed_adapter_->~PairedFeedAdapter();
        heap_caps_free(paired_feed_adapter_);
        paired_feed_adapter_ = nullptr;
    }
    if (source_history_) {
        source_history_->~SourceSignalHistory();
        heap_caps_free(source_history_);
        source_history_ = nullptr;
    }
    output_adapter_.reset();
    output_pending_speech_ = false;
    speech_.store(false);
    aec_active_.store(false, std::memory_order_release);
}

LocalAudioFrontend::SourceWindow LocalAudioFrontend::sourceWindow(uint64_t first, uint64_t end) const
{
    if (source_history_ && ready_.load(std::memory_order_acquire)) return source_history_->window(first, end);
    SourceWindow result;
    result.requested_first = first;
    result.requested_end = end;
    return result;
}

LocalAudioFrontend::Metrics LocalAudioFrontend::metrics() const
{
    Metrics value;
    value.enabled = enabled_.load(std::memory_order_relaxed);
    value.ready = ready_.load(std::memory_order_acquire);
    value.speech = value.ready && speech_.load(std::memory_order_relaxed);
    value.aec_requested = aecRequested();
    value.aec_active = value.ready && aec_active_.load(std::memory_order_relaxed);
    value.last_error = last_error_.load(std::memory_order_relaxed);
    value.feed_samples = feed_samples_.load(std::memory_order_relaxed);
    value.output_samples = output_samples_.load(std::memory_order_relaxed);
    value.output_frames = output_frames_.load(std::memory_order_relaxed);
    value.queue_frames = input_queue_ ? uxQueueMessagesWaiting(input_queue_) : 0;
    value.queue_peak = queue_peak_.load(std::memory_order_relaxed);
    value.feed_max_us = feed_max_us_.load(std::memory_order_relaxed);
    value.fetch_max_us = fetch_max_us_.load(std::memory_order_relaxed);
    value.feed_stack_free = feed_stack_free_.load(std::memory_order_relaxed);
    value.fetch_stack_free = fetch_stack_free_.load(std::memory_order_relaxed);
    value.feed_chunk_samples = feed_chunk_samples_.load(std::memory_order_relaxed);
    value.fetch_chunk_samples = fetch_chunk_samples_.load(std::memory_order_relaxed);
    value.vad_transitions = vad_transitions_.load(std::memory_order_relaxed);
    value.feed_total_us = feed_total_us_.load(std::memory_order_relaxed);
    value.output_blocks = output_blocks_.load(std::memory_order_relaxed);
    value.output_rms = output_rms_.load(std::memory_order_relaxed);
    value.output_peak = output_peak_.load(std::memory_order_relaxed);
    value.vad_thresholds_valid = vad_thresholds_valid_.load(std::memory_order_relaxed);
    value.vad_energy_threshold_cdbfs = vad_energy_threshold_cdbfs_.load(std::memory_order_relaxed);
    value.vad_probability_threshold_milli = vad_probability_threshold_milli_.load(std::memory_order_relaxed);
    value.vad_input_energy_cdbfs = vad_input_energy_cdbfs_.load(std::memory_order_relaxed);
    value.vad_input_energy_valid = value.vad_input_energy_cdbfs != INT32_MIN;
    value.vad_input_energy_peak_cdbfs = vad_input_energy_peak_cdbfs_.load(std::memory_order_relaxed);
    value.vad_input_energy_frames = vad_input_energy_frames_.load(std::memory_order_relaxed);
    value.vad_input_energy_invalid_frames = vad_input_energy_invalid_frames_.load(std::memory_order_relaxed);
    value.microphone_rms = microphone_rms_.load(std::memory_order_relaxed);
    value.microphone_peak = microphone_peak_.load(std::memory_order_relaxed);
    value.microphone_mean = microphone_mean_.load(std::memory_order_relaxed);
    value.microphone_ac_rms = microphone_ac_rms_.load(std::memory_order_relaxed);
    value.reference_rms = reference_rms_.load(std::memory_order_relaxed);
    value.reference_peak = reference_peak_.load(std::memory_order_relaxed);
    value.reference_mean = reference_mean_.load(std::memory_order_relaxed);
    value.reference_ac_rms = reference_ac_rms_.load(std::memory_order_relaxed);
    value.feed_core = aecRequested() ? kAecFeedCore : -1;
    value.fetch_core = aecRequested() ? kAecFetchCore : -1;
    value.feed_budget_waits = feed_budget_waits_.load(std::memory_order_relaxed);
    value.fetch_budget_waits = fetch_budget_waits_.load(std::memory_order_relaxed);
    value.feed_batch_max = feed_batch_max_.load(std::memory_order_relaxed);
    value.fetch_batch_max = fetch_batch_max_.load(std::memory_order_relaxed);
    value.feed_work_wall_max_us = feed_work_wall_max_us_.load(std::memory_order_relaxed);
    value.fetch_work_wall_max_us = fetch_work_wall_max_us_.load(std::memory_order_relaxed);
    return value;
}

void LocalAudioFrontend::logSchedulingDiagnostics() const
{
    const Metrics value = metrics();
    ESP_LOGI(kTag, "schedule policy=%s feed_core=%d fetch_core=%d priority=%u "
             "budget_us=%u budget_attempts=%u feed_waits=%u fetch_waits=%u "
             "feed_batch_max=%u fetch_batch_max=%u feed_work_wall_max_us=%u "
             "fetch_work_wall_max_us=%u wall_includes_success_fetch_wait=1 cpu_time=0",
             value.aec_requested ? "aec_cooperative" : "mono_established",
             static_cast<int>(value.feed_core), static_cast<int>(value.fetch_core),
             value.aec_requested ? 6U : 5U,
             value.aec_requested ? kWorkBudgetUs : 0,
             value.aec_requested ? kWorkBudgetBlocks : 0,
             static_cast<unsigned>(value.feed_budget_waits),
             static_cast<unsigned>(value.fetch_budget_waits),
             static_cast<unsigned>(value.feed_batch_max),
             static_cast<unsigned>(value.fetch_batch_max),
             static_cast<unsigned>(value.feed_work_wall_max_us),
             static_cast<unsigned>(value.fetch_work_wall_max_us));
}
