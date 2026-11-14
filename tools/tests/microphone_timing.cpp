#include "ics43434_microphone.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

struct MockChannel {
    i2s_event_callbacks_t callbacks{};
    void *user_data = nullptr;
    bool enabled = false;
};

namespace {
MockChannel *channel = nullptr;
std::atomic<int64_t> now_us{0};
bool internal_owner = true, fail_create = false, fail_std = false;
bool fail_register = false, fail_enable = false, fail_disable = false, fail_delete = false;
size_t next_read_bytes = 0;
esp_err_t read_result = ESP_OK;
unsigned register_calls = 0, delete_calls = 0, disable_calls = 0;
unsigned enable_calls = 0, read_calls = 0, log_calls = 0;
uint64_t read_advance_us = 0;
int consumer_core = 0;
void (*during_read)() = nullptr;
void (*during_core_sample)() = nullptr;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void reset()
{
    check(channel == nullptr, "test leaked a live channel");
    internal_owner = true;
    fail_create = fail_std = fail_register = fail_enable = fail_disable = fail_delete = false;
    next_read_bytes = 0;
    read_result = ESP_OK;
    register_calls = delete_calls = disable_calls = 0;
    enable_calls = read_calls = log_calls = 0;
    read_advance_us = 0;
    consumer_core = 0;
    during_read = during_core_sample = nullptr;
    now_us.store(0);
}

Ics43434Microphone::Config observedConfig()
{
    Ics43434Microphone::Config config;
    config.observe_dma_timing = true;
    return config;
}

void receive(size_t bytes = 256 * sizeof(int32_t))
{
    check(channel && channel->enabled && channel->callbacks.on_recv, "DMA callback unavailable");
    i2s_event_data_t event{nullptr, bytes};
    check(!channel->callbacks.on_recv(channel, &event, channel->user_data),
          "timing ISR must not request a task yield");
}

void overflow(size_t bytes = 256 * sizeof(int32_t))
{
    check(channel && channel->enabled && channel->callbacks.on_recv_q_ovf, "overflow callback unavailable");
    i2s_event_data_t event{nullptr, bytes};
    check(!channel->callbacks.on_recv_q_ovf(channel, &event, channel->user_data),
          "overflow ISR must not request a task yield");
}

void defaultAndDegrade()
{
    reset();
    Ics43434Microphone microphone;
    check(microphone.init() == ESP_OK && microphone.isReady(), "default capture starts");
    check(channel->enabled && enable_calls == 1 && microphone.dmaMetrics().capture_started,
          "default immediate initialization remains compatible");
    check(register_calls == 0 && !microphone.dmaMetrics().timing_enabled,
          "default configuration must not install timing callbacks");
    std::array<int32_t, 16> pcm{};
    size_t count = 0;
    next_read_bytes = pcm.size() * sizeof(int32_t);
    Ics43434Microphone::ReadTiming timing;
    check(microphone.read(pcm.data(), pcm.size(), &count, 100, &timing) == ESP_OK && count == 16,
          "default raw read remains functional");
    check(timing.first_sample == 0 && timing.sample_end == 16 && !timing.continuous,
          "unobserved read range is not claimed physically aligned");
    check(microphone.deinit() == ESP_OK, "default capture stops");

    reset();
    fail_register = true;
    check(microphone.init(observedConfig()) == ESP_OK && microphone.isReady(),
          "registration failure must degrade timing only");
    check(register_calls == 1 && !microphone.dmaMetrics().timing_enabled,
          "registration failure is visible");
    next_read_bytes = 4;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_OK && count == 1,
          "raw read survives registration failure");
    check(microphone.deinit() == ESP_OK, "degraded capture stops");

    reset();
    internal_owner = false;
    check(microphone.init(observedConfig()) == ESP_OK && register_calls == 0,
          "external-RAM owner cannot be registered as ISR context");
    check(!microphone.dmaMetrics().timing_enabled, "invalid owner reports unavailable timing");
    check(microphone.deinit() == ESP_OK, "external observer degradation stops cleanly");
}

