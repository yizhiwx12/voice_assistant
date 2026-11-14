#include "playback_reference.hpp"

#include <algorithm>
#include <new>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

namespace {
constexpr size_t kStereoFrameBytes = 2 * sizeof(int16_t);
constexpr size_t kDmaBytes = PlaybackReference::kSamplesPerBlock * kStereoFrameBytes;
static_assert(sizeof(PlaybackReference::Block) * 30 <= 16 * 1024,
              "The playback reference history must stay within its PSRAM budget");
}

esp_err_t PlaybackReference::start()
{
    if (queue_) return ESP_ERR_INVALID_STATE;
    // Xtensa 原子辅助函数对内部内存使用硬件 CAS 路径。
    // PSRAM 兼容处理可能使用锁，因此不能把外部 RAM 中的对象
    // 注册为 I2S 中断上下文。
    if (!esp_ptr_internal(this)) return ESP_ERR_INVALID_STATE;

    queue_ = static_cast<Block *>(heap_caps_malloc(
        sizeof(Block) * kQueueBlocks, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!queue_) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < kQueueBlocks; ++i) new (queue_ + i) Block{};

    history_ = static_cast<Block *>(heap_caps_malloc(
        sizeof(Block) * kHistoryBlocks, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (history_) {
        for (size_t i = 0; i < kHistoryBlocks; ++i) new (history_ + i) Block{};
    }

    queue_write_.store(0, std::memory_order_relaxed);
    queue_read_.store(0, std::memory_order_relaxed);
    generation_.store(0, std::memory_order_relaxed);
    playback_serial_.store(0, std::memory_order_relaxed);
    completed_sample_24k_low_.store(0, std::memory_order_relaxed);
    last_observed_end_us_low_.store(0, std::memory_order_relaxed);
    sample_cursor_ = 0;
    amplifier_boundary_.store(0, std::memory_order_relaxed);
    amplifier_transition_active_.store(0, std::memory_order_relaxed);
    pending_gap_.store(false, std::memory_order_relaxed);
    discontinuity_.store(0, std::memory_order_relaxed);
    binding_fault_.store(0, std::memory_order_relaxed);
    counters_.submitted_frames.store(0, std::memory_order_relaxed);
    counters_.partial_write_bytes.store(0, std::memory_order_relaxed);
    counters_.completed_blocks.store(0, std::memory_order_relaxed);
    counters_.completed_frames.store(0, std::memory_order_relaxed);
    counters_.amplifier_muted_blocks.store(0, std::memory_order_relaxed);
    counters_.amplifier_boundary_blocks.store(0, std::memory_order_relaxed);
    counters_.known_silence_blocks.store(0, std::memory_order_relaxed);
    counters_.non_silence_blocks.store(0, std::memory_order_relaxed);
    counters_.stereo_mismatch_samples.store(0, std::memory_order_relaxed);
    counters_.invalid_blocks.store(0, std::memory_order_relaxed);
    counters_.queue_dropped_blocks.store(0, std::memory_order_relaxed);
    history_write_ = history_count_ = history_overwrites_ = history_unavailable_ = 0;
    history_sequence_ = 0;
    rebind_allowed_ = true;
    write_failed_ = false;
    return ESP_OK;
}

void PlaybackReference::stop()
{
    if (history_) heap_caps_free(history_);
    if (queue_) heap_caps_free(queue_);
    history_ = nullptr;
    queue_ = nullptr;
    queue_write_.store(0, std::memory_order_relaxed);
    queue_read_.store(0, std::memory_order_relaxed);
}

void PlaybackReference::beginGeneration()
{
    // 仅在旧 TX 通道及其 ISR 已停止后调用。
    queue_write_.store(0, std::memory_order_relaxed);
    queue_read_.store(0, std::memory_order_relaxed);
    const uint32_t next = generation_.load(std::memory_order_relaxed) + 1;
    generation_.store(next, std::memory_order_release);
    playback_serial_.store(0, std::memory_order_release);
    completed_sample_24k_low_.store(0, std::memory_order_relaxed);
    last_observed_end_us_low_.store(0, std::memory_order_relaxed);
    sample_cursor_ = 0;
    amplifier_boundary_.store(0, std::memory_order_relaxed);
    amplifier_transition_active_.store(0, std::memory_order_relaxed);
    pending_gap_.store(false, std::memory_order_relaxed);
    rebind_allowed_ = true;
    write_failed_ = false;
    discontinuity_.store(0, std::memory_order_relaxed);
    binding_fault_.store(0, std::memory_order_relaxed);
    counters_.submitted_frames.store(0, std::memory_order_relaxed);
    counters_.partial_write_bytes.store(0, std::memory_order_relaxed);
    counters_.completed_blocks.store(0, std::memory_order_relaxed);
    counters_.completed_frames.store(0, std::memory_order_relaxed);
    counters_.amplifier_muted_blocks.store(0, std::memory_order_relaxed);
    counters_.amplifier_boundary_blocks.store(0, std::memory_order_relaxed);
    counters_.known_silence_blocks.store(0, std::memory_order_relaxed);
    counters_.non_silence_blocks.store(0, std::memory_order_relaxed);
    counters_.stereo_mismatch_samples.store(0, std::memory_order_relaxed);
    counters_.invalid_blocks.store(0, std::memory_order_relaxed);
    counters_.queue_dropped_blocks.store(0, std::memory_order_relaxed);
}

void PlaybackReference::recordSubmittedBytes(size_t bytes, size_t requested_bytes, bool successful)
{
    if (!queue_) return;
    rebind_allowed_ = false;
    const size_t frames = bytes / kStereoFrameBytes;
    counters_.submitted_frames.fetch_add(static_cast<uint32_t>(frames), std::memory_order_relaxed);
    if (!successful || bytes != requested_bytes || bytes % kStereoFrameBytes) {
        write_failed_ = true;
        counters_.partial_write_bytes.fetch_add(static_cast<uint32_t>(bytes), std::memory_order_relaxed);
        discontinuity_.store(1, std::memory_order_relaxed);
        binding_fault_.store(1, std::memory_order_relaxed);
        pending_gap_.store(1, std::memory_order_release);
    }
}

void PlaybackReference::markDrained()
{
    // 驱动仅在成功写入超过整个 DMA 环容量的零 PCM 后调用。
    // 此前的部分写入/错误仍明确保留。
    rebind_allowed_ = !write_failed_ && !binding_fault_.load(std::memory_order_relaxed);
}

bool PlaybackReference::bindPlaybackSerial(uint32_t serial)
{
    if (!queue_) return true;  // 观测被禁用，播放仍可工作。
    if (!serial) return false;
    if (write_failed_ || binding_fault_.load(std::memory_order_relaxed)) return false;
    const uint32_t previous = playback_serial_.load(std::memory_order_acquire);
    if (previous == serial) return true;
    if ((previous || counters_.submitted_frames.load(std::memory_order_relaxed)) && !rebind_allowed_)
        return false;
    // 只有初始为空或已明确排空的零 DMA 才能改变作用域。
    // 序号只是元数据，不改变物理样本坐标。
    playback_serial_.store(serial, std::memory_order_release);
    return true;
}

bool IRAM_ATTR PlaybackReference::onDmaSent(const void *stereo_pcm, size_t bytes,
                                            bool amplifier_enabled)
{
    if (!queue_ || !stereo_pcm) return false;

    const uint64_t first_sample = sample_cursor_;
    sample_cursor_ += kSamplesPerBlock;
    counters_.completed_blocks.fetch_add(1, std::memory_order_relaxed);
    counters_.completed_frames.fetch_add(kSamplesPerBlock, std::memory_order_relaxed);
    completed_sample_24k_low_.store(static_cast<uint32_t>(sample_cursor_), std::memory_order_relaxed);
    const int64_t observed_end_us = esp_timer_get_time();
    last_observed_end_us_low_.store(static_cast<uint32_t>(observed_end_us), std::memory_order_relaxed);
    if (bytes != kDmaBytes) {
        counters_.invalid_blocks.fetch_add(1, std::memory_order_relaxed);
        discontinuity_.store(1, std::memory_order_relaxed);
        binding_fault_.store(1, std::memory_order_relaxed);
        pending_gap_.store(true, std::memory_order_release);
        return false;
    }

    const uint32_t write = queue_write_.load(std::memory_order_relaxed);
    const uint32_t read = queue_read_.load(std::memory_order_acquire);
    if (write - read >= kQueueBlocks) {
        counters_.queue_dropped_blocks.fetch_add(1, std::memory_order_relaxed);
        discontinuity_.store(1, std::memory_order_relaxed);
        // 首次提交前，稳定静音且未绑定的代次不可能播放过 PCM。
        // 保留缺失区间及计数器，但不让这种空闲丢失阻止首个回答绑定。
        // 已提交、已绑定、已启用或正转换的作用域仍按失败关闭处理。
        const bool initial_muted = !amplifier_enabled &&
            !amplifier_transition_active_.load(std::memory_order_acquire) &&
            !playback_serial_.load(std::memory_order_acquire) &&
            !counters_.submitted_frames.load(std::memory_order_relaxed);
        if (!initial_muted) binding_fault_.store(1, std::memory_order_relaxed);
        pending_gap_.store(true, std::memory_order_release);
        return false;
    }

    const auto *stereo = static_cast<const int16_t *>(stereo_pcm);
    Block &block = queue_[write % kQueueBlocks];
    block.dma_generation = generation_.load(std::memory_order_relaxed);
    block.playback_serial = playback_serial_.load(std::memory_order_relaxed);
    block.first_sample_24k = first_sample;
    block.observed_end_us = observed_end_us;
    block.sample_count = kSamplesPerBlock;
    block.amplifier_enabled = amplifier_enabled;
    block.amplifier_boundary = amplifier_boundary_.exchange(0, std::memory_order_acq_rel) != 0;
    if (amplifier_transition_active_.load(std::memory_order_acquire)) block.amplifier_boundary = true;
    if (block.amplifier_boundary)
        counters_.amplifier_boundary_blocks.fetch_add(1, std::memory_order_relaxed);
    block.gap_before = pending_gap_.exchange(false, std::memory_order_acq_rel);
    bool raw_silence = true;
    bool stereo_match = true;
    uint32_t mismatches = 0;
    // 将电气静音落实为参考值前，先检查借用 DMA。
    // 只有真实零 PCM 才能证明跨任一 SD 状态的块产生零输出。
    for (size_t i = 0; i < kSamplesPerBlock; ++i) {
        const int16_t left = stereo[2 * i];
        const int16_t right = stereo[2 * i + 1];
        block.mono[i] = amplifier_enabled ? left : 0;
        if (left != right) {
            stereo_match = false;
            ++mismatches;
        }
        if (left != 0 || right != 0) raw_silence = false;
    }
    if (!amplifier_enabled)
        counters_.amplifier_muted_blocks.fetch_add(1, std::memory_order_relaxed);
    block.known_silence = !amplifier_enabled || raw_silence;
    block.stereo_match = stereo_match;
    if ((block.amplifier_boundary && !raw_silence) || !stereo_match) {
        // DMA 块可能跨越 GPIO 状态转换，因此即使采到了样本，
        // 其完整 PCM 也不是有效参考区间。
        block.interval_kind = IntervalKind::Unknown;
        block.known_silence = false;
    } else if (block.amplifier_boundary) {
        block.interval_kind = IntervalKind::DigitalSilence;
        counters_.known_silence_blocks.fetch_add(1, std::memory_order_relaxed);
    } else if (!amplifier_enabled) {
        block.interval_kind = IntervalKind::AmplifierMuted;
    } else if (raw_silence) {
        block.interval_kind = IntervalKind::DigitalSilence;
        counters_.known_silence_blocks.fetch_add(1, std::memory_order_relaxed);
    } else {
        block.interval_kind = IntervalKind::Content;
        counters_.non_silence_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    if (mismatches) {
        counters_.stereo_mismatch_samples.fetch_add(mismatches, std::memory_order_relaxed);
        discontinuity_.store(1, std::memory_order_relaxed);
        binding_fault_.store(1, std::memory_order_relaxed);
    }
    queue_write_.store(write + 1, std::memory_order_release);
    return false;
}

bool PlaybackReference::pop(Block *block)
{
    if (!queue_ || !block) return false;
    const uint32_t read = queue_read_.load(std::memory_order_relaxed);
    const uint32_t write = queue_write_.load(std::memory_order_acquire);
    if (read == write) return false;
    *block = queue_[read % kQueueBlocks];
    queue_read_.store(read + 1, std::memory_order_release);
    return true;
}

void PlaybackReference::pump()
{
    Block block{};
    for (size_t i = 0; i < kQueueBlocks && pop(&block); ++i) {
        if (!history_) {
            ++history_unavailable_;
            continue;
        }
        block.history_sequence = ++history_sequence_;
        history_[history_write_] = block;
        history_write_ = (history_write_ + 1) % kHistoryBlocks;
        if (history_count_ < kHistoryBlocks) ++history_count_;
        else ++history_overwrites_;
    }
}

PlaybackReference::Metrics PlaybackReference::metrics() const
{
    Metrics result{};
    result.enabled = queue_ != nullptr;
    result.history_available = history_ != nullptr;
    result.dma_generation = generation_.load(std::memory_order_acquire);
    result.playback_serial = playback_serial_.load(std::memory_order_acquire);
    result.submitted_frames = counters_.submitted_frames.load(std::memory_order_relaxed);
    result.partial_write_bytes = counters_.partial_write_bytes.load(std::memory_order_relaxed);
    result.completed_blocks = counters_.completed_blocks.load(std::memory_order_relaxed);
    result.completed_frames = counters_.completed_frames.load(std::memory_order_relaxed);
    result.completed_sample_24k_low = completed_sample_24k_low_.load(std::memory_order_relaxed);
    result.last_observed_end_us_low = last_observed_end_us_low_.load(std::memory_order_relaxed);
    result.amplifier_muted_blocks = counters_.amplifier_muted_blocks.load(std::memory_order_relaxed);
    result.amplifier_boundary_blocks = counters_.amplifier_boundary_blocks.load(std::memory_order_relaxed);
    result.known_silence_blocks = counters_.known_silence_blocks.load(std::memory_order_relaxed);
    result.non_silence_blocks = counters_.non_silence_blocks.load(std::memory_order_relaxed);
    result.stereo_mismatch_samples = counters_.stereo_mismatch_samples.load(std::memory_order_relaxed);
    result.invalid_blocks = counters_.invalid_blocks.load(std::memory_order_relaxed);
    result.queue_dropped_blocks = counters_.queue_dropped_blocks.load(std::memory_order_relaxed);
    result.history_blocks = history_count_;
    result.latest_history_sequence = history_sequence_;
    result.history_overwrites = history_overwrites_;
    result.history_unavailable_blocks = history_unavailable_;
    result.queue_blocks = queue_write_.load(std::memory_order_acquire) -
                          queue_read_.load(std::memory_order_acquire);
    result.discontinuity = discontinuity_.load(std::memory_order_relaxed) != 0;
    result.binding_fault = binding_fault_.load(std::memory_order_relaxed) != 0;
    return result;
}

void PlaybackReference::markAmplifierTransition()
{
    amplifier_boundary_.store(1, std::memory_order_release);
}

void PlaybackReference::setAmplifierTransitionActive(bool active)
{
    // 标记所有与 GPIO/生命周期转换并发的回调，
    // 也标记随后的首个完成块，因为它可能跨越边沿。
    if (active) amplifier_transition_active_.store(1, std::memory_order_release);
    markAmplifierTransition();
    if (!active) amplifier_transition_active_.store(0, std::memory_order_release);
}

size_t PlaybackReference::copyHistory(Block *destination, size_t capacity, uint32_t generation) const
{
    if (!history_ || !destination || !capacity) return 0;
    const uint32_t first = (history_write_ + kHistoryBlocks - history_count_) % kHistoryBlocks;
    size_t copied = 0;
    for (uint32_t i = 0; i < history_count_ && copied < capacity; ++i) {
        const Block &block = history_[(first + i) % kHistoryBlocks];
        if (generation && block.dma_generation != generation) continue;
        destination[copied++] = block;
    }
    return copied;
}

bool PlaybackReference::copyHistoryAfter(uint32_t cursor, Block *destination, bool *overwritten) const
{
    if (overwritten) *overwritten = false;
    if (!history_ || !destination || !history_count_) return false;
    const uint32_t first = (history_write_ + kHistoryBlocks - history_count_) % kHistoryBlocks;
    const uint32_t oldest_sequence = history_[first].history_sequence;
    if (overwritten && static_cast<int32_t>(oldest_sequence - cursor) > 1)
        *overwritten = true;
    for (uint32_t i = 0; i < history_count_; ++i) {
        const Block &block = history_[(first + i) % kHistoryBlocks];
        if (static_cast<int32_t>(block.history_sequence - cursor) > 0) {
            *destination = block;
            return true;
        }
    }
    return false;
}
