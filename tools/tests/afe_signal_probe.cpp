// 仅测试生成器/生命周期。下面前端明确使用替身，
// 不能从此可执行程序得出 ESP-SR DSP 或声学结论。
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../main/audio/afe_signal_probe.cpp"

namespace {
using voiceaudio::ProbeCase;
enum class Failure { None, Allocation, CallbackSetup, Start, StartDeadline, NoAec, Submit,
                     QueueDeadline, DrainDeadline, CallbackShape, Counter, Stop };
struct Fake {
    explicit Fake(ProbeCase kind) : stimulus(kind) {}
    voiceaudio::Stimulus stimulus;
    voiceaudio::PcmDcBlocker dc;
    uint32_t accepted = 0;
    int64_t start_us = 0;
    bool first_retry = true, stopped = false;
};
std::map<LocalAudioFrontend *, std::unique_ptr<Fake>> frontends;
std::map<void *, size_t> allocations;
std::vector<std::string> logs;
Failure failure = Failure::None;
int target_case = 0, starts = 0, stops = 0, destroyed = 0, callback_sets = 0;
int64_t now_us = 0;
unsigned checks = 0;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool inject(Failure value) { return failure == value && starts - 1 == target_case; }
bool contains(const std::string &needle)
{
    return std::any_of(logs.begin(), logs.end(), [&needle](const auto &line) {
        return line.find(needle) != std::string::npos;
    });
}
void reset(Failure selected = Failure::None, int selected_case = 0)
{
    expect(frontends.empty() && allocations.empty(), "previous case released all ownership");
    failure = selected; target_case = selected_case;
    starts = stops = destroyed = callback_sets = 0;
    now_us = 0;
    logs.clear();
}
void checkGenerators()
{
    voiceaudio::Stimulus zero(ProbeCase::Zero), ref(ProbeCase::ReferenceOnly),
        noise(ProbeCase::DcNoise), mixed(ProbeCase::DcNoiseReference), impulse(ProbeCase::Impulse);
    voiceaudio::Moments noise_stats, reference_stats;
    uint32_t impulses = 0, reference_changes = 0;
    int16_t previous_reference = 0;
    for (uint32_t i = 0; i < voiceaudio::kSamples; ++i) {
        int16_t z, zr, r, rr, n, nr, m, mr, p, pr;
        zero.next(z, zr); ref.next(r, rr); noise.next(n, nr);
        mixed.next(m, mr); impulse.next(p, pr);
        expect(z == 0 && zr == 0 && r == 0 && nr == 0 && pr == 0, "zero channels are exact");
        expect(n == m && rr == mr, "paired cases use identical deterministic streams");
        expect(n >= 266 && n <= 334, "noise bound includes the specified DC offset");
        if (i < 16000) expect(rr == 0, "reference first second is exact zero");
        if (i > 16000 && rr != previous_reference) ++reference_changes;
        previous_reference = rr;
        noise_stats.add(n, i); reference_stats.add(rr, i);
        if (p) { ++impulses; expect(i == voiceaudio::kImpulseSample && p == 12000, "known impulse ordinal"); }
    }
    expect(noise_stats.mean() >= 299 && noise_stats.mean() <= 300, "noise DC is approximately 300");
    expect(noise_stats.acRms() >= 19 && noise_stats.acRms() <= 20, "noise AC is approximately 20");
    expect(reference_stats.peak <= 7200 && reference_changes > 20000, "bounded changing voiced reference");
    expect(impulses == 1, "exactly one microphone impulse");
    auto context = std::make_unique<voiceaudio::Context>();
    context->observer.configure(context->observer_storage.data(), voiceaudio::kObserverCapacity,
        voiceaudio::kPrerollFrames, voiceaudio::kConfirmFrames, voiceaudio::kHoldFrames,
        voiceaudio::kMinimumAc);
    context->observer.enable(0);
    std::array<int16_t, 320> pcm;
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? 200 : -200;
    for (size_t i = 0; i < voiceaudio::kConfirmFrames; ++i)
        context->output(pcm.data(), pcm.size(), i * pcm.size(), true);
    expect(context->observer.snapshot().confirmations == 1, "probe invokes the real confirmation observer");
    expect(context->callback_error.load() == ESP_OK, "valid observer fixture has no callback fault");
}
void checkSuccess()
{
    reset();
    expect(voiceaudio::runAfeSignalProbe() == ESP_OK, "all five finite cases complete");
    expect(starts == 5 && stops == 5 && destroyed == 5 && callback_sets == 5,
           "every case starts/stops/destroys an independent frontend");
    expect(frontends.empty() && allocations.empty(), "successful diagnostic has no live context");
    for (const auto kind : voiceaudio::kCases) {
        const std::string prefix = std::string("case=") + voiceaudio::caseName(kind);
        expect(contains(prefix + " status=completed"), "each case has a completion line");
    }
    expect(contains("in=64000 out=64000") && contains("steady_samples=32000"), "finite sample/steady counts");
    expect(contains("aec_during_case=1 dc_stats=expected_same_filter"), "running AEC and expected DC clearly labeled");
    expect(contains("lag_is_group_delay=0"), "impulse ordinal is not claimed as calibrated group delay");
    expect(contains("acoustic_pass_claimed=0") && contains("normal_start_allowed=1"), "completion does not claim DSP passed");
    expect(now_us >= 19900000 && now_us <= 19910000, "all accepted frames follow the 20 ms pace");
}
void checkFailure(Failure selected, esp_err_t error)
{
    // 令第二个用例失败，验证用例之间已销毁资源，
    // 并且运行器在错误后不能悄悄继续执行用例 3–5。
    reset(selected, 1);
    expect(voiceaudio::runAfeSignalProbe() == error, "failure is propagated to startup owner");
    const int expected = selected == Failure::CallbackSetup ? 1 : 2;
    expect(starts == expected && callback_sets == 2 && stops == 2 && destroyed == 2,
           "failed case is joined/destroyed and subsequent cases do not start");
    expect(frontends.empty() && allocations.empty(), "ordinary error releases callback context after stop");
    expect(contains("end status=failed case=ref_only") && contains("normal_start_allowed=0"), "failure closes normal startup");
}
void checkRetainedStop()
{
    reset(Failure::Stop);
    expect(voiceaudio::runAfeSignalProbe() == ESP_ERR_TIMEOUT, "worker stop timeout is propagated");
    expect(starts == 1 && stops == 1 && destroyed == 0, "timed-out frontend is not destructed");
    expect(frontends.size() == 1 && allocations.size() == 1 && voiceaudio::retained_context,
           "entire frontend/observer/callback context is retained");
    expect(contains("context_retained=1") && contains("normal_start_allowed=0"), "retained timeout logs fail-closed status");
    auto *context = voiceaudio::retained_context;
    context->output(nullptr, 0, 0, false);
    expect(context->callback_error.load() == ESP_ERR_INVALID_SIZE, "late callback still has live backing storage");
    expect(voiceaudio::runAfeSignalProbe() == ESP_ERR_INVALID_STATE && starts == 1,
           "subsequent probe cannot start another AFE over retained workers");
    // 不清理：模拟尚未确认退出的工作任务。
}
}  // 命名空间

