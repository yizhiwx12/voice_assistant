// 真实 AudioService 与参考处理器，搭配确定性的硬件和 RTOS 替代实现。
// 私有访问只用于驱动单次服务机会；被测调度算法从 audio_service.cpp 编译而来。
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#define private public
#include "audio_service.hpp"
#undef private
#include "reference_monitor.hpp"
#include "afe_signal_probe.hpp"
#include "app/memory_diagnostics.hpp"
#include "app/audio_idle_guard.hpp"
#include "sdkconfig.h"

namespace {
struct Queue { unsigned capacity, item_size; std::deque<std::vector<uint8_t>> values; };
struct Semaphore { bool mutex; bool available; };
struct Task {
    void (*entry)(void *);
    void *argument;
    std::string name;
    unsigned priority;
    int core;
    bool ran = false;
};
struct Pair { std::array<int16_t, 320> microphone, reference; };
std::unordered_map<void *, size_t> allocations;
std::vector<PlaybackReference::Block> history;
std::vector<std::unique_ptr<Task>> tasks;
std::vector<Pair> submitted_pairs;
std::deque<Pair> frontend_pending;
std::vector<Pair> frontend_processed;
size_t frontend_capacity = SIZE_MAX, paired_try_calls = 0, paired_blocked_calls = 0;
std::vector<std::string> events, failure_reasons, log_messages;
Ics43434Microphone::DmaMetrics microphone_metrics{};
AudioService *capture_owner = nullptr;
int64_t now_us = 1000000, timer_step_us = 0, pump_cost_us = 0, callback_cost_us = 0;
int64_t history_copy_cost_us = 0;
std::function<void()> clock_tick, playback_write;
std::function<void(QueueHandle_t, TickType_t, bool)> queue_wait;
std::function<void(TickType_t)> task_delay;
std::vector<int16_t> played_pcm;
size_t held_mutexes = 0;
int64_t decode_cost_us = 0;
size_t fail_allocation_size = 0, live_queues = 0, live_semaphores = 0, task_stops = 0;
size_t startup_resource_calls = 0, signal_probe_calls = 0;
esp_err_t signal_probe_result = ESP_OK;
std::function<void()> signal_probe_hook;
size_t consumed_history_reads = 0, capture_read_budget = 0, capture_read_calls = 0, raw_frames = 0;
bool scratch_failed = false;
bool schedule_capture_on_start = true;
esp_err_t capture_start_result = ESP_OK, paired_enqueue_result = ESP_OK, drain_result = ESP_OK;
esp_err_t playback_result = ESP_OK;
size_t idle_guard_checks = 0;

void runTask(Task &task)
{
    if (task.ran) return;
    task.ran = true;
    const bool capture = task.name == "voice_capture";
    if (capture) capture_owner = static_cast<AudioService *>(task.argument);
    task.entry(task.argument);
    // 确定性读取钩子在脚本帧结束后停止循环。
    // 只恢复合成调度器状态，绝不恢复 AEC 失败锁存状态。
    if (capture) {
        capture_owner->running_.store(true);
        capture_owner = nullptr;
    }
}

size_t eventIndex(const std::string &event)
{
    const auto it = std::find(events.begin(), events.end(), event);
    assert(it != events.end());
    return static_cast<size_t>(it - events.begin());
}

void resetTestState()
{
    assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
    history.clear();
    tasks.clear();
    submitted_pairs.clear();
    frontend_pending.clear();
    frontend_processed.clear();
    frontend_capacity = SIZE_MAX;
    paired_try_calls = paired_blocked_calls = 0;
    events.clear();
    failure_reasons.clear();
    log_messages.clear();
    microphone_metrics = {};
    timer_step_us = pump_cost_us = callback_cost_us = 0;
    history_copy_cost_us = 0;
    clock_tick = playback_write = {};
    queue_wait = {};
    task_delay = {};
    played_pcm.clear();
    assert(held_mutexes == 0);
    decode_cost_us = 0;
    capture_read_budget = capture_read_calls = raw_frames = 0;
    capture_start_result = paired_enqueue_result = drain_result = ESP_OK;
    playback_result = ESP_OK;
    schedule_capture_on_start = true;
    startup_resource_calls = signal_probe_calls = 0;
    signal_probe_result = ESP_OK;
    signal_probe_hook = {};
}

void setHistory(size_t count, bool content = false)
{
    history.clear();
    for (size_t i = 0; i < count; ++i) {
        PlaybackReference::Block value{};
        value.dma_generation = 3;
        value.playback_serial = 1;
        value.history_sequence = static_cast<uint32_t>(i + 1);
        value.first_sample_24k = i * 240;
        value.observed_end_us = 200000 + static_cast<int64_t>((i + 1) * 10000);
        value.sample_count = 240;
        value.interval_kind = content ? PlaybackReference::IntervalKind::Content
                                     : PlaybackReference::IntervalKind::DigitalSilence;
        value.known_silence = !content;
        value.stereo_match = true;
        value.mono.fill(content ? 2000 : 0);
        history.push_back(value);
    }
    microphone_metrics = {};
    microphone_metrics.completed_blocks = static_cast<uint32_t>(count);
    microphone_metrics.completed_samples = count * 160;
    microphone_metrics.last_completion_us = 200000 + count * 10000;
    microphone_metrics.timing_enabled = microphone_metrics.timeline_valid = true;
}

void queueCapture(AudioService &service, uint64_t first, uint32_t serial)
{
    AudioService::ReferenceCaptureFrame frame{};
    frame.first_sample = first;
    frame.playback_serial = serial;
    frame.pcm.fill(1200);
    assert(xQueueSend(service.reference_capture_queue_, &frame, 0));
}

struct Fixture {
    AudioService service;
    Fixture() {
        assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
        tasks.clear(); submitted_pairs.clear(); events.clear(); failure_reasons.clear(); log_messages.clear();
        capture_read_calls = raw_frames = 0;
        const auto previous_probe_calls = signal_probe_calls;
        assert(service.start([](const int16_t *, size_t) {
            events.emplace_back("raw-callback");
            ++raw_frames;
            now_us += callback_cost_us;
        }, {}, {}, 1, 40) == ESP_OK);
        assert(signal_probe_calls == previous_probe_calls + CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT);
        assert(service.reference_monitor_ && service.reference_monitor_->metrics().ready);
        for (const auto &task : tasks) {
            const bool capture = task->name == "voice_capture";
            assert(task->name == "voice_capture" || task->name == "voice_play");
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
            // 采集保持在 CPU1、优先级 7。播放持有扬声器与参考时间线，
            // 因此不能与 feed 共用核心；否则同优先级 DSP 工作可能
            // 把参考服务延迟到超过 200 ms 的 AEC 配对年龄保护。
            assert((capture && task->core == 1 && task->priority == 7U) ||
                   (!capture && task->core == 0 && task->priority == 6U));
#else
            assert(task->core == -1 && task->priority == (capture ? 6U : 5U));
#endif
        }
    }
    ~Fixture() {
        assert(service.stop() == ESP_OK);
        assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
        resetTestState();
    }
};

void invalidStartupArgumentsDoNotRunTheProbeOrInitializeResources()
{
    {
        AudioService service;
        const auto capture = [](const int16_t *, size_t) {};
        assert(service.start({}, {}, {}, 1, 40) == ESP_ERR_INVALID_ARG);
        assert(service.start(capture, {}, {}, 0, 40) == ESP_ERR_INVALID_ARG);
        assert(service.start(capture, {}, {}, 17, 40) == ESP_ERR_INVALID_ARG);
        assert(service.start(capture, {}, {}, 1, 101) == ESP_ERR_INVALID_ARG);
        assert(signal_probe_calls == 0 && startup_resource_calls == 0);
        assert(events.empty() && tasks.empty() && allocations.empty());
        assert(live_queues == 0 && live_semaphores == 0);
        assert(!service.capture_ && !service.playback_done_ && !service.error_);
        assert(!service.microphone_.isReady() && !service.speaker_.isReady());
    }
    resetTestState();
}

#if CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT
void probeErrorsReturnUnchangedBeforeAnyNormalStartupAndPermitACleanRetry()
{
    for (const esp_err_t failure : {ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FINISHED}) {
        {
            AudioService service;
            bool callback_called = false;
            signal_probe_result = failure;
            signal_probe_hook = [&] {
                assert(startup_resource_calls == 0 && tasks.empty() && allocations.empty());
                assert(live_queues == 0 && live_semaphores == 0);
                assert(!service.capture_ && !service.playback_done_ && !service.error_);
                assert(!service.running_.load());
                assert(!service.microphone_.isReady() && !service.speaker_.isReady());
            };
            assert(service.start([&](const int16_t *, size_t) { callback_called = true; },
                                 [&](const voice::Identity &) { callback_called = true; },
                                 [&](esp_err_t) { callback_called = true; }, 3, 67) == failure);
            assert(signal_probe_calls == 1 && startup_resource_calls == 0);
            assert(events == std::vector<std::string>{"afe-signal-probe"});
            assert(tasks.empty() && allocations.empty() && live_queues == 0 && live_semaphores == 0);
            assert(!service.capture_ && !service.playback_done_ && !service.error_);
            assert(!service.playback_queue_ && !service.reference_monitor_ && !service.running_.load());
            assert(!service.microphone_.isReady() && !service.speaker_.isReady() && !callback_called);

            // 诊断返回错误后，同一个服务仍保持未启动的干净状态；
            // 后续成功探测仍能执行普通启动。
            signal_probe_result = ESP_OK;
            assert(service.start([](const int16_t *, size_t) {}, {}, {}, 1, 40) == ESP_OK);
            assert(signal_probe_calls == 2 && startup_resource_calls > 0);
            assert(service.capture_ && service.microphone_.isReady() && service.speaker_.isReady());
            assert(tasks.size() == 2 && service.reference_monitor_);
            assert(service.stop() == ESP_OK);
            assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
        }
        resetTestState();
    }
}
#endif

void startupProbeRunsBeforeNormalStartupOrIsAbsentFromOrdinaryBuilds()
{
    {
        AudioService service;
        // 非探测构建变体必须忽略测试定义，即使该定义会失败。
        signal_probe_result = CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT ? ESP_OK : ESP_FAIL;
        signal_probe_hook = [&] {
            assert(startup_resource_calls == 0 && allocations.empty() && tasks.empty());
            assert(live_queues == 0 && live_semaphores == 0);
            assert(!service.capture_ && !service.playback_done_ && !service.error_);
            assert(!service.microphone_.isReady() && !service.speaker_.isReady());
        };
        assert(service.start([](const int16_t *, size_t) {}, {}, {}, 3, 67) == ESP_OK);
        assert(signal_probe_calls == CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT);
        assert(service.capture_ && service.gain_ == 3 && service.speaker_.config_.volume_percent == 67);
        assert(service.microphone_.isReady() && service.speaker_.isReady());
        assert(service.reference_monitor_ && service.playback_queue_ && tasks.size() == 2);
        assert(startup_resource_calls > 0 && capture_read_calls == 1);
#if CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT
        assert(events.front() == "afe-signal-probe");
        for (const auto *event : {"rx-init", "speaker-init", "frontend-start", "task:voice_play", "task:voice_capture"})
            assert(eventIndex("afe-signal-probe") < eventIndex(event));
#else
        assert(std::find(events.begin(), events.end(), "afe-signal-probe") == events.end());
#endif
        // 现有的已启动状态保护同样先于诊断钩子执行。
        const auto previous_resources = startup_resource_calls;
        assert(service.start([](const int16_t *, size_t) {}, {}, {}, 1, 40) == ESP_ERR_INVALID_STATE);
        assert(signal_probe_calls == CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT &&
               startup_resource_calls == previous_resources);
        const auto stopped_before = task_stops;
        assert(service.stop() == ESP_OK);
        assert(task_stops == stopped_before + 2);
        assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
        assert(!service.microphone_.isReady() && !service.speaker_.isReady());
        assert(!service.playback_queue_ && !service.reference_monitor_);
        assert(service.stop() == ESP_OK);
        assert(signal_probe_calls == CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT);
    }
    resetTestState();
}

void boundedBatchAndOrderedContinuation()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20);
    queueCapture(service, 1600, 1);  // 已到处理时机但未陈旧，历史有积压。
    service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 8 && service.reference_history_pending_.load());
    assert(service.reference_monitor_->metrics().input_samples == 8 * 240);
    assert(service.reference_monitor_->acousticMetrics().attempts == 0);
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 1);
    service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 16 && service.reference_history_pending_.load());
    assert(service.reference_monitor_->acousticMetrics().attempts == 0);
    service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 20 && !service.reference_history_pending_.load());
    const auto metrics = service.reference_monitor_->metrics();
    assert(metrics.input_samples == 20 * 240 && metrics.output_samples == 20 * 160);
    assert(metrics.gap_resets == 0 && metrics.overwritten_blocks == 0 && metrics.processing_errors == 0);
    assert(service.reference_max_batch_ == 8 && service.reference_budget_yields_ == 2);
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    assert(service.reference_monitor_->acousticMetrics().attempts == 0);
    assert(service.reference_monitor_->metrics().probe_frames == 0);
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 1);
#else
    assert(service.reference_monitor_->acousticMetrics().attempts == 1);