void deferredCaptureLifecycle()
{
    reset();
    Ics43434Microphone microphone;
    auto config = observedConfig();
    config.start_immediately = false;
    check(microphone.startCapture() == ESP_ERR_INVALID_STATE && enable_calls == 0,
          "uninitialized channel cannot start capture");
    check(microphone.init(config) == ESP_OK && microphone.isReady() && !channel->enabled,
          "deferred init configures a ready channel without enabling RX");
    check(register_calls == 1 && enable_calls == 0, "deferred init installs callbacks before RX starts");
    now_us.store((int64_t{1} << 33) + 5000000);
    auto metrics = microphone.dmaMetrics();
    check(metrics.timing_enabled && !metrics.timeline_valid && !metrics.capture_started &&
          metrics.completed_blocks == 0 && metrics.overflow_events == 0 &&
          !metrics.first_read_started && metrics.capture_start_us == 0,
          "deferred hardware creates no DMA events or valid capture interval while consumers initialize");
    std::array<int32_t, 8> pcm{};
    size_t count = 99;
    Ics43434Microphone::ReadTiming timing;
    check(microphone.read(pcm.data(), pcm.size(), &count, 100, &timing) == ESP_ERR_INVALID_STATE &&
          count == 0 && read_calls == 0 && !microphone.dmaMetrics().first_read_started,
          "configured but stopped channel cannot consume or timestamp a read");
    fail_enable = true;
    check(microphone.startCapture() == ESP_FAIL && microphone.isReady() && !channel->enabled &&
          !microphone.dmaMetrics().capture_started && !microphone.dmaMetrics().timeline_valid,
          "deferred enable failure retains the initialized channel for retry");
    fail_enable = false;
    const uint64_t start_us = (uint64_t{1} << 33) + 5000123;
    now_us.store(static_cast<int64_t>(start_us));
    const auto logs_before_enable = log_calls;
    check(microphone.startCapture() == ESP_OK && channel->enabled && log_calls == logs_before_enable,
          "consumer starts RX without any UART log after enable");
    metrics = microphone.dmaMetrics();
    check(metrics.capture_started && metrics.capture_start_us == start_us && metrics.timeline_valid &&
          metrics.completed_blocks == 0 && !metrics.first_read_started,
          "successful enable publishes its entry timestamp without inventing DMA events");
    now_us.store(static_cast<int64_t>(start_us + 19));
    check(microphone.startCapture() == ESP_ERR_INVALID_STATE && enable_calls == 2 &&
          microphone.dmaMetrics().capture_start_us == start_us,
          "duplicate start neither re-enables nor resets the capture generation");
    check(microphone.read(nullptr, pcm.size(), &count) == ESP_ERR_INVALID_ARG &&
          !microphone.dmaMetrics().first_read_started,
          "invalid read does not publish consumer readiness");
    next_read_bytes = pcm.size() * sizeof(int32_t);
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_OK && read_calls == 1 &&
          microphone.dmaMetrics().first_read_started &&
          microphone.dmaMetrics().first_read_us == start_us + 19,
          "first legal read records its task entry timestamp with coherent 64-bit values");
    now_us.store(static_cast<int64_t>(start_us + 16000));
    receive();
    check(microphone.dmaMetrics().completed_samples == 256 &&
          microphone.dmaMetrics().capture_start_us == start_us,
          "physical DMA completions and startup task clocks remain separate");
    check(microphone.deinit() == ESP_OK && microphone.startCapture() == ESP_ERR_INVALID_STATE,
          "destroyed hardware cannot restart without init");
    check(microphone.init(config) == ESP_OK && !channel->enabled &&
          !microphone.dmaMetrics().capture_started && !microphone.dmaMetrics().first_read_started &&
          microphone.dmaMetrics().capture_start_us == 0 && microphone.dmaMetrics().first_read_us == 0,
          "controlled reinitialization resets startup diagnostic publication");
    check(microphone.deinit() == ESP_OK && disable_calls == 1,
          "unstarted deferred channel deletes without disabling RX");
}

