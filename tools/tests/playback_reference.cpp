#include "playback_reference.hpp"
#include "esp_heap_caps.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {
std::unordered_map<void *, int> allocations;
int fail_caps = 0;
int64_t now_us = 123000;
size_t internal_bytes = 0, external_bytes = 0;
std::array<int16_t, 480> stereo{};
std::array<PlaybackReference::Block, 40> history{};
void emit(PlaybackReference &reference, int16_t value = 321, bool enabled = true)
{
    stereo.fill(value);
    now_us += 10000;
    reference.onDmaSent(stereo.data(), sizeof(stereo), enabled);
}
}

void *heap_caps_malloc(size_t bytes, int caps)
{
    if (fail_caps && (caps & fail_caps)) { fail_caps = 0; return nullptr; }
    assert(bool(caps & MALLOC_CAP_INTERNAL) != bool(caps & MALLOC_CAP_SPIRAM));
    void *value = std::malloc(bytes);
    assert(value);
    allocations[value] = caps;
    if (caps & MALLOC_CAP_INTERNAL) internal_bytes = bytes;
    if (caps & MALLOC_CAP_SPIRAM) external_bytes = bytes;
    return value;
}
void heap_caps_free(void *value)
{
    assert(allocations.erase(value) == 1);
    std::free(value);
}
int64_t esp_timer_get_time() { return now_us; }