#endif
    service.pollReferenceMonitor();
    assert(service.reference_monitor_->metrics().input_samples == 20 * 240);  // 不重放。
}

void softBudgetAndPumpAccounting()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20);
    timer_step_us = 1500;
    service.pollReferenceMonitor();
    const auto first_cursor = service.reference_history_cursor_;
    assert(first_cursor > 0 && first_cursor < 8 && service.reference_history_pending_.load());
    assert(service.reference_budget_yields_ == 1);
    timer_step_us = 0;
    pump_cost_us = 1700;
    for (size_t i = 0; i < 10 && service.reference_history_pending_.load(); ++i)
        service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 20 && !service.reference_history_pending_.load());
    assert(service.reference_monitor_->metrics().input_samples == 4800);
    assert(service.reference_pump_max_us_ >= 1700);
    assert(service.reference_poll_max_us_ >= 3 * 1700);  // 包括重复的 pump 工作。
}

void staleCaptureAndOneAnalysisPerOpportunity()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(30);
    for (size_t i = 0; i < 4; ++i) service.pollReferenceMonitor();
    assert(!service.reference_history_pending_.load());
    const uint64_t sample_end = microphone_metrics.completed_samples;
    queueCapture(service, sample_end - 1800, 999);  // 旧播放序号。
    queueCapture(service, sample_end - 3400, 1);    // 早于 200 ms 之前。
    queueCapture(service, sample_end - 2000, 1);
    queueCapture(service, sample_end - 1600, 1);    // 已到处理时机，但下一次调用受节奏限流。
    service.pollReferenceMonitor();
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    // C 模式优先处理必要配对，再做可选 B 数字/声学工作。
    // M 模式仍覆盖下面原有的诊断调度。
    assert(service.reference_capture_stale_ == 0);
    assert(service.reference_monitor_->acousticMetrics().attempts == 0);
    assert(service.reference_monitor_->metrics().probe_frames == 0);
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 4);
    service.pollReferenceMonitor();
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 4);
    return;
#endif
    assert(service.reference_capture_stale_ == 2);
    assert(service.reference_monitor_->acousticMetrics().attempts == 1);
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 1);
    service.pollReferenceMonitor();
    assert(service.reference_monitor_->acousticMetrics().attempts == 1);
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 0);
    queueCapture(service, sample_end - 1000, 1);  // TX/FIR 支持范围尚未到达。
    service.pollReferenceMonitor();
    assert(uxQueueMessagesWaiting(service.reference_capture_queue_) == 1);
    assert(service.reference_monitor_->acousticMetrics().attempts == 1);
}

void scratchFailureKeepsDigitalAndStopReleases()
{
    fail_allocation_size = sizeof(AudioService::ReferenceCaptureFrame);
    scratch_failed = false;
    {
        Fixture fixture;
        auto &service = fixture.service;
        assert(scratch_failed && !service.reference_capture_scratch_ && !service.reference_capture_queue_);
        assert(service.reference_monitor_ && service.reference_monitor_->timelineMetrics().available);
        setHistory(4);
        service.pollReferenceMonitor();
        assert(service.reference_monitor_->metrics().input_samples == 4 * 240);
        assert(service.reference_monitor_->metrics().output_samples == 4 * 160);
        const auto before = task_stops;
        assert(service.stop() == ESP_OK);
        assert(task_stops == before + 2);
        assert(!service.reference_monitor_ && !service.reference_capture_scratch_ &&
               !service.reference_capture_queue_ && !service.playback_queue_);
        assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
        assert(service.stop() == ESP_OK);  // 后备清理后仍保持幂等。
    }
    // 正常分配和停止也必须释放单生产者临时区。
    Fixture restored;
    assert(restored.service.reference_capture_scratch_ && restored.service.reference_capture_queue_);
}

void deferredCaptureStartsOnItsConsumer()
{
    Fixture fixture;
    assert(!fixture.service.microphone_.config_.start_immediately);
    assert(eventIndex("frontend-start") < eventIndex("task:voice_capture"));
    assert(eventIndex("task:voice_capture") < eventIndex("rx-enable"));
    assert(eventIndex("rx-first-read") == eventIndex("rx-enable") + 1);
    assert(capture_read_calls == 1 && raw_frames == 0);
}

void captureDiagnosticsBracketTheRawCallback()
{
    capture_read_budget = 1;
    callback_cost_us = 731;
    Fixture fixture;
    std::vector<std::string> phases;
    for (const auto &event : events)
        if (event.compare(0, 14, "capture-phase:") == 0) phases.push_back(event);
    const std::vector<std::string> expected{
        "capture-phase:read", "capture-phase:convert", "capture-phase:callback",
        "capture-phase:queue", "capture-phase:read", "capture-phase:stopped"
    };
    assert(phases == expected);
    assert(eventIndex("capture-phase:callback") < eventIndex("raw-callback"));
    assert(eventIndex("raw-callback") < eventIndex("capture-phase:queue"));
    const auto metrics = fixture.service.microphone_.dmaMetrics().service;
    assert(raw_frames == 1 && metrics.coherent);
    assert(metrics.phase == Ics43434Microphone::ConsumerPhase::Stopped);
    assert(metrics.read_calls == 2 && metrics.read_completions == 1);
    // 合成回调耗时只检查生产钩子的放置位置；
    // 真实驱动发布/时钟计算由驱动自己的测试框架验证。
    assert(metrics.last_callback_us == 731 && metrics.max_callback_us == 731);
}

void captureEnableFailureIsReturnedAndReleased()
{
    capture_start_result = ESP_FAIL;
    AudioService service;
    assert(service.start([](const int16_t *, size_t) {}, {}, {}, 1, 40) == ESP_FAIL);
    assert(capture_read_calls == 0);
    assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
    resetTestState();
}

void captureTaskStartupTimeoutReleasesResources()
{
    schedule_capture_on_start = false;
    AudioService service;
    assert(service.start([](const int16_t *, size_t) {}, {}, {}, 1, 40) == ESP_ERR_TIMEOUT);
    assert(capture_read_calls == 0 && std::find(events.begin(), events.end(), "rx-enable") == events.end());
    assert(allocations.empty() && live_queues == 0 && live_semaphores == 0);
    resetTestState();
}

void playbackBoundaryUsesTheConfiguredFence()
{
    Fixture fixture;
    auto &service = fixture.service;
    events.clear();
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    const uint32_t generation = service.speaker_.playbackReferenceTag().dma_generation;
    // 排队音频块已使 TX 作用域包含内容。取消必须
    // 排空真实硬件作用域，才能允许新的播放绑定。
    service.playback_reference_drained_ = false;
    assert(service.beginPlayback({1, 1, 1}, 1, false, 0) == ESP_OK);
    assert(eventIndex("speaker-drain") < eventIndex("speaker-mute"));
    assert(eventIndex("speaker-mute") < eventIndex("reference-bind"));
    assert(std::find(events.begin(), events.end(), "speaker-reset") == events.end());
    assert(service.speaker_.playbackReferenceTag().dma_generation == generation);
    assert(service.playback_reference_drained_);
    events.clear();
    service.playback_reference_drained_ = false;
    drain_result = ESP_ERR_TIMEOUT;
    assert(service.beginPlayback({1, 2, 2}, 2, false, 0) == ESP_ERR_TIMEOUT);
    assert(std::find(events.begin(), events.end(), "reference-bind") == events.end());
#else
    assert(service.beginPlayback({1, 1, 1}, 1, false, 0) == ESP_OK);
    assert(eventIndex("speaker-reset") < eventIndex("reference-bind"));
#endif
}

#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
bool hasFailure(const char *reason)
{
    return std::any_of(failure_reasons.begin(), failure_reasons.end(),
        [reason](const std::string &message) { return message.find(reason) != std::string::npos; });
}

void emptyReplyCanCancelAndRebindWithoutChangingDma()
{
    Fixture fixture;
    auto &service = fixture.service;
    const uint32_t generation = service.speaker_.playbackReferenceTag().dma_generation;
    events.clear();
    uint32_t previous_serial = 0;
    for (uint32_t turn = 1; turn <= 3; ++turn) {
        assert(service.beginPlayback({1, turn, turn}, turn, false, 0) == ESP_OK);
        const auto tag = service.speaker_.playbackReferenceTag();
        assert(tag.dma_generation == generation && tag.playback_serial != previous_serial);
        previous_serial = tag.playback_serial;
        // 在首个内容写入前取消已宣告的流。
        assert(service.stopPlayback() == ESP_OK);
        assert(service.frontend_.aecActive() && service.aec_error_.load() == ESP_OK);
    }
    assert(std::find(events.begin(), events.end(), "speaker-drain") == events.end());
    assert(std::find(events.begin(), events.end(), "speaker-reset") == events.end());

    // 成功排空内容后，相同资格必须继续保留。
    const std::array<int16_t, 240> content = {123};
    assert(service.beginPlayback({1, 4, 4}, 4, false, 0) == ESP_OK);
    assert(service.speaker_.setEnabled(true) == ESP_OK);
    assert(service.speaker_.writeMono(content.data(), content.size(), 100) == ESP_OK);
    service.playback_reference_drained_ = false;
    assert(service.stopPlayback() == ESP_OK);
    events.clear();
    for (uint32_t turn = 5; turn <= 7; ++turn) {
        assert(service.beginPlayback({1, turn, turn}, turn, false, 0) == ESP_OK);
        assert(service.stopPlayback() == ESP_OK);
        assert(service.speaker_.playbackReferenceTag().dma_generation == generation);
    }
    assert(std::find(events.begin(), events.end(), "speaker-drain") == events.end());
    assert(std::find(events.begin(), events.end(), "speaker-reset") == events.end());
    assert(service.frontend_.aecActive() && service.aec_error_.load() == ESP_OK);
}

