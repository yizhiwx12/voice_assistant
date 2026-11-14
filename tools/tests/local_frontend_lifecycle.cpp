// 真实 LocalAudioFrontend 配合有界模拟 AFE 队列及真实主机线程。
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "local_audio_frontend.hpp"
#include "app/memory_diagnostics.hpp"

using namespace std::chrono_literals;
#if CONFIG_VOICE_AEC_OBSERVATION_ENABLED
constexpr bool kAec = true;
#else
constexpr bool kAec = false;
#endif
#if CONFIG_VOICE_BARGE_IN_OBSERVATION_ENABLED
constexpr bool kFullDuplex = kAec;
#else
constexpr bool kFullDuplex = false;
#endif
struct esp_afe_sr_data_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::array<int16_t, 512>> output;
    std::array<int16_t, 512> current{};
    afe_fetch_result_t result{};
    uint32_t feeds = 0;
    uint32_t fetches = 0;
    size_t ring_capacity_bytes = 0;
    size_t history_bytes = 0;
    size_t history_limit_bytes = 0;
    unsigned channels = 1;
    unsigned chunk_samples = 512;
    voiceaudio::PcmDcBlocker expected_microphone;
    bool encoded_microphone_available = true;
};

namespace {
struct Queue {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned capacity;
    unsigned bytes;
    std::deque<std::vector<uint8_t>> data;
};
struct Semaphore {
    std::mutex mutex;
    std::condition_variable changed;
    bool signalled = false;
};
std::vector<std::thread> tasks;
struct TaskRequest { std::string name; unsigned priority; int core; };
std::vector<TaskRequest> task_requests;
std::atomic<unsigned> queues{0}, semaphores{0}, afe_calls{0}, heap_blocks{0};
std::atomic<bool> reject_send{false}, reject_take{false};
std::atomic<int> task_fail_at{0}, task_creations{0};
std::atomic<bool> feed_error{false}, corrupt_fetch{false}, suppress_output{false};
std::atomic<bool> block_feed{false}, feed_waiting{false};
std::atomic<bool> hold_fetch{false};
std::atomic<unsigned> held_fetch_timeouts{0};
std::atomic<int> timeout_kind{0};
std::atomic<bool> force_silence{false};
std::atomic<bool> scripted_vad{false};
std::atomic<int> output_delta{0};
std::atomic<float> vad_input_energy{-72.25f};
float vad_energy_threshold = -60.0f;
float vad_probability_threshold = 0.5f;
std::atomic<unsigned> ring_full_rejections{0};
int default_ring_frames = 50;
int observed_ring_frames = 0;
esp_err_t model_error = ESP_OK;
bool invalid_format = false;
int config_corruption = 0;
unsigned checked_configs = 0;
unsigned config_creations = 0;
bool runtime_channel_mismatch = false;
unsigned chunk_samples = 512;
std::atomic<int> feed_result_delta{0};
std::atomic<unsigned> validated_reference_samples{0};
std::atomic<unsigned> zero_reference_samples{0};
std::atomic<unsigned> stack_scan_calls{0};
thread_local std::string current_task_name;
std::atomic<unsigned> feed_delay_calls{0}, fetch_delay_calls{0};
std::atomic<unsigned> feed_attempts{0}, fetch_attempts{0};
std::atomic<unsigned> feed_work_us{0}, fetch_work_us{0};
std::atomic<bool> fast_empty_fetch{false};
struct DelayObservation { std::string task; unsigned feed_frames; unsigned fetch_calls; };
std::mutex delay_mutex;
std::vector<DelayObservation> delay_observations;
std::atomic<unsigned> destroyed{0}, unloaded{0};
std::mutex log_mutex;
std::string logs;
size_t internal_free = 256 * 1024, internal_largest = 64 * 1024;
unsigned budget_checks = 0;
std::vector<std::string> memory_stages;

template <typename Predicate>
void waitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}
void joinTasks() {
    for (auto &task : tasks) if (task.joinable()) task.join();
    tasks.clear();
}
void reset() {
    joinTasks();
    assert(queues == 0 && semaphores == 0 && afe_calls == 0 && heap_blocks == 0);
    reject_send = reject_take = false;
    task_fail_at = task_creations = 0;
    task_requests.clear();
    feed_error = corrupt_fetch = suppress_output = false;
    block_feed = feed_waiting = false;
    hold_fetch = false;
    held_fetch_timeouts = 0;
    timeout_kind = 0;
    force_silence = false;
    scripted_vad = false;
    output_delta = 0;
    vad_input_energy = -72.25f;
    vad_energy_threshold = -60.0f;
    vad_probability_threshold = 0.5f;
    ring_full_rejections = 0;
    default_ring_frames = 50;
    observed_ring_frames = 0;
    destroyed = unloaded = 0;
    model_error = ESP_OK;
    invalid_format = false;
    config_corruption = 0;
    checked_configs = 0;
    config_creations = 0;
    runtime_channel_mismatch = false;
    chunk_samples = 512;
    feed_result_delta = 0;
    validated_reference_samples = 0;
    zero_reference_samples = 0;
    stack_scan_calls = 0;
    feed_delay_calls = fetch_delay_calls = 0;
    feed_attempts = fetch_attempts = 0;
    feed_work_us = fetch_work_us = 0;
    fast_empty_fetch = false;
    delay_observations.clear();
    internal_free = 256 * 1024;
    internal_largest = 64 * 1024;
    budget_checks = 0;
    memory_stages.clear();
    logs.clear();
}
esp_afe_sr_data_t *createAfe(afe_config_t *config) {
    assert(config->vad_init && config->vad_model_name);
    assert(checked_configs == config_creations);
    assert(config->aec_init == kAec && !config->se_init && !config->ns_init);
    assert(config->pcm_config.total_ch_num == (kAec ? 2 : 1));
    assert(config->afe_type == (kFullDuplex ? AFE_TYPE_FD : AFE_TYPE_SR));
    assert(config->afe_mode == AFE_MODE_LOW_COST);
    assert(config->aec_mode == (kFullDuplex ? AEC_MODE_FD_LOW_COST : AEC_MODE_SR_LOW_COST));
    assert(config->aec_nlp_level == (kFullDuplex ? AEC_NLP_LEVEL_VERYAGGR : AEC_NLP_LEVEL_AGGR));
    assert(!config->vad_mute_playback && !config->output_playback_channel);
    assert(config->fixed_output_channel == kFullDuplex);
    assert(!config->agc_init && !config->wakenet_init);
    assert(config->afe_ringbuf_size > 0 && config->afe_linear_gain == 1.0f);
    assert(config->afe_perferred_core == 0 && config->afe_perferred_priority == 9);
    auto *afe = new esp_afe_sr_data_t;
    afe->channels = config->pcm_config.total_ch_num;
    afe->chunk_samples = chunk_samples;
    observed_ring_frames = config->afe_ringbuf_size;
    // ESP-SR 2.6.1 单麦克风 AFE 使用 512 样本帧，并在同一环中保留
    // VAD 的 min_speech + delay 历史。容量不只是未读输出上限；
    // 忽略这部分预留曾使测试漏掉真实失败。
    afe->ring_capacity_bytes = static_cast<size_t>(config->afe_ringbuf_size) * chunk_samples * sizeof(int16_t);
    afe->history_limit_bytes = static_cast<size_t>(config->vad_min_speech_ms + config->vad_delay_ms) *
                               16000 * sizeof(int16_t) / 1000;
    return afe;
}
int feedAfe(esp_afe_sr_data_t *afe, const int16_t *pcm) {
    ++afe_calls;
    if (const unsigned work = feed_work_us.load()) std::this_thread::sleep_for(std::chrono::microseconds(work));
    if (feed_error.load()) { --afe_calls; return -1; }
    std::unique_lock<std::mutex> lock(afe->mutex);
    const size_t frame_bytes = afe->chunk_samples * sizeof(int16_t);
    const size_t pending_bytes = afe->output.size() * frame_bytes;
    assert(pending_bytes <= afe->ring_capacity_bytes);
    if (afe->ring_capacity_bytes - pending_bytes < afe->history_bytes + frame_bytes) {
        ++ring_full_rejections;
        --afe_calls;
        return 0;
    }
    ++afe->feeds;
    if (!suppress_output.load()) {
        std::array<int16_t, 512> output;
        for (size_t i = 0; i < afe->chunk_samples; ++i) {
            const int16_t microphone = pcm[i * afe->channels];
            if (afe->channels == 2) {
                const int16_t reference = pcm[i * 2 + 1];
                if (reference == 0) {
                    ++zero_reference_samples;
                    // 部分场景有意使用静音参考。
                    // 该参考不能编码原始麦克风信号。
                    afe->encoded_microphone_available = false;
                }
                if (!kFullDuplex) {
                    const int16_t expected = static_cast<int16_t>(static_cast<uint16_t>(microphone) ^ 0x5a5a);
                    assert(reference == expected || reference == 0);
                } else if (afe->encoded_microphone_available) {
                    const int16_t original = static_cast<int16_t>(static_cast<uint16_t>(reference) ^ 0x5a5a);
                    assert(microphone == afe->expected_microphone.process(original));
                }
                ++validated_reference_samples;
            }
            output[i] = static_cast<int16_t>(microphone + output_delta.load());
        }
        afe->output.push_back(output);
        afe->changed.notify_all();
    }
    if (block_feed.load()) {
        // 表示只有仍在运行的 fetch 工作任务才能解除的 feed 背压。
        feed_waiting = true;
        afe->changed.wait(lock, [&] { return afe->output.empty(); });
    }
    --afe_calls;
    // 2.6.1 单麦克风实现返回处理后的单声道环字节数，
    // 即使输入是双通道 MR 格式也一样。
    return static_cast<int>(frame_bytes) + feed_result_delta.load();
}
afe_fetch_result_t *fetchAfe(esp_afe_sr_data_t *afe, TickType_t ticks) {
    ++afe_calls;
    ++fetch_attempts;
    if (const unsigned work = fetch_work_us.load()) std::this_thread::sleep_for(std::chrono::microseconds(work));
    std::unique_lock<std::mutex> lock(afe->mutex);
    if (fast_empty_fetch.load()) {
        --afe_calls;
        if (timeout_kind == 0) return nullptr;
        afe->result.ret_value = timeout_kind == 1 ? ESP_FAIL : ESP_ERR_TIMEOUT;
        return &afe->result;
    }
    if (!afe->changed.wait_for(lock, std::chrono::milliseconds(ticks), [&] {
            return !afe->output.empty() && !hold_fetch.load();
        })) {
        if (hold_fetch.load() && !afe->output.empty()) ++held_fetch_timeouts;
        --afe_calls;
        if (timeout_kind == 0) return nullptr;
        afe->result.ret_value = timeout_kind == 1 ? ESP_FAIL : ESP_ERR_TIMEOUT;
        return &afe->result;
    }
    afe->current = afe->output.front();
    afe->output.pop_front();
    afe->result.data = afe->current.data();
    afe->result.data_size = corrupt_fetch ? 3 : afe->chunk_samples * sizeof(int16_t);
    afe->result.ret_value = ESP_OK;
    afe->result.data_volume = vad_input_energy.load();
    const auto vad = force_silence.load() ? VAD_SILENCE :
        ((scripted_vad.load() ? afe->fetches : afe->feeds) % 2 ? VAD_SPEECH : VAD_SILENCE);
    ++afe->fetches;
    if (vad == VAD_SILENCE && afe->result.vad_state == VAD_SPEECH) afe->history_bytes = 0;
    afe->result.vad_state = vad;
    afe->history_bytes = std::min(afe->history_limit_bytes,
                                afe->history_bytes + afe->chunk_samples * sizeof(int16_t));
    // 有意设为非空；重新插入此缓存会使计数虚增。
    afe->result.vad_cache = afe->current.data();
    afe->result.vad_cache_size = 1024;
    afe->changed.notify_all();
    --afe_calls;
    return &afe->result;
}
void destroyAfe(esp_afe_sr_data_t *afe) {
    assert(afe_calls == 0);  // feed/fetch 活动期间不得拆除模型/AFE。
    ++destroyed;
    delete afe;
}
const esp_afe_sr_iface_t interface{
    createAfe,
    [](esp_afe_sr_data_t *afe) { return invalid_format ? 4096 : static_cast<int>(afe->chunk_samples); },
    [](esp_afe_sr_data_t *afe) { return static_cast<int>(afe->chunk_samples); },
    [](esp_afe_sr_data_t *) { return 16000; },
    [](esp_afe_sr_data_t *afe) { return runtime_channel_mismatch ? 3 : static_cast<int>(afe->channels); },
    [](esp_afe_sr_data_t *) { return 1; },
    feedAfe, fetchAfe, destroyAfe,
    [](esp_afe_sr_data_t *) {},
    [](esp_afe_sr_data_t *) { return vad_energy_threshold; },
    [](esp_afe_sr_data_t *) { return vad_probability_threshold; },
};
}  // 命名空间

