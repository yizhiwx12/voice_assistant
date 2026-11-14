#include "max98357a_audio.hpp"
#include "esp_heap_caps.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/task.h"
#include "i2s_write_observer.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <algorithm>
#include <array>

struct MockChannel {
    bool enabled = false; size_t queued_bytes = 0, dma_used = 0;
    i2s_isr_callback_t on_sent = nullptr; void *user_data = nullptr;
    std::array<int16_t, 480> dma{};
};
namespace {
struct Pin { bool reserved = false, output = false, pulldown = false; uint32_t level = 1; } pin;
const char *failure = nullptr;
int allocations = 0, channels = 0, configured = 0, conflicts = 0, revokes = 0;
int gpio_calls = 0;
bool no_sd = false;
MockChannel *latest_channel = nullptr;
int64_t now_us = 0;
size_t next_written = static_cast<size_t>(-1);
esp_err_t next_write_result = ESP_OK;
std::array<int16_t, 480> last_submitted{};
void complete(MockChannel *tx)
{
    assert(tx && tx->enabled);
    if (tx->on_sent) {
        i2s_event_data_t event{tx->dma.data(), tx->dma.size() * sizeof(int16_t)};
        now_us += 10000;
        tx->on_sent(tx, &event, tx->user_data);
    }
    tx->dma.fill(0);  // 模拟 IDF 的 auto_clear_after_cb，在任务复用前清零。
    tx->dma_used = 0;
}
bool fail(const char *operation)
{
    if (!failure || std::strcmp(operation, failure)) return false;
    failure = nullptr;
    return true;
}
void muted() { assert(no_sd || (pin.output && pin.level == 0)); }
void clean() { assert(allocations == 0 && channels == 0 && !pin.reserved); }
}  // 命名空间