void queueAec(AudioService &service, uint64_t first, bool known_mute = false, int16_t value = 1200)
{
    AudioService::AecCaptureFrame frame{};
    frame.first_sample = first;
    frame.known_mute = known_mute;
    frame.pcm.fill(value);
    assert(xQueueSend(service.aec_capture_queue_, &frame, 0));
}

void aecSummaryPreservesWideCoordinatesAndRejectsInProgressPublication()
{
    Fixture fixture;
    auto &service = fixture.service;
    const auto absent = service.aecServiceSnapshot();
    assert(!absent.coherent && absent.queue == 0);
    assert(std::all_of(absent.words.begin(), absent.words.end(), [](uint32_t word) { return word == 0; }));

    const int64_t saved_now = now_us;
    now_us = (int64_t{1} << 40) + 123456789;
    const uint64_t first_sample = (uint64_t{1} << 35) + 73;
    const uint64_t sample_end = first_sample + 2016;
    queueAec(service, first_sample);
    service.reference_history_pending_.store(true);
    service.aec_last_pair_us_.store(4312);
    service.reference_service_gap_us_.store(137);
    service.reference_service_gap_max_us_.store(341);
    service.publishAecPairAttempt(first_sample, sample_end, true, true, 317);
    const auto complete = service.aecServiceSnapshot();
    assert(complete.coherent && complete.pending && complete.queue == 1);
    assert(((static_cast<uint64_t>(complete.words[1]) << 32) | complete.words[0]) == first_sample);
    assert(((static_cast<uint64_t>(complete.words[3]) << 32) | complete.words[2]) == sample_end);
    assert(((static_cast<uint64_t>(complete.words[9]) << 32) | complete.words[8]) == static_cast<uint64_t>(now_us));
    assert(complete.words[4] == 7 && complete.words[5] == 2016 && complete.words[6] == 1);
    assert(complete.words[7] == 317 && complete.words[10] == 4312);
    assert(complete.last_pair_us == 4312 && complete.service_gap_us == 137 && complete.service_gap_max_us == 341);

    service.publishAecPairAttempt(first_sample, first_sample + UINT32_MAX + uint64_t{8}, true, false, 0);
    assert(service.aecServiceSnapshot().words[5] == UINT32_MAX);  // 年龄饱和处理，绝不回绕。
    // 模拟写者在两次序号递增之间被抢占；快照必须直接返回，
    // 不能重试，也不能把半发布字段当作数据。
    service.aec_attempt_sequence_.fetch_add(1);
    service.aec_attempt_words_[0].store(0xBADU);
    service.reference_history_pending_.store(false);
    service.aec_last_pair_us_.store(4422);
    service.reference_service_gap_us_.store(501);
    service.reference_service_gap_max_us_.store(999);
    const auto in_progress = service.aecServiceSnapshot();
    assert(!in_progress.coherent && !in_progress.pending && in_progress.queue == 1);
    assert(std::all_of(in_progress.words.begin(), in_progress.words.end(), [](uint32_t word) { return word == 0; }));
    assert(in_progress.last_pair_us == 4422 && in_progress.service_gap_us == 501 && in_progress.service_gap_max_us == 999);
    service.aec_attempt_sequence_.fetch_add(1);
    now_us = saved_now;
}

void firstFailureDetailsRemainFrozenUntilOneDeferredFlush()
{
    Fixture fixture;
    auto &service = fixture.service;
    const int64_t saved_now = now_us;
    const auto messageCount = [](const char *prefix) {
        const size_t length = std::strlen(prefix);
        return std::count_if(log_messages.begin(), log_messages.end(),
            [prefix, length](const std::string &message) { return message.compare(0, length, prefix) == 0; });
    };
    microphone_metrics.completed_samples = 5120;
    microphone_metrics.last_completion_us = 4321000;
    microphone_metrics.timeline_valid = false;
    microphone_metrics.overflow_events = 3;
    microphone_metrics.first_overflow_us = 4310000;
    microphone_metrics.service.coherent = true;
    microphone_metrics.service.phase = Ics43434Microphone::ConsumerPhase::Callback;
    microphone_metrics.first_overflow_service = microphone_metrics.service;
    queueAec(service, 1234);
    service.reference_history_pending_.store(true);
    service.publishAecPairAttempt(1234, 4416, true, true, 127);
    service.failAecReference(ESP_ERR_TIMEOUT, "first_test_failure");
    const auto frozen = service.aec_failure_snapshot_;
    assert(service.aec_failure_log_pending_.load());
    assert(service.aec_error_.load() == ESP_ERR_TIMEOUT && !service.frontend_.aecActive());
    assert(frozen.service.coherent && frozen.service.pending && frozen.service.queue == 1);
    assert(frozen.service.words[7] == 127 && frozen.microphone.completed_samples == 5120);
    assert(frozen.at_us == static_cast<uint64_t>(now_us));
    assert(!frozen.clock.valid);
    assert(failure_reasons.size() == 1 && hasFailure("first_test_failure"));
    assert(messageCount("capture_failure ") == 0 && messageCount("capture_service ") == 0);
    assert(messageCount("aec_service ") == 0);

    // 首个失败锁定证据后，采集/持有者继续前进。
    // 第二次失败不能用更晚的指标替换最初原因。
    now_us += 50000;
    microphone_metrics.completed_samples = 9999;
    microphone_metrics.last_completion_us = 9999999;
    microphone_metrics.overflow_events = 80;
    microphone_metrics.service.phase = Ics43434Microphone::ConsumerPhase::Queue;
    // 首个失败之后发现的时钟故障不是其原因证据。
    // 发布可以稍后完成，但不能回填已冻结快照。
    service.reference_monitor_->observeMicrophone(9999, 9999999, 80, false);
    service.publishReferenceClockFailure();
    assert(service.reference_clock_failure_ready_.load());
    queueAec(service, 1555);
    service.reference_history_pending_.store(false);
    service.publishAecPairAttempt(1555, 7777, true, true, 320);
    service.failAecReference(ESP_FAIL, "secondary_test_failure");
    assert(service.aec_error_.load() == ESP_ERR_TIMEOUT && failure_reasons.size() == 1);
    assert(service.aec_failure_snapshot_.service.words == frozen.service.words);
    assert(service.aec_failure_snapshot_.service.queue == 1 && service.aec_failure_snapshot_.service.pending);
    assert(service.aec_failure_snapshot_.microphone.completed_samples == 5120);
    assert(service.aec_failure_snapshot_.microphone.overflow_events == 3);
    assert(service.aec_failure_snapshot_.microphone.service.phase == Ics43434Microphone::ConsumerPhase::Callback);
    assert(service.aec_failure_snapshot_.at_us == frozen.at_us);
    assert(!service.aec_failure_snapshot_.clock.valid);
    assert(messageCount("capture_failure ") == 0 && messageCount("capture_service ") == 0);

    service.logPendingAecFailure();
    assert(!service.aec_failure_log_pending_.load());
    assert(messageCount("capture_failure ") == 1 && messageCount("capture_service ") == 2);
    assert(messageCount("aec_service ") == 1);
    assert(messageCount("aec_clock_failure valid=0 ") == 1);
    assert(std::any_of(log_messages.begin(), log_messages.end(), [](const std::string &message) {
        return message == "capture_failure valid=0 dma_sample_end=5120 dma_end_us=4321000 overflow=3 invalid=0";
    }));
    const size_t logs_after_flush = log_messages.size();
    service.logPendingAecFailure();
    service.failAecReference(ESP_FAIL, "late_test_failure");
    service.logPendingAecFailure();
    assert(log_messages.size() == logs_after_flush && !service.aec_failure_log_pending_.load());
    assert(service.aec_failure_snapshot_.at_us == frozen.at_us);
    now_us = saved_now;
}

void ageFailurePreservesItsLastQueriedTargetAndReferenceFrontier()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    microphone_metrics.completed_samples = 4480;
    microphone_metrics.last_completion_us = 480000;
    queueAec(service, 2400);
    service.pollReferenceMonitor();
    service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 16 && service.reference_history_pending_.load());
    assert(service.aec_target_waiting_.load() && submitted_pairs.empty());
    const auto waiting = service.aecServiceSnapshot();
    assert(waiting.coherent && (waiting.words[4] & 2));
    assert(waiting.words[11] == 2400 && waiting.words[12] == 0);
    assert(waiting.words[13] > 0 && waiting.words[13] < 320);
    assert(waiting.words[14] == 0 && waiting.words[15] == waiting.words[13]);
    assert(waiting.words[16] == 2);  // 真实掩码证明尾部缺失。
    assert(waiting.words[17] == 16 && waiting.words[18] == 20 && waiting.words[19] == 4);
    const uint64_t frontier = (static_cast<uint64_t>(waiting.words[21]) << 32) | waiting.words[20];
    const uint64_t oldest = (static_cast<uint64_t>(waiting.words[23]) << 32) | waiting.words[22];
    assert((waiting.words[24] & 3) == 3 && oldest < 2400 && frontier > 2400 && frontier < 2720);

    // 更晚的 RX 完成在下一次查询前使同一队首过期。
    // 失败记录必须保留上次查询的身份与掩码，不能悄悄把无关全局有效计数
    // 关联到新发布的仅年龄尝试。
    microphone_metrics.completed_samples = 23 * 256;
    microphone_metrics.last_completion_us = 200000 + 23 * 16000;
    service.pollAecReference(microphone_metrics.completed_samples);
    assert(hasFailure("pair_age") && !service.frontend_.aecActive());
    const auto &frozen = service.aec_failure_snapshot_.service;
    assert(frozen.coherent && frozen.words[0] == 2400 && !(frozen.words[4] & 2));
    assert(frozen.words[7] == 0 && frozen.words[5] > 3200);
    for (size_t word = 11; word < waiting.words.size(); ++word)
        assert(frozen.words[word] == waiting.words[word]);
    assert(service.aec_failure_snapshot_.microphone.completed_samples == 23 * 256);
}

void coldCaptureFeedsKnownMuteInOrder()
{
    capture_read_budget = 2;
    Fixture fixture;
    auto &service = fixture.service;
    assert(raw_frames == 2 && uxQueueMessagesWaiting(service.aec_capture_queue_) == 2);
    service.pollAecReference(2240);
    assert(service.aec_error_.load() == ESP_OK && service.frontend_.aecActive());
    assert(service.aec_pairs_.load() == 2 && service.aec_initial_silence_.load() == 2);
    assert(submitted_pairs.size() == 2 && uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
    for (size_t i = 0; i < submitted_pairs.size(); ++i) {
        assert(std::all_of(submitted_pairs[i].reference.begin(), submitted_pairs[i].reference.end(),
            [](int16_t sample) { return sample == 0; }));
        assert(std::all_of(submitted_pairs[i].microphone.begin(), submitted_pairs[i].microphone.end(),
            [i](int16_t sample) { return sample == static_cast<int16_t>(100 + i); }));
    }
}

void completeContentFeedsActualReference()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    for (size_t i = 0; i < 3; ++i) service.pollReferenceMonitor();
    queueAec(service, 1600, false, 731);
    service.pollReferenceMonitor();
    assert(service.aec_error_.load() == ESP_OK && submitted_pairs.size() == 1);
    assert(service.aec_last_valid_samples_.load() == 320 && service.aec_initial_silence_.load() == 0);
    assert(std::all_of(submitted_pairs[0].microphone.begin(), submitted_pairs[0].microphone.end(),
        [](int16_t sample) { return sample == 731; }));
    assert(std::all_of(submitted_pairs[0].reference.begin(), submitted_pairs[0].reference.end(),
        [](int16_t sample) { return std::abs(static_cast<int>(sample) - 2000) <= 4; }));
}

