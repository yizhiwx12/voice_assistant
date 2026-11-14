#include "afe_signal_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <new>

#include "barge_in_observer.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_audio_frontend.hpp"
#include "pcm_dc_blocker.hpp"
#include "sdkconfig.h"

namespace voiceaudio {
namespace {
constexpr char kTag[] = "afe_signal_probe";
constexpr size_t kFrame = 320;
constexpr uint32_t kSamples = 64000;
constexpr uint32_t kSteadyFirst = 32000;
constexpr uint32_t kImpulseSample = 16000;
constexpr int64_t kFrameUs = 20000;
constexpr int64_t kDeadlineUs = 10000000;
static_assert(kSamples % kFrame == 0 && kSamples % 512 == 0);

#ifdef CONFIG_VOICE_BARGE_IN_CONFIRM_MS
constexpr size_t kConfirmFrames = (CONFIG_VOICE_BARGE_IN_CONFIRM_MS + 19) / 20;
#else
constexpr size_t kConfirmFrames = 10;
#endif
#ifdef CONFIG_VOICE_BARGE_IN_MIN_AC_RMS
constexpr uint32_t kMinimumAc = CONFIG_VOICE_BARGE_IN_MIN_AC_RMS;
#else
constexpr uint32_t kMinimumAc = 80;
#endif
constexpr size_t kPrerollFrames = 20;
constexpr size_t kHoldFrames = 100;
constexpr size_t kObserverCapacity = kPrerollFrames + kConfirmFrames + kHoldFrames + 4;

enum class ProbeCase : uint8_t { Zero, ReferenceOnly, DcNoise, DcNoiseReference, Impulse };
constexpr std::array<ProbeCase, 5> kCases{{ProbeCase::Zero, ProbeCase::ReferenceOnly,
    ProbeCase::DcNoise, ProbeCase::DcNoiseReference, ProbeCase::Impulse}};

const char *caseName(ProbeCase kind)
{
    switch (kind) {
    case ProbeCase::Zero: return "zero";
    case ProbeCase::ReferenceOnly: return "ref_only";
    case ProbeCase::DcNoise: return "dc_noise";
    case ProbeCase::DcNoiseReference: return "dc_noise_ref";
    case ProbeCase::Impulse: return "impulse";
    }
    return "invalid";
}

// 纯整数、有界的合成浊音谐波，带变化包络。
// 这是可复现的参考激励，不是真实语音或声学测试夹具。
// 起始处故意不连续，报告时必须说明它是合成激励。
int32_t triangle(uint32_t phase)
{
    const int32_t position = static_cast<int32_t>(phase >> 16);
    return position < 32768 ? position * 2 - 32768 : 98303 - position * 2;
}
int16_t referenceSample(uint32_t ordinal)
{
    if (ordinal < 16000) return 0;
    const uint32_t sample = ordinal - 16000;
    constexpr uint32_t fundamental = 36775657;  // 16 kHz 采样率下约为 137 Hz。
    const uint32_t phase = sample * fundamental;
    const int32_t voiced = (3 * triangle(phase) + triangle(phase * 2U) +
                           triangle(phase * 6U)) / 5;
    constexpr int32_t amplitudes[] = {1800, 5400, 2800, 7200, 0, 3200, 6000, 1200};
    const int32_t amplitude = amplitudes[(sample / 1600) % 8];
    return static_cast<int16_t>(voiced * amplitude / 32768);
}

class Stimulus {
public:
    explicit Stimulus(ProbeCase kind) : kind_(kind) {}
    void next(int16_t &microphone, int16_t &reference)
    {
        microphone = reference = 0;
        if (kind_ == ProbeCase::DcNoise || kind_ == ProbeCase::DcNoiseReference) {
            random_ ^= random_ << 13;
            random_ ^= random_ >> 17;
            random_ ^= random_ << 5;
            microphone = static_cast<int16_t>(300 + static_cast<int32_t>(random_ % 69) - 34);
        }
        if (kind_ == ProbeCase::ReferenceOnly || kind_ == ProbeCase::DcNoiseReference)
            reference = referenceSample(ordinal_);
        if (kind_ == ProbeCase::Impulse && ordinal_ == kImpulseSample) microphone = 12000;
        ++ordinal_;
    }
private:
    ProbeCase kind_;
    uint32_t random_ = 0x91e10da5;
    uint32_t ordinal_ = 0;
};

struct Moments {
    uint32_t count = 0, peak = 0;
    uint64_t peak_sample = 0, squares = 0;
    int64_t sum = 0;
    void add(int16_t value, uint64_t ordinal)
    {
        const int32_t sample = value;
        const uint32_t magnitude = static_cast<uint32_t>(sample < 0 ? -sample : sample);
        if (magnitude > peak) { peak = magnitude; peak_sample = ordinal; }
        sum += sample;
        squares += static_cast<int64_t>(sample) * sample;
        ++count;
    }
    int32_t mean() const { return count ? static_cast<int32_t>(sum / count) : 0; }
    uint32_t rms() const
    {
        return count ? static_cast<uint32_t>(std::sqrt(static_cast<double>(squares) / count)) : 0;
    }
    uint32_t acRms() const
    {
        if (!count) return 0;
        const double average = static_cast<double>(sum) / count;
        return static_cast<uint32_t>(std::sqrt(std::max(0.0,
            static_cast<double>(squares) / count - average * average)));
    }
};
struct SignalStats {
    Moments total, steady;
    void add(int16_t sample, uint64_t ordinal)
    {
        total.add(sample, ordinal);
        if (ordinal >= kSteadyFirst) steady.add(sample, ordinal);
    }
};

struct Context {
    LocalAudioFrontend frontend;
    BargeInObserver observer;
    std::array<BargeInObserver::Frame, kObserverCapacity> observer_storage{};
    std::array<int16_t, kFrame> microphone{}, reference{};
    SignalStats mic, dc, ref, out;
    PcmDcBlocker expected_dc;
    std::atomic<uint32_t> output_samples{0};
    std::atomic<esp_err_t> callback_error{ESP_OK};
    uint64_t next_output = 0;
    uint32_t vad_frames = 0, steady_vad_frames = 0;
    uint32_t input_samples = 0;