const char *esp_err_to_name(esp_err_t) { return "fake_error"; }
void stubLog(const char *format, ...)
{
    char buffer[4096];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    logs.emplace_back(buffer);
}
void *heap_caps_malloc(size_t count, int caps)
{
    expect(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "probe context requests PSRAM storage");
    if (failure == Failure::Allocation) return nullptr;
    void *block = std::malloc(count);
    expect(block != nullptr, "host test allocation succeeds");
    allocations[block] = count;
    return block;
}
void heap_caps_free(void *block)
{
    expect(allocations.erase(block) == 1, "context is freed exactly once");
    std::free(block);
}
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += static_cast<int64_t>(ticks) * 1000; }
void SrModelStore::unload() {}

LocalAudioFrontend::~LocalAudioFrontend()
{
    const auto found = frontends.find(this);
    if (found == frontends.end()) {
        expect(!ready_, "never-started fixture frontend owns no workers");
        return;
    }
    expect(found->second->stopped, "frontend destruction occurs only after successful stop");
    frontends.erase(found);
    ++destroyed;
}
esp_err_t LocalAudioFrontend::setOutputCallback(OutputCallback callback)
{
    expect(frontends.empty(), "previous case destroyed before next callback installation");
    const auto index = callback_sets++;
    expect(index < 5, "no unexpected sixth case");
    frontends.emplace(this, std::make_unique<Fake>(voiceaudio::kCases[static_cast<size_t>(index)]));
    if (failure == Failure::CallbackSetup && index == target_case) return ESP_FAIL;
    output_ = std::move(callback);
    return ESP_OK;
}
esp_err_t LocalAudioFrontend::start()
{
    ++starts;
    auto &state = *frontends.at(this);
    state.start_us = now_us;
    if (inject(Failure::Start)) return ESP_FAIL;
    if (inject(Failure::StartDeadline)) now_us += 11000000;
    ready_ = true;
    aec_active_ = !inject(Failure::NoAec);
    return ESP_OK;
}
esp_err_t LocalAudioFrontend::tryEnqueuePaired(const int16_t *mic, const int16_t *ref, size_t count)
{
    auto &state = *frontends.at(this);
    expect(ready_ && mic && ref && count == 320, "paired submission preserves the production contract");
    if (inject(Failure::Submit)) return ESP_FAIL;
    if (inject(Failure::QueueDeadline)) return ESP_ERR_NOT_FINISHED;
    if (state.first_retry) { state.first_retry = false; return ESP_ERR_NOT_FINISHED; }
    const int64_t due = state.start_us + static_cast<int64_t>(state.accepted / 320) * 20000;
    expect(now_us >= due && now_us <= due + 1000, "producer respects 20 ms pace, including initial retry");
    std::array<int16_t, 320> output;
    for (size_t i = 0; i < count; ++i) {
        int16_t expected_mic, expected_ref;
        state.stimulus.next(expected_mic, expected_ref);
        expect(mic[i] == expected_mic && ref[i] == expected_ref, "retry does not advance/duplicate generator samples");
        output[i] = state.dc.process(mic[i]);  // 明确的替身，不是 AEC DSP。
    }
    const uint32_t ordinal = state.accepted;
    state.accepted += count;
    feed_samples_ = state.accepted;
    if (inject(Failure::DrainDeadline)) return ESP_OK;
    output_samples_ = inject(Failure::Counter) ? state.accepted - 320 : state.accepted;
    output_(output.data(), inject(Failure::CallbackShape) ? count - 1 : count, ordinal, false);
    return ESP_OK;
}
esp_err_t LocalAudioFrontend::stop()
{
    ++stops;
    if (inject(Failure::Stop)) return ESP_ERR_TIMEOUT;
    frontends.at(this)->stopped = true;
    ready_ = false;
    aec_active_ = false;
    return ESP_OK;
}
LocalAudioFrontend::Metrics LocalAudioFrontend::metrics() const
{
    Metrics value;
    value.ready = ready_;
    value.aec_active = aec_active_;
    value.last_error = ESP_OK;
    value.feed_samples = feed_samples_;
    value.output_samples = output_samples_;
    return value;
}

int main()
{
    checkGenerators();
    checkSuccess();
    reset(Failure::Allocation);
    expect(voiceaudio::runAfeSignalProbe() == ESP_ERR_NO_MEM && starts == 0,
           "allocation failure starts no frontend");
    checkFailure(Failure::CallbackSetup, ESP_FAIL);
    checkFailure(Failure::Start, ESP_FAIL);
    checkFailure(Failure::StartDeadline, ESP_ERR_TIMEOUT);
    checkFailure(Failure::NoAec, ESP_ERR_INVALID_STATE);
    checkFailure(Failure::Submit, ESP_FAIL);
    checkFailure(Failure::QueueDeadline, ESP_ERR_TIMEOUT);
    checkFailure(Failure::DrainDeadline, ESP_ERR_TIMEOUT);
    checkFailure(Failure::CallbackShape, ESP_ERR_INVALID_SIZE);
    checkFailure(Failure::Counter, ESP_ERR_INVALID_SIZE);
    checkRetainedStop();
    std::printf("afe_signal_probe: %u generator/lifecycle assertions passed; DSP NOT RUN\n", checks);
}