namespace voice::memory {
Snapshot capture() {
    Snapshot snapshot;
    snapshot.internal.free_bytes = internal_free;
    snapshot.internal.largest_free_block = internal_largest;
    return snapshot;
}
void log(const char *stage) { memory_stages.emplace_back(stage); }
bool hasInternalBudget(const Snapshot &snapshot, size_t required_free, size_t required_largest) {
    // 验证生产顺序：AFE 及缓冲已存在，工作任务尚未创建。
    assert(queues == 1 && semaphores == 2 && heap_blocks == (kFullDuplex ? 2U : 1U) && tasks.empty());
    assert(required_free == 48 * 1024 && required_largest == 12 * 1024);
    ++budget_checks;
    return snapshot.internal.free_bytes >= required_free &&
           snapshot.internal.largest_free_block >= required_largest;
}
}  // 命名空间 voice::memory

const char *esp_err_to_name(esp_err_t) { return "host_error"; }
void stubLog(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(log_mutex);
    logs += buffer;
    logs += '\n';
}
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
QueueHandle_t xQueueCreateWithCaps(unsigned capacity, unsigned bytes, int) {
    auto *queue = new Queue;
    queue->capacity = capacity;
    queue->bytes = bytes;
    ++queues;
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t) {
    if (reject_send.exchange(false)) return 0;
    auto &queue = *static_cast<Queue *>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.data.size() == queue.capacity) return 0;
    const auto *bytes = static_cast<const uint8_t *>(item);
    queue.data.emplace_back(bytes, bytes + queue.bytes);
    queue.changed.notify_one();
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t ticks) {
    auto &queue = *static_cast<Queue *>(handle);
    std::unique_lock<std::mutex> lock(queue.mutex);
    if (!queue.changed.wait_for(lock, std::chrono::milliseconds(ticks), [&] { return !queue.data.empty(); })) return 0;
    std::memcpy(item, queue.data.front().data(), queue.bytes);
    queue.data.pop_front();
    ++feed_attempts;
    return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t handle) {
    auto &queue = *static_cast<Queue *>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    return static_cast<unsigned>(queue.data.size());
}
void vQueueDeleteWithCaps(QueueHandle_t handle) {
    --queues;
    delete static_cast<Queue *>(handle);
}
SemaphoreHandle_t xSemaphoreCreateBinary() {
    ++semaphores;
    return new Semaphore;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
    auto &semaphore = *static_cast<Semaphore *>(handle);
    std::lock_guard<std::mutex> lock(semaphore.mutex);
    semaphore.signalled = true;
    semaphore.changed.notify_one();
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
    if (reject_take.exchange(false)) return 0;
    auto &semaphore = *static_cast<Semaphore *>(handle);
    std::unique_lock<std::mutex> lock(semaphore.mutex);
    if (!semaphore.changed.wait_for(lock, std::chrono::milliseconds(ticks), [&] { return semaphore.signalled; })) return 0;
    semaphore.signalled = false;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t handle) {
    --semaphores;
    delete static_cast<Semaphore *>(handle);
}
BaseType_t createFrontendTask(void (*entry)(void *), const char *name, unsigned stack, void *argument,
                             unsigned priority, TaskHandle_t *handle, int core, int caps) {
    assert(stack == 8192 && priority == (kAec ? 6U : 5U) &&
           caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(core == (kAec ? 1 : -1));
    task_requests.push_back({name, priority, core});
    const int number = ++task_creations;
    if (task_fail_at == number) return 0;
    *handle = reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(number));
    tasks.emplace_back([entry, argument, task_name = std::string(name)] {
        current_task_name = task_name;
        entry(argument);
    });
    return pdPASS;
}
BaseType_t xTaskCreateWithCaps(void (*entry)(void *), const char *name, unsigned stack, void *argument,
                             unsigned priority, TaskHandle_t *handle, int caps) {
    return createFrontendTask(entry, name, stack, argument, priority, handle, -1, caps);
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*entry)(void *), const char *name, unsigned stack,
                                         void *argument, unsigned priority, TaskHandle_t *handle,
                                         BaseType_t core, int caps) {
    return createFrontendTask(entry, name, stack, argument, priority, handle, core, caps);
}
void vTaskDeleteWithCaps(TaskHandle_t) {}
void vTaskDelay(TickType_t ticks) {
    assert(ticks == 1);  // 真实阻塞一个节拍，而不是让出执行或零延迟。
    assert(current_task_name == "afe_feed" || current_task_name == "afe_fetch");
    if (current_task_name == "afe_feed") ++feed_delay_calls;
    else ++fetch_delay_calls;
    {
        std::lock_guard<std::mutex> lock(delay_mutex);
        delay_observations.push_back({current_task_name, feed_attempts.load(), fetch_attempts.load()});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) {
    ++stack_scan_calls;
    return 6144;
}
void *heap_caps_malloc(size_t bytes, int) {
    void *pointer = std::malloc(bytes);
    if (pointer) ++heap_blocks;
    return pointer;
}
void heap_caps_free(void *pointer) {
    if (pointer) --heap_blocks;
    std::free(pointer);
}
afe_config_t *afe_config_init(const char *format, srmodel_list_t *, int type, int mode) {
    assert(std::strcmp(format, kAec ? "MR" : "M") == 0);
    assert(type == (kFullDuplex ? AFE_TYPE_FD : AFE_TYPE_SR));
    assert(mode == AFE_MODE_LOW_COST);
    ++config_creations;
    auto *config = new afe_config_t;
    static uint8_t mic_id = 0, ref_id = 1;
    config->pcm_config = {kAec ? 2 : 1, 1, &mic_id, kAec ? 1 : 0, &ref_id, 16000};
    config->afe_type = type;
    config->afe_mode = mode;
    config->aec_mode = kFullDuplex ? AEC_MODE_FD_LOW_COST : AEC_MODE_SR_LOW_COST;
    config->fixed_output_channel = kFullDuplex;
    // 与实际固定版本 ESP-SR 库的 afe_config_init 默认值一致。
    // 专项负向对照将其改回 8，以复现旧问题。
    config->afe_ringbuf_size = default_ring_frames;
    return config;
}
afe_config_t *afe_config_check(afe_config_t *config) {
    ++checked_configs;
    // 通过核对过的合成值验证生产代码记录/保留 SDK 配置，
    // 而不是臆造内部任务的核分配。
    config->afe_perferred_core = 0;
    config->afe_perferred_priority = 9;
    if (config_corruption == 1) config->aec_init = !kAec;
    if (config_corruption == 2) config->pcm_config.total_ch_num = 3;
    if (config_corruption == 3) config->ns_init = true;
    if (config_corruption == 4) return nullptr;
    if (config_corruption == 5) config->afe_type = kFullDuplex ? AFE_TYPE_SR : AFE_TYPE_FD;
    if (config_corruption == 6)
        config->aec_mode = kFullDuplex ? AEC_MODE_SR_LOW_COST : AEC_MODE_FD_LOW_COST;
    if (config_corruption == 7) config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR + 1;
    if (config_corruption == 8) config->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;
    if (config_corruption == 9) config->vad_mute_playback = true;
    if (config_corruption == 10) config->output_playback_channel = true;
    if (config_corruption == 11) config->fixed_output_channel = false;
    return config;
}
void afe_config_free(afe_config_t *config) { delete config; }
const esp_afe_sr_iface_t *esp_afe_handle_from_config(afe_config_t *) { return &interface; }

esp_err_t submit(LocalAudioFrontend &frontend, const int16_t *pcm, size_t count) {
    if (!kAec) return frontend.enqueue(pcm, count);
    std::array<int16_t, 320> reference{};
    assert(count <= reference.size());
    for (size_t i = 0; i < count; ++i)
        reference[i] = static_cast<int16_t>(static_cast<uint16_t>(pcm[i]) ^ 0x5a5a);
    return frontend.enqueuePaired(pcm, reference.data(), count);
}
esp_err_t SrModelStore::load() {
    if (model_error != ESP_OK) return model_error;
    static srmodel_list_t models;
    models_ = &models;
    vad_model_name_ = "vadnet1_medium";
    return ESP_OK;
}
void SrModelStore::unload() {
    if (models_) {
        // 模型若已用于创建 AFE，先销毁 AFE。
        if (task_creations || invalid_format || budget_checks) assert(destroyed > 0);
        ++unloaded;
    }
    models_ = nullptr;
    vad_model_name_ = nullptr;
}

int main() {
    unsigned scenarios = 0;
    {
        // 独立信号性质补充下面模拟实现共用辅助函数的顺序检查。
        // 任何恒定启动偏置都不得产生启动尖峰，包括 PCM16 两端极值。
        voiceaudio::PcmDcBlocker blocker;
        std::array<int16_t, 320> constant{};
        for (const int value : {0, 300, -300, INT16_MIN, INT16_MAX}) {
            blocker.reset();
            for (unsigned frame = 0; frame < 3; ++frame) {
                constant.fill(static_cast<int16_t>(value));
                blocker.process(constant.data(), constant.size());
                for (const auto sample : constant) assert(sample == 0);
            }
        }
        blocker.reset();
        assert(blocker.process(1234) == 0);
        assert(blocker.process(1234) == 0);
        assert(blocker.process(-1234) < 0);
        ++scenarios;
    }
    {
        // 增加恒定直流偏置不能改变任何输出样本。输入幅度远离限幅，
        // 两路首样本都只建立各自基线。
        voiceaudio::PcmDcBlocker unbiased, biased;
        constexpr double pi = 3.14159265358979323846;
        for (unsigned i = 0; i < 16000; ++i) {
            const int16_t input = static_cast<int16_t>(
                1000.0 * std::sin(2.0 * pi * 80.0 * i / 16000.0) +
                500.0 * std::sin(2.0 * pi * 1000.0 * i / 16000.0));
            assert(unbiased.process(input) == biased.process(static_cast<int16_t>(input + 300)));
        }
        ++scenarios;
    }
    {
        // 队列/AFE 分块边界不能重置连续状态。
        std::vector<int16_t> continuous(8192);
        for (size_t i = 0; i < continuous.size(); ++i)
            continuous[i] = static_cast<int16_t>(static_cast<int>(i * 97 % 4001) - 2000);
        auto chunked = continuous;
        voiceaudio::PcmDcBlocker first, second;
        first.process(continuous.data(), continuous.size());
        const size_t chunks[] = {1, 319, 320, 511, 512, 37};
        for (size_t offset = 0, round = 0; offset < chunked.size(); ++round) {
            const size_t count = std::min(chunks[round % 6], chunked.size() - offset);
            second.process(chunked.data() + offset, count);
            offset += count;
        }
        assert(continuous == chunked);
        ++scenarios;
    }
    {
        // 低于语音频段的直流去除也必须保留 80 Hz 浊音能量。
        // 独立于递推公式，测量一秒稳定 PCM。
        constexpr double pi = 3.14159265358979323846;
        for (const unsigned frequency : {80U, 100U, 1000U}) {
            voiceaudio::PcmDcBlocker blocker;
            double input_energy = 0.0, output_energy = 0.0;
            for (unsigned i = 0; i < 32000; ++i) {
                const int16_t input = static_cast<int16_t>(10000.0 *
                    std::sin(2.0 * pi * frequency * i / 16000.0));
                const int16_t output = blocker.process(input);
                if (i >= 16000) {
                    input_energy += static_cast<double>(input) * input;
                    output_energy += static_cast<double>(output) * output;
                }
            }
            const double ratio = std::sqrt(output_energy / input_energy);
            assert(ratio > 0.97 && ratio < 1.01);
        }
        ++scenarios;
    }
    {
        // 真实电平变化产生极性正确的瞬态，随后衰减至精确静音，
        // 而不是进入整数极限环。
        for (const int16_t step : {int16_t{1000}, int16_t{-1000}}) {
            voiceaudio::PcmDcBlocker blocker;
            assert(blocker.process(0) == 0);
            assert(blocker.process(step) == step);
            for (unsigned i = 0; i < 16000; ++i) {
                const int16_t output = blocker.process(step);
                assert(step > 0 ? output >= 0 && output <= step : output <= 0 && output >= step);
                if (i >= 12000) assert(output == 0);
            }
        }
        ++scenarios;
    }
    {
        // 相反 PCM 极值应安全使用 int32 差值并按正确符号饱和；
        // 持续交替极值仍保持有界。
        voiceaudio::PcmDcBlocker blocker;
        assert(blocker.process(INT16_MIN) == 0);
        assert(blocker.process(INT16_MAX) == INT16_MAX);
        blocker.reset();
        assert(blocker.process(INT16_MAX) == 0);
        assert(blocker.process(INT16_MIN) == INT16_MIN);
        for (unsigned i = 0; i < 50000; ++i) {
            const int16_t input = i % 2 ? INT16_MIN : INT16_MAX;
            const int16_t output = blocker.process(input);
            assert(i % 2 ? output < 0 : output > 0);
        }
        ++scenarios;
    }
    std::printf("PcmDcBlocker independent signal properties: 6 scenarios passed "
                "(constant/reset, DC invariance, chunk continuity, 80/100/1000 Hz, "
                "signed step decay, PCM16 saturation)\n");
    {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        assert(task_requests.size() == 2);
        assert(task_requests[0].name == "afe_fetch" && task_requests[1].name == "afe_feed");
        for (const auto &request : task_requests)
            assert(request.priority == (kAec ? 6U : 5U) &&
                   request.core == (kAec ? 1 : -1));
        assert(frontend.metrics().feed_core == (kAec ? 1 : -1));
        assert(frontend.metrics().fetch_core == (kAec ? 1 : -1));
        assert(logs.find(kAec ? "policy=aec_cooperative" : "policy=mono_established") != std::string::npos);
        assert(logs.find("sdk_config_core=0 sdk_config_priority=9") != std::string::npos);
        assert(frontend.aecRequested() == kAec && frontend.aecActive() == kAec);
        assert(logs.find(kFullDuplex ? "profile=barge_in full_duplex=1 aec_profile=FD_LOW_COST nlp_requested=VERYAGGR" :
                                      "profile=speech_recognition full_duplex=0") != std::string::npos);
        assert(logs.find(kFullDuplex ? "mic_dc_blocker=on" : "mic_dc_blocker=off") != std::string::npos);
        assert(frontend.metrics().aec_active == kAec);
        assert(observed_ring_frames == 50);
        assert(budget_checks == 1);
        assert((memory_stages == std::vector<std::string>{
            "frontend_before_models", "frontend_models_ready", "frontend_afe_ready",
            "frontend_workers_ready"}));
        // 文档列出的三种定时空等待表示都必须视为普通结果。
        for (int kind = 0; kind < 3; ++kind) {
            timeout_kind = kind;
            std::this_thread::sleep_for(70ms);
            assert(frontend.metrics().ready && frontend.metrics().last_error == ESP_OK);
        }
        // 真实 50 ms 空等待不能被标成 50 ms CPU/工作时间。
        // 但它仍算一次尝试：四次空结果请求真正阻塞一个节拍，
        // 不让任何关于阻塞的推断放松追赶尝试上限。
        if (kAec) waitUntil([&] { return fetch_delay_calls > 0; });
        const auto empty_metrics = frontend.metrics();
        assert(empty_metrics.fetch_work_wall_max_us == 0);
        if (kAec) assert(empty_metrics.fetch_budget_waits > 0);
        else assert(empty_metrics.fetch_budget_waits == 0 && fetch_delay_calls == 0);
        assert(empty_metrics.fetch_batch_max == (kAec ? 4U : 0U));
        std::array<int16_t, 320> pcm{};
        for (unsigned i = 0; i < 16; ++i) {
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            std::this_thread::sleep_for(1ms);
        }
        waitUntil([&] { return frontend.metrics().output_samples == 5120; });
        const auto metrics = frontend.metrics();
        assert(metrics.feed_samples == 5120 && metrics.output_frames == 16);
        assert(metrics.feed_chunk_samples == 512 && metrics.fetch_chunk_samples == 512);
        assert(metrics.output_rms == 0 && metrics.output_peak == 0);
        if (kAec) assert(validated_reference_samples >= 512);
        else assert(frontend.enqueuePaired(pcm.data(), pcm.data(), pcm.size()) == ESP_ERR_INVALID_STATE);
        assert(metrics.vad_transitions > 0 && metrics.feed_stack_free > 0 && metrics.fetch_stack_free > 0);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        assert(frontend.stop() == ESP_OK);  // 幂等清理。
        if (!kAec) assert(feed_delay_calls == 0 && fetch_delay_calls == 0);
        ++scenarios;
    }
    {
        // 即使 VAD 为静音且输出 PCM 为零，也检查 SDK VAD 输入窗口。
        // 输出 RMS 或配置默认值都不能替代此证据。非有限值不能变成
        // 0 dBFS，也不能让先前当前观测继续标为有效。
        reset();
        chunk_samples = 320;
        force_silence = true;
        vad_energy_threshold = -67.25f;
        vad_probability_threshold = 0.625f;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        const auto initial = frontend.metrics();
        assert(initial.vad_thresholds_valid && initial.vad_energy_threshold_cdbfs == -6725);
        assert(initial.vad_probability_threshold_milli == 625);
        assert(!initial.vad_input_energy_valid && initial.vad_input_energy_cdbfs == INT32_MIN);
        assert(initial.vad_input_energy_peak_cdbfs == INT32_MIN && initial.vad_input_energy_frames == 0);
        assert(initial.vad_input_energy_invalid_frames == 0);
        assert(logs.find("energy_threshold_dbfs=-67.25") != std::string::npos);
        assert(logs.find("probability_threshold=0.625") != std::string::npos);
        std::array<int16_t, 320> pcm{};
        uint32_t blocks = 0;
        const auto observe = [&](float energy) {
            vad_input_energy = energy;
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            ++blocks;
            waitUntil([&] { return frontend.metrics().output_samples == blocks * pcm.size(); });
            return frontend.metrics();
        };
        const auto quiet = observe(-72.25f);
        assert(!quiet.speech && quiet.output_rms == 0 && quiet.vad_input_energy_valid);
        assert(quiet.vad_input_energy_cdbfs == -7225 && quiet.vad_input_energy_peak_cdbfs == -7225);
        const auto louder = observe(-48.125f);
        assert(louder.vad_input_energy_cdbfs == -4813 && louder.vad_input_energy_peak_cdbfs == -4813);
        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                    std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity(), 1.0f, -201.0f}) {
            const auto metrics = observe(invalid);
            assert(metrics.ready && !metrics.vad_input_energy_valid);
            assert(metrics.vad_input_energy_cdbfs == INT32_MIN && metrics.vad_input_energy_peak_cdbfs == -4813);
            assert(metrics.vad_input_energy_frames == 2);
        }
        const auto recovered = observe(-80.0f);
        assert(recovered.vad_input_energy_valid && recovered.vad_input_energy_cdbfs == -8000);
        assert(recovered.vad_input_energy_peak_cdbfs == -4813);
        assert(recovered.vad_input_energy_frames == 3 && recovered.vad_input_energy_invalid_frames == 5);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        // 新启动不能暴露前会话的能量/峰值，
        // 也不能把取值接口文档中的不可用返回值当作有效值。
        vad_energy_threshold = 1.0f;
        vad_probability_threshold = -1.0f;
        assert(frontend.start() == ESP_OK);
        const auto restarted = frontend.metrics();
        assert(!restarted.vad_thresholds_valid && restarted.vad_energy_threshold_cdbfs == INT32_MIN);
        assert(restarted.vad_probability_threshold_milli == -1);
        assert(!restarted.vad_input_energy_valid && restarted.vad_input_energy_peak_cdbfs == INT32_MIN);
        assert(restarted.vad_input_energy_frames == 0 && restarted.vad_input_energy_invalid_frames == 0);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        ++scenarios;
    }
    {
        reset();
        force_silence = true;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK && observed_ring_frames == 50);
        std::array<int16_t, 320> pcm{};
        // 640 个采集块对应 400 个 AFE 帧，即 12.8 秒连续静音。
        // 每批都排空，使其成为历史/容量测试，
        // 不受主机线程调度及生产者突发速度影响。
        for (unsigned batch = 0; batch < 80; ++batch) {
            for (unsigned i = 0; i < 8; ++i)
                assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            const uint32_t expected_samples = (batch + 1) * 8 * 320;
            waitUntil([&] { return frontend.metrics().output_samples == expected_samples; });
            const auto metrics = frontend.metrics();
            assert(metrics.ready && metrics.last_error == ESP_OK && !metrics.speech);
        }
        waitUntil([&] { return frontend.metrics().feed_samples == 204800; });
        const auto metrics = frontend.metrics();
        assert(metrics.output_samples == 204800 && metrics.output_blocks == 400);
        assert(metrics.output_frames == 640 && metrics.vad_transitions == 1);
        assert(ring_full_rejections == 0);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        assert(stack_scan_calls == 21);  // 首块取样，此后每 50 块：13 次 feed + 8 次 fetch。
        ++scenarios;
    }
    {
        reset();
        force_silence = true;
        default_ring_frames = 8;  // 负向对照：先前生产设置。
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK && observed_ring_frames == 8);
        std::array<int16_t, 320> pcm{};
        // 环中其他数据已空时，历史自身仍填满 8 帧容量。
        // 精确复现 4096 个输入/输出样本后，第九个 AFE 帧被拒绝；
        // 提高 fetch 优先级或重试都无法修复。
        for (unsigned captured = 1; captured <= 13; ++captured) {
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            const uint32_t expected_samples = captured * 320 / 512 * 512;
            waitUntil([&] { return frontend.metrics().output_samples == expected_samples; });
        }
        waitUntil([&] { return frontend.metrics().feed_samples == 4096; });
        assert(frontend.metrics().ready && frontend.metrics().output_samples == 4096);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return !frontend.metrics().ready; });
        const auto metrics = frontend.metrics();
        assert(metrics.feed_samples == 4096 && metrics.output_samples == 4096);
        assert(metrics.last_error == ESP_FAIL && ring_full_rejections == 1);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(logs.find("reason=afe_feed") != std::string::npos);
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        model_error = ESP_ERR_NOT_FOUND;
        assert(frontend.start() == ESP_ERR_NOT_FOUND);
        assert(frontend.metrics().enabled && !frontend.metrics().ready);
        assert(frontend.metrics().last_error == ESP_ERR_NOT_FOUND);
        assert(task_creations == 0 && destroyed == 0);
        assert(frontend.stop() == ESP_OK);
        ++scenarios;
    }
    {
        reset();
        chunk_samples = 256;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        assert(frontend.metrics().feed_chunk_samples == 256);
        assert(frontend.metrics().fetch_chunk_samples == 256);
        std::array<int16_t, 320> pcm{};
        for (unsigned i = 0; i < 4; ++i) assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return frontend.metrics().feed_samples == 1280; });
        waitUntil([&] { return frontend.metrics().output_samples == 1280; });
        assert(frontend.metrics().ready && frontend.metrics().last_error == ESP_OK);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    for (int corruption = 1; corruption <= (kFullDuplex ? 11 : kAec ? 7 : 5); ++corruption) {
        reset();
        config_corruption = corruption;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_ERR_INVALID_STATE);
        assert(!frontend.metrics().ready && !frontend.metrics().aec_active);
        assert(destroyed == 0 && unloaded == 1 && task_creations == 0);
        ++scenarios;
    }
    {
        reset();
        runtime_channel_mismatch = true;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_ERR_INVALID_SIZE);
        assert(destroyed == 1 && unloaded == 1 && task_creations == 0);
        ++scenarios;
    }
    {
        reset();
        feed_result_delta = 2;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> pcm{};
        for (unsigned i = 0; i < 4; ++i) assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return !frontend.metrics().ready; });
        assert(frontend.metrics().last_error == ESP_FAIL);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    if (kAec) {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> microphone{};
        std::array<int16_t, 320> known_silence_reference{};
        assert(frontend.enqueuePaired(microphone.data(), known_silence_reference.data(),
                                      microphone.size()) == ESP_OK);
        for (unsigned i = 0; i < 4; ++i)
            assert(frontend.enqueuePaired(microphone.data(), known_silence_reference.data(),
                                          microphone.size()) == ESP_OK);
        waitUntil([&] { return frontend.metrics().feed_samples >= 512; });
        assert(frontend.metrics().ready && frontend.metrics().last_error == ESP_OK);
        assert(zero_reference_samples >= 512);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    if (kAec) {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> pcm{};
        assert(frontend.enqueue(pcm.data(), pcm.size()) == ESP_ERR_INVALID_STATE);
        assert(!frontend.metrics().ready && frontend.metrics().last_error == ESP_ERR_INVALID_STATE);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1);
        ++scenarios;
    }
    {
        reset();
        invalid_format = true;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_ERR_INVALID_SIZE);
        assert(frontend.metrics().last_error == ESP_ERR_INVALID_SIZE);
        assert(task_creations == 0 && destroyed == 1 && unloaded == 1);
        ++scenarios;
    }
    for (const int failed_task : {1, 2}) {
        reset();
        task_fail_at = failed_task;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_ERR_NO_MEM);
        joinTasks();
        assert(!frontend.metrics().ready && frontend.metrics().last_error == ESP_ERR_NO_MEM);
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    for (const bool insufficient_total : {true, false}) {
        reset();
        if (insufficient_total) internal_free = 48 * 1024 - 1;
        else internal_largest = 12 * 1024 - 1;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_ERR_NO_MEM);
        const auto metrics = frontend.metrics();
        assert(metrics.enabled && !metrics.ready && !metrics.speech);
        assert(metrics.last_error == ESP_ERR_NO_MEM && budget_checks == 1);
        assert(task_creations == 0 && tasks.empty());
        assert(destroyed == 1 && unloaded == 1);
        assert(queues == 0 && semaphores == 0 && heap_blocks == 0);
        assert(logs.find("reason=internal_budget") != std::string::npos);
        assert(logs.find("automatic mode must stop; raw manual path remains available") != std::string::npos);
        assert((memory_stages == std::vector<std::string>{
            "frontend_before_models", "frontend_models_ready", "frontend_afe_ready",
            "frontend_internal_budget_failed", "frontend_budget_released"}));
        assert(frontend.stop() == ESP_OK);
        assert(destroyed == 1 && unloaded == 1);  // 不重复拆除。
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> pcm{};
        reject_send = true;
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_ERR_TIMEOUT);
        assert(!frontend.metrics().ready && !frontend.metrics().speech);
        assert(frontend.metrics().last_error == ESP_ERR_TIMEOUT);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1);
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        std::atomic<unsigned> outputs{0};
        assert(frontend.setOutputCallback([&](const int16_t *, size_t, uint64_t, bool) {
            ++outputs;
        }) == ESP_OK);
        block_feed = hold_fetch = true;
        assert(frontend.start() == ESP_OK);
        assert(!frontend.failureSnapshot().valid);
        std::array<int16_t, 320> pcm{};
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return feed_waiting.load(); });
        // 生产 feed 工作任务已进入真实阻塞的 AFE 调用。
        // 填满实际有界 RTOS 输入队列，不注入 reject_send。
        for (unsigned i = 0; i < 12; ++i)
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(frontend.metrics().queue_frames == 12 && frontend.metrics().queue_peak == 12);
        assert(!reject_send.load());
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_ERR_TIMEOUT);
        assert(!frontend.metrics().ready && !frontend.metrics().speech);
        assert(frontend.metrics().last_error == ESP_ERR_TIMEOUT);
        const auto failure = frontend.failureSnapshot();
        assert(failure.valid && failure.error == ESP_ERR_TIMEOUT && failure.at_us > 0);
        assert(failure.queue_frames == 12 && failure.queue_peak == 12);
        assert(failure.feed_samples == 0 && failure.output_samples == 0);
        const auto assert_failure_unchanged = [&] {
            const auto current = frontend.failureSnapshot();
            assert(current.valid == failure.valid && current.error == failure.error);
            assert(current.at_us == failure.at_us && current.queue_frames == failure.queue_frames);
            assert(current.queue_peak == failure.queue_peak && current.feed_samples == failure.feed_samples);
            assert(current.output_samples == failure.output_samples && current.feed_max_us == failure.feed_max_us);
            assert(current.fetch_max_us == failure.fetch_max_us);
        };
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_ERR_INVALID_STATE);
        frontend.invalidate(ESP_FAIL);  // 后续失败不能替换首因。
        assert(frontend.metrics().last_error == ESP_ERR_TIMEOUT);
        assert_failure_unchanged();
        const unsigned previous_timeouts = held_fetch_timeouts.load();
        // 失败后仍有多次定时 fetch，证明工作任务继续服务正在进行的 feed，
        // 而不是在 running_=false 时立即退出。
        waitUntil([&] { return held_fetch_timeouts.load() >= previous_timeouts + 2; });
        assert(destroyed == 0 && unloaded == 0 && afe_calls >= 1);
        hold_fetch = false;
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(frontend.metrics().last_error == ESP_ERR_TIMEOUT);
        assert(frontend.metrics().feed_samples == 512 && outputs == 0);
        assert_failure_unchanged();  // 正在进行的操作完成及停止只改变实时计数器。
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0 && afe_calls == 0);
        const std::string marker = "AFE/VAD unavailable: reason=frontend_backlog";
        const size_t first_failure = logs.find(marker);
        assert(first_failure != std::string::npos && logs.find(marker, first_failure + 1) == std::string::npos);
        assert(logs.find("reason=microphone_unavailable") == std::string::npos);
        ++scenarios;
    }
    if (kAec) {
        // 输入诊断针对最新接受的 20 ms 配对，
        // 而非独立取得的输出。直流不能计作交流。
        reset();
        LocalAudioFrontend frontend;
        std::array<int16_t, 320> microphone{}, reference{};
        for (unsigned round = 0; round < 2; ++round) {
            assert(frontend.start() == ESP_OK);
            const auto initial = frontend.metrics();
            assert(initial.microphone_mean == 0 && initial.microphone_ac_rms == 0 &&
                   initial.reference_mean == 0 && initial.reference_ac_rms == 0);
            microphone.fill(INT16_MIN);
            reference.fill(static_cast<int16_t>(static_cast<uint16_t>(INT16_MIN) ^ 0x5a5a));
            assert(frontend.tryEnqueuePaired(microphone.data(), reference.data(), microphone.size()) == ESP_OK);
            const auto constant = frontend.metrics();
            assert(constant.microphone_mean == INT16_MIN && constant.microphone_rms == 32768 &&
                   constant.microphone_ac_rms == 0 && constant.microphone_peak == 32768);
            assert(constant.reference_mean == reference[0] && constant.reference_ac_rms == 0);
            assert(constant.reference_rms == static_cast<uint32_t>(-reference[0]));
            for (size_t i = 0; i < microphone.size(); ++i) microphone[i] = i % 2 ? 310 : 290;
            reference.fill(0);
            reject_send = true;
            assert(frontend.tryEnqueuePaired(microphone.data(), reference.data(), microphone.size()) ==
                   ESP_ERR_NOT_FINISHED);
            const auto rejected = frontend.metrics();
            assert(rejected.ready && rejected.microphone_mean == constant.microphone_mean &&
                   rejected.microphone_ac_rms == constant.microphone_ac_rms &&
                   rejected.microphone_rms == constant.microphone_rms &&
                   rejected.reference_mean == constant.reference_mean &&
                   rejected.reference_ac_rms == constant.reference_ac_rms &&
                   rejected.reference_rms == constant.reference_rms);
            assert(frontend.tryEnqueuePaired(microphone.data(), reference.data(), microphone.size()) == ESP_OK);
            const auto biased = frontend.metrics();
            assert(biased.microphone_mean == 300 && biased.microphone_ac_rms == 10 &&
                   biased.microphone_rms == 300 && biased.microphone_peak == 310);
            assert(biased.reference_mean == 0 && biased.reference_ac_rms == 0 && biased.reference_rms == 0);
            waitUntil([&] { return frontend.metrics().output_samples == 512; });
            assert(frontend.stop() == ESP_OK);
            joinTasks();
        }
        ++scenarios;
    }
    if (kAec) {
        reset();
        LocalAudioFrontend frontend;
        std::array<int16_t, 320> microphone{}, reference{};
        assert(frontend.tryEnqueuePaired(microphone.data(), reference.data(), microphone.size()) ==
               ESP_ERR_INVALID_STATE);
        std::mutex output_mutex;
        std::vector<int16_t> output;
        assert(frontend.setOutputCallback([&](const int16_t *pcm, size_t count, uint64_t offset, bool) {
            std::lock_guard<std::mutex> lock(output_mutex);
            assert(offset == output.size());
            output.insert(output.end(), pcm, pcm + count);
        }) == ESP_OK);
        block_feed = hold_fetch = true;
        assert(frontend.start() == ESP_OK);
        const auto prepare = [&](unsigned frame) {
            for (size_t sample = 0; sample < microphone.size(); ++sample) {
                microphone[sample] = static_cast<int16_t>(frame * 320 + sample);
                reference[sample] = static_cast<int16_t>(
                    static_cast<uint16_t>(microphone[sample]) ^ 0x5a5a);
            }
        };
        const auto try_submit = [&] {
            return frontend.tryEnqueuePaired(microphone.data(), reference.data(), microphone.size());
        };
        prepare(0);
        assert(try_submit() == ESP_OK);
        prepare(1);
        assert(try_submit() == ESP_OK);
        waitUntil([&] { return feed_waiting.load(); });
        // 一次真实在途 feed 被 fetch 阻塞。接下来的十二个不同帧
        // 填满实际有界 RTOS 队列，不注入错误。
        for (unsigned frame = 2; frame < 14; ++frame) {
            prepare(frame);
            assert(try_submit() == ESP_OK);
        }
        const auto full = frontend.metrics();
        assert(full.queue_frames == 12 && full.queue_peak == 12);
        assert(full.feed_samples == 0 && full.output_samples == 0);
        // 即使源统计已经存在，已接受的队列输入及当前阻塞的 SDK 调用
        // 也不是已提交证据。
        assert(!frontend.sourceWindow(0, 320).valid);
        assert(full.microphone_rms > 0 && full.microphone_peak == 4479);
        assert(full.reference_rms > 0 && full.reference_peak > full.microphone_peak);
        prepare(14);
        for (unsigned retry = 0; retry < 20; ++retry) {
            assert(try_submit() == ESP_ERR_NOT_FINISHED);
            const auto busy = frontend.metrics();
            assert(busy.ready && busy.aec_active && busy.last_error == ESP_OK);
            assert(busy.queue_frames == 12 && busy.queue_peak == 12);
            assert(busy.microphone_rms == full.microphone_rms &&
                   busy.microphone_peak == full.microphone_peak &&
                   busy.microphone_mean == full.microphone_mean &&
                   busy.microphone_ac_rms == full.microphone_ac_rms);
            assert(busy.reference_rms == full.reference_rms &&
                   busy.reference_peak == full.reference_peak &&
                   busy.reference_mean == full.reference_mean &&
                   busy.reference_ac_rms == full.reference_ac_rms);
            assert(busy.feed_samples == full.feed_samples && busy.output_samples == full.output_samples);
            assert(!frontend.failureSnapshot().valid);
        }
        assert(!reject_send.load());
        hold_fetch = false;
        // 提交任何后续帧前，先重试保留帧。
        // 所有输出样本与偏移必须连续，且仅出现一次。
        for (unsigned frame = 14; frame < 16; ++frame) {
            prepare(frame);
            waitUntil([&] {
                const auto result = try_submit();
                assert(result == ESP_OK || result == ESP_ERR_NOT_FINISHED);
                return result == ESP_OK;
            });
            microphone.fill(-1);
            reference.fill(-1);  // 入队成功后，队列持有所复制的 PCM。
        }
        waitUntil([&] {
            const auto metrics = frontend.metrics();
            return metrics.output_frames == 16 && metrics.feed_samples == 5120;
        });
        assert(frontend.metrics().ready && frontend.metrics().last_error == ESP_OK);
        assert(frontend.metrics().feed_samples == 5120 && frontend.metrics().output_samples == 5120);
        assert(!frontend.failureSnapshot().valid);
        if (kFullDuplex) {
            waitUntil([&] { return frontend.sourceWindow(0, 5120).valid; });
            const auto source = frontend.sourceWindow(0, 5120);
            assert(source.valid && source.frames == 16 && source.samples == 5120);
            assert(source.covered_first == 0 && source.covered_end == 5120);
            // 十六个不同斜坡帧都恰好处理一次；
            // 二十次生产者满队列重试不得增加统计。
            assert(source.raw.mean == 2559 && source.raw.rms == 2955 && source.raw.ac_rms == 1478 &&
                   source.raw.peak == 5119 && source.raw.max_frame_ac_rms == 92);
            assert(source.dc.mean > 0 && source.dc.mean < source.raw.mean);
            assert(source.reference.peak > source.raw.peak);
            assert(!frontend.sourceWindow(5120, 5440).valid);
        } else {
            assert(!frontend.sourceWindow(0, 5120).valid);
        }
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(!frontend.sourceWindow(0, 320).valid);
        assert(output.size() == 5120 && validated_reference_samples == 5120);
        voiceaudio::PcmDcBlocker expected_microphone;
        for (size_t sample = 0; sample < output.size(); ++sample) {
            const int16_t original = static_cast<int16_t>(sample);
            assert(output[sample] == (kFullDuplex ? expected_microphone.process(original) : original));
        }
        assert(logs.find("reason=frontend_backlog") == std::string::npos);
        assert(frontend.tryEnqueuePaired(microphone.data(), reference.data(), microphone.size()) ==
               ESP_ERR_INVALID_STATE);
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0 && afe_calls == 0);
        ++scenarios;
    }
    for (const bool bad_feed : {true, false}) {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        feed_error = bad_feed;
        corrupt_fetch = !bad_feed;
        std::array<int16_t, 320> pcm{};
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return !frontend.metrics().ready; });
        assert(frontend.metrics().last_error == ESP_FAIL);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1);
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        block_feed = true;
        std::array<int16_t, 320> pcm{};
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return feed_waiting.load(); });
        reject_take = true;
        assert(frontend.stop() == ESP_ERR_TIMEOUT);
        assert(destroyed == 0 && unloaded == 0 && queues == 1 && semaphores == 2);
        // 重试只等待现有工作任务退出，不能提前创建或释放。
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    {
        reset();
        {
            LocalAudioFrontend frontend;
            assert(frontend.start() == ESP_OK);
            frontend.invalidate(ESP_OK);
            assert(!frontend.metrics().ready && frontend.metrics().last_error == ESP_FAIL);
            // 析构函数必须在模型成员卸载前停止工作任务。
        }
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && queues == 0 && semaphores == 0);
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        for (unsigned round = 0; round < 3; ++round) {
            assert(frontend.start() == ESP_OK);
            const auto started = frontend.metrics();
            assert(started.ready && started.last_error == ESP_OK);
            assert(started.feed_samples == 0 && started.output_frames == 0);
            assert(!frontend.sourceWindow(0, 320).valid);
            std::array<int16_t, 320> pcm{};
            pcm.fill(31);
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            assert(!frontend.sourceWindow(0, 320).valid);
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            waitUntil([&] { return frontend.metrics().output_samples == 512; });
            assert(frontend.metrics().output_frames == 1);
            if (kFullDuplex) {
                waitUntil([&] { return frontend.sourceWindow(0, 320).valid; });
                const auto source = frontend.sourceWindow(1, 319);
                assert(source.valid && source.samples == 320 && source.frames == 1);
                assert(source.ordinal_mapping && !source.lag_calibrated);
                assert(source.raw.mean == 31 && source.raw.rms == 31 && source.raw.ac_rms == 0);
                assert(source.dc.mean == 0 && source.dc.rms == 0 && source.dc.peak == 0);
                assert(source.reference.mean == (31 ^ 0x5a5a) &&
                       source.reference.rms == (31 ^ 0x5a5a) && source.reference.ac_rms == 0);
                const auto future = frontend.sourceWindow(320, 512);
                assert(!future.valid && future.available_end == 320);
            } else {
                assert(!frontend.sourceWindow(0, 320).valid);
            }
            assert(frontend.stop() == ESP_OK);
            joinTasks();
            assert(!frontend.sourceWindow(0, 320).valid);
            assert(destroyed == round + 1 && unloaded == round + 1);
            assert(queues == 0 && semaphores == 0 && heap_blocks == 0);
        }
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        suppress_output = true;
        std::array<int16_t, 320> pcm{};
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return !frontend.metrics().ready; });
        assert(frontend.metrics().last_error == ESP_ERR_TIMEOUT);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(destroyed == 1 && unloaded == 1 && heap_blocks == 0);
        ++scenarios;
    }
    {
        reset();
        scripted_vad = true;
        output_delta = 700;  // 区分真实 AFE 输出与原始采集。
        LocalAudioFrontend frontend;
        struct Output { std::array<int16_t, 320> pcm; uint64_t offset; bool speech; };
        std::mutex output_mutex;
        std::vector<Output> outputs;
        assert(frontend.setOutputCallback([&](const int16_t *pcm, size_t count, uint64_t offset, bool speech) {
            assert(count == 320);
            Output output{};
            std::copy_n(pcm, count, output.pcm.begin());
            output.offset = offset;
            output.speech = speech;
            std::lock_guard<std::mutex> lock(output_mutex);
            outputs.push_back(output);
        }) == ESP_OK);
        assert(frontend.start() == ESP_OK);
        assert(frontend.setOutputCallback({}) == ESP_ERR_INVALID_STATE);
        std::array<int16_t, 320> pcm{};
        for (unsigned frame = 0; frame < 16; ++frame) {
            for (size_t sample = 0; sample < pcm.size(); ++sample)
                pcm[sample] = static_cast<int16_t>(frame * 320 + sample);
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            std::this_thread::sleep_for(1ms);
        }
        waitUntil([&] { return frontend.metrics().output_frames == 16; });
        const auto output_metrics = frontend.metrics();
        assert(output_metrics.output_rms > 0 && output_metrics.output_peak >= output_metrics.output_rms);
        // 滤波后的斜坡趋近恒定斜率响应，
        // 因此其最后一帧的整数 RMS 和峰值相同是合理的。
        if (!kFullDuplex) assert(output_metrics.output_peak > output_metrics.output_rms);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(outputs.size() == 16);  // 连续数据不能与缺少偏移的缓存拼接。
        voiceaudio::PcmDcBlocker expected_microphone;
        for (size_t frame = 0; frame < outputs.size(); ++frame) {
            const auto &output = outputs[frame];
            assert(output.offset == frame * 320);
            bool expected_speech = false;
            for (size_t sample = 0; sample < output.pcm.size(); ++sample) {
                const size_t offset = frame * 320 + sample;
                const int16_t original = static_cast<int16_t>(offset);
                const int16_t expected = kFullDuplex ? expected_microphone.process(original) : original;
                assert(output.pcm[sample] == static_cast<int16_t>(expected + 700));
                expected_speech = expected_speech || (offset / 512 % 2 != 0);
            }
            assert(output.speech == expected_speech);
        }
        ++scenarios;
    }
    {
        reset();
        LocalAudioFrontend frontend;
        std::atomic<unsigned> outputs{0};
        std::atomic<uint64_t> first_offset{999};
        assert(frontend.setOutputCallback([&](const int16_t *pcm, size_t count, uint64_t offset, bool) {
            assert(count == 320);
            for (size_t i = 0; i < count; ++i) assert(pcm[i] == (kFullDuplex ? 0 : 31));
            if (outputs.fetch_add(1) == 0) first_offset = offset;
        }) == ESP_OK);
        for (unsigned round = 0; round < 2; ++round) {
            outputs = 0;
            first_offset = 999;
            assert(frontend.start() == ESP_OK);
            assert(!frontend.failureSnapshot().valid);
            std::array<int16_t, 320> pcm{};
            pcm.fill(31);
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
            waitUntil([&] { return outputs.load() == 1; });
            assert(first_offset == 0);
            // 存在未满的 feed/fetch 帧。失败必须停止本时间线，
            // 重新启动必须丢弃这些部分样本。
            frontend.invalidate(ESP_FAIL);
            assert(!frontend.metrics().ready && frontend.metrics().last_error == ESP_FAIL);
            assert(submit(frontend, pcm.data(), pcm.size()) == ESP_ERR_INVALID_STATE);
            assert(frontend.stop() == ESP_OK);
            joinTasks();
            assert(outputs == 1);
        }
        ++scenarios;
    }
    if (kAec) {
        // 实际有界队列及暂停的 AFE 消费者在两个生产工作任务中形成追赶工作。
        // 跨每次真实协作等待检查样本身份/偏移，
        // 而不是测试预算逻辑的复制品。
        reset();
        LocalAudioFrontend frontend;
        std::mutex output_mutex;
        std::vector<int16_t> output;
        assert(frontend.setOutputCallback([&](const int16_t *pcm, size_t count, uint64_t offset, bool) {
            std::lock_guard<std::mutex> lock(output_mutex);
            assert(offset == output.size());
            output.insert(output.end(), pcm, pcm + count);
        }) == ESP_OK);
        hold_fetch = true;
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> microphone{}, reference{};
        for (unsigned frame = 0; frame < 32; ++frame) {
            for (size_t sample = 0; sample < microphone.size(); ++sample) {
                microphone[sample] = static_cast<int16_t>(frame * 320 + sample);
                reference[sample] = static_cast<int16_t>(
                    static_cast<uint16_t>(microphone[sample]) ^ 0x5a5a);
            }
            waitUntil([&] {
                const esp_err_t result = frontend.tryEnqueuePaired(
                    microphone.data(), reference.data(), microphone.size());
                assert(result == ESP_OK || result == ESP_ERR_NOT_FINISHED);
                return result == ESP_OK;
            });
        }
        waitUntil([&] { return frontend.metrics().feed_samples == 10240; });
        assert(feed_delay_calls > 0 && frontend.metrics().ready);
        hold_fetch = false;
        waitUntil([&] { return frontend.metrics().output_frames == 32; });
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        const auto metrics = frontend.metrics();
        assert(metrics.ready == false && metrics.last_error == ESP_OK);
        assert(metrics.feed_samples == 10240 && metrics.output_samples == 10240);
        assert(metrics.feed_budget_waits == feed_delay_calls && metrics.fetch_budget_waits == fetch_delay_calls);
        assert(metrics.feed_budget_waits > 0 && metrics.fetch_budget_waits > 0);
        assert(metrics.feed_batch_max > 0 && metrics.feed_batch_max <= 4);
        assert(metrics.fetch_batch_max > 0 && metrics.fetch_batch_max <= 4);
        assert(output.size() == 10240 && validated_reference_samples == 10240 && zero_reference_samples == 0);
        voiceaudio::PcmDcBlocker expected_microphone;
        for (size_t sample = 0; sample < output.size(); ++sample) {
            const int16_t original = static_cast<int16_t>(sample);
            assert(output[sample] == (kFullDuplex ? expected_microphone.process(original) : original));
        }
        frontend.logSchedulingDiagnostics();
        assert(logs.find("wall_includes_success_fetch_wait=1 cpu_time=0") != std::string::npos);
        ++scenarios;
    }
    if (kAec) {
        // 不可拆分的慢 DSP 操作在不足四块时就超过实际耗时预算。
        // 延时必须在该操作完成并转发 PCM 后发生，
        // 不能发生在借用输出的处理中途。
        reset();
        feed_work_us = fetch_work_us = 9000;
        LocalAudioFrontend frontend;
        std::atomic<unsigned> outputs{0};
        assert(frontend.setOutputCallback([&](const int16_t *pcm, size_t count, uint64_t offset, bool) {
            assert(count == 320 && offset == 0);
            for (size_t sample = 0; sample < count; ++sample) assert(pcm[sample] == (kFullDuplex ? 0 : 23));
            ++outputs;
        }) == ESP_OK);
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> pcm{};
        pcm.fill(23);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return feed_delay_calls > 0 && fetch_delay_calls > 0 && outputs == 1; });
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        const auto metrics = frontend.metrics();
        assert(metrics.last_error == ESP_OK && metrics.feed_samples == 512 && metrics.output_samples == 512);
        assert(metrics.feed_work_wall_max_us >= 8000 && metrics.fetch_work_wall_max_us >= 8000);
        assert(metrics.feed_batch_max < 4 && metrics.fetch_batch_max < 4);
        assert(outputs == 1 && validated_reference_samples == 512);
        ++scenarios;
    }
    if (kAec) {
        // 持有者使前端失效时，feed 可能正在 SDK 中等待环空间。
        // 继续消费，并持续限制空 fetch 尝试，
        // 直到那次真实在途 feed 被解除阻塞。
        reset();
        block_feed = fast_empty_fetch = true;
        LocalAudioFrontend frontend;
        std::atomic<unsigned> outputs{0};
        assert(frontend.setOutputCallback([&](const int16_t *, size_t, uint64_t, bool) { ++outputs; }) == ESP_OK);
        assert(frontend.start() == ESP_OK);
        std::array<int16_t, 320> pcm{};
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        assert(submit(frontend, pcm.data(), pcm.size()) == ESP_OK);
        waitUntil([&] { return feed_waiting.load() && fetch_delay_calls >= 4; });
        frontend.invalidate(ESP_ERR_TIMEOUT, "pair_queue_full");
        frontend.invalidate(ESP_FAIL);  // 保留参考失败作为首因。
        const unsigned previous_waits = fetch_delay_calls;
        waitUntil([&] { return fetch_delay_calls >= previous_waits + 4; });
        assert(afe_calls >= 1 && destroyed == 0 && unloaded == 0);
        assert(!frontend.metrics().ready && frontend.metrics().fetch_batch_max <= 4);
        fast_empty_fetch = false;
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        assert(frontend.metrics().last_error == ESP_ERR_TIMEOUT);
        assert(frontend.metrics().feed_samples == 512 && outputs == 0);
        assert(destroyed == 1 && unloaded == 1 && afe_calls == 0);
        assert(logs.find("reason=pair_queue_full") != std::string::npos);
        assert(logs.find("reason=microphone_unavailable") == std::string::npos);
        ++scenarios;
    }
    if (kAec) for (int kind = 0; kind < 3; ++kind) {
        // fetch_with_delay 允许立即返回空结果。三种 SDK 表示
        // 都必须至少每四次尝试请求真正阻塞一个节拍，
        // 包括 stop 正在等待 feed 的期间。
        reset();
        timeout_kind = kind;
        fast_empty_fetch = true;
        LocalAudioFrontend frontend;
        assert(frontend.start() == ESP_OK);
        waitUntil([&] { return fetch_delay_calls >= 8; });
        assert(frontend.metrics().ready && frontend.metrics().feed_samples == 0);
        assert(frontend.stop() == ESP_OK);
        joinTasks();
        const auto metrics = frontend.metrics();
        assert(metrics.last_error == ESP_OK && metrics.output_samples == 0);
        assert(metrics.fetch_budget_waits == fetch_delay_calls && metrics.fetch_budget_waits >= 8);
        assert(metrics.fetch_batch_max > 0 && metrics.fetch_batch_max <= 4);
        assert(fetch_attempts <= fetch_delay_calls * 4 + 4);
        assert(feed_delay_calls == 0);
        unsigned previous = 0;
        for (const auto &observation : delay_observations) {
            assert(observation.task == "afe_fetch");
            assert(observation.fetch_calls - previous > 0 && observation.fetch_calls - previous <= 4);
            previous = observation.fetch_calls;
        }
        ++scenarios;
    }
    reset();
    std::printf("LocalAudioFrontend %s configuration/lifecycle: %u scenarios passed (no DSP)\n",
                kFullDuplex ? "FD/AEC" : kAec ? "SR/AEC" : "mono", scenarios);
}