void pendingHistoryFeedsItsCompleteTarget()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    queueAec(service, 1600, false, 731);
    service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 8 && service.reference_history_pending_.load());
    assert(submitted_pairs.empty() && service.aec_error_.load() == ESP_OK);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);

    // 下一批已覆盖此帧，即使还有更新的 TX 块待处理。
    // 等待全局尾部会无谓增加一个已有效配对的年龄。
    service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 16 && service.reference_history_pending_.load());
    assert(submitted_pairs.size() == 1 && service.aec_last_valid_samples_.load() == 320);
    assert(service.aec_error_.load() == ESP_OK && uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
    assert(std::all_of(submitted_pairs[0].microphone.begin(), submitted_pairs[0].microphone.end(),
        [](int16_t sample) { return sample == 731; }));
    assert(std::all_of(submitted_pairs[0].reference.begin(), submitted_pairs[0].reference.end(),
        [](int16_t sample) { return std::abs(static_cast<int>(sample) - 2000) <= 4; }));
    service.pollReferenceMonitor();
    assert(!service.reference_history_pending_.load() && submitted_pairs.size() == 1);
    assert(service.aec_error_.load() == ESP_OK && uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
}

void growingPendingTailCannotStarveAnOlderCompleteTarget()
{
    Fixture fixture;
    auto &service = fixture.service;
    queueAec(service, 1600);
    for (size_t i = 0; i < 4; ++i) {
        // 本工作任务转换有界批次时，运行中的 TX 生产者持续推进。
        // 所有这些服务机会中均保留更新的待处理尾部。
        setHistory(40 + i * 8, true);
        microphone_metrics.completed_blocks = static_cast<uint32_t>(20 + i * 4);
        microphone_metrics.completed_samples = microphone_metrics.completed_blocks * 160;
        microphone_metrics.last_completion_us = 200000 + microphone_metrics.completed_blocks * 10000;
        service.pollReferenceMonitor();
        assert(service.reference_history_pending_.load());
        assert(service.aec_error_.load() == ESP_OK);
        assert(submitted_pairs.size() == (i ? 1 : 0));
    }
    // RX 时钟现已推进到足够远，若帧仍在队列将触发 pair_age。
    // 该完整目标已在第 2 次机会送入前端。
    assert(microphone_metrics.completed_samples - 1600 > 3200);
    assert(service.aec_pairs_.load() == 1 && !hasFailure("pair_age"));
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
}

void liveDmaPacingCatchesTheTargetBeforeABlockingPlaybackWrite()
{
    Fixture fixture;
    auto &service = fixture.service;
    const int64_t saved_now = now_us;
    constexpr int64_t kClockOrigin = 200000;
    now_us = kClockOrigin + 340000;
    setHistory(18, true);
    // 物理时钟相互独立：RX 每 16 ms 完成 256 样本，TX 每 10 ms 完成 240 样本。
    // 持有者开始时，已转换 TX 落后于最早保留的麦克风帧，未超前于其目标。
    microphone_metrics.completed_blocks = 21;
    microphone_metrics.completed_samples = 21 * 256;
    microphone_metrics.last_completion_us = kClockOrigin + 21 * 16000;
    for (unsigned i = 0; i < 3; ++i) service.pollReferenceMonitor();
    assert(service.reference_history_cursor_ == 18 && submitted_pairs.empty());
    constexpr uint64_t kFirstRetainedSample = 3200;
    for (uint64_t first = kFirstRetainedSample; first < 5120; first += 320)
        queueAec(service, first, false, static_cast<int16_t>(first / 320 + 100));
    uint64_t next_capture_first = 5120;
    unsigned producer_drops = 0;
    clock_tick = [&] {
        const uint64_t elapsed = static_cast<uint64_t>(now_us - kClockOrigin);
        const size_t tx_blocks = static_cast<size_t>(elapsed / 10000);
        while (history.size() < tx_blocks) {
            const size_t index = history.size();
            PlaybackReference::Block block = history.back();
            block.history_sequence = static_cast<uint32_t>(index + 1);
            block.first_sample_24k = index * 240;
            block.observed_end_us = kClockOrigin + static_cast<int64_t>((index + 1) * 10000);
            history.push_back(block);
        }
        const uint64_t rx_blocks = elapsed / 16000;
        microphone_metrics.completed_blocks = static_cast<uint32_t>(rx_blocks);
        microphone_metrics.completed_samples = rx_blocks * 256;
        microphone_metrics.last_completion_us = kClockOrigin + static_cast<int64_t>(rx_blocks * 16000);
        while (next_capture_first + 320 <= microphone_metrics.completed_samples) {
            AudioService::AecCaptureFrame frame{};
            frame.first_sample = next_capture_first;
            frame.pcm.fill(static_cast<int16_t>(next_capture_first / 320 + 100));
            if (xQueueSend(service.aec_capture_queue_, &frame, 0) != pdTRUE) ++producer_drops;
            next_capture_first += 320;
        }
    };
    clock_tick();
    // 在复制/处理交接中注入有界实际耗时。复制两块即耗尽原有 4 ms 预算，
    // 同时生产者继续运行。待处理探测也复制一个块，但不推进游标。
    history_copy_cost_us = 2100;
    unsigned writes = 0;
    size_t pairs_before_write = 0;
    playback_write = [&] {
        ++writes;
        pairs_before_write = submitted_pairs.size();
        now_us += 70000;  // 在 100 ms 超时内合法延迟的写入。
        clock_tick();
        service.running_.store(false);
    };
    AudioService::PlaybackFrame playback{};
    playback.serial = service.serial_.load();
    playback.samples = 240;
    // 五个条目在接收后剩四个，避开独立的启动抖动缓冲等待，
    // 覆盖真实写入前持有者路径。
    for (unsigned i = 0; i < 5; ++i)
        assert(xQueueSend(service.playback_queue_, &playback, 0) == pdTRUE);
    service.playbackTask();
    clock_tick = playback_write = {};
    history_copy_cost_us = 0;
    assert(writes == 1 && pairs_before_write > 0);
    assert(producer_drops == 0 && !hasFailure("pair_age") && !hasFailure("reference_gap"));
    assert(service.aec_error_.load() == ESP_OK && service.frontend_.aecActive());
    // 追赶时不跳过任何帧，产生的参考是真实 PCM。
    for (size_t i = 0; i < submitted_pairs.size(); ++i) {
        const int16_t expected = static_cast<int16_t>(kFirstRetainedSample / 320 + i + 100);
        assert(std::all_of(submitted_pairs[i].microphone.begin(), submitted_pairs[i].microphone.end(),
            [expected](int16_t sample) { return sample == expected; }));
        assert(std::all_of(submitted_pairs[i].reference.begin(), submitted_pairs[i].reference.end(),
            [](int16_t sample) { return std::abs(static_cast<int>(sample) - 2000) <= 4; }));
    }
    now_us = saved_now;
}

void knownMuteFeedsWhileLaterContentStillWaits()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    queueAec(service, 0, true, 401);
    queueAec(service, 1600, false, 402);
    service.pollReferenceMonitor();
    assert(service.reference_history_pending_.load() && submitted_pairs.size() == 1);
    assert(service.aec_error_.load() == ESP_OK && service.aec_initial_silence_.load() == 1);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
    assert(std::all_of(submitted_pairs[0].microphone.begin(), submitted_pairs[0].microphone.end(),
        [](int16_t sample) { return sample == 401; }));
    assert(std::all_of(submitted_pairs[0].reference.begin(), submitted_pairs[0].reference.end(),
        [](int16_t sample) { return sample == 0; }));
    service.pollReferenceMonitor();
    assert(service.reference_history_pending_.load() && submitted_pairs.size() == 2);
    assert(service.aec_error_.load() == ESP_OK && uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
    assert(std::all_of(submitted_pairs[1].microphone.begin(), submitted_pairs[1].microphone.end(),
        [](int16_t sample) { return sample == 402; }));
    assert(std::all_of(submitted_pairs[1].reference.begin(), submitted_pairs[1].reference.end(),
        [](int16_t sample) { return std::abs(static_cast<int>(sample) - 2000) <= 4; }));
}

void incompleteTargetWaitsForPendingHistoryThenFailsExplicitly()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    history[11].interval_kind = PlaybackReference::IntervalKind::Unknown;
    queueAec(service, 1600);
    for (size_t i = 0; i < 2; ++i) {
        service.pollReferenceMonitor();
        assert(service.reference_history_pending_.load());
        assert(submitted_pairs.empty() && service.aec_error_.load() == ESP_OK);
        assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
    }
    service.pollReferenceMonitor();
    assert(!service.reference_history_pending_.load());
    assert(hasFailure("reference_gap") && submitted_pairs.empty());
    assert(service.aec_error_.load() == ESP_ERR_INVALID_STATE && !service.frontend_.aecActive());
    assert(service.aec_last_valid_samples_.load() < 320);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
}

void provenSilentScopeBoundariesKeepFullPairs()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(30, true);
    // 连续 TX 时钟承载启动静音、回答 1、全零 SD 转换及回答 2。
    // 应用序号变化不能重置 FIR，也不能阻止跨物理相邻零边界的插值。
    for (size_t i = 0; i < history.size(); ++i) {
        history[i].playback_serial = i < 10 ? 0 : (i < 20 ? 2 : 3);
        if (i < 11 || i == 20) {
            history[i].interval_kind = PlaybackReference::IntervalKind::DigitalSilence;
            history[i].known_silence = true;
            history[i].mono.fill(0);
        }
        if (i == 10 || i == 20) history[i].amplifier_boundary = true;
    }
    for (size_t i = 0; i < 4; ++i) service.pollReferenceMonitor();
    queueAec(service, 1600, false, 503);
    queueAec(service, 3200, false, 504);
    service.pollReferenceMonitor();
    assert(service.aec_error_.load() == ESP_OK && submitted_pairs.size() == 2);
    assert(service.aec_pairs_.load() == 2 && service.aec_initial_silence_.load() == 0);
    assert(service.aec_last_valid_samples_.load() == 320);
    assert(service.reference_monitor_->metrics().gap_resets == 0);
    assert(service.reference_monitor_->metrics().warmup_samples == 63);
    assert(service.reference_monitor_->metrics().unknown_blocks == 0);
    for (const auto &pair : submitted_pairs)
        assert(std::any_of(pair.reference.begin(), pair.reference.end(),
            [](int16_t sample) { return sample > 1000; }));
}

void incompleteReferenceFailsWithoutZeroPadding()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    history[11].interval_kind = PlaybackReference::IntervalKind::Unknown;
    for (size_t i = 0; i < 3; ++i) service.pollReferenceMonitor();
    queueAec(service, 1600);
    service.pollReferenceMonitor();
    assert(service.aec_error_.load() == ESP_ERR_INVALID_STATE && !service.frontend_.aecActive());
    assert(hasFailure("reference_gap") && submitted_pairs.empty());
    assert(service.aec_last_valid_samples_.load() < 320);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
}