void rangesAndOverflow()
{
    reset();
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "observed capture starts");
    now_us.store((int64_t{1} << 32) + 16000);
    receive();
    now_us.store((int64_t{1} << 32) + 32000);
    receive();
    const auto first = microphone.dmaMetrics();
    check(first.completed_blocks == 2 && first.completed_samples == 512 &&
          first.last_completion_us == (uint64_t{1} << 32) + 32000 && first.timeline_valid,
          "DMA snapshot preserves 64-bit sample and completion clock");
    std::array<int32_t, 320> pcm{};
    size_t count = 0;
    Ics43434Microphone::ReadTiming timing;
    next_read_bytes = 100 * sizeof(int32_t);
    now_us.store((int64_t{1} << 32) + 900000);  // 读取调度延迟不是 DMA 时间戳。
    check(microphone.read(pcm.data(), pcm.size(), &count, 100, &timing) == ESP_OK && count == 100,
          "first partial consumer range");
    check(timing.first_sample == 0 && timing.sample_end == 100 && timing.completed_samples_ahead == 412 &&
          timing.dma_completion_us == first.last_completion_us && timing.continuous,
          "consumer samples associate with observed DMA boundary, independent of task wakeup");
    next_read_bytes = 220 * sizeof(int32_t);
    check(microphone.read(pcm.data(), pcm.size(), &count, 100, &timing) == ESP_OK &&
          timing.first_sample == 100 && timing.sample_end == 320 && timing.completed_samples_ahead == 192,
          "partial reads preserve contiguous sample range");
    read_result = ESP_ERR_TIMEOUT;
    next_read_bytes = 32 * sizeof(int32_t);
    check(microphone.read(pcm.data(), pcm.size(), &count, 100, &timing) == ESP_ERR_TIMEOUT &&
          count == 32 && timing.first_sample == 320 && timing.sample_end == 352,
          "timeout counts data actually consumed by IDF");
    read_result = ESP_OK;
    overflow();
    auto broken = microphone.dmaMetrics();
    check(broken.overflow_events == 1 && broken.overflow_event_samples == 256 && !broken.timeline_valid,
          "queue overflow makes sample association explicitly unavailable");
    check(broken.first_read_started && broken.first_read_us == (uint64_t{1} << 32) + 900000 &&
          broken.first_overflow_us == (uint64_t{1} << 32) + 900000 &&
          broken.first_overflow_sample_end == 512,
          "first read and overflow diagnostics preserve 64-bit clocks and physical cursor");
    now_us.store((int64_t{1} << 32) + 48000);
    receive();
    next_read_bytes = 20 * sizeof(int32_t);
    check(microphone.read(pcm.data(), pcm.size(), &count, 100, &timing) == ESP_OK && !timing.continuous &&
          timing.sample_end == 372 && timing.overflow_events == 1,
          "raw reads continue without silently recovering physical timing");
    check(!microphone.dmaMetrics().timeline_valid, "valid later DMA does not clear an earlier gap");
    check(microphone.deinit() == ESP_OK, "overflowed observer stops");
    check(microphone.init(observedConfig()) == ESP_OK, "controlled restart resets timeline");
    now_us.store(16000);
    receive();
    auto recovered = microphone.dmaMetrics();
    check(recovered.completed_samples == 256 && recovered.overflow_events == 0 && recovered.timeline_valid,
          "controlled restart resets cursor and failure latch");
    check(recovered.first_overflow_us == 0 && recovered.first_overflow_sample_end == 0 &&
          !recovered.first_read_started, "controlled restart resets first-overflow/read diagnostics");
    receive(1023);
    broken = microphone.dmaMetrics();
    check(broken.invalid_events == 1 && !broken.timeline_valid && broken.completed_samples == 256,
          "malformed DMA event cannot invent a valid sample interval");
    check(microphone.deinit() == ESP_OK, "invalid event shutdown");
}

void firstOverflowDiagnostics()
{
    reset();
    Ics43434Microphone microphone;
    const uint64_t started_us = (uint64_t{1} << 33) + 700;
    now_us.store(static_cast<int64_t>(started_us));
    check(microphone.init(observedConfig()) == ESP_OK, "first overflow fixture starts");
    now_us.store(static_cast<int64_t>(started_us + 16000));
    receive();
    now_us.store(static_cast<int64_t>(started_us + 32000));
    receive();
    const auto logs_before_isr = log_calls;
    overflow();
    auto metrics = microphone.dmaMetrics();
    check(metrics.overflow_events == 1 && metrics.first_overflow_sample_end == 512 &&
          metrics.first_overflow_us == started_us + 32000 && log_calls == logs_before_isr &&
          !metrics.first_read_started && !metrics.timeline_valid,
          "overflow before first read is latched without ISR logging");
    check(metrics.first_overflow_service.coherent &&
          metrics.first_overflow_service.phase == Ics43434Microphone::ConsumerPhase::Starting &&
          metrics.first_overflow_service.core == -1 &&
          metrics.first_overflow_service.read_calls == 0,
          "overflow before consumer publication explicitly records starting with no executing core");
    for (unsigned i = 0; i < 6; ++i) {
        now_us.store(static_cast<int64_t>(started_us + 48000 + i * 16000));
        receive();
        overflow();
    }
    metrics = microphone.dmaMetrics();
    check(metrics.overflow_events == 7 && metrics.overflow_event_samples == 1792 &&
          metrics.first_overflow_sample_end == 512 &&
          metrics.first_overflow_us == started_us + 32000,
          "later overflow events increment totals but never replace the first failure clock/cursor");
    check(microphone.deinit() == ESP_OK, "first overflow fixture stops");
}