    void output(const int16_t *pcm, size_t count, uint64_t ordinal, bool speech)
    {
        // 仅由 fetch 工作任务执行，每次固定处理 320 个样本。
        // 不分配、不打印、不访问硬件、不取消，也不访问共享控制器状态。
        if (!pcm || count != kFrame || ordinal != next_output || ordinal + count > kSamples) {
            callback_error.store(ESP_ERR_INVALID_SIZE, std::memory_order_release);
            return;
        }
        for (size_t i = 0; i < count; ++i) out.add(pcm[i], ordinal + i);
        if (speech) {
            ++vad_frames;
            if (ordinal >= kSteadyFirst) ++steady_vad_frames;
        }
        observer.append(pcm, count, ordinal, speech);
        if (observer.snapshot().phase == BargeInObserver::Phase::Fault)
            callback_error.store(ESP_FAIL, std::memory_order_release);
        next_output += count;
        output_samples.store(static_cast<uint32_t>(next_output), std::memory_order_release);
    }
};

// 停止超时表示工作任务仍可能引用回调/上下文。
// 因此有意保留全部资源，后续调用也必须按失败关闭处理。
Context *retained_context = nullptr;

void delayOneTick() { vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(1))); }

void logCase(ProbeCase kind, const Context &context, esp_err_t result,
             const LocalAudioFrontend::Metrics &metrics, bool aec_during_case)
{
    const auto observer = context.observer.snapshot();
    const int64_t peak_sample = context.out.total.peak ?
        static_cast<int64_t>(context.out.total.peak_sample) : -1;
    const int64_t impulse_lag = kind == ProbeCase::Impulse && peak_sample >= 0 ?
        peak_sample - kImpulseSample : -1;
    ESP_LOGI(kTag, "afe_signal_probe case=%s status=%s error=%d in=%u out=%u "
        "mic_mean=%ld mic_rms=%u mic_ac=%u mic_peak=%u dc_mean=%ld dc_rms=%u dc_ac=%u "
        "ref_mean=%ld ref_ac=%u ref_peak=%u out_mean=%ld out_rms=%u out_ac=%u out_peak=%u "
        "steady_samples=%u steady_mic_ac=%u steady_dc_ac=%u steady_ref_ac=%u "
        "steady_out_mean=%ld steady_out_rms=%u steady_out_ac=%u steady_out_peak=%u "
        "vad_frames=%u steady_vad_frames=%u would_confirm=%u candidates=%u energy_rejections=%u "
        "peak_sample=%lld impulse_peak_ordinal_lag=%lld lag_is_group_delay=0 "
        "feed=%u fetched=%u aec_during_case=%u dc_stats=expected_same_filter "
        "min_ac=%u confirm_frames=%u",
        caseName(kind), result == ESP_OK ? "completed" : "failed", result,
        context.input_samples, context.output_samples.load(std::memory_order_acquire),
        static_cast<long>(context.mic.total.mean()), context.mic.total.rms(), context.mic.total.acRms(),
        context.mic.total.peak, static_cast<long>(context.dc.total.mean()), context.dc.total.rms(),
        context.dc.total.acRms(), static_cast<long>(context.ref.total.mean()), context.ref.total.acRms(),
        context.ref.total.peak, static_cast<long>(context.out.total.mean()), context.out.total.rms(),
        context.out.total.acRms(), context.out.total.peak, context.out.steady.count,
        context.mic.steady.acRms(), context.dc.steady.acRms(), context.ref.steady.acRms(),
        static_cast<long>(context.out.steady.mean()), context.out.steady.rms(), context.out.steady.acRms(),
        context.out.steady.peak, context.vad_frames, context.steady_vad_frames,
        observer.confirmations, observer.candidates, observer.energy_rejections,
        static_cast<long long>(peak_sample), static_cast<long long>(impulse_lag),
        metrics.feed_samples, metrics.output_samples, static_cast<unsigned>(aec_during_case),
        kMinimumAc, static_cast<unsigned>(kConfirmFrames));
}