void temporaryFrontendBackpressurePreservesThePairHeadAndExactAudio()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(30, true);
    for (size_t block = 0; block < history.size(); ++block)
        for (size_t sample = 0; sample < history[block].mono.size(); ++sample)
            history[block].mono[sample] = static_cast<int16_t>(
                static_cast<int>(((block * 240 + sample) * 37) % 6000) - 3000);
    for (unsigned poll = 0; poll < 4; ++poll) service.pollReferenceMonitor();
    assert(!service.reference_history_pending_.load());

    // 真实队列仍为八帧。六帧配对突发到达时，已有四个旧帧等待合成 feed 消费者。
    // 此替身模拟交接容量，不模拟 ESP-SR CPU 执行或 RTOS。
    frontend_capacity = 8;
    frontend_pending.resize(4);
    std::vector<Pair> expected(6);
    for (size_t i = 0; i < expected.size(); ++i) {
        const uint64_t first = 1600 + i * 320;
        expected[i].microphone.fill(static_cast<int16_t>(731 + i));
        size_t valid = 0;
        assert(service.reference_monitor_->copyReferenceFrame(
            first, 320, 0, 0, expected[i].reference.data(), 320, &valid));
        assert(valid == 320 && std::any_of(expected[i].reference.begin(),
            expected[i].reference.end(), [](int16_t sample) { return sample != 0; }));
        queueAec(service, first, false, static_cast<int16_t>(731 + i));
    }
    service.pollAecReference(4800);
    assert(submitted_pairs.size() == 4 && frontend_pending.size() == 8);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 2);
    for (unsigned retry = 0; retry < 5; ++retry) {
        service.pollAecReference(4800);
        AudioService::AecCaptureFrame head{};
        assert(xQueuePeek(service.aec_capture_queue_, &head, 0) == pdTRUE);
        assert(head.first_sample == 2880 && !head.known_mute);
        assert(head.pcm == expected[4].microphone);
        assert(submitted_pairs.size() == 4 && uxQueueMessagesWaiting(service.aec_capture_queue_) == 2);
        assert(service.aec_error_.load() == ESP_OK && service.frontend_.aecActive());
        assert(failure_reasons.empty() && service.aec_pairs_.load() == 4);
    }
    assert(paired_blocked_calls == 5);
    assert(service.aec_frontend_waits_.load() == 5);
    assert(service.aec_frontend_waiting_.load());

    // 给 feed 释放一个槽位，恰好转移原队首；
    // 下一帧留在原处，直到再有槽位可用。
    frontend_processed.push_back(frontend_pending.front());
    frontend_pending.pop_front();
    service.pollAecReference(4800);
    assert(submitted_pairs.size() == 5 && frontend_pending.size() == 8);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
    AudioService::AecCaptureFrame head{};
    assert(xQueuePeek(service.aec_capture_queue_, &head, 0) == pdTRUE);
    assert(head.first_sample == 3200 && head.pcm == expected[5].microphone);
    while (!frontend_pending.empty()) {
        frontend_processed.push_back(frontend_pending.front());
        frontend_pending.pop_front();
    }
    service.pollAecReference(4800);
    assert(frontend_pending.size() == 1 && submitted_pairs.size() == expected.size());
    frontend_processed.push_back(frontend_pending.front());
    frontend_pending.pop_front();
    assert(frontend_processed.size() == 4 + expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        assert(submitted_pairs[i].microphone == expected[i].microphone);
        assert(submitted_pairs[i].reference == expected[i].reference);
        assert(frontend_processed[4 + i].microphone == expected[i].microphone);
        assert(frontend_processed[4 + i].reference == expected[i].reference);
    }
    service.pollAecReference(4800);  // 空轮询不能重复尾部。
    assert(submitted_pairs.size() == expected.size() && service.aec_pairs_.load() == expected.size());
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
    assert(service.aec_error_.load() == ESP_OK && failure_reasons.empty());
    assert(service.aec_frontend_waits_.load() == 6 && service.aec_initial_silence_.load() == 0);
    assert(!service.aec_frontend_waiting_.load());
}

void idlePlaybackRetriesACompleteBackpressuredPairBeforeTheFullFrameWait()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(30, true);
    for (unsigned poll = 0; poll < 4; ++poll) service.pollReferenceMonitor();
    assert(!service.reference_history_pending_.load());
    // 该配对已到处理时机，距原有 200 ms 保护仅剩 20 ms。
    // 完整参考已可用，只有 AFE 交接队列满载。
    std::array<int16_t, 320> expected_reference{};
    size_t valid = 0;
    assert(service.reference_monitor_->copyReferenceFrame(
        1920, 320, 0, 0, expected_reference.data(), 320, &valid));
    assert(valid == 320);
    frontend_capacity = 1;
    frontend_pending.resize(1);
    queueAec(service, 1920, false, 731);
    service.pollAecReference(4800);
    assert(service.aec_frontend_waiting_.load() && !service.aec_target_waiting_.load());
    assert(service.aec_frontend_waits_.load() == 1 && submitted_pairs.empty());

    unsigned waits = 0;
    queue_wait = [&](QueueHandle_t handle, TickType_t ticks, bool write) {
        assert(handle == service.playback_queue_ && !write && !held_mutexes);
        ++waits;
        if (waits == 1) {
            // 真实生产持有者必须阻塞做短重试，
            // 不能持锁自旋，也不能等待通常的完整 20 ms 帧。
            assert(ticks == 1 && uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
            now_us += 1000;
            frontend_processed.push_back(frontend_pending.front());
            frontend_pending.pop_front();
        } else {
            assert(waits == 2 && ticks == pdMS_TO_TICKS(20));
            assert(!service.aec_frontend_waiting_.load());
            assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 0);
            service.running_.store(false);
        }
    };
    service.playbackTask();
    queue_wait = {};
    assert(waits == 2 && submitted_pairs.size() == 1);
    assert(std::all_of(submitted_pairs.front().microphone.begin(),
        submitted_pairs.front().microphone.end(), [](int16_t sample) { return sample == 731; }));
    assert(submitted_pairs.front().reference == expected_reference);
    assert(service.aec_pairs_.load() == 1 && failure_reasons.empty());
}

void persistentFrontendBackpressureStillExpiresAtTheOriginalPairAge()
{
    Fixture fixture;
    auto &service = fixture.service;
    frontend_capacity = 0;
    queueAec(service, 0, true, 912);
    for (const uint64_t end : {1600U, 2240U, 2880U, 3200U}) {
        service.pollAecReference(end);
        assert(submitted_pairs.empty() && uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
        assert(service.aec_error_.load() == ESP_OK && service.frontend_.aecActive());
        assert(service.aec_pairs_.load() == 0 && service.aec_initial_silence_.load() == 0);
    }
    assert(paired_blocked_calls == 4);
    assert(service.aec_frontend_waits_.load() == 4);
    service.pollAecReference(3201);
    assert(hasFailure("pair_age") && !hasFailure("frontend_pair"));
    assert(service.aec_error_.load() == ESP_ERR_TIMEOUT && !service.frontend_.aecActive());
    assert(!service.aec_frontend_waiting_.load());
    assert(submitted_pairs.empty() && uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
    AudioService::AecCaptureFrame head{};
    assert(xQueuePeek(service.aec_capture_queue_, &head, 0) == pdTRUE);
    assert(head.first_sample == 0 && head.known_mute && head.pcm[0] == 912);
}

void persistentIdlePlaybackBackpressureBlocksBetweenRetriesAndKeepsTheAgeGuard()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(12);
    for (unsigned poll = 0; poll < 2; ++poll) service.pollReferenceMonitor();
    frontend_capacity = 0;
    queueAec(service, 0, true, 912);
    service.pollAecReference(1920);
    assert(service.aec_frontend_waiting_.load());
    unsigned waits = 0;
    queue_wait = [&](QueueHandle_t handle, TickType_t ticks, bool write) {
        assert(handle == service.playback_queue_ && !write && !held_mutexes);
        ++waits;
        if (waits <= 9) {
            assert(ticks == 1 && service.aec_frontend_waiting_.load());
            const uint32_t advance = waits == 9 ? 1 : 160;
            microphone_metrics.completed_samples += advance;
            microphone_metrics.last_completion_us += waits == 9 ? 63 : 10000;
            now_us += 1000;
        } else {
            assert(waits == 10 && ticks == pdMS_TO_TICKS(20));
            assert(!service.aec_frontend_waiting_.load());
            assert(service.aec_error_.load() == ESP_ERR_TIMEOUT);
            service.running_.store(false);
        }
    };
    service.playbackTask();
    queue_wait = {};
    assert(waits == 10 && microphone_metrics.completed_samples == 3201);
    assert(hasFailure("pair_age") && !hasFailure("frontend_pair"));
    assert(submitted_pairs.empty() && uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
    AudioService::AecCaptureFrame head{};
    assert(xQueuePeek(service.aec_capture_queue_, &head, 0) == pdTRUE);
    assert(head.first_sample == 0 && head.known_mute && head.pcm[0] == 912);
}

void frontendBackpressureCannotConvertUnknownReferenceIntoSilence()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    history[11].interval_kind = PlaybackReference::IntervalKind::Unknown;
    for (unsigned poll = 0; poll < 3; ++poll) service.pollReferenceMonitor();
    frontend_capacity = 0;
    queueAec(service, 1600, false, 913);
    service.pollReferenceMonitor();
    assert(hasFailure("reference_gap") && submitted_pairs.empty());
    assert(service.aec_error_.load() == ESP_ERR_INVALID_STATE && !service.frontend_.aecActive());
    assert(service.aec_last_valid_samples_.load() < 320 && paired_try_calls == 0);
    assert(uxQueueMessagesWaiting(service.aec_capture_queue_) == 1);
}

void receiveOverflowFailsBeforePairing()
{
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20);
    microphone_metrics.timeline_valid = false;
    microphone_metrics.overflow_events = 1;
    queueAec(service, 1600, true);
    service.pollReferenceMonitor();
    assert(hasFailure("capture_timeline") && submitted_pairs.empty());
    assert(service.aec_error_.load() == ESP_ERR_INVALID_STATE && !service.frontend_.aecActive());
    const auto clock = service.aec_failure_snapshot_.clock;
    assert(clock.valid && clock.source == voiceaudio::ReferenceTimeline::ClockSource::Microphone);
    assert(clock.reason == voiceaudio::ReferenceTimeline::ClockFailureReason::MicrophoneOverflow);
}