void serviceReadProgress()
{
    reset();
    using Phase = Ics43434Microphone::ConsumerPhase;
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "service progress fixture starts");
    constexpr uint64_t kStart = (uint64_t{1} << 33) + 123;
    now_us.store(static_cast<int64_t>(kStart));
    read_advance_us = 170000;
    next_read_bytes = 8 * sizeof(int32_t);
    consumer_core = 1;
    std::array<int32_t, 8> pcm{};
    size_t count = 0;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_OK, "first timed read returns");
    auto service = microphone.dmaMetrics().service;
    check(service.coherent && service.phase == Phase::Read && service.core == 1 &&
          service.read_calls == 1 && service.read_completions == 1 &&
          service.first_read_end_us == kStart + 170000 && service.last_read_start_us == kStart &&
          service.last_read_end_us == kStart + 170000 && service.max_read_us == 170000 &&
          service.max_read_gap_us == 0 && service.last_first_sample == 0 && service.last_sample_end == 8,
          "read diagnostics preserve first completion, elapsed duration, counts, range and actual core");
    now_us.store(static_cast<int64_t>(kStart + 171000));
    microphone.noteConsumerPhase(Phase::Convert);
    now_us.store(static_cast<int64_t>(kStart + 172000));
    microphone.noteConsumerPhase(Phase::Callback);
    now_us.store(static_cast<int64_t>(kStart + 173000));
    microphone.noteConsumerPhase(Phase::Callback);
    now_us.store(static_cast<int64_t>(kStart + 372000));
    consumer_core = 0;
    microphone.noteConsumerPhase(Phase::Queue);
    service = microphone.dmaMetrics().service;
    check(service.phase == Phase::Queue && service.core == 0 && service.phase_since_us == kStart + 372000 &&
          service.last_callback_us == 200000 && service.max_callback_us == 200000,
          "callback diagnostic measures total duration and repeated phase notes do not reset its clock");
    now_us.store(static_cast<int64_t>(kStart + 420000));
    read_result = ESP_ERR_TIMEOUT;
    read_advance_us = 11000;
    next_read_bytes = 3 * sizeof(int32_t);
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_ERR_TIMEOUT && count == 3,
          "service diagnostics retain partial timeout behavior");
    service = microphone.dmaMetrics().service;
    check(service.read_calls == 2 && service.read_completions == 2 &&
          service.first_read_end_us == kStart + 170000 && service.last_read_end_us == kStart + 431000 &&
          service.max_read_us == 170000 && service.max_read_gap_us == 250000 &&
          service.last_first_sample == 8 && service.last_sample_end == 11,
          "later reads distinguish between-read service delay from time inside the driver");
    microphone.noteConsumerPhase(Phase::Stopped);
    check(microphone.dmaMetrics().service.phase == Phase::Stopped, "consumer shutdown phase is visible");
    check(microphone.deinit() == ESP_OK, "service progress fixture stops");
}