void *heap_caps_malloc(size_t bytes, int)
{
    if (fail("alloc")) return nullptr;
    if (bytes == sizeof(PlaybackReference::Block) * 8 && fail("observer_alloc")) return nullptr;
    void *result = std::malloc(bytes);
    if (result) ++allocations;
    return result;
}
int64_t esp_timer_get_time() { return now_us; }
void heap_caps_free(void *buffer) { assert(buffer && allocations > 0); --allocations; std::free(buffer); }
bool esp_gpio_is_reserved(uint64_t mask) { assert(mask == (1ULL << 8)); return pin.reserved; }
uint64_t esp_gpio_revoke(uint64_t mask)
{
    ++gpio_calls;
    assert(mask == (1ULL << 8) && pin.reserved && !pin.output && pin.pulldown && !pin.level);
    pin.reserved = false;
    ++revokes;
    return mask;
}
esp_err_t gpio_config(const gpio_config_t *config)
{
    ++gpio_calls;
    assert(config->pin_bit_mask == (1ULL << 8));
    assert(config->mode == GPIO_MODE_OUTPUT && pin.level == 0);
    if (pin.reserved) ++conflicts;
    pin.output = pin.reserved = true;
    pin.pulldown = false;
    ++configured;
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    ++gpio_calls;
    assert(gpio == 8);
    if (fail(level ? "sd_high" : "sd_low")) return ESP_FAIL;
    pin.level = level;
    return ESP_OK;
}
esp_err_t gpio_set_pull_mode(gpio_num_t gpio, int mode)
{
    ++gpio_calls;
    assert(gpio == 8 && mode == GPIO_PULLDOWN_ONLY && pin.level == 0);
    if (fail("pull")) return ESP_FAIL;
    pin.pulldown = true;
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t gpio, int mode)
{
    ++gpio_calls;
    assert(gpio == 8 && mode == GPIO_MODE_DISABLE && pin.pulldown && pin.level == 0);
    if (fail("direction")) return ESP_FAIL;
    pin.output = false;
    return ESP_OK;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, void *)
{
    muted();
    assert(config->dma_frame_num == 240 && config->dma_desc_num == 4 && config->auto_clear_after_cb);
    if (fail("new")) return ESP_FAIL;
    assert(*tx == nullptr);
    *tx = new MockChannel;
    latest_channel = *tx;
    ++channels;
    return ESP_OK;
}
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t tx, const i2s_event_callbacks_t *callbacks, void *user_data)
{
    assert(tx && !tx->enabled);
    if (fail("register")) return ESP_FAIL;
    tx->on_sent = callbacks->on_sent;
    tx->user_data = user_data;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t tx, const i2s_std_config_t *)
{
    muted();
    assert(tx && !tx->enabled && tx->queued_bytes == 0);
    return fail("init") ? ESP_FAIL : ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t tx)
{
    muted();
    assert(tx && !tx->enabled);
    if (fail("enable")) return ESP_FAIL;
    tx->enabled = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t tx)
{
    muted();
    assert(tx && tx->enabled);
    if (fail("disable")) return ESP_FAIL;
    tx->enabled = false;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t tx)
{
    muted();
    assert(tx && !tx->enabled);
    if (fail("delete")) return ESP_FAIL;
    delete tx;
    --channels;
    return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t bytes, size_t *written, uint32_t)
{
    assert(tx && tx->enabled);
    assert(no_sd || (pin.output && pin.level == 1));
    const size_t accepted = std::min(bytes, next_written);
    last_submitted.fill(0);
    std::memcpy(last_submitted.data(), data, std::min(bytes, sizeof(last_submitted)));
    const auto *source = static_cast<const unsigned char *>(data);
    size_t offset = 0;
    while (offset < accepted) {
        const size_t amount = std::min(sizeof(tx->dma) - tx->dma_used, accepted - offset);
        voice_i2s_write_observer(tx, tx->dma.data(), tx->dma_used, offset, amount, 0);
        std::memcpy(reinterpret_cast<unsigned char *>(tx->dma.data()) + tx->dma_used, source + offset, amount);
        voice_i2s_write_observer(tx, tx->dma.data(), tx->dma_used, offset, amount, 1);
        tx->dma_used += amount; offset += amount;
        if (tx->dma_used == sizeof(tx->dma)) complete(tx);
    }
    tx->queued_bytes += accepted;
    *written = accepted;
    next_written = static_cast<size_t>(-1);
    const auto result = next_write_result;
    next_write_result = ESP_OK;
    return result;
}