void firstClockFailureIsPublishedBeforePairingAndRetainedUntilRestart()
{
    using Timeline = voiceaudio::ReferenceTimeline;
    Fixture fixture;
    auto &service = fixture.service;
    setHistory(20, true);
    for (unsigned poll = 0; poll < 3; ++poll) service.pollReferenceMonitor();
    assert(!service.reference_clock_failure_ready_.load());
    // 真实生产时钟观测到 RX 硬跳变，而有效 DMA 未变且没有队列溢出。
    // 采集坐标仍使队首到达处理时机。
    microphone_metrics.completed_samples += 256;
    microphone_metrics.last_completion_us += 76000;  // 16 ms 样本加 60 ms 跳变。
    queueAec(service, 1600);
    service.pollReferenceMonitor();
    assert(hasFailure("reference_gap") && submitted_pairs.empty());
    assert(service.reference_clock_failure_ready_.load());
    const auto first = service.aec_failure_snapshot_.clock;
    assert(first.valid && first.source == Timeline::ClockSource::Microphone);
    assert(first.reason == Timeline::ClockFailureReason::ClockJump);
    assert(first.sample_end == microphone_metrics.completed_samples);
    assert(first.observed_end_us == static_cast<int64_t>(microphone_metrics.last_completion_us));
    assert(first.residual_us == 60000);
    // 后续驱动失败不能覆盖时间线的首个原因，
    // 也不能覆盖跨任务失败采集读取的不可变发布记录。
    microphone_metrics.timeline_valid = false;
    microphone_metrics.overflow_events = 1;
    service.pollReferenceMonitor();
    assert(service.reference_clock_failure_.reason == first.reason);
    assert(service.aec_failure_snapshot_.clock.sample_end == first.sample_end);
    service.logPendingAecFailure();
    assert(std::any_of(log_messages.begin(), log_messages.end(), [](const std::string &message) {
        return message.find("aec_clock_failure valid=1 source=microphone reason=clock_jump") == 0 &&
               message.find("residual_us=60000") != std::string::npos;
    }));
    // 所有旧工作任务退出后，才执行生命周期重置。
    assert(service.stop() == ESP_OK);
    assert(service.start([](const int16_t *, size_t) {}, {}, {}, 1, 40) == ESP_OK);
    assert(!service.reference_clock_failure_ready_.load());
    assert(!service.reference_clock_failure_.valid && !service.aec_failure_snapshot_.clock.valid);
}

void ageQueueAndEnqueueFailuresStayExplicit()
{
    {
        Fixture fixture;
        queueAec(fixture.service, 0, true);
        fixture.service.pollAecReference(3201);
        assert(hasFailure("pair_age") && submitted_pairs.empty());
        assert(fixture.service.aec_error_.load() == ESP_ERR_TIMEOUT);
        assert(uxQueueMessagesWaiting(fixture.service.aec_capture_queue_) == 1);
    }
    {
        capture_read_budget = 11;
        Fixture fixture;
        assert(raw_frames == 11 && hasFailure("pair_queue_full"));
        assert(fixture.service.aec_error_.load() == ESP_ERR_TIMEOUT && submitted_pairs.empty());
        assert(uxQueueMessagesWaiting(fixture.service.aec_capture_queue_) == 10);
    }
    {
        Fixture fixture;
        queueAec(fixture.service, 0, true);
        paired_enqueue_result = ESP_ERR_TIMEOUT;
        fixture.service.pollAecReference(1600);
        assert(hasFailure("frontend_pair") && submitted_pairs.empty());
        assert(fixture.service.aec_error_.load() == ESP_ERR_TIMEOUT && !fixture.service.frontend_.aecActive());
    }
}

void aecScratchFailureKeepsRawAndReleases()
{
    fail_allocation_size = sizeof(AudioService::AecScratch);
    capture_read_budget = 1;
    Fixture fixture;
    assert(hasFailure("reference_resources") && !fixture.service.frontend_.aecActive());
    assert(fixture.service.aec_error_.load() == ESP_ERR_NO_MEM && raw_frames == 1);
    assert(!fixture.service.aec_scratch_ && submitted_pairs.empty());
}
#endif

void playbackDiagnosticsSeparateDecodeAndQueueBackpressure()
{
    Fixture fixture;
    auto &service = fixture.service;
    const voice::Identity identity{7, 8, 9};
    assert(service.beginPlayback(identity, 11, true, 0) == ESP_OK);
    decode_cost_us = 700;
    const uint8_t payload = 1;
    assert(service.feedPlayback(identity, 11, 0, 0, &payload, 1) == ESP_OK);
    assert(service.feedPlayback(identity, 11, 1, 480, &payload, 1) == ESP_OK);
    AudioService::PlaybackFrame filler{};
    filler.serial = service.serial_.load();
    while (uxQueueMessagesWaiting(service.playback_queue_) < 50)
        assert(xQueueSend(service.playback_queue_, &filler, 0) == pdTRUE);
    queue_wait = [&](QueueHandle_t handle, TickType_t ticks, bool write) {
        assert(handle == service.playback_queue_ && write && ticks == 20);
        now_us += 9000;
        static_cast<Queue *>(handle)->values.pop_front();
    };
    service.playback_work_.enqueue_wait_total_us.store(UINT32_MAX - 1);
    assert(service.feedPlayback(identity, 11, 2, 960, &payload, 1) == ESP_OK);
    assert(service.playback_work_.decoded_packets.load() == 3);
    assert(service.playback_work_.decoded_samples.load() == 1440);
    assert(service.playback_work_.decode_last_us.load() == 700 && service.playback_work_.decode_max_us.load() == 700);
    assert(service.playback_work_.enqueue_waits.load() == 1 && service.playback_work_.enqueue_wait_max_us.load() == 9000);
    assert(service.playback_work_.enqueue_wait_total_us.load() == UINT32_MAX);
    assert(service.playback_work_.queue_peak.load() == 50);
    assert(service.endPlayback(identity, 11, 1440) == ESP_OK && service.playback_work_.end_received.load());
    queue_wait = {};
    assert(service.stopPlayback() == ESP_OK);
    assert(service.playback_work_.decoded_packets.load() == 3);  // 保留已结束/取消的作用域，供 ? 查询。
    const voice::Identity next{7, 9, 10};
    assert(service.beginPlayback(next, 12, false, 0) == ESP_OK);
    assert(service.playback_work_.serial.load() == service.serial_.load());
    assert(service.playback_work_.epoch.load() == 7 && service.playback_work_.turn.load() == 9);
    assert(service.playback_work_.generation.load() == 10 && service.playback_work_.stream.load() == 12);
    assert(!service.playback_work_.decoded_packets.load() && !service.playback_work_.decode_max_us.load());
    assert(!service.playback_work_.enqueue_waits.load() && !service.playback_work_.queue_peak.load());
    assert(!service.playback_work_.end_received.load() && !service.playback_last_write_end_us_);
    service.logPlaybackReference();
    assert(std::find(events.begin(), events.end(), "frontend-scheduling") != events.end());
    assert(std::any_of(log_messages.begin(), log_messages.end(), [](const std::string &line) {
        return line.find("playback_flow scope=stream coherent=1") != std::string::npos &&
               line.find("epoch=7 turn=9 generation=10 stream=12") != std::string::npos;
    }));
    assert(std::any_of(log_messages.begin(), log_messages.end(), [](const std::string &line) {
        return line.find("empty_wait_is_not_dma_underrun=1 dma_underruns=unmeasured") != std::string::npos;
    }));
}

void emptyPlaybackWaitsDoNotLeakAcrossScopesOrCountAnEndedStream()
{
    Fixture fixture;
    auto &service = fixture.service;
    assert(service.beginPlayback({1, 2, 3}, 4, false, 0) == ESP_OK);
    service.playing_.store(true);
    queue_wait = [&](QueueHandle_t handle, TickType_t, bool write) {
        assert(handle == service.playback_queue_ && !write);
        now_us += 23000;
        service.running_.store(false);
    };
    service.playbackTask();
    assert(service.playback_work_.empty_waits.load() == 1);
    assert(service.playback_work_.empty_wait_max_us.load() == 23000);
    assert(service.playback_work_.empty_wait_total_us.load() == 23000);
    service.running_.store(true);
    service.playback_work_.end_received.store(true);
    service.playbackTask();
    assert(service.playback_work_.empty_waits.load() == 1);
    service.running_.store(true);
    service.playback_work_.end_received.store(false);
    queue_wait = [&](QueueHandle_t handle, TickType_t, bool write) {
        assert(handle == service.playback_queue_ && !write && !held_mutexes);
        now_us += 33000;
        assert(service.beginPlayback({1, 5, 6}, 7, false, 0) == ESP_OK);
        service.running_.store(false);
    };
    service.playbackTask();
    queue_wait = {};
    assert(service.playback_work_.turn.load() == 5 && service.playback_work_.stream.load() == 7);
    assert(!service.playback_work_.empty_waits.load() && !service.playback_work_.empty_wait_total_us.load());
}

void playbackFramesKeepExactPcmAndReleaseLocksBeforeBudgetWait()
{
    Fixture fixture;
    auto &service = fixture.service;
    assert(service.beginPlayback({11, 12, 13}, 14, false, 0) == ESP_OK);
    setHistory(20, true);
    pump_cost_us = 100;  // 已接受写入之间可观测的播放持有者工作。
    std::vector<int16_t> expected;
    for (unsigned j = 0; j < 8; ++j) {
        AudioService::PlaybackFrame frame{};
        frame.serial = service.serial_.load();
        frame.samples = 480;
        for (unsigned i = 0; i < frame.samples; ++i) frame.pcm[i] = static_cast<int16_t>(j * 480 + i + 1);
        expected.insert(expected.end(), frame.pcm.begin(), frame.pcm.end());
        assert(service.enqueue(frame) == ESP_OK);
    }
    unsigned writes = 0, waits = 0;
    size_t first_chunk_history_reads = 0;
    playback_write = [&] {
        ++writes;
        assert(held_mutexes == 1);  // 真实调用方仍持有扬声器屏障。
        now_us += 500;
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
        if (writes % 2) first_chunk_history_reads = consumed_history_reads;
        else assert(consumed_history_reads == first_chunk_history_reads);  // 块之间不执行重型转换。
#else
        static_cast<void>(first_chunk_history_reads);
        if (writes == 4) service.running_.store(false);
#endif
    };
    task_delay = [&](TickType_t ticks) {
        assert(ticks == 1 && !held_mutexes);
        ++waits;
        assert(writes == 4 && played_pcm.size() == 960);  // 计数预算耗尽前处理两个完整帧。
        service.running_.store(false);
    };
    service.playbackTask();
    playback_write = {};
    task_delay = {};
    assert(writes == 4 && played_pcm.size() == 960);
    assert(std::equal(played_pcm.begin(), played_pcm.end(), expected.begin()));
    assert(service.playback_work_.write_calls.load() == 4 && service.playback_work_.write_samples.load() == 960);
    assert(service.playback_work_.write_max_us.load() == 500);
    assert(service.playback_work_.write_gap_max_us.load() >= 100);
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    assert(waits == 1 && service.playback_work_.budget_waits.load() == 1);
#else
    assert(waits == 0 && service.playback_work_.budget_waits.load() == 0);
#endif
}