void firstOverflowServiceProgress()
{
    reset();
    using Phase = Ics43434Microphone::ConsumerPhase;
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "overflow service fixture starts");
    constexpr uint64_t kStart = (uint64_t{1} << 33) + 999;
    now_us.store(static_cast<int64_t>(kStart));
    read_advance_us = 160000;
    consumer_core = 1;
    next_read_bytes = 8 * sizeof(int32_t);
    during_read = [] { overflow(); };
    std::array<int32_t, 8> pcm{};
    size_t count = 0;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_OK, "overflowed read still returns raw samples");
    auto metrics = microphone.dmaMetrics();
    auto service = metrics.first_overflow_service;
    check(service.coherent && service.phase == Phase::Read && service.core == 1 &&
          service.read_calls == 1 && service.read_completions == 0 &&
          service.last_read_start_us == kStart && service.first_read_end_us == 0 &&
          service.last_sample_end == 0 && metrics.first_overflow_us == kStart + 160000 &&
          metrics.service.read_completions == 1,
          "first overflow distinguishes an entered but incomplete read from its later return");
    check(microphone.deinit() == ESP_OK, "in-read overflow fixture stops");

    reset();
    now_us.store(static_cast<int64_t>(kStart));
    check(microphone.init(observedConfig()) == ESP_OK, "callback overflow fixture starts");
    next_read_bytes = 8 * sizeof(int32_t);
    read_advance_us = 10000;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_OK, "callback overflow fixture read");
    now_us.store(static_cast<int64_t>(kStart + 12000));
    microphone.noteConsumerPhase(Phase::Callback);
    now_us.store(static_cast<int64_t>(kStart + 112000));
    overflow();
    metrics = microphone.dmaMetrics();
    service = metrics.first_overflow_service;
    check(service.coherent && service.phase == Phase::Callback &&
          service.phase_since_us == kStart + 12000 && service.read_calls == 1 &&
          service.read_completions == 1 && service.first_read_end_us == kStart + 10000 &&
          service.last_first_sample == 0 && service.last_sample_end == 8 && service.max_read_us == 10000,
          "first overflow retains the callback start and last fully published read range");
    now_us.store(static_cast<int64_t>(kStart + 212000));
    microphone.noteConsumerPhase(Phase::Queue);
    overflow();
    metrics = microphone.dmaMetrics();
    check(metrics.overflow_events == 2 && metrics.first_overflow_service.phase == Phase::Callback &&
          metrics.first_overflow_service.phase_since_us == kStart + 12000 &&
          metrics.service.last_callback_us == 200000 && !metrics.timeline_valid,
          "first consumer snapshot is never replaced by later overflow or raw progress");
    check(microphone.deinit() == ESP_OK, "callback overflow fixture stops");
}

void overlappingServicePublication()
{
    reset();
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "overlapping publication fixture starts");
    now_us.store((int64_t{1} << 33) + 111);
    static Ics43434Microphone *publishing_microphone = nullptr;
    publishing_microphone = &microphone;
    during_core_sample = [] {
        during_core_sample = nullptr;
        check(!publishing_microphone->dmaMetrics().service.coherent,
              "task diagnostics preempting a publication return promptly with absent service progress");
        overflow();
    };
    microphone.noteConsumerPhase(Ics43434Microphone::ConsumerPhase::Convert);
    auto metrics = microphone.dmaMetrics();
    check(metrics.overflow_events == 1 && !metrics.first_overflow_service.coherent &&
          metrics.service.coherent && metrics.service.phase == Ics43434Microphone::ConsumerPhase::Convert,
          "ISR preempting an odd task sequence returns immediately and marks its snapshot absent");
    overflow();
    check(!microphone.dmaMetrics().first_overflow_service.coherent,
          "an incoherent first overflow is never silently replaced by a later coherent snapshot");
    check(microphone.deinit() == ESP_OK, "overlapping publication fixture stops");
}

void lifecycleFailures()
{
    for (int failure = 0; failure < 3; ++failure) {
        reset();
        Ics43434Microphone microphone;
        if (failure == 0) fail_create = true;
        if (failure == 1) fail_std = true;
        if (failure == 2) fail_enable = true;
        check(microphone.init(observedConfig()) != ESP_OK && !microphone.isReady() && channel == nullptr,
              "hardware startup failure releases incomplete channel");
        check(microphone.deinit() == ESP_OK, "failed startup deinit is idempotent");
    }
    reset();
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "shutdown failure fixture");
    fail_disable = true;
    check(microphone.deinit() == ESP_FAIL && channel && channel->enabled && !microphone.isReady() &&
          delete_calls == 0, "failed disable keeps live ISR channel resources");
    now_us.store(16000);
    receive();
    check(microphone.dmaMetrics().completed_samples == 256, "live ISR still has valid owner after failed stop");
    check(microphone.init(observedConfig()) == ESP_ERR_INVALID_ARG,
          "startup cannot overwrite a retained live channel");
    fail_disable = false;
    fail_delete = true;
    check(microphone.deinit() == ESP_FAIL && channel && !channel->enabled,
          "failed deletion retains disabled channel for retry");
    const unsigned disables = disable_calls;
    fail_delete = false;
    check(microphone.deinit() == ESP_OK && channel == nullptr && disable_calls == disables,
          "retry deletion does not disable a stopped channel again");
    check(!microphone.dmaMetrics().timing_enabled, "successful shutdown disables observation");
    check(microphone.deinit() == ESP_OK, "shutdown idempotency");
}