esp_err_t runCase(ProbeCase kind)
{
    // 初始化与提交、排空共用同一个有限用例预算。
    // 即使超时，最后仍必须执行有界的工作任务退出等待。
    const int64_t deadline = esp_timer_get_time() + kDeadlineUs;
    void *storage = heap_caps_malloc(sizeof(Context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) {
        ESP_LOGE(kTag, "afe_signal_probe case=%s status=allocation_failed error=%d",
                 caseName(kind), ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    auto *context = new (storage) Context;
    context->observer.configure(context->observer_storage.data(), kObserverCapacity,
                                kPrerollFrames, kConfirmFrames, kHoldFrames, kMinimumAc);
    context->observer.enable(0);
    esp_err_t result = context->frontend.setOutputCallback(
        [context](const int16_t *pcm, size_t count, uint64_t ordinal, bool speech) {
            context->output(pcm, count, ordinal, speech);
        });
    if (result == ESP_OK) result = context->frontend.start();
    if (result == ESP_OK && !context->frontend.aecActive()) result = ESP_ERR_INVALID_STATE;
    const int64_t started = esp_timer_get_time();
    Stimulus stimulus(kind);
    for (uint32_t first = 0; result == ESP_OK && first < kSamples; first += kFrame) {
        const int64_t due = started + static_cast<int64_t>(first / kFrame) * kFrameUs;
        while (esp_timer_get_time() < due && esp_timer_get_time() < deadline) delayOneTick();
        for (size_t i = 0; i < kFrame; ++i)
            stimulus.next(context->microphone[i], context->reference[i]);
        do {
            if (esp_timer_get_time() >= deadline) { result = ESP_ERR_TIMEOUT; break; }
            result = context->callback_error.load(std::memory_order_acquire);
            if (result != ESP_OK) break;
            const auto metrics = context->frontend.metrics();
            if (!metrics.ready || metrics.last_error != ESP_OK) {
                result = metrics.last_error == ESP_OK ? ESP_FAIL : metrics.last_error;
                break;
            }
            result = context->frontend.tryEnqueuePaired(context->microphone.data(),
                                                       context->reference.data(), kFrame);
            if (result == ESP_ERR_NOT_FINISHED) delayOneTick();
        } while (result == ESP_ERR_NOT_FINISHED);
        if (result != ESP_OK) break;
        for (size_t i = 0; i < kFrame; ++i) {
            context->mic.add(context->microphone[i], first + i);
            context->dc.add(context->expected_dc.process(context->microphone[i]), first + i);
            context->ref.add(context->reference[i], first + i);
        }
        context->input_samples += kFrame;
    }
    while (result == ESP_OK && context->output_samples.load(std::memory_order_acquire) < kSamples) {
        result = context->callback_error.load(std::memory_order_acquire);
        const auto metrics = context->frontend.metrics();
        if (result == ESP_OK && (!metrics.ready || metrics.last_error != ESP_OK))
            result = metrics.last_error == ESP_OK ? ESP_FAIL : metrics.last_error;
        if (result == ESP_OK && esp_timer_get_time() >= deadline) result = ESP_ERR_TIMEOUT;
        if (result == ESP_OK) delayOneTick();
    }
    // stop() 在安全等待工作任务退出后清除 aec_active。
    // 保留运行时状态用于诊断；最终计数仅在任务退出后收集。
    const bool aec_during_case = context->frontend.aecActive();
    const esp_err_t stop_error = context->frontend.stop();
    if (stop_error != ESP_OK) {
        retained_context = context;
        ESP_LOGE(kTag, "afe_signal_probe case=%s status=stop_failed error=%d "
                 "context_retained=1 in=%u out=%u normal_start_allowed=0",
                 caseName(kind), stop_error, context->input_samples,
                 context->output_samples.load(std::memory_order_acquire));
        return stop_error;
    }
    const auto metrics = context->frontend.metrics();
    const esp_err_t callback_error = context->callback_error.load(std::memory_order_acquire);
    if (result == ESP_OK && callback_error != ESP_OK) result = callback_error;
    if (result == ESP_OK && (context->input_samples != kSamples || metrics.feed_samples != kSamples ||
        metrics.output_samples != kSamples || context->output_samples.load() != kSamples))
        result = ESP_ERR_INVALID_SIZE;
    logCase(kind, *context, result, metrics, aec_during_case);
    context->~Context();
    heap_caps_free(context);
    return result;
}
}  // 命名空间

esp_err_t runAfeSignalProbe()
{
    if (retained_context) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(kTag, "afe_signal_probe begin cases=5 samples_per_case=%u rate=16000 "
             "input_frame=320 hardware=none observer_only=1 results_require_device=1", kSamples);
    for (const auto kind : kCases) {
        const esp_err_t result = runCase(kind);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "afe_signal_probe end status=failed case=%s error=%d normal_start_allowed=0",
                     caseName(kind), result);
            return result;
        }
    }
    ESP_LOGI(kTag, "afe_signal_probe end status=completed cases=5 all_instances_destroyed=1 "
             "normal_start_allowed=1 acoustic_pass_claimed=0");
    return ESP_OK;
}
}  // 命名空间 voiceaudio