int main()
{
    const int16_t sample[1] = {1234};
    Max98357aAudio audio;
    assert(audio.deinit() == ESP_OK && gpio_calls == 0);
    assert(audio.resetPlayback() == ESP_ERR_INVALID_STATE);
    assert(audio.init() == ESP_OK && audio.isReady());
    assert(audio.writeMono(sample, 1) == ESP_OK);
    assert(audio.deinit() == ESP_OK);
    clean();
    Max98357aAudio next_owner;
    assert(next_owner.init() == ESP_OK);  // 开机提示音向服务的所有权交接。
    const int initial_configured = configured;
    const int initial_revokes = revokes;
    for (int i = 0; i < 100; ++i) {
        assert(next_owner.writeMono(sample, 1) == ESP_OK);
        assert(next_owner.resetPlayback() == ESP_OK && next_owner.isReady());
        muted();
        assert(next_owner.writeMono(sample, 1) == ESP_ERR_INVALID_STATE);
        assert(configured == initial_configured && revokes == initial_revokes && channels == 1);
        assert(next_owner.setEnabled(true) == ESP_OK);
    }
    assert(next_owner.deinit() == ESP_OK);
    clean();

    for (const char *stage : {"alloc", "new", "init", "enable", "sd_high"}) {
        failure = stage;
        assert(audio.init() != ESP_OK && !audio.isReady() && failure == nullptr);
        clean();
        assert(audio.init() == ESP_OK);
        assert(audio.deinit() == ESP_OK);
        clean();
    }
    assert(audio.init() == ESP_OK);
    const int before_failures = configured;
    for (const char *stage : {"alloc", "new", "init", "enable"}) {
        failure = stage;
        assert(audio.resetPlayback() != ESP_OK && !audio.isReady() && failure == nullptr);
        muted();
        assert(pin.reserved && channels == 0 && allocations == 0);
        assert(audio.resetPlayback() == ESP_OK);
        assert(configured == before_failures);
        assert(audio.setEnabled(true) == ESP_OK);
    }
    for (const char *stage : {"disable", "delete"}) {
        failure = stage;
        assert(audio.resetPlayback() == ESP_FAIL && !audio.isReady() && failure == nullptr);
        muted();
        assert(pin.reserved && channels == 1 && allocations == 1);
        assert(audio.resetPlayback() == ESP_OK);
        assert(configured == before_failures);
        assert(audio.setEnabled(true) == ESP_OK);
    }
    failure = "sd_low";
    assert(audio.resetPlayback() == ESP_FAIL && !audio.isReady() && failure == nullptr);
    assert(pin.reserved && pin.level == 1 && channels == 1 && allocations == 1);
    assert(audio.resetPlayback() == ESP_OK && audio.setEnabled(true) == ESP_OK);
    for (const char *stage : {"pull", "direction"}) {
        failure = stage;
        assert(audio.deinit() == ESP_FAIL && failure == nullptr);
        muted();
        assert(pin.reserved && channels == 0 && allocations == 0);
        assert(audio.deinit() == ESP_OK);
        clean();
        assert(audio.init() == ESP_OK);
    }
    assert(audio.deinit() == ESP_OK);
    clean();

    pin.reserved = true;
    pin.level = 1;
    const int before_reserved = gpio_calls;
    assert(audio.init() == ESP_ERR_INVALID_STATE && gpio_calls == before_reserved && pin.level == 1);
    assert(audio.deinit() == ESP_OK && gpio_calls == before_reserved && pin.reserved);
    pin.reserved = false;

    no_sd = true;
    Max98357aAudio::Config config;
    config.sd_mode_gpio = GPIO_NUM_NC;
    assert(audio.init(config) == ESP_OK);
    assert(audio.writeMono(sample, 1) == ESP_OK);
    assert(audio.resetPlayback() == ESP_OK);
    assert(audio.writeMono(sample, 1) == ESP_ERR_INVALID_STATE);
    assert(audio.setEnabled(true) == ESP_OK && audio.writeMono(sample, 1) == ESP_OK);
    assert(audio.setEnabled(false) == ESP_OK && audio.deinit() == ESP_OK);
    assert(gpio_calls == before_reserved && conflicts == 0);
    clean();

    // 启用观测器的集成测试使用真实驱动和参考实现。
    no_sd = false;
    config.sd_mode_gpio = GPIO_NUM_8;
    config.sample_rate_hz = 24000;
    config.playback_reference_enabled = true;
    config.volume_percent = 40;
    const unsigned before_settle_delay = mock_delay_ticks;
    assert(audio.init(config) == ESP_OK);
    assert(mock_delay_ticks - before_settle_delay >= 21);
    assert(audio.playbackReferenceMetrics().enabled && latest_channel->on_sent);
    // 主机延时桩没有异步 ISR。补完零值边沿块，
    // 对应生产代码额外等待一个完整块稳定时间后允许送出的那一块。
    complete(latest_channel);
    audio.pollPlaybackReference();
    assert(audio.bindPlaybackSerial(812));
    assert(audio.bindPlaybackSerial(813));
    assert(audio.bindPlaybackSerial(812));  // 空作用域也可以取消。
    std::array<int16_t, 240> waveform{};
    for (size_t i = 0; i < waveform.size(); ++i) waveform[i] = (i & 1) ? -12345 : 12345;
    assert(audio.writeMono(waveform.data(), waveform.size()) == ESP_OK);
    assert(audio.playbackProgress().available && audio.playbackProgress().completed_samples == 240);
    assert(!audio.bindPlaybackSerial(813));
    audio.pollPlaybackReference();
    auto metrics = audio.playbackReferenceMetrics();
    assert(metrics.submitted_frames == 240 && metrics.completed_frames == 480);
    assert(metrics.non_silence_blocks == 1 && metrics.amplifier_boundary_blocks == 1 &&
           metrics.stereo_mismatch_samples == 0);
    std::array<PlaybackReference::Block, 8> reference_history{};
    assert(audio.copyPlaybackReferenceHistory(reference_history.data(), reference_history.size(),
                                              metrics.dma_generation) == 2);
    assert(reference_history[0].amplifier_boundary &&
           reference_history[0].interval_kind == PlaybackReference::IntervalKind::DigitalSilence);
    assert(reference_history[1].playback_serial == 812);
    assert(reference_history[1].interval_kind == PlaybackReference::IntervalKind::Content);
    for (size_t i = 0; i < waveform.size(); ++i) {
        assert(last_submitted[2*i] == waveform[i] * 40 / 100);
        assert(last_submitted[2*i+1] == last_submitted[2*i]);
        assert(latest_channel->dma[2*i] == 0);
        assert(reference_history[1].mono[i] == last_submitted[2*i]);
    }
    // 尾部与排空静音共用下一个物理 DMA 块。
    assert(audio.writeMono(sample, 1) == ESP_OK);
    unsigned progress_calls = 0;
    assert(audio.drain(1000, [](void *context) { ++*static_cast<unsigned *>(context); },
                       &progress_calls) == ESP_OK);
    assert(progress_calls == 5);
    assert(audio.playbackProgress().available && audio.playbackProgress().completed_samples == 241);
    audio.pollPlaybackReference();
    metrics = audio.playbackReferenceMetrics();
    assert(metrics.submitted_frames == 1441 && metrics.completed_frames == 1680);
    assert(metrics.known_silence_blocks >= 5 && metrics.non_silence_blocks == 2);
    assert(audio.copyPlaybackReferenceHistory(reference_history.data(), reference_history.size(),
                                              metrics.dma_generation) == 7);
    assert(reference_history[2].mono[0] == 493 && reference_history[2].mono[1] == 0);
    assert(audio.bindPlaybackSerial(813));
    assert(audio.playbackReferenceMetrics().dma_generation == metrics.dma_generation);
    assert(audio.bindPlaybackSerial(814));  // 在下次写入前取消。
    const auto generation = metrics.dma_generation;
    assert(audio.resetPlayback() == ESP_OK);
    metrics = audio.playbackReferenceMetrics();
    assert(metrics.dma_generation > generation && metrics.submitted_frames == 0);
    assert(audio.copyPlaybackReferenceHistory(reference_history.data(), reference_history.size(),
                                              metrics.dma_generation) == 0);
    complete(latest_channel);
    audio.pollPlaybackReference();
    assert(audio.playbackReferenceMetrics().amplifier_muted_blocks == 1);
    assert(audio.setEnabled(true) == ESP_OK);
    next_written = 8; next_write_result = ESP_ERR_TIMEOUT;
    assert(audio.writeMono(waveform.data(), waveform.size()) == ESP_ERR_TIMEOUT);
    metrics = audio.playbackReferenceMetrics();
    assert(metrics.submitted_frames == 2 && metrics.partial_write_bytes == 8);
    progress_calls = 0;
    next_written = 8; next_write_result = ESP_ERR_TIMEOUT;
    assert(audio.drain(1000, [](void *context) { ++*static_cast<unsigned *>(context); },
                       &progress_calls) == ESP_ERR_TIMEOUT);
    assert(progress_calls == 0 && !audio.bindPlaybackSerial(815));
    assert(audio.deinit() == ESP_OK);
    clean();

    // 复现初始化后静音且未绑定的 TX 在首次参考轮询前已运行的情况。
    // 空闲丢失必须保持可见，但不能阻止第一个回答。
    assert(audio.init(config) == ESP_OK);
    assert(audio.resetPlayback() == ESP_OK);
    muted();
    const auto idle_generation = audio.playbackReferenceMetrics().dma_generation;
    for (int i = 0; i < 12; ++i) complete(latest_channel);
    audio.pollPlaybackReference();
    metrics = audio.playbackReferenceMetrics();
    assert(metrics.playback_serial == 0 && metrics.submitted_frames == 0);
    assert(metrics.queue_dropped_blocks == 4 && metrics.discontinuity);
    assert(!metrics.binding_fault);
    complete(latest_channel);
    audio.pollPlaybackReference();
    std::array<PlaybackReference::Block, 16> idle_history{};
    assert(audio.copyPlaybackReferenceHistory(idle_history.data(), idle_history.size(),
                                              idle_generation) == 9);
    assert(idle_history[8].gap_before && idle_history[8].first_sample_24k == 12 * 240);
    assert(audio.bindPlaybackSerial(900));
    assert(audio.setEnabled(true) == ESP_OK);
    complete(latest_channel);  // 内容写入前先完成零值 SD 边沿。
    assert(audio.writeMono(waveform.data(), waveform.size()) == ESP_OK);
    assert(audio.drain() == ESP_OK);
    assert(audio.bindPlaybackSerial(901));
    metrics = audio.playbackReferenceMetrics();
    assert(metrics.dma_generation == idle_generation && metrics.discontinuity);
    assert(!metrics.binding_fault);
    assert(metrics.queue_dropped_blocks == 4);
    // 一旦提交 PCM 并绑定回答，即使静音空闲时丢失，
    // 成功排空后也必须继续拒绝后续绑定。
    assert(audio.setEnabled(false) == ESP_OK);
    for (int i = 0; i < 9; ++i) complete(latest_channel);
    audio.pollPlaybackReference();
    assert(!audio.bindPlaybackSerial(902));
    assert(audio.playbackReferenceMetrics().binding_fault);
    assert(audio.deinit() == ESP_OK);
    clean();

    failure = "observer_alloc";  // 观测器分配失败，音频仍可使用。
    assert(audio.init(config) == ESP_OK && !audio.playbackReferenceMetrics().enabled);
    assert(audio.writeMono(sample, 1) == ESP_OK && audio.deinit() == ESP_OK);
    clean();
    failure = "register";
    assert(audio.init(config) == ESP_OK && !audio.playbackReferenceMetrics().enabled);
    assert(audio.writeMono(sample, 1) == ESP_OK && audio.deinit() == ESP_OK);
    clean();
    assert(audio.init(config) == ESP_OK && audio.playbackReferenceMetrics().enabled);
    for (const char *stage : {"disable", "delete"}) {
        failure = stage;
        assert(audio.resetPlayback() == ESP_FAIL && !audio.isReady());
        // 未成功停止/删除的通道保留观测器存储。
        // 若禁用失败，正在进行的完成回调仍然安全。
        assert(audio.playbackReferenceMetrics().enabled && allocations == 3);
        if (latest_channel->enabled) complete(latest_channel);
        assert(audio.resetPlayback() == ESP_OK);
        assert(audio.setEnabled(true) == ESP_OK);
    }
    assert(audio.setVolume(0) == ESP_OK);
    assert(audio.writeMono(waveform.data(), waveform.size()) == ESP_OK);
    complete(latest_channel);  // 已知欠载零样本，不是重播内容。
    audio.pollPlaybackReference();
    assert(audio.playbackReferenceMetrics().known_silence_blocks == 2);
    assert(audio.playbackReferenceMetrics().amplifier_boundary_blocks == 1);
    assert(audio.deinit() == ESP_OK);
    clean();
    std::puts("Passed MAX98357A lifecycle: 100 muted DMA resets, ownership handoff, 14 injected failures, no-SD; no hardware used");
    std::puts("Passed observer integration: callback, zero edge settle, scaled stereo, progress/drain/rebind, initial-muted overflow recovery, bound-muted overflow rejection, partial timeout/failed drain, reset/mute, allocation/registration fallback, failed-stop storage retention, zero volume/underrun");
}