void inputValidation()
{
    reset();
    Ics43434Microphone microphone;
    std::array<int32_t, 8> pcm{};
    size_t count = 99;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_ERR_INVALID_STATE && count == 0,
          "stopped read initializes count");
    auto config = observedConfig();
    config.ws_gpio = config.bclk_gpio;
    check(microphone.init(config) == ESP_ERR_INVALID_ARG && channel == nullptr,
          "invalid pin map cannot allocate a channel");
    check(microphone.init(observedConfig()) == ESP_OK, "input validation fixture");
    check(microphone.read(nullptr, 8, &count) == ESP_ERR_INVALID_ARG && count == 0,
          "null buffer validation");
    check(microphone.read(pcm.data(), SIZE_MAX, &count) == ESP_ERR_INVALID_SIZE,
          "capacity byte multiplication protected");
    next_read_bytes = 3;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_ERR_INVALID_SIZE && count == 0,
          "misaligned driver result rejected");
    check(!microphone.dmaMetrics().timeline_valid,
          "unrepresentable consumed read range invalidates physical association");
    next_read_bytes = pcm.size() * sizeof(int32_t) + 4;
    check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_ERR_INVALID_SIZE && count == 0,
          "oversized driver result rejected");
    check(microphone.deinit() == ESP_OK, "input validation shutdown");
}

void coherentSnapshots()
{
    reset();
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "coherent snapshot fixture");
    constexpr uint64_t kBase = uint64_t{1} << 33;
    constexpr uint32_t kBlocks = 200000;
    constexpr uint32_t kOverflowBlock = 12345;
    std::atomic<bool> finished{false};
    std::thread isr([&] {
        for (uint32_t i = 1; i <= kBlocks; ++i) {
            now_us.store(static_cast<int64_t>(kBase + uint64_t{i} * 16000));
            receive();
            if (i == kOverflowBlock) overflow();
        }
        finished.store(true, std::memory_order_release);
    });
    unsigned snapshots = 0;
    do {
        const auto metrics = microphone.dmaMetrics();
        if (metrics.completed_blocks) {
            check(metrics.completed_samples == uint64_t{metrics.completed_blocks} * 256 &&
                  metrics.last_completion_us == kBase + uint64_t{metrics.completed_blocks} * 16000,
                  "snapshot must not combine different DMA completions");
        }
        if (metrics.overflow_events)
            check(metrics.first_overflow_us == kBase + uint64_t{kOverflowBlock} * 16000 &&
                  metrics.first_overflow_sample_end == uint64_t{kOverflowBlock} * 256 &&
                  metrics.overflow_event_samples == 256 && !metrics.timeline_valid,
                  "first overflow clock/cursor publishes coherently with its ISR failure latch");
        ++snapshots;
    } while (!finished.load(std::memory_order_acquire));
    isr.join();
    check(snapshots && microphone.dmaMetrics().completed_samples == uint64_t{kBlocks} * 256,
          "concurrent reader observes completed ISR work");
    check(microphone.deinit() == ESP_OK, "coherent snapshot shutdown");
}

void coherentServiceSnapshots()
{
    reset();
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "concurrent service snapshot fixture");
    constexpr uint32_t kReads = 20000;
    constexpr uint64_t kStart = (uint64_t{1} << 33) + 456;
    now_us.store(static_cast<int64_t>(kStart));
    next_read_bytes = 8 * sizeof(int32_t);
    std::atomic<bool> finished{false};
    std::thread consumer([&] {
        std::array<int32_t, 8> pcm{};
        size_t count = 0;
        for (uint32_t i = 0; i < kReads; ++i)
            check(microphone.read(pcm.data(), pcm.size(), &count) == ESP_OK && count == 8,
                  "concurrent consumer read");
        finished.store(true, std::memory_order_release);
    });
    unsigned snapshots = 0;
    do {
        const auto service = microphone.dmaMetrics().service;
        ++snapshots;
        if (!service.coherent) continue;
        check((service.read_calls == service.read_completions ||
               service.read_calls == service.read_completions + 1) &&
              service.last_sample_end == uint64_t{service.read_completions} * 8,
              "task snapshot never mixes entered/returned read counts and sample ranges");
        if (service.read_completions)
            check(service.first_read_end_us == kStart && service.last_read_end_us == kStart &&
                  service.last_first_sample + 8 == service.last_sample_end,
                  "task progress snapshots preserve coherent 64-bit completed reads");
    } while (!finished.load(std::memory_order_acquire));
    consumer.join();
    check(snapshots && microphone.dmaMetrics().service.last_sample_end == uint64_t{kReads} * 8,
          "concurrent task reader observes final service progress");
    check(microphone.deinit() == ESP_OK, "concurrent service snapshot fixture stops");
}