#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
void startupWaitsForActualSamplesAndKeepsReferenceServiceAlive()
{
    Fixture fixture;
    auto &service = fixture.service;
    assert(service.beginPlayback({31, 32, 33}, 34, false, 0) == ESP_OK);
    AudioService::PlaybackFrame frame{};
    frame.serial = service.serial_.load();
    frame.samples = 240;  // 部分首包按 10 ms 统计，不按 20 ms 统计。
    frame.pcm.fill(3456);
    assert(service.enqueue(frame) == ESP_OK);
    unsigned waits = 0;
    size_t previous_reference_reads = consumed_history_reads;
    setHistory(20, true);
    task_delay = [&](TickType_t ticks) {
        assert(ticks == 5 && !held_mutexes && played_pcm.empty());
        assert(std::find(events.begin(), events.end(), "speaker-unmute") == events.end());
        if (waits) assert(consumed_history_reads > previous_reference_reads);
        previous_reference_reads = consumed_history_reads;
        ++waits;
        // 七个 20 ms 帧加此部分帧仍然总共只有 150 ms。
        for (unsigned i = 0; i < (waits == 1 ? 7U : 1U); ++i) {
            frame.samples = 480;
            assert(service.enqueue(frame) == ESP_OK);
        }
    };
    playback_write = [&] { service.running_.store(false); };
    service.playbackTask();
    task_delay = {};
    playback_write = {};
    assert(waits == 2 && service.reference_history_cursor_ > 0);
    assert(service.playback_work_.startup_ready.load());
    assert(service.playback_work_.startup_samples.load() == 4080);
    assert(service.playback_work_.startup_wait_us.load() == 10000);
    assert(!service.playback_work_.startup_deadline.load() && !service.playback_work_.startup_end_received.load());
    assert(played_pcm == std::vector<int16_t>(240, 3456));
}

void startupDeadlineShortEndAndCancellationRemainBounded()
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Fixture fixture;
        auto &service = fixture.service;
        const voice::Identity identity{41, 42, 43};
        const bool short_end = scenario == 1;
        assert(service.beginPlayback(identity, 44, short_end, short_end ? 120 : 0) == ESP_OK);
        if (short_end) {
            const uint8_t payload = 1;
            assert(service.feedPlayback(identity, 44, 0, 0, &payload, 1) == ESP_OK);
            assert(service.endPlayback(identity, 44, 360) == ESP_OK);
        } else {
            AudioService::PlaybackFrame frame{};
            frame.serial = service.serial_.load();
            frame.samples = 240;
            frame.pcm.fill(4567);
            assert(service.enqueue(frame) == ESP_OK);
        }
        unsigned waits = 0;
        task_delay = [&](TickType_t ticks) {
            assert(ticks == 5 && !held_mutexes && played_pcm.empty());
            ++waits;
            if (scenario == 2) {
                assert(service.beginPlayback({41, 45, 46}, 47, false, 0) == ESP_OK);
                service.running_.store(false);
            } else if (scenario == 3) service.running_.store(false);
        };
        playback_write = [&] { service.running_.store(false); };
        service.playbackTask();
        task_delay = {};
        playback_write = {};
        if (scenario < 2) {
            assert(service.playback_work_.startup_ready.load());
            assert(service.playback_work_.startup_samples.load() == 240);
            assert(service.playback_work_.startup_deadline.load() == !short_end);
            assert(service.playback_work_.startup_end_received.load() == short_end);
            assert(waits == (short_end ? 0U : 60U));
            assert(service.playback_work_.startup_wait_us.load() == (short_end ? 0U : 300000U));
            assert(played_pcm.size() == 240);
        } else {
            assert(waits == 1 && played_pcm.empty());
            assert(!service.playback_work_.startup_ready.load());
            assert(std::find(events.begin(), events.end(), "speaker-unmute") == events.end());
            if (scenario == 2) assert(service.playback_work_.turn.load() == 45 &&
                                      !service.playback_work_.enqueued_samples.load());
        }
    }
}

void playbackWallBudgetWaitsAfterACompleteFrameAndSkipsStopCancelAndError()
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Fixture fixture;
        auto &service = fixture.service;
        assert(service.beginPlayback({21, 22, 23}, 24, false, 0) == ESP_OK);
        AudioService::PlaybackFrame frame{};
        frame.serial = service.serial_.load();
        frame.samples = 480;
        frame.pcm.fill(2345);
        for (unsigned i = 0; i < 8; ++i) assert(service.enqueue(frame) == ESP_OK);
        unsigned writes = 0, waits = 0, errors = 0;
        service.error_ = [&](esp_err_t error) {
            assert(error == ESP_ERR_TIMEOUT && !held_mutexes);
            ++errors;
            service.running_.store(false);
        };
        playback_write = [&] {
            ++writes;
            now_us += 5000;
            if (scenario == 1) service.running_.store(false);
            if (scenario == 2 && writes == 2) {
                service.serial_.fetch_add(1);  // 外部取消在取互斥锁前立即建立屏障。
                service.running_.store(false);
            }
        };
        if (scenario == 3) playback_result = ESP_ERR_TIMEOUT;
        task_delay = [&](TickType_t ticks) {
            assert(ticks == 1 && !held_mutexes && scenario == 0 && writes == 2);
            ++waits;
            service.running_.store(false);
        };
        service.playbackTask();
        playback_write = {};
        task_delay = {};
        assert(waits == (scenario == 0 ? 1U : 0U));
        assert(service.playback_work_.budget_waits.load() == waits);
        assert(writes == ((scenario == 0 || scenario == 2) ? 2U : 1U));
        assert(errors == (scenario == 3 ? 1U : 0U));
        assert(service.playback_work_.write_samples.load() == (scenario == 3 ? 0U : writes * 240));
    }
}
#endif
}  // 命名空间

namespace voice::audio_idle_guard {
esp_err_t initialize() { return ESP_OK; }
bool cooperate(const std::atomic<bool> &lifecycle, bool active_value, uint32_t max_wait_ms)
{
    assert(!held_mutexes && active_value && max_wait_ms == 8);
    ++idle_guard_checks;
    return lifecycle.load() == active_value;
}
Snapshot snapshot() { return {}; }
}

void *heap_caps_malloc(size_t size, int caps)
{
    ++startup_resource_calls;
    assert(caps & MALLOC_CAP_SPIRAM);
    if (size == fail_allocation_size) { fail_allocation_size = 0; scratch_failed = true; return nullptr; }
    void *result = std::malloc(size); assert(result);
    assert(allocations.emplace(result, size).second);
    return result;
}
void *heap_caps_calloc(size_t n, size_t size, int caps) {
    void *result = heap_caps_malloc(n * size, caps);
    if (result) std::memset(result, 0, n * size);
    return result;
}
void heap_caps_free(void *value) { if (value) { assert(allocations.erase(value) == 1); std::free(value); } }
const char *esp_err_to_name(esp_err_t) { return "mock"; }
void serviceReferenceLog(const char *format, ...) {
    events.emplace_back("log");
    char text[1024]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    log_messages.emplace_back(text);
    if (std::strstr(text, "failed reason=")) failure_reasons.emplace_back(text);
}
int64_t esp_timer_get_time() {
    now_us += timer_step_us;
    if (clock_tick) clock_tick();
    return now_us;
}
QueueHandle_t xQueueCreateWithCaps(unsigned capacity, unsigned item_size, int) {
    ++startup_resource_calls;
    ++live_queues; return new Queue{capacity, item_size, {}};
}
BaseType_t xQueueSend(QueueHandle_t handle, const void *data, TickType_t ticks) {
    auto *queue = static_cast<Queue *>(handle);
    if (queue->values.size() == queue->capacity && ticks && queue_wait) queue_wait(handle, ticks, true);
    if (queue->values.size() == queue->capacity) return 0;
    const auto *bytes = static_cast<const uint8_t *>(data);
    queue->values.emplace_back(bytes, bytes + queue->item_size); return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t handle, void *data, TickType_t) {
    auto *queue = static_cast<Queue *>(handle);
    if (queue->values.empty()) return 0;
    std::memcpy(data, queue->values.front().data(), queue->item_size); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *data, TickType_t ticks) {
    if (static_cast<Queue *>(handle)->values.empty() && ticks && queue_wait) queue_wait(handle, ticks, false);
    if (!xQueuePeek(handle, data, ticks)) return 0;
    static_cast<Queue *>(handle)->values.pop_front(); return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t handle) { static_cast<Queue *>(handle)->values.clear(); return pdTRUE; }
unsigned uxQueueMessagesWaiting(QueueHandle_t handle) { return static_cast<unsigned>(static_cast<Queue *>(handle)->values.size()); }
void vQueueDeleteWithCaps(QueueHandle_t handle) { assert(live_queues); --live_queues; delete static_cast<Queue *>(handle); }
SemaphoreHandle_t xSemaphoreCreateMutex() { ++startup_resource_calls; ++live_semaphores; return new Semaphore{true, true}; }
SemaphoreHandle_t xSemaphoreCreateBinary() { ++startup_resource_calls; ++live_semaphores; return new Semaphore{false, false}; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
    auto &semaphore = *static_cast<Semaphore *>(handle);
    if (ticks == 2000) { ++task_stops; return pdTRUE; }
    if (!semaphore.available && ticks == 1000 && schedule_capture_on_start) {
        for (auto &task : tasks) if (task->name == "voice_capture" && !task->ran) runTask(*task);
    }
    if (!semaphore.available) return 0;
    semaphore.available = false;
    if (semaphore.mutex) ++held_mutexes;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
    auto &semaphore = *static_cast<Semaphore *>(handle);
    if (semaphore.mutex) { assert(!semaphore.available && held_mutexes); --held_mutexes; }
    semaphore.available = true; return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t handle) { assert(live_semaphores); --live_semaphores; delete static_cast<Semaphore *>(handle); }
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned, void *argument, unsigned priority, TaskHandle_t *output) {
    ++startup_resource_calls;
    events.emplace_back(std::string("task:") + name);
    tasks.emplace_back(new Task{entry, argument, name, priority, -1});
    *output = tasks.back().get(); return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned, void *argument,
                                  unsigned priority, TaskHandle_t *output, int core) {
    ++startup_resource_calls;
    events.emplace_back(std::string("task:") + name);
    tasks.emplace_back(new Task{entry, argument, name, priority, core});
    *output = tasks.back().get(); return pdPASS;
}
void vTaskDelete(TaskHandle_t) {}
void vTaskDelay(TickType_t ticks) {
    if (task_delay) task_delay(ticks);
    events.emplace_back("delay:" + std::to_string(ticks));
    now_us += static_cast<int64_t>(ticks) * 1000;
}
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(now_us / 1000); }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }

// 真实 AudioService 编排调用这些模拟外设方法。
esp_err_t Ics43434Microphone::init(const Config &config) {
    ++startup_resource_calls;
    events.emplace_back("rx-init");
    config_ = config; ready_ = true; channel_enabled_ = false;
    microphone_metrics = {};
    return config.start_immediately ? startCapture() : ESP_OK;
}
esp_err_t Ics43434Microphone::startCapture() {
    assert(capture_owner && ready_ && !channel_enabled_);
    events.emplace_back("rx-enable");
    if (capture_start_result == ESP_OK) channel_enabled_ = true;
    return capture_start_result;
}
esp_err_t Ics43434Microphone::deinit() { ready_ = false; return ESP_OK; }
Ics43434Microphone::DmaMetrics Ics43434Microphone::dmaMetrics() const { return microphone_metrics; }
const char *Ics43434Microphone::consumerPhaseName(ConsumerPhase phase) {
    switch (phase) {
    case ConsumerPhase::Starting: return "starting";
    case ConsumerPhase::Read: return "read";
    case ConsumerPhase::Convert: return "convert";
    case ConsumerPhase::Callback: return "callback";
    case ConsumerPhase::Queue: return "queue";
    case ConsumerPhase::Stopped: return "stopped";
    }
    return "unknown";
}
void Ics43434Microphone::noteConsumerPhase(ConsumerPhase phase) {
    auto &service = microphone_metrics.service;
    if (service.coherent && service.phase == ConsumerPhase::Callback && phase != ConsumerPhase::Callback) {
        service.last_callback_us = static_cast<uint64_t>(now_us) - service.phase_since_us;
        service.max_callback_us = std::max(service.max_callback_us, service.last_callback_us);
    }
    service.coherent = true;
    service.phase = phase;
    service.core = 0;
    service.phase_since_us = static_cast<uint64_t>(now_us);
    events.emplace_back(std::string("capture-phase:") + consumerPhaseName(phase));
}
esp_err_t Ics43434Microphone::read(int32_t *data, size_t capacity, size_t *count, uint32_t, ReadTiming *timing) {
    assert(capture_owner && channel_enabled_);
    if (!capture_read_calls++) events.emplace_back("rx-first-read");
    noteConsumerPhase(ConsumerPhase::Read);
    ++microphone_metrics.service.read_calls;
    *count = 0;
    if (!capture_read_budget) {
        capture_owner->running_.store(false);
        return ESP_ERR_TIMEOUT;
    }
    --capture_read_budget;
    std::fill_n(data, capacity, static_cast<int32_t>((99 + capture_read_calls) * 65536));
    if (timing) {
        timing->first_sample = microphone_metrics.completed_samples;
        timing->sample_end = timing->first_sample + capacity;
        timing->timing_enabled = timing->continuous = true;
    }
    microphone_metrics.completed_samples += capacity;
    microphone_metrics.timing_enabled = microphone_metrics.timeline_valid = true;
    ++microphone_metrics.service.read_completions;
    *count = capacity;
    return ESP_OK;
}
esp_err_t Max98357aAudio::init(const Config &config) {
    ++startup_resource_calls;
    events.emplace_back("speaker-init");
    config_ = config; ready_ = true; reference_.generation_.store(1); reference_.playback_serial_.store(0);
    reference_.rebind_allowed_ = true;
    return ESP_OK;
}
esp_err_t Max98357aAudio::deinit() { ready_ = false; return ESP_OK; }
esp_err_t Max98357aAudio::setEnabled(bool enabled) {
    events.emplace_back(enabled ? "speaker-unmute" : "speaker-mute");
    enabled_ = enabled; return ESP_OK;
}
bool Max98357aAudio::bindPlaybackSerial(uint32_t serial) { return reference_.bindPlaybackSerial(serial); }
esp_err_t Max98357aAudio::resetPlayback() {
    events.emplace_back("speaker-reset");
    reference_.generation_.fetch_add(1); reference_.playback_serial_.store(0);
    reference_.rebind_allowed_ = true;
    enabled_ = false; return ESP_OK;
}
esp_err_t Max98357aAudio::writeMono(const int16_t *pcm, size_t count, uint32_t) {
    reference_.rebind_allowed_ = false;
    reference_.counters_.submitted_frames.fetch_add(static_cast<uint32_t>(count));
    if (playback_result == ESP_OK) played_pcm.insert(played_pcm.end(), pcm, pcm + count);
    if (playback_write) playback_write();
    return playback_result;
}
esp_err_t Max98357aAudio::drain(uint32_t, void (*progress)(void *), void *context) {
    events.emplace_back("speaker-drain");
    if (progress) progress(context);
    if (drain_result == ESP_OK) reference_.rebind_allowed_ = true;
    return drain_result;
}
void Max98357aAudio::logPlaybackReferenceSnapshot(const PlaybackReference::Metrics &) {}
PlaybackReference::Metrics PlaybackReference::metrics() const {
    Metrics result{};
    result.enabled = result.history_available = true;
    if (!history.empty()) result.latest_history_sequence = history.back().history_sequence;
    return result;
}
bool PlaybackReference::bindPlaybackSerial(uint32_t serial) {
    events.emplace_back("reference-bind");
    if (!serial) return false;
    const uint32_t previous = playback_serial_.load();
    if (previous == serial) return true;
    if ((previous || counters_.submitted_frames.load()) && !rebind_allowed_) return false;
    playback_serial_.store(serial);
    return true;
}
void PlaybackReference::pump() { now_us += pump_cost_us; }
bool PlaybackReference::copyHistoryAfter(uint32_t cursor, Block *destination, bool *overwritten) const {
    if (overwritten) *overwritten = false;
    for (const auto &value : history) if (value.history_sequence > cursor) {
        *destination = value;
        ++consumed_history_reads;
        now_us += history_copy_cost_us;
        if (clock_tick) clock_tick();
        return true;
    }
    return false;
}
LocalAudioFrontend::~LocalAudioFrontend() = default;
esp_err_t LocalAudioFrontend::start() {
    ++startup_resource_calls;
    events.emplace_back("frontend-start");
    enabled_.store(true); ready_.store(true); running_.store(true);
    aec_requested_.store(CONFIG_VOICE_AEC_OBSERVATION_ENABLED);
    aec_active_.store(aecRequested());
    return ESP_OK;
}
esp_err_t LocalAudioFrontend::stop() { running_.store(false); aec_active_.store(false); return ESP_OK; }
esp_err_t LocalAudioFrontend::enqueue(const int16_t *, size_t) { return ESP_OK; }
esp_err_t LocalAudioFrontend::enqueuePaired(const int16_t *mic, const int16_t *reference, size_t count) {
    const esp_err_t result = tryEnqueuePaired(mic, reference, count);
    if (result != ESP_ERR_NOT_FINISHED) return result;
    invalidate(ESP_ERR_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}
esp_err_t LocalAudioFrontend::tryEnqueuePaired(const int16_t *mic, const int16_t *reference, size_t count) {
    assert(aecActive() && count == 320);
    ++paired_try_calls;
    if (paired_enqueue_result != ESP_OK) return paired_enqueue_result;
    if (frontend_pending.size() == frontend_capacity) {
        ++paired_blocked_calls;
        return ESP_ERR_NOT_FINISHED;
    }
    Pair pair{};
    std::copy_n(mic, count, pair.microphone.begin());
    std::copy_n(reference, count, pair.reference.begin());
    submitted_pairs.push_back(pair);
    frontend_pending.push_back(pair);
    return ESP_OK;
}
void LocalAudioFrontend::invalidate(esp_err_t error, const char *) { last_error_.store(error); ready_.store(false); aec_active_.store(false); }
void LocalAudioFrontend::logSchedulingDiagnostics() const { assert(!held_mutexes); events.emplace_back("frontend-scheduling"); }
void SrModelStore::unload() {}
esp_err_t voiceaudio::OpusDecoder::open(uint32_t) { return ESP_OK; }
void voiceaudio::OpusDecoder::close() {}
esp_err_t voiceaudio::OpusDecoder::decode(const uint8_t *, size_t, int16_t *pcm, size_t capacity, size_t *samples) {
    now_us += decode_cost_us;
    std::fill_n(pcm, capacity, 1234);
    *samples = capacity;
    return ESP_OK;
}
void voice::memory::log(const char *) { events.emplace_back("memory-log"); }

esp_err_t voiceaudio::runAfeSignalProbe()
{
    ++signal_probe_calls;
    events.emplace_back("afe-signal-probe");
    if (signal_probe_hook) signal_probe_hook();
    return signal_probe_result;
}

int main()
{
    invalidStartupArgumentsDoNotRunTheProbeOrInitializeResources();
#if CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT
    probeErrorsReturnUnchangedBeforeAnyNormalStartupAndPermitACleanRetry();
#endif
    startupProbeRunsBeforeNormalStartupOrIsAbsentFromOrdinaryBuilds();
    boundedBatchAndOrderedContinuation();
    softBudgetAndPumpAccounting();
    staleCaptureAndOneAnalysisPerOpportunity();
    scratchFailureKeepsDigitalAndStopReleases();
    deferredCaptureStartsOnItsConsumer();
    captureDiagnosticsBracketTheRawCallback();
    captureEnableFailureIsReturnedAndReleased();
    captureTaskStartupTimeoutReleasesResources();
    playbackBoundaryUsesTheConfiguredFence();
    playbackDiagnosticsSeparateDecodeAndQueueBackpressure();
    emptyPlaybackWaitsDoNotLeakAcrossScopesOrCountAnEndedStream();
    playbackFramesKeepExactPcmAndReleaseLocksBeforeBudgetWait();
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
    startupWaitsForActualSamplesAndKeepsReferenceServiceAlive();
    startupDeadlineShortEndAndCancellationRemainBounded();
    playbackWallBudgetWaitsAfterACompleteFrameAndSkipsStopCancelAndError();
    emptyReplyCanCancelAndRebindWithoutChangingDma();
    aecSummaryPreservesWideCoordinatesAndRejectsInProgressPublication();
    firstFailureDetailsRemainFrozenUntilOneDeferredFlush();
    ageFailurePreservesItsLastQueriedTargetAndReferenceFrontier();
    coldCaptureFeedsKnownMuteInOrder();
    completeContentFeedsActualReference();
    pendingHistoryFeedsItsCompleteTarget();
    growingPendingTailCannotStarveAnOlderCompleteTarget();
    liveDmaPacingCatchesTheTargetBeforeABlockingPlaybackWrite();
    knownMuteFeedsWhileLaterContentStillWaits();
    incompleteTargetWaitsForPendingHistoryThenFailsExplicitly();
    provenSilentScopeBoundariesKeepFullPairs();
    incompleteReferenceFailsWithoutZeroPadding();
    temporaryFrontendBackpressurePreservesThePairHeadAndExactAudio();
    idlePlaybackRetriesACompleteBackpressuredPairBeforeTheFullFrameWait();
    persistentFrontendBackpressureStillExpiresAtTheOriginalPairAge();
    persistentIdlePlaybackBackpressureBlocksBetweenRetriesAndKeepsTheAgeGuard();
    frontendBackpressureCannotConvertUnknownReferenceIntoSilence();
    receiveOverflowFailsBeforePairing();
    firstClockFailureIsPublishedBeforePairingAndRetainedUntilRestart();
    ageQueueAndEnqueueFailuresStayExplicit();
    aecScratchFailureKeepsRawAndReleases();
    std::puts("Passed production AudioService MR/AEC: frame-boundary heavy reference service, exact nonzero playback FIFO, lock-free-boundary 8ms/2-frame waits, stop/cancel/error skip waits, per-stream decode/queue/write diagnostics and scope reset; preserved reference scheduler, delayed-write catch-up, ordered paired handoff, unknown/overflow/queue/3200-3201 age/feed/resource failures and cleanup; deterministic fakes, no hardware timing or acoustic claims");
#else
    std::puts("Passed production AudioService M: unchanged playback scheduling, exact nonzero FIFO, per-stream decode/queue/write diagnostics and scope reset; reference scheduler + deferred RX startup/enable failure and cleanup; deterministic fakes, no hardware timing claims");
#endif
#if CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT
    std::puts("Passed production AudioService boot signal-probe hook: exact error propagation before callbacks/resources/peripherals/tasks; clean retry, one-call normal startup, duplicate-start guard and stop cleanup; full MR/AEC reference regressions preserved; mocked probe API only, no DSP/acoustic claims");
#else
    std::puts("Passed production AudioService boot signal-probe bypass: ordinary build never calls the probe, including a failing linked mock; startup and cleanup preserved");
#endif
    assert((idle_guard_checks > 0) == static_cast<bool>(CONFIG_VOICE_AEC_OBSERVATION_ENABLED));
}