int main()
{
    PlaybackReference reference;
    assert(!reference.enabled());
    reference.pump(); reference.stop();
    fail_caps = MALLOC_CAP_INTERNAL;
    assert(reference.start() == ESP_ERR_NO_MEM && !reference.enabled());
    assert(allocations.empty());
    assert(reference.start() == ESP_OK);
    assert(reference.start() == ESP_ERR_INVALID_STATE);
    assert(internal_bytes <= 5 * 1024 && external_bytes <= 16 * 1024);
    reference.beginGeneration();
    const auto generation = reference.metrics().dma_generation;
    assert(generation != 0);
    assert(reference.bindPlaybackSerial(77));
    assert(reference.bindPlaybackSerial(77));
    assert(reference.bindPlaybackSerial(78));
    assert(reference.bindPlaybackSerial(77));  // 取消空作用域。
    // 提交仅接受完整立体声帧，完成计数仍独立。
    reference.recordSubmittedBytes(960, 960);
    reference.recordSubmittedBytes(8, 960);
    reference.recordSubmittedBytes(3, 960);
    auto metrics = reference.metrics();
    assert(metrics.submitted_frames == 242 && metrics.partial_write_bytes == 11);
    assert(metrics.completed_blocks == 0);
    // 借用 DMA 缓冲清零/复用后，数值仍保留。
    emit(reference, 1234);
    stereo.fill(-999);
    reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), generation) == 1);
    assert(history[0].mono[0] == 1234 && history[0].mono[239] == 1234);
    assert(history[0].observed_end_us == now_us && history[0].first_sample_24k == 0);
    assert(history[0].sample_count == 240 && history[0].stereo_match && !history[0].known_silence);
    assert(history[0].playback_serial == 77 && history[0].interval_kind == PlaybackReference::IntervalKind::Content);
    emit(reference, 0);
    // 静音 DMA 推进其样本时间线，但不产生 PCM 队列项。
    emit(reference, 2222, false);
    emit(reference, 2222, false);
    emit(reference, 765);
    reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), generation) == 5);
    assert(history[1].known_silence && history[2].first_sample_24k == 480);
    assert(history[2].interval_kind == PlaybackReference::IntervalKind::AmplifierMuted);
    assert(history[2].known_silence && history[2].mono[0] == 0 && !history[2].gap_before);
    assert(history[4].first_sample_24k == 960);
    metrics = reference.metrics();
    assert(metrics.completed_frames == 1200 && metrics.amplifier_muted_blocks == 2);
    assert(metrics.known_silence_blocks == 1 && metrics.non_silence_blocks == 2);
    // 无效长度及立体声不匹配保持可见，不越界复制。
    reference.onDmaSent(stereo.data(), 16, true);
    stereo.fill(500); stereo[3] = -500;
    reference.onDmaSent(stereo.data(), sizeof(stereo), true);
    reference.pump();
    const auto mismatch_count = reference.copyHistory(history.data(), history.size(), generation);
    assert(mismatch_count == 6 && history[5].gap_before && !history[5].stereo_match);
    assert(reference.metrics().stereo_mismatch_samples == 1 && reference.metrics().invalid_blocks == 1);
    // 满载时丢弃第九个事件，下一个接受事件带有缺口。
    for (int i = 0; i < 9; ++i) emit(reference, static_cast<int16_t>(i + 1));
    assert(reference.metrics().queue_dropped_blocks == 1);
    reference.pump();
    emit(reference, 999); reference.pump();
    const auto after_overflow = reference.copyHistory(history.data(), history.size(), generation);
    assert(after_overflow == 15 && history[14].gap_before && history[14].mono[0] == 999);
    PlaybackReference::Block next{};
    bool overwritten = false;
    assert(reference.copyHistoryAfter(0, &next, &overwritten));
    assert(overwritten == false && next.history_sequence == 1);
    assert(reference.copyHistoryAfter(14, &next, &overwritten));
    assert(!overwritten && next.mono[0] == 999);
    // 已关闭代次中未泵出的事件不能进入下一代次。
    emit(reference, 777);
    reference.beginGeneration();
    const auto next_generation = reference.metrics().dma_generation;
    assert(next_generation != generation);
    assert(reference.metrics().playback_serial == 0);
    assert(reference.bindPlaybackSerial(78));
    reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), next_generation) == 0);
    emit(reference, 888); reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), next_generation) == 1);
    assert(history[0].mono[0] == 888 && history[0].first_sample_24k == 0);
    reference.markAmplifierTransition();
    emit(reference, 889); reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), next_generation) == 2);
    assert(history[1].amplifier_boundary && !history[1].gap_before);
    assert(history[1].interval_kind == PlaybackReference::IntervalKind::Unknown);
    assert(reference.metrics().amplifier_boundary_blocks == 1);
    emit(reference, 890); reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), next_generation) == 3);
    assert(!history[2].amplifier_boundary && !history[2].gap_before);
    // 历史保持有界，快照顺序为最旧 -> 最新。
    for (int i = 0; i < 35; ++i) { emit(reference, static_cast<int16_t>(1000 + i)); reference.pump(); }
    assert(reference.metrics().history_blocks == 30 && reference.metrics().history_overwrites > 0);
    assert(reference.copyHistory(history.data(), history.size(), next_generation) == 30);
    assert(history[0].mono[0] == 1005 && history[29].mono[0] == 1034);
    reference.stop(); reference.stop();
    assert(allocations.empty());
    // 只有明确成功排空才允许重新绑定序号。任一次写入都会消耗该许可；
    // 部分写入/错误写入不能重新授予许可。
    assert(reference.start() == ESP_OK);
    reference.beginGeneration();
    const auto continuous_generation = reference.metrics().dma_generation;
    assert(reference.bindPlaybackSerial(100));
    assert(reference.bindPlaybackSerial(99));
    assert(reference.bindPlaybackSerial(100));
    reference.recordSubmittedBytes(960, 960);
    assert(!reference.bindPlaybackSerial(101));
    reference.markDrained();
    assert(reference.bindPlaybackSerial(101));
    assert(reference.metrics().dma_generation == continuous_generation);
    assert(reference.bindPlaybackSerial(102));  // 重新绑定的作用域中没有内容。
    reference.markDrained();
    reference.recordSubmittedBytes(960, 960);
    assert(!reference.bindPlaybackSerial(103));
    reference.recordSubmittedBytes(960, 960, false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(103));
    reference.beginGeneration();
    assert(reference.bindPlaybackSerial(103));
    reference.markAmplifierTransition();
    emit(reference, 0, false); reference.pump();
    assert(reference.copyHistory(history.data(), history.size()) == 1);
    assert(history[0].amplifier_boundary && !history[0].gap_before);
    assert(history[0].interval_kind == PlaybackReference::IntervalKind::DigitalSilence);
    assert(history[0].known_silence && history[0].stereo_match);
    reference.setAmplifierTransitionActive(true);
    emit(reference, 100, false); reference.pump();
    reference.setAmplifierTransitionActive(false);
    emit(reference, 0); reference.pump();
    assert(reference.copyHistory(history.data(), history.size()) == 3);
    assert(history[1].interval_kind == PlaybackReference::IntervalKind::Unknown);
    assert(!history[1].known_silence && !history[1].gap_before);
    assert(history[2].interval_kind == PlaybackReference::IntervalKind::DigitalSilence);
    // 初始未绑定的电气静音可能超过有界 ISR 队列时长。
    // 丢失区间仍明确保留，只放宽首次绑定的否决条件。
    reference.beginGeneration();
    const auto idle_generation = reference.metrics().dma_generation;
    for (int i = 0; i < 12; ++i) emit(reference, 1234, false);
    metrics = reference.metrics();
    assert(metrics.playback_serial == 0 && metrics.submitted_frames == 0);
    assert(metrics.queue_dropped_blocks == 4 && metrics.discontinuity);
    assert(!metrics.binding_fault);
    reference.pump();
    emit(reference, 1234, false); reference.pump();
    assert(reference.copyHistory(history.data(), history.size(), idle_generation) == 9);
    assert(history[7].first_sample_24k == 7 * 240);
    assert(history[8].first_sample_24k == 12 * 240 && history[8].gap_before);
    assert(history[8].interval_kind == PlaybackReference::IntervalKind::AmplifierMuted);
    assert(reference.bindPlaybackSerial(104));
    assert(reference.metrics().queue_dropped_blocks == 4 && reference.metrics().discontinuity);
    assert(!reference.metrics().binding_fault);
    reference.recordSubmittedBytes(960, 960);
    assert(!reference.bindPlaybackSerial(105));
    reference.markDrained();
    assert(reference.bindPlaybackSerial(105));
    assert(reference.metrics().dma_generation == idle_generation);

    // 其他任何丢失即使在 markDrained() 后也仍禁止绑定。
    // 功放已启用不属于初始稳定静音例外。
    reference.beginGeneration();
    for (int i = 0; i < 9; ++i) emit(reference, 0, true);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    // 有过提交或序号绑定的静音代次也不属于例外。
    reference.beginGeneration();
    reference.recordSubmittedBytes(960, 960);
    for (int i = 0; i < 9; ++i) emit(reference, 0, false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    reference.beginGeneration();
    assert(reference.bindPlaybackSerial(105));
    for (int i = 0; i < 9; ++i) emit(reference, 0, false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    // GPIO 转换不能证明丢失块全程静音。
    reference.beginGeneration();
    reference.setAmplifierTransitionActive(true);
    for (int i = 0; i < 9; ++i) emit(reference, 0, false);
    reference.setAmplifierTransitionActive(false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    // 无效 DMA、立体声不匹配及写入失败永远不豁免。
    reference.beginGeneration();
    reference.onDmaSent(stereo.data(), 16, false);
    for (int i = 0; i < 9; ++i) emit(reference, 0, false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    reference.beginGeneration();
    stereo.fill(0); stereo[3] = 1;
    reference.onDmaSent(stereo.data(), sizeof(stereo), false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    reference.beginGeneration();
    reference.recordSubmittedBytes(3, 3);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    reference.beginGeneration();
    reference.recordSubmittedBytes(0, 960, false);
    reference.markDrained();
    assert(!reference.bindPlaybackSerial(106));
    assert(reference.metrics().binding_fault);
    reference.stop();
    assert(allocations.empty());
    // 缺少 PSRAM 时保留观测计数，并记录历史不可用。
    fail_caps = MALLOC_CAP_SPIRAM;
    assert(reference.start() == ESP_OK && reference.enabled());
    assert(!reference.metrics().binding_fault);
    assert(!reference.metrics().history_available);
    reference.beginGeneration(); emit(reference); reference.pump();
    assert(reference.metrics().history_unavailable_blocks == 1);
    assert(reference.copyHistory(history.data(), history.size()) == 0);
    reference.stop();
    assert(allocations.empty());
    std::puts("Passed playback reference: bounded memory, copied DMA, silence/mute, raw-zero amplifier boundaries, mismatch/invalid, real overflow/gap, initial-muted loss recovery, unsafe-loss binding fences, drained-only serial rebind, write failure fencing, generation, history, allocation fallback; no hardware used");
}