void sampleCursorWrap()
{
    reset();
    Ics43434Microphone microphone;
    check(microphone.init(observedConfig()) == ESP_OK, "sample cursor wrap fixture");
    // 固定大小的生产 DMA 块，总量相当于 16 kHz 下超过 74 小时。
    // 驱动真实 ISR 越过 UINT32_MAX，而不是修改私有状态。
    constexpr uint32_t kBlocks = (uint32_t{1} << 24) + 1;
    for (uint32_t i = 0; i < kBlocks; ++i) receive();
    const auto metrics = microphone.dmaMetrics();
    check(metrics.completed_samples == (uint64_t{1} << 32) + 256 && metrics.timeline_valid,
          "64-bit physical sample cursor must survive low-word wrap");
    check(microphone.deinit() == ESP_OK, "sample cursor wrap shutdown");
}
}  // 命名空间

int64_t esp_timer_get_time() { return now_us.load(); }
int xPortGetCoreID()
{
    if (during_core_sample) during_core_sample();
    return consumer_core;
}
void microphoneTimingLog()
{
    check(!channel || !channel->enabled, "configuration UART log must precede RX DMA enable");
    ++log_calls;
}
bool esp_ptr_internal(const void *) { return internal_owner; }
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    check(config && config->dma_frame_num == 256 && tx == nullptr && rx && channel == nullptr,
          "production microphone channel configuration");
    if (fail_create) return ESP_FAIL;
    channel = new MockChannel;
    *rx = channel;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t value, const i2s_std_config_t *config)
{
    check(value == channel && config->slot_cfg.bits == 32 && config->slot_cfg.mode == I2S_SLOT_MODE_MONO,
          "production mono32 capture format");
    return fail_std ? ESP_FAIL : ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t value)
{
    check(value == channel && !value->enabled, "enable channel lifecycle");
    ++enable_calls;
    if (fail_enable) return ESP_FAIL;
    value->enabled = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t value)
{
    check(value == channel && value->enabled, "disable channel lifecycle");
    ++disable_calls;
    if (fail_disable) return ESP_FAIL;
    value->enabled = false;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t value)
{
    check(value == channel && !value->enabled, "cannot delete running DMA channel");
    ++delete_calls;
    if (fail_delete) return ESP_FAIL;
    delete channel;
    channel = nullptr;
    return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t value, void *destination, size_t capacity,
                           size_t *bytes, uint32_t)
{
    check(value == channel && value->enabled, "read requires enabled channel");
    ++read_calls;
    now_us.fetch_add(static_cast<int64_t>(read_advance_us));
    if (during_read) during_read();
    *bytes = next_read_bytes;
    std::fill_n(static_cast<int32_t *>(destination), std::min(capacity, next_read_bytes) / sizeof(int32_t), 123);
    return read_result;
}
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t value,
    const i2s_event_callbacks_t *callbacks, void *user_data)
{
    check(value == channel && !value->enabled && callbacks->on_recv && callbacks->on_recv_q_ovf && user_data,
          "callbacks registered before channel enable with internal context");
    ++register_calls;
    if (fail_register) return ESP_FAIL;
    value->callbacks = *callbacks;
    value->user_data = user_data;
    return ESP_OK;
}

int main()
{
    defaultAndDegrade();
    deferredCaptureLifecycle();
    rangesAndOverflow();
    firstOverflowDiagnostics();
    serviceReadProgress();
    firstOverflowServiceProgress();
    overlappingServicePublication();
    lifecycleFailures();
    inputValidation();
    coherentSnapshots();
    coherentServiceSnapshots();
    sampleCursorWrap();
    std::cout << "microphone timing: 12 scenario groups passed (consumer-controlled enable, retry/illegal lifecycle, pre-enable UART logging, read service/core/callback progress, bounded first-overflow service snapshots, coherent startup/first-read/first-overflow diagnostics, concurrent ISR and task snapshots, 64-bit wrap, read ranges, overflow, degradation, retryable shutdown)\n";
}
